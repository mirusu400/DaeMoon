#define _POSIX_C_SOURCE 200809L

#include "test.h"

#include <daemoon/util/strbuf.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int daemoon_test_failures = 0;
int daemoon_test_checks = 0;
const char *daemoon_test_current = "";

static char g_root[512] = ".";

const char *daemoon_test_root(void)
{
    return g_root;
}

int daemoon_test_read_fixture(const char *rel_path, char *buf, size_t cap, size_t *out_len)
{
    char path[768];
    daemoon_strbuf_t sb;
    FILE *fp;
    size_t n;

    daemoon_strbuf_init(&sb, path, sizeof(path));
    daemoon_strbuf_add(&sb, g_root);
    daemoon_strbuf_addc(&sb, '/');
    daemoon_strbuf_add(&sb, rel_path);
    if (daemoon_strbuf_result(&sb) != DAEMOON_OK) {
        return -1;
    }

    fp = fopen(path, "rb");
    if (fp == NULL) {
        printf("  cannot open fixture %s\n", path);
        return -1;
    }
    n = fread(buf, 1, cap - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    if (out_len != NULL) {
        *out_len = n;
    }
    return 0;
}

int daemoon_test_tempdir(char *buf, size_t cap, const char *tag)
{
    daemoon_strbuf_t sb;
    static unsigned counter = 0;

    daemoon_strbuf_init(&sb, buf, cap);
    daemoon_strbuf_add(&sb, "/tmp/daemoon-test-");
    daemoon_strbuf_add(&sb, tag);
    daemoon_strbuf_addc(&sb, '-');
    daemoon_strbuf_add_uint(&sb, (unsigned long long)getpid());
    daemoon_strbuf_addc(&sb, '-');
    daemoon_strbuf_add_uint(&sb, counter++);
    if (daemoon_strbuf_result(&sb) != DAEMOON_OK) {
        return -1;
    }
    if (mkdir(buf, 0755) != 0) {
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *env = getenv("DAEMOON_ROOT");

    /* Unbuffered: when a test trips the sanitizer, whatever was printed before the
     * crash is the only clue about where it got to. */
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc > 1) {
        (void)daemoon_strlcpy(g_root, sizeof(g_root), argv[1]);
    } else if (env != NULL && env[0] != '\0') {
        (void)daemoon_strlcpy(g_root, sizeof(g_root), env);
    }

    printf("daemoon core tests (root=%s)\n", g_root);

    test_util();
    test_i18n();
    test_manifest();
    test_api();
    test_archive();
    test_sync();
    test_hostile();
    test_net();
    test_fuzz();

    printf("%d checks, %d failures\n", daemoon_test_checks, daemoon_test_failures);
    return daemoon_test_failures == 0 ? 0 : 1;
}
