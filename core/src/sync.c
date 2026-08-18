#include <daemoon/sync.h>

#include <daemoon/i18n.h>
#include <daemoon/util/sha256.h>
#include <daemoon/util/strbuf.h>

#include "util/json.h"

#include <string.h>

#define SYNC_STATE_MAX_BYTES 512
#define SYNC_STATE_TOKENS    32
#define SYNC_PATH_MAX        (DAEMOON_PATH_MAX * 2)

/* created_at is informational and the console RTC is not trusted, so when the
 * platform offers no clock the field gets a fixed placeholder rather than a guess. */
static const char k_epoch[] = DAEMOON_TIMESTAMP_NONE;

const char *daemoon_sync_action_name(daemoon_sync_action_t a)
{
    switch (a) {
    case DAEMOON_SYNC_UPLOAD:   return "upload";
    case DAEMOON_SYNC_DOWNLOAD: return "download";
    case DAEMOON_SYNC_CONFLICT: return "conflict";
    default:                    return "none";
    }
}

daemoon_str_id_t daemoon_sync_action_str(daemoon_sync_action_t a)
{
    switch (a) {
    case DAEMOON_SYNC_UPLOAD:   return DAEMOON_STR_SYNC_ACTION_UPLOAD;
    case DAEMOON_SYNC_DOWNLOAD: return DAEMOON_STR_SYNC_ACTION_DOWNLOAD;
    case DAEMOON_SYNC_CONFLICT: return DAEMOON_STR_SYNC_ACTION_CONFLICT;
    default:                    return DAEMOON_STR_SYNC_ACTION_NONE;
    }
}

int daemoon_sync_local_dirty(const daemoon_local_state_t *local)
{
    if (local == NULL || !local->has_save) {
        return 0;
    }
    if (local->base_version == DAEMOON_VERSION_NONE || local->base_sha256[0] == '\0') {
        /* Never synced, so everything about it is new. */
        return 1;
    }
    return !daemoon_sha256_hex_equal(local->sha256, local->base_sha256);
}

daemoon_sync_action_t daemoon_sync_decide(const daemoon_local_state_t *local,
                                          const daemoon_remote_meta_t *remote)
{
    int dirty;

    if (local == NULL || remote == NULL) {
        return DAEMOON_SYNC_NONE;
    }

    if (!local->has_save) {
        return remote->exists ? DAEMOON_SYNC_DOWNLOAD : DAEMOON_SYNC_NONE;
    }
    if (!remote->exists) {
        return DAEMOON_SYNC_UPLOAD;
    }

    /* Same bytes on both sides. Whatever the version bookkeeping says, there is
     * nothing to move. */
    if (daemoon_sha256_hex_equal(local->sha256, remote->sha256)) {
        return DAEMOON_SYNC_NONE;
    }

    dirty = daemoon_sync_local_dirty(local);

    if (local->base_version == remote->latest_version) {
        /* This console is working on top of what the server has. */
        return dirty ? DAEMOON_SYNC_UPLOAD : DAEMOON_SYNC_CONFLICT;
    }
    if (local->base_version < remote->latest_version) {
        /* The server moved on. If this console did not touch the save since, taking
         * the server copy loses nothing. */
        return dirty ? DAEMOON_SYNC_CONFLICT : DAEMOON_SYNC_DOWNLOAD;
    }

    /* base_version > latest_version: the server was restored from a backup, or this
     * token now points at a different account. Never guess, ask. */
    return DAEMOON_SYNC_CONFLICT;
}

/* ------------------------------------------------------------------- paths */

static daemoon_result_t join_path(char *buf, size_t cap, const char *a, const char *b,
                                  const char *c)
{
    daemoon_strbuf_t sb;

    daemoon_strbuf_init(&sb, buf, cap);
    daemoon_strbuf_add(&sb, a);
    if (b != NULL) {
        daemoon_strbuf_addc(&sb, '/');
        daemoon_strbuf_add(&sb, b);
    }
    if (c != NULL) {
        daemoon_strbuf_addc(&sb, '/');
        daemoon_strbuf_add(&sb, c);
    }
    return daemoon_strbuf_result(&sb);
}

/* <platform>_<title id>, used as the leaf name everywhere. A title id is only
 * unique together with its platform. */
static daemoon_result_t title_key(char *buf, size_t cap, daemoon_platform_t platform,
                                  const char *title_id)
{
    daemoon_strbuf_t sb;

    daemoon_strbuf_init(&sb, buf, cap);
    daemoon_strbuf_add(&sb, daemoon_platform_name(platform));
    daemoon_strbuf_addc(&sb, '_');
    daemoon_strbuf_add(&sb, title_id);
    return daemoon_strbuf_result(&sb);
}

