#include <daemoon/util/utf8.h>

/* Returns the length of the sequence starting at s, or 0 when it is not a well
 * formed sequence. Rejects overlong forms, surrogates and anything above U+10FFFF,
 * because accepting them lets the same text have two encodings and then a digest
 * over a device label stops being stable. */
static size_t seq_len(const unsigned char *s, size_t avail, uint32_t *out_cp)
{
    uint32_t cp;
    size_t need, i;

    if (avail == 0) {
        return 0;
    }

    if (s[0] < 0x80u) {
        cp = s[0];
        need = 1;
    } else if ((s[0] & 0xe0u) == 0xc0u) {
        cp = s[0] & 0x1fu;
        need = 2;
    } else if ((s[0] & 0xf0u) == 0xe0u) {
        cp = s[0] & 0x0fu;
        need = 3;
    } else if ((s[0] & 0xf8u) == 0xf0u) {
        cp = s[0] & 0x07u;
        need = 4;
    } else {
        return 0; /* continuation byte or 5+ byte lead */
    }

    if (need > avail) {
        return 0;
    }
    for (i = 1; i < need; ++i) {
        if ((s[i] & 0xc0u) != 0x80u) {
            return 0;
        }
        cp = (cp << 6) | (uint32_t)(s[i] & 0x3fu);
    }

    if ((need == 2 && cp < 0x80u) ||
        (need == 3 && cp < 0x800u) ||
        (need == 4 && cp < 0x10000u)) {
        return 0; /* overlong */
    }
    if (cp > 0x10ffffu || (cp >= 0xd800u && cp <= 0xdfffu)) {
        return 0;
    }

    if (out_cp != NULL) {
        *out_cp = cp;
    }
    return need;
}

int daemoon_utf8_valid(const char *s, size_t len)
{
    const unsigned char *p = (const unsigned char *)s;
    size_t i = 0;

    if (s == NULL) {
        return 0;
    }
    while (i < len) {
        size_t n = seq_len(p + i, len - i, NULL);
        if (n == 0) {
            return 0;
        }
        i += n;
    }
    return 1;
}

size_t daemoon_utf8_length(const char *s, size_t len)
{
    const unsigned char *p = (const unsigned char *)s;
    size_t i = 0, count = 0;

    while (i < len) {
        size_t n = seq_len(p + i, len - i, NULL);
        i += (n == 0) ? 1 : n;
        ++count;
    }
    return count;
}

size_t daemoon_utf8_decode(const char *s, size_t len, uint32_t *out_cp)
{
    uint32_t cp = 0xfffdu;
    size_t n;

    if (len == 0) {
        if (out_cp != NULL) {
            *out_cp = 0;
        }
        return 0;
    }
    n = seq_len((const unsigned char *)s, len, &cp);
    if (n == 0) {
        if (out_cp != NULL) {
            *out_cp = 0xfffdu;
        }
        return 1;
    }
    if (out_cp != NULL) {
        *out_cp = cp;
    }
    return n;
}

size_t daemoon_utf8_truncate(const char *s, size_t len, size_t max_bytes)
{
    const unsigned char *p = (const unsigned char *)s;
    size_t i = 0;

    if (max_bytes >= len) {
        return len;
    }
    while (i < len) {
        size_t n = seq_len(p + i, len - i, NULL);
        if (n == 0) {
            n = 1;
        }
        if (i + n > max_bytes) {
            break;
        }
        i += n;
    }
    return i;
}

size_t daemoon_utf8_trim_partial(const char *s, size_t len)
{
    const unsigned char *p = (const unsigned char *)s;
    size_t i = 0;

    while (i < len) {
        size_t n = seq_len(p + i, len - i, NULL);
        if (n == 0) {
            /* Either a genuinely invalid byte, or a lead byte whose continuation
             * bytes were cut off. Only the second case is worth trimming, and it can
             * only happen within the last three bytes. */
            size_t need = 0;
            if ((p[i] & 0xe0u) == 0xc0u) {
                need = 2;
            } else if ((p[i] & 0xf0u) == 0xe0u) {
                need = 3;
            } else if ((p[i] & 0xf8u) == 0xf0u) {
                need = 4;
            }
            if (need > 0 && len - i < need) {
                return i;
            }
            n = 1;
        }
        i += n;
    }
    return len;
}

/* Wide ranges, from the East Asian Width property. Kept as a short table on
 * purpose: the full property table is far larger than anything a console UI needs,
 * and this only has to stop a label from overflowing a text box. */
static int cp_width(uint32_t cp)
{
    if (cp == 0) {
        return 0;
    }
    /* Combining marks and zero width joiners. */
    if ((cp >= 0x0300u && cp <= 0x036fu) || cp == 0x200bu || cp == 0x200du ||
        (cp >= 0xfe00u && cp <= 0xfe0fu)) {
        return 0;
    }
    if ((cp >= 0x1100u && cp <= 0x115fu) ||  /* Hangul Jamo */
        (cp >= 0x2e80u && cp <= 0xa4cfu) ||  /* CJK radicals through Yi */
        (cp >= 0xac00u && cp <= 0xd7a3u) ||  /* Hangul syllables */
        (cp >= 0xf900u && cp <= 0xfaffu) ||  /* CJK compatibility ideographs */
        (cp >= 0xfe30u && cp <= 0xfe6fu) ||  /* CJK compatibility forms */
        (cp >= 0xff00u && cp <= 0xff60u) ||  /* fullwidth forms */
        (cp >= 0xffe0u && cp <= 0xffe6u) ||
        (cp >= 0x1f300u && cp <= 0x1f64fu) || /* emoji */
        (cp >= 0x20000u && cp <= 0x3fffdu)) { /* CJK extension B and later */
        return 2;
    }
    return 1;
}

size_t daemoon_utf8_width(const char *s, size_t len)
{
    size_t i = 0, w = 0;

    while (i < len) {
        uint32_t cp = 0;
        size_t n = daemoon_utf8_decode(s + i, len - i, &cp);
        if (n == 0) {
            break;
        }
        w += (size_t)cp_width(cp);
        i += n;
    }
    return w;
}
