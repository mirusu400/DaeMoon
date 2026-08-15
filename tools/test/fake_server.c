#include "fake_server.h"

#include <daemoon/archive.h>
#include <daemoon/util/strbuf.h>

#include <stdlib.h>
#include <string.h>

void fake_server_init(fake_server_t *s)
{
    memset(s, 0, sizeof(*s));
}

void fake_server_free(fake_server_t *s)
{
    size_t i, j;

    for (i = 0; i < s->ntitles; ++i) {
        for (j = 0; j < s->titles[i].nversions; ++j) {
            free(s->titles[i].versions[j].blob);
        }
    }
    memset(s, 0, sizeof(*s));
}

static const fake_title_t *find_title_const(const fake_server_t *s, daemoon_platform_t platform,
                                            const char *title_id)
{
    size_t i;

    for (i = 0; i < s->ntitles; ++i) {
        if (s->titles[i].platform == platform && strcmp(s->titles[i].title_id, title_id) == 0) {
            return &s->titles[i];
        }
    }
    return NULL;
}

static fake_title_t *find_title(fake_server_t *s, daemoon_platform_t platform,
                                const char *title_id)
{
    const fake_title_t *t = find_title_const(s, platform, title_id);
    return (t == NULL) ? NULL : &s->titles[t - s->titles];
}

static fake_title_t *ensure_title(fake_server_t *s, daemoon_platform_t platform,
                                  const char *title_id)
{
    fake_title_t *t = find_title(s, platform, title_id);

    if (t != NULL) {
        return t;
    }
    if (s->ntitles >= FAKE_MAX_TITLES) {
        return NULL;
    }
    t = &s->titles[s->ntitles++];
    memset(t, 0, sizeof(*t));
    t->platform = platform;
    (void)daemoon_strlcpy(t->title_id, sizeof(t->title_id), title_id);
    return t;
}

/* ------------------------------------------------------------ mem streams */

typedef struct {
    const unsigned char *data;
    size_t               len;
    size_t               pos;
} mem_read_t;

static daemoon_result_t mem_read(void *ctx, void *buf, size_t cap, size_t *out_len)
{
    mem_read_t *m = (mem_read_t *)ctx;
    size_t n = m->len - m->pos;

    if (n > cap) {
        n = cap;
    }
    memcpy(buf, m->data + m->pos, n);
    m->pos += n;
    *out_len = n;
    return DAEMOON_OK;
}

static daemoon_result_t mem_seek(void *ctx, unsigned long long off)
{
    mem_read_t *m = (mem_read_t *)ctx;

    if (off > m->len) {
        return DAEMOON_ERR_IO_ERROR;
    }
    m->pos = (size_t)off;
    return DAEMOON_OK;
}

static daemoon_result_t mem_close(void *ctx)
{
    (void)ctx;
    return DAEMOON_OK;
}

static void mem_stream(daemoon_stream_t *s, mem_read_t *m, const void *data, size_t len)
{
    m->data = (const unsigned char *)data;
    m->len = len;
    m->pos = 0;

    memset(s, 0, sizeof(*s));
    s->read = mem_read;
    s->seek = mem_seek;
    s->close = mem_close;
    s->size = len;
    s->ctx = m;
}

/* ----------------------------------------------------------------- storage */

daemoon_result_t fake_server_put(fake_server_t *s, daemoon_platform_t platform,
                                 const char *title_id, const void *blob, size_t blob_len,
                                 const char *device_label)
{
    fake_title_t *t = ensure_title(s, platform, title_id);
    fake_version_t *v;
    daemoon_manifest_t m;
    daemoon_stream_t stream;
    mem_read_t reader;

    if (t == NULL || t->nversions >= FAKE_MAX_VERSIONS) {
        return DAEMOON_ERR_BUFFER_TOO_SMALL;
    }

    mem_stream(&stream, &reader, blob, blob_len);
    DAEMOON_TRY(daemoon_archive_read_manifest(&stream, &m));

    v = &t->versions[t->nversions++];
    memset(v, 0, sizeof(*v));
    v->version = t->latest_version + 1;
    v->parent_version = m.parent_version;
    v->size = m.size;
    (void)daemoon_strlcpy(v->sha256, sizeof(v->sha256), m.sha256);
    (void)daemoon_strlcpy(v->device_label, sizeof(v->device_label),
                          device_label != NULL ? device_label : m.device_label);
    (void)daemoon_strlcpy(v->received_at, sizeof(v->received_at), "2026-01-01T00:00:00Z");

    v->blob = (unsigned char *)malloc(blob_len);
    if (v->blob == NULL) {
        t->nversions--;
        return DAEMOON_ERR_OUT_OF_MEMORY;
    }
    memcpy(v->blob, blob, blob_len);
    v->blob_len = blob_len;

    t->latest_version = v->version;
    return DAEMOON_OK;
}

const fake_version_t *fake_server_latest(const fake_server_t *s, daemoon_platform_t platform,
                                         const char *title_id)
{
    const fake_title_t *t = find_title_const(s, platform, title_id);

    if (t == NULL || t->nversions == 0) {
        return NULL;
    }
    return &t->versions[t->nversions - 1];
}

