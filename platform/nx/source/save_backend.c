/* Switch save data, through libnx.
 *
 * The whole platform difference the design rests on is here, and on this console it
 * turns out to be short: `fsdevMountSaveData` makes a save into an ordinary mounted
 * filesystem, so listing and clearing it is the tree walk in platform/common and the
 * files are ordinary stdio. What is genuinely Switch is the three things around that.
 *
 *   - **The list comes from the saves, not the titles.** A save data info reader
 *     enumerates what exists rather than what is installed, which is the right
 *     question: a game with no save is nothing to sync, and an archive left behind by
 *     a game that has been deleted still is.
 *   - **A save belongs to one account.** `AccountUid` is part of what identifies it,
 *     so the reader is filtered by the selected user. A different account is a
 *     different save, and offering somebody else's would be the worst kind of
 *     helpful.
 *   - **Commit or lose it.** `fsdevCommitDevice`, checked, exactly like the 3DS's
 *     ControlArchive. Without it the write is in a cache and the console finds out
 *     when it is next turned on.
 *
 * One save is mounted at a time and the mount is the handle. The sync path opens one
 * save at once, and a second mount under the same name would silently shadow the
 * first rather than failing.
 */
#include "daemoon_nx.h"

#include <daemoon/util/strbuf.h>

#include <switch.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* The system's own limit is far higher; this is what one console plausibly holds and
 * it bounds a single allocation rather than a per title one. */
#define NX_MAX_TITLES 512

struct daemoon_save {
    unsigned long long application_id;
    int                writable;
    int                mounted;
};

/* Only ever one, for the reason in the file comment. Static rather than allocated so
 * a double open is a visible refusal instead of two handles onto one mount. */
static daemoon_save_t g_open;

/* ------------------------------------------------------------------- listing */

static daemoon_result_t from_libnx(Result rc)
{
    if (R_SUCCEEDED(rc)) {
        return DAEMOON_OK;
    }
    /* Deliberately coarse. The value itself is what a bug report needs, and it goes
     * into the trace beside every step rather than being flattened into one of these
     * - which is the lesson the 3DS SMDH hunt cost four rounds to learn. */
    return DAEMOON_ERR_BACKEND_ERROR;
}

/* A title's name, out of the ns service.
 *
 * Falls back to the hex id, which is what the 3DS build does when an SMDH cannot be
 * read. A list of hex is worse than a list of names and far better than no list. */
static void read_name(unsigned long long app_id, char *out, size_t cap)
{
    NsApplicationControlData *data;
    NacpLanguageEntry *entry = NULL;
    u64 size = 0;

    daemoon_nx_format_title_id(app_id, out, cap);

    /* Around 144 KiB. Heap rather than stack: this project has already had one data
     * abort from a large frame on a console, and the rule from it applies here even
     * though this console has room. */
    data = (NsApplicationControlData *)calloc(1, sizeof(*data));
    if (data == NULL) {
        return;
    }
    if (R_SUCCEEDED(nsGetApplicationControlData(NsApplicationControlSource_Storage,
                                                app_id, data, sizeof(*data), &size)) &&
        size >= sizeof(data->nacp) &&
        R_SUCCEEDED(nacpGetLanguageEntry(&data->nacp, &entry)) &&
        entry != NULL && entry->name[0] != '\0') {
        (void)daemoon_strlcpy(out, cap, entry->name);
    }
    free(data);
}

