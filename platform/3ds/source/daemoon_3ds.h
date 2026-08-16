/* Shared between the 3DS platform translation units.
 *
 * Nothing here is visible to core, and nothing here appears in core's headers.
 * This is the side of the wall that is allowed to know what a 3DS is.
 */
#ifndef DAEMOON_3DS_H
#define DAEMOON_3DS_H

#include <daemoon/api.h>
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

typedef struct daemoon_3ds_title_cache daemoon_3ds_title_cache_t;

typedef struct {
    /* MEDIATYPE_SD for installed titles. Cartridges are a Phase 1 question: they
     * work the same way through AM, but a card being pulled mid write is a failure
     * mode nothing else here has. */
    int media;
    /* Names and icons already read, so a second launch does not read them again.
     * NULL is allowed and means every lookup goes to the hardware, which is what
     * the conformance build and the survey want. */
    daemoon_3ds_title_cache_t *cache;
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

/* nds-bootstrap saves: plain .sav files on the SD card, in TWiLightMenu's layout
 * of ROMs in one directory and saves in another with the same base name.
 *
 * Phase 2 syncs these first, because there is nothing here that can go wrong in a
 * way unique to the platform: no permissions, no archive to commit, no service
 * that can refuse a read. If the sync path corrupts one of these, it is the sync
 * path. */
#define DAEMOON_3DS_NDS_MAX_TITLES 128
#define DAEMOON_3DS_NDS_ROM_DIR    "sdmc:/roms/nds"
#define DAEMOON_3DS_NDS_SAVE_DIR   "sdmc:/roms/nds/saves"

typedef struct {
    const char *rom_dir;
    const char *save_dir;
} daemoon_3ds_nds_ctx_t;

extern const daemoon_save_backend_t daemoon_3ds_nds_backend;

/* The 32x32 icon out of a DS cartridge's banner, converted into the same 48x48
 * tiled RGB565 buffer an SMDH icon produces - centred, so one upload serves both
 * libraries. out must hold DAEMOON_3DS_ICON_BYTES. not_found when the ROM is not
 * beside the save or carries no banner, which is normal.
 *
 * base is the save's name without .sav, which is also the ROM's name: a real card
 * has files with spaces and Hangul in them and nothing else connects the two. */
daemoon_result_t daemoon_3ds_nds_icon_read(const char *rom_dir, const char *base,
                                           void *out);

/* The network, over 3ds-curl and 3ds-mbedtls rather than httpc:C. That service
 * ships old cipher suites and a stale root CA store, and a self hosted server is
 * exactly the case where nobody can be asked to downgrade their TLS to suit a
 * console. */
typedef struct {
    /* A CA bundle on the SD card. Verification is not turned off when it is
     * missing: a save is not something to hand to whoever answers the
     * connection. */
    const char *ca_bundle;
    /* The last curl code, for a diagnostic that says more than "network error". */
    int last_curl_code;
} daemoon_3ds_net_ctx_t;

extern const daemoon_net_backend_t daemoon_3ds_net_backend;

/* The settings a console cannot work out for itself, from a file on the SD card.
 * Written by hand the first time, and by the pairing flow from Phase 4 on. */
#define DAEMOON_3DS_CONFIG_PATH DAEMOON_3DS_WORK_DIR "/config.txt"

typedef struct {
    char server_url[256];
    char token[DAEMOON_TOKEN_MAX];
    char device_label[DAEMOON_LABEL_MAX];
    char ca_bundle[DAEMOON_PATH_MAX];
    /* The id the server gave this console's current token.
     *
     * Kept so that pairing again can retire the token before it. Pairing mints a
     * new one and leaves the old one working, so a console paired three times is
     * three live credentials and three rows a person cannot tell apart - and the
     * console is the only party that could know they are the same console, because
     * the alternative is a hardware id and those follow somebody across services. */
    char device_id[DAEMOON_DEVICE_ID_MAX];
    /* A language code, or empty for "whatever the console is set to".
     *
     * Empty is the default and is not the same as "en": a console that is later
     * switched to Japanese should follow, and a user who picked English on a
     * Japanese console should not be overruled by it. Storing the choice and the
     * absence of one separately is the only way to tell those apart. */
    char language[12];
} daemoon_3ds_config_t;

void             daemoon_3ds_config_defaults(daemoon_3ds_config_t *cfg);
daemoon_result_t daemoon_3ds_config_load(const char *path, daemoon_3ds_config_t *cfg);
daemoon_result_t daemoon_3ds_config_save(const char *path, const daemoon_3ds_config_t *cfg);
int              daemoon_3ds_config_can_sync(const daemoon_3ds_config_t *cfg);

/* soc:U needs a buffer for the lifetime of the session, so the network is opened
 * once and closed once rather than per request. */
daemoon_result_t daemoon_3ds_net_init(void);
void             daemoon_3ds_net_exit(void);
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

/* The name and the icon, remembered between launches.
 *
 * Reading an SMDH opens the title's content and decrypts the front of it, and it
 * is by a wide margin the slowest thing in the loading screen. The answer only
 * changes when a title is installed, updated or deleted, so it is kept on the SD
 * card and the launch after the first one does not pay for it.
 *
 * Failed lookups are remembered too - on a real console those are the slowest
 * titles of all, because the failure arrives after the open. Which means a bug in
 * the lookup would be remembered as well, so the file carries a format number and
 * a build that changes the lookup discards every older file. The survey never
 * reads this: a diagnostic that answers from a cache is not one. */
#define DAEMOON_3DS_ICON_BYTES \
    (DAEMOON_3DS_SMDH_ICON_DIM * DAEMOON_3DS_SMDH_ICON_DIM * 2)
#define DAEMOON_3DS_CACHE_PATH DAEMOON_3DS_WORK_DIR "/cache/titles.bin"

daemoon_result_t daemoon_3ds_cache_open(const char *path, int lang,
                                        daemoon_3ds_title_cache_t **out);
void             daemoon_3ds_cache_close(daemoon_3ds_title_cache_t *cache);

/* Same contract as daemoon_3ds_title_name, answered from the cache when it can be.
 * unsupported means the name exists and the console cannot draw it, which is a
 * different problem from not_found and gets a different fallback. */
daemoon_result_t daemoon_3ds_cache_name(daemoon_3ds_title_cache_t *cache, int media,
                                        unsigned long long title_id, unsigned flags,
                                        char *out, size_t cap);

/* The raw 48x48 tiled RGB565 icon, or NULL. Owned by the cache. */
const void *daemoon_3ds_cache_icon(daemoon_3ds_title_cache_t *cache, int media,
                                   unsigned long long title_id);

/* Throw the whole thing away and read from hardware again. */
void daemoon_3ds_cache_forget(daemoon_3ds_title_cache_t *cache);

/* Writes back only what was asked for since the last flush, so titles that have
 * been deleted from the console drop out of the file. */
daemoon_result_t daemoon_3ds_cache_flush(daemoon_3ds_title_cache_t *cache,
                                         const char *path);

unsigned daemoon_3ds_cache_hits(const daemoon_3ds_title_cache_t *cache);
unsigned daemoon_3ds_cache_misses(const daemoon_3ds_title_cache_t *cache);

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
    /* Which language slot the name came from, or -1 when none was usable. The
     * console's own language is not always the one a game carries. */
    int  name_lang;
    /* Set when a slot held a name but the renderer could not draw it. That is a
     * font problem wearing the same clothes as a missing name. */
    int  rejected_non_ascii;
} daemoon_3ds_name_probe_t;

