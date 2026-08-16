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
#include "posix_internal.h"

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

    /* One title has a name in the console's language. */
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

/* A game sold in one region carries a name for its own languages and blanks for
 * the rest, so reading only the console's language leaves most of a library
 * showing up as a product code. That is what happened on hardware. */
TEST_CASE(a_name_missing_in_the_console_language_falls_back)
{
    char root[256];
    daemoon_3ds_save_ctx_t ctx;
    daemoon_title_t *titles = NULL;
    size_t count = 0;
    size_t i;
    int seen_english = 0;
    int seen_other = 0;

    CHECK_EQ_INT(daemoon_test_tempdir(root, sizeof(root), "3ds-smdh-fallback"), 0);
    daemoon_stub_init(root);
    daemoon_stub_add_title(TEST_TITLE_ID, "CTR-P-DUMY");
    daemoon_stub_add_title(TEST_TITLE_OTHER, "CTR-P-DUM2");
    CHECK_EQ_INT(make_archive(root, TEST_TITLE_ID), 0);
    CHECK_EQ_INT(make_archive(root, TEST_TITLE_OTHER), 0);

    /* Nothing in Korean. One has English, the other only Japanese. */
    daemoon_stub_set_title_name(TEST_TITLE_ID, 1, "Some Game");
    daemoon_stub_set_title_name(TEST_TITLE_OTHER, 0, "ゲーム");

    memset(&ctx, 0, sizeof(ctx));
    ctx.media = MEDIATYPE_SD;
    ctx.only_with_saves = 1;
    ctx.smdh_language = 7; /* Korean, and neither title has one */

    CHECK_OK(daemoon_3ds_save_backend.list_titles(&ctx, &titles, &count));
    for (i = 0; i < count; ++i) {
        if (strcmp(titles[i].id, "0004000000055D00") == 0) {
            CHECK_STR(titles[i].name, "Some Game");
            seen_english = 1;
        }
        if (strcmp(titles[i].id, "0004000000030800") == 0) {
            /* Not English either: whatever the game does have beats a code. */
            CHECK_STR(titles[i].name, "ゲーム");
            seen_other = 1;
        }
    }
    CHECK(seen_english);
    CHECK(seen_other);

    daemoon_3ds_save_backend.free_titles(&ctx, titles, count);
    daemoon_stub_reset();
    (void)daemoon_posix_rmtree(root);
}

/* The console draws the list with an 8x8 ASCII font. A name it cannot draw is a
 * blank line, which tells the user nothing about the save they are about to
 * overwrite - so the list prefers one it can draw, and the survey file keeps the
 * real one for reading somewhere with fonts. */
TEST_CASE(the_list_prefers_a_name_the_console_can_draw)
{
    char root[256];
    char name[DAEMOON_NAME_MAX];
    daemoon_3ds_save_ctx_t ctx;
    daemoon_title_t *titles = NULL;
    size_t count = 0;
    size_t i;
    int checked = 0;

    CHECK_EQ_INT(daemoon_test_tempdir(root, sizeof(root), "3ds-ascii"), 0);
    daemoon_stub_init(root);
    daemoon_stub_add_title(TEST_TITLE_ID, "CTR-P-DUMY");
    CHECK_EQ_INT(make_archive(root, TEST_TITLE_ID), 0);

    /* Korean console, and the game has both a Korean and an English name. */
    daemoon_stub_set_title_name(TEST_TITLE_ID, 7, "요괴워치");
    daemoon_stub_set_title_name(TEST_TITLE_ID, 1, "Yo-kai Watch");

    memset(&ctx, 0, sizeof(ctx));
    ctx.media = MEDIATYPE_SD;
    ctx.only_with_saves = 1;
    ctx.smdh_language = 7;
    ctx.ascii_names = 1;

    CHECK_OK(daemoon_3ds_save_backend.list_titles(&ctx, &titles, &count));
    for (i = 0; i < count; ++i) {
        if (strcmp(titles[i].id, "0004000000055D00") == 0) {
            CHECK_STR(titles[i].name, "Yo-kai Watch");
            checked = 1;
        }
    }
    CHECK(checked);
    daemoon_3ds_save_backend.free_titles(&ctx, titles, count);

    /* Without the flag the console's own language still wins, which is what the
     * survey records and what a Phase 3 font would let the list show. */
    CHECK_OK(daemoon_3ds_title_name((int)MEDIATYPE_SD, TEST_TITLE_ID, 7, 0, name, sizeof(name)));
    CHECK_STR(name, "요괴워치");

    /* A title with only a name it cannot draw falls back to something it can. */
    daemoon_stub_reset();
    daemoon_stub_init(root);
    daemoon_stub_add_title(TEST_TITLE_OTHER, "CTR-P-DUM2");
    CHECK_EQ_INT(make_archive(root, TEST_TITLE_OTHER), 0);
    daemoon_stub_set_title_name(TEST_TITLE_OTHER, 0, "ゲーム");

    ctx.smdh_language = 7;
    CHECK_OK(daemoon_3ds_save_backend.list_titles(&ctx, &titles, &count));
    for (i = 0; i < count; ++i) {
        if (strcmp(titles[i].id, "0004000000030800") == 0) {
            CHECK_STR(titles[i].name, "CTR-P-DUM2");
        }
    }
    daemoon_3ds_save_backend.free_titles(&ctx, titles, count);

    daemoon_stub_reset();
    (void)daemoon_posix_rmtree(root);
}

/* The read that four rounds of hardware testing were spent on.
 *
 * The file an SMDH comes from is decrypted as it is read, so a request that
 * starts anywhere but zero is refused - and the service reports that with the
 * same word it uses for a missing permission. Reading the whole thing from the
 * start is not an optimisation, it is the only thing that works. */
TEST_CASE(the_smdh_is_read_whole_from_the_start)
{
    char root[256];
    static unsigned char smdh[DAEMOON_3DS_SMDH_SIZE];
    char name[DAEMOON_NAME_MAX];
    size_t i;
    int icon_bytes_present = 0;

    CHECK_EQ_INT(daemoon_test_tempdir(root, sizeof(root), "3ds-smdh-read"), 0);
    daemoon_stub_init(root);
    daemoon_stub_add_title(TEST_TITLE_ID, "CTR-P-DUMY");
    CHECK_EQ_INT(make_archive(root, TEST_TITLE_ID), 0);
    daemoon_stub_set_title_name(TEST_TITLE_ID, 1, "Some Game");

    CHECK_OK(daemoon_3ds_smdh_load((int)MEDIATYPE_SD, TEST_TITLE_ID, smdh));

    /* The whole file: the name near the front and the icon near the end both have
     * to be there, because they are read in one pass and used by two callers. */
    CHECK_OK(daemoon_3ds_smdh_name(smdh, 1, 0, name, sizeof(name)));
    CHECK_STR(name, "Some Game");

    for (i = DAEMOON_3DS_SMDH_ICON_OFF; i < DAEMOON_3DS_SMDH_SIZE; ++i) {
        if (smdh[i] != 0) {
            icon_bytes_present = 1;
            break;
        }
    }
    CHECK(icon_bytes_present);

    daemoon_stub_reset();
    (void)daemoon_posix_rmtree(root);
}


/* ------------------------------------------------------------ backup picker */

/* Plain file writing, needed by the picker cases below and by the nds ones
 * further down. */
static void write_file(const char *path, const void *data, size_t len)
{
    FILE *fp = fopen(path, "wb");

    if (fp == NULL) {
        return;
    }
    (void)fwrite(data, 1, len, fp);
    (void)fclose(fp);
}

/* Puts one file into the stub's save archive through the backend, and commits,
 * so the digest afterwards is the one the application would compute. */
