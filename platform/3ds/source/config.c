/* The handful of settings that cannot be discovered.
 *
 * A console has no keyboard worth typing a URL on, so the server address and the
 * device token come off the SD card. The token is written there by the pairing
 * flow in Phase 4; until then it is put there by hand, which is also how somebody
 * self hosting will want to do it the first time.
 *
 * Deliberately not JSON. This file is edited on a PC by a person, sometimes on a
 * phone, and a missing brace should not be the reason a console cannot reach its
 * own server. Lines of key=value, everything else ignored.
 */
#include "daemoon_3ds.h"

#include <daemoon/util/strbuf.h>
#include <daemoon/util/utf8.h>

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

void daemoon_3ds_config_defaults(daemoon_3ds_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    (void)daemoon_strlcpy(cfg->device_label, sizeof(cfg->device_label), "3DS");
}

daemoon_result_t daemoon_3ds_config_load(const char *path, daemoon_3ds_config_t *cfg)
{
    char line[512];
    FILE *fp;

    daemoon_3ds_config_defaults(cfg);

    fp = fopen(path, "rb");
    if (fp == NULL) {
        /* No configuration is not a failure. It is a console that has not been
         * pointed at a server yet, and everything local still works. */
        return DAEMOON_ERR_NOT_FOUND;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *eq;
        const char *key;
        const char *value;

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
        trim((char *)key);
        trim((char *)value);

        if (strcmp(key, "server") == 0) {
            size_t len = strlen(value);

            /* A trailing slash here becomes a double slash in every path built
             * from it, and some servers answer those differently. */
            (void)daemoon_strlcpy(cfg->server_url, sizeof(cfg->server_url), value);
            len = strlen(cfg->server_url);
            while (len > 0 && cfg->server_url[len - 1] == '/') {
                cfg->server_url[--len] = '\0';
            }
        } else if (strcmp(key, "token") == 0) {
            (void)daemoon_strlcpy(cfg->token, sizeof(cfg->token), value);
        } else if (strcmp(key, "label") == 0) {
            /* Shown on another console in a conflict dialog, so it has to survive
             * a round trip as UTF-8 and be worth reading. */
            if (value[0] != '\0' && daemoon_utf8_valid(value, strlen(value))) {
                (void)daemoon_strlcpy(cfg->device_label, sizeof(cfg->device_label),
                                      value);
            }
        } else if (strcmp(key, "ca_bundle") == 0) {
            (void)daemoon_strlcpy(cfg->ca_bundle, sizeof(cfg->ca_bundle), value);
        }
    }
    (void)fclose(fp);

    return DAEMOON_OK;
}

int daemoon_3ds_config_can_sync(const daemoon_3ds_config_t *cfg)
{
    return cfg->server_url[0] != '\0' && cfg->token[0] != '\0';
}
