#include "test.h"

#include <daemoon/api.h>
#include <daemoon/util/strbuf.h>

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

/* ------------------------------------------------- a net backend that misbehaves */

/* Real failures are not tidy. This one can fail a fixed number of times before
 * succeeding, answer with an error body while a download sink is attached, and
 * count how many times it was called, which is the only way to see a retry
 * ceiling from the outside. */
typedef struct {
    unsigned calls;
    unsigned fail_first;     /* fail this many attempts before answering */
    daemoon_result_t failure; /* what those attempts return */
    int status;              /* status for the attempt that gets through */
    const char *body;        /* body for that attempt */
} flaky_net_t;

static daemoon_result_t flaky_request(void *vctx, const daemoon_http_req_t *req,
                                      daemoon_http_resp_t *resp)
{
    flaky_net_t *net = (flaky_net_t *)vctx;

    net->calls++;
    if (net->calls <= net->fail_first) {
        return net->failure;
    }

    /* The contract in backend.h: status is set before the first body_write, so a
     * caller can route an error body away from the sink a success body goes to. */
    resp->status = net->status;
    if (net->body != NULL && resp->body_write != NULL) {
        return resp->body_write(resp->body_ctx, net->body, strlen(net->body));
    }
    return DAEMOON_OK;
}

static const daemoon_net_backend_t flaky_backend = { flaky_request };

/* A backend that hands over the body before it knows the status, which is what
 * curl looks like if the status is read with curl_easy_getinfo after the transfer
 * instead of off the status line as it arrives. */
static daemoon_result_t late_status_request(void *vctx, const daemoon_http_req_t *req,
                                            daemoon_http_resp_t *resp)
{
    flaky_net_t *net = (flaky_net_t *)vctx;
    daemoon_result_t r = DAEMOON_OK;

    net->calls++;
    if (net->body != NULL && resp->body_write != NULL) {
        r = resp->body_write(resp->body_ctx, net->body, strlen(net->body));
    }
    resp->status = net->status; /* too late to be useful, and that is the point */
    return r;
}

static const daemoon_net_backend_t late_status_backend = { late_status_request };

static void flaky_env(daemoon_env_t *env, flaky_net_t *net)
{
    memset(env, 0, sizeof(*env));
    env->net = &flaky_backend;
    env->net_ctx = net;
    env->server_url = "http://example.invalid";
    env->token = "test-token";
    env->device_label = "test console";
}

/* A sink that records everything written to it, standing in for the staged file a
 * download streams into. */
typedef struct {
    char   buf[256];
    size_t len;
} sink_ctx_t;

static daemoon_result_t sink_write(void *ctx, const void *buf, size_t len)
{
    sink_ctx_t *s = (sink_ctx_t *)ctx;
    size_t room = sizeof(s->buf) - s->len;

    if (len > room) {
        len = room;
    }
    memcpy(s->buf + s->len, buf, len);
    s->len += len;
    return DAEMOON_OK;
}

TEST_CASE(a_retryable_failure_is_retried_up_to_the_ceiling)
{
    daemoon_env_t env;
    flaky_net_t net;
    daemoon_remote_meta_t meta;

    memset(&net, 0, sizeof(net));
    net.fail_first = 99; /* never recovers */
    net.failure = DAEMOON_ERR_NETWORK_ERROR;
    flaky_env(&env, &net);

    CHECK_RESULT(daemoon_api_get_latest(&env, DAEMOON_PLATFORM_3DS, "0004000000055D00", &meta),
                 DAEMOON_ERR_NETWORK_ERROR);
    /* Bounded. A console on hotel wifi must not sit in a loop, and a server that
     * is down must not be hammered. */
    CHECK_EQ_INT(net.calls, DAEMOON_RETRY_CEILING);
}

