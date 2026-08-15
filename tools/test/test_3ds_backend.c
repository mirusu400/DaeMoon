/* The 3DS save backend, run on a desktop against a libctru shaped stub.
 *
 * This does not prove the backend works on a console: only hardware can say
 * whether the FS service behaves the way the stub does, and that is what
 * docs/phase1-hardware.md is for.
 *
 * What it does prove is that the code in platform/3ds/source is not obviously
 * wrong, which until now was an untested claim about the one component that can
 * lose someone's save. It runs the same conformance suite the console build runs,
 * under the address and undefined behaviour sanitizers, so a bad pointer here is a
 * readable report rather than a console that hangs.
 *
 * It has already earned its place: the first run of the truncation cases is what
 * turned a silently truncated path - which opens, writes to, or deletes a
 * different file - into an error.
 */
#define _POSIX_C_SOURCE 200809L

#include "test.h"

#include "backend_conformance.h"
#include "ctru_stub/3ds.h"
#include "daemoon_posix.h"

#include "../../platform/3ds/source/daemoon_3ds.h"

#include <daemoon/archive.h>
#include <daemoon/sync.h>
#include <daemoon/util/strbuf.h>

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define TEST_TITLE_ID    0x0004000000055D00ull
#define TEST_TITLE_OTHER 0x0004000000030800ull

static void title_for(daemoon_title_t *t, unsigned long long id)
{
    memset(t, 0, sizeof(*t));
    daemoon_3ds_format_title_id(id, t->id, sizeof(t->id));
    (void)daemoon_strlcpy(t->name, sizeof(t->name), "Dummy");
    t->platform = DAEMOON_PLATFORM_3DS;
    t->save_type = DAEMOON_SAVE_SAVEDATA;
    t->size_hint = (unsigned long long)MEDIATYPE_SD;
    t->has_save = 1;
}

/* The stub only reports an archive that exists, matching a console where a title
 * that has never saved has nothing to open. */
static int make_archive(const char *root, unsigned long long id)
{
    char path[512];

    (void)snprintf(path, sizeof(path), "%s/%016llX", root, id);
    return mkdir(path, 0755) == 0 ? 0 : -1;
}

TEST_CASE(title_ids_round_trip)
{
    char text[DAEMOON_TITLE_ID_MAX];
    unsigned long long back = 0;

    /* The spelling the manifest, the server and the desktop client all use. A
     * mismatch here means a package written on a console opens as a different
     * title everywhere else. */
    daemoon_3ds_format_title_id(TEST_TITLE_ID, text, sizeof(text));
    CHECK_STR(text, "0004000000055D00");
    CHECK_OK(daemoon_3ds_parse_title_id(text, &back));
    CHECK(back == TEST_TITLE_ID);

    /* Lowercase is accepted on the way in, so a package from an older build still
     * opens, and never produced on the way out. */
    CHECK_OK(daemoon_3ds_parse_title_id("0004000000055d00", &back));
    CHECK(back == TEST_TITLE_ID);

    CHECK_RESULT(daemoon_3ds_parse_title_id("0004", &back), DAEMOON_ERR_INVALID_REQUEST);
    CHECK_RESULT(daemoon_3ds_parse_title_id("0004000000055D0G", &back),
                 DAEMOON_ERR_INVALID_REQUEST);
    CHECK_RESULT(daemoon_3ds_parse_title_id("00040000000055D00", &back),
                 DAEMOON_ERR_INVALID_REQUEST);
}

