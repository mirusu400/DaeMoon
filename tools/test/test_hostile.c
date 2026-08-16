/* Packages that arrive from somewhere else.
 *
 * A share code is downloaded without authentication, so a console can be handed a
 * package assembled by anyone. Everything here builds a deliberately malformed one
 * with miniz directly, rather than through daemoon_archive_pack, because the packer
 * would refuse to produce most of them and the point is what the reader does with
 * what it is given.
 *
 * The bar is not "does not crash". It is that the save archive is untouched
 * afterwards, and that nothing appears outside it.
 */
#define _POSIX_C_SOURCE 200809L

#include "test.h"

#include "daemoon_posix.h"
#include "fixture_env.h"
#include "posix_internal.h"

#include <daemoon/archive.h>
#include <daemoon/sync.h>
#include <daemoon/util/strbuf.h>

#include <miniz/miniz.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* A manifest that is well formed on its own, so a test exercises the entry
 * handling rather than tripping over the manifest first. */
static const char k_manifest[] =
    "{\"format_version\":1,\"platform\":\"3ds\",\"title_id\":\"0004000000055D00\","
    "\"save_type\":\"savedata\",\"version\":0,\"parent_version\":null,"
    "\"sha256\":\"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\","
    "\"size\":0,\"device_label\":\"attacker\",\"created_at\":\"1970-01-01T00:00:00Z\"}";

typedef struct {
    const char *name;
    const char *body;
} hostile_entry_t;

/* Writes a zip with exactly the entries given, bypassing every check the real
 * packer applies. */
static int write_hostile_zip(const char *path, const char *manifest,
                             const hostile_entry_t *entries, size_t n)
{
    mz_zip_archive zip;
    size_t i;

    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_writer_init_file(&zip, path, 0)) {
        return -1;
    }
    for (i = 0; i < n; ++i) {
        if (!mz_zip_writer_add_mem(&zip, entries[i].name, entries[i].body,
                                   strlen(entries[i].body), MZ_DEFAULT_COMPRESSION)) {
            mz_zip_writer_end(&zip);
            return -1;
        }
    }
    if (manifest != NULL) {
        if (!mz_zip_writer_add_mem(&zip, DAEMOON_ARCHIVE_MANIFEST_PATH, manifest,
                                   strlen(manifest), MZ_DEFAULT_COMPRESSION)) {
            mz_zip_writer_end(&zip);
            return -1;
        }
    }
    if (!mz_zip_writer_finalize_archive(&zip)) {
        mz_zip_writer_end(&zip);
        return -1;
    }
    mz_zip_writer_end(&zip);
    return 0;
}

static int path_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* Runs unpack against a package built from entries, and reports the result. */
static daemoon_result_t unpack_hostile(fixture_t *f, const daemoon_title_t *t,
                                       const char *manifest, const hostile_entry_t *entries,
                                       size_t n)
{
    char pkg_path[400];
    daemoon_strbuf_t sb;
    daemoon_stream_t *pkg = NULL;
    daemoon_save_t *save = NULL;
    daemoon_result_t r;

    daemoon_strbuf_init(&sb, pkg_path, sizeof(pkg_path));
    daemoon_strbuf_add(&sb, f->root);
    daemoon_strbuf_add(&sb, "/hostile.zip");
    if (daemoon_strbuf_result(&sb) != DAEMOON_OK) {
        return DAEMOON_ERR_BUFFER_TOO_SMALL;
    }
    if (write_hostile_zip(pkg_path, manifest, entries, n) != 0) {
        return DAEMOON_ERR_IO_ERROR;
    }

    r = f->env.fs->open(f->env.fs_ctx, pkg_path, DAEMOON_OPEN_READ, &pkg);
    if (r != DAEMOON_OK) {
        return r;
    }
    r = f->env.save->open_save_write(f->env.save_ctx, t, &save);
    if (r != DAEMOON_OK) {
        (void)daemoon_stream_close(pkg);
        return r;
    }

    r = daemoon_archive_unpack(&f->env, &f->actx, pkg, save);

    (void)daemoon_stream_close(pkg);
    (void)f->env.save->close_save(f->env.save_ctx, save);
    return r;
}

