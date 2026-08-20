# platform/nx

libnx, devkitA64, NRO. Implements the interfaces in
`core/include/daemoon/backend.h`.

Phase 6 in the roadmap, and deliberately last: everything it needs is already proven
on the 3DS side by then, so this is a backend implementation rather than a redesign.

## What the MVP is

It builds, and it implements the whole of `daemoon_save_backend_t`. An NRO that lists
the saves for a chosen account, backs one up to the card, syncs one against a server,
and can run the conformance suite against a dummy title.

The interface is a **text console**, on purpose. What Phase 6 has to establish is that
a save can be mounted, read, written and committed here; a textured grid would prove
exactly as much as a list does, and it is months the 3DS build has already spent once.

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

Each platform keeps the parts that are genuinely its own: `sockets.c` (soc:U against
`socketInitializeDefault`), `free_space.c`, and `trace.c` deciding where the trace
file goes.

One thing genuinely differs and it is not the code: **the Switch curl port is 7.69**,
against 8.4 on the 3DS. The root bundle is compiled into both binaries the same way
(`vendor/cacert`, bin2s), but `CURLOPT_CAINFO_BLOB` arrived in 7.77 and mbedTLS
learned it in 7.81, so this build cannot hand curl the array directly. `net_curl.c`
writes it to `/switch/DaeMoon/cacert.pem` on first use and points `CURLOPT_CAINFO`
there instead - the app writes that file, never a person. Check
`LIBCURL_VERSION_NUM`, not the platform: if the port is updated the blob path starts
being taken and the file stops being written, with nothing to change here.

## What is left for hardware

Everything that matters. `make core-test` covers the half with no libnx in it - title
id formatting, the config parser, the tree walk and the clear - and says nothing about
whether the FS service behaves the way this assumes. Run the conformance suite
(**X with L or R**, and it asks twice) against a dummy title under a selected account
before a real save is anywhere near this.

The `other` title case is skipped: two dummy titles that both already have saves is a
hardware setup question, and it is the case that would catch one account's save being
handed to another.

## Not done yet

- **The token is on the card.** The 3DS keeps it in the application's own save archive
  because a card comes out of a console; the equivalent here is this homebrew's own
  save data, which needs a title id this build does not have.
- **No pairing flow.** There is no camera, so the device code path is the one that
  applies, and it is not written. `config.txt` is edited by hand for now.
- **No fonts beyond the console's.** The shared font work below is still ahead.

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

`plInitialize()` and `plGetSharedFontByType()`. The shared fonts are split by type
(`Standard`, `ChineseSimplified`, `ChineseTraditional`, `KO`, `NintendoExtended`),
so rendering CJK means loading the matching type and falling back per glyph. Handle
the fallback chain explicitly; a missing glyph should not render as a box in the
middle of a confirmation the user has to answer.

## No camera

There is no camera, so QR pairing does not apply here. The device code flow is the
path: show a six digit code, approve from a phone or PC.

## Emulators

Ryujinx does not reproduce real save behaviour. Final verification is on hardware,
against dummy titles.
