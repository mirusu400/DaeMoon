/* strbuf.h - bounded string building over a caller supplied buffer.
 *
 * Core prefers caller supplied buffers to malloc: the 3DS heap fragments badly and
 * a sync run that allocates per title will eventually fail on a console that has
 * been awake for a while.
 *
 * Overflow is sticky. Appending to a full buffer is not an error at the call site;
 * the caller checks once at the end with daemoon_strbuf_result.
 */
#ifndef DAEMOON_UTIL_STRBUF_H
#define DAEMOON_UTIL_STRBUF_H

#include <stddef.h>

#include <daemoon/result.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
    int    overflow;
} daemoon_strbuf_t;

void daemoon_strbuf_init(daemoon_strbuf_t *sb, char *buf, size_t cap);
void daemoon_strbuf_reset(daemoon_strbuf_t *sb);

void daemoon_strbuf_add(daemoon_strbuf_t *sb, const char *s);
void daemoon_strbuf_addn(daemoon_strbuf_t *sb, const char *s, size_t n);
void daemoon_strbuf_addc(daemoon_strbuf_t *sb, char c);
void daemoon_strbuf_add_uint(daemoon_strbuf_t *sb, unsigned long long v);
void daemoon_strbuf_add_int(daemoon_strbuf_t *sb, long long v);

/* Append s as the body of a JSON string, escaping what RFC 8259 requires. Does not
 * write the surrounding quotes. */
void daemoon_strbuf_add_json(daemoon_strbuf_t *sb, const char *s);

/* Append s percent encoded for use in a single path segment or query value. */
void daemoon_strbuf_add_urlenc(daemoon_strbuf_t *sb, const char *s);

/* DAEMOON_ERR_BUFFER_TOO_SMALL if anything was dropped, otherwise DAEMOON_OK. */
daemoon_result_t daemoon_strbuf_result(const daemoon_strbuf_t *sb);

/* Bounded copy that always terminates. Returns DAEMOON_ERR_BUFFER_TOO_SMALL when src
 * did not fit, and truncates at a UTF-8 boundary so a cut multi byte sequence is
 * never left behind. */
daemoon_result_t daemoon_strlcpy(char *dst, size_t cap, const char *src);

#ifdef __cplusplus
}
#endif

#endif /* DAEMOON_UTIL_STRBUF_H */
