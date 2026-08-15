# Fonts

**Status: open. Decided in Phase 3, per the roadmap.**

This file exists so the decision has somewhere to land, and so it is obvious it has
not been made yet.

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

## What has to be recorded here

- Which option was taken, and the measured size if it is option 2.
- How a missing glyph is detected at runtime.
- What the user sees when their language cannot be rendered. Silently drawing boxes
  is not it.
