#include "test.h"

#include <daemoon/api.h>

#include <string.h>

TEST_CASE(parses_the_conflict_fixture)
{
    char json[1024];
    daemoon_conflict_t c;
    daemoon_result_t r;
    size_t len = 0;

    memset(&c, 0, sizeof(c));
    CHECK_EQ_INT(daemoon_test_read_fixture("shared/fixtures/error_version_conflict.json", json,
                                           sizeof(json), &len), 0);

    r = daemoon_api_parse_error(json, len, 409, &c);
    CHECK_RESULT(r, DAEMOON_ERR_VERSION_CONFLICT);

    /* This is what ui->choose has to show, so every field matters. */
    CHECK_EQ_INT(c.server_version, 43);
    CHECK_EQ_INT(c.parent_version, 41);
    CHECK_EQ_INT(c.server_size, 32768);
    CHECK_STR(c.server_device_label, "New 2DS");
    CHECK_STR(c.server_received_at, "2026-02-03T04:05:06Z");
}

TEST_CASE(body_code_beats_the_status)
{
    const char *body = "{\"error\":{\"code\":\"device_revoked\"}}";

    /* Both are 401 responses, but the codes mean different things to the user: one
     * says sign in again, the other says this console was revoked. */
    CHECK_RESULT(daemoon_api_parse_error(body, strlen(body), 401, NULL), DAEMOON_ERR_DEVICE_REVOKED);
}

TEST_CASE(falls_back_to_the_status_when_the_body_is_unusable)
{
    /* A captive portal or a proxy replacing the body must not turn into a
     * misleading error code. */
    const char *html = "<html><body>502 Bad Gateway</body></html>";

    CHECK_RESULT(daemoon_api_parse_error(html, strlen(html), 500, NULL), DAEMOON_ERR_INTERNAL_ERROR);
    CHECK_RESULT(daemoon_api_parse_error(NULL, 0, 404, NULL), DAEMOON_ERR_NOT_FOUND);
    CHECK_RESULT(daemoon_api_parse_error("{}", 2, 413, NULL), DAEMOON_ERR_SAVE_TOO_LARGE);
}

TEST_CASE(an_unknown_code_is_never_success)
{
    const char *body = "{\"error\":{\"code\":\"quantum_flux\"}}";

    CHECK_RESULT(daemoon_api_parse_error(body, strlen(body), 418, NULL), DAEMOON_ERR_INTERNAL_ERROR);
}

TEST_CASE(a_conflict_without_detail_still_parses)
{
    daemoon_conflict_t c;
    const char *body = "{\"error\":{\"code\":\"version_conflict\"}}";

    memset(&c, 0xff, sizeof(c));
    CHECK_RESULT(daemoon_api_parse_error(body, strlen(body), 409, &c), DAEMOON_ERR_VERSION_CONFLICT);
}

void test_api(void)
{
    printf("api\n");
    RUN(parses_the_conflict_fixture);
    RUN(body_code_beats_the_status);
    RUN(falls_back_to_the_status_when_the_body_is_unusable);
    RUN(an_unknown_code_is_never_success);
    RUN(a_conflict_without_detail_still_parses);
}
