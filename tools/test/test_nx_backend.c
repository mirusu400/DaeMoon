/* The Switch build's desktop-checkable half.
 *
 * Most of platform/nx needs libnx and a console. These do not: a title id is a string
 * format, a config file is a parser, and a directory tree walk is stdio - and all
 * three are on the path a save takes, so they are worth checking without a Switch.
 *
 * What is deliberately absent is a libnx stub. The 3DS has one because four rounds of
 * hardware debugging paid for it; building one here before the backend has ever run on
 * hardware would be guessing at what to imitate. The conformance suite is the plan,
 * run under a selected account against a dummy title.
 */
#include "test.h"

#include "daemoon_newlib.h"
#include "../../platform/nx/source/daemoon_nx.h"

#include <daemoon/util/strbuf.h>

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------ title id */

TEST_CASE(a_switch_title_id_is_sixteen_uppercase_hex_digits)
{
    char out[DAEMOON_TITLE_ID_MAX];
    unsigned long long back = 0;

    daemoon_nx_format_title_id(0x0100000000010000ULL, out, sizeof(out));
    CHECK_STR(out, "0100000000010000");

    /* Uppercase, because the id is what a manifest is keyed by and the server
     * compares strings. A lowercase one is a different title as far as it knows. */
    daemoon_nx_format_title_id(0x01008DB008C2C000ULL, out, sizeof(out));
    CHECK_STR(out, "01008DB008C2C000");

    CHECK_OK(daemoon_nx_parse_title_id(out, &back));
    CHECK(back == 0x01008DB008C2C000ULL);

    /* Lowercase is accepted on the way in - a hand edited file should work - and
     * comes back out uppercase. */
    CHECK_OK(daemoon_nx_parse_title_id("01008db008c2c000", &back));
    CHECK(back == 0x01008DB008C2C000ULL);
}

TEST_CASE(a_short_or_wrong_title_id_is_refused_rather_than_padded)
{
    unsigned long long back = 0;

    /* Sixteen digits exactly. A short id is not a small number here: it is a manifest
     * that will not match the one the other side wrote. */
    CHECK_RESULT(daemoon_nx_parse_title_id("100", &back), DAEMOON_ERR_INVALID_REQUEST);
    CHECK_RESULT(daemoon_nx_parse_title_id("01008DB008C2C0000", &back),
                 DAEMOON_ERR_INVALID_REQUEST);
    CHECK_RESULT(daemoon_nx_parse_title_id("01008DB008C2C00G", &back),
                 DAEMOON_ERR_INVALID_REQUEST);
    CHECK_RESULT(daemoon_nx_parse_title_id("", &back), DAEMOON_ERR_INVALID_REQUEST);
    CHECK_RESULT(daemoon_nx_parse_title_id(NULL, &back), DAEMOON_ERR_INVALID_REQUEST);
}

/* --------------------------------------------------------------------- config */

TEST_CASE(the_switch_config_reads_and_writes_the_same_file_shape_as_the_3ds)
{
    char root[256];
    char path[320];
    daemoon_nx_config_t out;
    daemoon_nx_config_t in;

    CHECK_EQ_INT(daemoon_test_tempdir(root, sizeof(root), "nx-config"), 0);
    (void)snprintf(path, sizeof(path), "%s/config.txt", root);

    daemoon_nx_config_defaults(&out);
    CHECK_STR(out.device_label, "Switch");
    /* Nothing to sync with until both halves are there. */
    CHECK(!daemoon_nx_config_can_sync(&out));

    (void)daemoon_strlcpy(out.server_url, sizeof(out.server_url),
                          "https://192.168.1.13:8443/");
    (void)daemoon_strlcpy(out.token, sizeof(out.token), "MCRV_abc-123_XYZ");
    (void)daemoon_strlcpy(out.device_label, sizeof(out.device_label), "거실 스위치");
    (void)daemoon_strlcpy(out.language, sizeof(out.language), "ko");

    CHECK_OK(daemoon_nx_config_save(path, &out));
    CHECK_OK(daemoon_nx_config_load(path, &in));

    /* The trailing slash is gone: it becomes a double slash in every path built from
     * it, and some servers answer those differently. */
    CHECK_STR(in.server_url, "https://192.168.1.13:8443");
    CHECK_STR(in.token, "MCRV_abc-123_XYZ");
    CHECK_STR(in.device_label, "거실 스위치");
    CHECK_STR(in.language, "ko");
    CHECK(daemoon_nx_config_can_sync(&in));
}

TEST_CASE(a_missing_switch_config_is_not_a_failure)
{
    daemoon_nx_config_t cfg;

    /* A console that has not been pointed at a server yet. Everything local still
     * works, which is the whole of Phase 1 on either platform. */
    CHECK_RESULT(daemoon_nx_config_load("/nonexistent/nowhere/config.txt", &cfg),
                 DAEMOON_ERR_NOT_FOUND);
    CHECK_STR(cfg.device_label, "Switch");
    CHECK(!daemoon_nx_config_can_sync(&cfg));
}