static int write_save_file(daemoon_env_t *env, const daemoon_title_t *title,
                           const char *name, const char *text)
{
    daemoon_save_t *save = NULL;
    daemoon_stream_t *f = NULL;

    if (env->save->open_save_write(env->save_ctx, title, &save) != DAEMOON_OK) {
        return -1;
    }
    if (env->save->open_file(env->save_ctx, save, name, DAEMOON_OPEN_WRITE, &f) !=
        DAEMOON_OK) {
        (void)env->save->close_save(env->save_ctx, save);
        return -1;
    }
    (void)daemoon_stream_write(f, text, strlen(text));
    (void)daemoon_stream_close(f);
    (void)env->save->commit(env->save_ctx, save);
    (void)env->save->close_save(env->save_ctx, save);
    return 0;
}

/* What the restore screen lists, without the screen.
 *
 * The first version of this was written straight into the drawing code and sent to
 * a console with no test behind it - which is the order this project exists to
 * avoid, and it cost a hardware round. The part worth testing is not the layout:
 * it is that up to thirty two files, any of which a card reader or a half finished
 * write could have damaged, are read without taking the application down.
 */
static void backup_env(daemoon_env_t *env, daemoon_3ds_save_ctx_t *save_ctx,
                       daemoon_posix_ui_ctx_t *ui, const char *work,
                       unsigned char *scratch, size_t scratch_len)
{
    memset(save_ctx, 0, sizeof(*save_ctx));
    save_ctx->media = MEDIATYPE_SD;
    daemoon_posix_ui_init(ui);

    memset(env, 0, sizeof(*env));
    env->save = &daemoon_3ds_save_backend;
    env->fs = &daemoon_3ds_fs_backend;
    env->ui = &daemoon_posix_ui_backend;
    env->save_ctx = save_ctx;
    env->ui_ctx = ui;
    env->device_label = "3DS";
    env->work_dir = work;
    env->scratch = scratch;
    env->scratch_len = scratch_len;
}

TEST_CASE(the_backup_list_reads_each_package_and_marks_the_current_one)
{
    char root[256];
    char work[320];
    char backups[384];
    char digest[DAEMOON_SHA256_HEX];
    daemoon_strbuf_t sb;
    daemoon_title_t title;
    daemoon_3ds_save_ctx_t save_ctx;
    daemoon_posix_ui_ctx_t ui;
    daemoon_archive_ctx_t actx;
    daemoon_env_t env;
    daemoon_3ds_backup_row_t rows[8];
    daemoon_save_t *save = NULL;
    unsigned long long size = 0;
    static unsigned char scratch[64 * 1024];
    size_t count;

    CHECK_EQ_INT(daemoon_test_tempdir(root, sizeof(root), "3ds-picklist"), 0);
    daemoon_stub_init(root);
    daemoon_stub_add_title(TEST_TITLE_ID, "CTR-P-DUMY");
    CHECK_EQ_INT(make_archive(root, TEST_TITLE_ID), 0);
    title_for(&title, TEST_TITLE_ID);

    daemoon_strbuf_init(&sb, work, sizeof(work));
    daemoon_strbuf_add(&sb, root);
    daemoon_strbuf_add(&sb, "/DaeMoon");
    CHECK_OK(daemoon_strbuf_result(&sb));
    (void)snprintf(backups, sizeof(backups), "%s/backups", work);

    backup_env(&env, &save_ctx, &ui, work, scratch, sizeof(scratch));
    actx.count = 0;

    /* One backup, then the save changes, then a second. Two packages, two
     * digests - which is the case the picker exists for. */
    CHECK_EQ_INT(write_save_file(&env, &title, "slot_1/playerData.dat", "first"), 0);
    CHECK_OK(daemoon_sync_backup_local(&env, &actx, &title, NULL, 0));
    CHECK_EQ_INT(write_save_file(&env, &title, "slot_1/playerData.dat", "second"), 0);
    CHECK_OK(daemoon_sync_backup_local(&env, &actx, &title, NULL, 0));

    /* The digest of what is "on the console" now, exactly as the restore action
     * computes it. */
    CHECK_OK(env.save->open_save(env.save_ctx, &title, &save));
    CHECK_OK(daemoon_archive_hash_save(&env, &actx, save, digest, &size));
    CHECK_OK(env.save->close_save(env.save_ctx, save));

    count = daemoon_3ds_backup_list(&env, backups, &title, digest, rows,
                                    sizeof(rows) / sizeof(rows[0]));
    CHECK_EQ_INT((int)count, 2);
    {
        size_t i;
        int current_rows = 0;

        for (i = 0; i < count; ++i) {
            CHECK(rows[i].readable);
            CHECK(rows[i].size > 0);
            CHECK(rows[i].sha256[0] != '\0');
            CHECK_STR(rows[i].device_label, "3DS");
            if (rows[i].is_current) {
                ++current_rows;
                CHECK_STR(rows[i].sha256, digest);
            }
        }
        /* Exactly one of them is what the console holds. Saying so is the only
         * claim on that screen that does not come from a clock the user can set. */
        CHECK_EQ_INT(current_rows, 1);
    }

    /* Another title's backups are not this title's. */
    {
        daemoon_title_t other;

        title_for(&other, TEST_TITLE_OTHER);
        CHECK_EQ_INT((int)daemoon_3ds_backup_list(&env, backups, &other, digest, rows,
                                                  sizeof(rows) / sizeof(rows[0])),
                     0);
    }

    daemoon_stub_reset();
    (void)daemoon_posix_rmtree(root);
}

TEST_CASE(a_damaged_package_is_a_row_rather_than_a_crash)
{
    char root[256];
    char work[320];
    char backups[384];
    char path[512];
    daemoon_strbuf_t sb;
    daemoon_title_t title;
    daemoon_3ds_save_ctx_t save_ctx;
    daemoon_posix_ui_ctx_t ui;
    daemoon_archive_ctx_t actx;
    daemoon_env_t env;
    daemoon_3ds_backup_row_t rows[8];
    static unsigned char scratch[64 * 1024];
    static unsigned char junk[4096];
    size_t count;
    size_t i;
    int readable = 0;
    int unreadable = 0;

    CHECK_EQ_INT(daemoon_test_tempdir(root, sizeof(root), "3ds-pickbad"), 0);
    daemoon_stub_init(root);
    daemoon_stub_add_title(TEST_TITLE_ID, "CTR-P-DUMY");
    CHECK_EQ_INT(make_archive(root, TEST_TITLE_ID), 0);
    title_for(&title, TEST_TITLE_ID);

    daemoon_strbuf_init(&sb, work, sizeof(work));
    daemoon_strbuf_add(&sb, root);
    daemoon_strbuf_add(&sb, "/DaeMoon");
    CHECK_OK(daemoon_strbuf_result(&sb));
    (void)snprintf(backups, sizeof(backups), "%s/backups", work);

    backup_env(&env, &save_ctx, &ui, work, scratch, sizeof(scratch));
    actx.count = 0;

    CHECK_EQ_INT(write_save_file(&env, &title, "slot_1/playerData.dat", "ok"), 0);
    CHECK_OK(daemoon_sync_backup_local(&env, &actx, &title, NULL, 0));

    /* Everything a directory of backups can actually contain after a card has been
     * pulled, a write has been interrupted, or somebody has copied a file in by
     * hand. Every one of these is opened by the picker. */
    (void)snprintf(path, sizeof(path), "%s/3ds_%s_000000000000.zip", backups, title.id);
    write_file(path, "", 0); /* empty */

    (void)snprintf(path, sizeof(path), "%s/3ds_%s_111111111111.zip", backups, title.id);
    memset(junk, 0x5A, sizeof(junk));
    write_file(path, junk, sizeof(junk)); /* not a zip at all */

    (void)snprintf(path, sizeof(path), "%s/3ds_%s_222222222222.zip", backups, title.id);
    memcpy(junk, "PK\003\004", 4); /* the right first four bytes and nothing else */
    write_file(path, junk, 64);

    (void)snprintf(path, sizeof(path), "%s/3ds_%s_333333333333.zip.tmp", backups,
                   title.id);
    write_file(path, junk, 64); /* a swap that never completed */

    (void)snprintf(path, sizeof(path), "%s/3ds_%s_444444444444.zip", backups, title.id);
    CHECK_EQ_INT(mkdir(path, 0755), 0); /* a directory wearing the name of a package */

    count = daemoon_3ds_backup_list(&env, backups, &title, NULL, rows,
                                    sizeof(rows) / sizeof(rows[0]));
    /* Every one of them is listed. Hiding a file the user cannot then delete would
     * leave the card filling up with something they cannot see. */
    CHECK_EQ_INT((int)count, 6);
    for (i = 0; i < count; ++i) {
        if (rows[i].readable) {
            ++readable;
            CHECK(rows[i].sha256[0] != '\0');
        } else {
            ++unreadable;
            /* Nothing downstream may read these as real values. */
            CHECK_EQ_INT((int)rows[i].size, 0);
            CHECK_STR(rows[i].sha256, "");
            CHECK_EQ_INT(rows[i].is_current, 0);
        }
    }
    CHECK_EQ_INT(readable, 1);
    CHECK_EQ_INT(unreadable, 5);

    /* And the readable one sorts above the wreckage, because that is the one the
     * user came here for. */
    CHECK(rows[0].readable);

    daemoon_stub_reset();
    (void)daemoon_posix_rmtree(root);
}

