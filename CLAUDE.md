# CLAUDE.md: DaeMoon

Save data sync for 3DS, Switch, and nds-bootstrap, backed by a self-hostable web server.

> The project name `DaeMoon` is referenced only in this line and in directory names. Keep it that way so a rename is a single grep.
>
> The name is Korean for "main gate" (대문). The resemblance to "daemon" is intentional wordplay only. It does **not** describe the architecture: see the pitfalls section on why a continuously running sync daemon is impossible here. Do not let the name lead the design.

---

## Stack (decided)

| Layer | Choice | Reason |
|---|---|---|
| Shared core | **C11** | libctru and libnx are both C APIs. A C core links directly into both. |
| 3DS client | libctru (devkitARM) | CIA build required for save permissions |
| Switch client | libnx (devkitA64) | NRO build |
| Client UI | C++17 allowed | Platform layer only. Core stays pure C. |
| Switch UI | **borealis** (xfangfang) | The console's shared fonts, per glyph fallback, and a GL context. `platform/nx` builds with CMake because of it. |
| Server | **Go 1.22+** | Single static binary, which matters because self-hosting is a primary goal |
| Database | **SQLite** (`modernc.org/sqlite`) | Pure Go driver, cross compiles without cgo |
| Blob storage | **SQLite (chunked rows)** | See "Blob storage" below |
| HTTP | stdlib `net/http` + `chi` | Minimal dependencies |
| JSON (client) | `jsmn` | Token based, zero allocation. 3DS heap is tight, so cJSON is not acceptable. |
| Zip (client) | `miniz` | Single file, no build system friction |

Dependencies are still a decision rather than a default, and the server's shape is
the constraint that has not moved: one static binary plus one database file. A client
side library is judged on what it does that this project would otherwise have to
write - `vendor/borealis` is here because drawing eight scripts with the console's own
fonts is months of work that already exists and has run on a lot of consoles.



---

## Repository strategy: single monorepo

One repository named `DaeMoon`. Do not split the clients into separate repos.

The two clients share actual compiled C code in `/core`. Splitting them forces either copy paste (the two copies drift, and a save corruption fix landing on only one platform is the worst possible outcome) or a submodule (every core change becomes three commits plus two pointer updates). A routine change like adding a manifest field touches server, 3ds, nx, and the OpenAPI spec at once, which is one commit in a monorepo.

Operating rules:
- Release tags are prefixed: `3ds/v1.2.0`, `nx/v1.0.0`, `server/v2.1.0`
- CI uses `paths:` filters so only affected directories build
- Each top level directory carries its own `CLAUDE.md`. This file holds global rules only.

If a split is ever needed, extract `server/` only. It shares a spec file with the clients, not compiled code. **Never draw a repo boundary between `platform/3ds` and `platform/nx`.** That is the one place where code is genuinely shared.

---

## Repository layout

```
DaeMoon/
├── CLAUDE.md
├── Makefile                    delegation only, no unified build
├── core/                       C11, platform headers strictly forbidden
│   ├── CLAUDE.md
│   ├── include/daemoon/
│   │   ├── backend.h           platform abstraction interface
│   │   ├── manifest.h
│   │   ├── sync.h
│   │   ├── archive.h
│   │   ├── api.h
│   │   ├── i18n.h
│   │   └── result.h
│   └── src/
│       ├── manifest.c
│       ├── sync.c
│       ├── archive.c
│       ├── api.c
│       ├── i18n.c
│       └── util/{sha256.c,strbuf.c,utf8.c}
├── platform/
│   ├── common/                 what both consoles share, no platform headers
│   ├── 3ds/                    Makefile, app.rsf, assets/, source/
│   ├── nx/                     Makefile, assets/, source/
│   └── posix/                  test only backend, no console needed
├── server/
│   ├── CLAUDE.md
│   ├── cmd/daemoond/main.go
│   ├── internal/{api,store,auth,config}/
│   └── migrations/0001_init.sql
├── shared/
│   ├── openapi.yaml
│   ├── manifest.schema.json
│   ├── errors.json             canonical error codes, consumed by both sides
│   ├── lang/{en,ko,ja,zh-Hans,zh-Hant,es,fr,de}.json
│   └── fixtures/               test data read by both C and Go tests
├── tools/test/                 core unit tests, linked against posix backend
├── vendor/{miniz,jsmn,quirc,cacert,borealis}/  copied in, not submodules
└── docs/
```

