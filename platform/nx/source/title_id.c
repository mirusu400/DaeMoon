/* A title id as this project spells it: 16 uppercase hex digits.
 *
 * The same spelling as the 3DS build and the same as the server's, because the id is
 * what a manifest is keyed by and a lowercase one would be a different title as far
 * as the server is concerned.
 *
 * No platform header, so `make core-test` checks it on a desktop.
 */
#include "daemoon_nx.h"

#include <stdio.h>
#include <string.h>

void daemoon_nx_format_title_id(unsigned long long id, char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return;
    }
    (void)snprintf(out, cap, "%016llX", id);
}

daemoon_result_t daemoon_nx_parse_title_id(const char *text, unsigned long long *out)
{
    unsigned long long value = 0;
    size_t i;

    if (text == NULL || out == NULL) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }
    /* Exactly sixteen digits. A short id is not a small number here: it is a
     * manifest that will not match the one the other side wrote. */
    if (strlen(text) != 16) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }
    for (i = 0; i < 16; ++i) {
        char c = text[i];
        unsigned digit;

        if (c >= '0' && c <= '9') {
            digit = (unsigned)(c - '0');
        } else if (c >= 'A' && c <= 'F') {
            digit = (unsigned)(c - 'A') + 10u;
        } else if (c >= 'a' && c <= 'f') {
            digit = (unsigned)(c - 'a') + 10u;
        } else {
            return DAEMOON_ERR_INVALID_REQUEST;
        }
        value = (value << 4) | digit;
    }
    *out = value;
    return DAEMOON_OK;
}
