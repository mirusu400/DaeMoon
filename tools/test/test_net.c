/* The posix HTTP backend, against a socket that answers badly on purpose.
 *
 * The end to end test only ever sees daemoond, which sets Content-Length on every
 * response. That means the chunked decoder - the branch that runs the moment
 * anyone puts a reverse proxy in front of their self hosted instance - has never
 * executed outside this file. The same goes for a truncated body, a status line
 * that is not a status line, and a connection that closes early.
 *
 * Each case forks a child that writes one canned response and exits, so the bytes
 * on the wire are exactly what the test says they are.
 */
#define _POSIX_C_SOURCE 200809L

#include "test.h"

#include "daemoon_posix.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* A canned response, served once. Returns the port, or 0 on failure, and stores
 * the child pid so the caller can reap it. */
static int serve_once(const char *response, size_t response_len, pid_t *out_pid)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    int listener;
    int port;
    pid_t pid;
    int one = 1;

    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        return 0;
    }
    (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; /* let the kernel choose */

    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(listener, 1) != 0 ||
        getsockname(listener, (struct sockaddr *)&addr, &len) != 0) {
        close(listener);
        return 0;
    }
    port = ntohs(addr.sin_port);

    pid = fork();
    if (pid < 0) {
        close(listener);
        return 0;
    }
    if (pid == 0) {
        char scratch[4096];
        int conn = accept(listener, NULL, NULL);

        if (conn >= 0) {
            /* Read the request headers so the client's write does not fail on a
             * closed peer, then answer. */
            ssize_t n = recv(conn, scratch, sizeof(scratch), 0);
            (void)n;
            if (response != NULL && response_len > 0) {
                size_t sent = 0;
                while (sent < response_len) {
                    ssize_t w = send(conn, response + sent, response_len - sent, 0);
                    if (w <= 0) {
                        break;
                    }
                    sent += (size_t)w;
                }
            }
            close(conn);
        }
        close(listener);
        /* _exit, not exit: the child shares the parent's stdio buffers and its
         * atexit handlers, and flushing them twice would duplicate test output. */
        _exit(0);
    }

    close(listener);
    *out_pid = pid;
    return port;
}

static void reap(pid_t pid)
{
    int status = 0;
    (void)waitpid(pid, &status, 0);
}

/* Collects a response body. */
typedef struct {
    char   buf[1024];
    size_t len;
    int    overflow;
} body_ctx_t;

static daemoon_result_t body_write(void *ctx, const void *buf, size_t len)
{
    body_ctx_t *b = (body_ctx_t *)ctx;
    size_t room = sizeof(b->buf) - b->len - 1;

    if (len > room) {
        len = room;
        b->overflow = 1;
    }
    memcpy(b->buf + b->len, buf, len);
    b->len += len;
    b->buf[b->len] = '\0';
    return DAEMOON_OK;
}

/* Runs one GET against a served response. */
static daemoon_result_t fetch(const char *response, size_t response_len, char *url_out,
                              size_t url_cap, daemoon_http_resp_t *resp, body_ctx_t *body)
{
    daemoon_posix_net_ctx_t net;
    daemoon_http_req_t req;
    pid_t pid = 0;
    int port;
    daemoon_result_t r;

    port = serve_once(response, response_len, &pid);
    if (port == 0) {
        return DAEMOON_ERR_NETWORK_ERROR;
    }
    (void)snprintf(url_out, url_cap, "http://127.0.0.1:%d/v1/titles", port);

    daemoon_posix_net_init(&net);

    memset(&req, 0, sizeof(req));
    req.method = "GET";
    req.url = url_out;
    req.body_len = -1;
    req.timeout_ms = 5000;

    memset(resp, 0, sizeof(*resp));
    memset(body, 0, sizeof(*body));
    resp->body_write = body_write;
    resp->body_ctx = body;

    r = daemoon_posix_net_backend.request(&net, &req, resp);
    reap(pid);
    return r;
}

#define RESP(literal) (literal), (sizeof(literal) - 1)

TEST_CASE(a_content_length_body_arrives_whole)
{
    char url[128];
    daemoon_http_resp_t resp;
    body_ctx_t body;

    CHECK_OK(fetch(RESP("HTTP/1.1 200 OK\r\n"
                        "Content-Type: application/zip\r\n"
                        "Content-Length: 11\r\n"
                        "\r\n"
                        "hello world"),
                   url, sizeof(url), &resp, &body));

    CHECK_EQ_INT(resp.status, 200);
    CHECK_EQ_INT(resp.content_length, 11);
    CHECK_STR(body.buf, "hello world");
}

