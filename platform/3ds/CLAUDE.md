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

## Phase 3 is verified on hardware

3DS savedata syncs to a server, and the conflict flow works in both directions
against a real one, with a desktop standing in for the second device.

The console's base was behind the server and both sides had changed:

- **Keep the server's** downloaded it, and because that path ends in
  `daemoon_sync_restore_package` it asked a second time and made a local backup
  first. The backup on the card is byte identical to what the console held a
  moment earlier, and the server was not touched. Both versions survived - one on
  the server, one on the SD card.
- **Keep this console's** uploaded on top, issuing v5 with parent v4. Every
  earlier version is still downloadable.

That is rules 1 and 2 holding on hardware rather than in a test: back up before
restoring, never auto merge, keep both sides.

The conflict dialog names the other device, which is the point of it: `Kali PC`
against `3DS`, with sizes. A dialog that cannot tell two saves apart is not a
choice.

Staging that test found the bug worth remembering from the whole phase - an empty
save could be uploaded over a good server version. See the commit; the guard was
on the local backup path and not on the wire, which is the half where the damage
leaves the device.

## Phase 4: the console pairs itself

Two rows in Settings, meeting at one exchange. Typing six digits uses the keyboard
the rules call painful and is there because it always works; scanning the code off
a browser is the one worth having, because the payload carries the server address
and a console that has never been configured needs nothing typed at all.

`qr_scan.c` is the camera, and three things about it are load bearing:

- The frame is RGB565 and quirc wants luma. The conversion is here, because a
  decoder has no idea what a 3DS frame looks like.
- `camuInit` is not enough. Select, configure, set the buffer, activate - and a
  frame taken before the camera settles is black, which decodes as nothing and
  looks exactly like a code that will not scan.
- The receive is a blocking service call. It has a timeout, because a camera that
  stops delivering must not become an application that never returns on a screen
  whose only way out is the frame callback.

The sensor stores **rows of the frame width, top to bottom**, and settling that
took four rounds for two reasons that are not about cameras.

The argument that first produced the right answer was wrong. quirc was decoding
real codes from the buffer read as 400 wide rows, and that looked like proof the
rows were 400 wide - reading columns as rows shears a picture, and surely no QR
code survives that. It does: quirc fits a perspective transform from the finder
corners, a shear is affine, and affine is inside what such a transform undoes. So
decoding proved the buffer held an image and nothing about its shape, and believing
otherwise is what made me abandon a correct answer.

**A decoder that corrects for something cannot be used to detect it.**

Then the preview was made to cycle through four candidates so the console could
answer instead of being guessed at - and the candidates were numbered, and I
reordered them between two builds. "Layout 2" meant two different things and a
round went into finding that out. They have names now, `cols/TL` and friends, the
indices do not move, and a test says so.

**Only the optics need hardware.** `make emu-pair` runs the whole flow in an
emulator against a real server, with the camera replaced by a file holding exactly
what a scan would have produced - same parser, same network backend, same token
write. It comes back `parse=ok net=ok pair=ok save=ok`. What is left to find out on
a console is whether a 3DS camera can read a code off a monitor.

The two unattended modes now clear each other's flag. A leftover `AUTOTEST` turned
the first pairing run into a conformance run and reported that the app "did not get
as far as the pairing call", which is true and says nothing.

## The first launch says what this is

`welcome.c` runs once, before the library is read, and asks two things: whether the
pages were read, and how to reach a server.

Three pages, and the third is the one somebody has to have seen - syncing while a
game holds its archive open loses a save with this application working exactly as
designed, and no code here can prevent it. The other two are what the reader came
for, so they go first; a page nobody reads is worse placed before the page that
matters.

**There is no "official server or your own" step**, and the reason is in the
payload: a pairing code is `DAEMOON|1|<server>|<code>`, so scanning answers the
address and the credential at once. An address step before a scan is a step the next
one overwrites, and it teaches the wrong thing about how pairing works. What the
screen offers instead is *how*: scan, type both by hand, or neither for now.

Typing needs the address first because pairing by code has to know where to send it.
That is the whole argument for preferring the camera.

`welcomed` in `config.txt` records that the screens had their turn - not that they
succeeded. Deducing it from "is there a server" is the version that annoys: somebody
who chose Not now has read them and still has no server, and would be shown them
every launch. A file written before the flag existed reads as "not yet", so an
existing install sees them once, which is the safe direction for the page about
running games. Settings has a row that brings them back on purpose.

The unattended runs skip it. `AUTOTEST` and `AUTOPAIR` answer to a file rather than
to a person and must not stop at a screen waiting for A.

`welcome_steps.c` holds the decisions - which pages, in what order, whether they are
due - and has no citro2d in it, so `make core-test` checks them. Drawing needs a
console; the order of the pages does not. The test also pins that "not now" is the
last choice, because leaving the screen returns the last one and a reorder would
make START pair the console.

## Phase 5: the sync that happens on the way to HOME