TEST_CASE(unpack_refuses_a_path_that_escapes_the_save)
{
    /* The classic zip slip. On a console the save root is an archive mount, so an
     * escape is not merely a stray file: it is a write through a handle that was
     * opened for one title's save. */
    static const char *const escapes[] = {
        "payload/../escape.bin",
        "payload/../../escape.bin",
        "payload/sub/../../escape.bin",
        "payload//escape.bin",
        "payload/sub/../ok.bin"  /* normalises inside, still refused: no .. at all */
    };
    size_t i;

    for (i = 0; i < sizeof(escapes) / sizeof(escapes[0]); ++i) {
        fixture_t f;
        const daemoon_title_t *t;
        hostile_entry_t entry;
        char escaped[512];
        daemoon_strbuf_t sb;

        CHECK_EQ_INT(fixture_open(&f, "zipslip"), 0);
        t = fixture_add_title(&f, "0004000000055D00", DAEMOON_PLATFORM_3DS);
        CHECK_EQ_INT(fixture_write_save_file(&f, t, "main.sav", "the real save"), 0);

        entry.name = escapes[i];
        entry.body = "owned";

        CHECK_RESULT(unpack_hostile(&f, t, k_manifest, &entry, 1), DAEMOON_ERR_ARCHIVE_ERROR);

        /* Nothing landed next to the save directory. */
        daemoon_strbuf_init(&sb, escaped, sizeof(escaped));
        daemoon_strbuf_add(&sb, f.saves);
        daemoon_strbuf_add(&sb, "/escape.bin");
        CHECK_OK(daemoon_strbuf_result(&sb));
        CHECK(!path_exists(escaped));

        daemoon_strbuf_reset(&sb);
        daemoon_strbuf_add(&sb, f.root);
        daemoon_strbuf_add(&sb, "/escape.bin");
        CHECK(!path_exists(escaped));

        /* And the archive was refused before it was cleared, so the save that was
         * there is still there. A package that cannot be trusted must not cost the
         * user the save they already had. */
        CHECK(fixture_save_file_exists(&f, t, "main.sav"));

        fixture_close(&f);
    }
}

TEST_CASE(unpack_refuses_an_absolute_path)
{
    fixture_t f;
    const daemoon_title_t *t;
    hostile_entry_t entry;

    CHECK_EQ_INT(fixture_open(&f, "absolute"), 0);
    t = fixture_add_title(&f, "0004000000055D00", DAEMOON_PLATFORM_3DS);
    CHECK_EQ_INT(fixture_write_save_file(&f, t, "main.sav", "the real save"), 0);

    entry.name = "payload//tmp/owned.bin";
    entry.body = "owned";

    CHECK_RESULT(unpack_hostile(&f, t, k_manifest, &entry, 1), DAEMOON_ERR_ARCHIVE_ERROR);
    CHECK(!path_exists("/tmp/owned.bin"));
    CHECK(fixture_save_file_exists(&f, t, "main.sav"));

    fixture_close(&f);
}

TEST_CASE(unpack_refuses_an_entry_outside_the_payload)
{
    /* A package is manifest.json plus payload/. Anything else means it was made by
     * something other than this project, and guessing at its intent is how a file
     * ends up somewhere nobody expected. */
    fixture_t f;
    const daemoon_title_t *t;
    hostile_entry_t entry;

    CHECK_EQ_INT(fixture_open(&f, "stray"), 0);
    t = fixture_add_title(&f, "0004000000055D00", DAEMOON_PLATFORM_3DS);
    CHECK_EQ_INT(fixture_write_save_file(&f, t, "main.sav", "the real save"), 0);

    entry.name = "boot.firm";
    entry.body = "not a save";

    CHECK_RESULT(unpack_hostile(&f, t, k_manifest, &entry, 1), DAEMOON_ERR_ARCHIVE_ERROR);
    CHECK(fixture_save_file_exists(&f, t, "main.sav"));

    fixture_close(&f);
}

TEST_CASE(unpack_refuses_a_control_character_in_a_path)
{
    fixture_t f;
    const daemoon_title_t *t;
    hostile_entry_t entry;

    CHECK_EQ_INT(fixture_open(&f, "control"), 0);
    t = fixture_add_title(&f, "0004000000055D00", DAEMOON_PLATFORM_3DS);
    CHECK_EQ_INT(fixture_write_save_file(&f, t, "main.sav", "the real save"), 0);

    /* A newline in a filename is not useful to anyone and is useful to somebody
     * writing a log line by hand. */
    entry.name = "payload/na\nme.bin";
    entry.body = "x";

    CHECK_RESULT(unpack_hostile(&f, t, k_manifest, &entry, 1), DAEMOON_ERR_ARCHIVE_ERROR);
    CHECK(fixture_save_file_exists(&f, t, "main.sav"));

    fixture_close(&f);
}

TEST_CASE(unpack_refuses_a_package_with_no_manifest)
{
    fixture_t f;
    const daemoon_title_t *t;
    hostile_entry_t entry;
    daemoon_result_t r;

    CHECK_EQ_INT(fixture_open(&f, "nomanifest"), 0);
    t = fixture_add_title(&f, "0004000000055D00", DAEMOON_PLATFORM_3DS);
    CHECK_EQ_INT(fixture_write_save_file(&f, t, "main.sav", "the real save"), 0);

    entry.name = "payload/main.sav";
    entry.body = "replacement";

    /* daemoon_archive_unpack does not read the manifest itself; the caller has
     * already verified. What matters is that the restore path refuses earlier,
     * which the manifest read covers. Here the package is structurally fine, so
     * unpacking is allowed and the caller is the one who must have checked. */
    r = unpack_hostile(&f, t, NULL, &entry, 1);
    CHECK_OK(r);

    /* The whole restore path, which is what a share code actually reaches, does
     * refuse: no manifest means nothing to verify against. */
    {
        char pkg_path[400];
        daemoon_strbuf_t sb;

        daemoon_strbuf_init(&sb, pkg_path, sizeof(pkg_path));
        daemoon_strbuf_add(&sb, f.root);
        daemoon_strbuf_add(&sb, "/hostile.zip");
        CHECK_OK(daemoon_strbuf_result(&sb));

        CHECK_RESULT(daemoon_sync_restore_package(&f.env, &f.actx, t, pkg_path),
                     DAEMOON_ERR_INVALID_MANIFEST);
    }

    fixture_close(&f);
}

