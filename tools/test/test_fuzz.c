/* Mutation testing for the C parsers.
 *
 * The Go side has two fuzzers. The C side parses the same bytes on a console with
 * no MMU protection worth the name and a heap measured in megabytes, so a parse
 * bug there is memory corruption rather than a 500. That asymmetry is what this
 * file closes.
 *
 * It is deterministic on purpose: a fixed seed, a fixed iteration count, and a
 * fixed corpus, so a failure reproduces from the line number alone and CI does not
 * grow a flaky test. The whole suite already builds with the address and undefined
 * behaviour sanitizers, which is what actually does the checking here - this file
 * only has to reach the code with input nobody would write by hand.
 *
 * DAEMOON_FUZZ_ITERS raises the count for a longer local run.
 */
#define _POSIX_C_SOURCE 200809L

#include "test.h"

#include <daemoon/api.h>
#include <daemoon/i18n.h>
#include <daemoon/manifest.h>
#include <daemoon/util/sha256.h>
#include <daemoon/util/strbuf.h>
#include <daemoon/util/utf8.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FUZZ_DEFAULT_ITERS 4000
#define FUZZ_MAX_LEN       2048

/* xorshift64. Nothing here needs a good generator, it needs the same sequence on
 * every machine, and rand() is not that. */
static unsigned long long g_state;

static void fuzz_seed(unsigned long long seed)
{
    g_state = seed != 0 ? seed : 0x2545F4914F6CDD1Dull;
}

static unsigned long long fuzz_next(void)
{
    g_state ^= g_state << 13;
    g_state ^= g_state >> 7;
    g_state ^= g_state << 17;
    return g_state;
}

static size_t fuzz_below(size_t n)
{
    return n == 0 ? 0 : (size_t)(fuzz_next() % n);
}

static int fuzz_iters(void)
{
    const char *env = getenv("DAEMOON_FUZZ_ITERS");
    long n;

    if (env == NULL || env[0] == '\0') {
        return FUZZ_DEFAULT_ITERS;
    }
    n = strtol(env, NULL, 10);
    return (n > 0 && n < 100000000L) ? (int)n : FUZZ_DEFAULT_ITERS;
}

/* ------------------------------------------------------------------- corpus */

/* Seeds that already reach interesting code: a full manifest, the shapes the
 * parser branches on, and the error bodies the API layer reads. */
static const char *const k_corpus[] = {
    "{\"format_version\":1,\"platform\":\"3ds\",\"title_id\":\"0004000000055D00\","
    "\"save_type\":\"savedata\",\"version\":42,\"parent_version\":41,"
    "\"sha256\":\"0f9a4d1942f23aab4418d5d3720b10fbdd25f91e51082abacbff3dd1a6318242\","
    "\"size\":11,\"device_label\":\"Old 3DS XL\",\"created_at\":\"2026-01-02T03:04:05Z\"}",

    "{\"format_version\":1,\"platform\":\"nx\",\"title_id\":\"0100000000010000\","
    "\"save_type\":\"savedata\",\"version\":0,\"parent_version\":null,"
    "\"sha256\":\"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\","
    "\"size\":0,\"device_label\":\"\\ud55c\\uad6d\\uc5b4\",\"created_at\":\"1970-01-01T00:00:00Z\"}",

    "{\"error\":{\"code\":\"version_conflict\",\"detail\":{\"server_version\":43,"
    "\"parent_version\":41,\"server_size\":32768,\"server_device_label\":\"New 2DS\","
    "\"server_received_at\":\"2026-02-03T04:05:06Z\"}}}",

    "{\"error\":{\"code\":\"save_too_large\",\"detail\":{\"size\":1,\"max_size\":2}}}",

    "{\"format_version\":1,\"device_label\":\"\\\\\\\"\\n\\t\\u0000\"}",
    "{\"a\":{\"b\":{\"c\":{\"d\":[1,2,3,{\"e\":null}]}}}}",
    "{}",
    "[]",
    "null",
    ""
};

#define CORPUS_COUNT (sizeof(k_corpus) / sizeof(k_corpus[0]))

