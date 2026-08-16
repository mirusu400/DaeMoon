-- 0002_accounts: people, so there is something to log in to.
--
-- Until now a user was an id and a creation date, made by `daemoond -pair` from
-- the command line. That is enough for a console, which authenticates with a
-- device token, and not enough for the web side the pairing flow was always
-- described in terms of: "log in on the web, display a QR, scan it".
--
-- Migrations are append only. Never edit this file once it has run anywhere.

-- Added rather than recreated, so an instance that has been running since Phase 0
-- keeps its users, its devices and its saves.
--
-- Nullable because those existing rows have no name and no password: a user made
-- by the command line can still own devices and saves, and the first person to
-- claim the account through the web setup page fills these in. A row with a null
-- password_hash cannot be logged into, which is the safe direction.
ALTER TABLE users ADD COLUMN username TEXT;
ALTER TABLE users ADD COLUMN password_hash TEXT;
ALTER TABLE users ADD COLUMN is_admin INTEGER NOT NULL DEFAULT 0;

-- Partial, so the existing null usernames do not collide with each other.
CREATE UNIQUE INDEX users_by_username ON users(username) WHERE username IS NOT NULL;

-- Browser sessions. Separate from device tokens on purpose: a console token is
-- long lived and lives on a removable card, a browser session is short lived and
-- lives in a cookie, and revoking one should never touch the other.
--
-- Only the hash is stored, the same rule device tokens follow. A stolen database
-- must not be a stolen session.
CREATE TABLE sessions (
    token_hash TEXT PRIMARY KEY,
    user_id    TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    expires_at TEXT NOT NULL,
    created_at TEXT NOT NULL
) STRICT;

CREATE INDEX sessions_by_user ON sessions(user_id);
CREATE INDEX sessions_by_expiry ON sessions(expires_at);
