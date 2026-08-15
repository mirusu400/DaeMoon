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

## What has to be recorded here

- Which option was taken, and the measured size if it is option 2.
- How a missing glyph is detected at runtime.
- What the user sees when their language cannot be rendered. Silently drawing boxes
  is not it.