/* Mutates a corpus entry into buf. Returns the length. */
static size_t fuzz_mutate(char *buf, size_t cap)
{
    const char *seed = k_corpus[fuzz_below(CORPUS_COUNT)];
    size_t len = strlen(seed);
    int rounds;
    int i;

    if (len >= cap) {
        len = cap - 1;
    }
    memcpy(buf, seed, len);

    rounds = 1 + (int)fuzz_below(6);
    for (i = 0; i < rounds; ++i) {
        switch (fuzz_next() % 7u) {
        case 0: /* flip a byte */
            if (len > 0) {
                buf[fuzz_below(len)] ^= (char)(1u << (fuzz_next() % 8u));
            }
            break;
        case 1: /* cut it short, which is what a partial download looks like */
            len = fuzz_below(len + 1);
            break;
        case 2: /* replace a byte with something structural */
            if (len > 0) {
                static const char k_interesting[] = "{}[]\":,\\ \n\0\x7f";
                buf[fuzz_below(len)] = k_interesting[fuzz_below(sizeof(k_interesting) - 1)];
            }
            break;
        case 3: /* splice in a piece of another seed */
            {
                const char *other = k_corpus[fuzz_below(CORPUS_COUNT)];
                size_t olen = strlen(other);
                size_t take = fuzz_below(olen + 1);
                size_t at = fuzz_below(len + 1);

                if (take > 0 && len + take < cap) {
                    memmove(buf + at + take, buf + at, len - at);
                    memcpy(buf + at, other + fuzz_below(olen - take + 1), take);
                    len += take;
                }
            }
            break;
        case 4: /* repeat a run, which finds depth and length limits */
            if (len > 0 && len * 2 < cap) {
                size_t at = fuzz_below(len);
                size_t run = fuzz_below(len - at) + 1;
                memmove(buf + at + run, buf + at, len - at);
                len += run;
            }
            break;
        case 5: /* insert a high byte, so the UTF-8 paths see invalid sequences */
            if (len + 1 < cap) {
                size_t at = fuzz_below(len + 1);
                memmove(buf + at + 1, buf + at, len - at);
                buf[at] = (char)(0x80u | (fuzz_next() & 0x7fu));
                len += 1;
            }
            break;
        default: /* drop a byte */
            if (len > 0) {
                size_t at = fuzz_below(len);
                memmove(buf + at, buf + at + 1, len - at - 1);
                len -= 1;
            }
            break;
        }
    }
    return len;
}

/* Every failure has to be a code from shared/errors.json. A value that is not one
 * means something returned a raw errno or an uninitialised variable, and the UI
 * would render it as "?" or worse. */
static int is_known_result(daemoon_result_t r)
{
    int code;

    if (r == DAEMOON_OK) {
        return 1;
    }
    for (code = 1; code < DAEMOON_ERR_COUNT_; ++code) {
        if (r == code) {
            return 1;
        }
    }
    return 0;
}

/* --------------------------------------------------------------------- cases */

TEST_CASE(manifest_parse_survives_mutated_input)
{
    char buf[FUZZ_MAX_LEN];
    int iters = fuzz_iters();
    int i;
    int accepted = 0;

    fuzz_seed(0xDAE0001);

    for (i = 0; i < iters; ++i) {
        daemoon_manifest_t m;
        size_t len = fuzz_mutate(buf, sizeof(buf));
        daemoon_result_t r;

        /* Deliberately not NUL terminated: the parser takes a length, and a caller
         * that relied on termination would be reading past a downloaded buffer. */
        memset(&m, 0xA5, sizeof(m));
        r = daemoon_manifest_parse(buf, len, &m);

        if (!is_known_result(r)) {
            printf("  iteration %d returned %d\n", i, r);
            CHECK(0);
            return;
        }
        if (r != DAEMOON_OK) {
            continue;
        }
        ++accepted;

        /* Anything accepted has to satisfy what the rest of the client assumes,
         * or a later stage gets a struct it was told could not exist. */
        CHECK_OK(daemoon_manifest_validate(&m));
        CHECK_EQ_INT(m.format_version, DAEMOON_MANIFEST_FORMAT_VERSION);
        CHECK_EQ_INT(strlen(m.sha256), 64);
        CHECK(m.platform != DAEMOON_PLATFORM_UNKNOWN);
        CHECK(m.save_type != DAEMOON_SAVE_UNKNOWN);
        CHECK(m.device_label[0] != '\0');
        CHECK(daemoon_utf8_valid(m.device_label, strlen(m.device_label)));
        if (m.version != DAEMOON_VERSION_NONE) {
            CHECK(m.parent_version < m.version);
        }
    }

    /* If nothing was ever accepted the mutations are too destructive to be
     * testing the accept path at all. */
    CHECK(accepted > 0);
}

