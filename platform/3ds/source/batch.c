/* One operation over every title in the library that is showing.
 *
 * The per title buttons were always the whole interface, and a console with forty
 * installed games made backing everything up forty presses of the same two buttons.
 * That is not a feature people use carefully; it is one they stop using.
 *
 * Three things this screen owes the person using it, and they are the reason it is
 * a screen rather than another button on the grid:
 *
 *   - the count, before anything runs, in the confirmation
 *   - which title is being worked on while it runs, because a frozen console and a
 *     slow one look identical
 *   - a way out that does not leave a half written save
 *
 * B stops after the title in flight rather than during it. Every write in core is
 * temp-then-rename or archive-then-commit, so stopping between titles cannot leave
 * anything half written, and stopping inside one is not something this screen is
 * able to promise.
 */
#include "daemoon_3ds.h"
#include "gfx.h"

#include <3ds.h>

#include <stdio.h>
#include <string.h>

/* Drawn between titles. The name is the one thing worth reading here: a person
 * watching this wants to know it is moving and where it has got to. */
static void draw_progress(daemoon_str_id_t op, const char *name, size_t done,
                          size_t total)
{
    char counter[64];
    char nums[2][12];
    const char *args[2];

    (void)snprintf(nums[0], sizeof(nums[0]), "%u", (unsigned)done);
    (void)snprintf(nums[1], sizeof(nums[1]), "%u", (unsigned)total);
    args[0] = nums[0];
    args[1] = nums[1];
    (void)daemoon_strf(counter, sizeof(counter), DAEMOON_STR_BATCH_RUNNING, args, 2);

    daemoon_gfx_frame_begin();
    daemoon_gfx_top();
    daemoon_gfx_rect(0.0f, 0.0f, GFX_TOP_W, 28.0f, GFX_ACCENT);
    daemoon_gfx_text(12.0f, 6.0f, 0.6f, GFX_TEXT, daemoon_str(op));
    daemoon_gfx_text(12.0f, 56.0f, 0.45f, GFX_TEXT_DIM, counter);
    (void)daemoon_gfx_text_wrapped(12.0f, 82.0f, GFX_TOP_W - 24.0f, 0.5f, GFX_TEXT,
                                   name);

    /* A bar rather than a percentage: the unit here is titles, and a title is not
     * a percentage of anything the person watching can see. */
    daemoon_gfx_rect(12.0f, GFX_SCREEN_H - 40.0f, GFX_TOP_W - 24.0f, 10.0f, GFX_PANEL);
    if (total > 0) {
        daemoon_gfx_rect(12.0f, GFX_SCREEN_H - 40.0f,
                         (GFX_TOP_W - 24.0f) * (float)done / (float)total, 10.0f,
                         GFX_ACCENT);
    }

    daemoon_gfx_bottom();
    daemoon_gfx_text(12.0f, GFX_SCREEN_H - 26.0f, 0.4f, GFX_TEXT_DIM,
                     daemoon_str(DAEMOON_STR_BATCH_HINT_RUNNING));
    daemoon_gfx_frame_end();
}

/* A list with the selected entry explained above it. The same shape as the welcome
 * connect screen, because it is the same kind of question. */
static int pick(daemoon_str_id_t title, size_t count,
                daemoon_str_id_t (*label)(size_t), daemoon_str_id_t (*hint)(size_t),
                size_t *selected)
{
    while (aptMainLoop()) {
        u32 down;
        size_t i;
        float y;

        hidScanInput();
        down = hidKeysDown();

        daemoon_gfx_frame_begin();
        daemoon_gfx_top();
        daemoon_gfx_rect(0.0f, 0.0f, GFX_TOP_W, 28.0f, GFX_ACCENT);
        daemoon_gfx_text(12.0f, 6.0f, 0.6f, GFX_TEXT, daemoon_str(title));
        (void)daemoon_gfx_text_wrapped(16.0f, 56.0f, GFX_TOP_W - 32.0f, 0.5f, GFX_TEXT,
                                       daemoon_str(hint(*selected)));

        daemoon_gfx_bottom();
        y = 40.0f;
        for (i = 0; i < count; ++i) {
            daemoon_gfx_rect(12.0f, y, GFX_BOTTOM_W - 24.0f, 34.0f,
                             i == *selected ? GFX_ACCENT : GFX_PANEL);
            daemoon_gfx_text_fit(22.0f, y + 9.0f, GFX_BOTTOM_W - 44.0f, 0.42f, GFX_TEXT,
                                 daemoon_str(label(i)));
            y += 40.0f;
        }
        daemoon_gfx_text(12.0f, GFX_SCREEN_H - 26.0f, 0.4f, GFX_TEXT_DIM,
                         daemoon_str(DAEMOON_STR_BATCH_HINT));
        daemoon_gfx_frame_end();

        if (down & KEY_B) {
            return 0;
        }
        if (down & KEY_A) {
            return 1;
        }
        if (down & KEY_DOWN) {
            *selected = (*selected + 1) % count;
        }
        if (down & KEY_UP) {
            *selected = (*selected + count - 1) % count;
        }
    }
    return 0;
}

