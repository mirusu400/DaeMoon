/* backend.h - the platform abstraction.
 *
 * 3DS and Switch differ only in how a save is reached. Manifest handling, conflict
 * resolution, zip packing and API calls are identical, so they are written once in
 * core/ and each platform implements the interfaces below.
 *
 * core/ must never include 3ds.h or switch.h. If something here forces a platform
 * header into core, the interface is wrong, not the rule.
 *
 * Two deliberate differences from the sketch in the root CLAUDE.md:
 *
 *   1. Every entry point takes a leading void *ctx. Without it a backend has to keep
 *      global state, and then two backends cannot coexist in one process - which is
 *      exactly what the posix tests do (a source save and a destination save in the
 *      same test).
 *   2. A save is a small file tree, not one byte stream. 3DS savedata and Switch
 *      savedata both hold multiple files, so open_save yields a handle and files are
 *      opened inside it. daemoon_stream_t is still the byte level type.
 */
#ifndef DAEMOON_BACKEND_H
#define DAEMOON_BACKEND_H

#include <stddef.h>

#include <daemoon/result.h>
#include <daemoon/util/sha256.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DAEMOON_TITLE_ID_MAX 33  /* 32 hex digits plus NUL */
#define DAEMOON_NAME_MAX     128
#define DAEMOON_PATH_MAX     256
#define DAEMOON_LABEL_MAX    65
/* DAEMOON_SHA256_HEX comes from util/sha256.h, where the digest itself lives. */

typedef enum {
    DAEMOON_PLATFORM_UNKNOWN = 0,
    DAEMOON_PLATFORM_3DS,
    DAEMOON_PLATFORM_NX,
    DAEMOON_PLATFORM_NDS
} daemoon_platform_t;

typedef enum {
    DAEMOON_SAVE_UNKNOWN = 0,
    DAEMOON_SAVE_SAVEDATA,
    DAEMOON_SAVE_EXTDATA,
    DAEMOON_SAVE_NDS
} daemoon_save_type_t;

const char        *daemoon_platform_name(daemoon_platform_t p);
daemoon_platform_t daemoon_platform_parse(const char *s, size_t len);
const char        *daemoon_save_type_name(daemoon_save_type_t t);
daemoon_save_type_t daemoon_save_type_parse(const char *s, size_t len);

typedef struct {
    char                id[DAEMOON_TITLE_ID_MAX];
    char                name[DAEMOON_NAME_MAX]; /* UTF-8, from the console */
    daemoon_platform_t  platform;
    daemoon_save_type_t save_type;
    unsigned long long  size_hint; /* 0 when the backend cannot say cheaply */
    unsigned char       has_save;
    /* Set when the title ties its save to a console stored secure value (Pokemon
     * and friends). Restoring another console's save into one of these can make the
     * game treat it as corrupt and delete it, so the user is warned first. */
    unsigned char       secure_value;
    /* Switch: the save belongs to one AccountUid, so a different account is a
     * different save and an account has to be selected before syncing. */
    unsigned char       account_bound;
} daemoon_title_t;

/* ------------------------------------------------------------------ streams */

typedef struct daemoon_stream daemoon_stream_t;

/* out_len 0 with DAEMOON_OK means end of stream. Short reads are normal. */
typedef daemoon_result_t (*daemoon_read_fn)(void *ctx, void *buf, size_t cap, size_t *out_len);
typedef daemoon_result_t (*daemoon_write_fn)(void *ctx, const void *buf, size_t len);

struct daemoon_stream {
    daemoon_read_fn  read;  /* NULL on a write only stream */
    daemoon_write_fn write; /* NULL on a read only stream */
    /* Absolute seek. NULL when the stream cannot seek, which is the normal case for
     * a socket. Zip reading needs it because the central directory is at the end,
     * so packages are always staged as a file before they are read. */
    daemoon_result_t (*seek)(void *ctx, unsigned long long offset);
    daemoon_result_t (*close)(void *ctx);
    unsigned long long size; /* known length for reads, 0 when unknown */
    void *ctx;
};

daemoon_result_t daemoon_stream_read(daemoon_stream_t *s, void *buf, size_t cap, size_t *out_len);
daemoon_result_t daemoon_stream_read_full(daemoon_stream_t *s, void *buf, size_t len);
daemoon_result_t daemoon_stream_write(daemoon_stream_t *s, const void *buf, size_t len);
daemoon_result_t daemoon_stream_seek(daemoon_stream_t *s, unsigned long long offset);
daemoon_result_t daemoon_stream_close(daemoon_stream_t *s);
/* Copy until end of stream through a caller supplied buffer. Nothing in core ever
 * sizes a buffer to the save; the 3DS heap does not have room for that. */