TEST_CASE(more_backups_than_the_screen_holds_are_not_written_past)
{
    char root[256];
    char work[320];
    char backups[384];
    char path[512];
    daemoon_strbuf_t sb;
    daemoon_title_t title;
    daemoon_3ds_save_ctx_t save_ctx;
    daemoon_posix_ui_ctx_t ui;
    daemoon_env_t env;
    daemoon_3ds_backup_row_t rows[4];
    static unsigned char scratch[64 * 1024];
    int i;

    CHECK_EQ_INT(daemoon_test_tempdir(root, sizeof(root), "3ds-pickmany"), 0);
    daemoon_stub_init(root);
    daemoon_stub_add_title(TEST_TITLE_ID, "CTR-P-DUMY");
    title_for(&title, TEST_TITLE_ID);

    daemoon_strbuf_init(&sb, work, sizeof(work));
    daemoon_strbuf_add(&sb, root);
    daemoon_strbuf_add(&sb, "/DaeMoon");
    CHECK_OK(daemoon_strbuf_result(&sb));
    (void)snprintf(backups, sizeof(backups), "%s/backups", work);

    backup_env(&env, &save_ctx, &ui, work, scratch, sizeof(scratch));
    CHECK_OK(env.fs->mkdir_p(env.fs_ctx, backups));

    for (i = 0; i < 20; ++i) {
        (void)snprintf(path, sizeof(path), "%s/3ds_%s_%012d.zip", backups, title.id, i);
        write_file(path, "not a package", 13);
    }

    /* The cap is the caller's array, not a constant this file happens to agree
     * with. Getting that wrong writes past a static buffer on a console. */
    CHECK_EQ_INT((int)daemoon_3ds_backup_list(&env, backups, &title, NULL, rows,
                                              sizeof(rows) / sizeof(rows[0])),
                 4);

    daemoon_stub_reset();
    (void)daemoon_posix_rmtree(root);
}

/* -------------------------------------------------------------- name cache */

/* The cache exists for one reason: reading an SMDH opens the title's content and
 * decrypts the front of it, and doing that per title per launch is most of what
 * the loading screen was. Which means the thing worth asserting is not that the
 * right name comes back - the lookup already has tests for that - but that the
 * hardware is not touched a second time. The stub counts the opens. */

static const char *cache_file(char *buf, size_t cap, const char *root)
{
    (void)snprintf(buf, cap, "%s/titles.bin", root);
    return buf;
}

TEST_CASE(a_cached_name_does_not_read_the_smdh_again)
{
    char root[256];
    char path[320];
    daemoon_3ds_title_cache_t *cache = NULL;
    char name[DAEMOON_NAME_MAX];
    unsigned after_first;

    CHECK_EQ_INT(daemoon_test_tempdir(root, sizeof(root), "3ds-cache"), 0);
    daemoon_stub_init(root);
    daemoon_stub_add_title(TEST_TITLE_ID, "CTR-P-DUMY");
    daemoon_stub_set_title_name(TEST_TITLE_ID, 1, "Some Game");
    (void)cache_file(path, sizeof(path), root);

    CHECK_OK(daemoon_3ds_cache_open(path, 1, &cache));

    CHECK_OK(daemoon_3ds_cache_name(cache, (int)MEDIATYPE_SD, TEST_TITLE_ID, 0, name,
                                    sizeof(name)));
    CHECK_STR(name, "Some Game");
    after_first = daemoon_stub_smdh_opens();
    CHECK(after_first > 0);

    /* The icon comes out of the same read. Before the cache these were two
     * separate opens per title, which is why the list took twice as long as it
     * had to. */
    CHECK(daemoon_3ds_cache_icon(cache, (int)MEDIATYPE_SD, TEST_TITLE_ID) != NULL);
    CHECK_EQ_INT((int)daemoon_stub_smdh_opens(), (int)after_first);

    CHECK_OK(daemoon_3ds_cache_name(cache, (int)MEDIATYPE_SD, TEST_TITLE_ID, 0, name,
                                    sizeof(name)));
    CHECK_EQ_INT((int)daemoon_stub_smdh_opens(), (int)after_first);
    CHECK_EQ_INT((int)daemoon_3ds_cache_misses(cache), 1);

    daemoon_3ds_cache_close(cache);
    daemoon_stub_reset();
    (void)daemoon_posix_rmtree(root);
}

TEST_CASE(a_cache_survives_a_relaunch_and_forgets_deleted_titles)
{
    char root[256];
    char path[320];
    daemoon_3ds_title_cache_t *cache = NULL;
    char name[DAEMOON_NAME_MAX];

    CHECK_EQ_INT(daemoon_test_tempdir(root, sizeof(root), "3ds-cache-persist"), 0);
    daemoon_stub_init(root);
    daemoon_stub_add_title(TEST_TITLE_ID, "CTR-P-DUMY");
    daemoon_stub_add_title(TEST_TITLE_OTHER, "CTR-P-OTHR");
    daemoon_stub_set_title_name(TEST_TITLE_ID, 1, "First Game");
    daemoon_stub_set_title_name(TEST_TITLE_OTHER, 1, "Second Game");
    (void)cache_file(path, sizeof(path), root);

    CHECK_OK(daemoon_3ds_cache_open(path, 1, &cache));
    CHECK_OK(daemoon_3ds_cache_name(cache, (int)MEDIATYPE_SD, TEST_TITLE_ID, 0, name,
                                    sizeof(name)));
    CHECK_OK(daemoon_3ds_cache_name(cache, (int)MEDIATYPE_SD, TEST_TITLE_OTHER, 0, name,
                                    sizeof(name)));
    CHECK_OK(daemoon_3ds_cache_flush(cache, path));
    daemoon_3ds_cache_close(cache);

    /* A second launch: the file is there, so nothing goes to the hardware. */
    {
        unsigned before;

        CHECK_OK(daemoon_3ds_cache_open(path, 1, &cache));
        before = daemoon_stub_smdh_opens();
        CHECK_OK(daemoon_3ds_cache_name(cache, (int)MEDIATYPE_SD, TEST_TITLE_ID, 0, name,
                                        sizeof(name)));
        CHECK_STR(name, "First Game");
        CHECK_EQ_INT((int)daemoon_stub_smdh_opens(), (int)before);
        CHECK_EQ_INT((int)daemoon_3ds_cache_hits(cache), 1);

        /* Only one of the two titles was asked about, so the other is one that is
         * no longer installed. Writing it back would grow the file forever. */
        CHECK_OK(daemoon_3ds_cache_flush(cache, path));
        daemoon_3ds_cache_close(cache);
    }

    {
        unsigned before;

        CHECK_OK(daemoon_3ds_cache_open(path, 1, &cache));
        before = daemoon_stub_smdh_opens();
        CHECK_OK(daemoon_3ds_cache_name(cache, (int)MEDIATYPE_SD, TEST_TITLE_OTHER, 0,
                                        name, sizeof(name)));
        CHECK_STR(name, "Second Game");
        /* Pruned, so this one is read from the hardware again. */
        CHECK(daemoon_stub_smdh_opens() > before);
        CHECK_OK(daemoon_3ds_cache_flush(cache, path));
        daemoon_3ds_cache_close(cache);
    }

    /* A launch where the title list could not be read asks about nothing, and that
     * must not be mistaken for every game having been uninstalled. Pruning on that
     * evidence would make one transient AM failure cost the launch after it too. */
    {
        unsigned before;

        CHECK_OK(daemoon_3ds_cache_open(path, 1, &cache));
        CHECK_OK(daemoon_3ds_cache_flush(cache, path));
        daemoon_3ds_cache_close(cache);

        CHECK_OK(daemoon_3ds_cache_open(path, 1, &cache));
        before = daemoon_stub_smdh_opens();
        CHECK_OK(daemoon_3ds_cache_name(cache, (int)MEDIATYPE_SD, TEST_TITLE_OTHER, 0,
                                        name, sizeof(name)));
        CHECK_STR(name, "Second Game");
        CHECK_EQ_INT((int)daemoon_stub_smdh_opens(), (int)before);
        daemoon_3ds_cache_close(cache);
    }

    daemoon_stub_reset();
    (void)daemoon_posix_rmtree(root);
}

