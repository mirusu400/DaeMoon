#include "test.h"

#include "fixture_env.h"

#include <daemoon/sync.h>
#include <daemoon/util/strbuf.h>

#include <stdlib.h>
#include <string.h>

static const char *const SHA_A =
    "1111111111111111111111111111111111111111111111111111111111111111";
static const char *const SHA_B =
    "2222222222222222222222222222222222222222222222222222222222222222";

static void set_local(daemoon_local_state_t *l, int has_save, const char *sha,
                      unsigned int base_version, const char *base_sha)
{
    memset(l, 0, sizeof(*l));
    l->has_save = has_save;
    if (sha != NULL) {
        (void)daemoon_strlcpy(l->sha256, sizeof(l->sha256), sha);
    }
    l->base_version = base_version;
    if (base_sha != NULL) {
        (void)daemoon_strlcpy(l->base_sha256, sizeof(l->base_sha256), base_sha);
    }
}

static void set_remote(daemoon_remote_meta_t *r, int exists, unsigned int latest,
                       const char *sha)
{
    memset(r, 0, sizeof(*r));
    r->exists = exists;
    r->latest_version = latest;
    if (sha != NULL) {
        (void)daemoon_strlcpy(r->sha256, sizeof(r->sha256), sha);
    }
}

/* -------------------------------------------------------- the decision table */

TEST_CASE(decide_covers_every_case)
{
    daemoon_local_state_t l;
    daemoon_remote_meta_t r;

    /* Nothing anywhere. */
    set_local(&l, 0, NULL, DAEMOON_VERSION_NONE, NULL);
    set_remote(&r, 0, 0, NULL);
    CHECK_EQ_INT(daemoon_sync_decide(&l, &r), DAEMOON_SYNC_NONE);

    /* Only this console has a save. */
    set_local(&l, 1, SHA_A, DAEMOON_VERSION_NONE, NULL);
    set_remote(&r, 0, 0, NULL);
    CHECK_EQ_INT(daemoon_sync_decide(&l, &r), DAEMOON_SYNC_UPLOAD);

    /* Only the server has one. */
    set_local(&l, 0, NULL, DAEMOON_VERSION_NONE, NULL);
    set_remote(&r, 1, 5, SHA_A);
    CHECK_EQ_INT(daemoon_sync_decide(&l, &r), DAEMOON_SYNC_DOWNLOAD);

    /* Identical content. The version bookkeeping does not matter. */
    set_local(&l, 1, SHA_A, 3, SHA_A);
    set_remote(&r, 1, 9, SHA_A);
    CHECK_EQ_INT(daemoon_sync_decide(&l, &r), DAEMOON_SYNC_NONE);

    /* This console played on top of the newest server version. */
    set_local(&l, 1, SHA_B, 5, SHA_A);
    set_remote(&r, 1, 5, SHA_A);
    CHECK_EQ_INT(daemoon_sync_decide(&l, &r), DAEMOON_SYNC_UPLOAD);

    /* The server moved on and this console did not touch its save. */
    set_local(&l, 1, SHA_A, 4, SHA_A);
    set_remote(&r, 1, 6, SHA_B);
    CHECK_EQ_INT(daemoon_sync_decide(&l, &r), DAEMOON_SYNC_DOWNLOAD);

    /* Both moved. Never merged, always asked. */
    set_local(&l, 1, SHA_B, 4, SHA_A);
    set_remote(&r, 1, 6, "3333333333333333333333333333333333333333333333333333333333333333");
    CHECK_EQ_INT(daemoon_sync_decide(&l, &r), DAEMOON_SYNC_CONFLICT);

    /* The server is behind what this console last synced: restored from a backup,
     * or the token now points at a different account. Never guess. */
    set_local(&l, 1, SHA_A, 9, SHA_A);
    set_remote(&r, 1, 4, SHA_B);
    CHECK_EQ_INT(daemoon_sync_decide(&l, &r), DAEMOON_SYNC_CONFLICT);

    /* Never synced but the server has something: a first run on a console that
     * already has a save. */
    set_local(&l, 1, SHA_A, DAEMOON_VERSION_NONE, NULL);
    set_remote(&r, 1, 2, SHA_B);
    CHECK_EQ_INT(daemoon_sync_decide(&l, &r), DAEMOON_SYNC_CONFLICT);
}

TEST_CASE(decide_never_consults_a_clock)
{
    /* There is no timestamp in either input struct, on purpose: the console RTC is
     * user settable, so a save "from the future" is a normal thing to encounter.
     * This test exists to make a future change that adds one fail here first. */
    daemoon_local_state_t l;
    daemoon_remote_meta_t r;

    set_local(&l, 1, SHA_B, 5, SHA_A);
    set_remote(&r, 1, 5, SHA_A);
    (void)daemoon_strlcpy(r.received_at, sizeof(r.received_at), "1999-01-01T00:00:00Z");
    CHECK_EQ_INT(daemoon_sync_decide(&l, &r), DAEMOON_SYNC_UPLOAD);

    (void)daemoon_strlcpy(r.received_at, sizeof(r.received_at), "2099-01-01T00:00:00Z");
    CHECK_EQ_INT(daemoon_sync_decide(&l, &r), DAEMOON_SYNC_UPLOAD);
}

TEST_CASE(dirty_tracking)
{
    daemoon_local_state_t l;

    set_local(&l, 0, NULL, DAEMOON_VERSION_NONE, NULL);
    CHECK(!daemoon_sync_local_dirty(&l));

    /* Never synced means everything about it is new. */
    set_local(&l, 1, SHA_A, DAEMOON_VERSION_NONE, NULL);
    CHECK(daemoon_sync_local_dirty(&l));

    set_local(&l, 1, SHA_A, 3, SHA_A);
    CHECK(!daemoon_sync_local_dirty(&l));

    set_local(&l, 1, SHA_B, 3, SHA_A);
    CHECK(daemoon_sync_local_dirty(&l));
}

/* ---------------------------------------------------------------- state file */

TEST_CASE(state_survives_a_round_trip)
{
    fixture_t f;
    daemoon_local_state_t in;
    daemoon_local_state_t out;

    CHECK_EQ_INT(fixture_open(&f, "state"), 0);

    /* Nothing recorded yet is not an error. */
    memset(&out, 0, sizeof(out));
    CHECK_OK(daemoon_sync_state_load(&f.env, DAEMOON_PLATFORM_3DS, "0004000000055D00", &out));
    CHECK_EQ_INT(out.base_version, DAEMOON_VERSION_NONE);

    set_local(&in, 1, SHA_A, 42, SHA_A);
    CHECK_OK(daemoon_sync_state_save(&f.env, DAEMOON_PLATFORM_3DS, "0004000000055D00", &in));

    memset(&out, 0, sizeof(out));
    CHECK_OK(daemoon_sync_state_load(&f.env, DAEMOON_PLATFORM_3DS, "0004000000055D00", &out));
    CHECK_EQ_INT(out.base_version, 42);
    CHECK_STR(out.base_sha256, SHA_A);

    /* A title id is only unique together with its platform. */
    memset(&out, 0, sizeof(out));
    CHECK_OK(daemoon_sync_state_load(&f.env, DAEMOON_PLATFORM_NX, "0004000000055D00", &out));
    CHECK_EQ_INT(out.base_version, DAEMOON_VERSION_NONE);

    fixture_close(&f);
}

