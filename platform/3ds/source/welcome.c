/* The first run: what this is, and how to connect it.
 *
 * A console that has just been installed has a server address it does not know, a
 * token it does not have, and a grid of games with two buttons that write to save
 * archives. The screens before this existed assumed somebody had already been told
 * what the application does, and there was nowhere they could have been told.
 *
 * Two questions, in this order:
 *
 *   1. What it does, what it will not do to a save, and when it is safe to use.
 *   2. How to reach the server: scan a code, type one, or neither for now.
 *
 * There is deliberately no "official server or your own" step. The pairing code
 * carries the server address inside it - `DAEMOON|1|<server>|<code>` - so scanning
 * answers both at once, and a step asking for an address that the next step is
 * about to overwrite is a step that teaches the wrong thing about how this works.
 *
 * Everything drawn here is a string id. The pages themselves are in
 * welcome_steps.c, which has no citro2d in it and is checked on a desktop.
 */
#include "daemoon_3ds.h"
#include "gfx.h"

#include <3ds.h>

/* One page of prose, with the pager on the bottom screen.
 *
 * Returns 1 to go on, 0 when the user asked to leave. B goes back a page rather
 * than out: the way out is START, and it is on the hint line, because a B that
 * quietly abandons an explanation is how somebody ends up at the grid never having
 * read the one page about running games.
 */
static int welcome_intro(void)
{
    size_t page = 0;
    size_t pages = daemoon_3ds_welcome_pages();

    while (aptMainLoop()) {
        u32 down;
        size_t i;
        float x;

        hidScanInput();
        down = hidKeysDown();

        daemoon_gfx_frame_begin();
        daemoon_gfx_top();
        daemoon_gfx_rect(0.0f, 0.0f, GFX_TOP_W, 28.0f, GFX_ACCENT);
        daemoon_gfx_text(12.0f, 6.0f, 0.6f, GFX_TEXT,
                         daemoon_str(DAEMOON_STR_WELCOME_TITLE));
        (void)daemoon_gfx_text_wrapped(16.0f, 60.0f, GFX_TOP_W - 32.0f, 0.5f, GFX_TEXT,
                                       daemoon_str(daemoon_3ds_welcome_page(page)));

        daemoon_gfx_bottom();
        /* One dot per page, filled up to the current one. A page count in words
         * would be a sentence to translate for something a person reads at a
         * glance. */
        x = (GFX_BOTTOM_W - (float)pages * 18.0f) / 2.0f;
        for (i = 0; i < pages; ++i) {
            daemoon_gfx_rect(x + (float)i * 18.0f, 110.0f, 10.0f, 10.0f,
                             i == page ? GFX_TEXT : GFX_PANEL);
        }
        daemoon_gfx_text(12.0f, GFX_SCREEN_H - 26.0f, 0.42f, GFX_TEXT_DIM,
                         daemoon_str(DAEMOON_STR_WELCOME_HINT_INTRO));
        daemoon_gfx_frame_end();

        if (down & KEY_START) {
            return 0;
        }
        if (down & KEY_A) {
            if (page + 1 >= pages) {
                return 1;
            }
            ++page;
        }
        if ((down & KEY_B) && page > 0) {
            --page;
        }
    }
    return 0;
}

/* The three ways to connect, with the selected one spelled out above.
 *
 * Returns the choice, or the last entry when the user left: "not now" is what
 * leaving means, and treating it as anything else would put a console that pressed
 * START into a pairing screen.
 */
static size_t welcome_choose(void)
{
    size_t selected = 0;
    size_t count = daemoon_3ds_welcome_choices();

    while (aptMainLoop()) {
        u32 down;
        size_t i;
        float y;

        hidScanInput();
        down = hidKeysDown();

        daemoon_gfx_frame_begin();
        daemoon_gfx_top();
        daemoon_gfx_rect(0.0f, 0.0f, GFX_TOP_W, 28.0f, GFX_ACCENT);
        daemoon_gfx_text(12.0f, 6.0f, 0.6f, GFX_TEXT,
                         daemoon_str(DAEMOON_STR_WELCOME_CONNECT));
        y = daemoon_gfx_text_wrapped(16.0f, 44.0f, GFX_TOP_W - 32.0f, 0.44f,
                                     GFX_TEXT_DIM,
                                     daemoon_str(DAEMOON_STR_WELCOME_CONNECT_HOW));
        /* Only the selected option is explained. Three hints at once is a wall,
         * and the one under the cursor is the one being considered. */
        (void)daemoon_gfx_text_wrapped(16.0f, y + 14.0f, GFX_TOP_W - 32.0f, 0.5f,
                                       GFX_TEXT,
                                       daemoon_str(daemoon_3ds_welcome_choice_hint(selected)));

        daemoon_gfx_bottom();
        y = 40.0f;
        for (i = 0; i < count; ++i) {
            daemoon_gfx_rect(12.0f, y, GFX_BOTTOM_W - 24.0f, 34.0f,
                             i == selected ? GFX_ACCENT : GFX_PANEL);
            daemoon_gfx_text_fit(22.0f, y + 9.0f, GFX_BOTTOM_W - 44.0f, 0.44f, GFX_TEXT,
                                 daemoon_str(daemoon_3ds_welcome_choice_label(i)));
            y += 40.0f;
        }
        daemoon_gfx_text(12.0f, GFX_SCREEN_H - 26.0f, 0.4f, GFX_TEXT_DIM,
                         daemoon_str(DAEMOON_STR_WELCOME_HINT_CHOOSE));
        daemoon_gfx_frame_end();

        if (down & KEY_START) {
            return count - 1;
        }
        if (down & KEY_A) {
            return selected;
        }
        if (down & KEY_DOWN) {
            selected = (selected + 1) % count;
        }
        if (down & KEY_UP) {
            selected = (selected + count - 1) % count;
        }
    }
    return count - 1;
}

int daemoon_3ds_welcome_run(const daemoon_3ds_welcome_actions_t *acts)
{
    int paired = 0;

    daemoon_3ds_trace("welcome/begin", NULL);

    if (!welcome_intro()) {
        /* Skipped rather than read. Still counts as having happened - the flag is
         * about whether these screens have had their turn, and coming back every
         * launch is how an application argues with somebody. */
        daemoon_3ds_trace("welcome/skipped", NULL);
        return 0;
    }

    for (;;) {
        size_t choice = welcome_choose();

        if (choice == DAEMOON_3DS_WELCOME_LATER) {
            daemoon_3ds_trace("welcome/later", NULL);
            break;
        }
        if (choice == DAEMOON_3DS_WELCOME_QR && acts->pair_qr != NULL) {
            paired = acts->pair_qr();
        } else if (choice == DAEMOON_3DS_WELCOME_MANUAL && acts->pair_manual != NULL) {
            paired = acts->pair_manual();
        }
        if (paired) {
            break;
        }
        /* Back to the three options rather than out. A camera that read nothing and
         * a code that was typed wrong are both recoverable by choosing the other
         * way, and dropping to the grid would make the fix a hunt through Settings
         * on the one launch where the person has never seen Settings. */
        daemoon_3ds_trace("welcome/retry", NULL);
    }

    daemoon_3ds_trace("welcome/done", paired ? "paired" : "later");
    return paired;
}
