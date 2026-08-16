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

void test_sync(void)
{
    printf("sync\n");
    RUN(an_empty_save_is_never_uploaded);
    RUN(decide_covers_every_case);
    RUN(decide_never_consults_a_clock);
    RUN(dirty_tracking);
    RUN(state_survives_a_round_trip);
    RUN(first_upload);
    RUN(declining_the_upload_changes_nothing);
    RUN(download_backs_up_verifies_and_commits);
    RUN(a_corrupt_download_never_reaches_the_save);
    RUN(a_failed_commit_is_a_failed_restore);
    RUN(conflict_keeping_local_uploads_without_discarding_the_server_copy);
    RUN(conflict_keeping_the_server_copy_backs_up_first);
    RUN(conflict_deferring_touches_nothing);
    RUN(cancelling_the_conflict_dialog_is_the_same_as_deferring);
    RUN(a_race_at_upload_time_becomes_a_conflict_not_an_overwrite);
    RUN(env_validation_rejects_a_half_wired_environment);
}