/* ------------------------------------------------------------- upload path */

TEST_CASE(first_upload)
{
    fixture_t f;
    const daemoon_title_t *t;
    daemoon_sync_stats_t stats;
    daemoon_local_state_t st;
    const fake_version_t *v;

    CHECK_EQ_INT(fixture_open(&f, "upload"), 0);
    t = fixture_add_title(&f, "0004000000055D00", DAEMOON_PLATFORM_3DS);
    CHECK_EQ_INT(fixture_write_save_file(&f, t, "main.sav", "player data v1"), 0);

    memset(&stats, 0, sizeof(stats));
    CHECK_OK(daemoon_sync_title(&f.env, &f.actx, t, &stats));

    CHECK_EQ_INT(stats.uploaded, 1);
    CHECK_EQ_INT(f.server.uploads, 1);
    CHECK_EQ_INT(f.ui.confirms, 1); /* nothing leaves the console unasked */

    v = fake_server_latest(&f.server, DAEMOON_PLATFORM_3DS, "0004000000055D00");
    CHECK(v != NULL);
    CHECK_EQ_INT(v->version, 1);

    /* The version the server issued is now what this console is based on, so a
     * second run has nothing to do. */
    CHECK_OK(daemoon_sync_state_load(&f.env, DAEMOON_PLATFORM_3DS, "0004000000055D00", &st));
    CHECK_EQ_INT(st.base_version, 1);
    CHECK_STR(st.base_sha256, v->sha256);

    memset(&stats, 0, sizeof(stats));
    CHECK_OK(daemoon_sync_title(&f.env, &f.actx, t, &stats));
    CHECK_EQ_INT(stats.skipped, 1);
    CHECK_EQ_INT(f.server.uploads, 1);

    fixture_close(&f);
}

TEST_CASE(declining_the_upload_changes_nothing)
{
    fixture_t f;
    const daemoon_title_t *t;
    daemoon_sync_stats_t stats;

    CHECK_EQ_INT(fixture_open(&f, "decline"), 0);
    t = fixture_add_title(&f, "0004000000055D00", DAEMOON_PLATFORM_3DS);
    CHECK_EQ_INT(fixture_write_save_file(&f, t, "main.sav", "player data"), 0);

    f.ui.confirm_answer = 0;
    memset(&stats, 0, sizeof(stats));
    CHECK_RESULT(daemoon_sync_title(&f.env, &f.actx, t, &stats), DAEMOON_ERR_USER_CANCELLED);
    CHECK_EQ_INT(f.server.uploads, 0);

    fixture_close(&f);
}

/* ----------------------------------------------------------- download path */

TEST_CASE(download_backs_up_verifies_and_commits)
{
    fixture_t f;
    const daemoon_title_t *t;
    daemoon_sync_stats_t stats;
    unsigned char *blob = NULL;
    size_t blob_len = 0;
    char buf[256];

    CHECK_EQ_INT(fixture_open(&f, "download"), 0);
    t = fixture_add_title(&f, "0004000000055D00", DAEMOON_PLATFORM_3DS);

    /* Build what another console would have uploaded, then put this console's own
     * save back to an older state. */
    CHECK_EQ_INT(fixture_write_save_file(&f, t, "main.sav", "newer data from elsewhere"), 0);
    CHECK_OK(fixture_pack_blob(&f, t, DAEMOON_VERSION_NONE, &blob, &blob_len));
    CHECK_OK(fake_server_put(&f.server, DAEMOON_PLATFORM_3DS, "0004000000055D00", blob, blob_len,
                             "other console"));
    free(blob);

    CHECK_EQ_INT(fixture_write_save_file(&f, t, "main.sav", "stale local data"), 0);
    CHECK_EQ_INT(fixture_write_save_file(&f, t, "leftover.bin", "should be removed"), 0);

    /* Pretend this console already synced at version 1 and has not touched the save
     * since, so the decision is a plain download rather than a conflict. */
    {
        daemoon_local_state_t st;
        daemoon_local_state_t scanned;
        CHECK_OK(daemoon_sync_scan_local(&f.env, &f.actx, t, &scanned));
        set_local(&st, 1, scanned.sha256, 1, scanned.sha256);
        CHECK_OK(daemoon_sync_state_save(&f.env, DAEMOON_PLATFORM_3DS, "0004000000055D00", &st));
    }
    /* The server is at version 1 as well, so move it forward with a second upload
     * of the same package to make the server strictly newer. */
    CHECK_EQ_INT(fixture_write_save_file(&f, t, "main.sav", "newer data from elsewhere"), 0);
    CHECK_EQ_INT(fixture_write_save_file(&f, t, "leftover.bin", "should be removed"), 0);
    CHECK_OK(fixture_pack_blob(&f, t, 1, &blob, &blob_len));
    CHECK_OK(fake_server_put(&f.server, DAEMOON_PLATFORM_3DS, "0004000000055D00", blob, blob_len,
                             "other console"));
    free(blob);
    CHECK_EQ_INT(fixture_write_save_file(&f, t, "main.sav", "stale local data"), 0);
    (void)fixture_write_save_file(&f, t, "leftover.bin", "should be removed");

    memset(&stats, 0, sizeof(stats));
    CHECK_OK(daemoon_sync_title(&f.env, &f.actx, t, &stats));
    CHECK_EQ_INT(stats.downloaded, 1);

    /* A local backup was made before anything was written. */
    CHECK(fixture_backup_count(&f) >= 1);

    /* The archive was committed. Without this nothing is persisted on a console. */
    CHECK(f.save.commits >= 1);

    CHECK_EQ_INT(fixture_read_save_file(&f, t, "main.sav", buf, sizeof(buf)), 0);
    CHECK_STR(buf, "newer data from elsewhere");

    fixture_close(&f);
}