/* ------------------------------------------------------------------ routing */

typedef struct {
    char               title_id[DAEMOON_TITLE_ID_MAX];
    daemoon_platform_t platform;
    unsigned int       version;
    unsigned int       parent_version;
    int                is_latest;
    int                is_blob;
} fake_route_t;

static unsigned int parse_uint(const char *s)
{
    unsigned int v = 0;

    while (*s >= '0' && *s <= '9') {
        v = v * 10u + (unsigned int)(*s - '0');
        ++s;
    }
    return v;
}

static const char *find_query(const char *url, const char *key)
{
    const char *q = strchr(url, '?');
    size_t klen = strlen(key);

    while (q != NULL) {
        const char *p = q + 1;
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            return p + klen + 1;
        }
        q = strchr(p, '&');
    }
    return NULL;
}

static int parse_route(const char *url, fake_route_t *out)
{
    const char *p = strstr(url, "/v1/titles/");
    const char *slash;
    const char *plat;
    size_t n;

    memset(out, 0, sizeof(*out));
    if (p == NULL) {
        return -1;
    }
    p += strlen("/v1/titles/");

    slash = strchr(p, '/');
    if (slash == NULL) {
        return -1;
    }
    n = (size_t)(slash - p);
    if (n == 0 || n >= sizeof(out->title_id)) {
        return -1;
    }
    memcpy(out->title_id, p, n);
    out->title_id[n] = '\0';

    p = slash + 1;
    if (strncmp(p, "latest", 6) == 0) {
        out->is_latest = 1;
    } else if (strncmp(p, "blob", 4) == 0) {
        out->is_blob = 1;
        if (p[4] == '/') {
            out->version = parse_uint(p + 5);
        }
    } else {
        return -1;
    }

    plat = find_query(url, "platform");
    if (plat != NULL) {
        const char *end = plat;
        while (*end != '\0' && *end != '&') {
            ++end;
        }
        out->platform = daemoon_platform_parse(plat, (size_t)(end - plat));
    }

    plat = find_query(url, "parent_version");
    if (plat != NULL) {
        out->parent_version = parse_uint(plat);
    }
    return 0;
}

/* ----------------------------------------------------------------- replies */

static daemoon_result_t reply(daemoon_http_resp_t *resp, int status, const char *body,
                              size_t len)
{
    resp->status = status;
    resp->content_length = (long long)len;
    if (resp->body_write != NULL && len > 0) {
        return resp->body_write(resp->body_ctx, body, len);
    }
    return DAEMOON_OK;
}

static daemoon_result_t reply_error(daemoon_http_resp_t *resp, int status, const char *code)
{
    char body[256];
    daemoon_strbuf_t sb;

    daemoon_strbuf_init(&sb, body, sizeof(body));
    daemoon_strbuf_add(&sb, "{\"error\":{\"code\":\"");
    daemoon_strbuf_add_json(&sb, code);
    daemoon_strbuf_add(&sb, "\"}}");
    return reply(resp, status, body, sb.len);
}

static void write_meta(daemoon_strbuf_t *sb, const fake_title_t *t, const fake_version_t *v)
{
    daemoon_strbuf_add(sb, "{\"title_id\":\"");
    daemoon_strbuf_add_json(sb, t->title_id);
    daemoon_strbuf_add(sb, "\",\"platform\":\"");
    daemoon_strbuf_add_json(sb, daemoon_platform_name(t->platform));
    daemoon_strbuf_add(sb, "\",\"version\":");
    daemoon_strbuf_add_uint(sb, v->version);
    daemoon_strbuf_add(sb, ",\"parent_version\":");
    daemoon_strbuf_add_uint(sb, v->parent_version);
    daemoon_strbuf_add(sb, ",\"sha256\":\"");
    daemoon_strbuf_add_json(sb, v->sha256);
    daemoon_strbuf_add(sb, "\",\"size\":");
    daemoon_strbuf_add_uint(sb, v->size);
    daemoon_strbuf_add(sb, ",\"device_label\":\"");
    daemoon_strbuf_add_json(sb, v->device_label);
    daemoon_strbuf_add(sb, "\",\"received_at\":\"");
    daemoon_strbuf_add_json(sb, v->received_at);
    daemoon_strbuf_add(sb, "\"}");
}

