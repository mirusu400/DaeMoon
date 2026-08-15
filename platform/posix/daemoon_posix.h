/* daemoon_posix.h - the desktop backend.
 *
 * This is a required component, not a convenience. It presents an ordinary
 * directory as a save archive, which lets the entire sync path run under a unit
 * test on a build machine. Console debugging is by far the most expensive kind, so
 * new core logic passes here before it goes near hardware.
 *
 * It is also the only backend with fault injection. A console will not fail a
 * commit on demand, and the code that has to survive a failed commit is exactly the
 * code that must never be shipped untested.
 */
#ifndef DAEMOON_POSIX_H
#define DAEMOON_POSIX_H

#include <daemoon/backend.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DAEMOON_POSIX_MAX_TITLES 32

/* ---------------------------------------------------------------- save/fs */

typedef struct {
    /* Saves live at <root>/<platform>_<title id>/. */
    char            root[DAEMOON_PATH_MAX];
    daemoon_title_t titles[DAEMOON_POSIX_MAX_TITLES];
    size_t          ntitles;

    /* Observed by tests. commits counts successful commit calls. */
    unsigned commits;
    unsigned opens;

    /* Fault injection. Non zero makes the matching call return that result. */
    daemoon_result_t fail_commit;
    daemoon_result_t fail_open_write;
} daemoon_posix_save_ctx_t;

/* Local storage. All paths are used as given, so tests hand it a temp directory. */
typedef struct {
    unsigned         writes;
    daemoon_result_t fail_open_write;
} daemoon_posix_fs_ctx_t;

void daemoon_posix_save_init(daemoon_posix_save_ctx_t *ctx, const char *root);

/* Register a title. The directory is created on demand by the first write. */
daemoon_result_t daemoon_posix_save_add_title(daemoon_posix_save_ctx_t *ctx, const char *title_id,
                                              const char *name, daemoon_platform_t platform,
                                              daemoon_save_type_t save_type);

/* Directory that backs one title, for tests that want to inspect it. */
daemoon_result_t daemoon_posix_save_dir(const daemoon_posix_save_ctx_t *ctx,
                                        const daemoon_title_t *title, char *buf, size_t cap);

extern const daemoon_save_backend_t daemoon_posix_save_backend;
extern const daemoon_fs_backend_t   daemoon_posix_fs_backend;

/* ---------------------------------------------------------------------- ui */

/* Scripted UI. Real consoles have a person behind these calls; tests need the
 * answers decided up front and the questions recorded. */
typedef struct {
    int confirm_answer; /* 1 yes (default), 0 no */
    int choose_answer;  /* index, or negative for cancel */

    unsigned confirms;
    unsigned chooses;
    unsigned notifies;
    unsigned progresses;

    daemoon_str_id_t last_confirm;
    daemoon_str_id_t last_choose;
    daemoon_str_id_t last_notify;

    /* When set, every prompt is also printed with the current language applied,
     * which is how the i18n path gets exercised rather than merely compiled. */
    int verbose;
} daemoon_posix_ui_ctx_t;

void daemoon_posix_ui_init(daemoon_posix_ui_ctx_t *ctx);
extern const daemoon_ui_backend_t daemoon_posix_ui_backend;

/* --------------------------------------------------------------------- net */

/* Plain HTTP/1.1 over a socket. No TLS: this exists to run the client against a
 * daemoond on localhost during development, and nothing else. The console backends
 * use 3ds-curl and libnx respectively, and those are the ones that ship. */
typedef struct {
    int      connect_timeout_ms;
    unsigned requests;
} daemoon_posix_net_ctx_t;

void daemoon_posix_net_init(daemoon_posix_net_ctx_t *ctx);
extern const daemoon_net_backend_t daemoon_posix_net_backend;

/* --------------------------------------------------------------------- misc */

/* ISO 8601 UTC from the host clock, for daemoon_env_t.clock_iso8601. Informational
 * only, like every timestamp in this project. */
daemoon_result_t daemoon_posix_clock_iso8601(void *ctx, char *buf, size_t cap);

/* rm -rf, used by tests to clean a temp tree. */
daemoon_result_t daemoon_posix_rmtree(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* DAEMOON_POSIX_H */
