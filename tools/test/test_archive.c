#define _POSIX_C_SOURCE 200809L

#include "test.h"

#include "daemoon_posix.h"
#include "fixture_env.h"
#include "fixture_json.h"
#include "posix_internal.h"

#include <daemoon/archive.h>
#include <daemoon/util/strbuf.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ tests */

TEST_CASE(payload_digest_matches_the_shared_vectors)
{
    /* shared/fixtures/payload_digest.json is also read by the Go tests, and its
     * expected values were produced by a third implementation. If this passes and
     * the Go side passes, the client and the server hash a save identically. */
    char json[8192];
    size_t len = 0;
    size_t ncases = 0;
    size_t i;

    CHECK_EQ_INT(daemoon_test_read_fixture("shared/fixtures/payload_digest.json", json,
                                           sizeof(json), &len), 0);

    ncases = fixture_digest_case_count(json, len);
    CHECK(ncases > 0);

    for (i = 0; i < ncases; ++i) {
        fixture_t f;
        const daemoon_title_t *t;
        char want_sha[DAEMOON_SHA256_HEX];
        char got_sha[DAEMOON_SHA256_HEX];
        char name[64];
        unsigned long long want_size = 0;
        unsigned long long got_size = 0;
        daemoon_save_t *save = NULL;
        size_t nentries = 0;
        size_t e;

        if (fixture_open(&f, "digest") != 0) {
            CHECK(0);
            return;
        }
        t = fixture_add_title(&f, "0004000000055D00", DAEMOON_PLATFORM_3DS);
        CHECK(t != NULL);

        CHECK_EQ_INT(fixture_digest_case(json, len, i, name, sizeof(name), want_sha,
                                              &want_size, &nentries), 0);

        for (e = 0; e < nentries; ++e) {
            char path[DAEMOON_PATH_MAX];
            char content[256];
            CHECK_EQ_INT(fixture_digest_entry(json, len, i, e, path, sizeof(path), content,
                                                   sizeof(content)), 0);
            CHECK_EQ_INT(fixture_write_save_file(&f, t, path, content), 0);
        }

        if (nentries == 0) {
            /* An empty save still has to hash to something well defined. */
            char dir[320];
            CHECK_OK(daemoon_posix_save_dir(&f.save, t, dir, sizeof(dir)));
            CHECK_OK(daemoon_posix_mkdir_p(dir));
        }

        CHECK_OK(f.env.save->open_save(f.env.save_ctx, t, &save));
        CHECK_OK(daemoon_archive_hash_save(&f.env, &f.actx, save, got_sha, &got_size));
        CHECK_OK(f.env.save->close_save(f.env.save_ctx, save));

        if (strcmp(got_sha, want_sha) != 0) {
            printf("  case %s\n", name);
        }
        CHECK_STR(got_sha, want_sha);
        CHECK_EQ_INT(got_size, want_size);

        fixture_close(&f);
    }
}

TEST_CASE(pack_verify_unpack_round_trip)
{
    fixture_t f;
    const daemoon_title_t *t;
    daemoon_save_t *save = NULL;
    daemoon_stream_t *pkg = NULL;
    daemoon_manifest_t m;
    daemoon_manifest_t read_back;
    char pkg_path[400];
    char buf[256];
    daemoon_strbuf_t sb;

    CHECK_EQ_INT(fixture_open(&f, "roundtrip"), 0);
    t = fixture_add_title(&f, "0004000000055D00", DAEMOON_PLATFORM_3DS);
    CHECK(t != NULL);

    CHECK_EQ_INT(fixture_write_save_file(&f, t, "main.sav", "hello world"), 0);
    CHECK_EQ_INT(fixture_write_save_file(&f, t, "sub/extra.bin", "more data"), 0);

    daemoon_strbuf_init(&sb, pkg_path, sizeof(pkg_path));
    daemoon_strbuf_add(&sb, f.root);
    daemoon_strbuf_add(&sb, "/pkg.zip");
    CHECK_OK(daemoon_strbuf_result(&sb));

    daemoon_manifest_init(&m);
    m.platform = t->platform;
    m.save_type = t->save_type;
    CHECK_OK(daemoon_strlcpy(m.title_id, sizeof(m.title_id), t->id));
    CHECK_OK(daemoon_strlcpy(m.device_label, sizeof(m.device_label), "test console"));
    CHECK_OK(daemoon_strlcpy(m.created_at, sizeof(m.created_at), "2026-01-01T00:00:00Z"));

    CHECK_OK(f.env.save->open_save(f.env.save_ctx, t, &save));
    CHECK_OK(f.env.fs->open(f.env.fs_ctx, pkg_path, DAEMOON_OPEN_WRITE, &pkg));
    CHECK_OK(daemoon_archive_pack(&f.env, &f.actx, save, &m, pkg));
    CHECK_OK(daemoon_stream_close(pkg));
    CHECK_OK(f.env.save->close_save(f.env.save_ctx, save));

    CHECK_EQ_INT(m.size, 20); /* 11 + 9, uncompressed payload bytes */

    /* The manifest is readable on its own, without unpacking anything. */
    CHECK_OK(f.env.fs->open(f.env.fs_ctx, pkg_path, DAEMOON_OPEN_READ, &pkg));
    CHECK_OK(daemoon_archive_read_manifest(pkg, &read_back));
    CHECK_OK(daemoon_stream_close(pkg));
    CHECK_STR(read_back.sha256, m.sha256);
    CHECK_STR(read_back.title_id, t->id);

    CHECK_OK(f.env.fs->open(f.env.fs_ctx, pkg_path, DAEMOON_OPEN_READ, &pkg));
    CHECK_OK(daemoon_archive_verify(&f.env, pkg, &read_back));
    CHECK_OK(daemoon_stream_close(pkg));

    /* Unpacking into a cleared archive brings the exact same bytes back. */
    CHECK_EQ_INT(fixture_write_save_file(&f, t, "stale.bin", "should not survive"), 0);

    CHECK_OK(f.env.save->open_save_write(f.env.save_ctx, t, &save));
    CHECK_OK(f.env.fs->open(f.env.fs_ctx, pkg_path, DAEMOON_OPEN_READ, &pkg));
    CHECK_OK(daemoon_archive_unpack(&f.env, pkg, save));
    CHECK_OK(daemoon_stream_close(pkg));
    CHECK_OK(f.env.save->commit(f.env.save_ctx, save));
    CHECK_OK(f.env.save->close_save(f.env.save_ctx, save));

    CHECK_EQ_INT(fixture_read_save_file(&f, t, "main.sav", buf, sizeof(buf)), 0);
    CHECK_STR(buf, "hello world");
    CHECK_EQ_INT(fixture_read_save_file(&f, t, "sub/extra.bin", buf, sizeof(buf)), 0);
    CHECK_STR(buf, "more data");

    /* A file the package does not contain must not survive a restore. */
    CHECK_EQ_INT(fixture_read_save_file(&f, t, "stale.bin", buf, sizeof(buf)), -1);

    CHECK_EQ_INT(f.save.commits, 1);
    fixture_close(&f);
}

