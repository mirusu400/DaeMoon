#include <daemoon/api.h>

#include <daemoon/util/strbuf.h>

#include "util/json.h"

#include <string.h>

/* Error bodies are small by design: a code and a flat detail object. Anything
 * larger than this is not something this client is going to understand anyway. */
#define API_ERRBUF_MAX 512
#define API_JSONBUF_MAX 1024
#define API_URL_MAX 512
#define API_TOKENS 64

/* ------------------------------------------------------------ small buffers */

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
    int    truncated;
} api_membuf_t;

static void membuf_init(api_membuf_t *mb, char *buf, size_t cap)
{
    mb->buf = buf;
    mb->cap = cap;
    mb->len = 0;
    mb->truncated = 0;
    if (cap > 0) {
        buf[0] = '\0';
    }
}

/* Routes the response body: a success body goes to the caller's sink, anything
 * else into a small buffer so the error code can be read out of it. Without this a
 * 409 body would be written into the file a download was streaming into. */
typedef struct {
    const daemoon_http_resp_t *resp;
    daemoon_stream_t          *sink; /* may be NULL when no success body is expected */
    api_membuf_t               ok_body;
    api_membuf_t               err_body;
    daemoon_result_t           err;
} api_sink_t;

static daemoon_result_t membuf_append(api_membuf_t *mb, const void *buf, size_t len)
{
    size_t room;

    if (mb->cap == 0) {
        mb->truncated = 1;
        return DAEMOON_OK;
    }
    room = mb->cap - mb->len - 1;
    if (len > room) {
        len = room;
        mb->truncated = 1;
    }
    memcpy(mb->buf + mb->len, buf, len);
    mb->len += len;
    mb->buf[mb->len] = '\0';
    return DAEMOON_OK;
}

static daemoon_result_t api_body_write(void *ctx, const void *buf, size_t len)
{
    api_sink_t *s = (api_sink_t *)ctx;
    int status = s->resp->status;

    if (s->err != DAEMOON_OK) {
        return s->err;
    }
    if (status == 0) {
        /* backend.h requires the status before the first body_write, because this
         * is where a save is told apart from an error message and there is no
         * asking again later.
         *
         * Said out loud rather than assumed. A backend that filled the status in
         * only after the transfer finished sent a successful upload's response
         * into the error buffer and left the success buffer empty, and the console
         * reported parse_error for an upload the server had accepted. A wrong
         * answer that looks like a parse failure is worse than a refusal. */
        s->err = DAEMOON_ERR_BACKEND_ERROR;
        return s->err;
    }
    if (status >= 200 && status < 300) {
        if (s->sink != NULL) {
            s->err = daemoon_stream_write(s->sink, buf, len);
            return s->err;
        }
        return membuf_append(&s->ok_body, buf, len);
    }
    return membuf_append(&s->err_body, buf, len);
}

/* ------------------------------------------------------------ request setup */

static daemoon_result_t build_base(const daemoon_env_t *env, daemoon_strbuf_t *sb, char *buf,
                                   size_t cap)
{
    if (env->server_url == NULL || env->server_url[0] == '\0') {
        return DAEMOON_ERR_INVALID_REQUEST;
    }
    daemoon_strbuf_init(sb, buf, cap);
    daemoon_strbuf_add(sb, env->server_url);
    return DAEMOON_OK;
}

static void add_platform_query(daemoon_strbuf_t *sb, daemoon_platform_t p, int first)
{
    daemoon_strbuf_addc(sb, first ? '?' : '&');
    daemoon_strbuf_add(sb, "platform=");
    daemoon_strbuf_add_urlenc(sb, daemoon_platform_name(p));
}

typedef struct {
    daemoon_http_header_t items[4];
    size_t                count;
    char                  auth[16 + DAEMOON_TOKEN_MAX];
} api_headers_t;