static daemoon_result_t list_titles(void *vctx, daemoon_title_t **out, size_t *count)
{
    daemoon_nx_save_ctx_t *ctx = (daemoon_nx_save_ctx_t *)vctx;
    FsSaveDataInfoReader reader;
    FsSaveDataInfo info;
    daemoon_title_t *titles;
    size_t n = 0;
    Result rc;

    if (out == NULL || count == NULL) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }
    *out = NULL;
    *count = 0;

    if (ctx == NULL || !ctx->account.valid) {
        /* Without an account there is no question to answer. Returning an empty list
         * would say "this console has no saves", which is a different and wrong
         * thing. */
        daemoon_nx_trace("list/no-account", NULL);
        return DAEMOON_ERR_INVALID_REQUEST;
    }

    rc = fsOpenSaveDataInfoReader(&reader, FsSaveDataSpaceId_User);
    daemoon_nx_trace("list/open-reader", R_SUCCEEDED(rc) ? "ok" : "failed");
    if (R_FAILED(rc)) {
        return from_libnx(rc);
    }

    titles = (daemoon_title_t *)calloc(NX_MAX_TITLES, sizeof(*titles));
    if (titles == NULL) {
        fsSaveDataInfoReaderClose(&reader);
        return DAEMOON_ERR_OUT_OF_MEMORY;
    }

    for (;;) {
        s64 read = 0;

        if (R_FAILED(fsSaveDataInfoReaderRead(&reader, &info, 1, &read)) || read == 0) {
            break;
        }
        if (n >= NX_MAX_TITLES) {
            /* Said out loud rather than silently truncated: a list that stops at 512
             * and does not mention it is a sync that quietly skips saves. */
            daemoon_nx_trace("list/truncated", NULL);
            break;
        }
        /* Account saves only. Device saves, cache, temporary and system storage are
         * all real save data spaces and none of them is the thing a person means by
         * "my save". */
        if (info.save_data_type != FsSaveDataType_Account) {
            continue;
        }
        if (info.uid.uid[0] != ctx->account.lower || info.uid.uid[1] != ctx->account.upper) {
            continue;
        }

        daemoon_nx_format_title_id(info.application_id, titles[n].id,
                                   sizeof(titles[n].id));
        if (ctx->names_available) {
            read_name(info.application_id, titles[n].name, sizeof(titles[n].name));
        } else {
            (void)daemoon_strlcpy(titles[n].name, sizeof(titles[n].name), titles[n].id);
        }
        titles[n].platform = DAEMOON_PLATFORM_NX;
        titles[n].save_type = DAEMOON_SAVE_SAVEDATA;
        titles[n].has_save = 1;
        /* Nothing on this platform is known to tie a save to the console the way a
         * 3DS secure value does, and claiming otherwise would put a warning in front
         * of every restore for no reason. */
        titles[n].secure_value = 0;
        titles[n].account_bound = 1;
        titles[n].size_hint = 0;
        ++n;
    }
    fsSaveDataInfoReaderClose(&reader);

    *out = titles;
    *count = n;
    {
        char detail[32];

        (void)snprintf(detail, sizeof(detail), "%u", (unsigned)n);
        daemoon_nx_trace("list/done", detail);
    }
    return DAEMOON_OK;
}

static void free_titles(void *ctx, daemoon_title_t *titles, size_t count)
{
    (void)ctx;
    (void)count;
    free(titles);
}

/* ------------------------------------------------------------------ mounting */

static daemoon_result_t mount_save(void *vctx, const daemoon_title_t *t, int writable,
                                   daemoon_save_t **out)
{
    daemoon_nx_save_ctx_t *ctx = (daemoon_nx_save_ctx_t *)vctx;
    unsigned long long app_id = 0;
    AccountUid uid;
    Result rc;

    if (ctx == NULL || t == NULL || out == NULL) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }
    if (!ctx->account.valid) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }
    if (g_open.mounted) {
        /* One at a time. Two handles onto one mount is the kind of thing that works
         * until a restore reads from the save it is writing. */
        daemoon_nx_trace("save/already-open", t->id);
        return DAEMOON_ERR_BACKEND_ERROR;
    }
    DAEMOON_TRY(daemoon_nx_parse_title_id(t->id, &app_id));

    uid.uid[0] = ctx->account.lower;
    uid.uid[1] = ctx->account.upper;

    rc = fsdevMountSaveData(DAEMOON_NX_SAVE_DEVICE, app_id, uid);
    {
        char detail[48];

        (void)snprintf(detail, sizeof(detail), "%s 0x%08X", t->id, (unsigned)rc);
        daemoon_nx_trace(writable ? "save/mount-write" : "save/mount-read", detail);
    }
    if (R_FAILED(rc)) {
        /* A save that is not there is a different answer from one that cannot be
         * reached, and the caller acts differently on it: a first upload has nothing
         * to read, and a restore has nothing to back up. */
        return DAEMOON_ERR_NOT_FOUND;
    }

    g_open.application_id = app_id;
    g_open.writable = writable;
    g_open.mounted = 1;
    *out = &g_open;
    return DAEMOON_OK;
}