static daemoon_result_t state_path(const daemoon_env_t *env, daemoon_platform_t platform,
                                   const char *title_id, char *buf, size_t cap)
{
    char key[DAEMOON_TITLE_ID_MAX + 16];
    char leaf[DAEMOON_TITLE_ID_MAX + 24];
    daemoon_strbuf_t sb;

    DAEMOON_TRY(title_key(key, sizeof(key), platform, title_id));
    daemoon_strbuf_init(&sb, leaf, sizeof(leaf));
    daemoon_strbuf_add(&sb, key);
    daemoon_strbuf_add(&sb, ".json");
    DAEMOON_TRY(daemoon_strbuf_result(&sb));

    return join_path(buf, cap, env->work_dir, "state", leaf);
}

/* --------------------------------------------------------------- state file */

daemoon_result_t daemoon_sync_state_load(const daemoon_env_t *env, daemoon_platform_t platform,
                                         const char *title_id, daemoon_local_state_t *out)
{
    char path[SYNC_PATH_MAX];
    char json[SYNC_STATE_MAX_BYTES];
    jsmntok_t toks[SYNC_STATE_TOKENS];
    daemoon_stream_t *f = NULL;
    unsigned long long n = 0;
    size_t len = 0;
    int ntok = 0;
    daemoon_result_t r;

    if (env == NULL || title_id == NULL || out == NULL) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }

    out->base_version = DAEMOON_VERSION_NONE;
    out->base_sha256[0] = '\0';

    DAEMOON_TRY(state_path(env, platform, title_id, path, sizeof(path)));
    if (!env->fs->exists(env->fs_ctx, path)) {
        /* No record yet just means this title has never been synced from here. */
        return DAEMOON_OK;
    }

    DAEMOON_TRY(env->fs->open(env->fs_ctx, path, DAEMOON_OPEN_READ, &f));
    for (;;) {
        size_t got = 0;
        r = daemoon_stream_read(f, json + len, sizeof(json) - 1 - len, &got);
        if (r != DAEMOON_OK) {
            (void)daemoon_stream_close(f);
            return r;
        }
        if (got == 0) {
            break;
        }
        len += got;
        if (len >= sizeof(json) - 1) {
            (void)daemoon_stream_close(f);
            return DAEMOON_ERR_PARSE_ERROR;
        }
    }
    DAEMOON_TRY(daemoon_stream_close(f));
    json[len] = '\0';

    DAEMOON_TRY(daemoon_json_parse(json, len, toks, SYNC_STATE_TOKENS, &ntok));
    if (daemoon_json_get_uint(json, toks, ntok, 0, "base_version", &n) == DAEMOON_OK &&
        n <= 0xffffffffull) {
        out->base_version = (unsigned int)n;
    }
    (void)daemoon_json_get_str(json, toks, ntok, 0, "base_sha256", out->base_sha256,
                               sizeof(out->base_sha256));
    return DAEMOON_OK;
}

daemoon_result_t daemoon_sync_state_save(const daemoon_env_t *env, daemoon_platform_t platform,
                                         const char *title_id, const daemoon_local_state_t *st)
{
    char path[SYNC_PATH_MAX];
    char tmp[SYNC_PATH_MAX];
    char dir[SYNC_PATH_MAX];
    char json[SYNC_STATE_MAX_BYTES];
    daemoon_strbuf_t sb;
    daemoon_stream_t *f = NULL;
    daemoon_result_t r;

    if (env == NULL || title_id == NULL || st == NULL) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }

    DAEMOON_TRY(join_path(dir, sizeof(dir), env->work_dir, "state", NULL));
    DAEMOON_TRY(env->fs->mkdir_p(env->fs_ctx, dir));
    DAEMOON_TRY(state_path(env, platform, title_id, path, sizeof(path)));

    daemoon_strbuf_init(&sb, tmp, sizeof(tmp));
    daemoon_strbuf_add(&sb, path);
    daemoon_strbuf_add(&sb, ".tmp");
    DAEMOON_TRY(daemoon_strbuf_result(&sb));

    daemoon_strbuf_init(&sb, json, sizeof(json));
    daemoon_strbuf_add(&sb, "{\"format_version\":1,\"platform\":\"");
    daemoon_strbuf_add_json(&sb, daemoon_platform_name(platform));
    daemoon_strbuf_add(&sb, "\",\"title_id\":\"");
    daemoon_strbuf_add_json(&sb, title_id);
    daemoon_strbuf_add(&sb, "\",\"base_version\":");
    daemoon_strbuf_add_uint(&sb, st->base_version);
    daemoon_strbuf_add(&sb, ",\"base_sha256\":\"");
    daemoon_strbuf_add_json(&sb, st->base_sha256);
    daemoon_strbuf_add(&sb, "\"}");
    DAEMOON_TRY(daemoon_strbuf_result(&sb));

    /* Temp path then rename: an interrupted write must not leave a state file that
     * parses into the wrong base version, because that turns into a silent
     * conflict decision on the next run. */
    DAEMOON_TRY(env->fs->open(env->fs_ctx, tmp, DAEMOON_OPEN_WRITE, &f));
    r = daemoon_stream_write(f, json, sb.len);
    if (r != DAEMOON_OK) {
        (void)daemoon_stream_close(f);
        (void)env->fs->remove(env->fs_ctx, tmp);
        return r;
    }
    r = daemoon_stream_close(f);
    if (r != DAEMOON_OK) {
        (void)env->fs->remove(env->fs_ctx, tmp);
        return r;
    }

    r = env->fs->rename(env->fs_ctx, tmp, path);
    if (r != DAEMOON_OK) {
        (void)env->fs->remove(env->fs_ctx, tmp);
        return r;
    }
    return DAEMOON_OK;
}