static void headers_init(api_headers_t *h, const daemoon_env_t *env, const char *content_type)
{
    h->count = 0;

    h->items[h->count].name = "Accept";
    h->items[h->count].value = "application/json";
    h->count++;

    if (content_type != NULL) {
        h->items[h->count].name = "Content-Type";
        h->items[h->count].value = content_type;
        h->count++;
    }

    if (env->token != NULL && env->token[0] != '\0') {
        daemoon_strbuf_t sb;
        daemoon_strbuf_init(&sb, h->auth, sizeof(h->auth));
        daemoon_strbuf_add(&sb, "Bearer ");
        daemoon_strbuf_add(&sb, env->token);
        if (daemoon_strbuf_result(&sb) == DAEMOON_OK) {
            h->items[h->count].name = "Authorization";
            h->items[h->count].value = h->auth;
            h->count++;
        }
    }
}

/* One request. Retries only when there is no request body to replay and only for
 * transport level failures, with a fixed ceiling: an upload is never retried
 * automatically because the server, not the client, decides what a second attempt
 * means. */
static daemoon_result_t api_do(const daemoon_env_t *env, daemoon_http_req_t *req,
                               api_sink_t *sink, daemoon_http_resp_t *resp)
{
    int attempt;
    daemoon_result_t r = DAEMOON_ERR_NETWORK_ERROR;
    int max_attempts = (req->body_read == NULL) ? DAEMOON_RETRY_CEILING : 1;

    if (env->net == NULL || env->net->request == NULL) {
        return DAEMOON_ERR_UNSUPPORTED;
    }

    for (attempt = 0; attempt < max_attempts; ++attempt) {
        resp->status = 0;
        resp->content_length = -1;
        resp->sha256[0] = '\0';
        sink->err = DAEMOON_OK;
        membuf_init(&sink->ok_body, sink->ok_body.buf, sink->ok_body.cap);
        membuf_init(&sink->err_body, sink->err_body.buf, sink->err_body.cap);

        r = env->net->request(env->net_ctx, req, resp);
        if (r == DAEMOON_OK) {
            break;
        }
        if (!daemoon_result_retryable(r)) {
            return r;
        }
    }
    if (r != DAEMOON_OK) {
        return r;
    }
    if (sink->err != DAEMOON_OK) {
        return sink->err;
    }
    return DAEMOON_OK;
}

/* ------------------------------------------------------------ error parsing */

daemoon_result_t daemoon_api_parse_error(const char *body, size_t len, int status,
                                         daemoon_conflict_t *out_conflict)
{
    jsmntok_t toks[API_TOKENS];
    char code[64];
    int ntok = 0;
    int err_obj, code_tok, detail;
    unsigned long long n = 0;

    if (body == NULL || len == 0) {
        return daemoon_result_from_http(status, NULL, 0);
    }
    if (daemoon_json_parse(body, len, toks, API_TOKENS, &ntok) != DAEMOON_OK) {
        /* A proxy or captive portal replacing the body must not turn into a
         * misleading error code. Fall back to the status. */
        return daemoon_result_from_http(status, NULL, 0);
    }

    err_obj = daemoon_json_find(body, toks, ntok, 0, "error");
    if (err_obj < 0 || toks[err_obj].type != JSMN_OBJECT) {
        return daemoon_result_from_http(status, NULL, 0);
    }
    code_tok = daemoon_json_find(body, toks, ntok, err_obj, "code");
    if (code_tok < 0 ||
        daemoon_json_str(body, &toks[code_tok], code, sizeof(code)) != DAEMOON_OK) {
        return daemoon_result_from_http(status, NULL, 0);
    }

    if (out_conflict != NULL) {
        detail = daemoon_json_find(body, toks, ntok, err_obj, "detail");
        if (detail >= 0 && toks[detail].type == JSMN_OBJECT) {
            memset(out_conflict, 0, sizeof(*out_conflict));
            if (daemoon_json_get_uint(body, toks, ntok, detail, "server_version", &n) == DAEMOON_OK) {
                out_conflict->server_version = (unsigned int)n;
            }
            if (daemoon_json_get_uint(body, toks, ntok, detail, "parent_version", &n) == DAEMOON_OK) {
                out_conflict->parent_version = (unsigned int)n;
            }
            if (daemoon_json_get_uint(body, toks, ntok, detail, "server_size", &n) == DAEMOON_OK) {
                out_conflict->server_size = n;
            }
            (void)daemoon_json_get_str(body, toks, ntok, detail, "server_device_label",
                                       out_conflict->server_device_label,
                                       sizeof(out_conflict->server_device_label));
            (void)daemoon_json_get_str(body, toks, ntok, detail, "server_received_at",
                                       out_conflict->server_received_at,
                                       sizeof(out_conflict->server_received_at));
        }
    }

    return daemoon_result_from_http(status, code, 0);
}

