# platform/nx

libnx, devkitA64, NRO. Implements the interfaces in
`core/include/daemoon/backend.h`.

Phase 6 in the roadmap, and deliberately last: everything it needs is already proven
on the 3DS side by then, so this is a backend implementation rather than a redesign.

## What the MVP is

It builds, and it implements the whole of `daemoon_save_backend_t`. An NRO that lists
the saves for a chosen account, backs one up to the card, syncs one against a server,
and can run the conformance suite against a dummy title.

## The interface is borealis

It was a text console, and that was the right call while the open question was whether
a save can be mounted, read, written and committed here: a list of lines proved that
as well as a grid of icons would. The console answered that question - 277 checks, 0
failures - and the console screen then became the thing in the way.

Two of its limits were not cosmetic:

- **It draws ASCII and stops.** A Korean console showed rubbish, so this build walked
  the whole string table looking for a byte above 0x7f and fell back to English when
  it found one. Every screen in English on a Korean console is a workaround, not a
  fallback. borealis draws with the console's own shared fonts through
  `plGetSharedFontByType` and falls back per glyph across Korean, both Chinese and the
  Nintendo extended set, which is exactly what `docs/fonts.md` recorded this platform
  as waiting for. The probe is gone; there is nothing left for it to catch.
- **There is nowhere to put an icon.** A person recognises a game by its picture
  before they read its name. `icons.c` reads it out of the same `ns` record the name
  comes from.

`vendor/borealis` is xfangfang's fork, which is the maintained one and the one
wiliwili ships. `vendor/README.md` records what was removed from it and why.

**This target builds with CMake and every other target builds with a Makefile.**
borealis carries a large set of compile options and include paths that follow from
which graphics backend it was built with, and copying that list into a devkitPro
Makefile would be two places holding one truth - the mistake the root `CLAUDE.md`
records about the Go version. `platform/nx/Makefile` is a two line wrapper so that
`make docker-nx` and CI did not have to learn a second way to build a console.

### What the screen is

`source/ui/` and nothing else. The rule the rest of the project rests on did not move:
nothing in there decides anything about save data, every sentence is still a
`daemoon_str_id_t` resolved through the tables, and every destructive action still
goes through `ui->confirm`.

- **A grid of tiles**, five across, icon and name, with the panel beside it showing
  what is known about whichever one the cursor is on.
- **The self test is a labelled button in that panel**, not a button combination. It
  was X with a shoulder held, which was undocumented until a legend was written for
  it and was still one press away from a real save on a list of them. Reaching it now
  means moving off the grid, onto a button that says it destroys the save, and then
  answering twice.
- **Operations run on a worker thread.** Core's UI backend is blocking by design -
  `confirm()` returns the answer - and borealis is one main loop that cannot be
  re-entered to ask a question. So the operation waits on the worker while the dialog
  lives on the main thread, which is also what keeps the screen drawing while a save
  is packed. One at a time, and never a queue: two at once is two writers to one save
  archive. See `source/ui/ops.cpp`.
- **borealis's own six hint strings** come out of `shared/lang` too, through
  `make gen` into `platform/nx/romfs/i18n/`. borealis ships four languages and this
  project carries eight, and six English words in the middle of a Korean screen is the
  kind of gap that never gets closed later.

**The platform difference turned out to be short.** `fsdevMountSaveData` makes a save
into an ordinary mounted filesystem, so listing and clearing it is a stdio tree walk
and the files are stdio. Three things are genuinely Switch:

- **The list comes from the saves, not the titles.** `fsOpenSaveDataInfoReader`
  enumerates what exists rather than what is installed, which is the right question: a
  game with no save is nothing to sync, and an archive left behind by a game that has
  been deleted still is.
- **A save belongs to one account**, so the reader is filtered by the selected
  `AccountUid`. Offering somebody else's would be the worst kind of helpful.
- **Commit or lose it**, which is the same rule with a different function name.

Only one save is mounted at a time and the mount is the handle. A second mount under
the same name would silently shadow the first rather than failing, and the sync path
opens exactly one save at once, so a double open is refused out loud.

## What is shared, and why it had to be

`platform/common` exists because of this build. Four things were identical to the 3DS
and are now written once:

| file | what |
|---|---|
| `fs_newlib.c` | the SD card, stdio and dirent. Was `platform/3ds/source/fs_backend.c` |
| `net_curl.c` | the libcurl request loop. Was `platform/3ds/source/net_backend.c` |
| `dir_tree.c` | walking and clearing a directory, which is what a mounted save is |
| `config_lines.c` | the key=value line reader |

`net_curl.c` is the one that mattered most. It contains the fix that cost Phase 2 a
hardware round - the HTTP status is read off the status line in the header callback
rather than with `curl_easy_getinfo` afterwards, because callbacks run first and a
successful upload's body would otherwise go to the error sink. Two copies of that file
would have been two chances to lose it.