const daemoon_3ds_name_probe_t *daemoon_3ds_last_name_probe(void);

/* Creates this application's own save archive, and refuses any other title. A
 * declared SaveDataSize does not create one; the title has to format it once. */
daemoon_result_t daemoon_3ds_format_own_save(const daemoon_title_t *t, unsigned blocks);

/* One backup, as the picker needs to show it.
 *
 * The metadata is read out of each package's manifest when the list is built, not
 * per frame: a backup is named after its content digest, which is the right name
 * for storage and tells a person nothing. */
typedef struct {
    char               name[96];
    unsigned long long size;
    char               created_at[DAEMOON_TIMESTAMP_MAX];
    char               device_label[DAEMOON_LABEL_MAX];
    char               sha256[DAEMOON_SHA256_HEX];
    /* Cleared when the package could not be read. Such a row is still shown -
     * it is a file taking up space and the user has to be able to delete it. */
    int                readable;
    /* Whether this package holds exactly what is on the console right now. A fact
     * about content rather than about clocks, so unlike created_at it can be
     * trusted: restoring it would change nothing. */
    int                is_current;
} daemoon_3ds_backup_row_t;

/* Fills rows with the backups belonging to one title, newest first by the date in
 * their manifests, and returns how many. Kept apart from the screen that draws
 * them so it can be run on a desktop against packages a card reader has damaged.
 *
 * current_digest may be NULL; when given, the row holding exactly what is on the
 * console right now is marked. */
size_t daemoon_3ds_backup_list(const daemoon_env_t *env, const char *dir,
                               const daemoon_title_t *title, const char *current_digest,
                               daemoon_3ds_backup_row_t *rows, size_t cap);

/* "967 B", "1 KB", "2.5 MB". */
void daemoon_3ds_backup_size_text(unsigned long long bytes, char *out, size_t cap);

/* Lists the backups belonging to one title, with what each package's own manifest
 * says about it, and lets the user choose one to restore or delete. Returns
 * not_found when there are none, and user_cancelled when they back out.
 *
 * current_digest may be NULL. When it is given, the backup holding exactly what is
 * on the console right now is marked - a fact about content, unlike the dates,
 * which come from a clock the user can set. */
daemoon_result_t daemoon_3ds_pick_backup(const daemoon_env_t *env, const char *dir,
                                         const daemoon_title_t *title,
                                         const char *current_digest, char *out,
                                         size_t cap);

/* One line per step, on the card, closed after each write.
 *
 * A console that dies mid action says nothing on its own: the screen is gone, and
 * a Luma dump only exists for the faults Luma catches and only means something
 * once its addresses are matched back to the right build. A trail of steps costs
 * a few SD writes per user action and answers "how far did it get" without
 * another trip to the console. */