TEST_CASE(the_3ds_backend_conforms)
{
    char root[256];
    daemoon_title_t title;
    daemoon_title_t other;
    daemoon_3ds_save_ctx_t ctx;
    daemoon_backend_under_test_t ut;
    unsigned char scratch[4096];

    CHECK_EQ_INT(daemoon_test_tempdir(root, sizeof(root), "3ds-conformance"), 0);
    daemoon_stub_init(root);
    daemoon_stub_add_title(TEST_TITLE_ID, "CTR-P-DUMY");
    daemoon_stub_add_title(TEST_TITLE_OTHER, "CTR-P-DUM2");
    CHECK_EQ_INT(make_archive(root, TEST_TITLE_ID), 0);
    CHECK_EQ_INT(make_archive(root, TEST_TITLE_OTHER), 0);

    title_for(&title, TEST_TITLE_ID);
    title_for(&other, TEST_TITLE_OTHER);

    memset(&ctx, 0, sizeof(ctx));
    ctx.media = MEDIATYPE_SD;

    memset(&ut, 0, sizeof(ut));
    ut.name = "3ds (against the libctru stub)";
    ut.backend = &daemoon_3ds_save_backend;
    ut.ctx = &ctx;
    ut.title = &title;
    ut.other = &other;
    ut.scratch = scratch;
    ut.scratch_len = sizeof(scratch);

    daemoon_backend_conformance(&ut);

    /* The suite commits repeatedly. Every one of those has to have reached the
     * service, because a commit that never happened is a save that never existed. */
    CHECK(daemoon_stub_commits() > 0);
    /* And nothing may be left open. On a console a leaked handle is a service slot
     * that never comes back. */
    CHECK_EQ_INT(daemoon_stub_open_handles(), 0);

    daemoon_stub_reset();
    (void)daemoon_posix_rmtree(root);
}

TEST_CASE(a_failed_commit_is_reported)
{
    char root[256];
    daemoon_title_t title;
    daemoon_3ds_save_ctx_t ctx;
    daemoon_save_t *save = NULL;
    daemoon_stream_t *f = NULL;

    CHECK_EQ_INT(daemoon_test_tempdir(root, sizeof(root), "3ds-commitfail"), 0);
    daemoon_stub_init(root);
    daemoon_stub_add_title(TEST_TITLE_ID, "CTR-P-DUMY");
    CHECK_EQ_INT(make_archive(root, TEST_TITLE_ID), 0);
    title_for(&title, TEST_TITLE_ID);

    memset(&ctx, 0, sizeof(ctx));
    ctx.media = MEDIATYPE_SD;

    CHECK_OK(daemoon_3ds_save_backend.open_save_write(&ctx, &title, &save));
    CHECK_OK(daemoon_3ds_save_backend.open_file(&ctx, save, "main.sav",
                                                DAEMOON_OPEN_WRITE, &f));
    CHECK_OK(daemoon_stream_write(f, "data", 4));
    CHECK_OK(daemoon_stream_close(f));

    /* The one failure that must never be swallowed: without a commit the write is
     * gone at power off, and the user finds out the next time they launch. */
    daemoon_stub_fail_next_commit();
    CHECK(daemoon_3ds_save_backend.commit(&ctx, save) != DAEMOON_OK);

    CHECK_OK(daemoon_3ds_save_backend.close_save(&ctx, save));
    daemoon_stub_reset();
    (void)daemoon_posix_rmtree(root);
}

TEST_CASE(a_path_too_long_is_refused_rather_than_truncated)
{
    /* libctru fills the buffer and returns the length the input would have needed,
     * so a caller that ignores the difference gets a shorter path and operates on
     * a different file. Deleting the wrong file inside a save archive is exactly
     * the class of bug this project cannot ship. */
    char root[256];
    char long_name[DAEMOON_PATH_MAX + 64];
    daemoon_title_t title;
    daemoon_3ds_save_ctx_t ctx;
    daemoon_save_t *save = NULL;
    daemoon_stream_t *f = NULL;
    size_t i;

    CHECK_EQ_INT(daemoon_test_tempdir(root, sizeof(root), "3ds-longpath"), 0);
    daemoon_stub_init(root);
    daemoon_stub_add_title(TEST_TITLE_ID, "CTR-P-DUMY");
    CHECK_EQ_INT(make_archive(root, TEST_TITLE_ID), 0);
    title_for(&title, TEST_TITLE_ID);

    memset(&ctx, 0, sizeof(ctx));
    ctx.media = MEDIATYPE_SD;

    for (i = 0; i < sizeof(long_name) - 1; ++i) {
        long_name[i] = 'a';
    }
    long_name[sizeof(long_name) - 1] = '\0';

    CHECK_OK(daemoon_3ds_save_backend.open_save_write(&ctx, &title, &save));
    CHECK(daemoon_3ds_save_backend.open_file(&ctx, save, long_name,
                                             DAEMOON_OPEN_WRITE, &f) != DAEMOON_OK);
    CHECK_OK(daemoon_3ds_save_backend.close_save(&ctx, save));

    daemoon_stub_reset();
    (void)daemoon_posix_rmtree(root);
}

