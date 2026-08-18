/* The sync that happens on the way to HOME.
 *
 * Luma can autoboot a title when the console is switched on. Set it to this one and
 * every power-on becomes: sync, then HOME. The reason that is worth building is in
 * the root CLAUDE.md - a sync is only valid before or after a game runs, and there
 * is no moment more certainly "before" than the console having just been turned on.
 *
 * It is also the moment nobody is watching, and that narrows what this is allowed to
 * do rather than widening it. daemoon_3ds_autosync_opts is where that argument lives;
 * this file is the screen and the loop.
 *
 * Three things it owes somebody who is not looking at it:
 *
 *   - a way out, before anything runs, that does not need timing a button press
 *   - a file saying what happened, because the screen is gone by the time anyone
 *     could read it
 *   - the network being absent must be a line in that file and not a hang
 */
#include "daemoon_3ds.h"
#include "gfx.h"

#include <3ds.h>

#include <stdio.h>
#include <string.h>

/* Long enough to read the sentence and get a thumb onto B, short enough that a
 * console set up to do this does not feel like it is waiting for permission. */
#define AUTOSYNC_GRACE_FRAMES 180 /* three seconds at 60Hz */
#define AUTOSYNC_LINGER_SECS  5

static void draw_banner(daemoon_str_id_t body, const char *detail)
{
    daemoon_gfx_frame_begin();
    daemoon_gfx_top();
    daemoon_gfx_rect(0.0f, 0.0f, GFX_TOP_W, 28.0f, GFX_ACCENT);
    daemoon_gfx_text(12.0f, 6.0f, 0.6f, GFX_TEXT,
                     daemoon_str(DAEMOON_STR_AUTOSYNC_TITLE));
    (void)daemoon_gfx_text_wrapped(16.0f, 56.0f, GFX_TOP_W - 32.0f, 0.5f, GFX_TEXT,
                                   daemoon_str(body));
    if (detail != NULL) {
        (void)daemoon_gfx_text_wrapped(16.0f, 150.0f, GFX_TOP_W - 32.0f, 0.45f,
                                       GFX_TEXT_DIM, detail);
    }
    daemoon_gfx_bottom();
    daemoon_gfx_frame_end();
}

/* The way out, offered before anything is touched.
 *
 * Held rather than pressed: a console that has just booted is one somebody is
 * picking up, and a press that has to land inside a window they cannot see is not a
 * way out. Returns 1 to go ahead. */
static int grace_period(void)
{
    int frames;

    for (frames = 0; frames < AUTOSYNC_GRACE_FRAMES; ++frames) {
        if (!aptMainLoop()) {
            return 0;
        }
        hidScanInput();
        if (hidKeysHeld() & KEY_B) {
            return 0;
        }
        draw_banner(DAEMOON_STR_AUTOSYNC_STARTING, NULL);
    }
    return 1;
}

/* The summary, then out. Counted down on screen so a console that is about to close
 * an application does not look like one that has stopped. */
static void linger(daemoon_str_id_t body, const char *detail)
{
    int secs;

    for (secs = AUTOSYNC_LINGER_SECS; secs > 0; --secs) {
        int frames;
        char line[96];
        char number[8];
        const char *args[1];

        (void)snprintf(number, sizeof(number), "%d", secs);
        args[0] = number;
        (void)daemoon_strf(line, sizeof(line), DAEMOON_STR_AUTOSYNC_RETURNING, args, 1);

        for (frames = 0; frames < 60; ++frames) {
            if (!aptMainLoop()) {
                return;
            }
            hidScanInput();
            /* A press skips the wait. Somebody watching should not have to. */
            if (hidKeysDown() & (KEY_A | KEY_B | KEY_START)) {
                return;
            }
            daemoon_gfx_frame_begin();
            daemoon_gfx_top();
            daemoon_gfx_rect(0.0f, 0.0f, GFX_TOP_W, 28.0f, GFX_OK);
            daemoon_gfx_text(12.0f, 6.0f, 0.6f, GFX_TEXT,
                             daemoon_str(DAEMOON_STR_AUTOSYNC_TITLE));
            (void)daemoon_gfx_text_wrapped(16.0f, 56.0f, GFX_TOP_W - 32.0f, 0.5f,
                                           GFX_TEXT, daemoon_str(body));
            if (detail != NULL) {
                (void)daemoon_gfx_text_wrapped(16.0f, 140.0f, GFX_TOP_W - 32.0f, 0.45f,
                                               GFX_TEXT_DIM, detail);
            }
            daemoon_gfx_text(16.0f, GFX_SCREEN_H - 26.0f, 0.42f, GFX_TEXT_DIM, line);
            daemoon_gfx_frame_end();
        }
    }
}