TEST_CASE(a_cache_from_another_language_or_format_is_discarded)
{
    char root[256];
    char path[320];
    daemoon_3ds_title_cache_t *cache = NULL;
    char name[DAEMOON_NAME_MAX];
    unsigned before;

    CHECK_EQ_INT(daemoon_test_tempdir(root, sizeof(root), "3ds-cache-lang"), 0);
    daemoon_stub_init(root);
    daemoon_stub_add_title(TEST_TITLE_ID, "CTR-P-DUMY");
    daemoon_stub_set_title_name(TEST_TITLE_ID, 1, "English Name");
    (void)cache_file(path, sizeof(path), root);

    CHECK_OK(daemoon_3ds_cache_open(path, 1, &cache));
    CHECK_OK(daemoon_3ds_cache_name(cache, (int)MEDIATYPE_SD, TEST_TITLE_ID, 0, name,
                                    sizeof(name)));
    CHECK_OK(daemoon_3ds_cache_flush(cache, path));
    daemoon_3ds_cache_close(cache);

    /* A console whose language changed has a file full of names chosen by a
     * fallback chain that started somewhere else. */
    CHECK_OK(daemoon_3ds_cache_open(path, 7, &cache));
    before = daemoon_stub_smdh_opens();
    CHECK_OK(daemoon_3ds_cache_name(cache, (int)MEDIATYPE_SD, TEST_TITLE_ID, 0, name,
                                    sizeof(name)));
    CHECK(daemoon_stub_smdh_opens() > before);
    daemoon_3ds_cache_close(cache);

    /* A truncated file - a card pulled during a write - is not read as a shorter
     * cache full of garbage. */
    {
        FILE *fp = fopen(path, "r+b");

        CHECK(fp != NULL);
        if (fp != NULL) {
            CHECK_EQ_INT(fseek(fp, 0, SEEK_SET), 0);
            (void)fwrite("XXXX", 1, 4, fp);
            (void)fclose(fp);
        }
    }
    CHECK_OK(daemoon_3ds_cache_open(path, 1, &cache));
    before = daemoon_stub_smdh_opens();
    CHECK_OK(daemoon_3ds_cache_name(cache, (int)MEDIATYPE_SD, TEST_TITLE_ID, 0, name,
                                    sizeof(name)));
    CHECK_STR(name, "English Name");
    CHECK(daemoon_stub_smdh_opens() > before);
    daemoon_3ds_cache_close(cache);

    daemoon_stub_reset();
    (void)daemoon_posix_rmtree(root);
}

TEST_CASE(a_title_with_no_smdh_is_only_asked_about_once)
{
    char root[256];
    char path[320];
    daemoon_3ds_title_cache_t *cache = NULL;
    char name[DAEMOON_NAME_MAX];
    unsigned after_first;

    CHECK_EQ_INT(daemoon_test_tempdir(root, sizeof(root), "3ds-cache-none"), 0);
    daemoon_stub_init(root);
    daemoon_stub_add_title(TEST_TITLE_ID, "CTR-P-DUMY");
    /* No name set, so the lookup fails - and on a real console the failure arrives
     * after the open, which makes these the most expensive titles of all. Not
     * remembering the negative would leave them paying full price every launch. */
    (void)cache_file(path, sizeof(path), root);

    CHECK_OK(daemoon_3ds_cache_open(path, 1, &cache));
    CHECK_RESULT(daemoon_3ds_cache_name(cache, (int)MEDIATYPE_SD, TEST_TITLE_ID, 0, name,
                                        sizeof(name)),
                 DAEMOON_ERR_NOT_FOUND);
    after_first = daemoon_stub_smdh_opens();

    CHECK_RESULT(daemoon_3ds_cache_name(cache, (int)MEDIATYPE_SD, TEST_TITLE_ID, 0, name,
                                        sizeof(name)),
                 DAEMOON_ERR_NOT_FOUND);
    CHECK_EQ_INT((int)daemoon_stub_smdh_opens(), (int)after_first);

    /* And the survey has to be able to get past it, because a cached failure is
     * indistinguishable from a bug in the lookup until the hardware is asked. */
    daemoon_3ds_cache_forget(cache);
    CHECK_RESULT(daemoon_3ds_cache_name(cache, (int)MEDIATYPE_SD, TEST_TITLE_ID, 0, name,
                                        sizeof(name)),
                 DAEMOON_ERR_NOT_FOUND);
    CHECK(daemoon_stub_smdh_opens() > after_first);

    daemoon_3ds_cache_close(cache);
    daemoon_stub_reset();
    (void)daemoon_posix_rmtree(root);
}

TEST_CASE(the_list_reads_each_titles_smdh_once)
{
    char root[256];
    char path[320];
    daemoon_3ds_save_ctx_t ctx;
    daemoon_title_t *titles = NULL;
    size_t count = 0;
    unsigned first_pass;

    CHECK_EQ_INT(daemoon_test_tempdir(root, sizeof(root), "3ds-cache-list"), 0);
    daemoon_stub_init(root);
    daemoon_stub_add_title(TEST_TITLE_ID, "CTR-P-DUMY");
    daemoon_stub_add_title(TEST_TITLE_OTHER, "CTR-P-OTHR");
    CHECK_EQ_INT(make_archive(root, TEST_TITLE_ID), 0);
    CHECK_EQ_INT(make_archive(root, TEST_TITLE_OTHER), 0);
    daemoon_stub_set_title_name(TEST_TITLE_ID, 1, "First Game");
    daemoon_stub_set_title_name(TEST_TITLE_OTHER, 1, "Second Game");
    (void)cache_file(path, sizeof(path), root);

    memset(&ctx, 0, sizeof(ctx));
    ctx.media = (int)MEDIATYPE_SD;
    ctx.smdh_language = 1;
    ctx.only_with_saves = 1;
    CHECK_OK(daemoon_3ds_cache_open(path, 1, &ctx.cache));

    CHECK_OK(daemoon_3ds_save_backend.list_titles(&ctx, &titles, &count));
    CHECK_EQ_INT((int)count, 2);
    first_pass = daemoon_stub_smdh_opens();
    /* Two titles, one read each. It used to be two: the name lookup and then the
     * icon loader, both opening the same file. */
    CHECK_EQ_INT((int)first_pass, 2);
    daemoon_3ds_save_backend.free_titles(&ctx, titles, count);

    /* The icons the grid draws come from the same records. */
    CHECK(daemoon_3ds_cache_icon(ctx.cache, ctx.media, TEST_TITLE_ID) != NULL);
    CHECK(daemoon_3ds_cache_icon(ctx.cache, ctx.media, TEST_TITLE_OTHER) != NULL);
    CHECK_EQ_INT((int)daemoon_stub_smdh_opens(), (int)first_pass);

    titles = NULL;
    count = 0;
    CHECK_OK(daemoon_3ds_save_backend.list_titles(&ctx, &titles, &count));
    CHECK_EQ_INT((int)count, 2);
    CHECK_STR(titles[0].name, "First Game");
    CHECK_EQ_INT((int)daemoon_stub_smdh_opens(), (int)first_pass);
    daemoon_3ds_save_backend.free_titles(&ctx, titles, count);

    daemoon_3ds_cache_close(ctx.cache);
    daemoon_stub_reset();
    (void)daemoon_posix_rmtree(root);
}