/* -------------------------------------------------------------- local scan */

daemoon_result_t daemoon_sync_scan_local(const daemoon_env_t *env, daemoon_archive_ctx_t *actx,
                                         const daemoon_title_t *title, daemoon_local_state_t *out)
{
    daemoon_save_t *save = NULL;
    daemoon_result_t r;

    if (env == NULL || actx == NULL || title == NULL || out == NULL) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }

    memset(out, 0, sizeof(*out));
    DAEMOON_TRY(daemoon_sync_state_load(env, title->platform, title->id, out));

    /* title->has_save is a hint from whenever the list was taken. The archive is
     * what decides, so it is opened and a missing one is a normal answer. */
    r = env->save->open_save(env->save_ctx, title, &save);
    if (r == DAEMOON_ERR_NOT_FOUND) {
        out->has_save = 0;
        return DAEMOON_OK;
    }
    DAEMOON_TRY(r);

    r = daemoon_archive_hash_save(env, actx, save, out->sha256, &out->size);
    if (r != DAEMOON_OK) {
        (void)env->save->close_save(env->save_ctx, save);
        return r;
    }
    DAEMOON_TRY(env->save->close_save(env->save_ctx, save));

    out->has_save = 1;
    return DAEMOON_OK;
}

/* ------------------------------------------------------------------ backup */

static daemoon_result_t fill_created_at(const daemoon_env_t *env, char *buf, size_t cap)
{
    if (env->clock_iso8601 != NULL &&
        env->clock_iso8601(env->clock_ctx, buf, cap) == DAEMOON_OK && buf[0] != '\0') {
        return DAEMOON_OK;
    }
    return daemoon_strlcpy(buf, cap, k_epoch);
}

static daemoon_result_t pack_title_to(const daemoon_env_t *env, daemoon_archive_ctx_t *actx,
                                      const daemoon_title_t *title, unsigned int parent_version,
                                      const char *out_path, daemoon_manifest_t *out_manifest)
{
    char tmp[SYNC_PATH_MAX];
    daemoon_strbuf_t sb;
    daemoon_save_t *save = NULL;
    daemoon_stream_t *f = NULL;
    daemoon_manifest_t m;
    daemoon_result_t r;

    daemoon_manifest_init(&m);
    m.platform = title->platform;
    m.save_type = title->save_type;
    m.version = DAEMOON_VERSION_NONE;
    m.parent_version = parent_version;
    DAEMOON_TRY(daemoon_strlcpy(m.title_id, sizeof(m.title_id), title->id));
    DAEMOON_TRY(daemoon_strlcpy(m.device_label, sizeof(m.device_label), env->device_label));
    /* The console knows what this game is called and the server has no other way to
     * find out. Best effort: a name too long for the field is a name not sent, not
     * a backup refused. */
    (void)daemoon_strlcpy(m.title_name, sizeof(m.title_name), title->name);
    DAEMOON_TRY(fill_created_at(env, m.created_at, sizeof(m.created_at)));

    /* The value the save is bound to, recorded with it.
     *
     * It lives outside the archive, so a package without it is a package that cannot
     * be fully restored: the game checks the save against whatever the console holds
     * at restore time, and that may have moved on since. Best effort - a backend that
     * cannot read it is not a backup refused, it is a backup that falls back to
     * preserving the console's current value the way it always did. */
    if (env->save->read_secure_value != NULL) {
        int exists = 0;
        unsigned long long value = 0;

        if (env->save->read_secure_value(env->save_ctx, title, &exists, &value) == DAEMOON_OK &&
            exists) {
            m.has_secure_value = 1;
            m.secure_value = value;
        }
    }

    daemoon_strbuf_init(&sb, tmp, sizeof(tmp));
    daemoon_strbuf_add(&sb, out_path);
    daemoon_strbuf_add(&sb, ".part");
    DAEMOON_TRY(daemoon_strbuf_result(&sb));

    DAEMOON_TRY(env->save->open_save(env->save_ctx, title, &save));

    r = env->fs->open(env->fs_ctx, tmp, DAEMOON_OPEN_WRITE, &f);
    if (r != DAEMOON_OK) {
        (void)env->save->close_save(env->save_ctx, save);
        return r;
    }

    /* The digest is not known until the payload has been written, so the manifest
     * is completed by the packer. */
    r = daemoon_archive_pack(env, actx, save, &m, f);

    if (r == DAEMOON_OK && actx->count == 0) {
        r = DAEMOON_ERR_EMPTY_SAVE;
    }

    /* An archive with nothing in it does not become a package.
     *
     * The resulting file is a manifest and no payload, and restoring one clears
     * the archive and writes nothing back - so a package that looks like a save is
     * the thing that wipes one. Whether the archive is genuinely empty or the
     * enumeration failed, neither is a save, and both are worth stopping for.
     *
     * This lived in daemoon_sync_backup_local, which is the half of the problem
     * that stays on one device. It was missing here, so an empty read uploaded a
     * manifest with the empty digest on top of a good version, and every other
     * console then downloaded nothing over its own save. Found exactly that way,
     * against a real server. */

    if (r == DAEMOON_OK) {
        r = daemoon_stream_close(f);
    } else {
        (void)daemoon_stream_close(f);
    }
    (void)env->save->close_save(env->save_ctx, save);

    if (r != DAEMOON_OK) {
        (void)env->fs->remove(env->fs_ctx, tmp);
        return r;
    }

    /* Only now does the package appear under its real name, so a half written
     * package can never be mistaken for a backup. */
    r = env->fs->rename(env->fs_ctx, tmp, out_path);
    if (r != DAEMOON_OK) {
        (void)env->fs->remove(env->fs_ctx, tmp);
        return r;
    }

    if (out_manifest != NULL) {
        *out_manifest = m;
    }
    return DAEMOON_OK;
}

