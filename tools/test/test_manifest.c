#include "test.h"

#include <daemoon/manifest.h>
#include <daemoon/util/strbuf.h>

#include <string.h>

TEST_CASE(parses_the_shared_fixture)
{
    char json[DAEMOON_MANIFEST_MAX_BYTES];
    daemoon_manifest_t m;
    size_t len = 0;

    /* The same file is parsed by the Go tests. If the two ever disagree about a
     * manifest, one of the suites fails here instead of a save being written back
     * wrong. */
    CHECK_EQ_INT(daemoon_test_read_fixture("shared/fixtures/manifest_valid.json", json,
                                           sizeof(json), &len), 0);
    CHECK_OK(daemoon_manifest_parse(json, len, &m));

    CHECK_EQ_INT(m.format_version, 1);
    CHECK_EQ_INT(m.platform, DAEMOON_PLATFORM_3DS);
    CHECK_STR(m.title_id, "0004000000055D00");
    CHECK_EQ_INT(m.save_type, DAEMOON_SAVE_SAVEDATA);
    CHECK_EQ_INT(m.version, 42);
    CHECK_EQ_INT(m.parent_version, 41);
    CHECK_STR(m.sha256, "0f9a4d1942f23aab4418d5d3720b10fbdd25f91e51082abacbff3dd1a6318242");
    CHECK_EQ_INT(m.size, 11);
    CHECK_STR(m.device_label, "Old 3DS XL");
    CHECK_STR(m.created_at, "2026-01-02T03:04:05Z");
}

TEST_CASE(parses_a_first_upload)
{
    char json[DAEMOON_MANIFEST_MAX_BYTES];
    daemoon_manifest_t m;
    size_t len = 0;

    CHECK_EQ_INT(daemoon_test_read_fixture("shared/fixtures/manifest_first_upload.json", json,
                                           sizeof(json), &len), 0);
    CHECK_OK(daemoon_manifest_parse(json, len, &m));

    CHECK_EQ_INT(m.platform, DAEMOON_PLATFORM_NX);
    CHECK_EQ_INT(m.version, DAEMOON_VERSION_NONE);
    CHECK_EQ_INT(m.parent_version, DAEMOON_VERSION_NONE);
    /* A non ASCII device label has to survive intact: it is shown in the conflict
     * dialog on the other console. */
    CHECK_STR(m.device_label, "리빙룸 스위치");
}

TEST_CASE(parses_a_secure_value_package)
{
    char json[DAEMOON_MANIFEST_MAX_BYTES];
    char out[DAEMOON_MANIFEST_MAX_BYTES];
    daemoon_manifest_t m;
    daemoon_manifest_t back;
    size_t len = 0;
    size_t out_len = 0;

    /* The server refused this file as an unknown field once, and every 3DS title
     * with a secure value failed to sync with a manifest error. Both suites read
     * it now, so the next field added on one side fails here. */
    CHECK_EQ_INT(daemoon_test_read_fixture("shared/fixtures/manifest_secure_value.json", json,
                                           sizeof(json), &len), 0);
    CHECK_OK(daemoon_manifest_parse(json, len, &m));

    CHECK_EQ_INT(m.has_secure_value, 1);
    CHECK(m.secure_value == 0xffffffffffffffffull);
    CHECK_STR(m.title_name, "포켓몬스터");

    /* And it survives being written back out: a restore reads the value from the
     * package it downloaded, not from the one it packed. */
    CHECK_OK(daemoon_manifest_write(&m, out, sizeof(out), &out_len));
    CHECK_OK(daemoon_manifest_parse(out, out_len, &back));
    CHECK_EQ_INT(back.has_secure_value, 1);
    CHECK(back.secure_value == m.secure_value);
}

TEST_CASE(rejects_a_newer_format_version)
{
    char json[DAEMOON_MANIFEST_MAX_BYTES];
    daemoon_manifest_t m;
    size_t len = 0;

    /* Guessing at a layout this build does not know is how a save gets written
     * back wrong. An unreadable package is recoverable; a misread one is not. */
    CHECK_EQ_INT(daemoon_test_read_fixture("shared/fixtures/manifest_future_format.json", json,
                                           sizeof(json), &len), 0);
    CHECK_RESULT(daemoon_manifest_parse(json, len, &m), DAEMOON_ERR_INVALID_MANIFEST);
}

TEST_CASE(rejects_a_parent_that_is_not_older)
{
    char json[DAEMOON_MANIFEST_MAX_BYTES];
    daemoon_manifest_t m;
    size_t len = 0;

    /* Versions are issued by the server and strictly increase. */
    CHECK_EQ_INT(daemoon_test_read_fixture("shared/fixtures/manifest_bad_parent.json", json,
                                           sizeof(json), &len), 0);
    CHECK_RESULT(daemoon_manifest_parse(json, len, &m), DAEMOON_ERR_INVALID_MANIFEST);
}

