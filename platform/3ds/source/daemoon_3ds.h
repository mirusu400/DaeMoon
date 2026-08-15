/* Shared between the 3DS platform translation units.
 *
 * Nothing here is visible to core, and nothing here appears in core's headers.
 * This is the side of the wall that is allowed to know what a 3DS is.
 */
#ifndef DAEMOON_3DS_H
#define DAEMOON_3DS_H

#include <daemoon/backend.h>

/* Where the app keeps backups, staging and per title sync state. On the SD card,
 * because a save archive is not a place to put anything that has to survive the
 * save being cleared. */
#define DAEMOON_3DS_WORK_DIR "sdmc:/DaeMoon"

typedef struct {
    /* MEDIATYPE_SD for installed titles. Cartridges are a Phase 1 question: they
     * work the same way through AM, but a card being pulled mid write is a failure
     * mode nothing else here has. */
    int media;
    /* Skip titles with no save archive when listing. The menu wants that; a
     * conformance run does not. */
    int only_with_saves;
} daemoon_3ds_save_ctx_t;

extern const daemoon_save_backend_t daemoon_3ds_save_backend;
extern const daemoon_fs_backend_t   daemoon_3ds_fs_backend;
extern const daemoon_ui_backend_t   daemoon_3ds_ui_backend;

/* 16 uppercase hex digits, the spelling used everywhere else in the project. */
void             daemoon_3ds_format_title_id(unsigned long long id, char *out, size_t cap);
daemoon_result_t daemoon_3ds_parse_title_id(const char *text, unsigned long long *out);

/* Some titles verify their save against a console stored value. Restoring another
 * console's save into one of those can make the game treat it as corrupt and
 * delete it, so the value is read before a restore and put back after.
 *
 * Whether that is sufficient is exactly the Phase 1 question, and it is why the
 * menu can show one without restoring anything. */
typedef struct {
    int                exists;
    unsigned long long value;
} daemoon_3ds_secure_value_t;

daemoon_result_t daemoon_3ds_read_secure_value(const daemoon_title_t *t,
                                               daemoon_3ds_secure_value_t *out);
daemoon_result_t daemoon_3ds_write_secure_value(const daemoon_title_t *t,
                                                const daemoon_3ds_secure_value_t *value);

/* Creates this application's own save archive, and refuses any other title. A
 * declared SaveDataSize does not create one; the title has to format it once. */
daemoon_result_t daemoon_3ds_format_own_save(const daemoon_title_t *t, unsigned blocks);

/* Lists the backups belonging to one title and lets the user choose. Returns
 * not_found when there are none, and user_cancelled when they back out. */
daemoon_result_t daemoon_3ds_pick_backup(const char *dir, const daemoon_title_t *title,
                                         char *out, size_t cap);

/* The console UI keeps a little state: which line is selected, what to draw. */
typedef struct {
    int selection;
} daemoon_3ds_ui_ctx_t;

void daemoon_3ds_ui_init(daemoon_3ds_ui_ctx_t *ctx);

#endif /* DAEMOON_3DS_H */
