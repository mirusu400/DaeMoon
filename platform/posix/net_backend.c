/* lstat, gmtime_r, getaddrinfo and readdir all need this before any include. */
#define _POSIX_C_SOURCE 200809L

/* Plain HTTP/1.1 over a socket.
 *
 * Development only. It talks to a daemoond on localhost so the whole sync path can
 * be exercised end to end without a console. There is deliberately no TLS here: the
 * consoles use 3ds-curl with 3ds-mbedtls and libnx respectively, because httpc:C
 * ships old cipher suites and a stale root CA store and fails against modern
 * servers regularly. Nothing in this file is meant to ship.
 *
 * Every socket carries a send and receive timeout. There are no unbounded waits.
 */
#include "daemoon_posix.h"

#include <daemoon/util/strbuf.h>

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#define NET_IO_CHUNK 16384
#define NET_LINE_MAX 1024
/* Matches DAEMOON_DEFAULT_TIMEOUT_MS in api.h. Duplicated rather than including
 * api.h here: a net backend has no business knowing the API layer. */
#define NET_DEFAULT_TIMEOUT_MS 30000

void daemoon_posix_net_init(daemoon_posix_net_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->connect_timeout_ms = NET_DEFAULT_TIMEOUT_MS;
}

/* --------------------------------------------------------------- URL split */

typedef struct {
    char host[256];
    char port[8];
    char path[512];
} net_url_t;

static daemoon_result_t split_url(const char *url, net_url_t *out)
{
    const char *p;
    const char *host_start;
    const char *host_end;
    const char *colon;
    size_t n;

    if (url == NULL || strncmp(url, "http://", 7) != 0) {
        /* https is refused rather than silently downgraded. */
        return DAEMOON_ERR_UNSUPPORTED;
    }
    host_start = url + 7;
    p = strchr(host_start, '/');
    host_end = (p != NULL) ? p : host_start + strlen(host_start);

    colon = memchr(host_start, ':', (size_t)(host_end - host_start));
    n = (size_t)((colon != NULL ? colon : host_end) - host_start);
    if (n == 0 || n >= sizeof(out->host)) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }
    memcpy(out->host, host_start, n);
    out->host[n] = '\0';

    if (colon != NULL) {
        n = (size_t)(host_end - colon - 1);
        if (n == 0 || n >= sizeof(out->port)) {
            return DAEMOON_ERR_INVALID_REQUEST;
        }
        memcpy(out->port, colon + 1, n);
        out->port[n] = '\0';
    } else {
        (void)daemoon_strlcpy(out->port, sizeof(out->port), "80");
    }

    if (p == NULL) {
        return daemoon_strlcpy(out->path, sizeof(out->path), "/");
    }
    return daemoon_strlcpy(out->path, sizeof(out->path), p);
}

/* ------------------------------------------------------------------ socket */

typedef struct {
    int           fd;
    unsigned char buf[NET_IO_CHUNK];
    size_t        len;
    size_t        pos;
} net_conn_t;

static daemoon_result_t conn_open(net_conn_t *c, const net_url_t *u, int timeout_ms)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *ai;
    struct timeval tv;
    int fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(u->host, u->port, &hints, &res) != 0) {
        return DAEMOON_ERR_NETWORK_ERROR;
    }

    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    for (ai = res; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        return DAEMOON_ERR_NETWORK_ERROR;
    }

    c->fd = fd;
    c->len = 0;
    c->pos = 0;
    return DAEMOON_OK;
}

static void conn_close(net_conn_t *c)
{
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
}

static daemoon_result_t conn_send(net_conn_t *c, const void *buf, size_t len)
{
    const unsigned char *p = (const unsigned char *)buf;
    size_t done = 0;

    while (done < len) {
        ssize_t n = send(c->fd, p + done, len - done, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return DAEMOON_ERR_TIMEOUT;
            }
            return DAEMOON_ERR_NETWORK_ERROR;
        }
        done += (size_t)n;
    }
    return DAEMOON_OK;
}

/* Refills the buffer. out_len 0 means the peer closed. */
static daemoon_result_t conn_fill(net_conn_t *c, size_t *out_len)
{
    ssize_t n;

    if (c->pos < c->len) {
        *out_len = c->len - c->pos;
        return DAEMOON_OK;
    }
    for (;;) {
        n = recv(c->fd, c->buf, sizeof(c->buf), 0);
        if (n >= 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return DAEMOON_ERR_TIMEOUT;
        }
        return DAEMOON_ERR_NETWORK_ERROR;
    }
    c->pos = 0;
    c->len = (size_t)n;
    *out_len = c->len;
    return DAEMOON_OK;
}

