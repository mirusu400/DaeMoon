-- 0001_init: the whole schema as of Phase 0.
--
-- Migrations are append only. Never edit this file once it has run anywhere:
-- someone's self hosted instance has already applied it, and changing it makes
-- their database silently different from a fresh one.

PRAGMA foreign_keys = ON;

CREATE TABLE users (
    id         TEXT PRIMARY KEY,
    created_at TEXT NOT NULL
) STRICT;

-- SD cards are removable, so a token is per device and revocable on its own.
-- Only the hash is stored; the token itself is shown once at pairing time.
CREATE TABLE devices (
    id         TEXT PRIMARY KEY,
    user_id    TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    label      TEXT NOT NULL,
    platform   TEXT NOT NULL CHECK (platform IN ('3ds', 'nx', 'nds')),
    token_hash TEXT NOT NULL UNIQUE,
    revoked_at TEXT,
    created_at TEXT NOT NULL
) STRICT;

CREATE INDEX devices_by_user ON devices(user_id);

-- A title id is only unique together with its platform.
CREATE TABLE titles (
    id             INTEGER PRIMARY KEY,
    user_id        TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    title_id       TEXT NOT NULL,
    platform       TEXT NOT NULL CHECK (platform IN ('3ds', 'nx', 'nds')),
    save_type      TEXT NOT NULL CHECK (save_type IN ('savedata', 'extdata', 'nds')),
    latest_version INTEGER NOT NULL DEFAULT 0,
    updated_at     TEXT NOT NULL,
    UNIQUE (user_id, platform, title_id)
) STRICT;

-- Content addressed: two consoles uploading identical saves store one copy, and
-- the two sides of a conflict dedupe naturally.
CREATE TABLE blobs (
    id         INTEGER PRIMARY KEY,
    sha256     TEXT NOT NULL UNIQUE CHECK (length(sha256) = 64),
    -- Uncompressed payload bytes, matching sha256. The stored chunks are the zip
    -- container and are usually smaller.
    size       INTEGER NOT NULL,
    refcount   INTEGER NOT NULL DEFAULT 0,
    created_at TEXT NOT NULL
) STRICT;

-- Blobs are chunked across rows. database/sql has no incremental blob IO, so a
-- multi megabyte save stored in one cell would have to be read whole into memory
-- every time. Chunking gives streaming reads and bounded memory.
-- Chunk size is 1 MiB. Do not change it without measuring.
CREATE TABLE blob_chunks (
    blob_id INTEGER NOT NULL REFERENCES blobs(id) ON DELETE CASCADE,
    seq     INTEGER NOT NULL,
    data    BLOB NOT NULL,
    PRIMARY KEY (blob_id, seq)
) STRICT, WITHOUT ROWID;

-- Every version is kept. A conflict retains both sides, and nothing here ever
-- deletes the other one.
CREATE TABLE versions (
    title_row_id   INTEGER NOT NULL REFERENCES titles(id) ON DELETE CASCADE,
    version        INTEGER NOT NULL,
    parent_version INTEGER,
    blob_id        INTEGER NOT NULL REFERENCES blobs(id),
    device_id      TEXT REFERENCES devices(id) ON DELETE SET NULL,
    device_label   TEXT NOT NULL,
    -- Server clock. Informational: it is never used for ordering, because ordering
    -- comes from `version` alone.
    received_at    TEXT NOT NULL,
    PRIMARY KEY (title_row_id, version)
) STRICT;

CREATE INDEX versions_by_blob ON versions(blob_id);

CREATE TABLE shares (
    code            TEXT PRIMARY KEY,
    title_row_id    INTEGER NOT NULL REFERENCES titles(id) ON DELETE CASCADE,
    version         INTEGER NOT NULL,
    expires_at      TEXT NOT NULL,
    created_at      TEXT NOT NULL
) STRICT;

-- Pairing codes for the QR and device code flows. Short lived by construction.
CREATE TABLE pairings (
    code       TEXT PRIMARY KEY,
    user_id    TEXT REFERENCES users(id) ON DELETE CASCADE,
    approved   INTEGER NOT NULL DEFAULT 0,
    expires_at TEXT NOT NULL,
    created_at TEXT NOT NULL
) STRICT;