static int count_cb(void *user, const char *path, unsigned long long size)
{
    (void)path;
    (void)size;
    ++(*(size_t *)user);
    return 0;
}

TEST_CASE(remove_all_clears_a_nested_tree)
{
    /* Deleting during the walk is what this used to do, and on FAT removing an
     * entry from a directory that has an open handle can skip the next one. A file
     * left behind after remove_all is a file the game sees mixed into the save
     * that was just restored. */
    char root[256];
    daemoon_title_t title;
    daemoon_3ds_save_ctx_t ctx;
    daemoon_save_t *save = NULL;
    size_t remaining = 0;
    int i;

    CHECK_EQ_INT(daemoon_test_tempdir(root, sizeof(root), "3ds-removeall"), 0);
    daemoon_stub_init(root);
    daemoon_stub_add_title(TEST_TITLE_ID, "CTR-P-DUMY");
    CHECK_EQ_INT(make_archive(root, TEST_TITLE_ID), 0);
    title_for(&title, TEST_TITLE_ID);

    memset(&ctx, 0, sizeof(ctx));
    ctx.media = MEDIATYPE_SD;

    CHECK_OK(daemoon_3ds_save_backend.open_save_write(&ctx, &title, &save));

    /* Enough files that a skipped entry shows up, and deep enough that the
     * recursion is exercised. */
    for (i = 0; i < 12; ++i) {
        char path[64];
        daemoon_stream_t *f = NULL;

        (void)snprintf(path, sizeof(path), "dir%d/sub/file%d.bin", i % 3, i);
        CHECK_OK(daemoon_3ds_save_backend.open_file(&ctx, save, path,
                                                    DAEMOON_OPEN_WRITE, &f));
        CHECK_OK(daemoon_stream_write(f, "x", 1));
        CHECK_OK(daemoon_stream_close(f));
    }
    CHECK_OK(daemoon_3ds_save_backend.commit(&ctx, save));

    CHECK_OK(daemoon_3ds_save_backend.remove_all(&ctx, save));

    /* Counting what is left through the same interface core would use. */
    remaining = 0;
    CHECK_OK(daemoon_3ds_save_backend.list_entries(&ctx, save, count_cb, &remaining));
    CHECK_EQ_INT(remaining, 0);

    CHECK_OK(daemoon_3ds_save_backend.close_save(&ctx, save));
    daemoon_stub_reset();
    (void)daemoon_posix_rmtree(root);
}

TEST_CASE(title_enumeration_reports_what_core_needs)
{
    char root[256];
    daemoon_3ds_save_ctx_t ctx;
    daemoon_title_t *titles = NULL;
    size_t count = 0;
    size_t i;
    int found = 0;

    CHECK_EQ_INT(daemoon_test_tempdir(root, sizeof(root), "3ds-titles"), 0);
    daemoon_stub_init(root);
    daemoon_stub_add_title(TEST_TITLE_ID, "CTR-P-DUMY");
    /* A system title, which must not be listed: reaching into one is how a console
     * stops booting. */
    daemoon_stub_add_title(0x0004001000021000ull, "CTR-S-SYS");
    CHECK_EQ_INT(make_archive(root, TEST_TITLE_ID), 0);

    memset(&ctx, 0, sizeof(ctx));
    ctx.media = MEDIATYPE_SD;
    ctx.only_with_saves = 1;

    CHECK_OK(daemoon_3ds_save_backend.list_titles(&ctx, &titles, &count));
    for (i = 0; i < count; ++i) {
        CHECK_EQ_INT(titles[i].platform, DAEMOON_PLATFORM_3DS);
        CHECK_EQ_INT(titles[i].save_type, DAEMOON_SAVE_SAVEDATA);
        CHECK_EQ_INT(strlen(titles[i].id), 16);
        /* Until hardware says which titles bind their save to the console, all of
         * them are treated as if they might, so the warning fires before every
         * restore. */
        CHECK(titles[i].secure_value);
        if (strcmp(titles[i].id, "0004000000055D00") == 0) {
            found = 1;
            CHECK_STR(titles[i].name, "CTR-P-DUMY");
            CHECK(titles[i].has_save);
        }
        CHECK(strcmp(titles[i].id, "0004001000021000") != 0);
    }
    CHECK(found);

    daemoon_3ds_save_backend.free_titles(&ctx, titles, count);
    daemoon_stub_reset();
    (void)daemoon_posix_rmtree(root);
}