Luma can autoboot a title when the console is switched on. Set it to this one and every
power-on becomes: sync, then HOME. The argument for it is in the root `CLAUDE.md` - a
sync is only valid before or after a game runs, and there is no moment more certainly
"before" than the console having just been turned on.

It is also the moment nobody is watching, and **that narrows what it may do rather than
widening it**. `daemoon_3ds_autosync_opts` is the whole safety argument:

- **DEFER on conflict.** The only defensible answer. The other two policies pick a
  side, and picking a side is a decision - one this project will not make on somebody's
  save while they are not there. A deferred conflict leaves both versions exactly where
  they are and gets counted, so the next launch has something to say.
- **The upload question is answered**, because an upload cannot lose anything: the
  console is unchanged and the server adds a version.
- **The restore question is answered too**, and that is safe *because* of the first
  point. After DEFER has taken the conflicts out, every remaining download replaces a
  copy of a version the server still holds - and rule 1 puts it on the card first
  regardless.

Three things it owes somebody who is not looking at it: a way out before anything runs
(B **held**, not pressed - a window you cannot see is not a way out), a report on the
card because the screen is gone by the time anyone could read it, and the network being
absent as a line in that report rather than a hang. Wi-Fi is not up the instant a
console is, so it probes the server for ten seconds first.

Off by default, and a Settings row rather than a flag file: it does something on every
launch, and a launch somebody made deliberately should not be spent on it unasked. It
is also decided **before** the welcome screens, because a console that boots into a
sync must not stop at an explanation waiting for A.

`make emu-autosync` runs the whole thing as an ARM binary against a real daemoond and
checks the report. What is left for hardware is the two things about booting: whether
Luma lands on this title, and whether exiting it puts somebody at HOME.

## Adding a Settings row means checking the arithmetic, or not having to

Twice a row was added and drawn below 240 - once at 214..244, again at 223..250 - and
both times it was a settings entry nobody could see or reach. The spacing now comes
from the row count rather than the row count having to fit the spacing.

## The token is not on the SD card

It lives in this application's own save archive, which is why the shipped build
declares `SAVEDATA_SIZE` at all now.

A card comes out of a console. While the token was in `config.txt`, a found card
was a working credential: whoever had it could read a person's saves off the server
and write over them. Revocation is the answer to that and always has been, but
revocation is something you have to notice you need, and by then the card is gone.

The alternative that gets proposed - derive the token from console hardware so it
never changes - trades that for something worse. A token the console computes is
one the server can neither rotate nor revoke, the derivation is in public source,
and a console that changes hands takes the identity with it. What was actually
wanted from it was a stable identity, and pairing already rotates the same device
in place, so there is nothing left for it to buy.

`config.txt` keeps the server, the label and the language, and the parser still
reads a `token` key so a console paired before this can be carried across once. It
is never written back, which is what takes it off the card.

The conformance suite clears the archive it is pointed at, so it re-saves the token
afterwards. Running a diagnostic should not unpair a console.

**A console updated from a build that declared `SAVEDATA_SIZE=0K` may not be able
to format one.** Save provisioning is recorded when a title is first installed and
an update does not revise it, so the service answers `FSUSER_FormatSaveData` with
`0xE0E046BC` - FS, invalid selection - no matter what size is asked for. Four sizes
are tried and the trace records each. A clean uninstall and install is what fixes
it.

Which is why the fallback exists rather than being a nicety: on such a console the
token goes on the card, pairing completes, and the settings screen says where the
token is. A console that cannot pair would be a worse outcome than one whose
credential is somewhere findable.

## One run over a whole library

`batch.c` is Back up everything and Sync everything, and it exists because the per
title buttons were the whole interface: forty installed games meant forty presses of
the same two buttons, which is not a feature people use carefully but one they stop
using.

Sync asks a second question, because a run over a library meets conflicts and being
asked forty times is a dialog somebody holds A through rather than a decision.
`daemoon_conflict_policy_t` in core answers it in advance: ask, keep this console's,
or keep the server's. The cursor starts on **ask**, and an index off the end of the
list is also ask, because the answer to a bug on that screen must not be a policy
nobody chose.

**Neither policy is a `--force`.** Rule 7 is about confirmation and rule 2 is about
merging, and both still hold:

- Keeping this console's uploads on top. The server keeps every version it had, so
  the other side is still downloadable. Nothing is discarded and nothing is merged.
- Keeping the server's goes through `daemoon_sync_restore_package`, which backs the
  console's save up to the card first and checks the digest before writing. Rule 1
  does not bend for a policy, and `make core-test` asserts that as well as asserting
  that the restore confirmation is still asked.

What the policy replaces is one question, and the confirmation it replaces it with is
asked where the decision actually is: over a library, "back up 41 saves" with the
count in it is the thing being agreed to.

Three questions, not a hundred and twenty. `daemoon_sync_opts_t` carries what the run
already asked: the conflict answer, `upload_confirmed`, and `restore_confirmed`.

The first two are easy. An upload changes nothing on the console and the server adds
a version rather than replacing one, so that dialog is about intent, and asking it
once per title made a bulk upload worse than doing them by hand.

