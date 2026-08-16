# platform/3ds

libctru, devkitARM. Implements the interfaces in `core/include/daemoon/backend.h`.

Phase 1 in the roadmap. The save, filesystem and UI backends are written and the
CIA builds. What is left is the part only a console can answer, and the procedure
for answering it is `docs/phase1-hardware.md`:

- whether the `FileSystemAccess` set in `app.rsf` actually reaches another title's
  save archive, and which entries of it are needed
- what the secure value does to a restored save

Until those are answered, `list_titles` marks **every** title as
`secure_value = 1`, so the warning fires before any restore. That is deliberately
pessimistic and should be narrowed once hardware says which titles bind their
saves.

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