#define DAEMOON_3DS_TRACE_PATH DAEMOON_3DS_WORK_DIR "/trace.txt"

/* An unattended pairing, from a payload on the card in exactly the form a scan
 * would produce. The camera is the only part of the QR path a desktop cannot stand
 * in for; this runs the rest as a real ARM binary against a real server, so the
 * emulator run and the hardware run differ in one step and nothing else. */
#define DAEMOON_3DS_AUTOPAIR_PATH DAEMOON_3DS_WORK_DIR "/AUTOPAIR"

void daemoon_3ds_trace(const char *step, const char *detail);
void daemoon_3ds_trace_uint(const char *step, unsigned long long value);

/* Draws the backup picker for a number of frames with no input, so an unattended
 * run can prove the drawing does not fault. Needs a console: it is in the header
 * only so the autotest can reach it. */
void daemoon_3ds_pick_backup_render_check(const daemoon_title_t *title, unsigned frames);

/* Reading a pairing code off the camera.
 *
 * frame_cb is called once per camera frame so the caller can draw and watch for a
 * cancel; returning 0 stops the scan. It is the only way out of a screen that is
 * otherwise waiting on hardware, which is why the receive has a timeout as well.
 *
 * Returns not_found when the loop ended without a readable code, user_cancelled
 * when frame_cb said so, and backend_error when the camera would not start. */
/* The camera frame size, so the preview can be scaled to the screen without the
 * drawing code guessing at it. */
#define DAEMOON_3DS_CAM_W 400
#define DAEMOON_3DS_CAM_H 240

/* Returns 1 to keep scanning, 0 to stop, 2 to switch cameras. */
typedef int (*daemoon_3ds_qr_frame_cb)(void *user);

daemoon_result_t daemoon_3ds_qr_scan(daemoon_3ds_qr_frame_cb frame_cb, void *user,
                                     char *out, size_t cap);

/* What the last scan saw.
 *
 * A scan that fails does so in one of three ways and they need different things
 * done about them: no frames at all is a camera that never started, frames with a
 * mean luma near zero is a lens covered or a room dark, and frames with codes seen
 * but not decoded is a code too small, too far, or out of focus. Without these
 * three numbers all of it is "the scan did not work". */
typedef struct {
    unsigned frames;
    unsigned mean_luma;
    int      codes_seen;
    unsigned decode_failures;
    unsigned receive_failures;
    unsigned timeouts;
    /* The camera port overran: a frame arrived with no receive armed. It stays
     * stopped until cleared, so one slow pass used to wedge it rather than drop a
     * frame. */
    unsigned buffer_errors;
    int      last_decode_error;
    int      camera; /* 0 outer, 1 inner */
    int      layout; /* which candidate sensor layout the preview is drawing */
    /* Milliseconds in each stage of the last pass.
     *
     * The preview was slow twice and both fixes were guesses at which stage was
     * the expensive one. There are four candidates - waiting for a frame, tiling
     * it for the GPU, drawing, and decoding - and no amount of reading tells them
     * apart on a 268 MHz processor. These do. */
    unsigned ms_capture;
    unsigned ms_tile;
    unsigned ms_draw;
    unsigned ms_decode;
} daemoon_3ds_qr_stats_t;

const daemoon_3ds_qr_stats_t *daemoon_3ds_qr_last_stats(void);

/* A camera frame, rotated and tiled into something the GPU can sample.
 *
 * Kept out of the scanner and free of citro2d so the swizzle and the rotation can
 * be checked on a desktop. A wrong swizzle does not fail - it draws noise, and
 * noise on a console is a photograph and a guess. */
size_t daemoon_3ds_tile_index(unsigned x, unsigned y, unsigned tex_w);
size_t daemoon_3ds_cam_index(unsigned x, unsigned y, unsigned cam_w);

/* Four candidate sensor layouts, so the console can be asked which one it uses
 * rather than told. 0 is what ships; the rest exist to be ruled out on screen. */
#define DAEMOON_3DS_CAM_LAYOUTS 4
/* rows/TD, which is what one console answered when asked by name. The index is not
 * moved to make it first: renumbering these is what made an earlier answer
 * ambiguous, and being the default is not worth being called zero. */
#define DAEMOON_3DS_CAM_LAYOUT_DEFAULT 2
size_t daemoon_3ds_cam_index_as(unsigned x, unsigned y, unsigned cam_w,
                                unsigned cam_h, int layout);
/* A name rather than a number: the layouts were renumbered once and the answer
 * that came back meant two different things depending on which build asked. */
const char *daemoon_3ds_cam_layout_name(int layout);
void   daemoon_3ds_cam_to_tiled(const unsigned short *frame, unsigned cam_w,
                                unsigned cam_h, unsigned short *tex, unsigned tex_w,
                                unsigned tex_h, int layout);

/* The console UI keeps a little state: which line is selected, what to draw. */
typedef struct {
    int selection;
} daemoon_3ds_ui_ctx_t;

void daemoon_3ds_ui_init(daemoon_3ds_ui_ctx_t *ctx);

#endif /* DAEMOON_3DS_H */
