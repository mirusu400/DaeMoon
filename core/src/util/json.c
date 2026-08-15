#include "json.h"

#include <daemoon/util/strbuf.h>
#include <daemoon/util/utf8.h>

#include <string.h>

daemoon_result_t daemoon_json_parse(const char *js, size_t len, jsmntok_t *toks, int max,
                                    int *out_ntok)
{
    jsmn_parser p;
    int n;

    if (js == NULL || toks == NULL || max <= 0) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }

    jsmn_init(&p);
    n = jsmn_parse(&p, js, len, toks, (unsigned int)max);
    if (n == JSMN_ERROR_NOMEM) {
        return DAEMOON_ERR_BUFFER_TOO_SMALL;
    }
    if (n < 0) {
        /* JSMN_ERROR_INVAL and JSMN_ERROR_PART both mean "do not trust this". */
        return DAEMOON_ERR_PARSE_ERROR;
    }
    if (n == 0) {
        return DAEMOON_ERR_PARSE_ERROR;
    }
    if (out_ntok != NULL) {
        *out_ntok = n;
    }
    return DAEMOON_OK;
}

static int tok_eq(const char *js, const jsmntok_t *t, const char *s)
{
    size_t len = (size_t)(t->end - t->start);
    return t->type == JSMN_STRING && strlen(s) == len && memcmp(js + t->start, s, len) == 0;
}

int daemoon_json_find(const char *js, const jsmntok_t *toks, int ntok, int obj, const char *key)
{
    int i;

    if (obj < 0 || obj >= ntok || toks[obj].type != JSMN_OBJECT) {
        return -1;
    }
    /* Keys of this object are the string tokens whose parent is obj. The value is
     * always the token right after its key. */
    for (i = obj + 1; i < ntok; ++i) {
        if (toks[i].parent != obj || toks[i].type != JSMN_STRING || toks[i].size != 1) {
            continue;
        }
        if (tok_eq(js, &toks[i], key)) {
            return (i + 1 < ntok) ? i + 1 : -1;
        }
    }
    return -1;
}

