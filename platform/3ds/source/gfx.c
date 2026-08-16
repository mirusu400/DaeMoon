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
/* 0 the console's own system font, 1 another region's, loaded on purpose. */
static int               g_font_source;
/* Which languages this console can draw, worked out once. See probe_languages. */
static unsigned          g_drawable;

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

/* One representative character per language.
 *
 * Here rather than in main.c because the answer depends on which font is loaded,
 * and this file is the only one that knows that. */
static unsigned int probe_for(daemoon_lang_t lang)
{
    switch (lang) {
    case DAEMOON_LANG_KO:      return 0xAC00u; /* 가 */
    case DAEMOON_LANG_JA:      return 0x3042u; /* あ */
    case DAEMOON_LANG_ZH_HANS: return 0x4E2Du; /* 中 */
    case DAEMOON_LANG_ZH_HANT: return 0x4E2Du;
    case DAEMOON_LANG_DE:      return 0x00DFu; /* ß */
    case DAEMOON_LANG_FR:      return 0x00E9u; /* é */
    case DAEMOON_LANG_ES:      return 0x00F1u; /* ñ */
    default:                   return 0u;      /* English needs nothing extra */
    }
}

static int font_has(C2D_Font font, unsigned int codepoint)
{
    FINF_s *info = C2D_FontGetInfo(font);

    if (codepoint == 0u) {
        return 1;
    }
    if (info == NULL) {
        return 0;
    }
    return C2D_FontGlyphIndexFromCodePoint(font, codepoint) != (int)info->alterCharIndex;
}

/* Which languages this console can draw at all, decided once at startup.
 *
 * It has to be decided by loading each candidate font, because the answer is not
 * a property of the console - it is a property of the font that would be used.
 * Asking the currently loaded font is what made a Korean console report that it
 * had no Korean font: it had loaded the Japanese one when the user switched, and
 * Japanese fonts have no Hangul.
 *
 * Done once and cached, because it loads and frees up to four fonts and the
 * language list redraws every frame. */
static void probe_languages(void)
{
    static const daemoon_lang_t k_all[] = {
        DAEMOON_LANG_EN, DAEMOON_LANG_KO, DAEMOON_LANG_JA, DAEMOON_LANG_ZH_HANS,
        DAEMOON_LANG_ZH_HANT, DAEMOON_LANG_ES, DAEMOON_LANG_FR, DAEMOON_LANG_DE
    };
    size_t i;

    g_drawable = 0;
    for (i = 0; i < sizeof(k_all) / sizeof(k_all[0]); ++i) {
        daemoon_lang_t lang = k_all[i];
        unsigned int cp = probe_for(lang);
        int ok;

        /* The console's own font first. When it can draw the language, nothing
         * else should be loaded: another region's font would replace the script
         * this console's game names are written in. */
        ok = font_has(NULL, cp);
        if (!ok) {
            C2D_Font font = C2D_FontLoadSystem(region_for(lang));

            if (font != NULL) {
                ok = font_has(font, cp);
                C2D_FontFree(font);
            }
        }
        if (ok) {
            g_drawable |= 1u << (unsigned)lang;
        }
    }
}

int daemoon_gfx_language_drawable(daemoon_lang_t lang)
{
    return (g_drawable & (1u << (unsigned)lang)) != 0;
}

/* Points the renderer at whichever font can draw this language.
 *
 * The console's own system font is preferred whenever it will do, because it is
 * also the font game names are written in: loading another region's font to show
 * a Japanese menu on a Korean console turns every Korean game name into
 * replacement characters, which is exactly what happened when the font was chosen
 * once at startup and never revisited.
 */
int daemoon_gfx_set_language(daemoon_lang_t lang)
{
    unsigned int cp = probe_for(lang);
    C2D_Font font = NULL;
    int source = 0;

    if (!font_has(NULL, cp)) {
        font = C2D_FontLoadSystem(region_for(lang));
        if (font != NULL && !font_has(font, cp)) {
            C2D_FontFree(font);
            font = NULL;
        }
        source = font != NULL ? 1 : 0;
    }

    /* Freed only after the replacement is in hand, and between frames - a text
     * buffer is parsed and drawn inside one frame and cleared at the start of the
     * next, so nothing outlives this. */
    if (g_font != NULL) {
        C2D_FontFree(g_font);
    }
    g_font = font;
    g_font_source = source;
    g_have_font = font != NULL;
    return font_has(g_font, cp);
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

        if (g_top == NULL || g_bottom == NULL || g_text_buf == NULL ||
        g_measure_buf == NULL) {
        return 0;
    }

    probe_languages();
    daemoon_gfx_set_language(lang);
    return 1;
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

/* Whether the font actually in use has a glyph for this codepoint.
 *
 * citro2d treats a NULL font as the console's own system font, and
 * C2D_FontGlyphIndexFromCodePoint follows that - so this answers for whichever
 * font is being drawn with, which is the only question worth asking. A font
 * reports a missing glyph by handing back its replacement character, so that is
 * what "missing" is compared against.
 *
 * This is what docs/fonts.md was waiting for. Until it existed the code guessed
 * from whether C2D_FontLoadSystem had returned something, which answers a
 * different question - whether an *extra* region font was loaded - and got it
 * wrong on the one console anybody had tested on. */
int daemoon_gfx_can_draw(unsigned int codepoint)
{
    return font_has(g_font, codepoint);
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
