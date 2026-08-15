/* sync.h - what to do with a title, and doing it.
 *
 * The decision never looks at a clock. Console RTC is user settable, so freshness
 * comes from the server issued version and from content digests, nothing else.
 *
 * Two versions are never merged. When both sides moved, the user chooses and both
 * versions stay on the server.
 */
#ifndef DAEMOON_SYNC_H
#define DAEMOON_SYNC_H

#include <stddef.h>

#include <daemoon/api.h>
#include <daemoon/archive.h>
#include <daemoon/backend.h>
#include <daemoon/manifest.h>
#include <daemoon/result.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DAEMOON_SYNC_NONE = 0, /* both sides agree, or there is nothing anywhere */
    DAEMOON_SYNC_UPLOAD,   /* only this console moved */
    DAEMOON_SYNC_DOWNLOAD, /* only the server moved */
    DAEMOON_SYNC_CONFLICT  /* both moved: ask, never merge */
} daemoon_sync_action_t;

const char *daemoon_sync_action_name(daemoon_sync_action_t a);
daemoon_str_id_t daemoon_sync_action_str(daemoon_sync_action_t a);

/* What this console has. base_* is the record of the last successful sync, kept in
 * work_dir; it is what makes "the save changed since we last synced" answerable
 * without a timestamp. */
typedef struct {
    int                has_save;
    char               sha256[DAEMOON_SHA256_HEX];
    unsigned long long size;
    unsigned int       base_version;                 /* DAEMOON_VERSION_NONE if never synced */
    char               base_sha256[DAEMOON_SHA256_HEX]; /* empty if never synced */
} daemoon_local_state_t;

/* Pure function, no IO. Every branch of it is unit tested on desktop. */
daemoon_sync_action_t daemoon_sync_decide(const daemoon_local_state_t *local,
                                          const daemoon_remote_meta_t *remote);

/* Whether the save differs from the one recorded at the last successful sync. */
int daemoon_sync_local_dirty(const daemoon_local_state_t *local);

/* The sync state file for one title, under work_dir. Written to a temp path and
 * renamed, so an interrupted write cannot produce a half readable state file. */
daemoon_result_t daemoon_sync_state_load(const daemoon_env_t *env, daemoon_platform_t platform,
                                         const char *title_id, daemoon_local_state_t *out);
daemoon_result_t daemoon_sync_state_save(const daemoon_env_t *env, daemoon_platform_t platform,
                                         const char *title_id, const daemoon_local_state_t *st);

/* Inspect the console side: hash the current save and merge in the recorded base. */
daemoon_result_t daemoon_sync_scan_local(const daemoon_env_t *env, daemoon_archive_ctx_t *actx,
                                         const daemoon_title_t *title, daemoon_local_state_t *out);

typedef enum {
    DAEMOON_CONFLICT_KEEP_LOCAL = 0,
    DAEMOON_CONFLICT_KEEP_SERVER,
    DAEMOON_CONFLICT_DEFER
} daemoon_conflict_choice_t;

typedef struct {
    unsigned uploaded;
    unsigned downloaded;
    unsigned skipped;
    unsigned conflicts;
    unsigned failed;
} daemoon_sync_stats_t;

/* One title, end to end: decide, confirm, act, verify, commit, record.
 *
 * Guarantees, in order, from the top priority rules:
 *   - a restore always makes a local backup first, and aborts if that fails
 *   - the package digest is verified immediately before anything is written
 *   - the save archive is committed and the commit result is checked
 *   - nothing destructive happens without ui->confirm
 *   - conflicts are resolved by the user and both versions are kept
 */
daemoon_result_t daemoon_sync_title(const daemoon_env_t *env, daemoon_archive_ctx_t *actx,
                                    const daemoon_title_t *title, daemoon_sync_stats_t *stats);

/* Back up a title's save into work_dir/backups without touching the server. This is
 * the whole of Phase 1 and the first half of every restore. out_path receives the
 * package path when non NULL. */
daemoon_result_t daemoon_sync_backup_local(const daemoon_env_t *env, daemoon_archive_ctx_t *actx,
                                           const daemoon_title_t *title, char *out_path,
                                           size_t path_len);

/* Restore a staged package into a title. Verifies, confirms, clears, writes,
 * commits. pkg_path must already exist under work_dir. */
daemoon_result_t daemoon_sync_restore_package(const daemoon_env_t *env, daemoon_archive_ctx_t *actx,
                                              const daemoon_title_t *title, const char *pkg_path);

#ifdef __cplusplus
}
#endif

#endif /* DAEMOON_SYNC_H */