TEST_CASE(a_corrupt_download_never_reaches_the_save)
{
    fixture_t f;
    const daemoon_title_t *t;
    daemoon_sync_stats_t stats;
    daemoon_local_state_t st;
    daemoon_local_state_t scanned;
    unsigned char *blob = NULL;
    size_t blob_len = 0;
    char buf[256];
    daemoon_result_t r;

    CHECK_EQ_INT(fixture_open(&f, "corrupt"), 0);
    t = fixture_add_title(&f, "0004000000055D00", DAEMOON_PLATFORM_3DS);

    /* Version 1 is what this console last synced. */
    CHECK_EQ_INT(fixture_write_save_file(&f, t, "main.sav", "precious local data"), 0);
    CHECK_OK(fixture_pack_blob(&f, t, DAEMOON_VERSION_NONE, &blob, &blob_len));
    CHECK_OK(fake_server_put(&f.server, DAEMOON_PLATFORM_3DS, t->id, blob, blob_len, "other"));
    free(blob);

    CHECK_OK(daemoon_sync_scan_local(&f.env, &f.actx, t, &scanned));
    set_local(&st, 1, scanned.sha256, 1, scanned.sha256);
    CHECK_OK(daemoon_sync_state_save(&f.env, DAEMOON_PLATFORM_3DS, t->id, &st));

    /* Version 2 arrives from elsewhere, so this is a plain download. */
    CHECK_EQ_INT(fixture_write_save_file(&f, t, "main.sav", "server side data"), 0);
    CHECK_OK(fixture_pack_blob(&f, t, 1, &blob, &blob_len));
    CHECK_OK(fake_server_put(&f.server, DAEMOON_PLATFORM_3DS, t->id, blob, blob_len, "other"));
    free(blob);
    CHECK_EQ_INT(fixture_write_save_file(&f, t, "main.sav", "precious local data"), 0);

    /* The server now advertises a digest the package it serves does not have. That
     * is what a damaged transfer or a mixed up blob looks like from here, and the
     * package would happily verify against its own manifest. */
    (void)daemoon_strlcpy(f.server.titles[0].versions[1].sha256, DAEMOON_SHA256_HEX,
                          "0000000000000000000000000000000000000000000000000000000000000000");

    memset(&stats, 0, sizeof(stats));
    r = daemoon_sync_title(&f.env, &f.actx, t, &stats);
    CHECK_RESULT(r, DAEMOON_ERR_CHECKSUM_MISMATCH);
    CHECK_EQ_INT(f.ui.last_notify, DAEMOON_STR_VERIFY_FAILED);

    /* The save on the console is exactly as it was. */
    CHECK_EQ_INT(fixture_read_save_file(&f, t, "main.sav", buf, sizeof(buf)), 0);
    CHECK_STR(buf, "precious local data");

    fixture_close(&f);
}

TEST_CASE(a_failed_commit_is_a_failed_restore)
{
    fixture_t f;
    const daemoon_title_t *t;
    unsigned char *blob = NULL;
    size_t blob_len = 0;
    char pkg_path[400];
    daemoon_strbuf_t sb;
    daemoon_stream_t *out = NULL;
    daemoon_result_t r;

    CHECK_EQ_INT(fixture_open(&f, "commitfail"), 0);
    t = fixture_add_title(&f, "0004000000055D00", DAEMOON_PLATFORM_3DS);
    CHECK_EQ_INT(fixture_write_save_file(&f, t, "main.sav", "incoming"), 0);
    CHECK_OK(fixture_pack_blob(&f, t, DAEMOON_VERSION_NONE, &blob, &blob_len));

    daemoon_strbuf_init(&sb, pkg_path, sizeof(pkg_path));
    daemoon_strbuf_add(&sb, f.work);
    daemoon_strbuf_add(&sb, "/incoming.zip");
    CHECK_OK(daemoon_strbuf_result(&sb));

    CHECK_OK(f.env.fs->open(f.env.fs_ctx, pkg_path, DAEMOON_OPEN_WRITE, &out));
    CHECK_OK(daemoon_stream_write(out, blob, blob_len));
    CHECK_OK(daemoon_stream_close(out));
    free(blob);

    /* On a console a failed commit means nothing was persisted. It has to surface
     * as a failure and the user has to be told not to launch the game. */
    f.save.fail_commit = DAEMOON_ERR_BACKEND_ERROR;

    r = daemoon_sync_restore_package(&f.env, &f.actx, t, pkg_path);
    CHECK_RESULT(r, DAEMOON_ERR_BACKEND_ERROR);
    CHECK_EQ_INT(f.ui.last_notify, DAEMOON_STR_COMMIT_FAILED);

    fixture_close(&f);
}

/* ---------------------------------------------------------- secure value */

/* A stand in for the console stored value. The posix backend has no such concept, so
 * the cases below wrap it: what is being checked is that core packs the value with the
 * save and writes it back from the package, not how a 3DS stores one. */
static int          g_sv_exists;
static unsigned long long g_sv_value;
static unsigned     g_sv_writes;
static unsigned     g_sv_clears;
static int          g_sv_read_fails;

static daemoon_result_t sv_read(void *ctx, const daemoon_title_t *t, int *exists,
                                unsigned long long *value)
{
    (void)ctx; (void)t;
    if (g_sv_read_fails) {
        return DAEMOON_ERR_BACKEND_ERROR;
    }
    *exists = g_sv_exists;
    *value = g_sv_value;
    return DAEMOON_OK;
}

static daemoon_result_t sv_write(void *ctx, const daemoon_title_t *t,
                                 unsigned long long value)
{
    (void)ctx; (void)t;
    ++g_sv_writes;
    g_sv_exists = 1;
    g_sv_value = value;
    return DAEMOON_OK;
}

static daemoon_result_t sv_clear(void *ctx, const daemoon_title_t *t)
{
    (void)ctx; (void)t;
    ++g_sv_clears;
    g_sv_exists = 0;
    g_sv_value = 0;
    return DAEMOON_OK;
}

/* Wires the hooks onto whatever backend the fixture is using. */
static daemoon_save_backend_t with_secure_value(const daemoon_save_backend_t *base)
{
    daemoon_save_backend_t b = *base;

    b.read_secure_value = sv_read;
    b.write_secure_value = sv_write;
    b.clear_secure_value = sv_clear;
    return b;
}

/* The value is part of the save, not part of the console: a backup that does not carry
 * it cannot be fully restored, because the console's may have moved on since. */
TEST_CASE(a_backup_carries_the_value_its_save_was_bound_to)
{
    fixture_t f;
    const daemoon_title_t *t;
    daemoon_save_backend_t backend;
    char pkg[DAEMOON_PATH_MAX];
    char buf[256];

    CHECK_EQ_INT(fixture_open(&f, "secure-backup"), 0);
    t = fixture_add_title(&f, "0004000000055D00", DAEMOON_PLATFORM_3DS);
    (void)fixture_write_save_file(&f, t, "main.sav", "bound to a value");

    backend = with_secure_value(f.env.save);
    f.env.save = &backend;
    g_sv_exists = 1;
    g_sv_value = 0x0123456789ABCDEFull;
    g_sv_writes = 0;
    g_sv_clears = 0;
    g_sv_read_fails = 0;

    CHECK_OK(daemoon_sync_backup_local(&f.env, &f.actx, t, pkg, sizeof(pkg)));

    /* The console moves on: the game played again and the value changed. This is the
     * case the old behaviour got wrong - it put *this* value back after a restore,
     * not the one the restored save belongs with. */
    g_sv_value = 0xFFFFFFFFFFFFFFFFull;
    (void)fixture_write_save_file(&f, t, "main.sav", "played since");

    CHECK_OK(daemoon_sync_restore_package(&f.env, &f.actx, t, pkg));

    CHECK_EQ_INT(fixture_read_save_file(&f, t, "main.sav", buf, sizeof(buf)), 0);
    CHECK_STR(buf, "bound to a value");
    /* Written back from the package, and it is the value from backup time. */
    CHECK_EQ_INT((int)g_sv_writes, 1);
    CHECK(g_sv_value == 0x0123456789ABCDEFull);

    fixture_close(&f);
}

/* Zero is a legitimate value and "this title has none" is a different state. A backend
 * that reports no value must not produce a package that writes zero over one. */
