-- 0003_title_names: what a game is called, so a page can say it.
--
-- The server has only ever seen title ids. A console knows the name - it reads it
-- out of the SMDH, and has done since Phase 1 - and simply never sent it, so the
-- web panel could do nothing but list 0004000000055D00 beside AZLK_GIRLSMODE.
--
-- Nullable, and stays null for every title synced before this: it is informational
-- and nothing is keyed by it. A title whose name is unknown is shown by its id, the
-- way everything was shown until now.
--
-- Migrations are append only. Never edit this file once it has run anywhere.

ALTER TABLE titles ADD COLUMN title_name TEXT;