daemoon_result_t daemoon_sync_backup_local(const daemoon_env_t *env, daemoon_archive_ctx_t *actx,
                                           const daemoon_title_t *title, char *out_path,
                                           size_t path_len)
{
    char dir[SYNC_PATH_MAX];
    char path[SYNC_PATH_MAX];
    char key[DAEMOON_TITLE_ID_MAX + 16];
    char leaf[DAEMOON_TITLE_ID_MAX + 48];
    char hex[DAEMOON_SHA256_HEX];
    daemoon_strbuf_t sb;
    daemoon_save_t *save = NULL;
    daemoon_local_state_t probe;
    daemoon_result_t r;

    if (env == NULL || actx == NULL || title == NULL) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }
    DAEMOON_TRY(daemoon_env_validate(env));

    DAEMOON_TRY(join_path(dir, sizeof(dir), env->work_dir, "backups", NULL));
    DAEMOON_TRY(env->fs->mkdir_p(env->fs_ctx, dir));

    /* The backup is named after its own content, so it needs no clock and two
     * backups of an unchanged save do not pile up. */
    DAEMOON_TRY(env->save->open_save(env->save_ctx, title, &save));
    r = daemoon_archive_hash_save(env, actx, save, hex, &probe.size);
    (void)env->save->close_save(env->save_ctx, save);
    DAEMOON_TRY(r);

    /* An archive with nothing in it does not get backed up.
     *
     * The resulting package would be a manifest and no payload, and restoring one
     * clears the archive and writes nothing back - so a backup that looks like it
     * worked would be the thing that wipes the save. Whether the archive is
     * genuinely empty or the enumeration failed, neither is a backup, and both are
     * worth stopping for. Found on hardware, where a real title produced exactly
     * that file. */
    if (actx->count == 0) {
        return DAEMOON_ERR_EMPTY_SAVE;
    }

    DAEMOON_TRY(title_key(key, sizeof(key), title->platform, title->id));
    daemoon_strbuf_init(&sb, leaf, sizeof(leaf));
    daemoon_strbuf_add(&sb, key);
    daemoon_strbuf_addc(&sb, '_');
    daemoon_strbuf_addn(&sb, hex, 12);
    daemoon_strbuf_add(&sb, ".zip");
    DAEMOON_TRY(daemoon_strbuf_result(&sb));

    DAEMOON_TRY(join_path(path, sizeof(path), dir, leaf, NULL));

    if (!env->fs->exists(env->fs_ctx, path)) {
        DAEMOON_TRY(pack_title_to(env, actx, title, DAEMOON_VERSION_NONE, path, NULL));
    }

    if (out_path != NULL && path_len > 0) {
        DAEMOON_TRY(daemoon_strlcpy(out_path, path_len, path));
    }
    return DAEMOON_OK;
}

/* ----------------------------------------------------------------- restore */

static void str_ref(daemoon_str_ref_t *ref, daemoon_str_id_t id, const char *a0, const char *a1)
{
    memset(ref, 0, sizeof(*ref));
    ref->id = id;
    if (a0 != NULL) {
        ref->args[ref->nargs++] = a0;
    }
    if (a1 != NULL) {
        ref->args[ref->nargs++] = a1;
    }
}

static void notify(const daemoon_env_t *env, daemoon_str_id_t id, const char *a0)
{
    daemoon_str_ref_t ref;

    if (env->ui->notify == NULL) {
        return;
    }
    str_ref(&ref, id, a0, NULL);
    env->ui->notify(env->ui_ctx, &ref);
}

static void progress(const daemoon_env_t *env, daemoon_str_id_t id, int pct)
{
    daemoon_str_ref_t ref;

    if (env->ui->progress == NULL) {
        return;
    }
    str_ref(&ref, id, NULL, NULL);
    env->ui->progress(env->ui_ctx, &ref, pct);
}

