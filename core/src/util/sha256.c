#include <daemoon/util/sha256.h>

#include <string.h>

/* FIPS 180-4. Plain portable implementation: the consoles have no SHA hardware
 * reachable from homebrew, and a save is small enough that this is never the
 * bottleneck compared with SD card IO. */

static const uint32_t k_round[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define BSIG0(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define BSIG1(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SSIG0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))
#define SSIG1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

static void sha256_compress(daemoon_sha256_t *c, const unsigned char block[64])
{
    uint32_t w[64];
    uint32_t a, b, cc, d, e, f, g, h;
    unsigned i;

    for (i = 0; i < 16; ++i) {
        w[i] = ((uint32_t)block[i * 4 + 0] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) |
               ((uint32_t)block[i * 4 + 3]);
    }
    for (i = 16; i < 64; ++i) {
        w[i] = SSIG1(w[i - 2]) + w[i - 7] + SSIG0(w[i - 15]) + w[i - 16];
    }

    a = c->state[0];
    b = c->state[1];
    cc = c->state[2];
    d = c->state[3];
    e = c->state[4];
    f = c->state[5];
    g = c->state[6];
    h = c->state[7];

    for (i = 0; i < 64; ++i) {
        uint32_t t1 = h + BSIG1(e) + CH(e, f, g) + k_round[i] + w[i];
        uint32_t t2 = BSIG0(a) + MAJ(a, b, cc);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = cc;
        cc = b;
        b = a;
        a = t1 + t2;
    }

    c->state[0] += a;
    c->state[1] += b;
    c->state[2] += cc;
    c->state[3] += d;
    c->state[4] += e;
    c->state[5] += f;
    c->state[6] += g;
    c->state[7] += h;
}

void daemoon_sha256_init(daemoon_sha256_t *c)
{
    c->state[0] = 0x6a09e667u;
    c->state[1] = 0xbb67ae85u;
    c->state[2] = 0x3c6ef372u;
    c->state[3] = 0xa54ff53au;
    c->state[4] = 0x510e527fu;
    c->state[5] = 0x9b05688cu;
    c->state[6] = 0x1f83d9abu;
    c->state[7] = 0x5be0cd19u;
    c->bitlen = 0;
    c->blocklen = 0;
}

void daemoon_sha256_update(daemoon_sha256_t *c, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;

    if (data == NULL || len == 0) {
        return;
    }

    if (c->blocklen > 0) {
        size_t need = 64 - c->blocklen;
        size_t take = len < need ? len : need;
        memcpy(c->block + c->blocklen, p, take);
        c->blocklen += take;
        p += take;
        len -= take;
        if (c->blocklen == 64) {
            sha256_compress(c, c->block);
            c->bitlen += 512;
            c->blocklen = 0;
        }
    }

    while (len >= 64) {
        sha256_compress(c, p);
        c->bitlen += 512;
        p += 64;
        len -= 64;
    }

    if (len > 0) {
        memcpy(c->block, p, len);
        c->blocklen = len;
    }
}

void daemoon_sha256_final(daemoon_sha256_t *c, unsigned char out[DAEMOON_SHA256_DIGEST_LEN])
{
    uint64_t total = c->bitlen + (uint64_t)c->blocklen * 8;
    size_t i = c->blocklen;

    c->block[i++] = 0x80;
    if (i > 56) {
        while (i < 64) {
            c->block[i++] = 0;
        }
        sha256_compress(c, c->block);
        i = 0;
    }
    while (i < 56) {
        c->block[i++] = 0;
    }
    for (i = 0; i < 8; ++i) {
        c->block[56 + i] = (unsigned char)((total >> (56 - i * 8)) & 0xffu);
    }
    sha256_compress(c, c->block);

    for (i = 0; i < 8; ++i) {
        out[i * 4 + 0] = (unsigned char)((c->state[i] >> 24) & 0xffu);
        out[i * 4 + 1] = (unsigned char)((c->state[i] >> 16) & 0xffu);
        out[i * 4 + 2] = (unsigned char)((c->state[i] >> 8) & 0xffu);
        out[i * 4 + 3] = (unsigned char)(c->state[i] & 0xffu);
    }
}

void daemoon_sha256_hex(const unsigned char digest[DAEMOON_SHA256_DIGEST_LEN], char *out)
{
    static const char hex[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < DAEMOON_SHA256_DIGEST_LEN; ++i) {
        out[i * 2 + 0] = hex[(digest[i] >> 4) & 0xf];
        out[i * 2 + 1] = hex[digest[i] & 0xf];
    }
    out[DAEMOON_SHA256_DIGEST_LEN * 2] = '\0';
}

void daemoon_sha256_buf(const void *data, size_t len, char *out_hex)
{
    daemoon_sha256_t c;
    unsigned char digest[DAEMOON_SHA256_DIGEST_LEN];

    daemoon_sha256_init(&c);
    daemoon_sha256_update(&c, data, len);
    daemoon_sha256_final(&c, digest);
    daemoon_sha256_hex(digest, out_hex);
}

int daemoon_sha256_hex_equal(const char *a, const char *b)
{
    unsigned char diff = 0;
    size_t i;

    if (a == NULL || b == NULL) {
        return 0;
    }
    /* Both operands are fixed length hex, so there is no length to leak and no
     * reason to stop early. */
    for (i = 0; i < DAEMOON_SHA256_DIGEST_LEN * 2; ++i) {
        if (a[i] == '\0' || b[i] == '\0') {
            return 0;
        }
        diff |= (unsigned char)(a[i] ^ b[i]);
    }
    return diff == 0 && a[i] == '\0' && b[i] == '\0';
}
