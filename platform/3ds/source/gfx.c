#include "gfx.h"

#include <daemoon/util/utf8.h>

#include <3ds.h>

#include <stdio.h>
#include <string.h>

#define TEXT_BUF_GLYPHS    4096
#define MEASURE_BUF_GLYPHS 512

static C3D_RenderTarget *g_top;
static C3D_RenderTarget *g_bottom;
static C2D_TextBuf       g_text_buf;
/* Measuring parses text too, and measuring happens far more often than drawing:
 * fitting one label to a cell probes it once per character. Sharing the frame's
 * buffer meant those probes filled it, and a citro2d text buffer that runs out
 * does not fail politely - the console came back with a prefetch abort inside
 * printf, which is what memory corruption looks like from the outside.
 *
 * So measurements get their own buffer and clear it every time. */
static C2D_TextBuf       g_measure_buf;
static C2D_Font          g_font;
static int               g_have_font;
/* 0 built in, 1 the selected language's region, 2 the console's own region. */
static int               g_font_source;

/* Which system font can draw a given language.
 *
 * The 3DS keeps a font per region and a console only has the ones it shipped
 * with, so this asks for the right one and finds out. A European console has no
 * Hangul; that is the question docs/fonts.md is waiting to answer, and it is
 * better to know the font is missing than to draw blanks and wonder. */
static CFG_Region region_for(daemoon_lang_t lang)
{
    switch (lang) {
    case DAEMOON_LANG_JA:      return CFG_REGION_JPN;
    case DAEMOON_LANG_KO:      return CFG_REGION_KOR;
    case DAEMOON_LANG_ZH_HANS: return CFG_REGION_CHN;
    case DAEMOON_LANG_ZH_HANT: return CFG_REGION_TWN;
    default:                   return CFG_REGION_USA;
    }
}

int daemoon_gfx_init(daemoon_lang_t lang)
{
    gfxInitDefault();
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE)) {
        return 0;
    }
    if (!C2D_Init(C2D_DEFAULT_MAX_OBJECTS)) {
        C3D_Fini();
        return 0;
    }
    C2D_Prepare();

    g_top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    g_bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    g_text_buf = C2D_TextBufNew(TEXT_BUF_GLYPHS);
    g_measure_buf = C2D_TextBufNew(MEASURE_BUF_GLYPHS);

    /* The font for the selected language, then the one for the console's own
     * region, then the built in one.
     *
     * The middle step matters: a Korean console shows Korean game names on its
     * HOME menu, so it has the glyphs, and asking only for the language's region
     * font and giving up leaves those names falling back to a product code on the
     * one console that could have shown them. */
    g_font = C2D_FontLoadSystem(region_for(lang));
    g_font_source = g_font != NULL ? 1 : 0;

    if (g_font == NULL) {
        u8 console_region = 0;

        if (R_SUCCEEDED(CFGU_SecureInfoGetRegion(&console_region))) {
            g_font = C2D_FontLoadSystem((CFG_Region)console_region);
            g_font_source = g_font != NULL ? 2 : 0;
        }
    }
    /* NULL is not a failure: citro2d falls back to the built in font, which draws
     * Latin and nothing else. That is what the ASCII restriction is for. */
    g_have_font = (g_font != NULL);

    return g_top != NULL && g_bottom != NULL && g_text_buf != NULL &&
           g_measure_buf != NULL;
}

void daemoon_gfx_exit(void)
{
    if (g_font != NULL) {
        C2D_FontFree(g_font);
        g_font = NULL;
    }
    if (g_text_buf != NULL) {
        C2D_TextBufDelete(g_text_buf);
        g_text_buf = NULL;
    }
    if (g_measure_buf != NULL) {
        C2D_TextBufDelete(g_measure_buf);
        g_measure_buf = NULL;
    }
    C2D_Fini();
    C3D_Fini();
    gfxExit();
}

int daemoon_gfx_has_language_font(void)
{
    return g_have_font;
}

int daemoon_gfx_font_source(void)
{
    return g_font_source;
}

void daemoon_gfx_frame_begin(void)
{
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    /* One buffer per frame: text is rebuilt every frame anyway, and a buffer that
     * is never cleared is a leak with a slow fuse. */
    C2D_TextBufClear(g_text_buf);

    C2D_TargetClear(g_top, GFX_BG);
    C2D_TargetClear(g_bottom, GFX_BG);
}

void daemoon_gfx_frame_end(void)
{
    C3D_FrameEnd(0);
}

void daemoon_gfx_top(void)
{
    C2D_SceneBegin(g_top);
}

void daemoon_gfx_bottom(void)
{
    C2D_SceneBegin(g_bottom);
}

void daemoon_gfx_rect(float x, float y, float w, float h, u32 colour)
{
    C2D_DrawRectSolid(x, y, 0.0f, w, h, colour);
}