static daemoon_result_t check_not_running(const daemoon_env_t *env, const daemoon_title_t *title)
{
    int running = 0;

    if (env->save->is_title_running == NULL) {
        return DAEMOON_OK; /* the platform cannot tell; the UI warns instead */
    }
    DAEMOON_TRY(env->save->is_title_running(env->save_ctx, title, &running));
    if (running) {
        /* A running game holds the archive: writes corrupt it and reads come back
         * stale. There is no safe way to continue. */
        notify(env, DAEMOON_STR_WARN_GAME_RUNNING, title->name);
        return DAEMOON_ERR_BACKEND_ERROR;
    }
    return DAEMOON_OK;
}

/* The restore, with the one question it asks made optional.
 *
 * `confirmed` is only ever set by a run that already asked, with a count and with
 * the sentence about overwriting in front of the person. Everything below the
 * question is unconditional and stays that way: the warning about a secure value,
 * the local backup that has to succeed, the digest check before a byte is written,
 * the commit afterwards. Rule 1 is what makes this survivable, and no caller can
 * turn it off.
 */
static daemoon_result_t restore_package(const daemoon_env_t *env, daemoon_archive_ctx_t *actx,
                                        const daemoon_title_t *title, const char *pkg_path,
                                        int confirmed)
{
    daemoon_manifest_t m;
    daemoon_str_ref_t ask;
    daemoon_stream_t *pkg = NULL;
    daemoon_save_t *save = NULL;
    daemoon_result_t r;

    if (env == NULL || actx == NULL || title == NULL || pkg_path == NULL) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }
    DAEMOON_TRY(daemoon_env_validate(env));
    DAEMOON_TRY(check_not_running(env, title));

    /* Some titles bind their save to a console stored secure value. Restoring a
     * foreign save into one of those can make the game delete it, so say so before
     * anything else happens. */
    if (title->secure_value) {
        notify(env, DAEMOON_STR_WARN_SECURE_VALUE, title->name);
    }

    if (!confirmed) {
        str_ref(&ask, DAEMOON_STR_CONFIRM_RESTORE, title->name, NULL);
        if (!env->ui->confirm(env->ui_ctx, &ask)) {
            return DAEMOON_ERR_USER_CANCELLED;
        }
    }

    /* 1. Back up first. If the backup fails, the restore does not happen.
     * A title with no save archive yet has nothing to back up, which is the only
     * acceptable reason to skip this step. */
    progress(env, DAEMOON_STR_BACKUP_CREATING, -1);
    r = daemoon_sync_backup_local(env, actx, title, NULL, 0);
    if (r != DAEMOON_OK && r != DAEMOON_ERR_NOT_FOUND) {
        notify(env, DAEMOON_STR_BACKUP_FAILED, title->name);
        return r;
    }

    /* 2. Verify the package against its own manifest before anything is written. */
    progress(env, DAEMOON_STR_PROGRESS_VERIFYING, -1);
    DAEMOON_TRY(env->fs->open(env->fs_ctx, pkg_path, DAEMOON_OPEN_READ, &pkg));
    r = daemoon_archive_read_manifest(pkg, &m);
    if (r == DAEMOON_OK) {
        r = daemoon_archive_verify(env, actx, pkg, &m);
    }
    (void)daemoon_stream_close(pkg);
    if (r != DAEMOON_OK) {
        if (r == DAEMOON_ERR_CHECKSUM_MISMATCH) {
            notify(env, DAEMOON_STR_VERIFY_FAILED, title->name);
        }
        return r;
    }

    if (m.platform != title->platform || strcmp(m.title_id, title->id) != 0) {
        /* Restoring one game's save into another is not a recoverable mistake. */
        return DAEMOON_ERR_INVALID_MANIFEST;
    }

    /* 3. Write, then commit, and treat a failed commit as a failed restore. */
    progress(env, DAEMOON_STR_PROGRESS_UNPACKING, -1);
    DAEMOON_TRY(env->fs->open(env->fs_ctx, pkg_path, DAEMOON_OPEN_READ, &pkg));
    r = env->save->open_save_write(env->save_ctx, title, &save);
    if (r != DAEMOON_OK) {
        (void)daemoon_stream_close(pkg);
        return r;
    }

    r = daemoon_archive_unpack(env, actx, pkg, save);
    (void)daemoon_stream_close(pkg);

    if (r == DAEMOON_OK) {
        r = env->save->commit(env->save_ctx, save);
        if (r != DAEMOON_OK) {
            notify(env, DAEMOON_STR_COMMIT_FAILED, title->name);
        }
    }

    {
        daemoon_result_t cr = env->save->close_save(env->save_ctx, save);
        if (r == DAEMOON_OK) {
            r = cr;
        }
    }

    /* 4. Put back the value this save was bound to.
     *
     * After the commit, because a value pointing at a save that was not written is
     * worse than one pointing at the save that was. A package without one leaves the
     * console's alone, which is what every package written before this did and what
     * the caller used to arrange by hand.
     *
     * A failure here is reported and does not fail the restore: the save is on the
     * console and committed, and telling somebody it failed would send them to
     * restore it again. */
    if (r == DAEMOON_OK) {
        daemoon_result_t sr = DAEMOON_OK;

        if (m.has_secure_value && env->save->write_secure_value != NULL) {
            sr = env->save->write_secure_value(env->save_ctx, title, m.secure_value);
        } else if (!m.has_secure_value && env->save->clear_secure_value != NULL) {
            /* The package recorded none, so the console must not keep one: whatever it
             * holds belongs to the save that was just replaced, and a value that does
             * not match the save is how a game decides the save is not the one it
             * wrote. Removing it is the only outcome that leaves the two consistent.
             *
             * Every package written before this field existed takes this path, which is
             * what makes those backups restorable at all. */
            sr = env->save->clear_secure_value(env->save_ctx, title);
        }
        if (sr != DAEMOON_OK) {
            notify(env, DAEMOON_STR_WARN_SECURE_VALUE, title->name);
        }
    }
    return r;
}