int daemoon_3ds_autosync_run(const daemoon_3ds_autosync_ctx_t *ctx)
{
    daemoon_sync_opts_t opts = daemoon_3ds_autosync_opts();
    daemoon_3ds_autosync_report_t rep;
    char detail[192];
    char when[40];
    int library;

    memset(&rep, 0, sizeof(rep));
    when[0] = '\0';
    if (ctx->when != NULL) {
        ctx->when(ctx->user, when, sizeof(when));
    }

    daemoon_3ds_trace("autosync/begin", NULL);
    if (!grace_period()) {
        daemoon_3ds_trace("autosync/cancelled", NULL);
        return 0;
    }

    /* Wi-Fi is not up the instant a console is, and this runs earlier in a boot than
     * anything else in this application ever has. A failure here is not a failure of
     * the sync: nothing has been touched, and the next start tries again. */
    draw_banner(DAEMOON_STR_AUTOSYNC_NETWORK, NULL);
    rep.network = ctx->wait_for_network(ctx->user);
    if (!rep.network) {
        daemoon_3ds_trace("autosync/no-network", NULL);
        (void)daemoon_3ds_autosync_write_report(ctx->env, DAEMOON_3DS_AUTOSYNC_REPORT,
                                                when, &rep);
        linger(DAEMOON_STR_AUTOSYNC_NO_NETWORK, NULL);
        return 1;
    }

    /* Both libraries, because a console carries both and the person who set this up
     * did not set it up for half of their saves. */
    for (library = 0; library < 2; ++library) {
        size_t count = ctx->count(ctx->user, library);
        size_t i;

        for (i = 0; i < count; ++i) {
            daemoon_result_t r;

            if (!aptMainLoop()) {
                daemoon_3ds_trace("autosync/interrupted", NULL);
                break;
            }
            draw_banner(DAEMOON_STR_AUTOSYNC_TITLE, ctx->name(ctx->user, library, i));

            r = ctx->sync_one(ctx->user, library, i, &opts, &rep.sync);
            ++rep.titles;
            if (r != DAEMOON_OK && r != DAEMOON_ERR_USER_CANCELLED) {
                ++rep.failed;
            }
        }
    }

    daemoon_3ds_trace("autosync/done", NULL);

    /* Before the summary is drawn, not after: the file is the account that survives,
     * and a console that is switched off during the countdown must still have it. */
    {
        daemoon_result_t wr = daemoon_3ds_autosync_write_report(
            ctx->env, DAEMOON_3DS_AUTOSYNC_REPORT, when, &rep);

        daemoon_3ds_trace("autosync/report", daemoon_result_code(wr));
    }

    /* The counts, in the same sentence a manual sync uses. */
    {
        char counts[4][12];
        const char *args[4];
        size_t i;
        const unsigned n[4] = { rep.sync.uploaded, rep.sync.downloaded,
                                rep.sync.skipped, rep.sync.conflicts };

        for (i = 0; i < 4; ++i) {
            (void)snprintf(counts[i], sizeof(counts[i]), "%u", n[i]);
            args[i] = counts[i];
        }
        (void)daemoon_strf(detail, sizeof(detail), DAEMOON_STR_SYNC_RESULT, args, 4);
    }

    /* A deferred conflict is the one outcome that needs somebody, so it gets its own
     * sentence rather than being a number in a row of four. */
    if (rep.sync.conflicts > 0) {
        char left[128];
        char number[12];
        const char *args[1];
        size_t len = strlen(detail);

        (void)snprintf(number, sizeof(number), "%u", rep.sync.conflicts);
        args[0] = number;
        (void)daemoon_strf(left, sizeof(left), DAEMOON_STR_AUTOSYNC_LEFT, args, 1);
        (void)snprintf(detail + len, sizeof(detail) - len, "\n%s", left);
    }

    linger(rep.titles > 0 ? DAEMOON_STR_AUTOSYNC_TITLE : DAEMOON_STR_AUTOSYNC_NOTHING,
           rep.titles > 0 ? detail : NULL);
    return 1;
}