static daemoon_result_t parse_version_meta(const char *body, size_t len,
                                           daemoon_remote_meta_t *out)
{
    jsmntok_t toks[API_TOKENS];
    unsigned long long n = 0;
    int ntok = 0;
    int idx;

    DAEMOON_TRY(daemoon_json_parse(body, len, toks, API_TOKENS, &ntok));
    if (toks[0].type != JSMN_OBJECT) {
        return DAEMOON_ERR_PARSE_ERROR;
    }

    memset(out, 0, sizeof(*out));
    out->exists = 1;

    DAEMOON_TRY(daemoon_json_get_uint(body, toks, ntok, 0, "version", &n));
    out->latest_version = (unsigned int)n;

    idx = daemoon_json_find(body, toks, ntok, 0, "parent_version");
    if (idx >= 0 && !daemoon_json_is_null(body, &toks[idx]) &&
        daemoon_json_uint(body, &toks[idx], &n) == DAEMOON_OK) {
        out->parent_version = (unsigned int)n;
    }

    DAEMOON_TRY(daemoon_json_get_str(body, toks, ntok, 0, "sha256", out->sha256,
                                     sizeof(out->sha256)));
    DAEMOON_TRY(daemoon_json_get_uint(body, toks, ntok, 0, "size", &out->size));

    (void)daemoon_json_get_str(body, toks, ntok, 0, "device_label", out->device_label,
                               sizeof(out->device_label));
    (void)daemoon_json_get_str(body, toks, ntok, 0, "received_at", out->received_at,
                               sizeof(out->received_at));
    return DAEMOON_OK;
}

/* ------------------------------------------------------------------- calls */

daemoon_result_t daemoon_api_get_latest(const daemoon_env_t *env, daemoon_platform_t platform,
                                        const char *title_id, daemoon_remote_meta_t *out)
{
    char url[API_URL_MAX];
    char okbuf[API_JSONBUF_MAX];
    char errbuf[API_ERRBUF_MAX];
    daemoon_strbuf_t sb;
    daemoon_http_req_t req;
    daemoon_http_resp_t resp;
    api_headers_t headers;
    api_sink_t sink;

    if (env == NULL || title_id == NULL || out == NULL) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }

    DAEMOON_TRY(build_base(env, &sb, url, sizeof(url)));
    daemoon_strbuf_add(&sb, "/v1/titles/");
    daemoon_strbuf_add_urlenc(&sb, title_id);
    daemoon_strbuf_add(&sb, "/latest");
    add_platform_query(&sb, platform, 1);
    DAEMOON_TRY(daemoon_strbuf_result(&sb));

    headers_init(&headers, env, NULL);

    memset(&req, 0, sizeof(req));
    req.method = "GET";
    req.url = url;
    req.headers = headers.items;
    req.nheaders = headers.count;
    req.body_len = -1;
    req.timeout_ms = DAEMOON_DEFAULT_TIMEOUT_MS;

    memset(&resp, 0, sizeof(resp));
    memset(&sink, 0, sizeof(sink));
    sink.resp = &resp;
    membuf_init(&sink.ok_body, okbuf, sizeof(okbuf));
    membuf_init(&sink.err_body, errbuf, sizeof(errbuf));
    resp.body_write = api_body_write;
    resp.body_ctx = &sink;

    DAEMOON_TRY(api_do(env, &req, &sink, &resp));

    if (resp.status == 404) {
        /* Never uploaded is a normal state, not a failure. */
        memset(out, 0, sizeof(*out));
        out->exists = 0;
        return DAEMOON_OK;
    }
    if (resp.status < 200 || resp.status >= 300) {
        return daemoon_api_parse_error(sink.err_body.buf, sink.err_body.len, resp.status, NULL);
    }
    if (sink.ok_body.truncated) {
        return DAEMOON_ERR_PARSE_ERROR;
    }
    return parse_version_meta(sink.ok_body.buf, sink.ok_body.len, out);
}

