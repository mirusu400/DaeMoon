/* Drawing, for the 3DS build.
 *
 * The text console this replaced could only draw 8x8 ASCII, so a Korean or
 * Japanese title rendered as a blank line - read correctly and impossible to
 * show. citro2d with the console's own font fixes that, and gets icons, which
 * are how a person actually recognises a game.
 *
 * Everything here is presentation. Nothing in this file or the ones that use it
 * decides anything about save data: the rules still live in core, and the UI
 * backend still receives string ids rather than sentences.
 */
#ifndef DAEMOON_3DS_GFX_H
#define DAEMOON_3DS_GFX_H

#include <daemoon/i18n.h>

#include <citro2d.h>

/* Screen sizes, named so the layout reads as intent rather than arithmetic. */
#define GFX_TOP_W    400.0f
#define GFX_BOTTOM_W 320.0f
#define GFX_SCREEN_H 240.0f

/* One palette, so a colour is chosen once rather than at each call site. */
#define GFX_BG        C2D_Color32(0x1a, 0x1d, 0x24, 0xff)
#define GFX_PANEL     C2D_Color32(0x24, 0x28, 0x32, 0xff)
#define GFX_ACCENT    C2D_Color32(0x2f, 0x6f, 0xd0, 0xff)
#define GFX_ACCENT_D  C2D_Color32(0x1f, 0x2a, 0x44, 0xff)
#define GFX_TEXT      C2D_Color32(0xf0, 0xf2, 0xf5, 0xff)
#define GFX_TEXT_DIM  C2D_Color32(0x98, 0xa0, 0xb0, 0xff)
#define GFX_WARN      C2D_Color32(0xe0, 0xa0, 0x30, 0xff)
#define GFX_DANGER    C2D_Color32(0xd0, 0x50, 0x50, 0xff)
#define GFX_OK        C2D_Color32(0x50, 0xc0, 0x70, 0xff)

/* The font that can draw the selected language, when the console has one. A
 * European console has no Hangul at all, which is the open question in
 * docs/fonts.md; when the font is missing this falls back and the text is blank,
 * exactly as it was before, and that is worth knowing rather than papering over. */
int  daemoon_gfx_init(daemoon_lang_t lang);
void daemoon_gfx_exit(void);

/* Whether a font covering the selected language was actually found.
 *
 * Narrower than it sounds, and it was mistaken for the broader question once
 * already: this is about an *extra* region font, and the console's own system
 * font draws its own region's script whether or not one was loaded. For "can this
 * be shown to the user", ask daemoon_gfx_can_draw. */
int daemoon_gfx_has_language_font(void);

/* Whether the font being drawn with has a glyph for this codepoint. A font
 * reports a missing one by handing back its replacement character. */
int daemoon_gfx_can_draw(unsigned int codepoint);

/* Whether this console could draw a language at all, decided once at startup by
 * loading each candidate font.
 *
 * Not the same as asking the font currently loaded, which is a question about
 * what is on screen now. A console that had switched to Japanese reported that it
 * had no Korean font, because it was holding the Japanese one and those have no
 * Hangul. */
int daemoon_gfx_language_drawable(daemoon_lang_t lang);

/* Loads whichever font can draw this language and keeps it. Returns 0 when none
 * can, in which case the caller must not select the language: the screen that
 * would change it back would be blank too.
 *
 * The console's own font is preferred whenever it will do, because it is also the
 * font game names are written in. */
int daemoon_gfx_set_language(daemoon_lang_t lang);

/* Which one: 0 the built in font, 1 the selected language's region, 2 the
 * console's own region. Written into the survey, because "the names are product
 * codes" has three different causes and this rules one of them in or out. */
int daemoon_gfx_font_source(void);

void daemoon_gfx_frame_begin(void);
void daemoon_gfx_frame_end(void);
void daemoon_gfx_top(void);    /* subsequent drawing goes to the top screen */
void daemoon_gfx_bottom(void);

void  daemoon_gfx_rect(float x, float y, float w, float h, u32 colour);
void  daemoon_gfx_text(float x, float y, float scale, u32 colour, const char *text);
/* Centred within a width, and cut with an ellipsis when it does not fit: a game
 * name is often longer than the space a grid cell has. */
void  daemoon_gfx_text_fit(float x, float y, float w, float scale, u32 colour,
                           const char *text);
float daemoon_gfx_text_width(float scale, const char *text);

/* Draws across several lines, breaking on words. A translated sentence runs half
 * again as long as the English, so anything that assumes one line loses its
 * second half in German. Returns the y below the last line. */
float daemoon_gfx_text_wrapped(float x, float y, float w, float scale, u32 colour,
                               const char *text);

/* A rectangle the user can press. Returns 1 when it was, by touch or by the
 * button it is labelled with being pressed. */
int daemoon_gfx_button(float x, float y, float w, float h, const char *label,
                       int selected, u32 keys_down, u32 touch_x, u32 touch_y,
                       int touched);

#endif /* DAEMOON_3DS_GFX_H */
