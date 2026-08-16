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

#include <daemoon/i18n.h>
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
        } else if (strcmp(key, "device") == 0) {
            (void)daemoon_strlcpy(cfg->device_id, sizeof(cfg->device_id), value);
        } else if (strcmp(key, "language") == 0) {
            daemoon_lang_t parsed;

            /* Kept only if this build knows it. A typo here would otherwise be a
             * console that silently ignores the setting and shows English. */
            if (daemoon_i18n_language_from_code(value, &parsed) == DAEMOON_OK) {
                (void)daemoon_strlcpy(cfg->language, sizeof(cfg->language), value);
            }
        } else if (strcmp(key, "ca_bundle") == 0) {
            (void)daemoon_strlcpy(cfg->ca_bundle, sizeof(cfg->ca_bundle), value);
        }
    }
    (void)fclose(fp);

    return DAEMOON_OK;
}

/* Writes the file back, keeping the shape a person can still edit by hand.
 *
 * Temp then rename, the same rule the save path follows. A configuration is not
 * save data and losing one costs a retype, but a half written file is read by the
 * next launch as a console with no server - and the first thing anyone would do
 * about that is type it all in again on a software keyboard.
 *
 * Only the four keys are written. A comment somebody added is lost, which is worth
 * saying out loud rather than pretending: the alternative is a rewriter that has to
 * understand a file format whose whole point is that it barely has one.
 */
daemoon_result_t daemoon_3ds_config_save(const char *path, const daemoon_3ds_config_t *cfg)
{
    char temp[DAEMOON_PATH_MAX];
    daemoon_strbuf_t sb;
    FILE *fp;
    int ok;

    daemoon_strbuf_init(&sb, temp, sizeof(temp));
    daemoon_strbuf_add(&sb, path);
    daemoon_strbuf_add(&sb, ".tmp");
    DAEMOON_TRY(daemoon_strbuf_result(&sb));

    fp = fopen(temp, "wb");
    if (fp == NULL) {
        return DAEMOON_ERR_IO_ERROR;
    }
    ok = fprintf(fp, "server = %s\nlabel = %s\n", cfg->server_url,
                 cfg->device_label) > 0;
    /* Absent rather than empty when there is none, the way `language` already is.
     *
     * This file is rewritten whole from memory whenever any setting changes, so an
     * unpaired console used to leave `token = ` behind. That parses back to the
     * same thing and is not a bug on its own - but a file that says a key exists
     * and a file that omits it should not both mean "not set", because the next
     * person reading it by hand has to know which. */
    if (ok && cfg->token[0] != '\0') {
        ok = fprintf(fp, "token = %s\n", cfg->token) > 0;
    }
    if (ok && cfg->device_id[0] != '\0') {
        ok = fprintf(fp, "device = %s\n", cfg->device_id) > 0;
    }
    if (ok && cfg->language[0] != '\0') {
        /* Absent rather than empty when unset, so the file says "follow the
         * console" by not mentioning it. */
        ok = fprintf(fp, "language = %s\n", cfg->language) > 0;
    }
    if (ok && cfg->ca_bundle[0] != '\0') {
        ok = fprintf(fp, "ca_bundle = %s\n", cfg->ca_bundle) > 0;
    }
    if (fclose(fp) != 0 || !ok) {
        (void)remove(temp);
        return DAEMOON_ERR_IO_ERROR;
    }

    (void)remove(path);
    if (rename(temp, path) != 0) {
        (void)remove(temp);
        return DAEMOON_ERR_IO_ERROR;
    }
    return DAEMOON_OK;
}

int daemoon_3ds_config_can_sync(const daemoon_3ds_config_t *cfg)
{
    return cfg->server_url[0] != '\0' && cfg->token[0] != '\0';
}