static daemoon_result_t open_save(void *ctx, const daemoon_title_t *t, daemoon_save_t **out)
{
    return mount_save(ctx, t, 0, out);
}

static daemoon_result_t open_save_write(void *ctx, const daemoon_title_t *t,
                                        daemoon_save_t **out)
{
    return mount_save(ctx, t, 1, out);
}

static daemoon_result_t close_save(void *ctx, daemoon_save_t *s)
{
    (void)ctx;
    if (s == NULL || !s->mounted) {
        return DAEMOON_OK;
    }
    (void)fsdevUnmountDevice(DAEMOON_NX_SAVE_DEVICE);
    s->mounted = 0;
    return DAEMOON_OK;
}

/* -------------------------------------------------------------------- files */

static daemoon_result_t list_entries(void *ctx, daemoon_save_t *s, daemoon_entry_cb cb,
                                     void *user)
{
    (void)ctx;
    if (s == NULL || !s->mounted) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }
    return daemoon_dir_walk(DAEMOON_NX_SAVE_ROOT, cb, user);
}

/* Every directory on the way to a file, because a restore writes a path the archive
 * does not have yet. The 3DS build learned this from a conformance case rather than
 * from a happy path. */
static daemoon_result_t make_parents(const char *abs)
{
    char work[DAEMOON_PATH_MAX * 2];
    char *slash;

    (void)daemoon_strlcpy(work, sizeof(work), abs);
    slash = strrchr(work, '/');
    if (slash == NULL) {
        return DAEMOON_OK;
    }
    *slash = '\0';

    for (slash = work; *slash != '\0'; ++slash) {
        if (*slash != '/' || slash == work) {
            continue;
        }
        *slash = '\0';
        if (strchr(work, ':') != NULL && strlen(work) == strcspn(work, ":") + 1) {
            /* "dmsave:" on its own is the device, not a directory. */
            *slash = '/';
            continue;
        }
        if (mkdir(work, 0777) != 0 && errno != EEXIST) {
            return DAEMOON_ERR_IO_ERROR;
        }
        *slash = '/';
    }
    if (mkdir(work, 0777) != 0 && errno != EEXIST) {
        return DAEMOON_ERR_IO_ERROR;
    }
    return DAEMOON_OK;
}

typedef struct {
    FILE *fp;
} nx_file_t;

static daemoon_result_t file_read(void *vctx, void *buf, size_t cap, size_t *out_len)
{
    nx_file_t *f = (nx_file_t *)vctx;
    size_t got = fread(buf, 1, cap, f->fp);

    if (got == 0 && ferror(f->fp)) {
        return DAEMOON_ERR_IO_ERROR;
    }
    *out_len = got;
    return DAEMOON_OK;
}

static daemoon_result_t file_write(void *vctx, const void *buf, size_t len)
{
    nx_file_t *f = (nx_file_t *)vctx;

    if (len == 0) {
        return DAEMOON_OK;
    }
    return fwrite(buf, 1, len, f->fp) == len ? DAEMOON_OK : DAEMOON_ERR_IO_ERROR;
}

static daemoon_result_t file_seek(void *vctx, unsigned long long offset)
{
    nx_file_t *f = (nx_file_t *)vctx;

    return fseek(f->fp, (long)offset, SEEK_SET) == 0 ? DAEMOON_OK : DAEMOON_ERR_IO_ERROR;
}