TEST_CASE(a_chunked_body_is_reassembled)
{
    /* This branch never runs against daemoond, which always knows its length. It
     * runs the first time somebody puts nginx or a tunnel in front of it. */
    char url[128];
    daemoon_http_resp_t resp;
    body_ctx_t body;

    CHECK_OK(fetch(RESP("HTTP/1.1 200 OK\r\n"
                        "Transfer-Encoding: chunked\r\n"
                        "\r\n"
                        "5\r\nhello\r\n"
                        "1\r\n \r\n"
                        "5\r\nworld\r\n"
                        "0\r\n"
                        "\r\n"),
                   url, sizeof(url), &resp, &body));

    CHECK_EQ_INT(resp.status, 200);
    CHECK_STR(body.buf, "hello world");
}

TEST_CASE(a_chunked_body_with_extensions_and_trailers)
{
    /* Chunk extensions after the size, and a trailer section after the final
     * chunk. Both are legal and both are things a proxy emits. */
    char url[128];
    daemoon_http_resp_t resp;
    body_ctx_t body;

    CHECK_OK(fetch(RESP("HTTP/1.1 200 OK\r\n"
                        "Transfer-Encoding: chunked\r\n"
                        "\r\n"
                        "4;name=value\r\nsave\r\n"
                        "4\r\ndata\r\n"
                        "0\r\n"
                        "X-Checksum: ignored\r\n"
                        "\r\n"),
                   url, sizeof(url), &resp, &body));

    CHECK_STR(body.buf, "savedata");
}

TEST_CASE(a_chunked_body_larger_than_one_chunk_header)
{
    /* A hex size with more than one digit, which is what any real payload has. */
    char url[128];
    daemoon_http_resp_t resp;
    body_ctx_t body;

    CHECK_OK(fetch(RESP("HTTP/1.1 200 OK\r\n"
                        "Transfer-Encoding: chunked\r\n"
                        "\r\n"
                        "1a\r\nabcdefghijklmnopqrstuvwxyz\r\n"
                        "0\r\n\r\n"),
                   url, sizeof(url), &resp, &body));

    CHECK_STR(body.buf, "abcdefghijklmnopqrstuvwxyz");
    CHECK_EQ_INT(body.len, 26);
}

TEST_CASE(a_body_that_ends_with_the_connection)
{
    /* No length and no chunking: the body is whatever arrives before the close.
     * HTTP/1.0 style, and what some proxies do on an error page. */
    char url[128];
    daemoon_http_resp_t resp;
    body_ctx_t body;

    CHECK_OK(fetch(RESP("HTTP/1.1 500 Internal Server Error\r\n"
                        "\r\n"
                        "gateway is unhappy"),
                   url, sizeof(url), &resp, &body));

    CHECK_EQ_INT(resp.status, 500);
    CHECK_STR(body.buf, "gateway is unhappy");
}

TEST_CASE(a_truncated_body_is_an_error)
{
    /* Content-Length promised more than arrived. Accepting this would hand the
     * archive layer a package that is half a package. */
    char url[128];
    daemoon_http_resp_t resp;
    body_ctx_t body;

    CHECK_RESULT(fetch(RESP("HTTP/1.1 200 OK\r\n"
                           "Content-Length: 100\r\n"
                           "\r\n"
                           "only this much"),
                       url, sizeof(url), &resp, &body),
                 DAEMOON_ERR_NETWORK_ERROR);
}

TEST_CASE(a_truncated_chunked_body_is_an_error)
{
    char url[128];
    daemoon_http_resp_t resp;
    body_ctx_t body;

    CHECK_RESULT(fetch(RESP("HTTP/1.1 200 OK\r\n"
                           "Transfer-Encoding: chunked\r\n"
                           "\r\n"
                           "20\r\nnot thirty two bytes"),
                       url, sizeof(url), &resp, &body),
                 DAEMOON_ERR_NETWORK_ERROR);
}

TEST_CASE(a_response_that_is_not_http_is_refused)
{
    char url[128];
    daemoon_http_resp_t resp;
    body_ctx_t body;

    /* A captive portal, a wrong port, or a plain socket answering on 80. */
    CHECK_RESULT(fetch(RESP("<html>you are not on the internet</html>"),
                       url, sizeof(url), &resp, &body),
                 DAEMOON_ERR_NETWORK_ERROR);

    CHECK_RESULT(fetch(RESP("HTTP/1.1\r\n\r\n"), url, sizeof(url), &resp, &body),
                 DAEMOON_ERR_NETWORK_ERROR);
}

