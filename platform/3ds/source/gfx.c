#include "gfx.h"

#include <daemoon/util/utf8.h>

#include <3ds.h>

#include <stdio.h>
#include <string.h>

#define TEXT_BUF_GLYPHS 4096

static C3D_RenderTarget *g_top;
static C3D_RenderTarget *g_bottom;
static C2D_TextBuf       g_text_buf;
static C2D_Font          g_font;
static int               g_have_font;

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

    /* The console's own font for the selected language, and the default one when
     * it has none. The default draws Latin and nothing else. */
    g_font = C2D_FontLoadSystem(region_for(lang));
    g_have_font = (g_font != NULL);

    return g_top != NULL && g_bottom != NULL && g_text_buf != NULL;
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
    C2D_Fini();
    C3D_Fini();
    gfxExit();
}

int daemoon_gfx_has_language_font(void)
{
    return g_have_font;
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

static void build_text(C2D_Text *out, const char *text)
{
    if (g_font != NULL) {
        C2D_TextFontParse(out, g_font, g_text_buf, text);
    } else {
        C2D_TextParse(out, g_text_buf, text);
    }
    C2D_TextOptimize(out);
}

void daemoon_gfx_text(float x, float y, float scale, u32 colour, const char *text)
{
    C2D_Text t;

    if (text == NULL || text[0] == '\0') {
        return;
    }
    build_text(&t, text);
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
    build_text(&t, text);
    C2D_TextGetDimensions(&t, scale, scale, &w, &h);
    return w;
}

void daemoon_gfx_text_fit(float x, float y, float w, float scale, u32 colour,
                          const char *text)
{
    char cut[128];
    float width;

    if (text == NULL || text[0] == '\0') {
        return;
    }

    width = daemoon_gfx_text_width(scale, text);
    if (width <= w) {
        daemoon_gfx_text(x + (w - width) / 2.0f, y, scale, colour, text);
        return;
    }

    /* Cut on a code point boundary and mark it. A name chopped mid sequence draws
     * as a broken glyph, which looks like a bug in the font rather than a label
     * that did not fit. */
    (void)snprintf(cut, sizeof(cut), "%s", text);
    while (cut[0] != '\0') {
        size_t len = strlen(cut);
        size_t shorter = daemoon_utf8_truncate(cut, len, len - 1);

        cut[shorter] = '\0';
        if (shorter == 0) {
            return;
        }

        {
            char probe[132];
            (void)snprintf(probe, sizeof(probe), "%s...", cut);
            if (daemoon_gfx_text_width(scale, probe) <= w) {
                daemoon_gfx_text(x, y, scale, colour, probe);
                return;
            }
        }
    }
}

int daemoon_gfx_button(float x, float y, float w, float h, const char *label,
                       int selected, u32 keys_down, u32 touch_x, u32 touch_y,
                       int touched)
{
    int hit = 0;

    (void)keys_down;

    daemoon_gfx_rect(x, y, w, h, selected ? GFX_ACCENT : GFX_PANEL);
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