TEST_CASE(a_retry_that_succeeds_returns_the_answer)
{
    daemoon_env_t env;
    flaky_net_t net;
    daemoon_remote_meta_t meta;

    memset(&net, 0, sizeof(net));
    net.fail_first = DAEMOON_RETRY_CEILING - 1;
    net.failure = DAEMOON_ERR_TIMEOUT;
    net.status = 200;
    net.body = "{\"title_id\":\"0004000000055D00\",\"platform\":\"3ds\",\"version\":7,"
               "\"parent_version\":6,"
               "\"sha256\":\"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\","
               "\"size\":42,\"device_label\":\"other\",\"received_at\":\"2026-01-01T00:00:00Z\"}";
    flaky_env(&env, &net);

    memset(&meta, 0, sizeof(meta));
    CHECK_OK(daemoon_api_get_latest(&env, DAEMOON_PLATFORM_3DS, "0004000000055D00", &meta));
    CHECK_EQ_INT(net.calls, DAEMOON_RETRY_CEILING);
    CHECK_EQ_INT(meta.exists, 1);
    CHECK_EQ_INT(meta.latest_version, 7);
    CHECK_EQ_INT(meta.size, 42);
}

/* backend.h says the status is filled in before the first body_write, because
 * that call is where a save is told apart from an error message and there is no
 * asking again afterwards.
 *
 * The 3DS backend read the status with curl_easy_getinfo after curl_easy_perform
 * returned, by which point every callback had already run with a status of zero.
 * A successful upload's response went into the error buffer, the success buffer
 * stayed empty, and the console said parse_error for an upload the server had
 * accepted - a wrong answer wearing the clothes of a parse failure.
 *
 * So a body offered without a status is refused rather than guessed at. */
TEST_CASE(a_body_before_the_status_is_refused_rather_than_misfiled)
{
    daemoon_env_t env;
    flaky_net_t net;
    daemoon_remote_meta_t meta;

    memset(&net, 0, sizeof(net));
    net.status = 200;
    net.body = "{\"title_id\":\"0004000000055D00\",\"platform\":\"3ds\",\"version\":7,"
               "\"sha256\":\"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\","
               "\"size\":42,\"device_label\":\"other\",\"received_at\":\"2026-01-01T00:00:00Z\"}";

    memset(&env, 0, sizeof(env));
    env.net = &late_status_backend;
    env.net_ctx = &net;
    env.server_url = "http://example.invalid";
    env.token = "test-token";
    env.device_label = "test console";

    memset(&meta, 0, sizeof(meta));
    /* Not parse_error: the body parsed fine, it was put in the wrong place. */
    CHECK_RESULT(daemoon_api_get_latest(&env, DAEMOON_PLATFORM_3DS, "0004000000055D00",
                                        &meta),
                 DAEMOON_ERR_BACKEND_ERROR);
    CHECK_EQ_INT(meta.exists, 0);
}

TEST_CASE(a_failure_that_cannot_be_retried_is_not_retried)
{
    daemoon_env_t env;
    flaky_net_t net;
    daemoon_remote_meta_t meta;

    memset(&net, 0, sizeof(net));
    net.fail_first = 99;
    net.failure = DAEMOON_ERR_UNSUPPORTED; /* retrying changes nothing */
    flaky_env(&env, &net);

    CHECK_RESULT(daemoon_api_get_latest(&env, DAEMOON_PLATFORM_3DS, "0004000000055D00", &meta),
                 DAEMOON_ERR_UNSUPPORTED);
    CHECK_EQ_INT(net.calls, 1);
}

TEST_CASE(a_missing_title_is_not_an_error)
{
    daemoon_env_t env;
    flaky_net_t net;
    daemoon_remote_meta_t meta;

    memset(&net, 0, sizeof(net));
    net.status = 404;
    net.body = "{\"error\":{\"code\":\"not_found\"}}";
    flaky_env(&env, &net);

    memset(&meta, 0xff, sizeof(meta));
    CHECK_OK(daemoon_api_get_latest(&env, DAEMOON_PLATFORM_3DS, "0004000000055D00", &meta));
    CHECK_EQ_INT(meta.exists, 0);
}

