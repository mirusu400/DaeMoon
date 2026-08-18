/* What a sync at startup is allowed to do, and what it writes down afterwards.
 *
 * Separate from autosync.c, which drives it, so the two decisions that matter can be
 * checked on a desktop: which options an unattended run uses, and what the report
 * says. Both are the whole of Phase 5's safety argument, and neither needs a console.
 */
#include "daemoon_3ds.h"

#include <daemoon/util/strbuf.h>

#include <stdio.h>
#include <string.h>

daemoon_sync_opts_t daemoon_3ds_autosync_opts(void)
{
    daemoon_sync_opts_t opts;

    memset(&opts, 0, sizeof(opts));

    /* Defer, and this is the only defensible answer.
     *
     * Nobody is watching a console that has just been switched on, and both of the
     * other policies pick a side. Picking a side is a decision, and the one thing
     * this project will not do is make that decision on somebody's save while they
     * are not there. A deferred conflict leaves both versions exactly where they
     * are and gets counted, so the next launch has something to say. */
    opts.conflict = DAEMOON_CONFLICT_POLICY_DEFER;

    /* An upload changes nothing on the console and the server adds a version rather
     * than replacing one. There is no way for this to lose anything, and it is the
     * whole point of syncing at startup: the save from last night's session is on
     * the server before anybody picks the console up. */
    opts.upload_confirmed = 1;

    /* And a download, but only the ones that are left after DEFER has taken the
     * conflicts out. Those are the titles where this console has not moved since the
     * last sync, so what gets overwritten is a copy of a version the server still
     * holds - and rule 1 puts it on the SD card first regardless.
     *
     * That is what makes this safe without a person present: after DEFER, no restore
     * this run performs can destroy anything that exists in only one place. */
    opts.restore_confirmed = 1;

    return opts;
}

int daemoon_3ds_autosync_due(int enabled, int can_sync, int cancel_held, int unattended)
{
    /* Every one of these has to be true, and each is a different kind of no:
     * turned off, nowhere to sync to, the person asked for the menu, or another
     * unattended mode already owns this launch. */
    return enabled && can_sync && !cancel_held && !unattended;
}

daemoon_result_t daemoon_3ds_autosync_write_report(const daemoon_env_t *env, const char *path,
                                                   const char *when,
                                                   const daemoon_3ds_autosync_report_t *rep)
{
    daemoon_stream_t *out = NULL;
    char line[256];
    daemoon_result_t r;

    if (env == NULL || path == NULL || rep == NULL) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }

    /* Written every time, including the runs that did nothing.
     *
     * Nobody sees this screen: the console is on its way to HOME. A run that found
     * no network and a run that never started look identical from the outside, and
     * an absent file is the one outcome that cannot be told apart from a build where
     * this feature is missing. */
    DAEMOON_TRY(env->fs->open(env->fs_ctx, path, DAEMOON_OPEN_WRITE, &out));

    (void)snprintf(line, sizeof(line),
                   "when=%s\n"
                   "titles=%u\n"
                   "uploaded=%u\ndownloaded=%u\nunchanged=%u\ndeferred=%u\nfailed=%u\n"
                   "network=%s\n",
                   when != NULL ? when : "unknown",
                   rep->titles,
                   rep->sync.uploaded, rep->sync.downloaded, rep->sync.skipped,
                   rep->sync.conflicts, rep->sync.failed + rep->failed,
                   rep->network ? "ok" : "unreachable");
    r = daemoon_stream_write(out, line, strlen(line));
    {
        daemoon_result_t cr = daemoon_stream_close(out);

        if (r == DAEMOON_OK) {
            r = cr;
        }
    }
    return r;
}