daemoon_result_t daemoon_api_download(const daemoon_env_t *env, daemoon_platform_t platform,
                                      const char *title_id, unsigned int version,
                                      daemoon_stream_t *sink_stream, char *out_sha256,
                                      size_t sha_len)
{
    char url[API_URL_MAX];
    char errbuf[API_ERRBUF_MAX];
    daemoon_strbuf_t sb;
    daemoon_http_req_t req;
    daemoon_http_resp_t resp;
    api_headers_t headers;
    api_sink_t sink;

    if (env == NULL || title_id == NULL || sink_stream == NULL || version == 0) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }

    DAEMOON_TRY(build_base(env, &sb, url, sizeof(url)));
    daemoon_strbuf_add(&sb, "/v1/titles/");
    daemoon_strbuf_add_urlenc(&sb, title_id);
    daemoon_strbuf_add(&sb, "/blob/");
    daemoon_strbuf_add_uint(&sb, version);
    add_platform_query(&sb, platform, 1);
    DAEMOON_TRY(daemoon_strbuf_result(&sb));

    headers_init(&headers, env, NULL);

    memset(&req, 0, sizeof(req));
    req.method = "GET";
    req.url = url;
    req.headers = headers.items;
    req.nheaders = headers.count;
    req.body_len = -1;
    req.timeout_ms = DAEMOON_DEFAULT_TIMEOUT_MS;

    memset(&resp, 0, sizeof(resp));
    memset(&sink, 0, sizeof(sink));
    sink.resp = &resp;
    sink.sink = sink_stream;
    membuf_init(&sink.ok_body, NULL, 0);
    membuf_init(&sink.err_body, errbuf, sizeof(errbuf));
    resp.body_write = api_body_write;
    resp.body_ctx = &sink;

    /* A retry here would append to a partly written file, so this is single shot
     * even though a GET is idempotent. The caller decides whether to start over. */
    req.body_read = NULL;
    DAEMOON_TRY(env->net->request(env->net_ctx, &req, &resp));
    DAEMOON_TRY(sink.err);

    if (resp.status < 200 || resp.status >= 300) {
        return daemoon_api_parse_error(sink.err_body.buf, sink.err_body.len, resp.status, NULL);
    }
    if (out_sha256 != NULL && sha_len > 0) {
        (void)daemoon_strlcpy(out_sha256, sha_len, resp.sha256);
    }
    return DAEMOON_OK;
}

static daemoon_result_t stream_body_read(void *ctx, void *buf, size_t cap, size_t *out_len)
{
    return daemoon_stream_read((daemoon_stream_t *)ctx, buf, cap, out_len);
}

