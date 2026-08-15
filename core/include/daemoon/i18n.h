/* i18n.h - user visible text.
 *
 * Rules, from the root CLAUDE.md:
 *   - No user facing string literals in code. Everything is a daemoon_str_id_t.
 *   - Tables are compiled in (core/src/lang_table.c, generated). Parsing JSON at
 *     runtime for UI strings would waste 3DS heap.
 *   - Templates carry placeholders, never concatenated sentence fragments, because
 *     word order varies by language.
 *   - Everything is UTF-8. One byte is not one character and not one column; use
 *     daemoon_utf8_* for anything involving length or truncation.
 *
 * Placeholders are {0} .. {9} and not printf specifiers. A translator reordering
 * "%s uses %d" into "%d uses %s" would be undefined behaviour in C; reordering
 * "{0} uses {1}" is not. "{{" is a literal "{". tools/gen rejects a translation
 * whose placeholder set differs from English.
 */
#ifndef DAEMOON_I18N_H
#define DAEMOON_I18N_H

#include <stddef.h>

#include <daemoon/result.h>
#include <daemoon/str_ids.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Generated tables. Declared here so lang_table.c needs no private header. */
extern const char *const daemoon_lang_table[DAEMOON_LANG_COUNT][DAEMOON_STR_COUNT];
extern const char *const daemoon_lang_codes[DAEMOON_LANG_COUNT];

/* Default is DAEMOON_LANG_EN until the platform layer sets one from the console
 * setting (3DS CFGU_GetSystemLanguage, Switch setGetSystemLanguage) or from the
 * user's stored choice. */
void          daemoon_i18n_set_language(daemoon_lang_t lang);
daemoon_lang_t daemoon_i18n_language(void);

/* BCP 47 style code, e.g. "zh-Hans". Matching is case insensitive, and a bare
 * "zh" resolves to Simplified. Unknown codes leave *out untouched and return
 * DAEMOON_ERR_UNSUPPORTED, so the caller keeps its previous choice. */
daemoon_result_t daemoon_i18n_language_from_code(const char *code, daemoon_lang_t *out);
const char      *daemoon_lang_code(daemoon_lang_t lang);

/* The language's own name, for the picker. Always taken from that language's own
 * table so the list reads natively regardless of the current setting. */
const char *daemoon_lang_name(daemoon_lang_t lang);

/* Resolved text. Never NULL: a missing entry falls back to English, and a missing
 * English entry falls back to the key name so a bug is visible instead of a crash. */
const char *daemoon_str(daemoon_str_id_t id);
const char *daemoon_str_in(daemoon_lang_t lang, daemoon_str_id_t id);

/* Substitute {0}.. with args[0].. into buf. Always NUL terminates when buflen > 0.
 * Returns DAEMOON_ERR_BUFFER_TOO_SMALL if the result did not fit, in which case buf
 * holds the truncated text cut at a UTF-8 boundary; callers that only display the
 * text may ignore that. A placeholder with no matching argument is left as is. */
daemoon_result_t daemoon_strf(char *buf, size_t buflen, daemoon_str_id_t id,
                              const char *const *args, size_t nargs);

/* Human readable byte count, e.g. "1.4 MiB". The unit is ASCII and identical in
 * every language, so this is not a translated string. */
void daemoon_fmt_bytes(char *buf, size_t buflen, unsigned long long bytes);

#ifdef __cplusplus
}
#endif

#endif /* DAEMOON_I18N_H */