`vendor/` holds copied sources rather than submodules. These are all single file or near single file libraries, and submodules interact badly with devkitPro cross compilation in CI. Record the upstream URL and version in `vendor/README.md`.

### Core design: write the logic once

3DS and Switch differ only in save access. Manifest parsing, conflict resolution, zip handling, and API calls are identical. **Do not write that logic twice.** Define the interface in `core/include/daemoon/backend.h` and let each platform implement it.

```c
typedef struct {
    int  (*list_titles)(daemoon_title_t **out, size_t *count);
    int  (*open_save)(const daemoon_title_t *t, daemoon_stream_t **out);
    int  (*open_save_write)(const daemoon_title_t *t, daemoon_stream_t **out);
    int  (*commit)(const daemoon_title_t *t);   /* 3DS: ControlArchive, NX: fsdevCommitDevice */
    void (*free_titles)(daemoon_title_t *, size_t);
} daemoon_save_backend_t;

typedef struct {
    int (*request)(const daemoon_http_req_t *req, daemoon_http_resp_t *out);
} daemoon_net_backend_t;

typedef struct {
    int  (*confirm)(daemoon_str_id_t msg);
    void (*progress)(daemoon_str_id_t label, int pct);
    int  (*choose)(daemoon_str_id_t msg, const daemoon_str_id_t *opts, size_t n);
} daemoon_ui_backend_t;
```

Note that the UI backend takes string IDs, not `const char *`. See the i18n section.

`platform/common` holds what turned out to be identical on both consoles once the
Switch build existed: the SD card backend over newlib stdio, the libcurl request loop,
walking and clearing a directory, and the key=value line reader. **The same rule core
has applies here: never include `3ds.h` or `switch.h`.** Each platform keeps the parts
that are genuinely its own - bringing sockets up, asking for free space, deciding where
the trace file goes - behind a named function the shared code calls.

The network one is the reason this directory exists rather than a nicety. That file
holds the fix that cost Phase 2 a hardware round, and two copies of it would have been
two chances to lose it.

`platform/posix` presents a local directory as a fake save archive. **It is a required component, not optional.** It lets the entire sync path be unit tested without a console, and console debugging is by far the most expensive kind. New core logic passes posix tests before it goes near hardware.

---

## Top priority rules: save data loss is unrecoverable

These are not negotiable for any reason.

1. **Always back up locally before restoring.** If the backup fails, abort the restore.
2. **Never auto merge.** Conflicts are always resolved by the user, and both versions are retained. A run covering a whole library may take the answer once up front (`daemoon_conflict_policy_t`) instead of per title, because forty identical dialogs is a reflex rather than a decision - but it still picks a side rather than merging, and both sides still survive: uploading leaves every server version in place, and downloading backs the console's save up to the card first.
3. **Always commit after writing.** 3DS `FSUSER_ControlArchive(..., ARCHIVE_ACTION_COMMIT_SAVE_DATA, ...)`, Switch `fsdevCommitDevice()`. Without it, nothing is persisted.
4. **Never decide freshness by timestamp.** Console RTC is user settable. Trust only the server issued `version`.
5. **Never leave a partial write.** Write to a temp path, then swap once everything succeeds.
6. **Verify sha256** immediately before restore. Abort on mismatch.
7. Destructive actions always pass through `ui->confirm()`. Do not add a `--force` bypass. A run over a whole library may ask **once**, in a sentence naming the count and saying what will be overwritten (`daemoon_sync_opts_t.restore_confirmed`) - that is asking where the decision is, not skipping it. Rule 1 has no equivalent and never gets one: the local backup before a restore is unconditional, and it is what makes rule 7 answerable in bulk at all.

---

## Platform pitfalls

### All platforms
**Sync while a game is running is impossible.** If the game holds the archive, writes corrupt it and reads return stale data. Sync is only valid before or after a game runs. Do not attempt a continuously running daemon sync.

