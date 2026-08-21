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

## What one Korean console actually draws

From its own survey header:

```
font=0  drawable=en,ko,ja,zh-Hans,zh-Hant,es,fr,de  ascii_names=0
```

`font=0` means no extra region font was loaded and none was needed: **the system
font this console shipped with draws all eight scripts this project ships** -
Hangul, kana and kanji, both Chinese, and Latin with accents.

So on this hardware the fallback never engages, and the decision above costs
nothing. It is one console, and a European one is still expected to answer
differently, which is exactly why the probe exists rather than an assumption.

### The trap that is left

A console's own font is also the font its game names are written in. Loading
another region's font to show a menu in a language the console's own font cannot
draw would replace the script the library is written in - Korean game names
becoming replacement characters to show a Japanese menu.

`daemoon_gfx_set_language` therefore prefers the console's own font whenever it
will do, and loads another region's only when it will not. On the console above
that branch is never taken.

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

## Switch: answered, and the wrong answer shipped first

**Now: the console's own shared fonts, all of them, falling back per glyph.**
borealis's `switch_font.cpp` asks `plGetSharedFontByType` for `Standard`,
`ChineseSimplified`, `ExtChineseSimplified`, `ChineseTraditional`, `KO` and
`NintendoExt`, and adds every one it gets to a single font stash, so nanovg picks
whichever has the glyph. Nothing in this project loads a font on this platform and
nothing bundles one - the same decision as the 3DS, reached without any code of ours.

The interesting part is what was there before it. The Switch build drew on libnx's
text console, which has a font of its own that covers ASCII and stops. A Korean
console showed rubbish, so the build did the only thing that screen allowed: it
walked the whole string table looking for a byte above `0x7f` and fell back to
English when it found one, writing `font/fallback` to `trace.txt` on the way past.

That was an honest fallback and a bad outcome. Every screen in English on a Korean
console is not a language that could not be drawn; it is a screen that could not draw
a language. The 3DS section above records the same shape of mistake from the other
side - a NULL from a loader read as "no Hangul available" - and both come down to the
same thing: **the question is what the thing drawing can draw, and the answer has to
come from asking it rather than from what surrounds it.**

The probe is gone with the console it existed for. There is nothing left for it to
catch: this screen draws the console's own font, which is also the font the console
writes its game names in, so a name and a menu are never in different scripts.

## What has to be recorded here

- ~~Which option was taken, and the measured size if it is option 2.~~ Option 1,
  nothing bundled.
- ~~How a missing glyph is detected at runtime.~~ `daemoon_gfx_can_draw`.
- ~~What the user sees when their language cannot be rendered.~~ English, and a
  line in `trace.txt` saying so. 3DS only; on the Switch the case no longer arises.