/* ------------------------------------------------------------------ upload */

static daemoon_result_t staging_path(const daemoon_env_t *env, const daemoon_title_t *title,
                                     char *buf, size_t cap)
{
    char dir[SYNC_PATH_MAX];
    char key[DAEMOON_TITLE_ID_MAX + 16];
    char leaf[DAEMOON_TITLE_ID_MAX + 24];
    daemoon_strbuf_t sb;

    DAEMOON_TRY(join_path(dir, sizeof(dir), env->work_dir, "tmp", NULL));
    DAEMOON_TRY(env->fs->mkdir_p(env->fs_ctx, dir));
    DAEMOON_TRY(title_key(key, sizeof(key), title->platform, title->id));

    daemoon_strbuf_init(&sb, leaf, sizeof(leaf));
    daemoon_strbuf_add(&sb, key);
    daemoon_strbuf_add(&sb, ".zip");
    DAEMOON_TRY(daemoon_strbuf_result(&sb));

    return join_path(buf, cap, dir, leaf, NULL);
}

static daemoon_result_t do_upload(const daemoon_env_t *env, daemoon_archive_ctx_t *actx,
                                  const daemoon_title_t *title, unsigned int parent_version,
                                  daemoon_local_state_t *local, daemoon_conflict_t *conflict)
{
    char path[SYNC_PATH_MAX];
    daemoon_manifest_t m;
    daemoon_remote_meta_t issued;
    daemoon_stream_t *f = NULL;
    daemoon_result_t r;

    DAEMOON_TRY(staging_path(env, title, path, sizeof(path)));

    progress(env, DAEMOON_STR_PROGRESS_PACKING, -1);
    DAEMOON_TRY(pack_title_to(env, actx, title, parent_version, path, &m));

    progress(env, DAEMOON_STR_PROGRESS_UPLOADING, -1);
    r = env->fs->open(env->fs_ctx, path, DAEMOON_OPEN_READ, &f);
    if (r != DAEMOON_OK) {
        (void)env->fs->remove(env->fs_ctx, path);
        return r;
    }

    memset(&issued, 0, sizeof(issued));
    /* The package on disk, not m.size: that one counts uncompressed payload bytes
     * and the body being sent is the zip. */
    r = daemoon_api_upload(env, &m, f, f->size, &issued, conflict);
    (void)daemoon_stream_close(f);
    (void)env->fs->remove(env->fs_ctx, path);
    if (r != DAEMOON_OK) {
        return r;
    }

    /* The version the server issued is now what this console is based on. */
    local->base_version = issued.latest_version;
    DAEMOON_TRY(daemoon_strlcpy(local->base_sha256, sizeof(local->base_sha256), m.sha256));
    return daemoon_sync_state_save(env, title->platform, title->id, local);
}

