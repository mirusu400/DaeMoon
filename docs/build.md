# Building

Two paths. The container one is the reference, and CI uses it.

## Containers

```bash
make docker-images   # once, and again when a Dockerfile changes
make docker-test     # the desktop suites: core, server, e2e
make docker-cia      # platform/3ds/daemoon.cia
make docker-nx       # platform/nx/daemoon.nro
```

Three images:

| Image | Base | For |
|---|---|---|
| `daemoon-dev:local` | `golang:1.22-bookworm` plus gcc and clang | core tests, server, `daemoonctl`, end to end |
| `daemoon-3ds:local` | `devkitpro/devkitarm` plus makerom and bannertool | the 3DS CIA |
| `daemoon-nx:local` | `devkitpro/devkita64` | the Switch NRO |

The tree is bind mounted rather than copied, so an edit does not need an image
rebuild. Every run maps the invoking user; without that the tree fills with root
owned object files that then need root to delete.

`make docker-shell IMAGE=daemoon-3ds:local` opens a shell in one of them.

### Why the 3DS image is not just devkitPro

A CIA needs two tools that are not in the devkitPro repositories:

- **makerom**, which applies the filesystem permissions in `app.rsf`. Those
  permissions are the whole reason this project ships a CIA: a `.3dsx` cannot
  reach another title's save archive, so a `.3dsx` build can only exercise the UI.
- **bannertool**, for the HOME menu banner. Its original repository was deleted,
  so the image uses the maintained fork.

Both are pinned by version **and by checksum**. A build tool that silently changes
is a build that silently changes, and this one signs the rights that let the app
write to other titles' save data.

## Without containers

Everything works on a machine that already has the tools.

```bash
make test        # core plus go, no console needed
make check       # what CI runs, minus the console builds
make server
make 3ds         # needs DEVKITARM, and makerom plus bannertool on PATH for `cia`
make nx          # needs DEVKITPRO with libnx
```

The desktop side needs a C compiler and Go 1.22 or newer, and nothing else. The
core tests build with the address and undefined behaviour sanitizers on by
default, which is where most of their value is.

## What comes out

| Target | Output | Runs on |
|---|---|---|
| `make server` | `build/daemoond` | anything; a static binary with `CGO_ENABLED=0` |
| `make -C tools/cli ROOT=$PWD` | `build/daemoonctl` | the desktop client, same core a console links |
| `make docker-cia` | `platform/3ds/daemoon.cia` | a 3DS with custom firmware |
| `make docker-nx` | `platform/nx/daemoon.nro` | a Switch with Atmosphere |

The 3DS build is Phase 1: it carries the save, filesystem and UI backends, and the
conformance suite. The Switch build is Phase 6 and its entry point still does
nothing beyond saying so.

That both compile and link the whole shared core is worth something on its own: it
is the standing proof that `core/` is free of platform assumptions, checked by
three compilers rather than by a grep.

## Checking the 3DS build without a console

```bash
make cia-verify     # reads the CIA back: are the rights actually in the exheader?
make emu-selftest   # runs the conformance suite in an emulator, unattended
```

`cia-verify` separates "the permissions did not take" from "the permissions are
not enough", which look identical from a console. It found a missing `am:u`.

`emu-selftest` needs an emulator; point `AZAHAR` at one, or it skips. It is not in
CI: the image is large and needs a display and a GL stack, and it is a convenience
ahead of hardware rather than a gate.

Neither replaces `docs/phase1-hardware.md`.

## Assets

`platform/3ds/assets/` holds a generated placeholder icon, banner and a second of
silence, produced by the script recorded in the commit that added them rather than
committed as binaries nobody can review. They are placeholders. Replace them
before anything ships.