daemoon_result_t daemoon_stream_copy(daemoon_stream_t *dst, daemoon_stream_t *src,
                                     void *scratch, size_t scratch_len,
                                     unsigned long long *out_copied);

typedef enum {
    DAEMOON_OPEN_READ = 0,
    DAEMOON_OPEN_WRITE     /* create or truncate */
} daemoon_open_mode_t;

/* ------------------------------------------------------------- save backend */

typedef struct daemoon_save daemoon_save_t;

/* Return non zero to stop the walk early. Paths use forward slashes and are
 * relative to the save root. */
typedef int (*daemoon_entry_cb)(void *user, const char *path, unsigned long long size);

typedef struct {
    daemoon_result_t (*list_titles)(void *ctx, daemoon_title_t **out, size_t *count);
    void             (*free_titles)(void *ctx, daemoon_title_t *titles, size_t count);

    daemoon_result_t (*open_save)(void *ctx, const daemoon_title_t *t, daemoon_save_t **out);
    daemoon_result_t (*open_save_write)(void *ctx, const daemoon_title_t *t, daemoon_save_t **out);

    daemoon_result_t (*list_entries)(void *ctx, daemoon_save_t *s, daemoon_entry_cb cb, void *user);
    daemoon_result_t (*open_file)(void *ctx, daemoon_save_t *s, const char *path,
                                  daemoon_open_mode_t mode, daemoon_stream_t **out);
    /* Clear the archive before a restore writes into it, so files the incoming save
     * does not have cannot survive. */
    daemoon_result_t (*remove_all)(void *ctx, daemoon_save_t *s);

    /* 3DS: FSUSER_ControlArchive(ARCHIVE_ACTION_COMMIT_SAVE_DATA).
     * Switch: fsdevCommitDevice().
     * Without this nothing is persisted. Never skip it, never ignore its result. */
    daemoon_result_t (*commit)(void *ctx, daemoon_save_t *s);
    daemoon_result_t (*close_save)(void *ctx, daemoon_save_t *s);

    /* Optional. Reports whether a game currently holds the archive. Syncing then is
     * never valid: writes corrupt it and reads come back stale. NULL means the
     * platform cannot tell, and the caller warns instead. */
    daemoon_result_t (*is_title_running)(void *ctx, const daemoon_title_t *t, int *out_running);

    /* Optional, and only the 3DS has it. Some titles bind their save to a value the
     * console stores outside the archive, and a save whose value does not match is
     * one the game treats as corrupt and deletes.
     *
     * Which means the value is part of the save, not part of the console: a backup
     * that does not carry it cannot be fully restored, because the value may have
     * moved on since. So it goes into the manifest at pack time and is written back
     * after a restore. NULL on a platform with no such concept, and both must be
     * NULL or neither.
     *
     * out_exists distinguishes "this title has no secure value" from "the value is
     * zero", which are different states and only one of them is worth writing back. */
    daemoon_result_t (*read_secure_value)(void *ctx, const daemoon_title_t *t,
                                          int *out_exists, unsigned long long *out_value);
    daemoon_result_t (*write_secure_value)(void *ctx, const daemoon_title_t *t,
                                           unsigned long long value);
    /* Removes the console's value for this title, which is what a save that arrived
     * without one needs.
     *
     * A package written before this project recorded the value carries none, and the
     * console still holds the one belonging to whatever save was there before. The
     * game compares them, they differ, and it refuses the restored save. Leaving the
     * value alone makes every such backup unusable on the titles that have one;
     * removing it takes the comparison away rather than losing it, and the game
     * records a fresh value the next time it saves. */
    daemoon_result_t (*clear_secure_value)(void *ctx, const daemoon_title_t *t);
} daemoon_save_backend_t;

/* -------------------------------------------------------------- net backend */

typedef struct {
    const char *name;
    const char *value;
} daemoon_http_header_t;