static daemoon_result_t do_download(const daemoon_env_t *env, daemoon_archive_ctx_t *actx,
                                    const daemoon_title_t *title,
                                    const daemoon_remote_meta_t *remote,
                                    daemoon_local_state_t *local,
                                    int restore_confirmed)
{
    char path[SYNC_PATH_MAX];
    char tmp[SYNC_PATH_MAX];
    daemoon_strbuf_t sb;
    daemoon_stream_t *f = NULL;
    daemoon_result_t r;

    DAEMOON_TRY(staging_path(env, title, path, sizeof(path)));

    daemoon_strbuf_init(&sb, tmp, sizeof(tmp));
    daemoon_strbuf_add(&sb, path);
    daemoon_strbuf_add(&sb, ".part");
    DAEMOON_TRY(daemoon_strbuf_result(&sb));

    progress(env, DAEMOON_STR_PROGRESS_DOWNLOADING, -1);
    DAEMOON_TRY(env->fs->open(env->fs_ctx, tmp, DAEMOON_OPEN_WRITE, &f));
    r = daemoon_api_download(env, title->platform, title->id, remote->latest_version, f, NULL, 0);
    if (r == DAEMOON_OK) {
        r = daemoon_stream_close(f);
    } else {
        (void)daemoon_stream_close(f);
    }
    if (r != DAEMOON_OK) {
        (void)env->fs->remove(env->fs_ctx, tmp);
        return r;
    }

    /* A half downloaded package must never be reachable under the name the restore
     * path reads from. */
    r = env->fs->rename(env->fs_ctx, tmp, path);
    if (r != DAEMOON_OK) {
        (void)env->fs->remove(env->fs_ctx, tmp);
        return r;
    }

    /* What the server said it was sending has to match what the package says it
     * is. A mismatch means the wrong blob arrived, and the payload digest inside
     * the package would happily verify against itself. */
    {
        daemoon_manifest_t got;
        daemoon_stream_t *check = NULL;

        r = env->fs->open(env->fs_ctx, path, DAEMOON_OPEN_READ, &check);
        if (r == DAEMOON_OK) {
            r = daemoon_archive_read_manifest(check, &got);
            (void)daemoon_stream_close(check);
        }
        if (r == DAEMOON_OK && !daemoon_sha256_hex_equal(got.sha256, remote->sha256)) {
            r = DAEMOON_ERR_CHECKSUM_MISMATCH;
        }
        if (r != DAEMOON_OK) {
            if (r == DAEMOON_ERR_CHECKSUM_MISMATCH) {
                notify(env, DAEMOON_STR_VERIFY_FAILED, title->name);
            }
            (void)env->fs->remove(env->fs_ctx, path);
            return r;
        }
    }

    r = restore_package(env, actx, title, path, restore_confirmed);
    (void)env->fs->remove(env->fs_ctx, path);
    if (r != DAEMOON_OK) {
        return r;
    }

    local->base_version = remote->latest_version;
    DAEMOON_TRY(daemoon_strlcpy(local->base_sha256, sizeof(local->base_sha256), remote->sha256));
    return daemoon_sync_state_save(env, title->platform, title->id, local);
}

daemoon_result_t daemoon_sync_restore_package(const daemoon_env_t *env, daemoon_archive_ctx_t *actx,
                                              const daemoon_title_t *title, const char *pkg_path)
{
    /* The published entry point always asks. A caller holding a package and a title
     * has not been through a screen that said how many saves are about to be
     * overwritten, so it does not get to skip the question. */
    return restore_package(env, actx, title, pkg_path, 0);
}

/* ---------------------------------------------------------------- conflict */

/* The dialog. Both sides described well enough to tell apart: a choice between two
 * saves that says nothing about either is not a choice. */
static int ask_conflict(const daemoon_env_t *env, const daemoon_title_t *title,
                        const daemoon_remote_meta_t *remote,
                        const daemoon_local_state_t *local)
{
    daemoon_str_ref_t msg;
    daemoon_str_ref_t opts[3];
    char local_size[24];
    char remote_size[24];

    daemoon_fmt_bytes(local_size, sizeof(local_size), local->size);
    daemoon_fmt_bytes(remote_size, sizeof(remote_size), remote->size);

    str_ref(&msg, DAEMOON_STR_CONFLICT_EXPLAIN, title->name, NULL);
    /* The local side has no trustworthy timestamp of its own, so it is described by
     * this console's label. The server side carries the label and receive time the
     * server recorded. */
    str_ref(&opts[0], DAEMOON_STR_CONFLICT_KEEP_LOCAL, local_size, env->device_label);
    str_ref(&opts[1], DAEMOON_STR_CONFLICT_KEEP_SERVER, remote_size,
            remote->device_label[0] != '\0' ? remote->device_label : remote->received_at);
    str_ref(&opts[2], DAEMOON_STR_CONFLICT_KEEP_BOTH, NULL, NULL);

    return env->ui->choose(env->ui_ctx, &msg, opts, 3);
}

static daemoon_result_t resolve_conflict(const daemoon_env_t *env, daemoon_archive_ctx_t *actx,
                                         const daemoon_title_t *title,
                                         const daemoon_remote_meta_t *remote,
                                         daemoon_local_state_t *local,
                                         daemoon_conflict_policy_t policy,
                                         int restore_confirmed,
                                         daemoon_sync_stats_t *stats)
{
    int choice;

    /* A policy answers the question that ask_conflict would have asked, and nothing
     * else. Everything below is the same code either way: the same upload, which
     * leaves every server version in place, and the same download, which goes
     * through a restore that backs the console's save up first. */
    switch (policy) {
    case DAEMOON_CONFLICT_POLICY_KEEP_LOCAL:
        choice = DAEMOON_CONFLICT_KEEP_LOCAL;
        break;
    case DAEMOON_CONFLICT_POLICY_KEEP_SERVER:
        choice = DAEMOON_CONFLICT_KEEP_SERVER;
        break;
    case DAEMOON_CONFLICT_POLICY_DEFER:
        choice = DAEMOON_CONFLICT_DEFER;
        break;
    case DAEMOON_CONFLICT_POLICY_ASK:
    default:
        choice = ask_conflict(env, title, remote, local);
        break;
    }
    if (choice < 0 || choice == DAEMOON_CONFLICT_DEFER) {
        /* Both versions stay where they are. Nothing is merged and nothing is lost. */
        stats->conflicts++;
        return DAEMOON_OK;
    }

    if (choice == DAEMOON_CONFLICT_KEEP_LOCAL) {
        /* Upload on top of what the server has. The server keeps the previous
         * version, so choosing this does not discard the other side. */
        daemoon_conflict_t again;
        daemoon_result_t r;

        memset(&again, 0, sizeof(again));
        r = do_upload(env, actx, title, remote->latest_version, local, &again);
        if (r == DAEMOON_OK) {
            stats->uploaded++;
        }
        return r;
    }

    {
        daemoon_result_t r = do_download(env, actx, title, remote, local, restore_confirmed);
        if (r == DAEMOON_OK) {
            stats->downloaded++;
        }
        return r;
    }
}