TEST_CASE(a_title_with_no_value_produces_a_package_that_writes_none)
{
    fixture_t f;
    const daemoon_title_t *t;
    daemoon_save_backend_t backend;
    char pkg[DAEMOON_PATH_MAX];

    CHECK_EQ_INT(fixture_open(&f, "secure-none"), 0);
    t = fixture_add_title(&f, "0004000000055D00", DAEMOON_PLATFORM_3DS);
    (void)fixture_write_save_file(&f, t, "main.sav", "no value here");

    backend = with_secure_value(f.env.save);
    f.env.save = &backend;
    g_sv_exists = 0;
    g_sv_value = 0;
    g_sv_writes = 0;
    g_sv_clears = 0;
    g_sv_read_fails = 0;

    CHECK_OK(daemoon_sync_backup_local(&f.env, &f.actx, t, pkg, sizeof(pkg)));
    CHECK_OK(daemoon_sync_restore_package(&f.env, &f.actx, t, pkg));

    CHECK_EQ_INT((int)g_sv_writes, 0);

    fixture_close(&f);
}

/* A backend that cannot read one still produces a usable backup. Refusing would make a
 * diagnostic failure into a save nobody has a copy of. */
TEST_CASE(a_backend_that_cannot_read_the_value_still_backs_the_save_up)
{
    fixture_t f;
    const daemoon_title_t *t;
    daemoon_save_backend_t backend;
    char pkg[DAEMOON_PATH_MAX];
    char buf[256];

    CHECK_EQ_INT(fixture_open(&f, "secure-unreadable"), 0);
    t = fixture_add_title(&f, "0004000000055D00", DAEMOON_PLATFORM_3DS);
    (void)fixture_write_save_file(&f, t, "main.sav", "still worth keeping");

    backend = with_secure_value(f.env.save);
    f.env.save = &backend;
    g_sv_read_fails = 1;
    g_sv_writes = 0;
    g_sv_clears = 0;

    CHECK_OK(daemoon_sync_backup_local(&f.env, &f.actx, t, pkg, sizeof(pkg)));
    (void)fixture_write_save_file(&f, t, "main.sav", "changed");
    CHECK_OK(daemoon_sync_restore_package(&f.env, &f.actx, t, pkg));

    CHECK_EQ_INT(fixture_read_save_file(&f, t, "main.sav", buf, sizeof(buf)), 0);
    CHECK_STR(buf, "still worth keeping");
    /* Nothing recorded, so nothing written: the console keeps what it has, which is
     * what every package written before this field existed does. */
    CHECK_EQ_INT((int)g_sv_writes, 0);

    fixture_close(&f);
}

/* A package written before this field existed carries no value, and the console still
 * holds the one belonging to the save being replaced. Leaving it is what made a real
 * restore fail on hardware: Pokemon Y refused the save as "not the data that was saved
 * last", because the value did not match it.
 *
 * So the value is removed. Nothing recorded means nothing to mismatch, and the game
 * writes a fresh one the next time it saves. */
TEST_CASE(a_package_without_a_value_clears_the_consoles_rather_than_leaving_a_mismatch)
{
    fixture_t f;
    const daemoon_title_t *t;
    daemoon_save_backend_t backend;
    char pkg[DAEMOON_PATH_MAX];
    char buf[256];

    CHECK_EQ_INT(fixture_open(&f, "secure-legacy"), 0);
    t = fixture_add_title(&f, "0004000000055D00", DAEMOON_PLATFORM_3DS);
    (void)fixture_write_save_file(&f, t, "main.sav", "packed by an older build");

    /* Packed with no hooks at all, the way the backend used to be. */
    CHECK_OK(daemoon_sync_backup_local(&f.env, &f.actx, t, pkg, sizeof(pkg)));

    backend = with_secure_value(f.env.save);
    f.env.save = &backend;
    /* The console has moved on and holds a value for the save about to be replaced. */
    g_sv_exists = 1;
    g_sv_value = 0xAAAAAAAAAAAAAAAAull;
    g_sv_writes = 0;
    g_sv_clears = 0;
    g_sv_read_fails = 0;
    (void)fixture_write_save_file(&f, t, "main.sav", "played since");

    CHECK_OK(daemoon_sync_restore_package(&f.env, &f.actx, t, pkg));

    CHECK_EQ_INT(fixture_read_save_file(&f, t, "main.sav", buf, sizeof(buf)), 0);
    CHECK_STR(buf, "packed by an older build");

    /* Removed, not overwritten: writing a value the package never had would be
     * inventing one, and zero is a legitimate value rather than an absence. */
    CHECK_EQ_INT((int)g_sv_clears, 1);
    CHECK_EQ_INT((int)g_sv_writes, 0);
    CHECK_EQ_INT(g_sv_exists, 0);

    fixture_close(&f);
}

/* A backend with no such concept is not asked to clear anything. NULL hooks mean the
 * platform has no value, not that it has one worth removing. */
TEST_CASE(a_platform_without_secure_values_is_left_alone_entirely)
{
    fixture_t f;
    const daemoon_title_t *t;
    char pkg[DAEMOON_PATH_MAX];

    CHECK_EQ_INT(fixture_open(&f, "secure-absent"), 0);
    t = fixture_add_title(&f, "0004000000055D00", DAEMOON_PLATFORM_3DS);
    (void)fixture_write_save_file(&f, t, "main.sav", "no hooks at all");

    g_sv_clears = 0;
    g_sv_writes = 0;

    CHECK_OK(daemoon_sync_backup_local(&f.env, &f.actx, t, pkg, sizeof(pkg)));
    CHECK_OK(daemoon_sync_restore_package(&f.env, &f.actx, t, pkg));

    CHECK_EQ_INT((int)g_sv_clears, 0);
    CHECK_EQ_INT((int)g_sv_writes, 0);

    fixture_close(&f);
}

/* -------------------------------------------------------------- conflicts */

/* Puts the fixture into the state where both sides moved. */
static void make_conflict(fixture_t *f, const daemoon_title_t *t)
{
    unsigned char *blob = NULL;
    size_t blob_len = 0;
    daemoon_local_state_t st;
    daemoon_local_state_t scanned;

    /* A shared starting point, synced as version 1. */
    (void)fixture_write_save_file(f, t, "main.sav", "common ancestor");
    (void)daemoon_sync_scan_local(&f->env, &f->actx, t, &scanned);
    set_local(&st, 1, scanned.sha256, 1, scanned.sha256);
    (void)daemoon_sync_state_save(&f->env, DAEMOON_PLATFORM_3DS, t->id, &st);

    /* The other console uploaded twice, so the server is at version 2. */
    (void)fixture_write_save_file(f, t, "main.sav", "ancestor");
    (void)fixture_pack_blob(f, t, DAEMOON_VERSION_NONE, &blob, &blob_len);
    (void)fake_server_put(&f->server, DAEMOON_PLATFORM_3DS, t->id, blob, blob_len, "other console");
    free(blob);

    (void)fixture_write_save_file(f, t, "main.sav", "server side progress");
    (void)fixture_pack_blob(f, t, 1, &blob, &blob_len);
    (void)fake_server_put(&f->server, DAEMOON_PLATFORM_3DS, t->id, blob, blob_len, "other console");
    free(blob);

    /* Meanwhile this console kept playing. */
    (void)fixture_write_save_file(f, t, "main.sav", "local progress");
}