typedef struct {
    const char                  *method;
    const char                  *url;
    const daemoon_http_header_t *headers;
    size_t                       nheaders;

    /* Request body, pulled in chunks. Both NULL means no body. */
    daemoon_read_fn  body_read;
    void            *body_ctx;
    long long        body_len; /* -1 when unknown */

    /* No unbounded waits anywhere. 0 means the backend default, which must also be
     * finite. */
    int timeout_ms;
} daemoon_http_req_t;

typedef struct {
    /* The backend fills this in BEFORE the first body_write call. Callers rely on
     * it to route an error body away from the sink a success body would go to. */
    int status;

    /* Response body sink, written as it arrives. A blob is never assembled whole in
     * memory on either side of the wire. */
    daemoon_write_fn body_write;
    void            *body_ctx;

    /* Headers the caller cares about, filled in by the backend if present. */
    char sha256[DAEMOON_SHA256_HEX];
    long long content_length; /* -1 when absent */
} daemoon_http_resp_t;

typedef struct {
    daemoon_result_t (*request)(void *ctx, const daemoon_http_req_t *req, daemoon_http_resp_t *out);
} daemoon_net_backend_t;

/* --------------------------------------------------------------- ui backend */

/* A message plus its substitution arguments. The UI never receives a const char *
 * sentence, only an id, which is what keeps literals out of the code. */
typedef struct {
    daemoon_str_id_t id;
    const char      *args[DAEMOON_STR_MAX_ARGS];
    size_t           nargs;
} daemoon_str_ref_t;

typedef struct {
    /* 1 yes, 0 no. Every destructive action goes through this. There is no force
     * flag and none is to be added. */
    int  (*confirm)(void *ctx, const daemoon_str_ref_t *msg);
    void (*progress)(void *ctx, const daemoon_str_ref_t *label, int pct); /* pct <0 = indeterminate */
    /* Index of the chosen option, or negative for cancel. */
    int  (*choose)(void *ctx, const daemoon_str_ref_t *msg, const daemoon_str_ref_t *opts, size_t n);
    void (*notify)(void *ctx, const daemoon_str_ref_t *msg);
} daemoon_ui_backend_t;

/* --------------------------------------------------------------- fs backend */

/* Local storage on the SD card: backups, staging and the token file. Kept apart
 * from the save backend because a backup must survive even when the save archive
 * cannot be opened. */
typedef struct {
    daemoon_result_t (*open)(void *ctx, const char *path, daemoon_open_mode_t mode,
                             daemoon_stream_t **out);
    daemoon_result_t (*remove)(void *ctx, const char *path);
    daemoon_result_t (*rename)(void *ctx, const char *from, const char *to);
    daemoon_result_t (*mkdir_p)(void *ctx, const char *path);
    int              (*exists)(void *ctx, const char *path);
    /* Free bytes on the volume, for refusing a restore that cannot fit. */
    daemoon_result_t (*free_space)(void *ctx, const char *path, unsigned long long *out_bytes);
} daemoon_fs_backend_t;

/* ---------------------------------------------------------------- assembled */

typedef struct {
    const daemoon_save_backend_t *save;
    const daemoon_net_backend_t  *net;
    const daemoon_ui_backend_t   *ui;
    const daemoon_fs_backend_t   *fs;

    void *save_ctx;
    void *net_ctx;
    void *ui_ctx;
    void *fs_ctx;

    /* Optional. Fills an ISO 8601 timestamp for manifest created_at, which is
     * informational only and never used for ordering, because the console RTC is
     * user settable. NULL is fine and yields a fixed placeholder. Nothing in core
     * derives a decision, a filename or a version from this. */
    daemoon_result_t (*clock_iso8601)(void *ctx, char *buf, size_t cap);
    void *clock_ctx;

    const char *server_url;   /* no trailing slash */
    const char *token;        /* NULL when not paired */
    const char *device_label; /* user set, shown in conflict dialogs */
    const char *work_dir;     /* backups and staging, e.g. "sdmc:/DaeMoon" */

    /* Scratch buffer owned by the caller, used for every streaming copy. Core does
     * not allocate one because the 3DS heap fragments badly. 64 KiB is plenty. */
    void   *scratch;
    size_t  scratch_len;
} daemoon_env_t;

/* Checks that every interface the sync path uses is present and that scratch is
 * usable. Cheap, and it turns a NULL function pointer into an error instead of a
 * crash on hardware. */
daemoon_result_t daemoon_env_validate(const daemoon_env_t *env);

#ifdef __cplusplus
}
#endif

#endif /* DAEMOON_BACKEND_H */
