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
- **No SMDH could be read** with only those three: every title came back
  `not supported`, so every name fell back to a product code. `Core` was added for
  that and nothing else. Whether something narrower would do is worth one more
  experiment; every right is one more thing an app that writes to save data can
  get wrong.
- **Secure values are rare.** One title out of sixteen had one. Two Pokemon titles
  sat next to each other in the list and only one of them did. That is the first
  real evidence on the question Phase 7 depends on, and it says the answer is per
  title rather than per publisher or per genre.
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
