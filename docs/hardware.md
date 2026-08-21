# What hardware has answered

One console, one card, and every question that only a console could settle. This is
the record: what was asked, what came back, and what is still open.

`docs/phase1-hardware.md` is the procedure for running the Phase 1 tests. This is
the result sheet for all of them.

The console is a Korean New 3DS with Luma, tested against a self hosted server on
the same network over both http and https.

---

## Phase 1: backup and restore

**Answered.** A dummy title was backed up, changed in the game, restored, and the
game read the restored save.

```
save/remove-all      8
save/remove-all-done ok
save/commit
restore/core-done    ok
```

Three `FileSystemAccess` entries are enough for all of it -
`CategorySystemApplication`, `DirectSdmc`, `DirectSdmcWrite`. No `IoAccessControl`
and no wider filesystem rights. That was established the hard way: four rounds went
into the exheader before the actual problem turned out to be that an SMDH must be
read from offset zero.

**Still open: what a secure value does to a save from another console.** It cannot
be answered here. The value is read before a restore and written back after, so a
same-console round trip never presents a mismatch - which is the case Phase 7
depends on. It needs a second console, or a title somebody is willing to lose.

One title in sixteen has a secure value. Two Pokemon titles sat next to each other
and only one of them did, so the answer is per title rather than per publisher.

## Phase 2: nds-bootstrap and the network

**Answered, both halves.** A `.sav` packed on the console, uploaded, and pulled
back on a desktop is byte for byte the file on the SD card. Three saves, uploads
and skips both behaving, over https with a self signed CA on the card.

Two things had to be fixed and neither was in the console code: the server compared
the device's platform with the save's and demanded they match, and the net backend
read the HTTP status after the transfer rather than off the status line, so a
successful upload's response went into the error buffer.

## Phase 3: savedata sync and conflicts

**Answered, both directions.** With the console's base behind the server and both
sides changed:

- **Keep the server's** downloaded it, asked a second time, and made a local backup
  first. The backup on the card is byte identical to what the console held a moment
  earlier; the server was untouched.
- **Keep this console's** uploaded on top as v5 with parent v4, and every earlier
  version is still downloadable.

Rules 1 and 2 holding on hardware rather than in a test.

Staging that test found the worst bug of the project: an empty save could be
uploaded over a good server version. The guard was on the local backup path and not
on the wire, which is the half where the damage leaves the device.

## Phase 4: pairing

**Answered.** A console scans a code off a browser and pairs itself. Pairing again
rotates the same device rather than adding another.

The camera took five rounds and none of them was about cameras:

- The preview had no picture, so a scan could not be aimed and a failure could not
  be told from a camera that never started.
- The sensor layout was guessed twice. quirc decoded real codes from the wrong
  interpretation, which looked like proof it was right - but quirc fits a
  perspective transform and a shear is affine, so **a decoder that corrects for
  something cannot be used to detect it.**
- The candidates were numbered and the numbers were reordered between builds, so an
  answer read off a screen meant two different things.
- The preview was slow and two fixes went into the decoder before anything was
  timed. Decoding cost 32 ms. Capture cost 250, which was the timeout: the camera
  had nowhere to write while the last frame was being used.

**Still open: the token is on the SD card on this console.** It belongs in the
application's own save archive, and the service refuses to format one -
`0xE0E046BC`, FS, invalid selection. Save provisioning is recorded when a title is
first installed and an update does not revise it, and this console installed
DaeMoon back when it declared no save data at all. A clean uninstall and install
should settle it. Until then the fallback keeps the token on the card and the
settings screen says so.

## Phase 5: sync on the way to HOME

**Written, and run as an ARM binary.** `make emu-autosync` boots a console in an
emulator against a real daemoond, with nothing touching the controls, and checks the
report the run leaves on the card. The grace period expires on its own, the network
probe answers, a save goes up, and the server has the title.

**Still open, and it is only two things:** whether Luma's autoboot lands on this title,
and whether exiting it puts somebody at the HOME menu. Neither is answerable anywhere
but on a console that has been switched off and on.

What the emulator cannot stage is a deferred conflict, because that needs a second
device. Core's tests cover it: a run with the unattended options asks nothing, touches
neither side, and counts the title.

## Phase 6: the Switch backend

**An MVP exists and builds.** An NRO that lists the saves for a chosen account, backs
one up to the card, syncs one against a server, and can run the conformance suite
against a dummy title. Zero warnings from a clean build.