TEST_CASE(a_language_the_build_does_not_know_is_ignored_rather_than_stored)
{
    char root[256];
    char path[320];
    daemoon_nx_config_t cfg;
    FILE *fp;

    CHECK_EQ_INT(daemoon_test_tempdir(root, sizeof(root), "nx-config-lang"), 0);
    (void)snprintf(path, sizeof(path), "%s/config.txt", root);

    fp = fopen(path, "wb");
    CHECK(fp != NULL);
    /* A comment, a blank line, an unknown key, a line with no equals, and a language
     * this build has no table for. All of it survivable: the file is hand edited. */
    (void)fprintf(fp, "# a comment\n\nserver = http://host:8080\nnonsense\n"
                      "unknown = 1\nlanguage = klingon\n");
    (void)fclose(fp);

    CHECK_OK(daemoon_nx_config_load(path, &cfg));
    CHECK_STR(cfg.server_url, "http://host:8080");
    /* Empty rather than "klingon": a stored code the build cannot resolve would be a
     * console that silently ignores the setting and shows English anyway. */
    CHECK_STR(cfg.language, "");
}

/* ------------------------------------------------------------------ dir tree */

typedef struct {
    char  seen[16][DAEMOON_PATH_MAX];
    size_t n;
    unsigned long long bytes;
} walk_t;

static int walk_cb(void *user, const char *path, unsigned long long size)
{
    walk_t *w = (walk_t *)user;

    if (w->n < 16) {
        (void)daemoon_strlcpy(w->seen[w->n], sizeof(w->seen[w->n]), path);
        ++w->n;
    }
    w->bytes += size;
    return 0;
}

static int saw(const walk_t *w, const char *path)
{
    size_t i;

    for (i = 0; i < w->n; ++i) {
        if (strcmp(w->seen[i], path) == 0) {
            return 1;
        }
    }
    return 0;
}

static void write_at(const char *root, const char *rel, const char *body)
{
    char path[512];
    char dir[512];
    char *slash;
    FILE *fp;

    (void)snprintf(path, sizeof(path), "%s/%s", root, rel);
    (void)daemoon_strlcpy(dir, sizeof(dir), path);
    slash = strrchr(dir, '/');
    if (slash != NULL) {
        *slash = '\0';
        (void)daemoon_fs_newlib_backend.mkdir_p(NULL, dir);
    }
    fp = fopen(path, "wb");
    if (fp != NULL) {
        (void)fputs(body, fp);
        (void)fclose(fp);
    }
}

/* A mounted Switch save is an ordinary directory, so this is the walk that lists one
 * for packing and the clear that empties one before a restore. Both are on the path a
 * save takes, and both are stdio - so neither needs a console. */
TEST_CASE(a_save_shaped_directory_walks_with_relative_forward_slash_paths)
{
    char root[256];
    walk_t w;

    CHECK_EQ_INT(daemoon_test_tempdir(root, sizeof(root), "nx-walk"), 0);
    write_at(root, "main.sav", "0123456789");
    write_at(root, "sub/inner.dat", "abc");
    write_at(root, "sub/deeper/leaf.bin", "xy");

    memset(&w, 0, sizeof(w));
    CHECK_OK(daemoon_dir_walk(root, walk_cb, &w));

    CHECK_EQ_INT((int)w.n, 3);
    /* Relative to the root and forward slashed, which is what daemoon_entry_cb is
     * specified to receive and what goes into a package. An absolute path here would
     * put somebody's directory layout inside a zip. */
    CHECK(saw(&w, "main.sav"));
    CHECK(saw(&w, "sub/inner.dat"));
    CHECK(saw(&w, "sub/deeper/leaf.bin"));
    CHECK(w.bytes == 15);
}

TEST_CASE(clearing_a_save_shaped_directory_empties_it_and_keeps_the_root)
{
    char root[256];
    walk_t w;
    struct stat st;

    CHECK_EQ_INT(daemoon_test_tempdir(root, sizeof(root), "nx-clear"), 0);
    write_at(root, "main.sav", "0123456789");
    write_at(root, "sub/inner.dat", "abc");
    write_at(root, "sub/deeper/leaf.bin", "xy");

    CHECK_OK(daemoon_dir_remove_all(root));

    /* Nested files go, not only the ones at the top. A restore that left a file
     * behind in a subdirectory would be a save with somebody else's data in it, and
     * the 3DS conformance suite has a case for exactly this. */
    memset(&w, 0, sizeof(w));
    CHECK_OK(daemoon_dir_walk(root, walk_cb, &w));
    CHECK_EQ_INT((int)w.n, 0);

    /* The archive is being cleared, not deleted. Removing the root would be removing
     * the save. */
    CHECK_EQ_INT(stat(root, &st), 0);
    CHECK(S_ISDIR(st.st_mode));
}

TEST_CASE(walking_something_that_is_not_there_says_so)
{
    walk_t w;

    memset(&w, 0, sizeof(w));
    CHECK_RESULT(daemoon_dir_walk("/nonexistent/nowhere", walk_cb, &w),
                 DAEMOON_ERR_NOT_FOUND);
    CHECK_EQ_INT((int)w.n, 0);

    /* Clearing one, though, is already the state being asked for. */
    CHECK_OK(daemoon_dir_remove_all("/nonexistent/nowhere"));
}

void test_nx_backend(void)
{
    printf("switch backend (the half that needs no console)\n");
    RUN(a_switch_title_id_is_sixteen_uppercase_hex_digits);
    RUN(a_short_or_wrong_title_id_is_refused_rather_than_padded);
    RUN(the_switch_config_reads_and_writes_the_same_file_shape_as_the_3ds);
    RUN(a_missing_switch_config_is_not_a_failure);
    RUN(a_language_the_build_does_not_know_is_ignored_rather_than_stored);
    RUN(a_save_shaped_directory_walks_with_relative_forward_slash_paths);
    RUN(clearing_a_save_shaped_directory_empties_it_and_keeps_the_root);
    RUN(walking_something_that_is_not_there_says_so);
}
