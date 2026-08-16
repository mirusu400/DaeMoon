/* The 3DS network backend: libcurl over 3ds-curl and 3ds-mbedtls.
 *
 * Not httpc:C. That service ships old cipher suites and a root CA store that was
 * current in 2011, and it fails against ordinary modern servers - including, in
 * practice, anything behind Let's Encrypt. A self hosted server is exactly the
 * case where the operator cannot be asked to downgrade their TLS to suit a
 * console, so the TLS stack is linked in rather than borrowed from the system.
 *
 * Bodies stream in both directions. A save never exists whole in memory here: the
 * request body is pulled from a daemoon_read_fn as curl asks for it, and the
 * response is pushed into a daemoon_write_fn as it arrives.
 */
#include "daemoon_3ds.h"

#include <daemoon/util/strbuf.h>

#include <3ds.h>
#include <curl/curl.h>

#include <malloc.h>
#include <stdio.h>
#include <string.h>

/* soc:U wants a page aligned buffer it keeps for the lifetime of the session.
 * 128 KiB is what the 3DS examples use and is enough for one connection at a
 * time, which is all this ever opens. */
#define SOC_BUFFER_SIZE  (128 * 1024)
#define SOC_BUFFER_ALIGN 0x1000

static u32 *g_soc_buffer;
static int  g_soc_ready;

daemoon_result_t daemoon_3ds_net_init(void)
{
    Result res;

    if (g_soc_ready) {
        return DAEMOON_OK;
    }

    g_soc_buffer = (u32 *)memalign(SOC_BUFFER_ALIGN, SOC_BUFFER_SIZE);
    if (g_soc_buffer == NULL) {
        return DAEMOON_ERR_OUT_OF_MEMORY;
    }

    res = socInit(g_soc_buffer, SOC_BUFFER_SIZE);
    if (R_FAILED(res)) {
        free(g_soc_buffer);
        g_soc_buffer = NULL;
        return DAEMOON_ERR_NETWORK_ERROR;
    }

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        socExit();
        free(g_soc_buffer);
        g_soc_buffer = NULL;
        return DAEMOON_ERR_NETWORK_ERROR;
    }

    g_soc_ready = 1;
    return DAEMOON_OK;
}

void daemoon_3ds_net_exit(void)
{
    if (!g_soc_ready) {
        return;
    }
    curl_global_cleanup();
    socExit();
    free(g_soc_buffer);
    g_soc_buffer = NULL;
    g_soc_ready = 0;
}

/* --------------------------------------------------------------- callbacks */

typedef struct {
    const daemoon_http_req_t *req;
    daemoon_result_t          err;
} upload_ctx_t;

static size_t on_read(char *buffer, size_t size, size_t count, void *user)
{
    upload_ctx_t *up = (upload_ctx_t *)user;
    size_t want = size * count;
    size_t got = 0;

    if (up->err != DAEMOON_OK || up->req->body_read == NULL) {
        return 0;
    }
    up->err = up->req->body_read(up->req->body_ctx, buffer, want, &got);
    if (up->err != DAEMOON_OK) {
        /* Aborting the transfer beats sending a body that is missing its middle:
         * the server would store a package whose digest does not match and hand
         * it back to a console later. */
        return CURL_READFUNC_ABORT;
    }
    return got;
}

typedef struct {
    const daemoon_http_resp_t *resp;
    daemoon_result_t           err;
} download_ctx_t;

static size_t on_write(char *buffer, size_t size, size_t count, void *user)
{
    download_ctx_t *down = (download_ctx_t *)user;
    size_t len = size * count;

    if (down->err != DAEMOON_OK) {
        return 0;
    }
    if (down->resp->body_write == NULL) {
        return len;
    }
    down->err = down->resp->body_write(down->resp->body_ctx, buffer, len);
    if (down->err != DAEMOON_OK) {
        /* Returning short tells curl to stop, which is what a full SD card
         * should do to a download. */
        return 0;
    }
    return len;
}

static size_t on_header(char *buffer, size_t size, size_t count, void *user)
{
    daemoon_http_resp_t *resp = (daemoon_http_resp_t *)user;
    size_t len = size * count;
    static const char wanted[] = "x-daemoon-sha256:";
    size_t i;

    /* The status line, which curl delivers as the first header of a response.
     *
     * backend.h requires the status before the first body_write, and reading it
     * from curl_easy_getinfo after curl_easy_perform returns is far too late:
     * every callback has already run. Getting this wrong put a successful upload's
     * response into the caller's error buffer and left the success buffer empty,
     * which the console reported as parse_error for an upload the server had
     * accepted.
     *
     * A response can begin more than once - a 100-continue, or a redirect - so the
     * per response fields are reset here rather than only before the request. */
    if (len > 8 && buffer[0] == 'H' && buffer[1] == 'T' && buffer[2] == 'T' &&
        buffer[3] == 'P' && buffer[4] == '/') {
        const char *space = (const char *)memchr(buffer, ' ', len);

        if (space != NULL) {
            int code = 0;
            size_t n;

            for (n = 1; n < len - (size_t)(space - buffer) && space[n] >= '0' &&
                        space[n] <= '9';
                 ++n) {
                code = code * 10 + (space[n] - '0');
            }
            if (code >= 100 && code < 600) {
                resp->status = code;
                resp->sha256[0] = '\0';
                resp->content_length = -1;
            }
        }
        return len;
    }

    /* Case insensitive, because a header name is. */
    if (len <= sizeof(wanted) - 1) {
        return len;
    }
    for (i = 0; i < sizeof(wanted) - 1; ++i) {
        char c = buffer[i];

        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != wanted[i]) {
            return len;
        }
    }

    {
        const char *value = buffer + sizeof(wanted) - 1;
        size_t remaining = len - (sizeof(wanted) - 1);
        size_t n = 0;

        while (remaining > 0 && (*value == ' ' || *value == '\t')) {
            ++value;
            --remaining;
        }
        while (n < remaining && value[n] != '\r' && value[n] != '\n' &&
               n + 1 < sizeof(resp->sha256)) {
            ++n;
        }
        memcpy(resp->sha256, value, n);
        resp->sha256[n] = '\0';
    }
    return len;
}