daemoon_result_t daemoon_api_upload(const daemoon_env_t *env, const daemoon_manifest_t *m,
                                    daemoon_stream_t *body, unsigned long long body_len,
                                    daemoon_remote_meta_t *out, daemoon_conflict_t *conflict)
{
    char url[API_URL_MAX];
    char okbuf[API_JSONBUF_MAX];
    char errbuf[API_ERRBUF_MAX];
    daemoon_strbuf_t sb;
    daemoon_http_req_t req;
    daemoon_http_resp_t resp;
    api_headers_t headers;
    api_sink_t sink;
    daemoon_result_t r;

    if (env == NULL || m == NULL || body == NULL || out == NULL) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }

    DAEMOON_TRY(build_base(env, &sb, url, sizeof(url)));
    daemoon_strbuf_add(&sb, "/v1/titles/");
    daemoon_strbuf_add_urlenc(&sb, m->title_id);
    daemoon_strbuf_add(&sb, "/blob?parent_version=");
    daemoon_strbuf_add_uint(&sb, m->parent_version);
    add_platform_query(&sb, m->platform, 0);
    DAEMOON_TRY(daemoon_strbuf_result(&sb));

    headers_init(&headers, env, "application/zip");

    memset(&req, 0, sizeof(req));
    req.method = "POST";
    req.url = url;
    req.headers = headers.items;
    req.nheaders = headers.count;
    req.body_read = stream_body_read;
    req.body_ctx = body;
    req.body_len = (long long)body_len;
    req.timeout_ms = DAEMOON_DEFAULT_TIMEOUT_MS;

    memset(&resp, 0, sizeof(resp));
    memset(&sink, 0, sizeof(sink));
    sink.resp = &resp;
    membuf_init(&sink.ok_body, okbuf, sizeof(okbuf));
    membuf_init(&sink.err_body, errbuf, sizeof(errbuf));
    resp.body_write = api_body_write;
    resp.body_ctx = &sink;

    DAEMOON_TRY(api_do(env, &req, &sink, &resp));

    if (resp.status < 200 || resp.status >= 300) {
        r = daemoon_api_parse_error(sink.err_body.buf, sink.err_body.len, resp.status, conflict);
        return r;
    }
    if (sink.ok_body.truncated) {
        return DAEMOON_ERR_PARSE_ERROR;
    }
    return parse_version_meta(sink.ok_body.buf, sink.ok_body.len, out);
}

typedef struct {
    const char *data;
    size_t      len;
    size_t      pos;
} api_const_body_t;

static daemoon_result_t const_body_read(void *ctx, void *buf, size_t cap, size_t *out_len)
{
    api_const_body_t *b = (api_const_body_t *)ctx;
    size_t n = b->len - b->pos;

    if (n > cap) {
        n = cap;
    }
    memcpy(buf, b->data + b->pos, n);
    b->pos += n;
    *out_len = n;
    return DAEMOON_OK;
}