TEST_CASE(the_secure_value_round_trips)
{
    /* Phase 1 reads the value before a restore and puts it back after. Whether
     * that is sufficient is the hardware question; that the code does what it says
     * is this one. */
    char root[256];
    daemoon_title_t title;
    daemoon_3ds_secure_value_t before;
    daemoon_3ds_secure_value_t after;

    CHECK_EQ_INT(daemoon_test_tempdir(root, sizeof(root), "3ds-secure"), 0);
    daemoon_stub_init(root);
    daemoon_stub_add_title(TEST_TITLE_ID, "CTR-P-DUMY");
    CHECK_EQ_INT(make_archive(root, TEST_TITLE_ID), 0);
    title_for(&title, TEST_TITLE_ID);

    CHECK_OK(daemoon_3ds_read_secure_value(&title, &before));
    CHECK_EQ_INT(before.exists, 0);

    before.exists = 1;
    before.value = 0x0123456789ABCDEFull;
    CHECK_OK(daemoon_3ds_write_secure_value(&title, &before));

    CHECK_OK(daemoon_3ds_read_secure_value(&title, &after));
    CHECK_EQ_INT(after.exists, 1);
    CHECK(after.value == before.value);

    /* Nothing recorded means nothing to put back. Inventing a value would be worse
     * than leaving whatever the console has. */
    after.exists = 0;
    CHECK_OK(daemoon_3ds_write_secure_value(&title, &after));

    daemoon_stub_reset();
    (void)daemoon_posix_rmtree(root);
}

/* ------------------------------------------------------- the Phase 1 feature */

/* Backup and restore, driven through core, with the 3DS backends underneath.
 *
 * This is what Phase 1 is: no server, no network, a save copied to the SD card and
 * put back. Running it here means the ordering rules - back up first, verify
 * before writing, commit and check the result - are exercised against the console
 * code paths rather than only against the desktop ones. */
