/* json.h - the thin layer over jsmn that core actually uses.
 *
 * Internal to core/src. It is not under core/include because jsmntok_t leaks into
 * the signatures, and the platform layers have no business parsing JSON: everything
 * they receive from core is already a struct.
 */
#ifndef DAEMOON_SRC_UTIL_JSON_H
#define DAEMOON_SRC_UTIL_JSON_H

#include <stddef.h>

#define JSMN_PARENT_LINKS
#define JSMN_HEADER
#include <jsmn/jsmn.h>

#include <daemoon/result.h>

/* Parse into a caller supplied token array. Returns DAEMOON_ERR_PARSE_ERROR for
 * malformed or truncated input, and DAEMOON_ERR_BUFFER_TOO_SMALL when the document
 * needs more tokens than max. Both are refusals, never a partial accept. */
daemoon_result_t daemoon_json_parse(const char *js, size_t len, jsmntok_t *toks, int max,
                                    int *out_ntok);

/* Index of the value token for key inside the object token at obj, or -1. Only
 * direct children are considered, so a nested object cannot shadow a top level key. */
int daemoon_json_find(const char *js, const jsmntok_t *toks, int ntok, int obj, const char *key);

/* Copy a string token out, unescaping the JSON escapes that manifests can contain.
 * DAEMOON_ERR_BUFFER_TOO_SMALL when it does not fit; the destination is still
 * terminated and cut on a UTF-8 boundary. */
daemoon_result_t daemoon_json_str(const char *js, const jsmntok_t *t, char *out, size_t cap);

/* Non negative integer. Rejects a sign, a fraction, an exponent and anything that
 * would overflow, because a version number that silently wraps is a corruption
 * waiting to happen. */
daemoon_result_t daemoon_json_uint(const char *js, const jsmntok_t *t, unsigned long long *out);

/* Convenience: look up key and convert. Returns DAEMOON_ERR_PARSE_ERROR when the
 * key is absent, so a caller that wants an optional field checks with
 * daemoon_json_find first. */
daemoon_result_t daemoon_json_get_str(const char *js, const jsmntok_t *toks, int ntok, int obj,
                                      const char *key, char *out, size_t cap);
daemoon_result_t daemoon_json_get_uint(const char *js, const jsmntok_t *toks, int ntok, int obj,
                                       const char *key, unsigned long long *out);

/* 1 when the token is JSON null. */
int daemoon_json_is_null(const char *js, const jsmntok_t *t);

#endif /* DAEMOON_SRC_UTIL_JSON_H */