static void build_text(C2D_Text *out, C2D_TextBuf buf, const char *text)
{
    if (g_font != NULL) {
        C2D_TextFontParse(out, g_font, buf, text);
    } else {
        C2D_TextParse(out, buf, text);
    }
    C2D_TextOptimize(out);
}

void daemoon_gfx_text(float x, float y, float scale, u32 colour, const char *text)
{
    C2D_Text t;

    if (text == NULL || text[0] == '\0') {
        return;
    }
    build_text(&t, g_text_buf, text);
    C2D_DrawText(&t, C2D_WithColor, x, y, 0.0f, scale, scale, colour);
}

float daemoon_gfx_text_width(float scale, const char *text)
{
    C2D_Text t;
    float w = 0.0f;
    float h = 0.0f;

    if (text == NULL || text[0] == '\0') {
        return 0.0f;
    }
    /* Cleared per call, so a hundred measurements cost what one does. */
    C2D_TextBufClear(g_measure_buf);
    build_text(&t, g_measure_buf, text);
    C2D_TextGetDimensions(&t, scale, scale, &w, &h);
    return w;
}

void daemoon_gfx_text_fit(float x, float y, float w, float scale, u32 colour,
                          const char *text)
{
    char cut[160];
    float width;
    size_t len;

    if (text == NULL || text[0] == '\0') {
        return;
    }

    width = daemoon_gfx_text_width(scale, text);
    if (width <= w) {
        daemoon_gfx_text(x + (w - width) / 2.0f, y, scale, colour, text);
        return;
    }

    /* Estimate the cut from the proportion that fits, then walk it in rather than
     * measuring once per character. A game name is measured every frame, for every
     * visible cell, and the naive version was enough to exhaust the text buffer. */
    (void)snprintf(cut, sizeof(cut), "%s", text);
    len = strlen(cut);
    {
        size_t guess = (size_t)((float)len * (w / width));

        if (guess >= len) {
            guess = len - 1;
        }
        len = daemoon_utf8_truncate(cut, strlen(cut), guess);
    }

    while (len > 0) {
        char probe[168];

        cut[len] = '\0';
        (void)snprintf(probe, sizeof(probe), "%s...", cut);
        if (daemoon_gfx_text_width(scale, probe) <= w) {
            daemoon_gfx_text(x, y, scale, colour, probe);
            return;
        }
        len = daemoon_utf8_truncate(cut, len, len - 1);
    }
}

float daemoon_gfx_text_wrapped(float x, float y, float w, float scale, u32 colour,
                          const char *text)
{
    char line[256];
    const char *p = text;

    while (*p != '\0') {
        size_t take = 0;
        size_t i = 0;

        /* Word by word, measuring once per word rather than once per character.
         * The per character version parsed text hundreds of times a frame and
         * exhausted the citro2d text buffer, which shows up as a crash somewhere
         * else entirely. */
        while (p[i] != '\0' && i < sizeof(line) - 1) {
            size_t word = i;

            while (p[word] != '\0' && p[word] != ' ') {
                size_t step = daemoon_utf8_truncate(p + word, strlen(p + word), 1);
                word += (step == 0) ? 1 : step;
            }
            while (p[word] == ' ') {
                ++word;
            }
            if (word >= sizeof(line) - 1) {
                word = sizeof(line) - 2;
            }

            memcpy(line, p, word);
            line[word] = '\0';
            if (daemoon_gfx_text_width(scale, line) > w && take > 0) {
                break;
            }
            take = word;
            i = word;
            if (p[i] == '\0') {
                break;
            }
        }

        if (take == 0) {
            take = i > 0 ? i : 1;
        }

        memcpy(line, p, take);
        line[take] = '\0';
        daemoon_gfx_text(x, y, scale, colour, line);
        y += 18.0f;

        p += take;
        while (*p == ' ') {
            ++p;
        }
    }
    return y;
}

int daemoon_gfx_button(float x, float y, float w, float h, const char *label,
                       int selected, u32 keys_down, u32 touch_x, u32 touch_y,
                       int touched)
{
    int hit = 0;

    (void)keys_down;

    daemoon_gfx_rect(x, y, w, h, selected ? GFX_ACCENT : GFX_ACCENT_D);
    daemoon_gfx_rect(x + 1.0f, y + 1.0f, w - 2.0f, h - 2.0f,
                     selected ? GFX_ACCENT : GFX_PANEL);
    daemoon_gfx_text_fit(x + 4.0f, y + (h - 14.0f) / 2.0f, w - 8.0f, 0.5f,
                         GFX_TEXT, label);

    if (touched && (float)touch_x >= x && (float)touch_x < x + w &&
        (float)touch_y >= y && (float)touch_y < y + h) {
        hit = 1;
    }
    return hit;
}