TEST_CASE(conflict_keeping_local_uploads_without_discarding_the_server_copy)
{
    fixture_t f;
    const daemoon_title_t *t;
    daemoon_sync_stats_t stats;
    char buf[256];

    CHECK_EQ_INT(fixture_open(&f, "conflict-local"), 0);
    t = fixture_add_title(&f, "0004000000055D00", DAEMOON_PLATFORM_3DS);
    make_conflict(&f, t);

    f.ui.choose_answer = DAEMOON_CONFLICT_KEEP_LOCAL;
    memset(&stats, 0, sizeof(stats));
    CHECK_OK(daemoon_sync_title(&f.env, &f.actx, t, &stats));

    CHECK_EQ_INT(f.ui.chooses, 1);
    CHECK_EQ_INT(stats.uploaded, 1);

    /* Both versions are retained: the server keeps what it had and adds the new
     * one on top. */
    CHECK_EQ_INT(f.server.titles[0].nversions, 3);
    CHECK_EQ_INT(f.server.titles[0].latest_version, 3);

    /* The console keeps its own save. */
    CHECK_EQ_INT(fixture_read_save_file(&f, t, "main.sav", buf, sizeof(buf)), 0);
    CHECK_STR(buf, "local progress");

    fixture_close(&f);
}

TEST_CASE(conflict_keeping_the_server_copy_backs_up_first)
{
    fixture_t f;
    const daemoon_title_t *t;
    daemoon_sync_stats_t stats;
    char buf[256];

    CHECK_EQ_INT(fixture_open(&f, "conflict-server"), 0);
    t = fixture_add_title(&f, "0004000000055D00", DAEMOON_PLATFORM_3DS);
    make_conflict(&f, t);

    f.ui.choose_answer = DAEMOON_CONFLICT_KEEP_SERVER;
    memset(&stats, 0, sizeof(stats));
    CHECK_OK(daemoon_sync_title(&f.env, &f.actx, t, &stats));

    CHECK_EQ_INT(stats.downloaded, 1);
    CHECK_EQ_INT(fixture_read_save_file(&f, t, "main.sav", buf, sizeof(buf)), 0);
    CHECK_STR(buf, "server side progress");

    /* The overwritten local save is still on the SD card. Losing it would be
     * unrecoverable. */
    CHECK(fixture_backup_count(&f) >= 1);

    fixture_close(&f);
}

/* A policy answers the conflict without asking, and answers it the same way the
 * dialog would have. These are the two that a run over a whole library offers, and
 * the property that makes them safe is that neither loses a version. */
TEST_CASE(a_policy_of_keeping_local_never_asks_and_keeps_every_server_version)
{
    fixture_t f;
    daemoon_sync_opts_t opts = { DAEMOON_CONFLICT_POLICY_ASK, 0 };
    const daemoon_title_t *t;
    daemoon_sync_stats_t stats;
    char buf[256];

    CHECK_EQ_INT(fixture_open(&f, "policy-local"), 0);
    t = fixture_add_title(&f, "0004000000055D00", DAEMOON_PLATFORM_3DS);
    make_conflict(&f, t);

    /* Answering DEFER would be the outcome if the dialog were reached, so the
     * result below is only possible if it was not. */
    f.ui.choose_answer = DAEMOON_CONFLICT_DEFER;
    memset(&stats, 0, sizeof(stats));
    opts.conflict = DAEMOON_CONFLICT_POLICY_KEEP_LOCAL;
    CHECK_OK(daemoon_sync_title_with(&f.env, &f.actx, t, &opts, &stats));

    CHECK_EQ_INT(f.ui.chooses, 0);
    CHECK_EQ_INT(stats.uploaded, 1);
    CHECK_EQ_INT(stats.conflicts, 0);

    /* Nothing was discarded: the server's two versions are still there under the
     * new one. This is what makes the policy recoverable rather than a force flag. */
    CHECK_EQ_INT(f.server.titles[0].nversions, 3);
    CHECK_EQ_INT(f.server.titles[0].latest_version, 3);
    CHECK_EQ_INT(fixture_read_save_file(&f, t, "main.sav", buf, sizeof(buf)), 0);
    CHECK_STR(buf, "local progress");

    fixture_close(&f);
}

TEST_CASE(a_policy_of_keeping_the_server_still_backs_the_console_up_first)
{
    fixture_t f;
    daemoon_sync_opts_t opts = { DAEMOON_CONFLICT_POLICY_ASK, 0 };
    const daemoon_title_t *t;
    daemoon_sync_stats_t stats;
    char buf[256];

    CHECK_EQ_INT(fixture_open(&f, "policy-server"), 0);
    t = fixture_add_title(&f, "0004000000055D00", DAEMOON_PLATFORM_3DS);
    make_conflict(&f, t);

    f.ui.choose_answer = DAEMOON_CONFLICT_DEFER;
    memset(&stats, 0, sizeof(stats));
    opts.conflict = DAEMOON_CONFLICT_POLICY_KEEP_SERVER;
    CHECK_OK(daemoon_sync_title_with(&f.env, &f.actx, t, &opts, &stats));

    CHECK_EQ_INT(f.ui.chooses, 0);
    CHECK_EQ_INT(stats.downloaded, 1);
    CHECK_EQ_INT(fixture_read_save_file(&f, t, "main.sav", buf, sizeof(buf)), 0);
    CHECK_STR(buf, "server side progress");

    /* Rule 1 does not bend for a policy. The save that was overwritten is on the
     * card, which is the whole reason this option can be offered at all. */
    CHECK(fixture_backup_count(&f) >= 1);

    fixture_close(&f);
}

/* A conflict policy on its own does not touch the restore confirmation. Answering
 * "which side wins" is not answering "overwrite this console's save": a caller has
 * to say so separately, and this is what says the two are separate. */
TEST_CASE(a_policy_alone_does_not_skip_the_confirmation_before_a_restore)
{
    fixture_t f;
    daemoon_sync_opts_t opts = { DAEMOON_CONFLICT_POLICY_ASK, 0 };
    const daemoon_title_t *t;
    daemoon_sync_stats_t stats;
    char buf[256];

    CHECK_EQ_INT(fixture_open(&f, "policy-confirm"), 0);
    t = fixture_add_title(&f, "0004000000055D00", DAEMOON_PLATFORM_3DS);
    make_conflict(&f, t);

    f.ui.choose_answer = DAEMOON_CONFLICT_DEFER;
    f.ui.confirm_answer = 0;
    memset(&stats, 0, sizeof(stats));
    opts.conflict = DAEMOON_CONFLICT_POLICY_KEEP_SERVER;
    opts.upload_confirmed = 1;
    (void)daemoon_sync_title_with(&f.env, &f.actx, t, &opts, &stats);

    CHECK(f.ui.confirms > 0);
    CHECK_EQ_INT(stats.downloaded, 0);
    /* Refused, so the console still holds its own save. */
    CHECK_EQ_INT(fixture_read_save_file(&f, t, "main.sav", buf, sizeof(buf)), 0);
    CHECK_STR(buf, "local progress");

    fixture_close(&f);
}

/* A run over a library answers the upload question once. Every title that only
 * moved here goes up without a dialog of its own. */