int daemoon_json_is_null(const char *js, const jsmntok_t *t)
{
    return t != NULL && t->type == JSMN_PRIMITIVE && js[t->start] == 'n';
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static void append_utf8(daemoon_strbuf_t *sb, unsigned long cp)
{
    if (cp < 0x80u) {
        daemoon_strbuf_addc(sb, (char)cp);
    } else if (cp < 0x800u) {
        daemoon_strbuf_addc(sb, (char)(0xc0u | (cp >> 6)));
        daemoon_strbuf_addc(sb, (char)(0x80u | (cp & 0x3fu)));
    } else if (cp < 0x10000u) {
        daemoon_strbuf_addc(sb, (char)(0xe0u | (cp >> 12)));
        daemoon_strbuf_addc(sb, (char)(0x80u | ((cp >> 6) & 0x3fu)));
        daemoon_strbuf_addc(sb, (char)(0x80u | (cp & 0x3fu)));
    } else {
        daemoon_strbuf_addc(sb, (char)(0xf0u | (cp >> 18)));
        daemoon_strbuf_addc(sb, (char)(0x80u | ((cp >> 12) & 0x3fu)));
        daemoon_strbuf_addc(sb, (char)(0x80u | ((cp >> 6) & 0x3fu)));
        daemoon_strbuf_addc(sb, (char)(0x80u | (cp & 0x3fu)));
    }
}

daemoon_result_t daemoon_json_str(const char *js, const jsmntok_t *t, char *out, size_t cap)
{
    daemoon_strbuf_t sb;
    const char *p;
    const char *end;

    if (t == NULL || out == NULL || cap == 0) {
        return DAEMOON_ERR_BUFFER_TOO_SMALL;
    }
    if (t->type != JSMN_STRING) {
        return DAEMOON_ERR_PARSE_ERROR;
    }

    daemoon_strbuf_init(&sb, out, cap);
    p = js + t->start;
    end = js + t->end;

    while (p < end) {
        if (*p != '\\') {
            daemoon_strbuf_addc(&sb, *p++);
            continue;
        }
        if (++p >= end) {
            return DAEMOON_ERR_PARSE_ERROR;
        }
        switch (*p) {
        case '"':  daemoon_strbuf_addc(&sb, '"');  ++p; break;
        case '\\': daemoon_strbuf_addc(&sb, '\\'); ++p; break;
        case '/':  daemoon_strbuf_addc(&sb, '/');  ++p; break;
        case 'b':  daemoon_strbuf_addc(&sb, '\b'); ++p; break;
        case 'f':  daemoon_strbuf_addc(&sb, '\f'); ++p; break;
        case 'n':  daemoon_strbuf_addc(&sb, '\n'); ++p; break;
        case 'r':  daemoon_strbuf_addc(&sb, '\r'); ++p; break;
        case 't':  daemoon_strbuf_addc(&sb, '\t'); ++p; break;
        case 'u': {
            unsigned long cp = 0;
            int i;
            if (end - p < 5) {
                return DAEMOON_ERR_PARSE_ERROR;
            }
            for (i = 1; i <= 4; ++i) {
                int v = hex_val(p[i]);
                if (v < 0) {
                    return DAEMOON_ERR_PARSE_ERROR;
                }
                cp = (cp << 4) | (unsigned long)v;
            }
            p += 5;
            if (cp >= 0xd800u && cp <= 0xdbffu) {
                unsigned long lo = 0;
                if (end - p < 6 || p[0] != '\\' || p[1] != 'u') {
                    return DAEMOON_ERR_PARSE_ERROR;
                }
                for (i = 2; i <= 5; ++i) {
                    int v = hex_val(p[i]);
                    if (v < 0) {
                        return DAEMOON_ERR_PARSE_ERROR;
                    }
                    lo = (lo << 4) | (unsigned long)v;
                }
                if (lo < 0xdc00u || lo > 0xdfffu) {
                    return DAEMOON_ERR_PARSE_ERROR;
                }
                cp = 0x10000u + ((cp - 0xd800u) << 10) + (lo - 0xdc00u);
                p += 6;
            } else if (cp >= 0xdc00u && cp <= 0xdfffu) {
                return DAEMOON_ERR_PARSE_ERROR; /* lone low surrogate */
            }
            append_utf8(&sb, cp);
            break;
        }
        default:
            return DAEMOON_ERR_PARSE_ERROR;
        }
    }

    if (daemoon_strbuf_result(&sb) != DAEMOON_OK) {
        /* Escapes are decoded byte by byte, so a full buffer can hold a partial
         * sequence. Cut it back to a boundary before handing it out. */
        out[daemoon_utf8_trim_partial(out, sb.len)] = '\0';
        return DAEMOON_ERR_BUFFER_TOO_SMALL;
    }
    return DAEMOON_OK;
}

daemoon_result_t daemoon_json_uint(const char *js, const jsmntok_t *t, unsigned long long *out)
{
    unsigned long long v = 0;
    int i;

    if (t == NULL || out == NULL) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }
    if (t->type != JSMN_PRIMITIVE || t->end <= t->start) {
        return DAEMOON_ERR_PARSE_ERROR;
    }

    for (i = t->start; i < t->end; ++i) {
        char c = js[i];
        if (c < '0' || c > '9') {
            return DAEMOON_ERR_PARSE_ERROR;
        }
        if (v > (0xffffffffffffffffull - (unsigned long long)(c - '0')) / 10ull) {
            return DAEMOON_ERR_PARSE_ERROR;
        }
        v = v * 10ull + (unsigned long long)(c - '0');
    }

    *out = v;
    return DAEMOON_OK;
}

daemoon_result_t daemoon_json_get_str(const char *js, const jsmntok_t *toks, int ntok, int obj,
                                      const char *key, char *out, size_t cap)
{
    int i = daemoon_json_find(js, toks, ntok, obj, key);
    if (i < 0) {
        return DAEMOON_ERR_PARSE_ERROR;
    }
    return daemoon_json_str(js, &toks[i], out, cap);
}

daemoon_result_t daemoon_json_get_uint(const char *js, const jsmntok_t *toks, int ntok, int obj,
                                       const char *key, unsigned long long *out)
{
    int i = daemoon_json_find(js, toks, ntok, obj, key);
    if (i < 0) {
        return DAEMOON_ERR_PARSE_ERROR;
    }
    return daemoon_json_uint(js, &toks[i], out);
}
