#include <daemoon/manifest.h>

#include <daemoon/util/strbuf.h>
#include <daemoon/util/utf8.h>

#include "util/json.h"

#include <string.h>

/* A manifest is a flat object of ten fields. 64 tokens is far more than that needs
 * and still small enough to sit on the stack. */
#define MANIFEST_MAX_TOKENS 64

void daemoon_manifest_init(daemoon_manifest_t *m)
{
    if (m == NULL) {
        return;
    }
    memset(m, 0, sizeof(*m));
    m->format_version = DAEMOON_MANIFEST_FORMAT_VERSION;
    m->platform = DAEMOON_PLATFORM_UNKNOWN;
    m->save_type = DAEMOON_SAVE_UNKNOWN;
    m->version = DAEMOON_VERSION_NONE;
    m->parent_version = DAEMOON_VERSION_NONE;
}

static int is_lower_hex64(const char *s)
{
    size_t i;
    for (i = 0; i < 64; ++i) {
        char c = s[i];
        int ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!ok) {
            return 0;
        }
    }
    return s[64] == '\0';
}

static int is_valid_title_id(const char *s)
{
    size_t i;
    size_t n = strlen(s);

    if (n < 4 || n > DAEMOON_TITLE_ID_MAX - 1) {
        return 0;
    }
    for (i = 0; i < n; ++i) {
        char c = s[i];
        int ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || c == '_' || c == '-';
        if (!ok) {
            return 0;
        }
    }
    return 1;
}

daemoon_result_t daemoon_manifest_validate(const daemoon_manifest_t *m)
{
    if (m == NULL) {
        return DAEMOON_ERR_INVALID_MANIFEST;
    }
    if (m->format_version != DAEMOON_MANIFEST_FORMAT_VERSION) {
        /* A package from a newer build is refused outright. Guessing at a layout
         * this build does not know is how a save gets written back wrong. */
        return DAEMOON_ERR_INVALID_MANIFEST;
    }
    if (m->platform == DAEMOON_PLATFORM_UNKNOWN || m->save_type == DAEMOON_SAVE_UNKNOWN) {
        return DAEMOON_ERR_INVALID_MANIFEST;
    }
    if (!is_valid_title_id(m->title_id)) {
        return DAEMOON_ERR_INVALID_MANIFEST;
    }
    if (!is_lower_hex64(m->sha256)) {
        return DAEMOON_ERR_INVALID_MANIFEST;
    }
    if (m->device_label[0] == '\0' ||
        !daemoon_utf8_valid(m->device_label, strlen(m->device_label))) {
        return DAEMOON_ERR_INVALID_MANIFEST;
    }
    if (m->version != DAEMOON_VERSION_NONE && m->parent_version >= m->version) {
        /* Versions are issued by the server and strictly increase. A package that
         * claims otherwise is either corrupt or forged. */
        return DAEMOON_ERR_INVALID_MANIFEST;
    }
    return DAEMOON_OK;
}