TEST_CASE(an_answered_upload_question_is_not_asked_again_per_title)
{
    fixture_t f;
    const daemoon_title_t *t;
    daemoon_sync_stats_t stats;
    daemoon_sync_opts_t opts = { DAEMOON_CONFLICT_POLICY_ASK, 1 };

    CHECK_EQ_INT(fixture_open(&f, "opts-upload"), 0);
    t = fixture_add_title(&f, "0004000000055D00", DAEMOON_PLATFORM_3DS);
    (void)fixture_write_save_file(&f, t, "main.sav", "only here");

    /* Refusing every confirmation, so an upload that happens anyway is one that was
     * never asked about. */
    f.ui.confirm_answer = 0;
    memset(&stats, 0, sizeof(stats));
    CHECK_OK(daemoon_sync_title_with(&f.env, &f.actx, t, &opts, &stats));

    CHECK_EQ_INT(f.ui.confirms, 0);
    CHECK_EQ_INT(stats.uploaded, 1);
    CHECK_EQ_INT(f.server.titles[0].nversions, 1);

    fixture_close(&f);
}

/* And without it the question is asked, which is what the single title path does. */
TEST_CASE(the_upload_question_is_asked_when_it_has_not_been_answered)
{
    fixture_t f;
    const daemoon_title_t *t;
    daemoon_sync_stats_t stats;

    CHECK_EQ_INT(fixture_open(&f, "opts-upload-ask"), 0);
    t = fixture_add_title(&f, "0004000000055D00", DAEMOON_PLATFORM_3DS);
    (void)fixture_write_save_file(&f, t, "main.sav", "only here");

    f.ui.confirm_answer = 0;
    memset(&stats, 0, sizeof(stats));
    CHECK_EQ_INT((int)daemoon_sync_title(&f.env, &f.actx, t, &stats),
                 (int)DAEMOON_ERR_USER_CANCELLED);

    CHECK_EQ_INT(f.ui.confirms, 1);
    CHECK_EQ_INT(stats.uploaded, 0);
    CHECK_EQ_INT(f.server.uploads, 0);

    fixture_close(&f);
}

/* And with it, a run that already asked does not ask again. This is rule 7 being
 * answered once for a batch rather than bypassed: the caller has shown a sentence
 * naming the count and saying that saves will be overwritten.
 *
 * The two things that must survive it are checked here, because they are what make
 * the answer recoverable rather than final: the save is on the card afterwards, and
 * the server still holds the version that was replaced. */
TEST_CASE(an_answered_restore_question_is_not_asked_again_but_the_backup_still_happens)
{
    fixture_t f;
    const daemoon_title_t *t;
    daemoon_sync_stats_t stats;
    daemoon_sync_opts_t opts = { DAEMOON_CONFLICT_POLICY_KEEP_SERVER, 1, 1 };
    char buf[256];

    CHECK_EQ_INT(fixture_open(&f, "opts-restore"), 0);
    t = fixture_add_title(&f, "0004000000055D00", DAEMOON_PLATFORM_3DS);
    make_conflict(&f, t);

    /* Every dialog answers no, so anything that happens was never asked about. */
    f.ui.confirm_answer = 0;
    f.ui.choose_answer = DAEMOON_CONFLICT_DEFER;
    memset(&stats, 0, sizeof(stats));
    CHECK_OK(daemoon_sync_title_with(&f.env, &f.actx, t, &opts, &stats));

    CHECK_EQ_INT(f.ui.confirms, 0);
    CHECK_EQ_INT(f.ui.chooses, 0);
    CHECK_EQ_INT(stats.downloaded, 1);
    CHECK_EQ_INT(fixture_read_save_file(&f, t, "main.sav", buf, sizeof(buf)), 0);
    CHECK_STR(buf, "server side progress");

    /* Rule 1 does not have a field and cannot be turned off. The save that was
     * overwritten is on the card. */
    CHECK(fixture_backup_count(&f) >= 1);
    /* And the other side is still on the server, so neither version is gone. */
    CHECK_EQ_INT(f.server.titles[0].nversions, 2);

    fixture_close(&f);
}

/* The published restore always asks, whatever a batch elsewhere has answered. A
 * caller holding a package has not been through a screen that named a count. */
TEST_CASE(the_published_restore_has_no_way_to_skip_its_question)
{
    fixture_t f;
    const daemoon_title_t *t;
    char pkg_path[DAEMOON_PATH_MAX];
    char buf[256];

    CHECK_EQ_INT(fixture_open(&f, "restore-always-asks"), 0);
    t = fixture_add_title(&f, "0004000000055D00", DAEMOON_PLATFORM_3DS);
    (void)fixture_write_save_file(&f, t, "main.sav", "what is there now");
    CHECK_OK(daemoon_sync_backup_local(&f.env, &f.actx, t, pkg_path, sizeof(pkg_path)));
    (void)fixture_write_save_file(&f, t, "main.sav", "changed since");

    f.ui.confirm_answer = 0;
    CHECK_EQ_INT((int)daemoon_sync_restore_package(&f.env, &f.actx, t, pkg_path),
                 (int)DAEMOON_ERR_USER_CANCELLED);
    CHECK(f.ui.confirms > 0);
    CHECK_EQ_INT(fixture_read_save_file(&f, t, "main.sav", buf, sizeof(buf)), 0);
    CHECK_STR(buf, "changed since");

    fixture_close(&f);
}

/* The policy an unattended run uses. Nobody is there, so nothing is chosen: both
 * versions stay where they are and the title is counted for somebody to come back
 * to. This is the assertion that a sync at startup cannot lose a save. */
TEST_CASE(a_deferring_policy_leaves_both_sides_untouched_and_asks_nothing)
{
    fixture_t f;
    const daemoon_title_t *t;
    daemoon_sync_stats_t stats;
    /* Exactly what daemoon_3ds_autosync_opts returns: defer, and both questions
     * already answered, because there is no screen to answer them on. */
    daemoon_sync_opts_t opts = { DAEMOON_CONFLICT_POLICY_DEFER, 1, 1 };
    char buf[256];

    CHECK_EQ_INT(fixture_open(&f, "policy-defer"), 0);
    t = fixture_add_title(&f, "0004000000055D00", DAEMOON_PLATFORM_3DS);
    make_conflict(&f, t);

    memset(&stats, 0, sizeof(stats));
    CHECK_OK(daemoon_sync_title_with(&f.env, &f.actx, t, &opts, &stats));

    /* No dialog of any kind was reached, which is the point: this runs on a console
     * nobody is holding. */
    CHECK_EQ_INT(f.ui.chooses, 0);
    CHECK_EQ_INT(f.ui.confirms, 0);

    CHECK_EQ_INT(stats.conflicts, 1);
    CHECK_EQ_INT(stats.uploaded, 0);
    CHECK_EQ_INT(stats.downloaded, 0);

    /* Neither side moved. The server is where it was and the console still holds its
     * own save, so the choice is still there to be made later. */
    CHECK_EQ_INT(f.server.uploads, 0);
    CHECK_EQ_INT(f.server.titles[0].nversions, 2);
    CHECK_EQ_INT(fixture_read_save_file(&f, t, "main.sav", buf, sizeof(buf)), 0);
    CHECK_STR(buf, "local progress");

    fixture_close(&f);
}

/* And the titles that are not in conflict still sync, which is what makes the run
 * worth doing at all: last night's save is on the server before anybody picks the
 * console up. */