TEST_CASE(a_backup_and_restore_round_trip_through_the_3ds_backends)
{
    char root[256];
    char work[320];
    char backup[512];
    daemoon_strbuf_t sb;
    daemoon_title_t title;
    daemoon_3ds_save_ctx_t save_ctx;
    daemoon_posix_ui_ctx_t ui;
    daemoon_archive_ctx_t actx;
    daemoon_env_t env;
    daemoon_save_t *save = NULL;
    daemoon_stream_t *f = NULL;
    static unsigned char scratch[64 * 1024];
    char buf[64];
    size_t got = 0;

    CHECK_EQ_INT(daemoon_test_tempdir(root, sizeof(root), "3ds-phase1"), 0);
    daemoon_stub_init(root);
    daemoon_stub_add_title(TEST_TITLE_ID, "CTR-P-DUMY");
    CHECK_EQ_INT(make_archive(root, TEST_TITLE_ID), 0);
    title_for(&title, TEST_TITLE_ID);

    daemoon_strbuf_init(&sb, work, sizeof(work));
    daemoon_strbuf_add(&sb, root);
    daemoon_strbuf_add(&sb, "/DaeMoon");
    CHECK_OK(daemoon_strbuf_result(&sb));

    memset(&save_ctx, 0, sizeof(save_ctx));
    save_ctx.media = MEDIATYPE_SD;
    daemoon_posix_ui_init(&ui);
    actx.count = 0;

    memset(&env, 0, sizeof(env));
    env.save = &daemoon_3ds_save_backend;
    env.fs = &daemoon_3ds_fs_backend;   /* the SD card side, also console code */
    env.ui = &daemoon_posix_ui_backend; /* a console UI needs a console */
    env.save_ctx = &save_ctx;
    env.ui_ctx = &ui;
    env.device_label = "3DS";
    env.work_dir = work;
    env.scratch = scratch;
    env.scratch_len = sizeof(scratch);
    CHECK_OK(daemoon_env_validate(&env));

    /* A save worth losing. */
    CHECK_OK(env.save->open_save_write(env.save_ctx, &title, &save));
    CHECK_OK(env.save->remove_all(env.save_ctx, save));
    CHECK_OK(env.save->open_file(env.save_ctx, save, "main.sav", DAEMOON_OPEN_WRITE, &f));
    CHECK_OK(daemoon_stream_write(f, "the original save", 17));
    CHECK_OK(daemoon_stream_close(f));
    CHECK_OK(env.save->open_file(env.save_ctx, save, "sub/extra.bin",
                                 DAEMOON_OPEN_WRITE, &f));
    CHECK_OK(daemoon_stream_write(f, "nested", 6));
    CHECK_OK(daemoon_stream_close(f));
    CHECK_OK(env.save->commit(env.save_ctx, save));
    CHECK_OK(env.save->close_save(env.save_ctx, save));

    CHECK_OK(daemoon_sync_backup_local(&env, &actx, &title, backup, sizeof(backup)));

    /* Play on. */
    CHECK_OK(env.save->open_save_write(env.save_ctx, &title, &save));
    CHECK_OK(env.save->open_file(env.save_ctx, save, "main.sav", DAEMOON_OPEN_WRITE, &f));
    CHECK_OK(daemoon_stream_write(f, "later progress", 14));
    CHECK_OK(daemoon_stream_close(f));
    /* And something the backup does not contain, which must not survive. */
    CHECK_OK(env.save->open_file(env.save_ctx, save, "stray.bin", DAEMOON_OPEN_WRITE, &f));
    CHECK_OK(daemoon_stream_write(f, "x", 1));
    CHECK_OK(daemoon_stream_close(f));
    CHECK_OK(env.save->commit(env.save_ctx, save));
    CHECK_OK(env.save->close_save(env.save_ctx, save));

    {
        unsigned commits_before = daemoon_stub_commits();

        CHECK_OK(daemoon_sync_restore_package(&env, &actx, &title, backup));
        /* The restore committed. Without that the archive is unchanged at power
         * off and the user finds out the next time they launch the game. */
        CHECK(daemoon_stub_commits() > commits_before);
    }
    /* And it asked before overwriting. */
    CHECK(ui.confirms > 0);

    CHECK_OK(env.save->open_save(env.save_ctx, &title, &save));
    CHECK_OK(env.save->open_file(env.save_ctx, save, "main.sav", DAEMOON_OPEN_READ, &f));
    CHECK_OK(daemoon_stream_read(f, buf, sizeof(buf) - 1, &got));
    CHECK_OK(daemoon_stream_close(f));
    buf[got] = '\0';
    CHECK_STR(buf, "the original save");

    CHECK_OK(env.save->open_file(env.save_ctx, save, "sub/extra.bin",
                                 DAEMOON_OPEN_READ, &f));
    CHECK_OK(daemoon_stream_read(f, buf, sizeof(buf) - 1, &got));
    CHECK_OK(daemoon_stream_close(f));
    buf[got] = '\0';
    CHECK_STR(buf, "nested");

    /* The file the backup never had is gone. */
    CHECK_RESULT(env.save->open_file(env.save_ctx, save, "stray.bin",
                                     DAEMOON_OPEN_READ, &f),
                 DAEMOON_ERR_NOT_FOUND);
    CHECK_OK(env.save->close_save(env.save_ctx, save));

    /* Nothing left open on either side. */
    CHECK_EQ_INT(daemoon_stub_open_handles(), 0);

    daemoon_stub_reset();
    (void)daemoon_posix_rmtree(root);
}

/* The SD card side: backups, staging and the state file all go through this, and
 * rule one of the project is that a restore does not happen without a backup. */
