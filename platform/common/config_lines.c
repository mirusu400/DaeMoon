/* Reading a key=value file, once, for both consoles.
 *
 * Deliberately not JSON. This file is edited on a PC by a person, sometimes on a
 * phone, and a missing brace should not be the reason a console cannot reach its own
 * server. Lines of key=value, everything else ignored.
 *
 * Only the reading is shared. Which keys exist and what they mean differs between the
 * two builds - a 3DS has a startup sync toggle and a Switch has an account - and
 * pretending otherwise would be a shared table nobody could add to. So this hands each
 * pair to a callback and the platform decides.
 */
#include "daemoon_newlib.h"

#include <stdio.h>
#include <string.h>

static void trim(char *s)
{
    size_t len = strlen(s);
    size_t start = 0;

    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' || s[len - 1] == ' ' ||
                       s[len - 1] == '\t')) {
        s[--len] = '\0';
    }
    while (s[start] == ' ' || s[start] == '\t') {
        ++start;
    }
    if (start > 0) {
        memmove(s, s + start, len - start + 1);
    }
}

daemoon_result_t daemoon_config_read_lines(const char *path, daemoon_config_pair_fn cb,
                                           void *user)
{
    char line[512];
    FILE *fp;

    if (path == NULL || cb == NULL) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }

    fp = fopen(path, "rb");
    if (fp == NULL) {
        /* No configuration is not a failure. It is a console that has not been pointed
         * at a server yet, and everything local still works. */
        return DAEMOON_ERR_NOT_FOUND;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *eq;
        char *key;
        char *value;

        trim(line);
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }
        eq = strchr(line, '=');
        if (eq == NULL) {
            continue;
        }
        *eq = '\0';
        key = line;
        value = eq + 1;
        trim(key);
        trim(value);
        cb(user, key, value);
    }
    (void)fclose(fp);
    return DAEMOON_OK;
}
