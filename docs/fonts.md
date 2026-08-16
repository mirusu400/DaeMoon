# Fonts

**Status: decided in Phase 3. Option 1, detected at runtime.**

## The decision

DaeMoon bundles no font. It draws with the console's own system font, and when
that font cannot draw the selected language the **UI language falls back to
English**, detected at startup rather than assumed.

```c
int daemoon_gfx_can_draw(unsigned int codepoint);   /* platform/3ds/source/gfx.c */
```

`C2D_FontGlyphIndexFromCodePoint` against `C2D_FontGetInfo(font)->alterCharIndex`:
a font reports a missing glyph by handing back its replacement character, so that
is what "missing" is compared against. citro2d treats a NULL font as the system
font and this call follows it, so the answer is about whichever font is actually
being drawn with.

`main.c` probes one representative character per language - 가, あ, 中, ß, é, ñ -
and calls `daemoon_i18n_set_language(DAEMOON_LANG_EN)` if it is missing. The
fallback is written to `trace.txt` as `font/fallback`, because a user who suddenly
reads English should be a fact in a file rather than a surprise.

Game names are separate and are never restricted: they are drawn as the title
carries them. A glyph the console lacks draws as nothing, and the survey records
the real name either way, so it stays a fact rather than a guess.

### Why not a bundled subset

It was the expected answer and it is not needed. The case it was for - a console
that cannot draw the selected language - is a console whose owner chose a language
their hardware has no font for, and shipping a few hundred kilobytes into every
build to serve it is a poor trade against falling back to English. If somebody
turns up who needs it, the detection above is the hook it would attach to.

## What one Korean console actually showed, and what it corrected

The earlier version of this file recorded that the built in font could draw
nothing but Latin and that thirteen of sixteen titles were therefore
unreadable. **That was wrong, and it cost real time.**

`C2D_FontLoadSystem` did return NULL for both the Korean region and the console's
own region. The conclusion drawn from that - no Hangul available - did not follow.
citro2d falls back to the *system* font, which is the console's own region font,
and that is the same font the HOME menu draws those exact names with. Hangul
rendered perfectly the whole time. It was the `ascii_names` restriction, switched
on by a NULL from `C2D_FontLoadSystem`, that was replacing thirteen names with
product codes.

Restriction removed, names now show in Korean, confirmed on hardware.

**A NULL from a loader is not an answer about what can be drawn.** Ask the font.

## Switch: mostly answered

`plInitialize()` and `plGetSharedFontByType()`. The shared fonts are split by type
(`Standard`, `ChineseSimplified`, `ChineseTraditional`, `KO`, `NintendoExtended`),
so rendering CJK means loading the matching type and falling back per glyph. The
fallback chain has to be explicit; a missing glyph rendering as a box in the middle
of a confirmation the user has to answer is not acceptable.

Phase 6 inherits the rule above: probe the font, do not infer from a loader.

## What has to be recorded here

- ~~Which option was taken, and the measured size if it is option 2.~~ Option 1,
  nothing bundled.
- ~~How a missing glyph is detected at runtime.~~ `daemoon_gfx_can_draw`.
- ~~What the user sees when their language cannot be rendered.~~ English, and a
  line in `trace.txt` saying so.
