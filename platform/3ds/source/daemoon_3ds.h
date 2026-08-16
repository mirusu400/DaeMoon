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

/* Called as the list is read, so the screen can say how far along it is. Reading
 * the list opens every save archive on the console and, for the ones that have
 * one, decrypts a little of the title's content; a still screen through all of
 * that is indistinguishable from a hang. */
typedef void (*daemoon_3ds_progress_fn)(void *user, unsigned done, unsigned total);

typedef struct {
    /* MEDIATYPE_SD for installed titles. Cartridges are a Phase 1 question: they
     * work the same way through AM, but a card being pulled mid write is a failure
     * mode nothing else here has. */
    int media;
    /* Which language to read a title's name in, as an SMDH index: 1 is English,
     * which is also the fallback the console itself uses. */
    int smdh_language;
    daemoon_3ds_progress_fn progress;
    void                   *progress_user;

    /* Restrict listed names to what the console can draw. The survey writes the
     * real ones to a file regardless. */
    int ascii_names;
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

/* A whole SMDH: header, one name per language, then the two icons.
 *
 * It is read in one piece from offset zero, because the file it comes from is
 * decrypted as it is read and a request that starts anywhere else is refused -
 * which the service reports as "not supported", the same words it uses for a
 * missing right. That cost four trips to a console.
 *
 * One read serves both the name and the icon, which is also why they are loaded
 * together now. */
#define DAEMOON_3DS_SMDH_SIZE       0x36C0
#define DAEMOON_3DS_SMDH_NAME_OFF   0x0008
#define DAEMOON_3DS_SMDH_NAME_STRIDE 0x200
#define DAEMOON_3DS_SMDH_ICON_OFF   0x24C0
#define DAEMOON_3DS_SMDH_ICON_DIM   48

/* Reads a title's SMDH. out must hold DAEMOON_3DS_SMDH_SIZE bytes. */
daemoon_result_t daemoon_3ds_smdh_load(int media, unsigned long long title_id, void *out);

/* The name the HOME menu shows, out of an SMDH already read. */
daemoon_result_t daemoon_3ds_smdh_name(const void *smdh, int lang, unsigned flags,
                                       char *out, size_t cap);

/* The name the HOME menu shows, read from the title's SMDH.
 *
 * lang is an SMDH index, which is also the console's own language numbering. A
 * game sold in one region leaves the other slots blank, so this falls back to
 * English, then Japanese, then whatever the game does carry. Returns not_found
 * when it carries nothing at all. */
/* Only accept a name the console's text renderer can actually draw. Its font is
 * 8x8 ASCII with no CJK, so without this a Korean or Japanese title shows up as a
 * blank line - read correctly, and indistinguishable from not read at all.
 * Dropping this flag is part of the Phase 3 font decision. */
#define DAEMOON_3DS_NAME_ASCII 1u

daemoon_result_t daemoon_3ds_title_name(int media, unsigned long long title_id,
                                        int lang, unsigned flags, char *out, size_t cap);

/* The last raw Result from a title name lookup.
 *
 * The wire codes this project uses are deliberately coarse, which is right for a
 * user and useless for working out why a service said no. A 32 bit Result names
 * the module, the summary and the description, and can be looked up. */
unsigned long daemoon_3ds_last_name_result(void);

/* Every step of the last name lookup.
 *
 * The service reports a missing filesystem right, a missing ARM9 right and an
 * unsupported operation with the same word, so the only way to tell them apart is
 * to try each route and write down what each one said. `which` is the route that
 * worked: 1 direct, 2 the ExeFS-only archive, 3 archive then file, 0 none. */
typedef struct {
    long open_direct;
    long open_direct2;
    long open_archive;
    long open_file;
    long read;
    unsigned read_bytes;
    int  which;
} daemoon_3ds_name_probe_t;

const daemoon_3ds_name_probe_t *daemoon_3ds_last_name_probe(void);

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
