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
}
