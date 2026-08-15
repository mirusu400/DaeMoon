# The save package format

A save package is a `.zip` (miniz on the client, `archive/zip` on the server) with
two kinds of entry:

```
manifest.json          metadata, see shared/manifest.schema.json
payload/<path>         the save files, relative to the save root
```

`manifest.json` is written last by the client, because the payload digest is not
known until the payload has been written and zip imposes no ordering. Readers go
through the central directory, so this is invisible to them.

## The payload digest

`manifest.sha256` is a digest of the **files**, not of the zip container. Two
packages of the same save, made on different days or with different compression
settings, have the same digest. That is what makes the server's content addressing
work and what lets the client answer "did this save change" without comparing byte
streams.

Entries are sorted by raw path bytes. For each entry, in order:

```
sha256_update(path bytes)          e.g. "sub/extra.bin"
sha256_update(one 0x00 byte)
sha256_update(size, 8 bytes big endian)
sha256_update(file contents)
```

The NUL and the length are both load bearing. Without them, `"ab" + "c"` and
`"a" + "bc"` would hash the same, and so would two different sets of files.

`manifest.size` is the sum of the entry sizes: uncompressed payload bytes, not the
size of the zip.

### Where it is implemented

| Side | File |
|---|---|
| Client | `core/src/archive.c`, `daemoon_archive_pack` and `daemoon_archive_hash_save` |
| Server | `server/internal/pkgfmt/pkgfmt.go`, `Digest` |

### How the two are kept honest

`shared/fixtures/payload_digest.json` holds vectors whose expected values came from
a third implementation, so neither side is checked against itself. The C tests
(`tools/test/test_archive.c`) and the Go tests
(`server/internal/pkgfmt/pkgfmt_test.go`) both read that file.

If the two sides ever disagree, one of those suites fails. The alternative is a save
that refuses to restore on someone's console, months later, with the correct data in
hand and no way to tell which side is wrong.

## Entry paths

Treated as hostile input on both sides, because a package can arrive through a share
code. Refused: absolute paths, anything containing `..`, backslashes, colons, control
characters, doubled slashes, trailing slashes, and anything at or beyond 256 bytes.

A package with an unsafe path is rejected whole. It is never partially extracted.

## Timestamps

`created_at` is informational. Nothing reads it for ordering, on either side. The
console RTC is user settable, so a save "from the future" is an ordinary thing to
encounter and a save "from the past" proves nothing. Ordering comes from the server
issued `version`, and equality comes from the digest.

The client fills `created_at` from the platform clock if one is offered and writes
`1970-01-01T00:00:00Z` otherwise. Neither case changes any decision.

## format_version

`1`. A package claiming anything else is refused rather than parsed leniently. An
unreadable package is recoverable; a misread one is not.
