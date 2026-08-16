# Fonts

**Status: open. Decided in Phase 3, per the roadmap.**

This file exists so the decision has somewhere to land, and so it is obvious it has
not been made yet.

## What one Korean console actually showed

From `sdmc:/DaeMoon/survey.txt`, sixteen titles:

| | |
|---|---|
| names read from the SMDH | 16 of 16 |
| carrying only a Korean name | **13** |
| carrying an English name | 3 |
| icons loaded | 16 of 16 |
| font loaded | none - the built in one |

So this is not a rare case to plan for later. On a Korean console, most of the
library is Korean-only, and the built in font cannot draw any of it.
`C2D_FontLoadSystem` was asked for the Korean region font and then for the
console's own region font, and both came back empty on a console whose HOME menu
displays those names perfectly well.

The icons are what keeps the application usable in the meantime: a person
recognises a game from its icon in under a second, and thirteen of sixteen rows
currently say `CTR-P-EKJA` next to a picture that says Pokemon.

That is the state, and it is worse than the section below assumed: the workaround
fails on the majority of a real library rather than at the edges.

## The question

Can DaeMoon render Korean, Japanese and both Chinese scripts on a 3DS without
bundling a font?

## Switch: mostly answered

`plInitialize()` and `plGetSharedFontByType()`. The shared fonts are split by type
(`Standard`, `ChineseSimplified`, `ChineseTraditional`, `KO`, `NintendoExtended`),
so rendering CJK means loading the matching type and falling back per glyph. The
fallback chain has to be explicit; a missing glyph rendering as a box in the middle
of a confirmation the user has to answer is not acceptable.

## 3DS: open

The system font varies by console region. A European console may have no Hangul and
no kanji at all, and the selected language is a user choice that has nothing to do
with the console's region.

Options, none free:

1. **Fall back to English** when a glyph is missing. Cheap, and it means a Korean
   user on a European console reads English.
2. **Bundle a subset font** covering the strings this app actually ships. The string
   set is small and known at build time, so a subset is plausible.
3. **Bundle a full CJK font.** Ruled out: the size is unacceptable for a homebrew
   application.

Option 2 is the likely answer. It needs measuring: build the subset from
`shared/lang/*.json`, see what it costs.

## What Phase 1 ran into, and what it does about it

The 3DS build draws its lists with `consoleInit`, which is an 8x8 bitmap font with
no CJK in it. A Korean or Japanese game title is read from the SMDH correctly and
then renders as a blank line - and from the other side of the screen, a name that
cannot be drawn is indistinguishable from a name that was never read. That is
exactly how it was reported.

The interim answer, which is not the decision this file is waiting for:

- the **list** asks for a name the renderer can draw, falling back through the
  console's language, English, Japanese, anything, and finally the product code
- the **survey file** records the real name, in whatever script it is, because
  that file is read on a machine with fonts

So the console shows "Yo-kai Watch" where the SMDH also says "요괴워치", and a
title that has only a name it cannot draw shows its product code rather than an
empty line.

This is a workaround for the missing font, not a substitute for one. It is worth
noticing that it fails exactly where it matters most: a Korean user with Korean
games sees English or a product code. Deciding this properly is the point of the
section above.

## What the 3DS build does now

citro2d with `C2D_FontLoadSystem` for the region matching the selected language.
On a console that has that font - a Korean console asked for the Korean one - the
names render properly and the ASCII fallback never engages.

The fallback is still there and still applies when `C2D_FontLoadSystem` comes back
empty, which is the case this file exists for: a European console has no Hangul
to load. Nothing here has answered that, and a user in that position still sees
English or a product code.

## What has to be recorded here

- Which option was taken, and the measured size if it is option 2.
- How a missing glyph is detected at runtime.
- What the user sees when their language cannot be rendered. Silently drawing boxes
  is not it.
