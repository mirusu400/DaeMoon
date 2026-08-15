# core/

C11. The logic that is identical on every platform, written once.

The root `CLAUDE.md` holds the global rules. This file holds what is specific to
this directory.

## The hard rule

**No platform headers.** `3ds.h` and `switch.h` never appear under `core/`, not even
behind an `#ifdef`. `make core-isolation` greps for them and fails the build.

If something here needs a platform capability, the answer is a new function pointer
in `backend.h`, not an include. The moment core knows which console it is on, the
two clients start diverging and a save corruption fix lands on one of them.

## Layout

```
include/daemoon/
  result.h        one error type, values from shared/errors.json
  error_codes.h   GENERATED from shared/errors.json
  str_ids.h       GENERATED from shared/lang/en.json
  backend.h       the platform interfaces
  manifest.h      manifest.json
  archive.h       save packages, payload digest
  api.h           client side of shared/openapi.yaml
  sync.h          what to do with a title, and doing it
  i18n.h          user visible text
  util/{sha256,strbuf,utf8}.h
src/
  error_table.c   GENERATED
  lang_table.c    GENERATED
  util/json.h     jsmn wrapper, internal to core/src
```

`make gen` regenerates the four generated files. They are committed so a console
build never needs Go installed.

## Memory

- **Caller supplied buffers, not malloc.** The 3DS heap fragments badly and a sync
  run that allocates per title will eventually fail on a console that has been awake
  for a while. `daemoon_env_t.scratch` is the copy buffer for everything.
- Nothing sizes a buffer to a save. Packing, unpacking, hashing, uploading and
  downloading all stream.
- `daemoon_archive_ctx_t` is the one large structure, and the caller owns it.
  Allocate it once for the app.

## Errors

- Every fallible function returns `daemoon_result_t`. Use `DAEMOON_TRY`, or
  `DAEMOON_TRY_CLEANUP` anywhere a failure would leave a partial write behind.
- `daemoon_result_from_code` maps an unknown code to `internal_error` and never to
  `DAEMOON_OK`. A newer server must not be able to convince an older client that a
  failure succeeded.
- Platform `Result` values are converted at the boundary with
  `DAEMOON_FROM_BACKEND`. The numeric value is deliberately dropped: it belongs in a
  log line, not in core control flow.

## Text

No string literals reach the screen. The UI backend takes `daemoon_str_ref_t`, which
carries an id and its arguments, so a literal cannot get there by accident.

Templates use `{0}`, not `%s`. A translator reordering `"%s uses %d"` would be
undefined behaviour in C; reordering `"{0} uses {1}"` is not, and `tools/gen`
rejects a translation whose placeholder set differs from English.

## The digest

`archive.h` documents it. The short version: sorted by raw path bytes, and for each
entry the path, a NUL, the size as eight big endian bytes, then the contents. It is
over the files, not over the zip, so the same save packed twice hashes the same.

`shared/fixtures/payload_digest.json` holds vectors produced by a third
implementation. Both the C tests and the Go tests check against them, which is what
stops the client and the server from drifting apart.

## The backend contract

`backend.h` says what the function pointers mean in prose.
`tools/test/backend_conformance.c` says the same thing in a form that can be run,
against any implementation, on the machine or console it will run on. Changing what
core expects of a backend means changing that file in the same commit, or the 3DS
and Switch backends will be written against a contract that no longer holds.

## Testing

`make core-test` builds everything here against `platform/posix` with the address
and undefined behaviour sanitizers on, and runs `tools/test`. New logic passes there
before it goes near hardware.
