/* A test harness small enough to read in one sitting.
 *
 * No framework: the core tests have to build with the same compiler set that
 * builds for devkitARM, and a dependency here would be a dependency there. */
#ifndef DAEMOON_TEST_H
#define DAEMOON_TEST_H

#include <daemoon/result.h>

#include <stdio.h>
#include <string.h>

extern int daemoon_test_failures;
extern int daemoon_test_checks;
extern const char *daemoon_test_current;

#define TEST_CASE(name) static void name(void)

#define RUN(fn)                                  \
    do {                                         \
        daemoon_test_current = #fn;              \
        fn();                                    \
    } while (0)

#define CHECK(cond)                                                              \
    do {                                                                         \
        daemoon_test_checks++;                                                   \
        if (!(cond)) {                                                           \
            daemoon_test_failures++;                                             \
            printf("  FAIL %s:%d in %s: %s\n", __FILE__, __LINE__,               \
                   daemoon_test_current, #cond);                                 \
        }                                                                        \
    } while (0)

#define CHECK_EQ_INT(got, want)                                                  \
    do {                                                                         \
        long long got_ = (long long)(got);                                       \
        long long want_ = (long long)(want);                                     \
        daemoon_test_checks++;                                                   \
        if (got_ != want_) {                                                     \
            daemoon_test_failures++;                                             \
            printf("  FAIL %s:%d in %s: %s == %lld, want %lld\n", __FILE__,      \
                   __LINE__, daemoon_test_current, #got, got_, want_);           \
        }                                                                        \
    } while (0)

/* Results print as their wire code, which is far more readable than a number and
 * is also what would appear in a server log. */
#define CHECK_RESULT(got, want)                                                  \
    do {                                                                         \
        daemoon_result_t got_ = (got);                                           \
        daemoon_result_t want_ = (want);                                         \
        daemoon_test_checks++;                                                   \
        if (got_ != want_) {                                                     \
            daemoon_test_failures++;                                             \
            printf("  FAIL %s:%d in %s: %s == %s, want %s\n", __FILE__, __LINE__,\
                   daemoon_test_current, #got, daemoon_result_code(got_),        \
                   daemoon_result_code(want_));                                  \
        }                                                                        \
    } while (0)

#define CHECK_OK(expr) CHECK_RESULT(expr, DAEMOON_OK)

#define CHECK_STR(got, want)                                                     \
    do {                                                                         \
        const char *got_ = (got);                                                \
        const char *want_ = (want);                                              \
        daemoon_test_checks++;                                                   \
        if (got_ == NULL || strcmp(got_, want_) != 0) {                          \
            daemoon_test_failures++;                                             \
            printf("  FAIL %s:%d in %s: %s == \"%s\", want \"%s\"\n", __FILE__,  \
                   __LINE__, daemoon_test_current, #got,                         \
                   got_ != NULL ? got_ : "(null)", want_);                       \
        }                                                                        \
    } while (0)

/* Suites. Each lives in its own file and is called from test_main.c. */
void test_util(void);
void test_i18n(void);
void test_manifest(void);
void test_api(void);
void test_archive(void);
void test_sync(void);
void test_hostile(void);

/* Repository root, so a test can read shared/fixtures/. Set from argv or the
 * DAEMOON_ROOT environment variable. */
const char *daemoon_test_root(void);

/* Reads a whole file under the repository root. Returns 0 on success. */
int daemoon_test_read_fixture(const char *rel_path, char *buf, size_t cap, size_t *out_len);

/* A unique scratch directory under the system temp dir, removed by the caller with
 * daemoon_posix_rmtree. */
int daemoon_test_tempdir(char *buf, size_t cap, const char *tag);

#endif /* DAEMOON_TEST_H */
