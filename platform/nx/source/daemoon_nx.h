/* Shared between the Switch platform translation units.
 *
 * Nothing here is visible to core, and nothing here appears in core's headers. This
 * is the side of the wall that is allowed to know what a Switch is.
 */
#ifndef DAEMOON_NX_H
#define DAEMOON_NX_H

#include <daemoon/api.h>
#include <daemoon/backend.h>
#include <daemoon/sync.h>

#include "daemoon_newlib.h"

/* The interface is C++ now - borealis is a C++ framework - and the backends below
 * are still C. This is the seam. */
#ifdef __cplusplus
extern "C" {
#endif

/* The SD card, which on this platform is the root of the default device. Backups,
 * staging and per title sync state, the same layout the 3DS build uses so a card
 * moved between the two reads the same. */
#define DAEMOON_NX_WORK_DIR    "/switch/DaeMoon"
#define DAEMOON_NX_CONFIG_PATH DAEMOON_NX_WORK_DIR "/config.txt"
#define DAEMOON_NX_TRACE_PATH  DAEMOON_NX_WORK_DIR "/trace.txt"
/* Written by the app, not by a person: the root bundle compiled into the binary,
 * spilled here because this platform's curl is too old to take it directly. */
#define DAEMOON_NX_CA_CACHE_PATH DAEMOON_NX_WORK_DIR "/cacert.pem"

/* The name the save is mounted under while it is open.
 *
 * One at a time, deliberately: a second mount under the same name would silently
 * shadow the first, and the sync path opens exactly one save at once. */
#define DAEMOON_NX_SAVE_DEVICE "dmsave"
#define DAEMOON_NX_SAVE_ROOT   DAEMOON_NX_SAVE_DEVICE ":/"

/* ------------------------------------------------------------------- account */

/* A save on this platform belongs to one account, so a different account is a
 * different save. That is the difference from the 3DS the roadmap calls out, and it
 * is why an account has to be picked before a title list means anything.
 *
 * Preselected first: an application launched with a user already chosen should not
 * ask again. Otherwise the system's own selector, which is the one people recognise.
 */
typedef struct {
    unsigned long long lower;
    unsigned long long upper;
    char               nickname[64];
    int                valid;
} daemoon_nx_account_t;

daemoon_result_t daemoon_nx_account_select(daemoon_nx_account_t *out);

/* ---------------------------------------------------------------------- save */

typedef struct {
    daemoon_nx_account_t account;
    /* Names come from the ns service, which has to be reachable for a list to say
     * anything but hex. A failure there is not a failure of the list. */
    int names_available;
} daemoon_nx_save_ctx_t;

extern const daemoon_save_backend_t daemoon_nx_save_backend;

/* --------------------------------------------------------------------- icons */

/* A title's icon, out of the same ns record its name comes from: a JPEG, exactly as
 * the console stores it, decoded by whoever draws it.
 *
 * Returned as an allocated copy because the record it is read out of is around
 * 144 KiB and the icon is a fraction of that - holding the whole thing to keep a
 * thumbnail would be sixty of those alive at once. Free it with free().
 *
 * A title with no icon is normal rather than an error: a save can outlive the game
 * that wrote it, and an archive left behind by a deleted title has nothing to read.
 * The caller draws a plain tile in that case, which keeps the grid aligned where a
 * gap would not. */
daemoon_result_t daemoon_nx_icon_load(unsigned long long app_id, unsigned char **out,
                                      size_t *out_len);

/* The selected account's profile picture, the same way. Also a JPEG, also freed by
 * the caller. */
daemoon_result_t daemoon_nx_account_image(const daemoon_nx_account_t *account,
                                          unsigned char **out, size_t *out_len);

/* ----------------------------------------------------------------- net, misc */

typedef struct {
    /* A CA bundle on the card. Verification is never turned off: a save is not
     * something to hand to whoever answers the connection. */
    const char *ca_bundle;
    int         last_curl_code;
} daemoon_nx_net_ctx_t;

extern const daemoon_net_backend_t daemoon_nx_net_backend;
extern const daemoon_ui_backend_t  daemoon_nx_ui_backend;

typedef struct {
    int selection;
} daemoon_nx_ui_ctx_t;

void daemoon_nx_ui_init(daemoon_nx_ui_ctx_t *ctx);

daemoon_result_t daemoon_nx_net_init(void);
void             daemoon_nx_net_exit(void);

/* One line per step, opened and closed per line so it survives a crash. The 3DS
 * build's most useful diagnostic, and the reason four rounds of hardware debugging
 * became one. */
void daemoon_nx_trace(const char *step, const char *detail);

/* 16 uppercase hex digits, the spelling the rest of the project uses. */
void daemoon_nx_format_title_id(unsigned long long id, char *out, size_t cap);
daemoon_result_t daemoon_nx_parse_title_id(const char *text, unsigned long long *out);

/* The config file, same key=value shape as the 3DS build. */
typedef struct {
    char server_url[256];
    char token[DAEMOON_TOKEN_MAX];
    char device_label[DAEMOON_LABEL_MAX];
    char ca_bundle[DAEMOON_PATH_MAX];
    char device_id[DAEMOON_DEVICE_ID_MAX];
    char language[12];
} daemoon_nx_config_t;

void             daemoon_nx_config_defaults(daemoon_nx_config_t *cfg);
daemoon_result_t daemoon_nx_config_load(const char *path, daemoon_nx_config_t *cfg);
daemoon_result_t daemoon_nx_config_save(const char *path, const daemoon_nx_config_t *cfg);
int              daemoon_nx_config_can_sync(const daemoon_nx_config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* DAEMOON_NX_H */