/* ------------------------------------------------------------ nds-bootstrap */

/* Phase 2's backend, run here rather than through a stub, because it is ordinary
 * file IO: no permissions, no archive, no service. That is also why it is the one
 * that goes on the network first - if the sync path corrupts a save here, the
 * sync path is what is wrong. */

/* A DS cartridge header is a 12 byte title then a 4 character game code. Only
 * those first sixteen bytes are read, so that is all a test ROM needs. */
static void write_rom(const char *path, const char *title, const char *code)
{
    unsigned char header[16];

    memset(header, 0, sizeof(header));
    memcpy(header, title, strlen(title) < 12 ? strlen(title) : 12);
    memcpy(header + 12, code, 4);
    write_file(path, header, sizeof(header));
}

typedef struct {
    char   name[DAEMOON_PATH_MAX];
    size_t size;
    int    seen;
} nds_seen_t;

static int nds_entry_cb(void *user, const char *path, unsigned long long size)
{
    nds_seen_t *seen = (nds_seen_t *)user;

    (void)daemoon_strlcpy(seen->name, sizeof(seen->name), path);
    seen->size = (size_t)size;
    seen->seen = 1;
    return 0;
}

TEST_CASE(nds_titles_are_named_from_the_cartridge_header)
{
    char root[256];
    char roms[320];
    char saves[320];
    char path[512];
    daemoon_strbuf_t sb;
    daemoon_3ds_nds_ctx_t ctx;
    daemoon_title_t *titles = NULL;
    size_t count = 0;
    size_t i;
    int found_rom = 0;
    int found_orphan = 0;

    CHECK_EQ_INT(daemoon_test_tempdir(root, sizeof(root), "nds-titles"), 0);

    daemoon_strbuf_init(&sb, roms, sizeof(roms));
    daemoon_strbuf_add(&sb, root);
    daemoon_strbuf_add(&sb, "/roms");
    CHECK_OK(daemoon_strbuf_result(&sb));
    CHECK_OK(daemoon_posix_mkdir_p(roms));

    daemoon_strbuf_init(&sb, saves, sizeof(saves));
    daemoon_strbuf_add(&sb, root);
    daemoon_strbuf_add(&sb, "/roms/saves");
    CHECK_OK(daemoon_strbuf_result(&sb));
    CHECK_OK(daemoon_posix_mkdir_p(saves));

    /* A real card has names with spaces and Hangul in them, which is exactly why
     * the id comes from the cartridge header instead. */
    (void)snprintf(path, sizeof(path), "%s/5684 포켓몬화이트 (K).nds", roms);
    write_rom(path, "POKEMON W", "IRAK");
    (void)snprintf(path, sizeof(path), "%s/5684 포켓몬화이트 (K).sav", saves);
    write_file(path, "save data", 9);

    /* And a save with no ROM beside it, which is a thing people do. */
    (void)snprintf(path, sizeof(path), "%s/orphan.sav", saves);
    write_file(path, "x", 1);

    ctx.rom_dir = roms;
    ctx.save_dir = saves;

    CHECK_OK(daemoon_3ds_nds_backend.list_titles(&ctx, &titles, &count));
    CHECK_EQ_INT(count, 2);

    for (i = 0; i < count; ++i) {
        CHECK_EQ_INT(titles[i].platform, DAEMOON_PLATFORM_NDS);
        CHECK_EQ_INT(titles[i].save_type, DAEMOON_SAVE_NDS);
        CHECK(titles[i].has_save);
        /* The manifest schema accepts uppercase, digits, underscore and dash. */
        {
            const char *p;
            for (p = titles[i].id; *p != '\0'; ++p) {
                CHECK((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'Z') ||
                      *p == '_' || *p == '-');
            }
            CHECK(strlen(titles[i].id) >= 4);
        }
        if (strcmp(titles[i].id, "IRAK_POKEMON_W") == 0) {
            found_rom = 1;
            CHECK_EQ_INT(titles[i].size_hint, 9);
        }
        if (strncmp(titles[i].id, "ORPHAN", 6) == 0) {
            found_orphan = 1;
        }
    }
    CHECK(found_rom);
    CHECK(found_orphan);

    daemoon_3ds_nds_backend.free_titles(&ctx, titles, count);
    (void)daemoon_posix_rmtree(root);
}

TEST_CASE(an_nds_save_round_trips_through_core)
{
    char root[256];
    char roms[320];
    char saves[320];
    char work[320];
    char backup[512];
    char path[512];
    daemoon_strbuf_t sb;
    daemoon_3ds_nds_ctx_t ctx;
    daemoon_posix_ui_ctx_t ui;
    daemoon_archive_ctx_t actx;
    daemoon_env_t env;
    daemoon_title_t *titles = NULL;
    size_t count = 0;
    static unsigned char scratch[64 * 1024];
    char buf[64];
    FILE *fp;
    size_t got;

    CHECK_EQ_INT(daemoon_test_tempdir(root, sizeof(root), "nds-roundtrip"), 0);

    daemoon_strbuf_init(&sb, roms, sizeof(roms));
    daemoon_strbuf_add(&sb, root);
    daemoon_strbuf_add(&sb, "/roms");
    CHECK_OK(daemoon_strbuf_result(&sb));
    CHECK_OK(daemoon_posix_mkdir_p(roms));

    daemoon_strbuf_init(&sb, saves, sizeof(saves));
    daemoon_strbuf_add(&sb, root);
    daemoon_strbuf_add(&sb, "/roms/saves");
    CHECK_OK(daemoon_strbuf_result(&sb));
    CHECK_OK(daemoon_posix_mkdir_p(saves));

    daemoon_strbuf_init(&sb, work, sizeof(work));
    daemoon_strbuf_add(&sb, root);
    daemoon_strbuf_add(&sb, "/DaeMoon");
    CHECK_OK(daemoon_strbuf_result(&sb));

    (void)snprintf(path, sizeof(path), "%s/game.nds", roms);
    write_rom(path, "TEST GAME", "ATGE");
    (void)snprintf(path, sizeof(path), "%s/game.sav", saves);
    write_file(path, "the original save", 17);

    ctx.rom_dir = roms;
    ctx.save_dir = saves;
    daemoon_posix_ui_init(&ui);
    actx.count = 0;

    memset(&env, 0, sizeof(env));
    env.save = &daemoon_3ds_nds_backend;
    env.fs = &daemoon_3ds_fs_backend;
    env.ui = &daemoon_posix_ui_backend;
    env.save_ctx = &ctx;
    env.ui_ctx = &ui;
    env.device_label = "3DS";
    env.work_dir = work;
    env.scratch = scratch;
    env.scratch_len = sizeof(scratch);
    CHECK_OK(daemoon_env_validate(&env));

    CHECK_OK(daemoon_3ds_nds_backend.list_titles(&ctx, &titles, &count));
    CHECK_EQ_INT(count, 1);

    CHECK_OK(daemoon_sync_backup_local(&env, &actx, &titles[0], backup, sizeof(backup)));

    /* Play on: a shorter save, so a restore that does not clear first would leave
     * the tail of the longer one behind. */
    write_file(path, "later", 5);

    CHECK_OK(daemoon_sync_restore_package(&env, &actx, &titles[0], backup));
    CHECK(ui.confirms > 0);

    fp = fopen(path, "rb");
    CHECK(fp != NULL);
    got = fread(buf, 1, sizeof(buf) - 1, fp);
    (void)fclose(fp);
    buf[got] = '\0';
    CHECK_STR(buf, "the original save");

    daemoon_3ds_nds_backend.free_titles(&ctx, titles, count);
    (void)daemoon_posix_rmtree(root);
}