### 3DS
- `.3dsx` lacks permission to reach other titles' save archives. **CIA plus exheader FS permissions are mandatory.** Test as a CIA from day one.
- **Secure value**: some titles (Pokemon among them) verify saves against a console stored secure value. Restoring a save from another console makes the game treat it as corrupt and delete it. Handle via `FSUSER_SetSaveDataSecureValue` and friends. Verify on hardware in Phase 1, since Phase 7 sharing depends on the outcome.
- **TLS**: `httpc:C` ships old cipher suites and a stale root CA store, and fails against modern servers regularly. Prefer static `3ds-curl` plus `3ds-mbedtls`. Fallback is `HTTPC_AddTrustedRootCA()`. A TLS stack with nothing to trust is half an answer: the roots ship inside the binary (`vendor/cacert`, bin2s plus `CURLOPT_CAINFO_BLOB`), because a build that needs a `.pem` placed on the card first works only for the person who built it. `ca_bundle` on the card overrides them, for a private CA.
- Heap is small. Stream and chunk saves rather than loading them whole.

### Switch
- **Atmosphere assumed.** Save mounting needs `fsOpen_SaveData` with an explicit `SaveDataSpaceId`, and hbloader must launch the app with adequate `fsp-srv` permissions.
- **Applet mode has severely limited memory.** Assume title takeover mode. Detect applet mode at startup, warn, and restrict features.
- Saves are bound to a user account (`AccountUid`). Unlike 3DS, **an account selection step is required**. A different account is a different save.
- Missing `fsdevCommitDevice()` loses data exactly like the 3DS case.

### nds-bootstrap
Plain `.sav` files on the SD card, so no permission concerns. **Use this as the Phase 2 network testbed**, since it carries the lowest corruption risk.

---

## Internationalization (required, not a later addition)

Multi language support is a launch requirement. Retrofitting it is expensive, so build it in from Phase 0.

### Client

- **No literal user facing strings in code.** Every user visible string is a `daemoon_str_id_t` enum resolved through `daemoon_str(id)`. The UI backend signatures above enforce this at compile time.
- Language tables live in `shared/lang/*.json` and are converted to C arrays by a build step generating `core/src/lang_table.c`. Runtime JSON parsing for UI strings wastes 3DS heap.
- **Use format placeholders, never sentence concatenation.** Word order varies by language. Write `"Restore %s to %s?"` as a full template string, not assembled fragments.
- Everything is UTF-8 internally. Do not assume one byte equals one character or one column. Use `util/utf8.c` for length and truncation.
- Language is user selectable and persisted. Default is derived from the console setting: 3DS `CFGU_GetSystemLanguage()`, Switch `setGetSystemLanguage()`. Always fall back to English when a key is missing.

### Fonts, the actual hard part

- **Switch**: use `plInitialize()` and `plGetSharedFontByType()`. Shared fonts are split by type (`Standard`, `ChineseSimplified`, `ChineseTraditional`, `KO`, `NintendoExtended`). Rendering CJK requires loading the matching type and falling back per glyph. Handle the fallback chain explicitly.
- **3DS**: the system font varies by console region and may lack glyphs for the selected language. Detect missing glyphs and either fall back to English or bundle a subset font. Do not bundle a full CJK font, since the size is unacceptable. Decide this in Phase 3 and record the outcome in `docs/fonts.md`.
- Layout must tolerate text expansion. German and French commonly run 30 percent longer than English. Do not hardcode label widths.

### Server

**The API does not localize.** It returns machine readable error codes, and the client renders the text.

```json
{ "error": { "code": "version_conflict", "detail": { "server_version": 42 } } }
```

All codes are defined in `shared/errors.json`, which is the single source of truth for both the Go handlers and the C client. Adding a code means adding it to every file in `shared/lang/`, and CI fails if a language file is missing a key.

The **web panel** is a client, not the API, so it does render text - out of the same `shared/lang/*.json`. Keys prefixed `web.` are compiled into the server binary and left out of the C table; everything else goes to the consoles. See `docs/i18n.md`.

---

## Server

### Sync model

The server issues a monotonically increasing `version` per title.

```
POST /v1/titles/{tid}/blob  { parent_version, sha256, size, blob }
  parent_version == server latest  → accept, issue version+1
  parent_version != server latest  → 409 version_conflict, retain both
```