daemoon_result_t daemoon_api_pair(const daemoon_env_t *env, const char *grant, const char *code,
                                  const char *label, daemoon_platform_t platform,
                                  char *out_token, size_t token_len,
                                  char *out_device_id, size_t device_id_len)
{
    char url[API_URL_MAX];
    char reqbody[512];
    char okbuf[API_JSONBUF_MAX];
    char errbuf[API_ERRBUF_MAX];
    jsmntok_t toks[API_TOKENS];
    daemoon_strbuf_t sb;
    daemoon_http_req_t req;
    daemoon_http_resp_t resp;
    api_headers_t headers;
    api_sink_t sink;
    api_const_body_t body;
    int ntok = 0;

    if (env == NULL || grant == NULL || code == NULL || label == NULL || out_token == NULL) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }

    DAEMOON_TRY(build_base(env, &sb, url, sizeof(url)));
    daemoon_strbuf_add(&sb, "/v1/devices/pair");
    DAEMOON_TRY(daemoon_strbuf_result(&sb));

    daemoon_strbuf_init(&sb, reqbody, sizeof(reqbody));
    daemoon_strbuf_add(&sb, "{\"grant\":\"");
    daemoon_strbuf_add_json(&sb, grant);
    daemoon_strbuf_add(&sb, "\",\"code\":\"");
    daemoon_strbuf_add_json(&sb, code);
    daemoon_strbuf_add(&sb, "\",\"label\":\"");
    daemoon_strbuf_add_json(&sb, label);
    daemoon_strbuf_add(&sb, "\",\"platform\":\"");
    daemoon_strbuf_add_json(&sb, daemoon_platform_name(platform));
    daemoon_strbuf_add(&sb, "\"}");
    DAEMOON_TRY(daemoon_strbuf_result(&sb));

    body.data = reqbody;
    body.len = sb.len;
    body.pos = 0;

    headers_init(&headers, env, "application/json");

    memset(&req, 0, sizeof(req));
    req.method = "POST";
    req.url = url;
    req.headers = headers.items;
    req.nheaders = headers.count;
    req.body_read = const_body_read;
    req.body_ctx = &body;
    req.body_len = (long long)body.len;
    req.timeout_ms = DAEMOON_DEFAULT_TIMEOUT_MS;

    memset(&resp, 0, sizeof(resp));
    memset(&sink, 0, sizeof(sink));
    sink.resp = &resp;
    membuf_init(&sink.ok_body, okbuf, sizeof(okbuf));
    membuf_init(&sink.err_body, errbuf, sizeof(errbuf));
    resp.body_write = api_body_write;
    resp.body_ctx = &sink;

    DAEMOON_TRY(api_do(env, &req, &sink, &resp));

    if (resp.status < 200 || resp.status >= 300) {
        /* pairing_pending is retryable and the caller polls with a ceiling. */
        return daemoon_api_parse_error(sink.err_body.buf, sink.err_body.len, resp.status, NULL);
    }

    DAEMOON_TRY(daemoon_json_parse(sink.ok_body.buf, sink.ok_body.len, toks, API_TOKENS, &ntok));
    DAEMOON_TRY(daemoon_json_get_str(sink.ok_body.buf, toks, ntok, 0, "token", out_token,
                                     token_len));
    if (out_device_id != NULL && device_id_len > 0) {
        (void)daemoon_json_get_str(sink.ok_body.buf, toks, ntok, 0, "device_id", out_device_id,
                                   device_id_len);
    }
    return DAEMOON_OK;
}

/* ------------------------------------------------------------- pairing QR */

/* `DAEMOON|1|<server>|<code>`.
 *
 * Every field is bounded and the whole thing is refused unless all four parts are
 * present and the version is one this build knows. A payload that is misread is
 * worse than one that is refused: it points a console at an address somebody else
 * chose, and the next thing that happens is a save being uploaded there.
 */