TEST_CASE(a_connection_that_closes_before_answering)
{
    char url[128];
    daemoon_http_resp_t resp;
    body_ctx_t body;

    CHECK_RESULT(fetch(NULL, 0, url, sizeof(url), &resp, &body), DAEMOON_ERR_NETWORK_ERROR);
}

TEST_CASE(the_digest_header_is_captured)
{
    char url[128];
    daemoon_http_resp_t resp;
    body_ctx_t body;

    /* Case insensitive, because a proxy is free to rewrite the casing. */
    CHECK_OK(fetch(RESP("HTTP/1.1 200 OK\r\n"
                        "content-length: 2\r\n"
                        "x-daemoon-sha256: "
                        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\r\n"
                        "\r\n"
                        "ok"),
                   url, sizeof(url), &resp, &body));

    CHECK_STR(resp.sha256,
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK_EQ_INT(resp.content_length, 2);
}

TEST_CASE(https_is_refused_rather_than_downgraded)
{
    /* This backend has no TLS. Quietly sending a device token over plain HTTP
     * because the scheme said https would be worse than not working. */
    daemoon_posix_net_ctx_t net;
    daemoon_http_req_t req;
    daemoon_http_resp_t resp;

    daemoon_posix_net_init(&net);
    memset(&req, 0, sizeof(req));
    req.method = "GET";
    req.url = "https://example.invalid/v1/titles";
    req.body_len = -1;
    req.timeout_ms = 1000;
    memset(&resp, 0, sizeof(resp));

    CHECK_RESULT(daemoon_posix_net_backend.request(&net, &req, &resp),
                 DAEMOON_ERR_UNSUPPORTED);
}

TEST_CASE(malformed_urls_are_refused)
{
    daemoon_posix_net_ctx_t net;
    daemoon_http_req_t req;
    daemoon_http_resp_t resp;
    size_t i;
    static const char *const bad[] = {
        "http://",
        "http:///v1/titles",
        "http://host:/v1/titles",
        "ftp://example.invalid/x",
        "example.invalid/v1/titles"
    };

    daemoon_posix_net_init(&net);
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        memset(&req, 0, sizeof(req));
        req.method = "GET";
        req.url = bad[i];
        req.body_len = -1;
        req.timeout_ms = 1000;
        memset(&resp, 0, sizeof(resp));

        CHECK(daemoon_posix_net_backend.request(&net, &req, &resp) != DAEMOON_OK);
    }
}

/* A body reader that would be a bug to call: the request is refused before any of
 * it is read. */
static daemoon_result_t never_read(void *ctx, void *buf, size_t cap, size_t *out_len)
{
    (void)ctx;
    (void)buf;
    (void)cap;
    *out_len = 0;
    return DAEMOON_ERR_IO_ERROR;
}

TEST_CASE(a_body_with_no_length_is_refused)
{
    /* Every caller stages its package as a file first, so a body always has a
     * known length. Chunked upload would work but nothing needs it, and guessing
     * would mean sending a request the server cannot frame. */
    daemoon_posix_net_ctx_t net;
    daemoon_http_req_t req;
    daemoon_http_resp_t resp;
    pid_t pid = 0;
    char url[128];
    int port;

    port = serve_once(RESP("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n"), &pid);
    CHECK(port != 0);
    (void)snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1/titles", port);

    daemoon_posix_net_init(&net);
    memset(&req, 0, sizeof(req));
    req.method = "POST";
    req.url = url;
    req.body_read = never_read;
    req.body_len = -1;
    req.timeout_ms = 1000;
    memset(&resp, 0, sizeof(resp));

    CHECK_RESULT(daemoon_posix_net_backend.request(&net, &req, &resp), DAEMOON_ERR_UNSUPPORTED);
    reap(pid);
}

void test_net(void)
{
    printf("posix net backend\n");

    /* A peer that closes early would otherwise kill the test run with SIGPIPE
     * instead of returning an error from send. */
    (void)signal(SIGPIPE, SIG_IGN);

    RUN(a_content_length_body_arrives_whole);
    RUN(a_chunked_body_is_reassembled);
    RUN(a_chunked_body_with_extensions_and_trailers);
    RUN(a_chunked_body_larger_than_one_chunk_header);
    RUN(a_body_that_ends_with_the_connection);
    RUN(a_truncated_body_is_an_error);
    RUN(a_truncated_chunked_body_is_an_error);
    RUN(a_response_that_is_not_http_is_refused);
    RUN(a_connection_that_closes_before_answering);
    RUN(the_digest_header_is_captured);
    RUN(https_is_refused_rather_than_downgraded);
    RUN(malformed_urls_are_refused);
    RUN(a_body_with_no_length_is_refused);
}