/* -------------------------------------------------------------- sync_title */

daemoon_result_t daemoon_sync_title(const daemoon_env_t *env, daemoon_archive_ctx_t *actx,
                                    const daemoon_title_t *title, daemoon_sync_stats_t *stats)
{
    return daemoon_sync_title_with(env, actx, title, NULL, stats);
}

daemoon_result_t daemoon_sync_title_with(const daemoon_env_t *env, daemoon_archive_ctx_t *actx,
                                         const daemoon_title_t *title,
                                         const daemoon_sync_opts_t *opts,
                                         daemoon_sync_stats_t *stats)
{
    /* NULL is "ask about everything", so a caller that has not thought about any of
     * this gets the behaviour the rules describe. */
    static const daemoon_sync_opts_t k_ask_everything = {
        DAEMOON_CONFLICT_POLICY_ASK, 0, 0
    };

    daemoon_local_state_t local;
    daemoon_remote_meta_t remote;
    daemoon_sync_stats_t dummy;
    daemoon_sync_action_t action;
    daemoon_str_ref_t ask;
    daemoon_result_t r;

    if (env == NULL || actx == NULL || title == NULL) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }
    if (opts == NULL) {
        opts = &k_ask_everything;
    }
    if (stats == NULL) {
        memset(&dummy, 0, sizeof(dummy));
        stats = &dummy;
    }
    DAEMOON_TRY(daemoon_env_validate(env));
    DAEMOON_TRY(check_not_running(env, title));

    DAEMOON_TRY(daemoon_sync_scan_local(env, actx, title, &local));
    DAEMOON_TRY(daemoon_api_get_latest(env, title->platform, title->id, &remote));

    action = daemoon_sync_decide(&local, &remote);

    switch (action) {
    case DAEMOON_SYNC_NONE:
        stats->skipped++;
        /* Content already matches. Record the version so the next run does not have
         * to reach the same conclusion the long way. */
        if (local.has_save && remote.exists && local.base_version != remote.latest_version) {
            local.base_version = remote.latest_version;
            DAEMOON_TRY(daemoon_strlcpy(local.base_sha256, sizeof(local.base_sha256),
                                        local.sha256));
            DAEMOON_TRY(daemoon_sync_state_save(env, title->platform, title->id, &local));
        }
        return DAEMOON_OK;

    case DAEMOON_SYNC_UPLOAD: {
        daemoon_conflict_t conflict;

        /* Already answered, when a run over a library asked once with the count in
         * front of the person. Nothing on the console changes here and the server
         * adds a version rather than replacing one, so this is a question about
         * intent - and asking it forty more times does not make the answer better
         * informed. */
        if (!opts->upload_confirmed) {
            str_ref(&ask, DAEMOON_STR_CONFIRM_UPLOAD, title->name, NULL);
            if (!env->ui->confirm(env->ui_ctx, &ask)) {
                stats->skipped++;
                return DAEMOON_ERR_USER_CANCELLED;
            }
        }

        memset(&conflict, 0, sizeof(conflict));
        r = do_upload(env, actx, title, remote.exists ? remote.latest_version : DAEMOON_VERSION_NONE,
                      &local, &conflict);
        if (r == DAEMOON_ERR_VERSION_CONFLICT) {
            /* Someone else uploaded between the check and the upload. Re-read and
             * hand it to the user rather than overwriting. */
            DAEMOON_TRY(daemoon_api_get_latest(env, title->platform, title->id, &remote));
            return resolve_conflict(env, actx, title, &remote, &local, opts->conflict,
                                opts->restore_confirmed, stats);
        }
        if (r != DAEMOON_OK) {
            stats->failed++;
            return r;
        }
        stats->uploaded++;
        return DAEMOON_OK;
    }

    case DAEMOON_SYNC_DOWNLOAD:
        r = do_download(env, actx, title, &remote, &local, opts->restore_confirmed);
        if (r != DAEMOON_OK) {
            if (r != DAEMOON_ERR_USER_CANCELLED) {
                stats->failed++;
            }
            return r;
        }
        stats->downloaded++;
        return DAEMOON_OK;

    case DAEMOON_SYNC_CONFLICT:
    default:
        r = resolve_conflict(env, actx, title, &remote, &local, opts->conflict,
                             opts->restore_confirmed, stats);
        if (r != DAEMOON_OK && r != DAEMOON_ERR_USER_CANCELLED) {
            stats->failed++;
        }
        return r;
    }
}
