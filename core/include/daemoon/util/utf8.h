/* utf8.h - everything in this codebase is UTF-8.
 *
 * One byte is not one character and one character is not one column. Any code that
 * measures or cuts a user visible string goes through here. Cutting a string in the
 * middle of a multi byte sequence produces text a console font renderer will draw as
 * garbage, and in the worst case a label that no longer round trips through the
 * server.
 */
#ifndef DAEMOON_UTIL_UTF8_H
#define DAEMOON_UTIL_UTF8_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Well formed UTF-8: no overlong forms, no surrogates, nothing above U+10FFFF. */
int daemoon_utf8_valid(const char *s, size_t len);

/* Number of code points. Invalid bytes count as one each so this never loops. */
size_t daemoon_utf8_length(const char *s, size_t len);

/* Decode one code point. Returns the byte count consumed, or 0 at the end. An
 * invalid sequence yields U+FFFD and consumes one byte. */
size_t daemoon_utf8_decode(const char *s, size_t len, uint32_t *out_cp);

/* Largest byte count <= max_bytes that ends on a code point boundary. */
size_t daemoon_utf8_truncate(const char *s, size_t len, size_t max_bytes);

/* len minus a trailing incomplete sequence. Used wherever text was assembled byte
 * by byte and then ran out of room: cutting inside a sequence leaves bytes a font
 * renderer draws as garbage. */
size_t daemoon_utf8_trim_partial(const char *s, size_t len);

/* Rough display width in columns: 2 for the CJK and fullwidth ranges, 0 for
 * combining marks, 1 otherwise. Enough to keep a label inside a console text box.
 * German and French run about 30 percent longer than English, so no layout in this
 * project may hardcode a label width. */
size_t daemoon_utf8_width(const char *s, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* DAEMOON_UTIL_UTF8_H */
