/* The settings a Switch cannot work out for itself.
 *
 * Same file format as the 3DS build, and the same reason: a console has no keyboard
 * worth typing a URL on, so the server address and the token come off the card. The
 * line reader is shared (platform/common/config_lines.c); which keys exist is not,
 * because the two consoles do not have the same ones.
 *
 * The token is on the card here, and that is a Phase 6 shortcoming rather than a
 * decision. The 3DS keeps it in the application's own save archive because a card
 * comes out of a console; the equivalent on this platform is the homebrew's own save
 * data, which needs a title id this build does not have yet.
 */
#include "daemoon_nx.h"

#include <daemoon/i18n.h>
#include <daemoon/util/strbuf.h>
#include <daemoon/util/utf8.h>

#include <stdio.h>
#include <string.h>

void daemoon_nx_config_defaults(daemoon_nx_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    (void)daemoon_strlcpy(cfg->device_label, sizeof(cfg->device_label), "Switch");
}

static void config_pair(void *user, const char *key, const char *value)
{
    daemoon_nx_config_t *cfg = (daemoon_nx_config_t *)user;

    if (strcmp(key, "server") == 0) {
        size_t len;

        /* A trailing slash here becomes a double slash in every path built from it,
         * and some servers answer those differently. */
        (void)daemoon_strlcpy(cfg->server_url, sizeof(cfg->server_url), value);
        len = strlen(cfg->server_url);
        while (len > 0 && cfg->server_url[len - 1] == '/') {
            cfg->server_url[--len] = '\0';
        }
    } else if (strcmp(key, "token") == 0) {
        (void)daemoon_strlcpy(cfg->token, sizeof(cfg->token), value);
    } else if (strcmp(key, "label") == 0) {
        /* Shown on another console in a conflict dialog, so it has to survive a round
         * trip as UTF-8 and be worth reading. */
        if (value[0] != '\0' && daemoon_utf8_valid(value, strlen(value))) {
            (void)daemoon_strlcpy(cfg->device_label, sizeof(cfg->device_label), value);
        }
    } else if (strcmp(key, "device") == 0) {
        (void)daemoon_strlcpy(cfg->device_id, sizeof(cfg->device_id), value);
    } else if (strcmp(key, "language") == 0) {
        daemoon_lang_t parsed;

        if (daemoon_i18n_language_from_code(value, &parsed) == DAEMOON_OK) {
            (void)daemoon_strlcpy(cfg->language, sizeof(cfg->language), value);
        }
    } else if (strcmp(key, "ca_bundle") == 0) {
        (void)daemoon_strlcpy(cfg->ca_bundle, sizeof(cfg->ca_bundle), value);
    }
}

daemoon_result_t daemoon_nx_config_load(const char *path, daemoon_nx_config_t *cfg)
{
    daemoon_nx_config_defaults(cfg);
    return daemoon_config_read_lines(path, config_pair, cfg);
}

/* Temp then rename, the same rule the save path follows. A configuration is not save
 * data and losing one costs a retype, but a half written file is read by the next
 * launch as a console with no server. */
daemoon_result_t daemoon_nx_config_save(const char *path, const daemoon_nx_config_t *cfg)
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
    if (ok && cfg->token[0] != '\0') {
        ok = fprintf(fp, "token = %s\n", cfg->token) > 0;
    }
    if (ok && cfg->device_id[0] != '\0') {
        ok = fprintf(fp, "device = %s\n", cfg->device_id) > 0;
    }
    if (ok && cfg->language[0] != '\0') {
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

int daemoon_nx_config_can_sync(const daemoon_nx_config_t *cfg)
{
    return cfg->server_url[0] != '\0' && cfg->token[0] != '\0';
}
