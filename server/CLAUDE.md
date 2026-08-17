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
- **The API does not localize.** It returns a code from `shared/errors.json` and the
  client renders the text. There is not one user facing sentence anywhere under
  `internal/api`, and there should not be.

  `internal/web` is the exception, and it is not really one: the panel is a client,
  the only one written in Go, and rendering text is what a client does. It has no
  sentences of its own either - it reads the same `shared/lang/*.json` the consoles
  read, through `internal/i18n`. A handler sets a **key** in `page.Title` or
  `page.Error` and the template resolves it with `{{$.T "web.something"}}`. A
  literal in a template is a bug, and two tests in that package say so.
- **`shared/openapi.yaml` is authoritative.** `make spec-check` compares it against
  `api.Describe()` and fails when they disagree. Changing a handler means changing
  the spec in the same commit.
- **Migrations are append only.** Never edit one that has run anywhere: somebody's
  self hosted instance has already applied it.

## Accounts and signing up

Three ways an account comes into being, and they are not interchangeable:

- **`/setup`** makes the first one and grants it administrator. It stops working the
  moment an account exists, and every other path redirects to it while there is
  none.
- **People → Add someone** is an administrator making an account for somebody.
- **`/register`** is a person making their own, and it is **closed unless an
  administrator opens it** (`settings.open_registration`, absent meaning no).

The default is the point. This is a save sync server: an open sign up page on an
address a router forwards is somewhere for anybody to put data, and whoever installs
this is not always whoever decided what that router does. The switch is on the People
page, it says which state it is in, and the flag is read again inside the POST
handler because a form being drawn is not permission.

An account made this way is never an administrator. Separation is already there and
is not new code: `ListDevices` and `ListTitles` take a user id, so a new account
starts with nothing and can see nothing else.

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
