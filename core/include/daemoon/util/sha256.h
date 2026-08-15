/* sha256.h - streaming SHA-256.
 *
 * Streaming, not one shot, because a save is never held in memory: the digest is
 * computed while the bytes go past on their way into or out of a package.
 */
#ifndef DAEMOON_UTIL_SHA256_H
#define DAEMOON_UTIL_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DAEMOON_SHA256_DIGEST_LEN 32
/* Lowercase hex plus the NUL. Every digest in this project is carried as text:
 * it goes into manifests, into state files and over the wire in that form. */
#define DAEMOON_SHA256_HEX        65

typedef struct {
    uint32_t      state[8];
    uint64_t      bitlen;
    unsigned char block[64];
    size_t        blocklen;
} daemoon_sha256_t;

void daemoon_sha256_init(daemoon_sha256_t *c);
void daemoon_sha256_update(daemoon_sha256_t *c, const void *data, size_t len);
void daemoon_sha256_final(daemoon_sha256_t *c, unsigned char out[DAEMOON_SHA256_DIGEST_LEN]);

/* Lowercase hex, NUL terminated. out must hold 65 bytes. */
void daemoon_sha256_hex(const unsigned char digest[DAEMOON_SHA256_DIGEST_LEN], char *out);
void daemoon_sha256_buf(const void *data, size_t len, char *out_hex);

/* Constant time compare of two hex digests. Length is fixed at 64, so this is a
 * plain fixed length comparison with no early exit. */
int daemoon_sha256_hex_equal(const char *a, const char *b);

#ifdef __cplusplus
}
#endif

#endif /* DAEMOON_UTIL_SHA256_H */