TEST_CASE(the_sd_backend_handles_nested_paths_and_replacement)
{
    char root[256];
    char nested[512];
    char other[512];
    daemoon_strbuf_t sb;
    daemoon_stream_t *f = NULL;
    char buf[64];
    size_t got = 0;

    CHECK_EQ_INT(daemoon_test_tempdir(root, sizeof(root), "3ds-sd"), 0);

    daemoon_strbuf_init(&sb, nested, sizeof(nested));
    daemoon_strbuf_add(&sb, root);
    daemoon_strbuf_add(&sb, "/DaeMoon/backups/deep/file.zip");
    CHECK_OK(daemoon_strbuf_result(&sb));

    /* Opening for writing creates the directories the path needs. core never
     * creates one, so if this does not, every backup fails on a fresh card. */
    CHECK_OK(daemoon_3ds_fs_backend.open(NULL, nested, DAEMOON_OPEN_WRITE, &f));
    CHECK_OK(daemoon_stream_write(f, "package", 7));
    CHECK_OK(daemoon_stream_close(f));
    CHECK(daemoon_3ds_fs_backend.exists(NULL, nested));

    CHECK_OK(daemoon_3ds_fs_backend.open(NULL, nested, DAEMOON_OPEN_READ, &f));
    CHECK_OK(daemoon_stream_read(f, buf, sizeof(buf) - 1, &got));
    CHECK_OK(daemoon_stream_close(f));
    buf[got] = '\0';
    CHECK_STR(buf, "package");

    /* Rename replaces. FAT will not overwrite, which is the "write to a temp path
     * then swap" step, and a state file that failed to swap is a wrong base
     * version on the next run. */
    daemoon_strbuf_init(&sb, other, sizeof(other));
    daemoon_strbuf_add(&sb, root);
    daemoon_strbuf_add(&sb, "/DaeMoon/state/x.json");
    CHECK_OK(daemoon_strbuf_result(&sb));

    CHECK_OK(daemoon_3ds_fs_backend.open(NULL, other, DAEMOON_OPEN_WRITE, &f));
    CHECK_OK(daemoon_stream_write(f, "old", 3));
    CHECK_OK(daemoon_stream_close(f));

    CHECK_OK(daemoon_3ds_fs_backend.rename(NULL, nested, other));
    CHECK(!daemoon_3ds_fs_backend.exists(NULL, nested));

    CHECK_OK(daemoon_3ds_fs_backend.open(NULL, other, DAEMOON_OPEN_READ, &f));
    CHECK_OK(daemoon_stream_read(f, buf, sizeof(buf) - 1, &got));
    CHECK_OK(daemoon_stream_close(f));
    buf[got] = '\0';
    CHECK_STR(buf, "package");

    /* Removing something that is not there is not a failure: the cleanup paths
     * call this after a failure that may or may not have created the file. */
    CHECK_OK(daemoon_3ds_fs_backend.remove(NULL, nested));
    CHECK_OK(daemoon_3ds_fs_backend.remove(NULL, other));
    CHECK(!daemoon_3ds_fs_backend.exists(NULL, other));

    /* A missing file reads as not_found, which is how the state file being absent
     * is told apart from the card being unreadable. */
    CHECK_RESULT(daemoon_3ds_fs_backend.open(NULL, other, DAEMOON_OPEN_READ, &f),
                 DAEMOON_ERR_NOT_FOUND);

    (void)daemoon_posix_rmtree(root);
}

/* The bug this catches was found on hardware: a real title produced a backup
 * containing a manifest and no payload, because a failed directory read looked
 * exactly like the end of the directory. Restoring that file clears the archive
 * and writes nothing back, so the backup that looked like it worked is the thing
 * that wipes the save. */
