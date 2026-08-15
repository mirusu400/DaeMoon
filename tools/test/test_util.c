#include "test.h"

#include <daemoon/util/sha256.h>
#include <daemoon/util/strbuf.h>
#include <daemoon/util/utf8.h>

#include <string.h>

TEST_CASE(sha256_vectors)
{
    char hex[DAEMOON_SHA256_HEX];

    daemoon_sha256_buf("", 0, hex);
    CHECK_STR(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    daemoon_sha256_buf("abc", 3, hex);
    CHECK_STR(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    daemoon_sha256_buf("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, hex);
    CHECK_STR(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE(sha256_streaming_matches_one_shot)
{
    /* Every digest in this project is computed while bytes stream past, so a
     * chunked update has to agree with a single call byte for byte. */
    daemoon_sha256_t c;
    unsigned char digest[DAEMOON_SHA256_DIGEST_LEN];
    char streamed[DAEMOON_SHA256_HEX];
    char oneshot[DAEMOON_SHA256_HEX];
    char data[1000];
    size_t i;

    for (i = 0; i < sizeof(data); ++i) {
        data[i] = (char)(i * 7 + 3);
    }

    daemoon_sha256_init(&c);
    for (i = 0; i < sizeof(data); i += 37) {
        size_t n = sizeof(data) - i;
        daemoon_sha256_update(&c, data + i, n < 37 ? n : 37);
    }
    daemoon_sha256_final(&c, digest);
    daemoon_sha256_hex(digest, streamed);

    daemoon_sha256_buf(data, sizeof(data), oneshot);
    CHECK_STR(streamed, oneshot);
}

TEST_CASE(sha256_hex_equal)
{
    const char *a = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    const char *b = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b856";

    CHECK(daemoon_sha256_hex_equal(a, a));
    CHECK(!daemoon_sha256_hex_equal(a, b));
    CHECK(!daemoon_sha256_hex_equal(a, ""));
    CHECK(!daemoon_sha256_hex_equal(NULL, a));
}

TEST_CASE(strbuf_overflow_is_sticky)
{
    char buf[8];
    daemoon_strbuf_t sb;

    daemoon_strbuf_init(&sb, buf, sizeof(buf));
    daemoon_strbuf_add(&sb, "abc");
    CHECK_OK(daemoon_strbuf_result(&sb));

    daemoon_strbuf_add(&sb, "defghijkl");
    CHECK_RESULT(daemoon_strbuf_result(&sb), DAEMOON_ERR_BUFFER_TOO_SMALL);
    CHECK_STR(buf, "abcdefg");

    /* Still terminated, still not past the end. */
    CHECK_EQ_INT(strlen(buf), 7);
}

TEST_CASE(strbuf_numbers_and_escaping)
{
    char buf[128];
    daemoon_strbuf_t sb;

    daemoon_strbuf_init(&sb, buf, sizeof(buf));
    daemoon_strbuf_add_uint(&sb, 0);
    daemoon_strbuf_addc(&sb, ' ');
    daemoon_strbuf_add_uint(&sb, 18446744073709551615ull);
    daemoon_strbuf_addc(&sb, ' ');
    daemoon_strbuf_add_int(&sb, -42);
    CHECK_OK(daemoon_strbuf_result(&sb));
    CHECK_STR(buf, "0 18446744073709551615 -42");

    daemoon_strbuf_reset(&sb);
    daemoon_strbuf_add_json(&sb, "a\"b\\c\nd");
    CHECK_STR(buf, "a\\\"b\\\\c\\nd");

    daemoon_strbuf_reset(&sb);
    daemoon_strbuf_add_urlenc(&sb, "a b/c?d=e");
    CHECK_STR(buf, "a%20b%2Fc%3Fd%3De");
}

TEST_CASE(strlcpy_cuts_on_a_codepoint_boundary)
{
    /* Half a multi byte sequence renders as garbage on a console and would not
     * survive a round trip through the server. */
    char buf[8];
    daemoon_result_t r;

    r = daemoon_strlcpy(buf, sizeof(buf), "日本語です");
    CHECK_RESULT(r, DAEMOON_ERR_BUFFER_TOO_SMALL);
    CHECK_STR(buf, "日本"); /* 6 bytes, not 7 */
    CHECK(daemoon_utf8_valid(buf, strlen(buf)));

    CHECK_OK(daemoon_strlcpy(buf, sizeof(buf), "short"));
    CHECK_STR(buf, "short");
}

TEST_CASE(utf8_validation)
{
    CHECK(daemoon_utf8_valid("hello", 5));
    CHECK(daemoon_utf8_valid("한국어", 9));
    CHECK(daemoon_utf8_valid("", 0));

    CHECK(!daemoon_utf8_valid("\xc3", 1));         /* truncated */
    CHECK(!daemoon_utf8_valid("\x80", 1));         /* stray continuation */
    CHECK(!daemoon_utf8_valid("\xc0\xaf", 2));     /* overlong '/' */
    CHECK(!daemoon_utf8_valid("\xed\xa0\x80", 3)); /* surrogate */
    CHECK(!daemoon_utf8_valid("\xf5\x80\x80\x80", 4)); /* above U+10FFFF */
}

TEST_CASE(utf8_length_and_width)
{
    CHECK_EQ_INT(daemoon_utf8_length("hello", 5), 5);
    CHECK_EQ_INT(daemoon_utf8_length("한국어", 9), 3);

    /* A CJK label takes twice the columns of a Latin one of the same length, which
     * is why no layout in this project may hardcode a width. */
    CHECK_EQ_INT(daemoon_utf8_width("abc", 3), 3);
    CHECK_EQ_INT(daemoon_utf8_width("한국어", 9), 6);
}

TEST_CASE(utf8_truncate)
{
    const char *s = "aé日";  /* 1 + 2 + 3 bytes */

    CHECK_EQ_INT(daemoon_utf8_truncate(s, 6, 6), 6);
    CHECK_EQ_INT(daemoon_utf8_truncate(s, 6, 5), 3);
    CHECK_EQ_INT(daemoon_utf8_truncate(s, 6, 2), 1);
    CHECK_EQ_INT(daemoon_utf8_truncate(s, 6, 0), 0);
}

void test_util(void)
{
    printf("util\n");
    RUN(sha256_vectors);
    RUN(sha256_streaming_matches_one_shot);
    RUN(sha256_hex_equal);
    RUN(strbuf_overflow_is_sticky);
    RUN(strbuf_numbers_and_escaping);
    RUN(strlcpy_cuts_on_a_codepoint_boundary);
    RUN(utf8_validation);
    RUN(utf8_length_and_width);
    RUN(utf8_truncate);
}