TEST_CASE(a_deferring_policy_still_uploads_what_only_this_console_changed)
{
    fixture_t f;
    const daemoon_title_t *t;
    daemoon_sync_stats_t stats;
    daemoon_sync_opts_t opts = { DAEMOON_CONFLICT_POLICY_DEFER, 1, 1 };

    CHECK_EQ_INT(fixture_open(&f, "defer-upload"), 0);
    t = fixture_add_title(&f, "0004000000055D00", DAEMOON_PLATFORM_3DS);
    (void)fixture_write_save_file(&f, t, "main.sav", "last night");

    f.ui.confirm_answer = 0; /* nothing may be asked, so nothing may depend on it */
    memset(&stats, 0, sizeof(stats));
    CHECK_OK(daemoon_sync_title_with(&f.env, &f.actx, t, &opts, &stats));

    CHECK_EQ_INT(f.ui.confirms, 0);
    CHECK_EQ_INT(stats.uploaded, 1);
    CHECK_EQ_INT(stats.conflicts, 0);

    fixture_close(&f);
}

/* The plain entry point is the asking one. A caller that has not thought about
 * policies gets the behaviour the rules describe. */
TEST_CASE(sync_title_is_the_asking_policy)
{
    fixture_t f;
    const daemoon_title_t *t;
    daemoon_sync_stats_t stats;

    CHECK_EQ_INT(fixture_open(&f, "policy-default"), 0);
    t = fixture_add_title(&f, "0004000000055D00", DAEMOON_PLATFORM_3DS);
    make_conflict(&f, t);

    f.ui.choose_answer = DAEMOON_CONFLICT_DEFER;
    memset(&stats, 0, sizeof(stats));
    CHECK_OK(daemoon_sync_title(&f.env, &f.actx, t, &stats));
    CHECK_EQ_INT(f.ui.chooses, 1);
    CHECK_EQ_INT(stats.conflicts, 1);

    fixture_close(&f);
}

TEST_CASE(conflict_deferring_touches_nothing)
{
    fixture_t f;
    const daemoon_title_t *t;
    daemoon_sync_stats_t stats;
    char buf[256];

    CHECK_EQ_INT(fixture_open(&f, "conflict-defer"), 0);
    t = fixture_add_title(&f, "0004000000055D00", DAEMOON_PLATFORM_3DS);
    make_conflict(&f, t);

    f.ui.choose_answer = DAEMOON_CONFLICT_DEFER;
    memset(&stats, 0, sizeof(stats));
    CHECK_OK(daemoon_sync_title(&f.env, &f.actx, t, &stats));

    CHECK_EQ_INT(stats.conflicts, 1);
    CHECK_EQ_INT(f.server.uploads, 0);
    CHECK_EQ_INT(f.server.titles[0].nversions, 2);
    CHECK_EQ_INT(fixture_read_save_file(&f, t, "main.sav", buf, sizeof(buf)), 0);
    CHECK_STR(buf, "local progress");

    fixture_close(&f);
}

TEST_CASE(cancelling_the_conflict_dialog_is_the_same_as_deferring)
{
    fixture_t f;
    const daemoon_title_t *t;
    daemoon_sync_stats_t stats;

    CHECK_EQ_INT(fixture_open(&f, "conflict-cancel"), 0);
    t = fixture_add_title(&f, "0004000000055D00", DAEMOON_PLATFORM_3DS);
    make_conflict(&f, t);

    f.ui.choose_answer = -1;
    memset(&stats, 0, sizeof(stats));
    CHECK_OK(daemoon_sync_title(&f.env, &f.actx, t, &stats));
    CHECK_EQ_INT(stats.conflicts, 1);
    CHECK_EQ_INT(f.server.uploads, 0);

    fixture_close(&f);
}

TEST_CASE(a_race_at_upload_time_becomes_a_conflict_not_an_overwrite)
{
    fixture_t f;
    const daemoon_title_t *t;
    daemoon_sync_stats_t stats;
    unsigned char *blob = NULL;
    size_t blob_len = 0;

    CHECK_EQ_INT(fixture_open(&f, "race"), 0);
    t = fixture_add_title(&f, "0004000000055D00", DAEMOON_PLATFORM_3DS);

    /* The server already has a version this console has never seen, and the local
     * state file says it is up to date at version 1. That is what the window
     * between the metadata read and the upload looks like. */
    CHECK_EQ_INT(fixture_write_save_file(&f, t, "main.sav", "elsewhere"), 0);
    CHECK_OK(fixture_pack_blob(&f, t, DAEMOON_VERSION_NONE, &blob, &blob_len));
    CHECK_OK(fake_server_put(&f.server, DAEMOON_PLATFORM_3DS, t->id, blob, blob_len, "other"));
    free(blob);
    CHECK_OK(fixture_pack_blob(&f, t, 1, &blob, &blob_len));
    CHECK_OK(fake_server_put(&f.server, DAEMOON_PLATFORM_3DS, t->id, blob, blob_len, "other"));
    free(blob);

    CHECK_EQ_INT(fixture_write_save_file(&f, t, "main.sav", "local progress"), 0);
    {
        daemoon_local_state_t st;
        set_local(&st, 1, SHA_A, 1, SHA_B);
        CHECK_OK(daemoon_sync_state_save(&f.env, DAEMOON_PLATFORM_3DS, t->id, &st));
    }

    f.ui.choose_answer = DAEMOON_CONFLICT_DEFER;
    memset(&stats, 0, sizeof(stats));
    CHECK_OK(daemoon_sync_title(&f.env, &f.actx, t, &stats));

    CHECK_EQ_INT(stats.conflicts, 1);
    CHECK_EQ_INT(f.server.uploads, 0);

    fixture_close(&f);
}

TEST_CASE(env_validation_rejects_a_half_wired_environment)
{
    fixture_t f;
    daemoon_env_t broken;

    CHECK_EQ_INT(fixture_open(&f, "envcheck"), 0);
    CHECK_OK(daemoon_env_validate(&f.env));

    /* Without confirm there is no way to honour "destructive actions always ask",
     * and there is no force path to fall back to. */
    broken = f.env;
    broken.ui = NULL;
    CHECK_RESULT(daemoon_env_validate(&broken), DAEMOON_ERR_INVALID_REQUEST);

    broken = f.env;
    broken.scratch = NULL;
    CHECK_RESULT(daemoon_env_validate(&broken), DAEMOON_ERR_INVALID_REQUEST);

    broken = f.env;
    broken.work_dir = "";
    CHECK_RESULT(daemoon_env_validate(&broken), DAEMOON_ERR_INVALID_REQUEST);

    fixture_close(&f);
}

/* The empty save guard has to cover the wire, not just the SD card.
 *
 * daemoon_sync_backup_local has refused an empty archive since Phase 1, when a
 * real title produced a package that was a manifest and no payload. The upload
 * path did not - and that is the half where the damage leaves the device: an
 * empty read becomes a new server version carrying the digest of nothing, and
 * every other console then downloads that over its own save.
 *
 * Found against a real server, on a title that already had a good version. */