TEST_CASE(rejects_malformed_input)
{
    daemoon_manifest_t m;
    const char *truncated = "{\"format_version\":1,\"platform\":\"3ds\"";
    const char *not_object = "[1,2,3]";
    const char *empty = "";

    CHECK_RESULT(daemoon_manifest_parse(truncated, 0, &m), DAEMOON_ERR_PARSE_ERROR);
    CHECK_RESULT(daemoon_manifest_parse(not_object, 0, &m), DAEMOON_ERR_INVALID_MANIFEST);
    CHECK_RESULT(daemoon_manifest_parse(empty, 1, &m), DAEMOON_ERR_PARSE_ERROR);
}

TEST_CASE(rejects_a_bad_digest_field)
{
    daemoon_manifest_t m;
    const char *upper_hex =
        "{\"format_version\":1,\"platform\":\"3ds\",\"title_id\":\"0004000000055D00\","
        "\"save_type\":\"savedata\",\"version\":1,\"parent_version\":null,"
        "\"sha256\":\"0F9A4D1942F23AAB4418D5D3720B10FBDD25F91E51082ABACBFF3DD1A6318242\","
        "\"size\":11,\"device_label\":\"x\",\"created_at\":\"1970-01-01T00:00:00Z\"}";
    const char *too_short =
        "{\"format_version\":1,\"platform\":\"3ds\",\"title_id\":\"0004000000055D00\","
        "\"save_type\":\"savedata\",\"version\":1,\"parent_version\":null,"
        "\"sha256\":\"abc\",\"size\":11,\"device_label\":\"x\","
        "\"created_at\":\"1970-01-01T00:00:00Z\"}";

    /* Digests are compared as text, so one canonical spelling only. */
    CHECK_RESULT(daemoon_manifest_parse(upper_hex, 0, &m), DAEMOON_ERR_INVALID_MANIFEST);
    CHECK_RESULT(daemoon_manifest_parse(too_short, 0, &m), DAEMOON_ERR_INVALID_MANIFEST);
}

TEST_CASE(round_trips)
{
    char json[DAEMOON_MANIFEST_MAX_BYTES];
    char out[DAEMOON_MANIFEST_MAX_BYTES];
    daemoon_manifest_t a;
    daemoon_manifest_t b;
    size_t len = 0;
    size_t out_len = 0;

    CHECK_EQ_INT(daemoon_test_read_fixture("shared/fixtures/manifest_valid.json", json,
                                           sizeof(json), &len), 0);
    CHECK_OK(daemoon_manifest_parse(json, len, &a));
    CHECK_OK(daemoon_manifest_write(&a, out, sizeof(out), &out_len));
    CHECK_OK(daemoon_manifest_parse(out, out_len, &b));

    CHECK_EQ_INT(memcmp(&a, &b, sizeof(a)), 0);
}

TEST_CASE(write_escapes_the_device_label)
{
    char out[DAEMOON_MANIFEST_MAX_BYTES];
    daemoon_manifest_t m;
    daemoon_manifest_t back;
    size_t out_len = 0;

    daemoon_manifest_init(&m);
    m.platform = DAEMOON_PLATFORM_NDS;
    m.save_type = DAEMOON_SAVE_NDS;
    (void)daemoon_strlcpy(m.title_id, sizeof(m.title_id), "ADAE_POKEMON");
    (void)daemoon_strlcpy(m.sha256, sizeof(m.sha256),
                          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    /* A user set label can contain anything a software keyboard allows. */
    (void)daemoon_strlcpy(m.device_label, sizeof(m.device_label), "he said \"hi\"\\n");
    (void)daemoon_strlcpy(m.created_at, sizeof(m.created_at), "1970-01-01T00:00:00Z");

    CHECK_OK(daemoon_manifest_write(&m, out, sizeof(out), &out_len));
    CHECK_OK(daemoon_manifest_parse(out, out_len, &back));
    CHECK_STR(back.device_label, "he said \"hi\"\\n");
}

TEST_CASE(write_refuses_a_buffer_that_is_too_small)
{
    char json[DAEMOON_MANIFEST_MAX_BYTES];
    char out[32];
    daemoon_manifest_t m;
    size_t len = 0;

    CHECK_EQ_INT(daemoon_test_read_fixture("shared/fixtures/manifest_valid.json", json,
                                           sizeof(json), &len), 0);
    CHECK_OK(daemoon_manifest_parse(json, len, &m));
    CHECK_RESULT(daemoon_manifest_write(&m, out, sizeof(out), NULL),
                 DAEMOON_ERR_BUFFER_TOO_SMALL);
}

void test_manifest(void)
{
    printf("manifest\n");
    RUN(parses_the_shared_fixture);
    RUN(parses_a_first_upload);
    RUN(parses_a_secure_value_package);
    RUN(rejects_a_newer_format_version);
    RUN(rejects_a_parent_that_is_not_older);
    RUN(rejects_malformed_input);
    RUN(rejects_a_bad_digest_field);
    RUN(round_trips);
    RUN(write_escapes_the_device_label);
    RUN(write_refuses_a_buffer_that_is_too_small);
}
