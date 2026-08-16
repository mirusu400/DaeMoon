# platform/3ds

libctru, devkitARM. Implements the interfaces in `core/include/daemoon/backend.h`.

Phase 1 in the roadmap. **Backup and restore work on hardware.** A dummy title was
backed up, changed in the game, restored, and the game read the restored save:

```
save/remove-all      8
save/remove-all-done ok
save/commit
restore/core-done    ok
```

So the first Phase 1 question is answered: the three `FileSystemAccess` entries in
`app.rsf` - `CategorySystemApplication`, `DirectSdmc`, `DirectSdmcWrite` - reach
another title's save archive for reading, writing, clearing and committing. No
`IoAccessControl` and no wider filesystem rights are needed for any of it.

The second one is not. **What a secure value does to a save from another console
is still open**, and it cannot be answered on one console: the value is read
before a restore and written back after, so a same-console round trip never
presents a mismatch, which is the case Phase 7 depends on. It needs a second
console, or a title somebody is willing to lose.

Until then `list_titles` marks **every** title as `secure_value = 1`, so the
warning fires before any restore. That is deliberately pessimistic. Hardware says
it is also wrong for fifteen of sixteen titles, and narrowing it is a Phase 3
change, not a Phase 1 one.

## Non negotiable

Build it with `make docker-cia` from the repository root. The image pins makerom
and bannertool by checksum, because neither is in the devkitPro repositories and
one of the two lost its upstream.

**Build and test as a CIA from day one.** A `.3dsx` cannot reach another title's
save archive. The permissions come from the exheader in `app.rsf`, and discovering
that at the end of Phase 1 means redoing Phase 1.

**Commit or lose it.** `FSUSER_ControlArchive(archive, ARCHIVE_ACTION_COMMIT_SAVE_DATA, ...)`
after every write, and the `Result` is checked. Without it nothing is persisted and
the user finds out the next time they launch the game.

**Secure value.** Some titles (Pokemon among them) verify their save against a
console stored secure value. Restoring a save from another console makes the game
treat it as corrupt and delete it. `FSUSER_SetSaveDataSecureValue` and friends.
Verify the behaviour on hardware during Phase 1 and record the outcome: Phase 7
sharing depends on the answer.

## Phase 2: two libraries

The grid shows either the console's installed titles or the DS saves
nds-bootstrap keeps as plain `.sav` files, and L or R switches between them.

The second one is where syncing goes first, and the reason is in
`nds_backend.c`: no permissions, no archive to commit, no service that can refuse
a read. A save is one file. If the sync path corrupts something there, it is the
sync path - which is not a sentence that can be said about the 3DS savedata
backend, where four rounds of hardware testing went into finding out that a read
had to start at offset zero.

That backend is ordinary stdio, so the desktop tests run it directly rather than
through a stub, and the conformance suite grew an option for backends that hold
exactly one entry.

## What is checked where

`make core-test` runs this backend on a desktop against a libctru shaped stub in
`tools/test/ctru_stub/`, under the address and undefined behaviour sanitizers. That
covers the parts that are ours - path building, the tree walk, the truncation
checks, whether `remove_all` clears everything, whether a read only handle refuses
writes - and it is where two real bugs were found before any console saw them.

It says nothing about whether the FS service behaves the way the stub does.

`make emu-selftest` closes some of that gap: it runs the same suite as a real ARM
binary through real libctru against a real save archive, unattended, and reports
`failures=0` today. That is not the verification the roadmap asks for - emulators
do not reproduce real save archive behaviour - but it is where the "directory
already exists" bug was found, and the stub could not have found it.

Only hardware answers the rest, and the same conformance suite runs there.

## What hardware has answered so far