TEST_CASE(restore_refuses_a_package_for_another_title)
{
    /* Restoring one game's save into another is not a recoverable mistake, and a
     * share code is exactly how a package for the wrong title turns up. */
    fixture_t f;
    const daemoon_title_t *source;
    const daemoon_title_t *target;
    unsigned char *blob = NULL;
    size_t blob_len = 0;
    char pkg_path[400];
    daemoon_strbuf_t sb;
    daemoon_stream_t *out = NULL;

    CHECK_EQ_INT(fixture_open(&f, "wrongtitle"), 0);
    source = fixture_add_title(&f, "0004000000055D00", DAEMOON_PLATFORM_3DS);
    target = fixture_add_title(&f, "0004000000030800", DAEMOON_PLATFORM_3DS);

    CHECK_EQ_INT(fixture_write_save_file(&f, source, "main.sav", "a different game"), 0);
    CHECK_EQ_INT(fixture_write_save_file(&f, target, "main.sav", "the target save"), 0);
    CHECK_OK(fixture_pack_blob(&f, source, DAEMOON_VERSION_NONE, &blob, &blob_len));

    daemoon_strbuf_init(&sb, pkg_path, sizeof(pkg_path));
    daemoon_strbuf_add(&sb, f.work);
    daemoon_strbuf_add(&sb, "/wrong.zip");
    CHECK_OK(daemoon_strbuf_result(&sb));

    CHECK_OK(f.env.fs->open(f.env.fs_ctx, pkg_path, DAEMOON_OPEN_WRITE, &out));
    CHECK_OK(daemoon_stream_write(out, blob, blob_len));
    CHECK_OK(daemoon_stream_close(out));
    free(blob);

    CHECK_RESULT(daemoon_sync_restore_package(&f.env, &f.actx, target, pkg_path),
                 DAEMOON_ERR_INVALID_MANIFEST);

    {
        char buf[64];
        CHECK_EQ_INT(fixture_read_save_file(&f, target, "main.sav", buf, sizeof(buf)), 0);
        CHECK_STR(buf, "the target save");
    }

    fixture_close(&f);
}

TEST_CASE(read_manifest_survives_a_truncated_package)
{
    /* A download cut off partway is the ordinary damaged input, and it has to come
     * back as a refusal rather than a read past the end of the file. */
    fixture_t f;
    const daemoon_title_t *t;
    unsigned char *blob = NULL;
    size_t blob_len = 0;
    size_t cut;

    CHECK_EQ_INT(fixture_open(&f, "truncated"), 0);
    t = fixture_add_title(&f, "0004000000055D00", DAEMOON_PLATFORM_3DS);
    CHECK_EQ_INT(fixture_write_save_file(&f, t, "main.sav", "some save data here"), 0);
    CHECK_OK(fixture_pack_blob(&f, t, DAEMOON_VERSION_NONE, &blob, &blob_len));

    for (cut = 1; cut < blob_len; cut += (blob_len / 8) + 1) {
        char pkg_path[400];
        daemoon_strbuf_t sb;
        daemoon_stream_t *s = NULL;
        daemoon_manifest_t m;
        daemoon_result_t r;

        daemoon_strbuf_init(&sb, pkg_path, sizeof(pkg_path));
        daemoon_strbuf_add(&sb, f.root);
        daemoon_strbuf_add(&sb, "/cut.zip");
        CHECK_OK(daemoon_strbuf_result(&sb));

        CHECK_OK(f.env.fs->open(f.env.fs_ctx, pkg_path, DAEMOON_OPEN_WRITE, &s));
        CHECK_OK(daemoon_stream_write(s, blob, cut));
        CHECK_OK(daemoon_stream_close(s));

        CHECK_OK(f.env.fs->open(f.env.fs_ctx, pkg_path, DAEMOON_OPEN_READ, &s));
        r = daemoon_archive_read_manifest(s, &m);
        (void)daemoon_stream_close(s);

        /* Any failure is fine. Success is not, unless the cut happened to land
         * after everything that matters, which a zip's trailing central directory
         * makes impossible. */
        CHECK(r != DAEMOON_OK);
    }

    free(blob);
    fixture_close(&f);
}

void test_hostile(void)
{
    printf("hostile packages\n");
    RUN(unpack_refuses_a_path_that_escapes_the_save);
    RUN(unpack_refuses_an_absolute_path);
    RUN(unpack_refuses_an_entry_outside_the_payload);
    RUN(unpack_refuses_a_control_character_in_a_path);
    RUN(unpack_refuses_a_package_with_no_manifest);
    RUN(restore_refuses_a_package_for_another_title);
    RUN(read_manifest_survives_a_truncated_package);
}