daemoon_result_t daemoon_manifest_parse(const char *json, size_t len, daemoon_manifest_t *out)
{
    jsmntok_t toks[MANIFEST_MAX_TOKENS];
    daemoon_manifest_t m;
    char scratch[64];
    unsigned long long n = 0;
    int ntok = 0;
    int idx;

    if (json == NULL || out == NULL) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }
    if (len == 0) {
        len = strlen(json);
    }
    if (len > DAEMOON_MANIFEST_MAX_BYTES) {
        return DAEMOON_ERR_INVALID_MANIFEST;
    }

    DAEMOON_TRY(daemoon_json_parse(json, len, toks, MANIFEST_MAX_TOKENS, &ntok));
    if (toks[0].type != JSMN_OBJECT) {
        return DAEMOON_ERR_INVALID_MANIFEST;
    }

    daemoon_manifest_init(&m);

    DAEMOON_TRY(daemoon_json_get_uint(json, toks, ntok, 0, "format_version", &n));
    if (n > 0x7fffffffull) {
        return DAEMOON_ERR_INVALID_MANIFEST;
    }
    m.format_version = (int)n;
    if (m.format_version != DAEMOON_MANIFEST_FORMAT_VERSION) {
        return DAEMOON_ERR_INVALID_MANIFEST;
    }

    DAEMOON_TRY(daemoon_json_get_str(json, toks, ntok, 0, "platform", scratch, sizeof(scratch)));
    m.platform = daemoon_platform_parse(scratch, 0);

    DAEMOON_TRY(daemoon_json_get_str(json, toks, ntok, 0, "save_type", scratch, sizeof(scratch)));
    m.save_type = daemoon_save_type_parse(scratch, 0);

    DAEMOON_TRY(daemoon_json_get_str(json, toks, ntok, 0, "title_id", m.title_id,
                                     sizeof(m.title_id)));
    DAEMOON_TRY(daemoon_json_get_str(json, toks, ntok, 0, "sha256", m.sha256, sizeof(m.sha256)));
    DAEMOON_TRY(daemoon_json_get_str(json, toks, ntok, 0, "device_label", m.device_label,
                                     sizeof(m.device_label)));

    DAEMOON_TRY(daemoon_json_get_uint(json, toks, ntok, 0, "version", &n));
    if (n > 0xffffffffull) {
        return DAEMOON_ERR_INVALID_MANIFEST;
    }
    m.version = (unsigned int)n;

    DAEMOON_TRY(daemoon_json_get_uint(json, toks, ntok, 0, "size", &m.size));

    /* parent_version is optional and may be null, which both mean "first upload". */
    idx = daemoon_json_find(json, toks, ntok, 0, "parent_version");
    if (idx >= 0 && !daemoon_json_is_null(json, &toks[idx])) {
        DAEMOON_TRY(daemoon_json_uint(json, &toks[idx], &n));
        if (n > 0xffffffffull) {
            return DAEMOON_ERR_INVALID_MANIFEST;
        }
        m.parent_version = (unsigned int)n;
    }

    /* created_at is informational. It is carried through so a person reading a
     * package can see it, and it is never consulted for ordering. */
    idx = daemoon_json_find(json, toks, ntok, 0, "created_at");
    if (idx >= 0 && toks[idx].type == JSMN_STRING) {
        (void)daemoon_json_str(json, &toks[idx], m.created_at, sizeof(m.created_at));
    }

    DAEMOON_TRY(daemoon_manifest_validate(&m));

    *out = m;
    return DAEMOON_OK;
}

daemoon_result_t daemoon_manifest_write(const daemoon_manifest_t *m, char *buf, size_t buflen,
                                        size_t *out_len)
{
    daemoon_strbuf_t sb;

    if (m == NULL || buf == NULL) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }
    DAEMOON_TRY(daemoon_manifest_validate(m));

    daemoon_strbuf_init(&sb, buf, buflen);

    daemoon_strbuf_add(&sb, "{\"format_version\":");
    daemoon_strbuf_add_uint(&sb, (unsigned long long)m->format_version);

    daemoon_strbuf_add(&sb, ",\"platform\":\"");
    daemoon_strbuf_add_json(&sb, daemoon_platform_name(m->platform));

    daemoon_strbuf_add(&sb, "\",\"title_id\":\"");
    daemoon_strbuf_add_json(&sb, m->title_id);

    daemoon_strbuf_add(&sb, "\",\"save_type\":\"");
    daemoon_strbuf_add_json(&sb, daemoon_save_type_name(m->save_type));

    daemoon_strbuf_add(&sb, "\",\"version\":");
    daemoon_strbuf_add_uint(&sb, m->version);

    daemoon_strbuf_add(&sb, ",\"parent_version\":");
    if (m->parent_version == DAEMOON_VERSION_NONE) {
        daemoon_strbuf_add(&sb, "null");
    } else {
        daemoon_strbuf_add_uint(&sb, m->parent_version);
    }

    daemoon_strbuf_add(&sb, ",\"sha256\":\"");
    daemoon_strbuf_add_json(&sb, m->sha256);

    daemoon_strbuf_add(&sb, "\",\"size\":");
    daemoon_strbuf_add_uint(&sb, m->size);

    daemoon_strbuf_add(&sb, ",\"device_label\":\"");
    daemoon_strbuf_add_json(&sb, m->device_label);

    daemoon_strbuf_add(&sb, "\",\"created_at\":\"");
    daemoon_strbuf_add_json(&sb, m->created_at);
    daemoon_strbuf_add(&sb, "\"}");

    DAEMOON_TRY(daemoon_strbuf_result(&sb));

    if (out_len != NULL) {
        *out_len = sb.len;
    }
    return DAEMOON_OK;
}