`restore_confirmed` is rule 7 and is not easy, so the terms are written down. When
the answer is to take the server's, the confirmation is **its own sentence** - it
names the count, says saves on this console will be overwritten, and says each is
backed up to the card first - rather than the generic one. One screen that says that,
read once, is worth more than forty identical dialogs, and forty identical dialogs is
what a person holds A through.

What has no field, and cannot be turned off by anything, is **rule 1**. Every restore
still writes a package to the SD card before it writes to an archive and still aborts
if that fails. That is what makes the answer recoverable rather than final, and it is
the reason the field can exist at all. `daemoon_sync_restore_package` - the published
restore, which is what the Restore button uses - always asks: a caller holding a
package has not been through a screen that named a count.

`make core-test` pins all of it: that a conflict policy alone does not skip the
restore question, that the flag does skip it, that the backup still happens and the
server still holds the replaced version when it does, and that the published restore
has no way to skip its own.

Coming back out is cheap. `action_batch` returns whether a save archive on this
console actually changed, and only then is the library re-read - so pressing B on the
way in costs nothing, and a batch backup or a batch upload costs nothing either.
Re-reading opens every archive on the console, which is long enough to look like a
hang; paying that for a screen somebody opened by accident is what it used to do.

B stops after the title in flight rather than during it. Every write in core is
temp-then-rename or archive-then-commit, so stopping between titles cannot leave
anything half written - and stopping inside one is not something this screen could
promise.

**Both libraries, not the one on screen.** It used to run over whichever list was
showing, which made "everything" mean half of it on a console that carries the installed
titles and the DS saves - and the startup sync always did both, so the two disagreed
about what the word meant. The count in the confirmation is the total across both, which
is why opening the screen reads the library that has not been read yet: a number in a
question has to be the number of things that will happen. The library the person was
looking at is put back afterwards.

`batch_steps.c` holds the order and what each answer means, has no citro2d in it, and
is checked on a desktop. Same split as the welcome, for the same reason. `LIB_3DS` and
`LIB_NDS` moved into the header when this changed: batch.c and autosync.c index by them
now, so they are part of the contract rather than names local to main.c.

## The self test is not a button any more

It used to sit on the grid, run against whichever title the cursor was on, and it
clears that title's save archive to prove clearing works - two rows above Back up, on
a screen whose other buttons are things you do to a game you care about.

The contract has not gone anywhere. `tools/test/backend_conformance.c` is still
linked in, `make core-test` still runs it against the stub, and `run_autotest` still
runs it on hardware against **this application's own save archive**, which is the
only archive it has any business destroying. What is gone is the one press between a
real save and a wipe.

## The secure value belongs to the save, not to the console

It used to be arranged in `main.c`: read the console's current value before a restore,
put it back after. That preserves the *console's* value - which is not the same as the
one the save being restored was bound to. Back up a save, play the game some more, then
restore: the value has moved on, the game checks the restored save against it, they do
not match, and the game deletes the save. Doing nothing at all would have failed the
same way.

So it is packed **with** the save. `daemoon_save_backend_t` grew `read_secure_value` and
`write_secure_value` - core cannot call libctru, and this is exactly what backend.h is
for - the manifest carries `secure_value`, and a restore writes back the value from the
package after the commit.

Three states, and they stay distinguishable all the way through:

- **a value** - packed, and written back on restore
- **no value** (the title has none) - nothing packed, nothing written. Zero is a
  legitimate value, so this cannot be a zero in the field
- **could not be read** - nothing packed, and the backup still happens. A diagnostic
  failure must not turn into a save nobody has a copy of

A package written before this has no field, so a restore from one leaves the console's
value alone, exactly as it did. `make core-test` pins all four cases.

Written after the commit, not before: a value pointing at a save that was not written
is worse than one pointing at the save that was. A failure to write it is reported and
does not fail the restore, because the save is already on the console and telling
somebody it failed would send them to restore it again.

## Survey is a diagnostic, and sits where diagnostics sit

It was on the grid between Sync and Settings, labelled "look at every title and write it
to the SD card", which reads like a backup. It is not one: it opens every archive, reads
what each step returned, records the secure value, the build stamp and which languages
this console can draw, and writes one text file. Nothing it does touches a save.

It is in Settings now, called "collect debug information", with a line under it saying
what it is for. It is still the button to press when a name or an icon looks wrong,
because it ignores the name cache and clears it afterwards.

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

**The roots come with the build.** `vendor/cacert/data/cacert.bin` is turned into an
object by bin2s (`DATA` in the Makefile) and handed to curl as `CURLOPT_CAINFO_BLOB`.
Linking a TLS stack and then shipping it with nothing to trust is only half an
answer: it worked on the console of whoever wrote it, because that console had a
`.pem` on its card, and it returned `tls_error` against a perfectly ordinary Let's
Encrypt server on every other one. Do not remove the bundle and do not turn
verification off; `ca_bundle` in `config.txt` overrides it for a private CA, which is
the case a shipped list cannot cover.

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
