# platform/3ds

libctru, devkitARM. Implements the interfaces in `core/include/daemoon/backend.h`.

Phase 1 in the roadmap. What is here now is the build wiring and the entry point;
the backend implementations land with Phase 1, on hardware.

## Non negotiable

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