TEST_CASE(an_nds_package_only_carries_the_one_entry)
{
    /* A package made from something else would write a file the game cannot read,
     * so the backend refuses any entry but its own. */
    char root[256];
    char saves[320];
    char path[512];
    daemoon_strbuf_t sb;
    daemoon_3ds_nds_ctx_t ctx;
    daemoon_title_t *titles = NULL;
    daemoon_save_t *save = NULL;
    daemoon_stream_t *f = NULL;
    nds_seen_t seen;
    size_t count = 0;

    CHECK_EQ_INT(daemoon_test_tempdir(root, sizeof(root), "nds-entry"), 0);

    daemoon_strbuf_init(&sb, saves, sizeof(saves));
    daemoon_strbuf_add(&sb, root);
    daemoon_strbuf_add(&sb, "/saves");
    CHECK_OK(daemoon_strbuf_result(&sb));
    CHECK_OK(daemoon_posix_mkdir_p(saves));

    (void)snprintf(path, sizeof(path), "%s/game.sav", saves);
    write_file(path, "data", 4);

    ctx.rom_dir = root;
    ctx.save_dir = saves;

    CHECK_OK(daemoon_3ds_nds_backend.list_titles(&ctx, &titles, &count));
    CHECK_EQ_INT(count, 1);

    CHECK_OK(daemoon_3ds_nds_backend.open_save(&ctx, &titles[0], &save));

    memset(&seen, 0, sizeof(seen));
    CHECK_OK(daemoon_3ds_nds_backend.list_entries(&ctx, save, nds_entry_cb, &seen));
    CHECK(seen.seen);
    CHECK_STR(seen.name, "save.sav");
    CHECK_EQ_INT(seen.size, 4);

    CHECK_RESULT(daemoon_3ds_nds_backend.open_file(&ctx, save, "something-else.bin",
                                                   DAEMOON_OPEN_READ, &f),
                 DAEMOON_ERR_NOT_FOUND);
    /* And a read only save refuses a write, like every other backend. */
    CHECK_RESULT(daemoon_3ds_nds_backend.open_file(&ctx, save, "save.sav",
                                                   DAEMOON_OPEN_WRITE, &f),
                 DAEMOON_ERR_FORBIDDEN);

    CHECK_OK(daemoon_3ds_nds_backend.close_save(&ctx, save));
    daemoon_3ds_nds_backend.free_titles(&ctx, titles, count);
    (void)daemoon_posix_rmtree(root);
}

/* The 8x8 Morton order a 3DS texture stores a tile in, written out by hand.
 *
 * Here on purpose rather than shared with the code under test: a swizzle that is
 * wrong does not fail, it draws noise, and checking it against the same
 * expression that produced it would check nothing. This table is the hardware
 * layout, and it is short enough to read. */
static const unsigned char k_morton8[8][8] = {
    {  0,  1,  4,  5, 16, 17, 20, 21 },
    {  2,  3,  6,  7, 18, 19, 22, 23 },
    {  8,  9, 12, 13, 24, 25, 28, 29 },
    { 10, 11, 14, 15, 26, 27, 30, 31 },
    { 32, 33, 36, 37, 48, 49, 52, 53 },
    { 34, 35, 38, 39, 50, 51, 54, 55 },
    { 40, 41, 44, 45, 56, 57, 60, 61 },
    { 42, 43, 46, 47, 58, 59, 62, 63 }
};

/* Where (x, y) of a 48 wide tiled image lands. The band arithmetic is the same
 * one icons.c copies with: 8 rows at a time, 6 tiles of 64 across. */
static size_t tiled_at(unsigned x, unsigned y)
{
    return ((size_t)(y / 8) * 6u + (x / 8)) * 64u + k_morton8[y % 8][x % 8];
}

/* A ROM with just enough of a header and a banner to have an icon. */
static void write_rom_with_banner(const char *path, const unsigned char *bitmap,
                                  const unsigned short *palette)
{
    static unsigned char rom[0x440];
    size_t i;

    memset(rom, 0, sizeof(rom));
    /* Header offset 0x68: where the banner is. */
    rom[0x68] = 0x00;
    rom[0x69] = 0x02;
    memcpy(rom + 0x200 + 0x20, bitmap, 512);
    for (i = 0; i < 16; ++i) {
        rom[0x200 + 0x220 + i * 2] = (unsigned char)(palette[i] & 0xff);
        rom[0x200 + 0x220 + i * 2 + 1] = (unsigned char)(palette[i] >> 8);
    }
    write_file(path, rom, sizeof(rom));
}

/* Sets one pixel of a DS icon bitmap: sixteen 8x8 tiles, four bits each, low
 * nibble first. */
static void nds_set_pixel(unsigned char *bitmap, unsigned x, unsigned y, unsigned idx)
{
    unsigned tile = (y / 8) * 4 + (x / 8);
    unsigned within = (y % 8) * 8 + (x % 8);
    unsigned char *b = &bitmap[tile * 32 + within / 2];

    if (within & 1) {
        *b = (unsigned char)((*b & 0x0f) | (idx << 4));
    } else {
        *b = (unsigned char)((*b & 0xf0) | idx);
    }
}