int daemoon_3ds_batch_run(const daemoon_3ds_batch_ctx_t *ctx)
{
    size_t op = 0;
    size_t policy_row = 0;
    daemoon_conflict_policy_t policy = DAEMOON_CONFLICT_POLICY_ASK;
    daemoon_str_ref_t ask;
    char count_text[12];
    size_t counts[DAEMOON_3DS_BATCH_LIBRARIES];
    size_t total = 0;
    int    library;
    size_t done = 0;
    int stopped = 0;

    /* Both libraries, counted before anything is offered: the confirmation names a
     * number and it has to be the number of things that will actually happen.
     *
     * Reading the library that is not on screen costs what it costs - it opens every
     * save archive on the console - and it happens here rather than mid run, so the
     * count is honest and the wait is before the question rather than after it. */
    for (library = 0; library < DAEMOON_3DS_BATCH_LIBRARIES; ++library) {
        counts[library] = ctx->count(ctx->user, library);
        total += counts[library];
    }
    if (total == 0) {
        ctx->message(DAEMOON_STR_BATCH_EMPTY, GFX_WARN);
        return 0;
    }

    if (!pick(DAEMOON_STR_BATCH_TITLE, daemoon_3ds_batch_ops(),
              daemoon_3ds_batch_op_label, daemoon_3ds_batch_op_hint, &op)) {
        return 0;
    }

    if (daemoon_3ds_batch_op_label(op) == DAEMOON_STR_BATCH_SYNC) {
        if (!ctx->can_sync()) {
            ctx->message(DAEMOON_STR_ERR_NO_SERVER, GFX_WARN);
            return 0;
        }
        if (!pick(DAEMOON_STR_BATCH_POLICY, daemoon_3ds_batch_policies(),
                  daemoon_3ds_batch_policy_label, daemoon_3ds_batch_policy_hint,
                  &policy_row)) {
            return 0;
        }
        policy = daemoon_3ds_batch_policy(policy_row);
    }

    /* One confirmation, with the count and the answer in it.
     *
     * This is the confirmation the rules ask for, and it is asked where the
     * decision is: over a library, "back up 41 saves" is the thing being agreed to,
     * and asking again per title would turn a decision into a reflex. The
     * safeguards under it do not move - every restore still backs up first, every
     * digest is still checked, every write is still committed. */
    (void)snprintf(count_text, sizeof(count_text), "%u", (unsigned)total);
    memset(&ask, 0, sizeof(ask));
    if (daemoon_3ds_batch_op_label(op) == DAEMOON_STR_BATCH_SYNC) {
        if (policy == DAEMOON_CONFLICT_POLICY_KEEP_SERVER) {
            /* Its own sentence, because this is the only answer on this screen that
             * overwrites saves on the console, and the per title confirmation that
             * would otherwise say so is what this run is replacing. It names the
             * count, says what is overwritten, and says where the previous contents
             * go - which is more than forty separate dialogs would have got read. */
            ask.id = DAEMOON_STR_BATCH_CONFIRM_SYNC_SERVER;
            ask.args[0] = count_text;
            ask.nargs = 1;
        } else {
            ask.id = DAEMOON_STR_BATCH_CONFIRM_SYNC;
            ask.args[0] = count_text;
            ask.args[1] = daemoon_str(daemoon_3ds_batch_policy_label(policy_row));
            ask.nargs = 2;
        }
    } else {
        ask.id = DAEMOON_STR_BATCH_CONFIRM_BACKUP;
        ask.args[0] = count_text;
        ask.nargs = 1;
    }
    if (!ctx->confirm(&ask)) {
        return 0;
    }

    daemoon_3ds_trace("batch/begin",
                      daemoon_3ds_batch_op_label(op) == DAEMOON_STR_BATCH_SYNC
                          ? "sync" : "backup");

    for (library = 0; library < DAEMOON_3DS_BATCH_LIBRARIES && !stopped; ++library) {
        size_t i;

        for (i = 0; i < counts[library]; ++i) {
            u32 down;

            hidScanInput();
            down = hidKeysDown();
            if ((down & KEY_B) || !aptMainLoop()) {
                stopped = 1;
                break;
            }

            draw_progress(daemoon_3ds_batch_op_label(op),
                          ctx->name(ctx->user, library, i), done, total);
            if (daemoon_3ds_batch_op_label(op) == DAEMOON_STR_BATCH_SYNC) {
                ctx->sync_one(ctx->user, library, i, policy);
            } else {
                ctx->backup_one(ctx->user, library, i);
            }
            ++done;
        }
    }

    daemoon_3ds_trace("batch/done", stopped ? "stopped" : "all");
    if (stopped) {
        ctx->message(DAEMOON_STR_BATCH_STOPPED, GFX_WARN);
    }
    return 1;
}