The 409 body carries metadata for both sides (size, device_label, server receive time). The client presents them through `ui->choose()`.

### Blob storage

Blobs are stored in SQLite, not on the filesystem. This is deliberate: self hosting means the entire service state is one file to back up, uploads become transactional with their metadata, and orphaned files cannot happen.

**Blobs are chunked across rows.** Storing a multi megabyte save as one cell would force the whole thing into memory on every read, because `database/sql` has no incremental blob IO. Chunking gives streaming reads and bounded memory.

```sql
blob_chunks(blob_id, seq, data BLOB, PRIMARY KEY (blob_id, seq))
```

- Chunk size: 1 MiB. Do not change it without measuring.
- Content addressing is retained: `blobs.sha256` is unique, so identical content is stored once and conflict versions dedupe naturally.
- Enable WAL mode and set a generous `busy_timeout`.
- Enforce a configurable max save size, default 64 MiB. Reject oversized uploads before reading the body.
- Reads stream chunk by chunk straight to the HTTP response. Never assemble a full blob in memory.
- Deletion is reference counted, but rows are not removed immediately. GC runs separately with a retention window, then `VACUUM`.

If blob volume ever outgrows SQLite, `internal/store` exposes a `BlobStore` interface so an S3 backend can be added without touching handlers. Do not build that now.

### Schema outline

```sql
users(id, created_at)
devices(id, user_id, label, platform, token_hash, revoked_at, created_at)
titles(id, user_id, title_id, platform, save_type, latest_version)
blobs(id, sha256 UNIQUE, size, refcount, created_at)
versions(title_row_id, version, parent_version, blob_id, device_id, received_at)
blob_chunks(blob_id, seq, data)
shares(code, version_row_id, expires_at, created_at)
settings(key, value, updated_at)
```

### API

```
POST   /v1/devices/pair            device pairing
DELETE /v1/devices/{id}            revoke token
GET    /v1/titles                  synced titles
GET    /v1/titles/{tid}/latest     latest metadata
GET    /v1/titles/{tid}/blob/{v}   download
POST   /v1/titles/{tid}/blob       upload
POST   /v1/shares                  create share code
GET    /v1/shares/{code}           download shared save, no auth
```

`shared/openapi.yaml` is authoritative. **Changing a handler means changing the spec in the same commit.**

### Blob format

`.zip` (miniz) with `manifest.json` at the root:

```json
{
  "format_version": 1,
  "platform": "3ds | nx | nds",
  "title_id": "0004000000055D00",
  "save_type": "savedata | extdata | nds",
  "version": 42,
  "parent_version": 41,
  "sha256": "...",
  "device_label": "user set string",
  "secure_value": 1234567890,
  "created_at": "iso8601, informational only, never used for ordering"
}
```

### Authentication

Console software keyboards are painful to type on, so avoid manual token entry.

1. **QR pairing** (preferred): log in on the web, display a QR, scan with the console camera via `quirc`, store the token. 3DS only, since the Switch has no camera.
2. **Device code flow**: show a six digit code in the app, approve from a phone or PC. This is the Switch default path.

- Tokens live on the SD card. SD cards are removable, so **per device revocation is required**.
- The server stores only `token_hash`.
- Never use a hardware ID such as `PS_GetDeviceId()` as an authentication factor. It is not secret and it carries privacy problems.

---

## Build

```bash
make test          # core tests plus go tests, no console required
make core-test     # core only, the main development loop
make server
make 3ds           # devkitARM
make nx            # devkitA64
```

The root Makefile only delegates. Do not attempt a unified build: devkitPro templates depend heavily on `DEVKITARM` and `DEVKITA64` environment variables and break when forced together.

Platform Makefiles pull in the core like this:

```makefile
SOURCES  := source source/ui ../../core/src ../../core/src/util \
            ../../vendor/miniz ../../vendor/jsmn
INCLUDES := source ../../core/include ../../vendor
```

Emulators (Citra/Azahar, Ryujinx) do not reproduce real save archive behavior. **Final verification is always on hardware.**

### CI and releases

`.github/workflows/`:

