/* The console UI.
 *
 * Text on the top screen, buttons for input. It is plain on purpose: Phase 1 is
 * about whether save data survives, and every hour spent here is an hour not spent
 * on the part that can lose someone's game.
 *
 * The whole backend takes daemoon_str_ref_t and never a sentence, so nothing here
 * can put an untranslated string on screen even by accident.
 */
#include "daemoon_3ds.h"

#include <daemoon/i18n.h>

#include <3ds.h>

#include <stdio.h>
#include <string.h>

void daemoon_3ds_ui_init(daemoon_3ds_ui_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
}

static void render(const daemoon_str_ref_t *ref)
{
    char text[512];

    (void)daemoon_strf(text, sizeof(text), ref->id, ref->args, ref->nargs);
    printf("%s\n", text);
}

/* Waits for one of a set of buttons, or for the user to close the lid on the whole
 * thing. Returns 0 when the app is exiting. */
static u32 wait_for(u32 mask)
{
    while (aptMainLoop()) {
        u32 down;

        hidScanInput();
        down = hidKeysDown();
        if (down & mask) {
            return down;
        }
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    return 0;
}

static int ui_confirm(void *ctx, const daemoon_str_ref_t *msg)
{
    u32 down;

    (void)ctx;
    printf("\n");
    render(msg);
    printf("  (A) %s   (B) %s\n", daemoon_str(DAEMOON_STR_BTN_YES),
           daemoon_str(DAEMOON_STR_BTN_NO));

    down = wait_for(KEY_A | KEY_B);
    return (down & KEY_A) ? 1 : 0;
}

static void ui_progress(void *ctx, const daemoon_str_ref_t *label, int pct)
{
    char text[512];

    (void)ctx;
    (void)daemoon_strf(text, sizeof(text), label->id, label->args, label->nargs);
    if (pct < 0) {
        printf("  %s...\n", text);
    } else {
        printf("  %s... %d%%\n", text, pct);
    }
    /* Drawn immediately: a long pack with no feedback looks like a hang, and a
     * user who thinks a save tool has hung will pull the power. */
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
}

static int ui_choose(void *ctx, const daemoon_str_ref_t *msg, const daemoon_str_ref_t *opts,
                     size_t n)
{
    size_t selected = 0;

    (void)ctx;
    if (n == 0) {
        return -1;
    }

    for (;;) {
        size_t i;
        u32 down;

        consoleClear();
        render(msg);
        printf("\n");
        for (i = 0; i < n; ++i) {
            char text[512];
            (void)daemoon_strf(text, sizeof(text), opts[i].id, opts[i].args, opts[i].nargs);
            printf("%s %s\n", i == selected ? ">" : " ", text);
        }
        printf("\n  (A) select   (B) cancel\n");

        down = wait_for(KEY_A | KEY_B | KEY_UP | KEY_DOWN);
        if (down == 0 || (down & KEY_B)) {
            return -1;
        }
        if (down & KEY_A) {
            return (int)selected;
        }
        if ((down & KEY_UP) && selected > 0) {
            --selected;
        }
        if ((down & KEY_DOWN) && selected + 1 < n) {
            ++selected;
        }
    }
}

static void ui_notify(void *ctx, const daemoon_str_ref_t *msg)
{
    (void)ctx;
    printf("\n! ");
    render(msg);
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
}

const daemoon_ui_backend_t daemoon_3ds_ui_backend = {
    ui_confirm,
    ui_progress,
    ui_choose,
    ui_notify
};
