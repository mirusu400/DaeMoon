# server/

Go. One static binary plus one database file.

The root `CLAUDE.md` holds the global rules. This file holds what is specific to
this directory.

## Why the module lives here

The Go module is `server/go.mod`, not a root one. The repository root has a
`vendor/` directory holding copied in C sources, and Go claims that name for itself.
`tools/` has its own module for the same reason.

## Layout

```
cmd/daemoond/       the binary
internal/apierr/    shared/errors.json, and how a failure reaches the client
internal/api/       the HTTP surface, one file, thin handlers
internal/auth/      tokens and the bearer middleware
internal/config/    environment variables, all with defaults
internal/pkgfmt/    manifests and save packages, the Go half of core/src/archive.c
internal/store/     every SQL statement
migrations/         numbered, append only, embedded in the binary
```

## Rules

- **Handlers stay thin.** No SQL in `internal/api`. If a handler needs a query, it
  goes in `internal/store`.
- **Wrap and propagate.** `fmt.Errorf("...: %w", err)`. `_ = err` is forbidden.
- **Every request has a timeout and a body limit.** `config.Config` carries both and
  `cmd/daemoond` applies them.
- **The server does not localize.** It returns a code from `shared/errors.json` and
  the client renders the text. There is not one user facing sentence in this tree,
  and there should not be.
- **`shared/openapi.yaml` is authoritative.** `make spec-check` compares it against
  `api.Describe()` and fails when they disagree. Changing a handler means changing
  the spec in the same commit.
- **Migrations are append only.** Never edit one that has run anywhere: somebody's
  self hosted instance has already applied it.

## Uploads

The request body is the save package itself, not JSON. `parent_version`, `sha256`,
`size` and `device_label` all live in the `manifest.json` inside it, so a console
never has to buffer and re-encode a multi megabyte save. `parent_version` is
repeated as a query parameter so the conflict check can run before the body is read.

The package is staged in a temp file rather than buffered: a zip has to be read back
to front to find its central directory. It is then verified against its own manifest
before anything is stored, because a package the server keeps is one it will hand
back to a console later, and a broken one would surface as a failed restore on some
other day with nothing to explain it.

Two size limits apply, and they are not the same thing:

- the request body, refused before it is read, which is about transfer and storage
- the uncompressed payload from the manifest, which is about what a console will
  have to write back. A highly compressible save can be tiny on the wire and far too
  large for the archive it has to fit into.

## Blobs

Stored in SQLite, chunked across `blob_chunks` rows at 1 MiB. `database/sql` has no
incremental blob IO, so one cell per save would mean reading the whole thing into
memory on every download. Chunking gives streaming reads and bounded memory.

`blobs.sha256` is the payload digest, not a digest of the zip, and `blobs.size` is
the uncompressed payload to match. Two consoles packing the same save with different
compression produce the same row, which is what makes content addressing work.

Deletion is reference counted. Nothing deletes yet: GC with a retention window and
then `VACUUM` comes later.

## Conflicts

`parent_version != latest` is `409 version_conflict`, and **both versions stay**.
The 409 body carries the server side (`server_version`, `server_size`,
`server_device_label`, `server_received_at`) because the client shows exactly that
through `ui->choose()`. An empty field there is a dialog the user cannot answer.

## Testing

`go test ./...`. The API tests drive a real `httptest` server over a real SQLite
file, and the package tests read `shared/fixtures/`, the same files the C tests
read.
