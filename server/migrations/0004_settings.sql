-- 0004_settings: the few things about an instance that an administrator decides.
--
-- One key/value table rather than a column per answer. There is exactly one of
-- these settings today - whether a stranger may create an account - and a table
-- that already exists is cheaper to add the second one to than a migration per
-- boolean.
--
-- Everything absent means the safe answer. There is no row until somebody changes
-- something, and an instance that has never been configured must not be one where
-- anybody who finds the address can sign up.
--
-- Migrations are append only. Never edit this file once it has run anywhere.

CREATE TABLE settings (
    key        TEXT PRIMARY KEY,
    value      TEXT NOT NULL,
    updated_at TEXT NOT NULL
) STRICT;
