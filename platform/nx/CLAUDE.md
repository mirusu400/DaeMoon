# platform/nx

libnx, devkitA64, NRO. Implements the interfaces in
`core/include/daemoon/backend.h`.

Phase 6 in the roadmap, and deliberately last: everything it needs is already proven
on the 3DS side by then, so this is a backend implementation rather than a redesign.

## Non negotiable

**Commit or lose it.** `fsdevCommitDevice()` after every write, result checked.
Exactly the same failure mode as the 3DS: without it nothing is persisted.

**An account has to be selected.** Saves are bound to an `AccountUid`. A different
account is a different save, so unlike the 3DS there is a selection step before
anything else can happen. `daemoon_title_t.account_bound` exists for this.

**Atmosphere is assumed.** Mounting a save needs `fsOpen_SaveData` with an explicit
`SaveDataSpaceId`, and hbloader has to launch the app with adequate `fsp-srv`
permissions.

## Proving the backend

`tools/test/backend_conformance.c` applies here unchanged: it is written against
`daemoon_save_backend_t` and knows nothing about how a save is reached. Run it
against a dummy title, under a selected account, before trusting this backend with
anything real.

The isolation case matters more here than on the 3DS, because a different
`AccountUid` is a different save and nothing in core knows that.

## Applet mode

Applet mode has severely limited memory. Assume title takeover. Detect applet mode
at startup, warn with `warn.applet_mode`, and restrict features rather than failing
somewhere deep in a sync where a save is already half written.

## Fonts

`plInitialize()` and `plGetSharedFontByType()`. The shared fonts are split by type
(`Standard`, `ChineseSimplified`, `ChineseTraditional`, `KO`, `NintendoExtended`),
so rendering CJK means loading the matching type and falling back per glyph. Handle
the fallback chain explicitly; a missing glyph should not render as a box in the
middle of a confirmation the user has to answer.

## No camera

There is no camera, so QR pairing does not apply here. The device code flow is the
path: show a six digit code, approve from a phone or PC.

## Emulators

Ryujinx does not reproduce real save behaviour. Final verification is on hardware,
against dummy titles.
