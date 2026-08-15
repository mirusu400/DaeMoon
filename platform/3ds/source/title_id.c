/* Title ids as text.
 *
 * 16 uppercase hex digits, which is the spelling the manifest schema, the server
 * and the desktop client all use. The conversion lives here rather than inline so
 * that a title id written into a package and one used to open an archive cannot
 * drift apart.
 */
#include "daemoon_3ds.h"

#include <string.h>

void daemoon_3ds_format_title_id(unsigned long long id, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    int i;

    if (cap < 17) {
        if (cap > 0) {
            out[0] = '\0';
        }
        return;
    }
    for (i = 0; i < 16; ++i) {
        out[i] = hex[(id >> ((15 - i) * 4)) & 0xfull];
    }
    out[16] = '\0';
}

daemoon_result_t daemoon_3ds_parse_title_id(const char *text, unsigned long long *out)
{
    unsigned long long value = 0;
    int i;

    if (text == NULL || out == NULL) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }
    if (strlen(text) != 16) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }

    for (i = 0; i < 16; ++i) {
        char c = text[i];
        unsigned digit;

        if (c >= '0' && c <= '9') {
            digit = (unsigned)(c - '0');
        } else if (c >= 'A' && c <= 'F') {
            digit = (unsigned)(c - 'A' + 10);
        } else if (c >= 'a' && c <= 'f') {
            /* Accepted on the way in, never produced on the way out: a package
             * written by an older build should still open. */
            digit = (unsigned)(c - 'a' + 10);
        } else {
            return DAEMOON_ERR_INVALID_REQUEST;
        }
        value = (value << 4) | digit;
    }

    *out = value;
    return DAEMOON_OK;
}
