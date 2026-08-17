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

/* How a conflict is answered.
 *
 * ASK is what a single title does and what the rules describe: the user chooses and
 * both versions are kept. The other two exist for a run that covers a whole
 * library, where being asked once per title is not a decision anybody makes forty
 * times - it is a dialog somebody holds A through, which is worse than choosing.
 *
 * Neither of them merges and neither discards a version. KEEP_LOCAL uploads on top,
 * and the server keeps everything it had. KEEP_SERVER downloads, and the restore it
 * goes through backs the console's save up to the SD card first, exactly as rule 1
 * requires. So both sides of every conflict still exist afterwards, which is the
 * property the "never auto merge" rule is protecting.
 */
typedef enum {
    DAEMOON_CONFLICT_POLICY_ASK = 0,
    DAEMOON_CONFLICT_POLICY_KEEP_LOCAL,
    DAEMOON_CONFLICT_POLICY_KEEP_SERVER
} daemoon_conflict_policy_t;

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

/* The questions a run over many titles has already asked.
 *
 * Both fields exist because asking per title turns a decision into a reflex when the
 * run covers a library, and a person holding A through forty dialogs is not somebody
 * who was consulted. What they may replace is carefully bounded:
 *
 *   conflict        - which side wins. Neither answer merges and neither discards a
 *                     version. See daemoon_conflict_policy_t.
 *   upload_confirmed - the "send this save to the server?" question. An upload is not
 *                     destructive: nothing on the console changes, and the server
 *                     adds a version rather than replacing one. The question is about
 *                     intent, and the caller states that it has been answered.
 *   restore_confirmed - the "overwrite this console's save?" question. This one is
 *                     rule 7, and setting it is a real decision rather than a
 *                     convenience, so a caller that does has obligations:
 *
 *                       - it asked, and the sentence it asked said that saves on the
 *                         console will be overwritten and how many
 *                       - it is a run the person started deliberately, not a step
 *                         inside something else
 *
 *                     What does not move is rule 1. Every restore still backs the
 *                     console's save up to local storage first and still aborts if
 *                     that backup fails, so what gets overwritten is recoverable.
 *                     That is the whole reason this field can exist at all, and it
 *                     is why there is no field that turns the backup off.
 *
 * daemoon_sync_restore_package - the published restore - always asks. A caller
 * holding a package has not been through such a screen.
 */
typedef struct {
    daemoon_conflict_policy_t conflict;
    int                       upload_confirmed;
    int                       restore_confirmed;
} daemoon_sync_opts_t;

/* The same thing, with those questions answered in advance. NULL opts means ask
 * about everything, which is what daemoon_sync_title passes.
 *
 * Every other guarantee is identical: the confirmation before a restore, the local
 * backup, the digest check, the commit.
 */
daemoon_result_t daemoon_sync_title_with(const daemoon_env_t *env, daemoon_archive_ctx_t *actx,
                                         const daemoon_title_t *title,
                                         const daemoon_sync_opts_t *opts,
                                         daemoon_sync_stats_t *stats);

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