TEST_CASE(an_empty_archive_is_not_backed_up)
{
    char root[256];
    char work[320];
    daemoon_strbuf_t sb;
    daemoon_title_t title;
    daemoon_3ds_save_ctx_t save_ctx;
    daemoon_posix_ui_ctx_t ui;
    daemoon_archive_ctx_t actx;
    daemoon_env_t env;
    static unsigned char scratch[64 * 1024];

    CHECK_EQ_INT(daemoon_test_tempdir(root, sizeof(root), "3ds-empty"), 0);
    daemoon_stub_init(root);
    daemoon_stub_add_title(TEST_TITLE_ID, "CTR-P-DUMY");
    CHECK_EQ_INT(make_archive(root, TEST_TITLE_ID), 0);
    title_for(&title, TEST_TITLE_ID);

    daemoon_strbuf_init(&sb, work, sizeof(work));
    daemoon_strbuf_add(&sb, root);
    daemoon_strbuf_add(&sb, "/DaeMoon");
    CHECK_OK(daemoon_strbuf_result(&sb));

    memset(&save_ctx, 0, sizeof(save_ctx));
    save_ctx.media = MEDIATYPE_SD;
    daemoon_posix_ui_init(&ui);
    actx.count = 0;

    memset(&env, 0, sizeof(env));
    env.save = &daemoon_3ds_save_backend;
    env.fs = &daemoon_3ds_fs_backend;
    env.ui = &daemoon_posix_ui_backend;
    env.save_ctx = &save_ctx;
    env.ui_ctx = &ui;
    env.device_label = "3DS";
    env.work_dir = work;
    env.scratch = scratch;
    env.scratch_len = sizeof(scratch);

    /* The archive exists and has nothing in it. */
    CHECK_RESULT(daemoon_sync_backup_local(&env, &actx, &title, NULL, 0),
                 DAEMOON_ERR_EMPTY_SAVE);

    daemoon_stub_reset();
    (void)daemoon_posix_rmtree(root);
}

/* A name a person recognises, rather than a product code nobody can map to a
 * game. */
TEST_CASE(titles_are_named_from_their_smdh)
{
    char root[256];
    daemoon_3ds_save_ctx_t ctx;
    daemoon_title_t *titles = NULL;
    size_t count = 0;
    size_t i;
    int checked = 0;

    CHECK_EQ_INT(daemoon_test_tempdir(root, sizeof(root), "3ds-smdh"), 0);
    daemoon_stub_init(root);
    daemoon_stub_add_title(TEST_TITLE_ID, "CTR-P-DUMY");
    daemoon_stub_add_title(TEST_TITLE_OTHER, "CTR-P-DUM2");
    CHECK_EQ_INT(make_archive(root, TEST_TITLE_ID), 0);
    CHECK_EQ_INT(make_archive(root, TEST_TITLE_OTHER), 0);

    /* One title has a name in the console's language, the other has none. */
    daemoon_stub_set_title_name(TEST_TITLE_ID, 7, "포켓몬스터");

    memset(&ctx, 0, sizeof(ctx));
    ctx.media = MEDIATYPE_SD;
    ctx.only_with_saves = 1;
    ctx.smdh_language = 7; /* Korean */

    CHECK_OK(daemoon_3ds_save_backend.list_titles(&ctx, &titles, &count));
    for (i = 0; i < count; ++i) {
        if (strcmp(titles[i].id, "0004000000055D00") == 0) {
            CHECK_STR(titles[i].name, "포켓몬스터");
            checked = 1;
        }
        if (strcmp(titles[i].id, "0004000000030800") == 0) {
            /* No SMDH: the product code, which is still better than the id. */
            CHECK_STR(titles[i].name, "CTR-P-DUM2");
        }
    }
    CHECK(checked);

    daemoon_3ds_save_backend.free_titles(&ctx, titles, count);
    daemoon_stub_reset();
    (void)daemoon_posix_rmtree(root);
}

void test_3ds_backend(void)
{
    printf("3ds backend (stubbed libctru)\n");
    RUN(title_ids_round_trip);
    RUN(the_3ds_backend_conforms);
    RUN(a_failed_commit_is_reported);
    RUN(a_path_too_long_is_refused_rather_than_truncated);
    RUN(remove_all_clears_a_nested_tree);
    RUN(title_enumeration_reports_what_core_needs);
    RUN(the_secure_value_round_trips);
    RUN(the_sd_backend_handles_nested_paths_and_replacement);
    RUN(a_backup_and_restore_round_trip_through_the_3ds_backends);
    RUN(an_empty_archive_is_not_backed_up);
    RUN(titles_are_named_from_their_smdh);
}