static daemoon_result_t conn_read(net_conn_t *c, void *buf, size_t cap, size_t *out_len)
{
    size_t avail = 0;

    DAEMOON_TRY(conn_fill(c, &avail));
    if (avail == 0) {
        *out_len = 0;
        return DAEMOON_OK;
    }
    if (avail > cap) {
        avail = cap;
    }
    memcpy(buf, c->buf + c->pos, avail);
    c->pos += avail;
    *out_len = avail;
    return DAEMOON_OK;
}

static daemoon_result_t conn_read_line(net_conn_t *c, char *line, size_t cap)
{
    size_t len = 0;

    for (;;) {
        size_t avail = 0;
        unsigned char ch;

        DAEMOON_TRY(conn_fill(c, &avail));
        if (avail == 0) {
            return DAEMOON_ERR_NETWORK_ERROR; /* truncated headers */
        }
        ch = c->buf[c->pos++];
        if (ch == '\n') {
            break;
        }
        if (ch == '\r') {
            continue;
        }
        if (len + 1 < cap) {
            line[len++] = (char)ch;
        }
        /* Anything longer than the buffer is a header this client does not need;
         * the tail is dropped rather than growing an allocation on the 3DS side of
         * the same interface. */
    }
    line[len] = '\0';
    return DAEMOON_OK;
}

/* ----------------------------------------------------------------- headers */

static int header_is(const char *line, const char *name, const char **out_value)
{
    size_t n = strlen(name);
    size_t i;

    for (i = 0; i < n; ++i) {
        char a = line[i];
        char b = name[i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a + ('a' - 'A'));
        }
        if (b >= 'A' && b <= 'Z') {
            b = (char)(b + ('a' - 'A'));
        }
        if (a != b) {
            return 0;
        }
    }
    if (line[n] != ':') {
        return 0;
    }
    *out_value = line + n + 1;
    while (**out_value == ' ' || **out_value == '\t') {
        (*out_value)++;
    }
    return 1;
}

/* ------------------------------------------------------------------ driver */

static daemoon_result_t send_request(net_conn_t *c, const daemoon_http_req_t *req,
                                     const net_url_t *u)
{
    char head[2048];
    daemoon_strbuf_t sb;
    size_t i;

    daemoon_strbuf_init(&sb, head, sizeof(head));
    daemoon_strbuf_add(&sb, req->method);
    daemoon_strbuf_addc(&sb, ' ');
    daemoon_strbuf_add(&sb, u->path);
    daemoon_strbuf_add(&sb, " HTTP/1.1\r\nHost: ");
    daemoon_strbuf_add(&sb, u->host);
    if (strcmp(u->port, "80") != 0) {
        daemoon_strbuf_addc(&sb, ':');
        daemoon_strbuf_add(&sb, u->port);
    }
    /* One request per connection. Keep alive would need connection reuse
     * bookkeeping that this development backend has no reason to carry. */
    daemoon_strbuf_add(&sb, "\r\nConnection: close\r\n");

    for (i = 0; i < req->nheaders; ++i) {
        daemoon_strbuf_add(&sb, req->headers[i].name);
        daemoon_strbuf_add(&sb, ": ");
        daemoon_strbuf_add(&sb, req->headers[i].value);
        daemoon_strbuf_add(&sb, "\r\n");
    }

    if (req->body_read != NULL) {
        if (req->body_len < 0) {
            /* Chunked upload would work, but every caller here knows its length
             * because the package is staged as a file first. */
            return DAEMOON_ERR_UNSUPPORTED;
        }
        daemoon_strbuf_add(&sb, "Content-Length: ");
        daemoon_strbuf_add_uint(&sb, (unsigned long long)req->body_len);
        daemoon_strbuf_add(&sb, "\r\n");
    }
    daemoon_strbuf_add(&sb, "\r\n");
    DAEMOON_TRY(daemoon_strbuf_result(&sb));

    DAEMOON_TRY(conn_send(c, head, sb.len));

    if (req->body_read != NULL) {
        unsigned char buf[NET_IO_CHUNK];
        long long remaining = req->body_len;

        while (remaining > 0) {
            size_t want = sizeof(buf);
            size_t got = 0;

            if ((long long)want > remaining) {
                want = (size_t)remaining;
            }
            DAEMOON_TRY(req->body_read(req->body_ctx, buf, want, &got));
            if (got == 0) {
                /* Fewer bytes than Content-Length promised. Sending a truncated
                 * body would leave the server parsing a corrupt package. */
                return DAEMOON_ERR_IO_ERROR;
            }
            DAEMOON_TRY(conn_send(c, buf, got));
            remaining -= (long long)got;
        }
    }
    return DAEMOON_OK;
}