From a survey of 16 titles on one console (`sdmc:/DaeMoon/survey.txt`, produced by
the app's own Survey action):

- **Every save archive opened.** `CategorySystemApplication`, `DirectSdmc` and
  `DirectSdmcWrite` are enough for the save work itself.
- **No SMDH could be read.** Every title came back `not supported`, so every name
  fell back to a product code and no icon loaded. It took three rounds to find
  out why, and the answer was not the filesystem rights: with every single
  `FileSystemAccess` bit set it still failed, and the code turned out to be
  identical to Checkpoint's line for line.

  The raw Result said it: `0xE0C046F8` is fs / not_supported / usage. Reading
  another title's content is not purely an ARM11 filesystem operation - the NCCH
  is decrypted by the ARM9 - and the exheader had an empty `Arm9Capability`
  because the RSF declared no `IoAccessControl`. The service says "not supported"
  for that just as it does for a missing FS right, which is why three rounds went
  into the wrong half of the exheader.

  And the ARM9 rights were not it either. Recording each step separately - open,
  fallback open, archive-then-file, read - showed the open had been succeeding all
  along and the **read** was what failed. An SMDH's file is decrypted as it is
  read, so a request that starts anywhere but offset zero is refused, and the
  service reports that with the same word it uses for a missing permission. The
  name lookup was reading from offset 8 and the icon from 0x24C0. Both now read
  the whole file from the start, in one pass that serves both.

  **Record the raw Result, not the wire code, and do not let one variable carry
  two steps.** `daemoon_result_code` said `unsupported` for four rounds. The raw
  Result narrowed it to fs/usage. Only per-step recording found it, and the first
  attempt at that overwrote itself between the open and the read - which is how a
  survey came back claiming both that the lookup failed and that the fallback had
  not been needed.

  The permissions that were added while chasing this - the full FileSystemAccess
  list and IoAccessControl - were never the fix, and have been taken back out. The
  set is the original three again. If a name or an icon stops loading, the survey
  records the raw Result for every step, so what is missing will be a fact.

- **Names are read; most of them cannot be drawn.** Thirteen of sixteen titles on
  the surveyed console carry only a Korean name, and `C2D_FontLoadSystem` returned
  nothing for the Korean region or the console's own region. The list shows their
  product code beside a correct icon. See `docs/fonts.md`: this is the Phase 3
  font question, and it turns out to affect most of a real library rather than the
  edges of one.
- **Secure values are rare.** One title out of sixteen had one. Two Pokemon titles
  sat next to each other in the list and only one of them did. That is the first
  real evidence on the question Phase 7 depends on, and it says the answer is per
  title rather than per publisher or per genre.
- **The build stamp needs a content hash, not just a commit.** Two builds from the
  same dirty tree carried the same stamp, and a survey could not be attributed to
  either of them. That is precisely the case the stamp was added for.
- **Three titles had an archive with nothing in it.** Backing one of those up is
  now refused rather than producing a package whose restore would clear a save.

## The screens are one UI now, and were not

Restoring froze the application, on the screen after "restore" - which is the
worst place on the console for it to freeze, because from the outside a freeze
there and a restore that stopped halfway look identical. Nothing had been written:
the picker runs before core's confirmation, so no save was ever opened for
writing.

The cause was left over from the text console the UI replaced. `backups.c` still
called `consoleClear` and `printf` on a console that is no longer initialised, and
still drove its input loop with `gfxSwapBuffers` and `gspWaitForVBlank` while
citro3d owns the GPU. Nothing drew, nothing swapped, and the loop had no exit.

**A screen that is not drawn by `gfx.c` is a screen that is not drawn.** When the
UI changed, the calls that were replaced were replaced everywhere they were
obvious - and this one was two function calls deep in a file about backups, so it
compiled, linked, and shipped. Grep for `printf` and `console` in this directory
before believing a screen works.

## A desktop stack is eight megabytes and this one is not

The first restore ever run on this console died in a data abort, writing sixteen
bytes below the stack pointer into a section with no mapping at all. The photo of
Luma's screen was the whole diagnosis: `PC 001099A4`, `FAR` just under `SP`,
access type write. `addr2line` put the PC in `reader_open`'s prologue and the LR
in `daemoon_archive_verify`.

`daemoon_archive_verify` and `daemoon_archive_unpack` each held a
`daemoon_archive_ctx_t` in a local. **That structure is 50,696 bytes.** Both are
on the restore path, both passed every desktop test - where the stack is a
hundred times larger and nothing ever complained.

Both now take the context from the caller, the way `daemoon_archive_pack` and
`daemoon_archive_hash_save` already did. The rule in the root `CLAUDE.md` about
preferring caller supplied buffers is written about the heap; it applies at least
as hard to the stack, and there the failure is not fragmentation but a console
that stops.

`make check` runs `tools/stack-check.sh`, which compiles core and this directory
with `-fstack-usage` and fails on any frame over 8 KiB. It needs no console and
no cross compiler - the structures are the same size on a desktop, which is the
part that goes wrong. Reverting the fix makes it name both functions and exit 1.

And the ceiling that frame hit was not the one `app.rsf` declares. `StackSize` in
the RSF goes into the exheader and is **not** what the main thread runs on:
libctru carries a weak `__stacksize__` whose default is **32 KiB**, and that is
the number that decides whether a call chain fits. The crash registers said so -
the caller's frames sat around `0x08007xxx` with nothing mapped below
`0x08000000`.

Thirty two kilobytes does not hold curl and mbedtls; a TLS handshake alone is tens
of kilobytes. `main.c` sets `__stacksize__` to 256 KiB, and `make cia-verify` reads
it back out of the binary through `tools/check-stack-size.sh`, because nothing at
runtime reports how much stack there is until it runs out.

The three lessons are the same one three times: **the console's limits are not the
desktop's, and they are not what the configuration file says either - so measure
them rather than remembering them.**

## Names and icons are read once, ever

`title_cache.c` keeps each title's name and its 48x48 icon in
`sdmc:/DaeMoon/cache/titles.bin`.

Reading an SMDH opens the title's content and decrypts the front of it, and it was
being done **twice per title per launch** - once by `list_titles` for the name and
again by the icon loader for the picture, on the same file. Now one read fills
both, and the launch after the first one does neither.

Three things about it are deliberate:

- **Failures are cached too.** On this console several titles have no readable
  SMDH, and those are the most expensive ones, because the failure arrives after
  the open.
- **Which means a bug in the lookup gets cached.** One has already happened here -
  the read that had to start at offset zero. So the file carries a format number,
  a build that changes the lookup bumps it, and old files are discarded rather
  than migrated. The console's language is in the header too, because the name
  fallback chain starts there.
- **The Survey never reads it**, and clears it when it finishes. A diagnostic that
  answers from yesterday's cache is not a diagnostic, and this also makes Survey
  the button to press when a name or an icon looks wrong.

Pruning is by what was asked about since the last write, so uninstalled titles
drop out - except when *nothing* was asked about, which on a console means the
title list failed to read rather than that every game was uninstalled. Wiping a
good cache on that evidence would make one transient AM failure cost the next
launch as well.

The stub counts SMDH opens, so "the cache works" is an assertion in
`make core-test` rather than an impression that the loading screen feels quicker.

## Phase 2 is verified on hardware

A `.sav` was packed on the console, uploaded, and pulled back down on a desktop:
the payload inside the package the 3DS built is byte for byte the file on the SD
card. Three saves, uploads and skips both behaving.

Two things had to be fixed to get there and neither was in this directory:

- The server compared the device's platform with the save's and demanded they
  match. A 3DS carries both libraries, which is the whole design.
- The net backend read the HTTP status with `curl_easy_getinfo` after
  `curl_easy_perform` returned, by which point every callback had already run with
  a status of zero. `backend.h` has always said the status is filled in before the
  first `body_write`, because that call is where a save is told apart from an error
  message. A successful upload's response went into the error buffer and came back
  as `parse_error`. The status now comes off the status line in the header
  callback, and core refuses a body offered with no status rather than guessing.

## Two libraries, read once

L and R used to free one list and enumerate the other from scratch, every press.
Both are now held side by side - list, icons, and where the cursor was - and only
the first visit to each pays for a read.

nds icons come from the ROM's banner beside the save (`nds_icon.c`): a 32x32 4bpp
image and a BGR555 palette, converted into the same 48x48 tiled buffer an SMDH
produces so one upload serves both libraries. The swizzle is tested on a desktop
against a Morton table written out by hand, because a wrong swizzle does not fail -
it draws noise, and noise on a console is a photograph and a guess.

## Names are not restricted to ASCII any more

`ascii_names` was tied to `daemoon_gfx_has_language_font()`, which answers a
narrower question: whether an *extra* region font was loaded. It never is, so every
Korean title in the list showed a product code.

Then hardware rendered a Korean error dialog perfectly. The system font a console
ships with is its own region's - the font the HOME menu draws these same names
with - so there was never a reason to refuse them. The survey records the real name
either way, so a title whose glyphs are genuinely missing stays a fact rather than
a guess.

## VRAM has to be mapped, and nothing about drawing says so

Opening the software keyboard was a data abort inside libctru's
`aptConvertScreenForCapture`, reading an address in VRAM. `app.rsf` had an empty
`MemoryMapping`.

The GPU reaches VRAM physically and does not go through the process's MMU, so
**everything drew perfectly without the mapping** - icons, text, six months of UI
work. The one path that needs it is the one where the *CPU* reads the framebuffer:
handing a screenshot to a library applet. That is the software keyboard, and it is
also the HOME button, which is why HOME used to take the console down and was
blamed on a text buffer.

```
MemoryMapping:
  - 1f000000-1f5fffff:r
```

`make cia-verify` checks it, because an application that never opens an applet
would never find out, and this one went months without opening one.

## Proving the backend

`tools/test/backend_conformance.c` is the contract, written against the interface
only and free of anything filesystem shaped. Link it into this build, point it at a
dummy title, and run it on hardware before a real save is anywhere near the app.

Every case there names the caller in core that would break, so a failure says what
is wrong rather than only that something is. "It synced once" is not the bar: the
cases that matter are the ones a happy path never reaches, like `remove_all`
missing a nested file, or `open_file` for writing not creating the directories a
path needs.

## TLS

`httpc:C` ships old cipher suites and a stale root CA store and fails against
modern servers regularly. Link `3ds-curl` plus `3ds-mbedtls` statically for the net
backend. `HTTPC_AddTrustedRootCA()` is the fallback, not the plan.

## Memory

The heap is small. The core interfaces are already streaming and take a caller
supplied scratch buffer; keep it that way. Do not add a path that loads a save whole
just because it is easier on this platform.

## Fonts

The system font varies by console region and may lack glyphs for the selected
language. Detect a missing glyph and either fall back to English or bundle a subset
font. Do **not** bundle a full CJK font: the size is unacceptable. Decide in Phase 3
and write the outcome into `docs/fonts.md`.

## Emulators

Citra and Azahar do not reproduce real save archive behaviour. They are useful for
UI work and for nothing else. Final verification is always on hardware, against
dummy titles, with the SD card backed up first.