TEST_CASE(an_nds_icon_comes_out_of_the_cartridge_banner)
{
    char root[256];
    char roms[320];
    char rom[420];
    static unsigned char bitmap[512];
    static unsigned short out[DAEMOON_3DS_ICON_BYTES / 2];
    unsigned short palette[16];
    /* BGR555, which is not the order it will come out in. */
    const unsigned short bgr_red = 0x001f;
    const unsigned short bgr_green = 0x03e0;
    const unsigned short bgr_blue = 0x7c00;
    const unsigned inset = (48 - 32) / 2;

    CHECK_EQ_INT(daemoon_test_tempdir(root, sizeof(root), "nds-icon"), 0);
    (void)snprintf(roms, sizeof(roms), "%s/roms", root);
    CHECK_EQ_INT(mkdir(roms, 0755), 0);
    (void)snprintf(rom, sizeof(rom), "%s/A Game (K)", roms);

    memset(palette, 0, sizeof(palette));
    palette[1] = bgr_red;
    palette[2] = bgr_green;
    palette[3] = bgr_blue;

    memset(bitmap, 0, sizeof(bitmap));
    /* Corners and a pixel inside the second tile, so a swizzle that is off by a
     * tile and one that is off within a tile both show up. */
    nds_set_pixel(bitmap, 0, 0, 1);
    nds_set_pixel(bitmap, 1, 0, 2);
    nds_set_pixel(bitmap, 0, 1, 3);
    nds_set_pixel(bitmap, 9, 2, 1);
    nds_set_pixel(bitmap, 31, 31, 2);

    {
        char with_ext[460];

        (void)snprintf(with_ext, sizeof(with_ext), "%s.nds", rom);
        write_rom_with_banner(with_ext, bitmap, palette);
    }

    CHECK_OK(daemoon_3ds_nds_icon_read(roms, "A Game (K)", out));

    /* BGR555 red is RGB565 red, in the other half of the word. */
    CHECK_EQ_INT((int)out[tiled_at(inset + 0, inset + 0)], 0xf800);
    CHECK_EQ_INT((int)out[tiled_at(inset + 1, inset + 0)], 0x07e0);
    CHECK_EQ_INT((int)out[tiled_at(inset + 0, inset + 1)], 0x001f);
    CHECK_EQ_INT((int)out[tiled_at(inset + 9, inset + 2)], 0xf800);
    CHECK_EQ_INT((int)out[tiled_at(inset + 31, inset + 31)], 0x07e0);

    /* Index 0 is transparent and the texture has nowhere to say so, so it becomes
     * the same flat tile the grid draws for a title with no icon - and so does
     * everything outside the 32x32. */
    {
        const unsigned short panel = (unsigned short)(((0x24 >> 3) << 11) |
                                                      ((0x28 >> 2) << 5) | (0x32 >> 3));
        int coloured = 0;
        size_t i;

        CHECK_EQ_INT((int)out[tiled_at(inset + 5, inset + 5)], (int)panel);
        CHECK_EQ_INT((int)out[tiled_at(0, 0)], (int)panel);
        CHECK_EQ_INT((int)out[tiled_at(47, 47)], (int)panel);

        for (i = 0; i < sizeof(out) / sizeof(out[0]); ++i) {
            if (out[i] != panel) {
                ++coloured;
            }
        }
        /* Exactly the five pixels that were set, so nothing was written twice and
         * nothing landed outside the icon. */
        CHECK_EQ_INT(coloured, 5);
    }

    (void)daemoon_posix_rmtree(root);
}

TEST_CASE(a_rom_with_no_banner_has_no_icon)
{
    char root[256];
    char roms[320];
    char path[460];
    static unsigned char rom[0x440];
    static unsigned short out[DAEMOON_3DS_ICON_BYTES / 2];

    CHECK_EQ_INT(daemoon_test_tempdir(root, sizeof(root), "nds-icon-none"), 0);
    (void)snprintf(roms, sizeof(roms), "%s/roms", root);
    CHECK_EQ_INT(mkdir(roms, 0755), 0);

    /* No ROM beside the save at all: normal, somebody copied a .sav on its own. */
    CHECK_RESULT(daemoon_3ds_nds_icon_read(roms, "Missing", out),
                 DAEMOON_ERR_NOT_FOUND);

    /* A ROM whose banner pointer is zero: normal for homebrew. */
    memset(rom, 0, sizeof(rom));
    (void)snprintf(path, sizeof(path), "%s/Homebrew.nds", roms);
    write_file(path, rom, sizeof(rom));
    CHECK_RESULT(daemoon_3ds_nds_icon_read(roms, "Homebrew", out),
                 DAEMOON_ERR_NOT_FOUND);

    /* A ROM that points past its own end - a copy that stopped early. */
    rom[0x68] = 0x00;
    rom[0x69] = 0x02;
    (void)snprintf(path, sizeof(path), "%s/Truncated.nds", roms);
    write_file(path, rom, 0x210);
    CHECK_RESULT(daemoon_3ds_nds_icon_read(roms, "Truncated", out),
                 DAEMOON_ERR_NOT_FOUND);

    (void)daemoon_posix_rmtree(root);
}

TEST_CASE(the_nds_backend_conforms)
{
    char root[256];
    char saves[320];
    char path[512];
    daemoon_strbuf_t sb;
    daemoon_3ds_nds_ctx_t ctx;
    daemoon_title_t *titles = NULL;
    daemoon_backend_under_test_t ut;
    size_t count = 0;
    unsigned char scratch[4096];

    CHECK_EQ_INT(daemoon_test_tempdir(root, sizeof(root), "nds-conformance"), 0);

    daemoon_strbuf_init(&sb, saves, sizeof(saves));
    daemoon_strbuf_add(&sb, root);
    daemoon_strbuf_add(&sb, "/saves");
    CHECK_OK(daemoon_strbuf_result(&sb));
    CHECK_OK(daemoon_posix_mkdir_p(saves));

    (void)snprintf(path, sizeof(path), "%s/first-game.sav", saves);
    write_file(path, "one", 3);
    (void)snprintf(path, sizeof(path), "%s/second-game.sav", saves);
    write_file(path, "two", 3);

    ctx.rom_dir = root;
    ctx.save_dir = saves;

    CHECK_OK(daemoon_3ds_nds_backend.list_titles(&ctx, &titles, &count));
    CHECK_EQ_INT(count, 2);

    memset(&ut, 0, sizeof(ut));
    ut.name = "nds (plain .sav files)";
    ut.backend = &daemoon_3ds_nds_backend;
    ut.ctx = &ctx;
    ut.title = &titles[0];
    ut.other = &titles[1];
    ut.scratch = scratch;
    ut.scratch_len = sizeof(scratch);
    /* One file is all an nds save is, so the cases that write a tree do not
     * apply. */
    ut.single_entry = 1;
    ut.entry_name = "save.sav";

    daemoon_backend_conformance(&ut);

    daemoon_3ds_nds_backend.free_titles(&ctx, titles, count);
    (void)daemoon_posix_rmtree(root);
}


/* ------------------------------------------------------------------ config */

TEST_CASE(the_config_file_is_forgiving_but_not_careless)
{
    char root[256];
    char path[512];
    daemoon_3ds_config_t cfg;

    CHECK_EQ_INT(daemoon_test_tempdir(root, sizeof(root), "cfg"), 0);
    (void)snprintf(path, sizeof(path), "%s/config.txt", root);

    /* No file is a console that has not been pointed at a server, not a failure:
     * everything local still works. */
    CHECK_RESULT(daemoon_3ds_config_load(path, &cfg), DAEMOON_ERR_NOT_FOUND);
    CHECK_EQ_INT(daemoon_3ds_config_can_sync(&cfg), 0);
    CHECK_STR(cfg.device_label, "3DS");

    write_file(path,
               "# edited on a phone, probably\n"
               "server = https://saves.example.invalid/ \n"
               "token=abc123\n"
               "label = 거실 3DS\n"
               "nonsense without an equals sign\n"
               "unknown_key = ignored\n",
               strlen("# edited on a phone, probably\n"
                      "server = https://saves.example.invalid/ \n"
                      "token=abc123\n"
                      "label = 거실 3DS\n"
                      "nonsense without an equals sign\n"
                      "unknown_key = ignored\n"));

    CHECK_OK(daemoon_3ds_config_load(path, &cfg));
    /* The trailing slash is gone: kept, it becomes a double slash in every path
     * built from it, and servers do not agree about those. */
    CHECK_STR(cfg.server_url, "https://saves.example.invalid");
    CHECK_STR(cfg.token, "abc123");
    /* The label is shown on another console in a conflict dialog, so it has to
     * survive as UTF-8. */
    CHECK_STR(cfg.device_label, "거실 3DS");
    CHECK_EQ_INT(daemoon_3ds_config_can_sync(&cfg), 1);

    /* A server with no token cannot sync, and saying so beats a stack of 401s. */
    write_file(path, "server=https://x.invalid\n", 25);
    CHECK_OK(daemoon_3ds_config_load(path, &cfg));
    CHECK_EQ_INT(daemoon_3ds_config_can_sync(&cfg), 0);

    /* A label that is not valid UTF-8 is dropped rather than carried into a
     * manifest that the server would reject. */
    write_file(path, "server=https://x.invalid\ntoken=t\nlabel=\xff\xfe\n", 42);
    CHECK_OK(daemoon_3ds_config_load(path, &cfg));
    CHECK_STR(cfg.device_label, "3DS");

    (void)daemoon_posix_rmtree(root);
}