daemoon_result_t daemoon_pair_parse(const char *text, size_t len,
                                    daemoon_pair_payload_t *out)
{
    static const char tag[] = DAEMOON_PAIR_TAG "|";
    const char *p;
    const char *end;
    const char *bar;
    size_t n;

    if (text == NULL || out == NULL) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }
    memset(out, 0, sizeof(*out));

    end = text + len;
    if (len < sizeof(tag) || memcmp(text, tag, sizeof(tag) - 1) != 0) {
        return DAEMOON_ERR_PARSE_ERROR;
    }
    p = text + sizeof(tag) - 1;

    /* The format version. Not a range check: an unknown version means the rest of
     * the payload is laid out some other way, and guessing is the failure this
     * whole shape exists to prevent. */
    bar = (const char *)memchr(p, '|', (size_t)(end - p));
    if (bar == NULL || bar == p) {
        return DAEMOON_ERR_PARSE_ERROR;
    }
    {
        unsigned long long version = 0;

        for (n = 0; n < (size_t)(bar - p); ++n) {
            if (p[n] < '0' || p[n] > '9') {
                return DAEMOON_ERR_PARSE_ERROR;
            }
            version = version * 10u + (unsigned long long)(p[n] - '0');
            if (version > 999u) {
                return DAEMOON_ERR_PARSE_ERROR;
            }
        }
        if (version != (unsigned long long)DAEMOON_PAIR_FORMAT) {
            return DAEMOON_ERR_UNSUPPORTED;
        }
    }
    p = bar + 1;

    bar = (const char *)memchr(p, '|', (size_t)(end - p));
    if (bar == NULL || bar == p) {
        return DAEMOON_ERR_PARSE_ERROR;
    }
    n = (size_t)(bar - p);
    if (n >= sizeof(out->server)) {
        return DAEMOON_ERR_BUFFER_TOO_SMALL;
    }
    memcpy(out->server, p, n);
    out->server[n] = '\0';
    /* Only the two schemes this client can speak. A payload naming anything else
     * is either a different product's QR code or an attempt to be clever. */
    if (strncmp(out->server, "http://", 7) != 0 &&
        strncmp(out->server, "https://", 8) != 0) {
        return DAEMOON_ERR_PARSE_ERROR;
    }
    p = bar + 1;

    /* The code runs to the end. A trailing bar would leave it empty, which is
     * caught by the length check below. */
    n = (size_t)(end - p);
    while (n > 0 && (p[n - 1] == '\n' || p[n - 1] == '\r' || p[n - 1] == '\0')) {
        --n;
    }
    if (n == 0 || n >= sizeof(out->code)) {
        return DAEMOON_ERR_PARSE_ERROR;
    }
    if (memchr(p, '|', n) != NULL) {
        return DAEMOON_ERR_PARSE_ERROR;
    }
    memcpy(out->code, p, n);
    out->code[n] = '\0';

    return DAEMOON_OK;
}

/* ------------------------------------------------------------ revocation */

daemoon_result_t daemoon_api_revoke_device(const daemoon_env_t *env, const char *token,
                                           const char *device_id)
{
    char url[API_URL_MAX];
    char errbuf[API_ERRBUF_MAX];
    char okbuf[16];
    daemoon_strbuf_t sb;
    daemoon_http_req_t req;
    daemoon_http_resp_t resp;
    api_headers_t headers;
    api_sink_t sink;
    daemoon_env_t as_old;

    if (env == NULL || token == NULL || device_id == NULL ||
        token[0] == '\0' || device_id[0] == '\0') {
        return DAEMOON_ERR_INVALID_REQUEST;
    }

    DAEMOON_TRY(build_base(env, &sb, url, sizeof(url)));
    daemoon_strbuf_add(&sb, "/v1/devices/");
    daemoon_strbuf_add_urlenc(&sb, device_id);
    DAEMOON_TRY(daemoon_strbuf_result(&sb));

    /* Authenticated as the token being retired rather than as whatever the caller
     * is holding now. The whole point is to spend the old credential on itself,
     * and by the time this is called the environment already carries the new one. */
    as_old = *env;
    as_old.token = token;
    headers_init(&headers, &as_old, NULL);

    memset(&req, 0, sizeof(req));
    req.method = "DELETE";
    req.url = url;
    req.headers = headers.items;
    req.nheaders = headers.count;
    req.body_len = -1;
    req.timeout_ms = DAEMOON_DEFAULT_TIMEOUT_MS;

    memset(&resp, 0, sizeof(resp));
    memset(&sink, 0, sizeof(sink));
    sink.resp = &resp;
    membuf_init(&sink.ok_body, okbuf, sizeof(okbuf));
    membuf_init(&sink.err_body, errbuf, sizeof(errbuf));
    resp.body_write = api_body_write;
    resp.body_ctx = &sink;

    DAEMOON_TRY(api_do(&as_old, &req, &sink, &resp));

    if (resp.status == 404) {
        /* Already gone. The caller wanted it gone. */
        return DAEMOON_OK;
    }
    if (resp.status < 200 || resp.status >= 300) {
        return daemoon_api_parse_error(sink.err_body.buf, sink.err_body.len,
                                       resp.status, NULL);
    }
    return DAEMOON_OK;
}