/* ------------------------------------------------------------------ request */

static daemoon_result_t from_curl(CURLcode code)
{
    switch (code) {
    case CURLE_OK:
        return DAEMOON_OK;
    case CURLE_OPERATION_TIMEDOUT:
        return DAEMOON_ERR_TIMEOUT;
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_RESOLVE_PROXY:
    case CURLE_COULDNT_CONNECT:
    case CURLE_SEND_ERROR:
    case CURLE_RECV_ERROR:
    case CURLE_GOT_NOTHING:
        return DAEMOON_ERR_NETWORK_ERROR;
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_SSL_CACERT_BADFILE:
        /* Kept apart from a plain network failure on purpose. A certificate that
         * does not verify is the one network error a user can act on, and the one
         * this project is most likely to hit: a self hosted server behind a
         * certificate the console has never heard of. */
        return DAEMOON_ERR_TLS_ERROR;
    case CURLE_OUT_OF_MEMORY:
        return DAEMOON_ERR_OUT_OF_MEMORY;
    default:
        return DAEMOON_ERR_NETWORK_ERROR;
    }
}

static daemoon_result_t net_request(void *vctx, const daemoon_http_req_t *req,
                                    daemoon_http_resp_t *resp)
{
    daemoon_3ds_net_ctx_t *ctx = (daemoon_3ds_net_ctx_t *)vctx;
    struct curl_slist *headers = NULL;
    upload_ctx_t up;
    download_ctx_t down;
    CURL *curl;
    CURLcode code;
    long status = 0;
    size_t i;
    daemoon_result_t r;

    if (!g_soc_ready) {
        return DAEMOON_ERR_NETWORK_ERROR;
    }

    curl = curl_easy_init();
    if (curl == NULL) {
        return DAEMOON_ERR_OUT_OF_MEMORY;
    }

    up.req = req;
    up.err = DAEMOON_OK;
    down.resp = resp;
    down.err = DAEMOON_OK;

    resp->status = 0;
    resp->content_length = -1;
    resp->sha256[0] = '\0';

    for (i = 0; i < req->nheaders; ++i) {
        char line[256];
        daemoon_strbuf_t sb;
        struct curl_slist *next;

        daemoon_strbuf_init(&sb, line, sizeof(line));
        daemoon_strbuf_add(&sb, req->headers[i].name);
        daemoon_strbuf_add(&sb, ": ");
        daemoon_strbuf_add(&sb, req->headers[i].value);
        if (daemoon_strbuf_result(&sb) != DAEMOON_OK) {
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            return DAEMOON_ERR_BUFFER_TOO_SMALL;
        }
        next = curl_slist_append(headers, line);
        if (next == NULL) {
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            return DAEMOON_ERR_OUT_OF_MEMORY;
        }
        headers = next;
    }
    /* curl would otherwise announce a 100-continue on a large upload and wait a
     * second for a reply many servers never send. */
    headers = curl_slist_append(headers, "Expect:");

    curl_easy_setopt(curl, CURLOPT_URL, req->url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &down);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, resp);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "DaeMoon/1.0 (3DS)");

    /* No unbounded waits. Both are set, because a connection that establishes and
     * then stalls is the failure a console on hotel wifi actually has. */
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,
                     (long)(req->timeout_ms > 0 ? req->timeout_ms : 30000));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 15000L);

    /* The console has no CA store worth the name, so one is carried on the SD
     * card. Verification stays on: a save is the sort of thing that should not be
     * handed to whoever answers the connection. */
    if (ctx != NULL && ctx->ca_bundle != NULL && ctx->ca_bundle[0] != '\0') {
        curl_easy_setopt(curl, CURLOPT_CAINFO, ctx->ca_bundle);
    }
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    if (strcmp(req->method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_READFUNCTION, on_read);
        curl_easy_setopt(curl, CURLOPT_READDATA, &up);
        if (req->body_len >= 0) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                             (curl_off_t)req->body_len);
        }
    } else if (strcmp(req->method, "DELETE") == 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    } else {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    }

    code = curl_easy_perform(curl);
    if (code == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        resp->status = (int)status;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    /* A failure in one of the callbacks is more specific than whatever curl
     * concluded from it, so it wins. */
    if (up.err != DAEMOON_OK) {
        return up.err;
    }
    if (down.err != DAEMOON_OK) {
        return down.err;
    }

    r = from_curl(code);
    if (r != DAEMOON_OK && ctx != NULL) {
        ctx->last_curl_code = (int)code;
    }
    return r;
}

const daemoon_net_backend_t daemoon_3ds_net_backend = { net_request };
