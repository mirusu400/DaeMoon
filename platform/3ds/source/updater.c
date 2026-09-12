/* In-place updater for the installed CIA.
 *
 * The fixed URLs live on the project server. The version endpoint is a few bytes,
 * and the CIA endpoint redirects to the current nightly release asset. The CIA is
 * written straight into AM's import handle, so a one-megabyte update never has to
 * fit in memory or remain as a temporary file on the SD card. */
#include "daemoon_3ds.h"

#include <3ds.h>
#include <curl/curl.h>

#include <limits.h>
#include <string.h>

#define UPDATE_VERSION_URL "https://daemoon.mir.sh/install/3ds.version"
#define UPDATE_CIA_URL     "https://daemoon.mir.sh/install/3ds.cia"

typedef struct {
    char   *buf;
    size_t  cap;
    size_t  len;
    int     full;
} text_sink_t;

static size_t write_text(char *data, size_t size, size_t count, void *user)
{
    text_sink_t *sink = (text_sink_t *)user;
    size_t len = size * count;

    if (len > sink->cap - sink->len - 1) {
        sink->full = 1;
        return 0;
    }
    memcpy(sink->buf + sink->len, data, len);
    sink->len += len;
    sink->buf[sink->len] = '\0';
    return len;
}

static void common_options(CURL *curl, const char *url)
{
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "DaeMoon-Updater/1.0 (3DS)");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 120000L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
}

static daemoon_result_t curl_result(CURLcode code)
{
    if (code == CURLE_OK) {
        return DAEMOON_OK;
    }
    if (code == CURLE_OPERATION_TIMEDOUT) {
        return DAEMOON_ERR_TIMEOUT;
    }
    if (code == CURLE_OUT_OF_MEMORY) {
        return DAEMOON_ERR_OUT_OF_MEMORY;
    }
    return DAEMOON_ERR_NETWORK_ERROR;
}

daemoon_result_t daemoon_3ds_update_available(const char *current_build,
                                               char *latest, size_t latest_cap,
                                               int *out_available)
{
    text_sink_t sink;
    CURL *curl;
    CURLcode code;
    size_t i;

    if (current_build == NULL || latest == NULL || latest_cap < 2 ||
        out_available == NULL) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }
    latest[0] = '\0';
    *out_available = 0;
    DAEMOON_TRY(daemoon_net_curl_init());

    curl = curl_easy_init();
    if (curl == NULL) {
        return DAEMOON_ERR_OUT_OF_MEMORY;
    }
    memset(&sink, 0, sizeof(sink));
    sink.buf = latest;
    sink.cap = latest_cap;

    common_options(curl, UPDATE_VERSION_URL);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_text);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    code = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (code != CURLE_OK) {
        return curl_result(code);
    }
    if (sink.full || sink.len == 0) {
        return DAEMOON_ERR_PARSE_ERROR;
    }
    while (sink.len > 0 && (latest[sink.len - 1] == '\r' ||
                            latest[sink.len - 1] == '\n' ||
                            latest[sink.len - 1] == ' ')) {
        latest[--sink.len] = '\0';
    }
    if (sink.len == 0) {
        return DAEMOON_ERR_PARSE_ERROR;
    }
    for (i = 0; i < sink.len; ++i) {
        const char c = latest[i];

        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') || c == '.' || c == '_' || c == '-')) {
            return DAEMOON_ERR_PARSE_ERROR;
        }
    }
    *out_available = strstr(current_build, latest) == NULL;
    return DAEMOON_OK;
}

typedef struct {
    Handle handle;
    u64 offset;
    daemoon_result_t error;
    daemoon_3ds_update_progress_fn progress;
    void *progress_user;
} cia_sink_t;

static size_t write_cia(char *data, size_t size, size_t count, void *user)
{
    cia_sink_t *sink = (cia_sink_t *)user;
    size_t len = size * count;
    u32 wrote = 0;

    if (len > UINT_MAX || R_FAILED(FSFILE_Write(sink->handle, &wrote, sink->offset,
                                                data, (u32)len, 0)) ||
        wrote != (u32)len) {
        sink->error = DAEMOON_ERR_IO_ERROR;
        return 0;
    }
    sink->offset += wrote;
    return len;
}

static int update_progress(void *user, curl_off_t total, curl_off_t now,
                           curl_off_t upload_total, curl_off_t upload_now)
{
    cia_sink_t *sink = (cia_sink_t *)user;
    unsigned done;
    unsigned all;

    (void)upload_total;
    (void)upload_now;
    done = now > (curl_off_t)UINT_MAX ? UINT_MAX : (unsigned)now;
    all = total > (curl_off_t)UINT_MAX ? UINT_MAX : (unsigned)total;
    if (sink->progress != NULL) {
        sink->progress(sink->progress_user, done, all);
    }
    return aptMainLoop() ? 0 : 1;
}

daemoon_result_t daemoon_3ds_update_install(daemoon_3ds_update_progress_fn progress,
                                             void *progress_user)
{
    cia_sink_t sink;
    CURL *curl;
    CURLcode code;
    Result result;

    DAEMOON_TRY(daemoon_net_curl_init());
    memset(&sink, 0, sizeof(sink));
    sink.error = DAEMOON_OK;
    sink.progress = progress;
    sink.progress_user = progress_user;

    result = AM_StartCiaInstallOverwrite(&sink.handle, MEDIATYPE_SD);
    if (R_FAILED(result)) {
        return DAEMOON_ERR_IO_ERROR;
    }
    curl = curl_easy_init();
    if (curl == NULL) {
        (void)AM_CancelCIAInstall(sink.handle);
        return DAEMOON_ERR_OUT_OF_MEMORY;
    }

    common_options(curl, UPDATE_CIA_URL);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cia);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, update_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &sink);
    code = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (sink.error != DAEMOON_OK || code != CURLE_OK) {
        (void)AM_CancelCIAInstall(sink.handle);
        return sink.error != DAEMOON_OK ? sink.error : curl_result(code);
    }
    result = AM_FinishCiaInstall(sink.handle);
    return R_SUCCEEDED(result) ? DAEMOON_OK : DAEMOON_ERR_IO_ERROR;
}
