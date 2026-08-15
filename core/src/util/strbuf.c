#include <daemoon/util/strbuf.h>
#include <daemoon/util/utf8.h>

#include <string.h>

void daemoon_strbuf_init(daemoon_strbuf_t *sb, char *buf, size_t cap)
{
    sb->buf = buf;
    sb->cap = cap;
    sb->len = 0;
    sb->overflow = 0;
    if (cap > 0) {
        buf[0] = '\0';
    } else {
        sb->overflow = 1;
    }
}

void daemoon_strbuf_reset(daemoon_strbuf_t *sb)
{
    sb->len = 0;
    sb->overflow = (sb->cap == 0);
    if (sb->cap > 0) {
        sb->buf[0] = '\0';
    }
}

void daemoon_strbuf_addn(daemoon_strbuf_t *sb, const char *s, size_t n)
{
    size_t room;

    if (s == NULL || n == 0) {
        return;
    }
    if (sb->cap == 0) {
        sb->overflow = 1;
        return;
    }
    room = sb->cap - sb->len - 1; /* keep space for the NUL */
    if (n > room) {
        n = room;
        sb->overflow = 1;
    }
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
}

void daemoon_strbuf_add(daemoon_strbuf_t *sb, const char *s)
{
    if (s != NULL) {
        daemoon_strbuf_addn(sb, s, strlen(s));
    }
}

void daemoon_strbuf_addc(daemoon_strbuf_t *sb, char c)
{
    daemoon_strbuf_addn(sb, &c, 1);
}

void daemoon_strbuf_add_uint(daemoon_strbuf_t *sb, unsigned long long v)
{
    char tmp[24];
    size_t i = sizeof(tmp);

    do {
        tmp[--i] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0 && i > 0);

    daemoon_strbuf_addn(sb, tmp + i, sizeof(tmp) - i);
}

void daemoon_strbuf_add_int(daemoon_strbuf_t *sb, long long v)
{
    if (v < 0) {
        daemoon_strbuf_addc(sb, '-');
        /* Negating LLONG_MIN is undefined, so widen before negating. */
        daemoon_strbuf_add_uint(sb, (unsigned long long)(-(v + 1)) + 1u);
        return;
    }
    daemoon_strbuf_add_uint(sb, (unsigned long long)v);
}

void daemoon_strbuf_add_json(daemoon_strbuf_t *sb, const char *s)
{
    static const char hex[] = "0123456789abcdef";
    const unsigned char *p = (const unsigned char *)s;

    if (s == NULL) {
        return;
    }
    for (; *p != '\0'; ++p) {
        switch (*p) {
        case '"':
            daemoon_strbuf_add(sb, "\\\"");
            break;
        case '\\':
            daemoon_strbuf_add(sb, "\\\\");
            break;
        case '\n':
            daemoon_strbuf_add(sb, "\\n");
            break;
        case '\r':
            daemoon_strbuf_add(sb, "\\r");
            break;
        case '\t':
            daemoon_strbuf_add(sb, "\\t");
            break;
        case '\b':
            daemoon_strbuf_add(sb, "\\b");
            break;
        case '\f':
            daemoon_strbuf_add(sb, "\\f");
            break;
        default:
            if (*p < 0x20) {
                daemoon_strbuf_add(sb, "\\u00");
                daemoon_strbuf_addc(sb, hex[(*p >> 4) & 0xf]);
                daemoon_strbuf_addc(sb, hex[*p & 0xf]);
            } else {
                /* UTF-8 passes through unescaped, which is valid JSON and keeps
                 * manifests readable when someone opens one on a PC. */
                daemoon_strbuf_addc(sb, (char)*p);
            }
            break;
        }
    }
}

void daemoon_strbuf_add_urlenc(daemoon_strbuf_t *sb, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    const unsigned char *p = (const unsigned char *)s;

    if (s == NULL) {
        return;
    }
    for (; *p != '\0'; ++p) {
        unsigned char c = *p;
        int unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                         (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~';
        if (unreserved) {
            daemoon_strbuf_addc(sb, (char)c);
        } else {
            daemoon_strbuf_addc(sb, '%');
            daemoon_strbuf_addc(sb, hex[(c >> 4) & 0xf]);
            daemoon_strbuf_addc(sb, hex[c & 0xf]);
        }
    }
}

daemoon_result_t daemoon_strbuf_result(const daemoon_strbuf_t *sb)
{
    return sb->overflow ? DAEMOON_ERR_BUFFER_TOO_SMALL : DAEMOON_OK;
}

daemoon_result_t daemoon_strlcpy(char *dst, size_t cap, const char *src)
{
    size_t len;

    if (dst == NULL || cap == 0) {
        return DAEMOON_ERR_BUFFER_TOO_SMALL;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return DAEMOON_OK;
    }

    len = strlen(src);
    if (len < cap) {
        memcpy(dst, src, len + 1);
        return DAEMOON_OK;
    }

    /* Cut on a code point boundary: half a UTF-8 sequence renders as garbage and
     * would not survive a round trip through the server. */
    len = daemoon_utf8_truncate(src, len, cap - 1);
    memcpy(dst, src, len);
    dst[len] = '\0';
    return DAEMOON_ERR_BUFFER_TOO_SMALL;
}
