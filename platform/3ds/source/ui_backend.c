/* The UI backend, drawn with citro2d.
 *
 * core hands this string ids and their arguments, never sentences, so nothing
 * here can put an untranslated string on screen even by accident. What changed
 * from the text console it replaced is only how the text gets drawn - with the
 * console's own font, which can render the languages this project ships.
 *
 * Every destructive action in the project comes through confirm(). It is
 * deliberately plain: no timers, no default-yes, and the dangerous option is not
 * the one the cursor starts on.
 */
#include "daemoon_3ds.h"
#include "gfx.h"

#include <daemoon/i18n.h>

#include <daemoon/util/utf8.h>

#include <3ds.h>

#include <stdio.h>
#include <string.h>

void daemoon_3ds_ui_init(daemoon_3ds_ui_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
}

/* Wraps a rendered string across a width, drawing it line by line. A translated
 * sentence is often half again as long as the English, so a dialog that assumes
 * one line is a dialog that loses its second half in German. */
static float draw_wrapped(float x, float y, float w, float scale, u32 colour,
                          const char *text)
{
    char line[256];
    const char *p = text;

    while (*p != '\0') {
        size_t take = 0;
        size_t last_break = 0;
        size_t i = 0;

        /* Grow the line until it no longer fits, remembering the last place a
         * break would look deliberate. */
        while (p[i] != '\0' && i < sizeof(line) - 1) {
            size_t step = daemoon_utf8_truncate(p + i, strlen(p + i), 1);

            if (step == 0) {
                step = 1;
            }
            memcpy(line + i, p + i, step);
            line[i + step] = '\0';
            if (daemoon_gfx_text_width(scale, line) > w) {
                break;
            }
            i += step;
            take = i;
            if (p[i] == ' ') {
                last_break = i;
            }
        }

        if (p[i] == '\0') {
            take = i;
        } else if (last_break > 0) {
            take = last_break;
        }
        if (take == 0) {
            take = 1;
        }

        memcpy(line, p, take);
        line[take] = '\0';
        daemoon_gfx_text(x, y, scale, colour, line);
        y += 16.0f * scale / 0.5f * 0.5f + 4.0f;

        p += take;
        while (*p == ' ') {
            ++p;
        }
    }
    return y;
}

static void render(const daemoon_str_ref_t *ref, char *out, size_t cap)
{
    (void)daemoon_strf(out, cap, ref->id, ref->args, ref->nargs);
}

/* One modal, driven until the user answers. Returns the chosen index, or -1. */
static int modal(const char *title, const char *body, const char *const *options,
                 size_t option_count, int start, u32 accent)
{
    int selected = start;

    while (aptMainLoop()) {
        u32 down;
        touchPosition touch;
        size_t i;
        float y;

        hidScanInput();
        down = hidKeysDown();
        hidTouchRead(&touch);

        daemoon_gfx_frame_begin();

        daemoon_gfx_top();
        daemoon_gfx_rect(0.0f, 0.0f, GFX_TOP_W, 28.0f, accent);
        daemoon_gfx_text(12.0f, 6.0f, 0.6f, GFX_TEXT, title);
        (void)draw_wrapped(12.0f, 48.0f, GFX_TOP_W - 24.0f, 0.5f, GFX_TEXT, body);

        daemoon_gfx_bottom();
        y = 24.0f;
        for (i = 0; i < option_count; ++i) {
            int hit = daemoon_gfx_button(16.0f, y, GFX_BOTTOM_W - 32.0f, 34.0f,
                                         options[i], (int)i == selected,
                                         down, touch.px, touch.py,
                                         (down & KEY_TOUCH) != 0);
            if (hit) {
                daemoon_gfx_frame_end();
                return (int)i;
            }
            y += 40.0f;
        }
        daemoon_gfx_text(16.0f, GFX_SCREEN_H - 24.0f, 0.4f, GFX_TEXT_DIM,
                         "A select   B back   up/down move");

        daemoon_gfx_frame_end();

        if (down & KEY_A) {
            return selected;
        }
        if (down & KEY_B) {
            return -1;
        }
        if ((down & KEY_UP) && selected > 0) {
            --selected;
        }
        if ((down & KEY_DOWN) && (size_t)selected + 1 < option_count) {
            ++selected;
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

    /* Starting on "no". The question is only ever asked before something that
     * cannot be undone, and a cursor resting on yes turns a reflex into a
     * decision. */
    return modal(daemoon_str(DAEMOON_STR_APP_TITLE), body, options, 2, 0,
                 GFX_WARN) == 1;
}

static void ui_progress(void *ctx, const daemoon_str_ref_t *label, int pct)
{
    char text[256];

    (void)ctx;
    render(label, text, sizeof(text));

    daemoon_gfx_frame_begin();
    daemoon_gfx_top();
    daemoon_gfx_rect(0.0f, 0.0f, GFX_TOP_W, 28.0f, GFX_ACCENT);
    daemoon_gfx_text(12.0f, 6.0f, 0.6f, GFX_TEXT, daemoon_str(DAEMOON_STR_APP_TITLE));
    daemoon_gfx_text(12.0f, 100.0f, 0.55f, GFX_TEXT, text);

    if (pct >= 0) {
        float w = (GFX_TOP_W - 24.0f) * (float)pct / 100.0f;
        daemoon_gfx_rect(12.0f, 130.0f, GFX_TOP_W - 24.0f, 8.0f, GFX_PANEL);
        daemoon_gfx_rect(12.0f, 130.0f, w, 8.0f, GFX_ACCENT);
    }
    daemoon_gfx_frame_end();
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

    return modal(daemoon_str(DAEMOON_STR_CONFLICT_TITLE), body, options, n, 0,
                 GFX_WARN);
}

static void ui_notify(void *ctx, const daemoon_str_ref_t *msg)
{
    char body[512];
    const char *options[1];

    (void)ctx;
    render(msg, body, sizeof(body));
    options[0] = daemoon_str(DAEMOON_STR_BTN_OK);
    (void)modal(daemoon_str(DAEMOON_STR_APP_TITLE), body, options, 1, 0, GFX_DANGER);
}

const daemoon_ui_backend_t daemoon_3ds_ui_backend = {
    ui_confirm,
    ui_progress,
    ui_choose,
    ui_notify
};