/* The settings screen writes this file, so what it writes has to be what the
 * parser reads back - including a label with a space and Hangul in it, which is
 * what somebody actually types on a console keyboard. */
TEST_CASE(a_saved_config_is_read_back_exactly)
{
    char root[256];
    char path[320];
    daemoon_3ds_config_t out;
    daemoon_3ds_config_t in;

    CHECK_EQ_INT(daemoon_test_tempdir(root, sizeof(root), "3ds-cfgsave"), 0);
    (void)snprintf(path, sizeof(path), "%s/config.txt", root);

    daemoon_3ds_config_defaults(&out);
    (void)daemoon_strlcpy(out.server_url, sizeof(out.server_url),
                          "http://192.168.1.13:8080");
    (void)daemoon_strlcpy(out.token, sizeof(out.token), "MCRV_abc-123_XYZ");
    (void)daemoon_strlcpy(out.device_label, sizeof(out.device_label), "거실 3DS");

    CHECK_OK(daemoon_3ds_config_save(path, &out));
    CHECK_OK(daemoon_3ds_config_load(path, &in));
    CHECK_STR(in.server_url, "http://192.168.1.13:8080");
    /* No language set means "follow the console", which is not the same as English
     * and has to survive a round trip as an absence rather than as a value. */
    CHECK_STR(in.language, "");
    CHECK_STR(in.token, "MCRV_abc-123_XYZ");
    CHECK_STR(in.device_label, "거실 3DS");
    CHECK_EQ_INT(daemoon_3ds_config_can_sync(&in), 1);

    /* A trailing slash is stripped on the way in, so saving what was loaded does
     * not slowly grow a URL. */
    (void)daemoon_strlcpy(out.server_url, sizeof(out.server_url),
                          "http://example.test:8080/");
    CHECK_OK(daemoon_3ds_config_save(path, &out));
    CHECK_OK(daemoon_3ds_config_load(path, &in));
    CHECK_STR(in.server_url, "http://example.test:8080");
    CHECK_OK(daemoon_3ds_config_save(path, &in));
    CHECK_OK(daemoon_3ds_config_load(path, &in));
    CHECK_STR(in.server_url, "http://example.test:8080");

    /* A chosen language is kept; a code this build does not know is refused
     * rather than stored, because a typo would otherwise be a console that
     * silently shows English and a settings file that disagrees with it. */
    (void)daemoon_strlcpy(out.language, sizeof(out.language), "ko");
    CHECK_OK(daemoon_3ds_config_save(path, &out));
    CHECK_OK(daemoon_3ds_config_load(path, &in));
    CHECK_STR(in.language, "ko");

    {
        FILE *fp = fopen(path, "ab");

        CHECK(fp != NULL);
        if (fp != NULL) {
            (void)fputs("language = klingon\n", fp);
            (void)fclose(fp);
        }
    }
    CHECK_OK(daemoon_3ds_config_load(path, &in));
    CHECK_STR(in.language, "ko");

    (void)daemoon_posix_rmtree(root);
}

/* ---------------------------------------------------------- camera preview */

/* The camera frame, rotated and tiled for the GPU.
 *
 * The first attempt used GX_DisplayTransfer and drew noise, and there was no way
 * to tell which of three guesses was wrong - buffer dimensions, orientation, or
 * subtexture - because looking at the result meant photographing a console. Here
 * it can be wrong for the price of a line of output.
 *
 * The tiling is checked against the same hand written Morton table the nds icons
 * use, and the rotation against a frame whose corners are all different.
 */
TEST_CASE(a_camera_frame_is_rotated_and_tiled)
{
    enum { CW = 16, CH = 8, TW = 16, TH = 16 };
    static unsigned short frame[CW * CH];
    static unsigned short tex[TW * TH];
    unsigned x;
    unsigned y;

    /* Each pixel encodes where it came from, so a mapping that is off by a row, a
     * column, or a flip is a different value rather than a similar picture. */
    for (y = 0; y < CH; ++y) {
        for (x = 0; x < CW; ++x) {
            frame[daemoon_3ds_cam_index(x, y, CW)] =
                (unsigned short)(0x1000u + y * 0x40u + x);
        }
    }

    daemoon_3ds_cam_to_tiled(frame, CW, CH, tex, TW, TH, 0);

    for (y = 0; y < CH; ++y) {
        for (x = 0; x < CW; ++x) {
            unsigned short want = (unsigned short)(0x1000u + y * 0x40u + x);
            size_t at = ((size_t)(y / 8) * (TW / 8) + (x / 8)) * 64u +
                        k_morton8[y % 8][x % 8];

            if (tex[at] != want) {
                printf("  (%u,%u): tex[%u] = %04X, want %04X\n", x, y,
                       (unsigned)at, tex[at], want);
            }
            CHECK_EQ_INT((int)tex[at], (int)want);
        }
    }

    /* And everything outside the frame is cleared, so a smaller frame in a larger
     * texture does not leave the previous one showing around the edges. */
    {
        int stale = 0;
        size_t i;

        for (i = 0; i < (size_t)TW * TH; ++i) {
            if (tex[i] != 0 && (tex[i] < 0x1000u || tex[i] > 0x1000u + 8 * 0x40u)) {
                stale = 1;
            }
        }
        CHECK_EQ_INT(stale, 0);
    }
}

/* The camera keeps a frame in rows, left to right, top to bottom.
 *
 * Two attempts assumed otherwise, on the strength of the camera being "in
 * framebuffer order", and both drew a scrambled screen. What settles it is not a
 * document: quirc is handed the same buffer as a 400 wide image and decodes real
 * codes from it, and reading columns as 400 wide rows would shear the picture one
 * pixel further along on every line rather than rotate it. No QR code survives
 * that. It decoded, so these are rows. */
TEST_CASE(the_camera_layout_is_rows_of_the_frame_width)
{
    CHECK_EQ_INT((int)daemoon_3ds_cam_index(0, 0, 400), 0);
    CHECK_EQ_INT((int)daemoon_3ds_cam_index(399, 0, 400), 399);
    /* One row down is one frame width further on. */
    CHECK_EQ_INT((int)daemoon_3ds_cam_index(0, 1, 400), 400);
    /* And the last pixel of a 400x240 frame is the last index. */
    CHECK_EQ_INT((int)daemoon_3ds_cam_index(399, 239, 400), 400 * 240 - 1);
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
    RUN(a_name_missing_in_the_console_language_falls_back);
    RUN(the_list_prefers_a_name_the_console_can_draw);
    RUN(the_smdh_is_read_whole_from_the_start);

    printf("backup picker\n");
    RUN(the_backup_list_reads_each_package_and_marks_the_current_one);
    RUN(a_damaged_package_is_a_row_rather_than_a_crash);
    RUN(more_backups_than_the_screen_holds_are_not_written_past);

    printf("camera preview\n");
    RUN(a_camera_frame_is_rotated_and_tiled);
    RUN(the_camera_layout_is_rows_of_the_frame_width);

    printf("name and icon cache\n");
    RUN(a_cached_name_does_not_read_the_smdh_again);
    RUN(a_cache_survives_a_relaunch_and_forgets_deleted_titles);
    RUN(a_cache_from_another_language_or_format_is_discarded);
    RUN(a_title_with_no_smdh_is_only_asked_about_once);
    RUN(the_list_reads_each_titles_smdh_once);

    printf("nds-bootstrap backend\n");
    RUN(nds_titles_are_named_from_the_cartridge_header);
    RUN(an_nds_save_round_trips_through_core);
    RUN(an_nds_package_only_carries_the_one_entry);
    RUN(an_nds_icon_comes_out_of_the_cartridge_banner);
    RUN(a_rom_with_no_banner_has_no_icon);
    RUN(the_nds_backend_conforms);
    RUN(the_config_file_is_forgiving_but_not_careless);
    RUN(a_saved_config_is_read_back_exactly);
}
