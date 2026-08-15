#include <daemoon/i18n.h>
#include <daemoon/util/strbuf.h>
#include <daemoon/util/utf8.h>

#include <string.h>

/* The selected language is process wide because there is exactly one screen and one
 * user. It is set once at startup from the console setting or the stored choice. */
static daemoon_lang_t g_lang = DAEMOON_LANG_EN;

/* Parallel to daemoon_str_id_t, used only when a string is missing from every table,
 * which means the generator and the code went out of sync. Showing the id is more
 * useful than showing nothing and much better than dereferencing NULL. */
static const char k_missing[] = "?";

void daemoon_i18n_set_language(daemoon_lang_t lang)
{
    /* Compared as unsigned so this reads the same whether the toolchain gives the
     * enum a signed or an unsigned underlying type: devkitARM picks unsigned, and
     * a signed bounds check there is a warning and half a check. */
    if ((unsigned)lang < (unsigned)DAEMOON_LANG_COUNT) {
        g_lang = lang;
    }
}

daemoon_lang_t daemoon_i18n_language(void)
{
    return g_lang;
}

const char *daemoon_lang_code(daemoon_lang_t lang)
{
    if ((unsigned)lang >= (unsigned)DAEMOON_LANG_COUNT) {
        return daemoon_lang_codes[DAEMOON_LANG_EN];
    }
    return daemoon_lang_codes[lang];
}

static int ascii_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

static int code_eq_ci(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        if (ascii_lower((unsigned char)*a) != ascii_lower((unsigned char)*b)) {
            return 0;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

daemoon_result_t daemoon_i18n_language_from_code(const char *code, daemoon_lang_t *out)
{
    int i;

    if (code == NULL || out == NULL) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }

    for (i = 0; i < DAEMOON_LANG_COUNT; ++i) {
        if (code_eq_ci(code, daemoon_lang_codes[i])) {
            *out = (daemoon_lang_t)i;
            return DAEMOON_OK;
        }
    }

    /* A console reporting a bare "zh" does not say which script. Simplified is the
     * larger audience and the Switch shared font set defaults the same way. */
    if (code_eq_ci(code, "zh") || code_eq_ci(code, "zh-CN") || code_eq_ci(code, "zh-Hans-CN")) {
        *out = DAEMOON_LANG_ZH_HANS;
        return DAEMOON_OK;
    }
    if (code_eq_ci(code, "zh-TW") || code_eq_ci(code, "zh-HK") || code_eq_ci(code, "zh-Hant-TW")) {
        *out = DAEMOON_LANG_ZH_HANT;
        return DAEMOON_OK;
    }

    /* Leave *out alone so the caller keeps whatever it had. */
    return DAEMOON_ERR_UNSUPPORTED;
}

const char *daemoon_str_in(daemoon_lang_t lang, daemoon_str_id_t id)
{
    const char *s;

    if ((unsigned)id >= (unsigned)DAEMOON_STR_COUNT) {
        return k_missing;
    }
    if ((unsigned)lang < (unsigned)DAEMOON_LANG_COUNT) {
        s = daemoon_lang_table[lang][id];
        if (s != NULL) {
            return s;
        }
    }
    /* Always fall back to English when a key is missing. */
    s = daemoon_lang_table[DAEMOON_LANG_EN][id];
    return s != NULL ? s : k_missing;
}

const char *daemoon_str(daemoon_str_id_t id)
{
    return daemoon_str_in(g_lang, id);
}

const char *daemoon_lang_name(daemoon_lang_t lang)
{
    /* Read from that language's own table so the picker reads natively no matter
     * what is currently selected. */
    return daemoon_str_in(lang, DAEMOON_STR_LANG_NAME);
}

daemoon_result_t daemoon_strf(char *buf, size_t buflen, daemoon_str_id_t id,
                              const char *const *args, size_t nargs)
{
    daemoon_strbuf_t sb;
    const char *t = daemoon_str(id);

    if (buf == NULL || buflen == 0) {
        return DAEMOON_ERR_BUFFER_TOO_SMALL;
    }
    if (nargs > DAEMOON_STR_MAX_ARGS) {
        nargs = DAEMOON_STR_MAX_ARGS;
    }

    daemoon_strbuf_init(&sb, buf, buflen);

    while (*t != '\0') {
        if (t[0] == '{' && t[1] == '{') {
            daemoon_strbuf_addc(&sb, '{');
            t += 2;
            continue;
        }
        if (t[0] == '{' && t[1] >= '0' && t[1] <= '9' && t[2] == '}') {
            size_t idx = (size_t)(t[1] - '0');
            if (idx < nargs && args != NULL && args[idx] != NULL) {
                daemoon_strbuf_add(&sb, args[idx]);
            } else {
                /* No argument for this slot. Leaving the placeholder visible makes
                 * the bug obvious instead of producing a sentence with a hole. */
                daemoon_strbuf_addn(&sb, t, 3);
            }
            t += 3;
            continue;
        }
        daemoon_strbuf_addc(&sb, *t++);
    }

    if (daemoon_strbuf_result(&sb) != DAEMOON_OK) {
        buf[daemoon_utf8_trim_partial(buf, sb.len)] = '\0';
        return DAEMOON_ERR_BUFFER_TOO_SMALL;
    }
    return DAEMOON_OK;
}

void daemoon_fmt_bytes(char *buf, size_t buflen, unsigned long long bytes)
{
    static const char *const unit[] = { "B", "KiB", "MiB", "GiB" };
    daemoon_strbuf_t sb;
    unsigned long long whole = bytes;
    unsigned frac = 0;
    int u = 0;

    if (buf == NULL || buflen == 0) {
        return;
    }
    daemoon_strbuf_init(&sb, buf, buflen);

    while (whole >= 1024ull && u < 3) {
        frac = (unsigned)(((whole % 1024ull) * 10ull) / 1024ull);
        whole /= 1024ull;
        ++u;
    }

    daemoon_strbuf_add_uint(&sb, whole);
    if (u > 0 && whole < 100ull) {
        daemoon_strbuf_addc(&sb, '.');
        daemoon_strbuf_add_uint(&sb, frac);
    }
    daemoon_strbuf_addc(&sb, ' ');
    daemoon_strbuf_add(&sb, unit[u]);
}