| workflow | when | what |
|---|---|---|
| `ci.yml` | every push and pull request | contracts, core under both compilers and both sanitizers, the server, end to end |
| `console.yml` | pull requests touching console paths | the CIA (with its exheader read back) and the NRO |
| `nightly.yml` | every push to main, daily, or by hand | `make check`, then all four targets, published as one moving prerelease |

The nightly tag is `nightly` and it **moves**. There is one question it answers - what
main builds into right now - and a tag per build would be a release list nobody can
read. Versioned releases are prefixed per target (`3ds/v1.2.0`, `server/v2.1.0`) and
are a decision a person makes, not something a workflow does.

Nothing is published that has not passed the same checks a pull request has to. "It is
only a nightly" is how a broken one gets published on purpose.

**The Go version comes from `server/go.mod` and nowhere else.** It was written out as a
literal in three workflow jobs and in `docker/dev.Dockerfile`, and it said 1.22 while
the module said 1.25 and the password hashing used `crypto/pbkdf2` - which is 1.24 and
later. Every Go job was failing on a number nobody had a reason to look at. `setup-go`
reads `go-version-file`; use it.

---

## Roadmap

Each phase starts only after the previous one is verified on hardware.

| Phase | Scope | Done when |
|---|---|---|
| 0 | core, posix backend, i18n scaffolding, server skeleton | sync and conflict logic pass on desktop |
| 1 | 3DS local backup and restore CIA | CIA permissions work, **secure value behavior confirmed** |
| 2 | nds-bootstrap `.sav` sync | network and TLS verified on hardware |
| 3 | 3DS savedata sync with conflicts | 409 flow and choice UI work, font decision recorded |
| 4 | QR and device code auth | tokens issue and revoke |
| 5 | Luma autoboot sync | boot, sync, return to HOME |
| 6 | Switch backend | account selection and title takeover work |

Phase 5 is written and runs as an ARM binary: `make emu-autosync` boots a console in an
emulator against a real server and checks the report it leaves behind. What is left for
hardware is the two things about booting - whether Luma autoboots this title, and
whether exiting it lands on HOME.

Phase 6 has an MVP: an NRO that lists the saves for a chosen account, backs one up,
syncs one, and can run the conformance suite against a dummy title. Its "done when" is
still a console away, which is the point of that column.
| 7 | Share codes | requires the secure value question to be resolved |

Phase 0 comes first because everything verifiable without a console should be locked down before hardware debugging starts.

---

## Coding rules

### Client (C)
- **Never swallow errors.** Check every libctru and libnx `Result` and propagate upward. A missing `if (R_FAILED(res))` is a review blocker.
- Save IO must be rollback safe at every failure point.
- Prefer caller supplied buffers over `malloc` in core. Heap fragmentation is a real problem on 3DS.
- **Never include `3ds.h` or `switch.h` from `core/`.** Breaking this collapses the whole design. CI enforces it with a grep check.
- No user facing string literals outside the language tables.

### Server (Go)
- Wrap and propagate errors with `fmt.Errorf("...: %w", err)`. `_ = err` is forbidden.
- Handlers stay thin, logic lives in `internal/`. No SQL in handlers.
- Every request gets a timeout and a body size limit.
- Migrations are append only numbered files. Never edit an existing migration.

### Both
- Always set network timeouts and a retry ceiling. No unbounded waits.
- This file holds global rules only. Platform detail belongs in `core/CLAUDE.md`, `platform/3ds/CLAUDE.md`, `platform/nx/CLAUDE.md`, and `server/CLAUDE.md`.

## Testing

- Conflict resolution, manifest parsing, and zip round trips are tested **on desktop** using `shared/fixtures/`. The same fixture files are read by both the C tests and the Go tests, which is what prevents client and server from interpreting a manifest differently.
- CI checks that every key in `shared/lang/en.json` exists in all other language files.
- Hardware save IO testing uses **dummy titles only**. Never test against a save you actually play.
- Before handling a new game's save format for the first time, back up the entire SD card.

## References

- Checkpoint (GPLv3): useful for title enumeration and edge case handling. **Copying its code makes this entire project GPLv3.** Read it, then write your own.
- FBI: QR scanning reference
- JKSV: Switch save mounting flow reference