TEST_CASE(verify_rejects_a_tampered_payload)
{
    fixture_t f;
    const daemoon_title_t *t;
    daemoon_save_t *save = NULL;
    daemoon_stream_t *pkg = NULL;
    daemoon_manifest_t m;
    char pkg_path[400];
    daemoon_strbuf_t sb;

    CHECK_EQ_INT(fixture_open(&f, "tamper"), 0);
    t = fixture_add_title(&f, "0004000000055D00", DAEMOON_PLATFORM_3DS);
    CHECK_EQ_INT(fixture_write_save_file(&f, t, "main.sav", "hello world"), 0);

    daemoon_strbuf_init(&sb, pkg_path, sizeof(pkg_path));
    daemoon_strbuf_add(&sb, f.root);
    daemoon_strbuf_add(&sb, "/pkg.zip");
    CHECK_OK(daemoon_strbuf_result(&sb));

    daemoon_manifest_init(&m);
    m.platform = t->platform;
    m.save_type = t->save_type;
    CHECK_OK(daemoon_strlcpy(m.title_id, sizeof(m.title_id), t->id));
    CHECK_OK(daemoon_strlcpy(m.device_label, sizeof(m.device_label), "test console"));
    CHECK_OK(daemoon_strlcpy(m.created_at, sizeof(m.created_at), "2026-01-01T00:00:00Z"));

    CHECK_OK(f.env.save->open_save(f.env.save_ctx, t, &save));
    CHECK_OK(f.env.fs->open(f.env.fs_ctx, pkg_path, DAEMOON_OPEN_WRITE, &pkg));
    CHECK_OK(daemoon_archive_pack(&f.env, &f.actx, save, &m, pkg));
    CHECK_OK(daemoon_stream_close(pkg));
    CHECK_OK(f.env.save->close_save(f.env.save_ctx, save));

    /* Claim a digest the payload does not have. This is exactly the state a
     * corrupted download leaves behind, and it must never reach a save archive. */
    CHECK_OK(daemoon_strlcpy(m.sha256, sizeof(m.sha256),
                             "0000000000000000000000000000000000000000000000000000000000000000"));

    CHECK_OK(f.env.fs->open(f.env.fs_ctx, pkg_path, DAEMOON_OPEN_READ, &pkg));
    CHECK_RESULT(daemoon_archive_verify(&f.env, pkg, &m), DAEMOON_ERR_CHECKSUM_MISMATCH);
    CHECK_OK(daemoon_stream_close(pkg));

    fixture_close(&f);
}

TEST_CASE(read_manifest_rejects_a_non_package)
{
    fixture_t f;
    daemoon_stream_t *pkg = NULL;
    daemoon_manifest_t m;
    char path[400];
    daemoon_strbuf_t sb;
    daemoon_result_t r;

    CHECK_EQ_INT(fixture_open(&f, "notzip"), 0);

    daemoon_strbuf_init(&sb, path, sizeof(path));
    daemoon_strbuf_add(&sb, f.root);
    daemoon_strbuf_add(&sb, "/junk.zip");
    CHECK_OK(daemoon_strbuf_result(&sb));

    CHECK_OK(f.env.fs->open(f.env.fs_ctx, path, DAEMOON_OPEN_WRITE, &pkg));
    CHECK_OK(daemoon_stream_write(pkg, "this is not a zip file at all", 29));
    CHECK_OK(daemoon_stream_close(pkg));

    CHECK_OK(f.env.fs->open(f.env.fs_ctx, path, DAEMOON_OPEN_READ, &pkg));
    r = daemoon_archive_read_manifest(pkg, &m);
    CHECK(r == DAEMOON_ERR_ARCHIVE_ERROR || r == DAEMOON_ERR_INVALID_MANIFEST);
    CHECK_OK(daemoon_stream_close(pkg));

    fixture_close(&f);
}

void test_archive(void)
{
    printf("archive\n");
    RUN(payload_digest_matches_the_shared_vectors);
    RUN(pack_verify_unpack_round_trip);
    RUN(verify_rejects_a_tampered_payload);
    RUN(read_manifest_rejects_a_non_package);
}