Each platform keeps the parts that are genuinely its own: `sockets.c`, `free_space.c`,
and `trace.c` deciding where the trace file goes.

`sockets.c` is now two empty functions and that is deliberate. borealis supplies this
platform's entry point, and its `userAppInit` brings the socket driver up before main
with a larger configuration than the default. Initialising it again fails, and that
failure used to travel up through `daemoon_net_curl_init` as "no network" on a console
whose network was fine. Whoever opens a thing closes it.

One thing genuinely differs and it is not the code: **the Switch curl port is 7.69**,
against 8.4 on the 3DS. The root bundle is compiled into both binaries the same way
(`vendor/cacert`, bin2s), but `CURLOPT_CAINFO_BLOB` arrived in 7.77 and mbedTLS
learned it in 7.81, so this build cannot hand curl the array directly. `net_curl.c`
writes it to `/switch/DaeMoon/cacert.pem` on first use and points `CURLOPT_CAINFO`
there instead - the app writes that file, never a person. Check
`LIBCURL_VERSION_NUM`, not the platform: if the port is updated the blob path starts
being taken and the file stops being written, with nothing to change here.

## What hardware has said

**The backend behaves: `277 checks, 0 failures`.** The conformance suite, run the way
this file asks for it - a dummy title, under a selected account, before a real save was
anywhere near it. Thirteen mounts and twelve commits, every one `ok`. `docs/hardware.md`
carries the detail, including the three faults found getting there: a `NULL` passed to
`pselShowUserSelector`, a missing font fallback on a console whose language this screen
cannot draw, and a fresh `PadState` reading held buttons as pressed.

The suite was **X with a shoulder or a trigger held** at the time, and getting to it
was its own lesson: L against ZL is not a distinction anybody makes while reading a
legend, and pressing the wrong one silently reloaded the list - which looks exactly
like a combination that does not work. It is a labelled button now, for that reason
and because a hidden combination on a list of real saves was never the right place
for it.

`make core-test` covers the half with no libnx in it - title id formatting, the config
parser, the tree walk and the clear - and still says nothing about the FS service. That
is what the suite is for, and it now has an answer on a console.

**Still open: the `other` title case.** Two dummy titles that both already have saves is
a hardware setup question, and it is the case that would catch one account's save being
handed to another.

## Not done yet

- **The token is on the card.** The 3DS keeps it in the application's own save archive
  because a card comes out of a console; the equivalent here is this homebrew's own
  save data, which needs a title id this build does not have.
- **No pairing flow.** There is no camera, so the device code path is the one that
  applies, and it is not written. `config.txt` is edited by hand for now.
- **No settings screen.** The 3DS build has one; here `config.txt` is still edited by
  hand. Everything it would need is in place now that there is a UI to put it in.

## Non negotiable

Build it with `make docker-nx` from the repository root.

**Commit or lose it.** `fsdevCommitDevice()` after every write, result checked.
Exactly the same failure mode as the 3DS: without it nothing is persisted.

**An account has to be selected.** Saves are bound to an `AccountUid`. A different
account is a different save, so unlike the 3DS there is a selection step before
anything else can happen. `daemoon_title_t.account_bound` exists for this.

**Atmosphere is assumed.** Mounting a save needs `fsOpen_SaveData` with an explicit
`SaveDataSpaceId`, and hbloader has to launch the app with adequate `fsp-srv`
permissions.

## Proving the backend

`tools/test/backend_conformance.c` applies here unchanged: it is written against
`daemoon_save_backend_t` and knows nothing about how a save is reached. Run it
against a dummy title, under a selected account, before trusting this backend with
anything real.

The isolation case matters more here than on the 3DS, because a different
`AccountUid` is a different save and nothing in core knows that.

## Applet mode

Applet mode has severely limited memory. Assume title takeover. Detect applet mode
at startup, warn with `warn.applet_mode`, and restrict features rather than failing
somewhere deep in a sync where a save is already half written.

## Fonts

**Done, by borealis.** `switch_font.cpp` in it does what this section used to ask for:
`plGetSharedFontByType` for `Standard`, `ChineseSimplified`, `ExtChineseSimplified`,
`ChineseTraditional`, `KO` and `NintendoExt`, added to one font stash so nanovg falls
back per glyph. Nothing here loads a font and nothing here bundles one.

The consequence worth remembering is that this is the console's own font, which is
also the font its game names are written in - so a name and a menu are never in
different scripts, which is the trap `docs/fonts.md` describes for the 3DS.

## No camera

There is no camera, so QR pairing does not apply here. The device code flow is the
path: show a six digit code, approve from a phone or PC.

## Emulators

Ryujinx does not reproduce real save behaviour. Final verification is on hardware,
against dummy titles.