TEST_CASE(an_error_body_never_reaches_the_download_sink)
{
    /* Without routing, a 409 body would be written into the file the download was
     * streaming into, and the package staged on the SD card would be a JSON error
     * with a zip extension. */
    daemoon_env_t env;
    flaky_net_t net;
    daemoon_stream_t sink;
    sink_ctx_t ctx;

    memset(&net, 0, sizeof(net));
    net.status = 404;
    net.body = "{\"error\":{\"code\":\"not_found\"}}";
    flaky_env(&env, &net);

    memset(&ctx, 0, sizeof(ctx));
    memset(&sink, 0, sizeof(sink));
    sink.write = sink_write;
    sink.ctx = &ctx;

    CHECK_RESULT(daemoon_api_download(&env, DAEMOON_PLATFORM_3DS, "0004000000055D00", 1, &sink,
                                      NULL, 0),
                 DAEMOON_ERR_NOT_FOUND);
    CHECK_EQ_INT(ctx.len, 0);
}

TEST_CASE(a_download_is_never_retried)
{
    /* A second attempt would append to a partly written file. The caller starts
     * over instead. */
    daemoon_env_t env;
    flaky_net_t net;
    daemoon_stream_t sink;
    sink_ctx_t ctx;

    memset(&net, 0, sizeof(net));
    net.fail_first = 99;
    net.failure = DAEMOON_ERR_NETWORK_ERROR;
    flaky_env(&env, &net);

    memset(&ctx, 0, sizeof(ctx));
    memset(&sink, 0, sizeof(sink));
    sink.write = sink_write;
    sink.ctx = &ctx;

    CHECK_RESULT(daemoon_api_download(&env, DAEMOON_PLATFORM_3DS, "0004000000055D00", 1, &sink,
                                      NULL, 0),
                 DAEMOON_ERR_NETWORK_ERROR);
    CHECK_EQ_INT(net.calls, 1);
}

TEST_CASE(a_successful_download_reaches_the_sink)
{
    daemoon_env_t env;
    flaky_net_t net;
    daemoon_stream_t sink;
    sink_ctx_t ctx;

    memset(&net, 0, sizeof(net));
    net.status = 200;
    net.body = "PK\x03\x04 pretend package";
    flaky_env(&env, &net);

    memset(&ctx, 0, sizeof(ctx));
    memset(&sink, 0, sizeof(sink));
    sink.write = sink_write;
    sink.ctx = &ctx;

    CHECK_OK(daemoon_api_download(&env, DAEMOON_PLATFORM_3DS, "0004000000055D00", 1, &sink,
                                  NULL, 0));
    CHECK_EQ_INT(ctx.len, strlen(net.body));
}