static daemoon_result_t reply_conflict(daemoon_http_resp_t *resp, const fake_version_t *v,
                                       unsigned int parent_version)
{
    char body[512];
    daemoon_strbuf_t sb;

    /* Everything the client needs for ui->choose, and nothing discarded on this
     * side either. */
    daemoon_strbuf_init(&sb, body, sizeof(body));
    daemoon_strbuf_add(&sb, "{\"error\":{\"code\":\"version_conflict\",\"detail\":{"
                            "\"server_version\":");
    daemoon_strbuf_add_uint(&sb, v->version);
    daemoon_strbuf_add(&sb, ",\"parent_version\":");
    daemoon_strbuf_add_uint(&sb, parent_version);
    daemoon_strbuf_add(&sb, ",\"server_size\":");
    daemoon_strbuf_add_uint(&sb, v->size);
    daemoon_strbuf_add(&sb, ",\"server_device_label\":\"");
    daemoon_strbuf_add_json(&sb, v->device_label);
    daemoon_strbuf_add(&sb, "\",\"server_received_at\":\"");
    daemoon_strbuf_add_json(&sb, v->received_at);
    daemoon_strbuf_add(&sb, "\"}}}");
    return reply(resp, 409, body, sb.len);
}

/* ------------------------------------------------------------------ handler */

static daemoon_result_t read_request_body(const daemoon_http_req_t *req, unsigned char **out,
                                          size_t *out_len)
{
    size_t cap = 64 * 1024;
    size_t len = 0;
    unsigned char *buf;

    *out = NULL;
    *out_len = 0;
    if (req->body_read == NULL) {
        return DAEMOON_OK;
    }

    buf = (unsigned char *)malloc(cap);
    if (buf == NULL) {
        return DAEMOON_ERR_OUT_OF_MEMORY;
    }
    for (;;) {
        size_t got = 0;
        daemoon_result_t r;

        if (len == cap) {
            unsigned char *bigger;
            cap *= 2;
            bigger = (unsigned char *)realloc(buf, cap);
            if (bigger == NULL) {
                free(buf);
                return DAEMOON_ERR_OUT_OF_MEMORY;
            }
            buf = bigger;
        }
        r = req->body_read(req->body_ctx, buf + len, cap - len, &got);
        if (r != DAEMOON_OK) {
            free(buf);
            return r;
        }
        if (got == 0) {
            break;
        }
        len += got;
    }

    *out = buf;
    *out_len = len;
    return DAEMOON_OK;
}

static daemoon_result_t fake_request(void *vctx, const daemoon_http_req_t *req,
                                     daemoon_http_resp_t *resp)
{
    fake_server_t *s = (fake_server_t *)vctx;
    fake_route_t route;
    fake_title_t *t;
    char body[1024];
    daemoon_strbuf_t sb;
    daemoon_result_t r;

    s->requests++;

    if (s->next_status != 0) {
        int status = s->next_status;
        s->next_status = 0;
        return reply_error(resp, status, status == 429 ? "rate_limited" : "internal_error");
    }

    if (parse_route(req->url, &route) != 0) {
        return reply_error(resp, 404, "not_found");
    }

    t = find_title(s, route.platform, route.title_id);

    if (route.is_latest) {
        if (t == NULL || t->nversions == 0) {
            /* Never uploaded is a normal state and the client treats it as such. */
            return reply_error(resp, 404, "not_found");
        }
        daemoon_strbuf_init(&sb, body, sizeof(body));
        write_meta(&sb, t, &t->versions[t->nversions - 1]);
        return reply(resp, 200, body, sb.len);
    }

    if (route.is_blob && strcmp(req->method, "GET") == 0) {
        size_t i;

        if (t == NULL) {
            return reply_error(resp, 404, "not_found");
        }
        for (i = 0; i < t->nversions; ++i) {
            if (t->versions[i].version == route.version) {
                s->downloads++;
                resp->status = 200;
                resp->content_length = (long long)t->versions[i].blob_len;
                (void)daemoon_strlcpy(resp->sha256, sizeof(resp->sha256), t->versions[i].sha256);
                if (resp->body_write != NULL) {
                    return resp->body_write(resp->body_ctx, t->versions[i].blob,
                                            t->versions[i].blob_len);
                }
                return DAEMOON_OK;
            }
        }
        return reply_error(resp, 404, "not_found");
    }

    if (route.is_blob && strcmp(req->method, "POST") == 0) {
        unsigned char *blob = NULL;
        size_t blob_len = 0;
        unsigned int latest = (t != NULL) ? t->latest_version : 0;

        if (route.parent_version != latest) {
            /* Nothing is discarded: the client is told what the server has and the
             * user decides. */
            s->conflicts++;
            if (t == NULL || t->nversions == 0) {
                return reply_error(resp, 409, "version_conflict");
            }
            return reply_conflict(resp, &t->versions[t->nversions - 1], route.parent_version);
        }

        r = read_request_body(req, &blob, &blob_len);
        if (r != DAEMOON_OK) {
            return r;
        }

        r = fake_server_put(s, route.platform, route.title_id, blob, blob_len, NULL);
        free(blob);
        if (r != DAEMOON_OK) {
            return reply_error(resp, 400, daemoon_result_code(r));
        }

        s->uploads++;
        t = find_title(s, route.platform, route.title_id);
        daemoon_strbuf_init(&sb, body, sizeof(body));
        write_meta(&sb, t, &t->versions[t->nversions - 1]);
        return reply(resp, 201, body, sb.len);
    }

    return reply_error(resp, 404, "not_found");
}

const daemoon_net_backend_t fake_server_net_backend = { fake_request };