**Answered: the backend behaves.** `277 checks, 0 failures. this backend behaves the
way core assumes`, run in the order `platform/nx/CLAUDE.md` asks for - the conformance
suite, under a selected account, against a dummy title, before a real save was
anywhere near it. Thirteen mounts and twelve commits against SUPER MARIO ODYSSEY
(`0100000000010000`), every one `ok`: reads, writes, truncation, clearing an archive
whole, nested directories, chunked streaming, and a commit after each. A save on this
platform is an ordinary mounted filesystem and it acts like one.

The console is a Korean Switch on Atmosphere, launched through Sphaira in full mode,
against a server on the same network. Sixty-one saves enumerated for the selected
account.

**Still open: the `other` title case.** It needs two dummy titles that both already
have saves, and it is the case that would catch one account's save being handed to
another. One console with one disposable save cannot ask it.

Three things had to be fixed to get there, and each of them is a thing only a console
would have said:

- **`pselShowUserSelector` dereferences its settings.** They were being passed as
  `NULL`. The header marks the argument `[in]` and never says it may be absent, so
  there is no default to fall back on and the applet library reads address zero. It
  is a data abort before the account is ever chosen, and it was the first thing this
  build did on the first console it reached.
- **The text console draws ASCII and nothing else.** `docs/fonts.md` settled the
  policy in Phase 3 - probe at runtime, fall back to English, write the fallback down
  - and the 3DS build implements it. The Switch build never had the check, so a Korean
  console rendered every string as rubbish. The table is now walked for any byte above
  `0x7f`, which needs no list of representative glyphs to keep in step with the
  languages and stops being true on its own the day this build draws with
  `plGetSharedFontByType`.
- **A fresh `PadState` reads held buttons as pressed.** `padGetButtonsDown` is the
  edge between two updates and a newly initialised state has no past, so its first
  update turns whatever is currently down into a press. Every dialog opened a pad of
  its own, so the A that answered one question was still down when the next one
  initialised and answered itself - with the cursor where it starts, on No. The
  destructive confirmation was declining before it could be read, which from the
  outside is indistinguishable from a button combination that does not work. Worse
  was on the same fault: returning to the list with A still down could run a backup
  on whatever the cursor sat on. One discarded update after initialising fixes all
  three sites.

That last one cost four rounds, and it cost them because the trace could not tell
"the combination never fired" from "the confirmation was declined" - both were a
`list/done` followed by `app/exit`. Every exit from the self test is now recorded
(`selftest/asked`, `selftest/declined first|second`, `selftest/run`), which is the
same lesson as the section below and it had to be learned again here.

Writing it moved four files into `platform/common`, which is the thing worth recording
here. The SD card backend, the curl request loop, a directory tree walk and the config
line reader were all identical to the 3DS versions - and the curl one holds the fix
that cost Phase 2 a hardware round. Two copies of that file would have been two chances
to lose it. The 3DS build was rebuilt and re-verified on the shared code before any of
it was called Phase 6.

---

## Things this console said that nothing else could

- **An SMDH must be read from offset zero.** It is decrypted as it is read, and a
  request starting anywhere else is refused with the same word the service uses for
  a missing permission.
- **The main thread's stack is 32 KiB**, not the 256 the RSF asks for. `app.rsf`'s
  `StackSize` is not what libctru puts under the main thread.
- **VRAM is not mapped unless the exheader says so.** Everything draws without it,
  because the GPU does not go through the process's MMU. The one path that needs it
  is the CPU reading the framebuffer, which is the software keyboard and the HOME
  button.
- **The system font draws all eight scripts this project ships.** `font=0
  drawable=en,ko,ja,zh-Hans,zh-Hant,es,fr,de`. A `NULL` from `C2D_FontLoadSystem`
  means no *extra* region font was loaded and says nothing about what can be drawn.
- **The camera stores rows of the frame width**, and overruns its port if no
  receive is armed while a frame is being processed.
- **A homebrew crash in application mode wedges the launcher.** After the data
  abort above, every NRO - not only this one - showed a black screen and returned
  to the menu, with no trace line and no crash report to say why. Three rounds were
  spent reading that as evidence about this application. A reboot cleared it. On
  this platform "it closed instantly" is a statement about the console until a
  second homebrew says otherwise.

## The pattern

Every one of these cost more rounds than it should have, and the same thing fixed
it every time: **make the console record what actually happened, per step, rather
than one word for all of it.** The SMDH probe, `trace.txt`, the QR statistics, the
per-stage timings, the raw `Result` beside every wire code, and the server logging
why a pairing became a new device.

A round trip to a console that comes back as "still broken" is one bit of
information. That is the thing to design against.