TEST_CASE(an_upload_is_never_retried)
{
    /* The body is a stream that cannot be rewound, and what a second attempt means
     * is the server's decision, not the client's. */
    daemoon_env_t env;
    flaky_net_t net;
    daemoon_manifest_t m;
    daemoon_remote_meta_t issued;
    daemoon_conflict_t conflict;
    daemoon_stream_t body;
    sink_ctx_t unused;

    memset(&net, 0, sizeof(net));
    net.fail_first = 99;
    net.failure = DAEMOON_ERR_NETWORK_ERROR;
    flaky_env(&env, &net);

    daemoon_manifest_init(&m);
    m.platform = DAEMOON_PLATFORM_3DS;
    m.save_type = DAEMOON_SAVE_SAVEDATA;
    CHECK_OK(daemoon_strlcpy(m.title_id, sizeof(m.title_id), "0004000000055D00"));
    CHECK_OK(daemoon_strlcpy(m.sha256, sizeof(m.sha256),
                             "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_OK(daemoon_strlcpy(m.device_label, sizeof(m.device_label), "test console"));
    CHECK_OK(daemoon_strlcpy(m.created_at, sizeof(m.created_at), "1970-01-01T00:00:00Z"));

    memset(&unused, 0, sizeof(unused));
    memset(&body, 0, sizeof(body));
    body.read = NULL; /* never reached: the request fails first */
    body.ctx = &unused;

    memset(&issued, 0, sizeof(issued));
    memset(&conflict, 0, sizeof(conflict));
    CHECK_RESULT(daemoon_api_upload(&env, &m, &body, 0, &issued, &conflict),
                 DAEMOON_ERR_NETWORK_ERROR);
    CHECK_EQ_INT(net.calls, 1);
}

TEST_CASE(a_conflict_body_fills_in_the_choice_dialog)
{
    daemoon_env_t env;
    flaky_net_t net;
    daemoon_manifest_t m;
    daemoon_remote_meta_t issued;
    daemoon_conflict_t conflict;
    daemoon_stream_t body;

    memset(&net, 0, sizeof(net));
    net.status = 409;
    net.body = "{\"error\":{\"code\":\"version_conflict\",\"detail\":{"
               "\"server_version\":43,\"parent_version\":41,\"server_size\":32768,"
               "\"server_device_label\":\"New 2DS\","
               "\"server_received_at\":\"2026-02-03T04:05:06Z\"}}}";
    flaky_env(&env, &net);

    daemoon_manifest_init(&m);
    m.platform = DAEMOON_PLATFORM_3DS;
    m.save_type = DAEMOON_SAVE_SAVEDATA;
    CHECK_OK(daemoon_strlcpy(m.title_id, sizeof(m.title_id), "0004000000055D00"));
    CHECK_OK(daemoon_strlcpy(m.sha256, sizeof(m.sha256),
                             "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_OK(daemoon_strlcpy(m.device_label, sizeof(m.device_label), "test console"));
    CHECK_OK(daemoon_strlcpy(m.created_at, sizeof(m.created_at), "1970-01-01T00:00:00Z"));

    memset(&body, 0, sizeof(body));
    memset(&issued, 0, sizeof(issued));
    memset(&conflict, 0, sizeof(conflict));

    CHECK_RESULT(daemoon_api_upload(&env, &m, &body, 0, &issued, &conflict),
                 DAEMOON_ERR_VERSION_CONFLICT);
    /* Every field the user needs in order to choose. */
    CHECK_EQ_INT(conflict.server_version, 43);
    CHECK_EQ_INT(conflict.server_size, 32768);
    CHECK_STR(conflict.server_device_label, "New 2DS");
    CHECK_STR(conflict.server_received_at, "2026-02-03T04:05:06Z");
}

TEST_CASE(api_calls_refuse_a_missing_server_url)
{
    daemoon_env_t env;
    flaky_net_t net;
    daemoon_remote_meta_t meta;

    memset(&net, 0, sizeof(net));
    flaky_env(&env, &net);
    env.server_url = "";

    CHECK_RESULT(daemoon_api_get_latest(&env, DAEMOON_PLATFORM_3DS, "0004000000055D00", &meta),
                 DAEMOON_ERR_INVALID_REQUEST);
    CHECK_EQ_INT(net.calls, 0);
}

void test_api(void)
{
    printf("api\n");
    RUN(parses_the_conflict_fixture);
    RUN(body_code_beats_the_status);
    RUN(falls_back_to_the_status_when_the_body_is_unusable);
    RUN(an_unknown_code_is_never_success);
    RUN(a_conflict_without_detail_still_parses);
    RUN(a_retryable_failure_is_retried_up_to_the_ceiling);
    RUN(a_retry_that_succeeds_returns_the_answer);
    RUN(a_body_before_the_status_is_refused_rather_than_misfiled);
    RUN(a_failure_that_cannot_be_retried_is_not_retried);
    RUN(a_missing_title_is_not_an_error);
    RUN(an_error_body_never_reaches_the_download_sink);
    RUN(a_download_is_never_retried);
    RUN(a_successful_download_reaches_the_sink);
    RUN(an_upload_is_never_retried);
    RUN(a_conflict_body_fills_in_the_choice_dialog);
    RUN(api_calls_refuse_a_missing_server_url);
}