TEST_CASE(an_empty_save_is_never_uploaded)
{
    fixture_t f;
    const daemoon_title_t *t;
    daemoon_sync_stats_t stats;
    const fake_version_t *v;

    CHECK_EQ_INT(fixture_open(&f, "empty-upload"), 0);
    t = fixture_add_title(&f, "0004000000055D00", DAEMOON_PLATFORM_3DS);

    /* A good version on the server first. This is not about a first upload; it is
     * about an empty one landing on top of something worth keeping. */
    CHECK_EQ_INT(fixture_write_save_file(&f, t, "main.sav", "the real save"), 0);
    memset(&stats, 0, sizeof(stats));
    CHECK_OK(daemoon_sync_title(&f.env, &f.actx, t, &stats));
    CHECK_EQ_INT(stats.uploaded, 1);

    /* Now the archive reads as empty. A failed enumeration is indistinguishable
     * from this here, which is the whole reason both are refused. */
    CHECK_EQ_INT(fixture_remove_save_file(&f, t, "main.sav"), 0);

    memset(&stats, 0, sizeof(stats));
    CHECK_RESULT(daemoon_sync_title(&f.env, &f.actx, t, &stats), DAEMOON_ERR_EMPTY_SAVE);
    CHECK_EQ_INT(stats.uploaded, 0);
    CHECK_EQ_INT(f.server.uploads, 1);

    /* And the server still has exactly what it had. */
    v = fake_server_latest(&f.server, DAEMOON_PLATFORM_3DS, "0004000000055D00");
    CHECK(v != NULL);
    CHECK_EQ_INT(v->version, 1);
    CHECK(v->size > 0);

    fixture_close(&f);
}

/* The console knows what a game is called and the server has no other way to find
 * out. A page listing 0004000000055D00 beside AZLK_GIRLSMODE is a page nobody can
 * read, and the name was missing for one reason: nothing sent it.
 *
 * Optional on the way in as well - a package written before this existed has no
 * name, and that is a package to read rather than one to refuse. */
TEST_CASE(a_package_carries_the_name_the_console_shows)
{
    fixture_t f;
    const daemoon_title_t *t;
    daemoon_sync_stats_t stats;
    const fake_version_t *v;
    daemoon_manifest_t m;
    char json[DAEMOON_MANIFEST_MAX_BYTES];
    size_t len = 0;

    CHECK_EQ_INT(fixture_open(&f, "title-name"), 0);
    t = fixture_add_title(&f, "0004000000055D00", DAEMOON_PLATFORM_3DS);
    CHECK_EQ_INT(fixture_write_save_file(&f, t, "main.sav", "player data"), 0);

    memset(&stats, 0, sizeof(stats));
    CHECK_OK(daemoon_sync_title(&f.env, &f.actx, t, &stats));
    CHECK_EQ_INT(stats.uploaded, 1);

    v = fake_server_latest(&f.server, DAEMOON_PLATFORM_3DS, "0004000000055D00");
    CHECK(v != NULL);

    /* The name the backend gave the title is what the manifest carries. */
    daemoon_manifest_init(&m);
    m.platform = DAEMOON_PLATFORM_3DS;
    m.save_type = DAEMOON_SAVE_SAVEDATA;
    CHECK_OK(daemoon_strlcpy(m.title_id, sizeof(m.title_id), "0004000000055D00"));
    CHECK_OK(daemoon_strlcpy(m.sha256, sizeof(m.sha256),
                             "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_OK(daemoon_strlcpy(m.device_label, sizeof(m.device_label), "test console"));
    CHECK_OK(daemoon_strlcpy(m.created_at, sizeof(m.created_at),
                             "2026-01-01T00:00:00Z"));
    CHECK_OK(daemoon_strlcpy(m.title_name, sizeof(m.title_name), "포켓몬스터 Y"));

    CHECK_OK(daemoon_manifest_write(&m, json, sizeof(json), &len));
    CHECK(strstr(json, "\"title_name\":\"포켓몬스터 Y\"") != NULL);

    {
        daemoon_manifest_t back;

        CHECK_OK(daemoon_manifest_parse(json, len, &back));
        CHECK_STR(back.title_name, "포켓몬스터 Y");
    }

    /* And a manifest from before the field existed still parses, with no name. */
    {
        static const char old[] =
            "{\"format_version\":1,\"platform\":\"3ds\","
            "\"title_id\":\"0004000000055D00\",\"save_type\":\"savedata\","
            "\"version\":0,\"parent_version\":null,"
            "\"sha256\":\"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\","
            "\"size\":0,\"device_label\":\"3DS\","
            "\"created_at\":\"1970-01-01T00:00:00Z\"}";
        daemoon_manifest_t back;

        CHECK_OK(daemoon_manifest_parse(old, strlen(old), &back));
        CHECK_STR(back.title_name, "");
    }

    /* A name with no room is a name not sent, not a backup refused. */
    {
        daemoon_manifest_t big = m;
        char json2[DAEMOON_MANIFEST_MAX_BYTES];

        memset(big.title_name, 'A', sizeof(big.title_name) - 1);
        big.title_name[sizeof(big.title_name) - 1] = '\0';
        CHECK_OK(daemoon_manifest_write(&big, json2, sizeof(json2), &len));
    }

    fixture_close(&f);
}

void test_sync(void)
{
    printf("sync\n");
    RUN(an_empty_save_is_never_uploaded);
    RUN(a_package_carries_the_name_the_console_shows);
    RUN(decide_covers_every_case);
    RUN(decide_never_consults_a_clock);
    RUN(dirty_tracking);
    RUN(state_survives_a_round_trip);
    RUN(first_upload);
    RUN(declining_the_upload_changes_nothing);
    RUN(download_backs_up_verifies_and_commits);
    RUN(a_corrupt_download_never_reaches_the_save);
    RUN(a_failed_commit_is_a_failed_restore);
    RUN(a_backup_carries_the_value_its_save_was_bound_to);
    RUN(a_title_with_no_value_produces_a_package_that_writes_none);
    RUN(a_backend_that_cannot_read_the_value_still_backs_the_save_up);
    RUN(a_package_without_a_value_clears_the_consoles_rather_than_leaving_a_mismatch);
    RUN(a_platform_without_secure_values_is_left_alone_entirely);
    RUN(conflict_keeping_local_uploads_without_discarding_the_server_copy);
    RUN(conflict_keeping_the_server_copy_backs_up_first);
    RUN(conflict_deferring_touches_nothing);
    RUN(a_policy_of_keeping_local_never_asks_and_keeps_every_server_version);
    RUN(a_policy_of_keeping_the_server_still_backs_the_console_up_first);
    RUN(a_policy_alone_does_not_skip_the_confirmation_before_a_restore);
    RUN(an_answered_upload_question_is_not_asked_again_per_title);
    RUN(the_upload_question_is_asked_when_it_has_not_been_answered);
    RUN(an_answered_restore_question_is_not_asked_again_but_the_backup_still_happens);
    RUN(the_published_restore_has_no_way_to_skip_its_question);
    RUN(a_deferring_policy_leaves_both_sides_untouched_and_asks_nothing);
    RUN(a_deferring_policy_still_uploads_what_only_this_console_changed);
    RUN(sync_title_is_the_asking_policy);
    RUN(cancelling_the_conflict_dialog_is_the_same_as_deferring);
    RUN(a_race_at_upload_time_becomes_a_conflict_not_an_overwrite);
    RUN(env_validation_rejects_a_half_wired_environment);
}
