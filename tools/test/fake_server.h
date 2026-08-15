/* An in-process stand in for daemoond.
 *
 * It implements just enough of shared/openapi.yaml for the sync path: latest,
 * download, upload with the version check. It keeps every version it is given,
 * because "both versions are retained" is a rule and a test double that quietly
 * dropped one would let a regression through.
 *
 * It is a test double, not a reference implementation. The real server is in
 * server/, and the two are held together by shared/fixtures and shared/openapi.yaml.
 */
#ifndef DAEMOON_TEST_FAKE_SERVER_H
#define DAEMOON_TEST_FAKE_SERVER_H

#include <daemoon/backend.h>
#include <daemoon/manifest.h>

#define FAKE_MAX_TITLES   4
#define FAKE_MAX_VERSIONS 8

typedef struct {
    unsigned int       version;
    unsigned int       parent_version;
    char               sha256[DAEMOON_SHA256_HEX];
    unsigned long long size;
    char               device_label[DAEMOON_LABEL_MAX];
    char               received_at[DAEMOON_TIMESTAMP_MAX];
    unsigned char     *blob;
    size_t             blob_len;
} fake_version_t;

typedef struct {
    char               title_id[DAEMOON_TITLE_ID_MAX];
    daemoon_platform_t platform;
    unsigned int       latest_version;
    fake_version_t     versions[FAKE_MAX_VERSIONS];
    size_t             nversions;
} fake_title_t;

typedef struct {
    fake_title_t titles[FAKE_MAX_TITLES];
    size_t       ntitles;

    unsigned requests;
    unsigned uploads;
    unsigned downloads;
    unsigned conflicts;

    /* Fault injection: when non zero, the next request answers with this status
     * and a matching error code instead of doing any work. */
    int next_status;
} fake_server_t;

void fake_server_init(fake_server_t *s);
void fake_server_free(fake_server_t *s);

/* Seed a version directly, as if another console had uploaded it. */
daemoon_result_t fake_server_put(fake_server_t *s, daemoon_platform_t platform,
                                 const char *title_id, const void *blob, size_t blob_len,
                                 const char *device_label);

const fake_version_t *fake_server_latest(const fake_server_t *s, daemoon_platform_t platform,
                                         const char *title_id);

extern const daemoon_net_backend_t fake_server_net_backend;

#endif /* DAEMOON_TEST_FAKE_SERVER_H */