static daemoon_result_t file_close(void *vctx)
{
    nx_file_t *f = (nx_file_t *)vctx;
    daemoon_stream_t *stream;
    int rc;

    /* The stream and its context are one allocation, so this frees both. */
    rc = fclose(f->fp);
    stream = (daemoon_stream_t *)((char *)f - sizeof(daemoon_stream_t));
    free(stream);
    return rc == 0 ? DAEMOON_OK : DAEMOON_ERR_IO_ERROR;
}

static daemoon_result_t open_file(void *ctx, daemoon_save_t *s, const char *path,
                                  daemoon_open_mode_t mode, daemoon_stream_t **out)
{
    char abs[DAEMOON_PATH_MAX * 2];
    daemoon_strbuf_t sb;
    daemoon_stream_t *stream;
    nx_file_t *f;
    FILE *fp;

    (void)ctx;
    if (s == NULL || !s->mounted || path == NULL || out == NULL) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }
    if (mode == DAEMOON_OPEN_WRITE && !s->writable) {
        /* A read only handle refuses writes rather than discovering it at the first
         * fwrite. The conformance suite has a case for exactly this. */
        return DAEMOON_ERR_FORBIDDEN;
    }

    daemoon_strbuf_init(&sb, abs, sizeof(abs));
    daemoon_strbuf_add(&sb, DAEMOON_NX_SAVE_ROOT);
    daemoon_strbuf_add(&sb, path);
    DAEMOON_TRY(daemoon_strbuf_result(&sb));

    if (mode == DAEMOON_OPEN_WRITE) {
        DAEMOON_TRY(make_parents(abs));
    }

    fp = fopen(abs, mode == DAEMOON_OPEN_WRITE ? "wb" : "rb");
    if (fp == NULL) {
        return errno == ENOENT ? DAEMOON_ERR_NOT_FOUND : DAEMOON_ERR_IO_ERROR;
    }

    stream = (daemoon_stream_t *)calloc(1, sizeof(*stream) + sizeof(nx_file_t));
    if (stream == NULL) {
        (void)fclose(fp);
        return DAEMOON_ERR_OUT_OF_MEMORY;
    }
    f = (nx_file_t *)((char *)stream + sizeof(*stream));
    f->fp = fp;

    stream->ctx = f;
    stream->close = file_close;
    if (mode == DAEMOON_OPEN_WRITE) {
        stream->write = file_write;
    } else {
        struct stat st;

        stream->read = file_read;
        stream->seek = file_seek;
        if (stat(abs, &st) == 0) {
            stream->size = (unsigned long long)st.st_size;
        }
    }
    *out = stream;
    return DAEMOON_OK;
}

static daemoon_result_t remove_all(void *ctx, daemoon_save_t *s)
{
    (void)ctx;
    if (s == NULL || !s->mounted) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }
    if (!s->writable) {
        return DAEMOON_ERR_FORBIDDEN;
    }
    return daemoon_dir_remove_all(DAEMOON_NX_SAVE_ROOT);
}

static daemoon_result_t commit(void *ctx, daemoon_save_t *s)
{
    Result rc;

    (void)ctx;
    if (s == NULL || !s->mounted) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }
    /* The whole platform in one call. Without it the writes above sit in a cache and
     * the console finds out at the next power on, which is the same failure the 3DS
     * has and the same rule: never skip it, never ignore its result. */
    rc = fsdevCommitDevice(DAEMOON_NX_SAVE_DEVICE);
    daemoon_nx_trace("save/commit", R_SUCCEEDED(rc) ? "ok" : "failed");
    return from_libnx(rc);
}

const daemoon_save_backend_t daemoon_nx_save_backend = {
    list_titles,
    free_titles,
    open_save,
    open_save_write,
    list_entries,
    open_file,
    remove_all,
    commit,
    close_save,
    /* Whether a game is running is answerable here through pm, and is not answered
     * yet. The caller warns instead, which is what a NULL means - and on this console
     * an application cannot be running while this one is, because an application
     * replaces it. That is a claim for hardware to confirm rather than for this
     * pointer to assume. */
    NULL,
    /* Nothing on this platform is known to bind a save to the console the way a 3DS
     * secure value does. */
    NULL, NULL, NULL
};
