/* api.h - the client side of shared/openapi.yaml.
 *
 * shared/openapi.yaml is authoritative. Changing anything here means changing it in
 * the same commit.
 *
 * Every call goes through env->net, so this file has no idea whether it is talking
 * over 3ds-curl, libnx or a test double. Bodies stream in both directions: nothing
 * assembles a save in memory.
 */
#ifndef DAEMOON_API_H
#define DAEMOON_API_H

#include <stddef.h>

#include <daemoon/backend.h>
#include <daemoon/manifest.h>
#include <daemoon/result.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DAEMOON_TOKEN_MAX      129
#define DAEMOON_DEVICE_ID_MAX  65
#define DAEMOON_DEFAULT_TIMEOUT_MS 30000
#define DAEMOON_RETRY_CEILING       3 /* total attempts, never unbounded */

/* What the server holds for one title. */
typedef struct {
    int                exists;
    unsigned int       latest_version;
    unsigned int       parent_version;
    char               sha256[DAEMOON_SHA256_HEX];
    unsigned long long size;
    char               device_label[DAEMOON_LABEL_MAX];
    char               received_at[DAEMOON_TIMESTAMP_MAX]; /* server clock, informational */
} daemoon_remote_meta_t;

/* The detail of a 409. Both versions are retained on the server, so this is only
 * what the user needs in order to choose. */
typedef struct {
    unsigned int       server_version;
    unsigned int       parent_version;
    unsigned long long server_size;
    char               server_device_label[DAEMOON_LABEL_MAX];
    char               server_received_at[DAEMOON_TIMESTAMP_MAX];
} daemoon_conflict_t;

/* GET /v1/titles/{tid}/latest. A title the server has never seen is not an error:
 * out->exists is 0 and DAEMOON_OK comes back. */
daemoon_result_t daemoon_api_get_latest(const daemoon_env_t *env, daemoon_platform_t platform,
                                        const char *title_id, daemoon_remote_meta_t *out);

/* GET /v1/titles/{tid}/blob/{v}, streamed into sink. The digest advertised by the
 * response header is written to out_sha256 when it is non NULL; it is a hint for
 * logging only, and the real check is daemoon_archive_verify against the manifest
 * inside the package. */
daemoon_result_t daemoon_api_download(const daemoon_env_t *env, daemoon_platform_t platform,
                                      const char *title_id, unsigned int version,
                                      daemoon_stream_t *sink, char *out_sha256, size_t sha_len);

/* POST /v1/titles/{tid}/blob. body is the package, already staged as a file so its
 * length is known. On a version conflict this returns DAEMOON_ERR_VERSION_CONFLICT
 * and fills conflict; nothing was discarded on either side. */
daemoon_result_t daemoon_api_upload(const daemoon_env_t *env, const daemoon_manifest_t *m,
                                    daemoon_stream_t *body, unsigned long long body_len,
                                    daemoon_remote_meta_t *out, daemoon_conflict_t *conflict);

/* POST /v1/devices/pair. grant is "qr" or "device_code". While the user has not
 * approved yet the server answers pairing_pending, which is retryable; the caller
 * polls with a ceiling and never waits without a bound. */
daemoon_result_t daemoon_api_pair(const daemoon_env_t *env, const char *grant, const char *code,
                                  const char *label, daemoon_platform_t platform,
                                  char *out_token, size_t token_len,
                                  char *out_device_id, size_t device_id_len);

/* Parse an error body: {"error":{"code":"...","detail":{...}}}. Used by the calls
 * above and exposed for tests. A body that cannot be parsed maps to the status. */
daemoon_result_t daemoon_api_parse_error(const char *body, size_t len, int status,
                                         daemoon_conflict_t *out_conflict);

#ifdef __cplusplus
}
#endif

#endif /* DAEMOON_API_H */
