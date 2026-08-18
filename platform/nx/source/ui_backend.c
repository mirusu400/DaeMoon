/* The console UI, for the Switch MVP.
 *
 * A text console rather than deko3d, on purpose. The 3DS build spent months getting
 * to icons and a grid, and none of that is what Phase 6 is about: what Phase 6 has to
 * establish is that a save can be mounted, read, written and committed on this
 * platform, and a text list proves that as well as a textured one would.
 *
 * The important half is that the interface is the same one core already talks to, so
 * the dialogs it puts up are the dialogs the rules require - a confirmation before
 * anything destructive, and a choice rather than a merge on a conflict.
 */
#include "daemoon_nx.h"

#include <daemoon/i18n.h>

#include <switch.h>

#include <stdio.h>
#include <string.h>

void daemoon_nx_ui_init(daemoon_nx_ui_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
}

static void render(const daemoon_str_ref_t *ref, char *out, size_t cap)
{
    (void)daemoon_strf(out, cap, ref->id, ref->args, ref->nargs);
}

/* A blocking question on a console with no pointer. A and B, and the cursor starts on
 * "no" - the question is only ever asked before something that cannot be undone, and
 * a cursor resting on yes turns a reflex into a decision that was never made. */
static int ask(const char *body, const char *const *options, size_t n, size_t start)
{
    PadState pad;
    size_t selected = start;

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    while (appletMainLoop()) {
        u64 down;
        size_t i;

        consoleClear();
        printf("\n%s\n\n", body);
        for (i = 0; i < n; ++i) {
            printf("  %s %s\n", i == selected ? ">" : " ", options[i]);
        }
        printf("\n  A select   up/down move\n");
        consoleUpdate(NULL);

        padUpdate(&pad);
        down = padGetButtonsDown(&pad);
        if (down & HidNpadButton_A) {
            return (int)selected;
        }
        if (down & HidNpadButton_B) {
            return -1;
        }
        if ((down & HidNpadButton_Down) && selected + 1 < n) {
            ++selected;
        }
        if ((down & HidNpadButton_Up) && selected > 0) {
            --selected;
        }
    }
    return -1;
}

static int ui_confirm(void *ctx, const daemoon_str_ref_t *msg)
{
    char body[512];
    const char *options[2];

    (void)ctx;
    render(msg, body, sizeof(body));
    options[0] = daemoon_str(DAEMOON_STR_BTN_NO);
    options[1] = daemoon_str(DAEMOON_STR_BTN_YES);

    return ask(body, options, 2, 0) == 1;
}

static void ui_progress(void *ctx, const daemoon_str_ref_t *label, int pct)
{
    char body[256];

    (void)ctx;
    render(label, body, sizeof(body));
    if (pct >= 0) {
        printf("%s %d%%\n", body, pct);
    } else {
        printf("%s\n", body);
    }
    consoleUpdate(NULL);
}

static int ui_choose(void *ctx, const daemoon_str_ref_t *msg, const daemoon_str_ref_t *opts,
                     size_t n)
{
    char body[512];
    char rendered[4][256];
    const char *options[4];
    size_t i;

    (void)ctx;
    if (n > 4) {
        n = 4;
    }
    render(msg, body, sizeof(body));
    for (i = 0; i < n; ++i) {
        render(&opts[i], rendered[i], sizeof(rendered[i]));
        options[i] = rendered[i];
    }
    /* No default. A conflict is two saves and the answer is not this application's to
     * lean toward, so the cursor sits on the first option and B leaves both alone. */
    return ask(body, options, n, 0);
}

static void ui_notify(void *ctx, const daemoon_str_ref_t *msg)
{
    char body[512];

    (void)ctx;
    render(msg, body, sizeof(body));
    printf("%s\n", body);
    consoleUpdate(NULL);
}

const daemoon_ui_backend_t daemoon_nx_ui_backend = {
    ui_confirm,
    ui_progress,
    ui_choose,
    ui_notify
};
