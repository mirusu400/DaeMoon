/* manifest.h - manifest.json, the root entry of every save package.
 *
 * Shape and constraints are in shared/manifest.schema.json. Both this parser and the
 * Go server read the same fixture files from shared/fixtures/, which is what keeps
 * the two sides from interpreting a manifest differently.
 *
 * created_at is informational. Nothing here ever decides freshness from it: the
 * console RTC is user settable, so only the server issued version is trusted.
 */
#ifndef DAEMOON_MANIFEST_H
#define DAEMOON_MANIFEST_H

#include <stddef.h>

#include <daemoon/backend.h>
#include <daemoon/result.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DAEMOON_MANIFEST_FORMAT_VERSION 1
#define DAEMOON_MANIFEST_MAX_BYTES      2048
#define DAEMOON_TIMESTAMP_MAX           32

/* No version has been issued yet, i.e. this title has never been uploaded. */
#define DAEMOON_VERSION_NONE 0u

typedef struct {
    int                 format_version;
    daemoon_platform_t  platform;
    char                title_id[DAEMOON_TITLE_ID_MAX];
    daemoon_save_type_t save_type;
    unsigned int        version;
    /* The version this package was derived from. DAEMOON_VERSION_NONE for a first
     * upload. Stored separately from version so a conflict can be detected without
     * consulting anything else. */
    unsigned int        parent_version;
    char                sha256[DAEMOON_SHA256_HEX]; /* payload digest, see archive.h */
    unsigned long long  size;                       /* uncompressed payload bytes */
    char                device_label[DAEMOON_LABEL_MAX];
    char                created_at[DAEMOON_TIMESTAMP_MAX]; /* ISO 8601, informational only */
} daemoon_manifest_t;

void daemoon_manifest_init(daemoon_manifest_t *m);

/* Parse manifest.json. Rejects anything with a format_version this build does not
 * know: an unreadable package is recoverable, a misread one is not. */
daemoon_result_t daemoon_manifest_parse(const char *json, size_t len, daemoon_manifest_t *out);

/* Serialise. Writes at most buflen bytes including the NUL and reports the length
 * without it. Field order is fixed so a round trip is byte stable. */
daemoon_result_t daemoon_manifest_write(const daemoon_manifest_t *m, char *buf, size_t buflen,
                                        size_t *out_len);

/* Structural check: known format version, plausible title id, lowercase 64 digit
 * digest, non empty label, parent_version < version when a version is set. */
daemoon_result_t daemoon_manifest_validate(const daemoon_manifest_t *m);

#ifdef __cplusplus
}
#endif

#endif /* DAEMOON_MANIFEST_H */