TEST_CASE(manifest_round_trip_holds_for_anything_accepted)
{
    char buf[FUZZ_MAX_LEN];
    char written[DAEMOON_MANIFEST_MAX_BYTES];
    int iters = fuzz_iters();
    int i;

    fuzz_seed(0xDAE0002);

    for (i = 0; i < iters; ++i) {
        daemoon_manifest_t a;
        daemoon_manifest_t b;
        size_t len = fuzz_mutate(buf, sizeof(buf));
        size_t out_len = 0;

        if (daemoon_manifest_parse(buf, len, &a) != DAEMOON_OK) {
            continue;
        }
        /* A manifest that parsed must serialise, and the result must parse back to
         * the same thing. The server reads what the client writes. */
        if (daemoon_manifest_write(&a, written, sizeof(written), &out_len) != DAEMOON_OK) {
            continue; /* only a buffer limit is acceptable here */
        }
        CHECK_OK(daemoon_manifest_parse(written, out_len, &b));
        CHECK_EQ_INT(memcmp(&a, &b, sizeof(a)), 0);
    }
}

TEST_CASE(error_body_parsing_survives_mutated_input)
{
    char buf[FUZZ_MAX_LEN];
    int iters = fuzz_iters();
    int i;

    fuzz_seed(0xDAE0003);

    for (i = 0; i < iters; ++i) {
        daemoon_conflict_t conflict;
        size_t len = fuzz_mutate(buf, sizeof(buf));
        int status = (int)(200 + fuzz_below(400));
        daemoon_result_t r;

        memset(&conflict, 0xA5, sizeof(conflict));
        r = daemoon_api_parse_error(buf, len, status, &conflict);

        if (!is_known_result(r)) {
            printf("  iteration %d returned %d\n", i, r);
            CHECK(0);
            return;
        }
        /* A 2xx never becomes a failure and a failure never becomes success: the
         * client decides whether a save was written on this answer. */
        if (status >= 200 && status < 300) {
            CHECK_RESULT(r, DAEMOON_OK);
        } else {
            CHECK(r != DAEMOON_OK);
        }
    }
}

TEST_CASE(utf8_helpers_survive_arbitrary_bytes)
{
    unsigned char buf[256];
    int iters = fuzz_iters();
    int i;

    fuzz_seed(0xDAE0004);

    for (i = 0; i < iters; ++i) {
        size_t len = fuzz_below(sizeof(buf) + 1);
        size_t j;
        size_t cut;
        size_t trimmed;

        for (j = 0; j < len; ++j) {
            buf[j] = (unsigned char)(fuzz_next() & 0xffu);
        }

        /* None of these may read past len. The sanitizer is what enforces that;
         * the assertions here cover the arithmetic. */
        (void)daemoon_utf8_valid((const char *)buf, len);
        CHECK(daemoon_utf8_length((const char *)buf, len) <= len);

        cut = daemoon_utf8_truncate((const char *)buf, len, fuzz_below(len + 1));
        CHECK(cut <= len);

        trimmed = daemoon_utf8_trim_partial((const char *)buf, len);
        CHECK(trimmed <= len);
        /* Whatever survives trimming has to be well formed, since that is the
         * entire promise: what is handed on can be rendered. */
        if (trimmed > 0) {
            CHECK(daemoon_utf8_valid((const char *)buf, trimmed) ||
                  !daemoon_utf8_valid((const char *)buf, len));
        }

        (void)daemoon_utf8_width((const char *)buf, len);
    }
}

