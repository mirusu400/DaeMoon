# Phase 1 on hardware

Everything in Phase 1 that a machine can check is checked. What is left needs a
3DS, and it is the part that matters: whether the CIA's permissions actually reach
another title's save archive, and what the secure value does to a restored save.

Read this before running anything. The app writes to save data.

## Before you start

1. **Custom firmware.** boot9strap plus Luma3DS. A stock console cannot install
   this and cannot reach another title's saves if it could.
2. **Back up the whole SD card.** Copy it somewhere else, in full. This is the
   standing rule for touching a save format for the first time, and it is the only
   thing that makes any of the steps below recoverable.
3. **Pick a dummy title.** Something free, small, and that you do not play. Start
   it once so it has a save. Every destructive step below is aimed at this title.
   Never point them at a game you care about.
4. Have a way to read the screen while pressing buttons. Several steps print a
   result code that is worth photographing.

## Install

```bash
make docker-images     # once
make docker-cia        # platform/3ds/daemoon.cia
```

Copy `daemoon.cia` to the SD card and install it with FBI. It appears on the HOME
menu as DaeMoon.

A `.3dsx` build exists (`make docker-3ds`) and is useless for this: it cannot open
another title's save archive at all. If the CIA behaves like the 3dsx, the
permissions did not take, which is finding number one below.

## What to establish, in order

### 1. The CIA permissions work

Launch the app. The title list should have entries.

| What you see | What it means |
|---|---|
| A list of titles with product codes | `am:u` and the FS rights work. Continue. |
| "am:u unavailable" | The build was not installed as a CIA, or the service list in `app.rsf` is wrong. |
| An empty list | `AM_GetTitleList` worked but no title had an openable save archive. Suspect `FileSystemAccess` in `app.rsf`. |

If the list is empty but the console clearly has games with saves, that is the
first real result: the `FileSystemAccess` set is insufficient. Record what you
tried. The set currently in `app.rsf` is the conventional one for a save manager
and is deliberately narrow.

Before suspecting the code, run `make cia-verify`. It reads the built CIA back and
checks that the rights are actually in the exheader, so "the permissions did not
take" and "the permissions are not enough" can be told apart without guessing.
That check already found one real problem: `am:u` was missing from the service
list, which would have produced exactly this empty list.

If the rights are present and the list is still empty, the next thing to try is
`AccessibleSaveDataIds` with `UseOtherVariationSaveData` in the RSF, which is the
finer grained mechanism for reaching another title's save data. Record whether it
was needed.

### 2. The backend behaves the way core assumes

Menu: **Run the backend self test**. It asks twice, then backs up first, then runs
the conformance suite from `tools/test/backend_conformance.c` against the title
you picked.

**It destroys that title's save.** That is the point: it writes files, clears the
archive, commits, and checks that each of those did what core is entitled to
assume.

- `0 failures` means this backend can be trusted with the rest.
- Any failure names the case. Do not sync with that build; the failing case is a
  thing core already relies on.

Then relaunch the dummy game and confirm it still sees a save, or offers to make a
new one. Either is fine. A crash or a corruption message is a finding.

### 3. Backup and restore round trip

1. Play the dummy title briefly so its save has something in it.
2. **Back up a save** on that title. It writes
   `sdmc:/DaeMoon/backups/3ds_<title id>_<digest>.zip`.
3. Play again and change something you can see, in a way you would notice.
4. **Restore a save from a backup**, choosing the file from step 2.
5. Launch the game.

Expected: the state from step 1, not step 3. If the game says the save is corrupt,
go to the next section, because that is the secure value.

Also worth doing once: back up twice without playing in between. The second run
should not produce a second file, because backups are named after their content.

### 4. The secure value

This is the open question the whole phase exists to answer, and the answer decides
whether Phase 7 sharing is possible at all.

Menu: **Show a title's secure value**, on several titles. Record for each:

| Title | Has a secure value | Notes |
|---|---|---|
| your dummy title | | |
| a Pokemon game, if you have one | | expected to have one |
| something first party and older | | |

Then, on the dummy title only:

1. Note whether it has a secure value.
2. Back up, change the save in game, restore.
3. Launch the game.

The restore path reads the secure value before writing and puts it back after. The
question is whether that is enough.

- **Restores cleanly, secure value or not.** Then preserving the value works for
  this title.
- **Game reports a corrupt save and offers to start over.** Then either the value
  is not what binds the save, or something else is checked too. This is the
  interesting result. Record the title, whether it had a value, and what the game
  said.

If you have a second console and are willing to risk the dummy title's save on it,
the more valuable experiment is: back up on console A, restore on console B, and
see what the game does. That is the case Phase 7 is made of, and it is the one no
amount of desktop testing can answer.

## What to write down

Put the answers in `platform/3ds/CLAUDE.md`, replacing the notes that say hardware
has to answer them:

- Which `FileSystemAccess` entries turned out to be necessary. Narrow the set in
  `app.rsf` to what worked; every extra right is one more thing an app that writes
  to save data can get wrong.
- Whether the self test passed, and on what console model and firmware.
- Which titles have a secure value, and whether preserving it across a restore is
  sufficient.

That last one is the gate on Phase 7. Until it is answered, sharing a save between
consoles stays out of scope.

## If something goes wrong

- **The game says the save is corrupt.** Restore the backup the app made, in
  `sdmc:/DaeMoon/backups/`. That is why the restore path makes one first and
  aborts if it cannot.
- **The app hangs.** Hold POWER. Nothing is persisted until the commit, so an
  interrupted write is the state before it started, not half a save.
- **A commit failed.** The app says so and tells you not to launch the game.
  Believe it: the archive is in whatever state the failed write left, and running
  the game will make the game's view of it authoritative.
- **The SD card is unreadable afterwards.** This is what the full backup in step 2
  of the prerequisites was for.