static daemoon_result_t read_body_sized(net_conn_t *c, const daemoon_http_resp_t *resp,
                                        long long content_length)
{
    unsigned char buf[NET_IO_CHUNK];
    long long remaining = content_length;

    for (;;) {
        size_t want = sizeof(buf);
        size_t got = 0;

        if (content_length >= 0) {
            if (remaining <= 0) {
                break;
            }
            if ((long long)want > remaining) {
                want = (size_t)remaining;
            }
        }
        DAEMOON_TRY(conn_read(c, buf, want, &got));
        if (got == 0) {
            if (content_length >= 0 && remaining > 0) {
                return DAEMOON_ERR_NETWORK_ERROR; /* truncated response */
            }
            break;
        }
        if (resp->body_write != NULL) {
            DAEMOON_TRY(resp->body_write(resp->body_ctx, buf, got));
        }
        if (content_length >= 0) {
            remaining -= (long long)got;
        }
    }
    return DAEMOON_OK;
}

static daemoon_result_t read_body_chunked(net_conn_t *c, const daemoon_http_resp_t *resp)
{
    char line[NET_LINE_MAX];
    unsigned char buf[NET_IO_CHUNK];

    for (;;) {
        unsigned long long size = 0;
        const char *p;

        DAEMOON_TRY(conn_read_line(c, line, sizeof(line)));
        for (p = line; *p != '\0'; ++p) {
            int v;
            if (*p >= '0' && *p <= '9') {
                v = *p - '0';
            } else if (*p >= 'a' && *p <= 'f') {
                v = *p - 'a' + 10;
            } else if (*p >= 'A' && *p <= 'F') {
                v = *p - 'A' + 10;
            } else {
                break; /* chunk extension or end of the size field */
            }
            size = size * 16ull + (unsigned long long)v;
        }
        if (size == 0) {
            /* Trailer section, then done. */
            for (;;) {
                DAEMOON_TRY(conn_read_line(c, line, sizeof(line)));
                if (line[0] == '\0') {
                    break;
                }
            }
            return DAEMOON_OK;
        }
        while (size > 0) {
            size_t want = sizeof(buf);
            size_t got = 0;

            if ((unsigned long long)want > size) {
                want = (size_t)size;
            }
            DAEMOON_TRY(conn_read(c, buf, want, &got));
            if (got == 0) {
                return DAEMOON_ERR_NETWORK_ERROR;
            }
            if (resp->body_write != NULL) {
                DAEMOON_TRY(resp->body_write(resp->body_ctx, buf, got));
            }
            size -= got;
        }
        DAEMOON_TRY(conn_read_line(c, line, sizeof(line))); /* trailing CRLF */
    }
}

static daemoon_result_t net_request(void *vctx, const daemoon_http_req_t *req,
                                    daemoon_http_resp_t *resp)
{
    daemoon_posix_net_ctx_t *net = (daemoon_posix_net_ctx_t *)vctx;
    net_conn_t conn;
    net_url_t url;
    char line[NET_LINE_MAX];
    long long content_length = -1;
    int chunked = 0;
    int timeout_ms;
    daemoon_result_t r;

    if (req == NULL || resp == NULL) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }
    timeout_ms = req->timeout_ms > 0 ? req->timeout_ms : NET_DEFAULT_TIMEOUT_MS;

    DAEMOON_TRY(split_url(req->url, &url));

    conn.fd = -1;
    r = conn_open(&conn, &url, timeout_ms);
    if (r != DAEMOON_OK) {
        return r;
    }
    if (net != NULL) {
        net->requests++;
    }

    r = send_request(&conn, req, &url);
    if (r != DAEMOON_OK) {
        goto done;
    }

    /* Status line. The status is set before any body byte is handed to the caller,
     * which is what lets an error body be routed away from a download sink. */
    r = conn_read_line(&conn, line, sizeof(line));
    if (r != DAEMOON_OK) {
        goto done;
    }
    if (strncmp(line, "HTTP/1.", 7) != 0 || strlen(line) < 12) {
        r = DAEMOON_ERR_NETWORK_ERROR;
        goto done;
    }
    resp->status = (line[9] - '0') * 100 + (line[10] - '0') * 10 + (line[11] - '0');

    for (;;) {
        const char *value = NULL;

        r = conn_read_line(&conn, line, sizeof(line));
        if (r != DAEMOON_OK) {
            goto done;
        }
        if (line[0] == '\0') {
            break;
        }
        if (header_is(line, "Content-Length", &value)) {
            content_length = 0;
            while (*value >= '0' && *value <= '9') {
                content_length = content_length * 10 + (*value - '0');
                ++value;
            }
        } else if (header_is(line, "Transfer-Encoding", &value)) {
            chunked = (strstr(value, "chunked") != NULL);
        } else if (header_is(line, "X-DaeMoon-SHA256", &value)) {
            (void)daemoon_strlcpy(resp->sha256, sizeof(resp->sha256), value);
        }
    }
    resp->content_length = content_length;

    if (chunked) {
        r = read_body_chunked(&conn, resp);
    } else {
        r = read_body_sized(&conn, resp, content_length);
    }

done:
    conn_close(&conn);
    return r;
}

const daemoon_net_backend_t daemoon_posix_net_backend = { net_request };