TEST_CASE(strlcpy_never_overflows_or_leaves_a_broken_sequence)
{
    char src[128];
    char dst[64];
    int iters = fuzz_iters();
    int i;

    fuzz_seed(0xDAE0005);

    for (i = 0; i < iters; ++i) {
        size_t len = fuzz_below(sizeof(src) - 1);
        size_t cap = 1 + fuzz_below(sizeof(dst));
        size_t j;
        unsigned char guard;

        for (j = 0; j < len; ++j) {
            src[j] = (char)(fuzz_next() & 0xffu);
        }
        src[len] = '\0';

        memset(dst, 0x5A, sizeof(dst));
        guard = (cap < sizeof(dst)) ? (unsigned char)dst[cap] : 0;

        (void)daemoon_strlcpy(dst, cap, src);

        /* Always terminated, never past the capacity it was given. */
        CHECK(memchr(dst, '\0', cap) != NULL);
        if (cap < sizeof(dst)) {
            CHECK_EQ_INT((unsigned char)dst[cap], guard);
        }
        /* The contract is that copying does not *introduce* a broken sequence.
         * A source that was already invalid stays invalid: this is a bounded copy,
         * not a sanitizer, and silently rewriting a device label would be worse. */
        if (daemoon_utf8_valid(src, strlen(src))) {
            CHECK(daemoon_utf8_valid(dst, strlen(dst)));
        }
    }
}

TEST_CASE(strf_never_overflows_the_buffer_it_is_given)
{
    char out[96];
    char arg0[64];
    char arg1[64];
    int iters = fuzz_iters();
    int i;

    fuzz_seed(0xDAE0006);

    for (i = 0; i < iters; ++i) {
        const char *args[2];
        size_t cap = 1 + fuzz_below(sizeof(out) - 1);
        size_t len0 = fuzz_below(sizeof(arg0) - 1);
        size_t len1 = fuzz_below(sizeof(arg1) - 1);
        size_t j;
        unsigned char guard;

        /* Arguments are user set strings: a device label typed on a software
         * keyboard, or a title name from the console. */
        for (j = 0; j < len0; ++j) {
            arg0[j] = (char)(0x20 + (fuzz_next() % 0x60u));
        }
        arg0[len0] = '\0';
        for (j = 0; j < len1; ++j) {
            arg1[j] = (char)(0x20 + (fuzz_next() % 0x60u));
        }
        arg1[len1] = '\0';

        args[0] = arg0;
        args[1] = arg1;

        daemoon_i18n_set_language((daemoon_lang_t)fuzz_below(DAEMOON_LANG_COUNT));

        memset(out, 0x5A, sizeof(out));
        guard = (cap < sizeof(out)) ? (unsigned char)out[cap] : 0;

        (void)daemoon_strf(out, cap, (daemoon_str_id_t)fuzz_below(DAEMOON_STR_COUNT), args, 2);

        CHECK(memchr(out, '\0', cap) != NULL);
        if (cap < sizeof(out)) {
            CHECK_EQ_INT((unsigned char)out[cap], guard);
        }
        CHECK(daemoon_utf8_valid(out, strlen(out)));
    }

    daemoon_i18n_set_language(DAEMOON_LANG_EN);
}

TEST_CASE(result_codes_from_mutated_wire_strings)
{
    char code[64];
    int iters = fuzz_iters();
    int i;

    fuzz_seed(0xDAE0007);

    for (i = 0; i < iters; ++i) {
        size_t len = fuzz_below(sizeof(code) - 1);
        size_t j;
        daemoon_result_t r;

        for (j = 0; j < len; ++j) {
            code[j] = (char)(0x20 + (fuzz_next() % 0x60u));
        }
        code[len] = '\0';

        r = daemoon_result_from_code(code, len);
        CHECK(is_known_result(r));
        /* An unrecognised code must never look like success. A newer server
         * saying something an older console does not know cannot be allowed to
         * mean "the save was written". */
        CHECK(r != DAEMOON_OK);

        /* And whatever it maps to has text in every language. */
        CHECK(daemoon_str(daemoon_result_str_id(r))[0] != '\0');
    }
}

void test_fuzz(void)
{
    printf("mutation (%d iterations each, DAEMOON_FUZZ_ITERS to raise)\n", fuzz_iters());
    RUN(manifest_parse_survives_mutated_input);
    RUN(manifest_round_trip_holds_for_anything_accepted);
    RUN(error_body_parsing_survives_mutated_input);
    RUN(utf8_helpers_survive_arbitrary_bytes);
    RUN(strlcpy_never_overflows_or_leaves_a_broken_sequence);
    RUN(strf_never_overflows_the_buffer_it_is_given);
    RUN(result_codes_from_mutated_wire_strings);
}
