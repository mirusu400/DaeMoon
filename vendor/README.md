# vendor/

Third-party sources are **copied in, not submodules**. Submodules interact badly with
devkitPro cross compilation in CI (see root `CLAUDE.md`).

Update procedure: replace the files, update the table below in the same commit, and
run `make test`. Never patch vendored sources in place; if a patch is unavoidable,
record it in this file under the entry.

| Directory | Upstream | Version | License | Used by |
|---|---|---|---|---|
| `jsmn/` | https://github.com/zserge/jsmn | master @ 2023-06 (`jsmn.h` single header) | MIT | `core/src/manifest.c`, `core/src/api.c` |
| `miniz/` | https://github.com/richgel999/miniz | 3.0.2 (release amalgamation) | MIT | `core/src/archive.c` |
| `quirc/` | https://github.com/dlbeer/quirc | 1.2 (`lib/` only) | ISC | Phase 4, 3DS QR pairing; and `make check`, which decodes what the server encodes |
| `cacert/` | https://curl.se/ca/cacert.pem | 2026-08-13 upstream, filtered by `roots.txt` | MPL 2.0 (Mozilla CA list) | `platform/common/net_curl.c`, via bin2s in both console Makefiles |
| `borealis/` | https://github.com/xfangfang/borealis | `5f08b28`, 2026-04-25 | Apache 2.0 | the Switch interface, `platform/nx/` |

Notes:

- `jsmn` is used in `JSMN_PARENT_LINKS` mode. It is compiled once in
  `core/src/util/jsmn_impl.c`; every other translation unit includes it with
  `JSMN_HEADER` defined.
- `miniz` is compiled from the release amalgamation (`miniz.c` + `miniz.h`) with no
  configuration macros. Do not enable `MINIZ_NO_MALLOC`; the archive layer already
  bounds allocation by streaming.
- `quirc` is the `lib/` directory of the 1.2 release and nothing else; the `demo/`
  tree needs OpenCV and SDL and is not used. Tarball sha256
  `73c12ea33d337ec38fb81218c7674f57dba7ec0570bddd5c7f7a977c0deb64c5`.
- Known defect, not patched: the first `quirc_resize` calls `memcpy` with a null
  source and a length of zero, which the undefined behaviour sanitizer reports on
  every run. It is harmless. `tools/test/ubsan.supp` silences it, because a
  suppression is visible and a patched vendor tree is not.
- It is linked into the 3DS build only - the Switch has no camera - but it is also
  compiled into the desktop tests, where it decodes the QR codes the Go server
  encodes. The two sides of the pairing flow are written in different languages by
  different code, so having one check the other is worth more than either testing
  itself.
- `borealis` is the exception to everything else in this table: it is a UI framework
  rather than a single file, it is C++ rather than C, and it is the only vendored
  thing here with a build system of its own. It is here because the Switch interface
  needed two things a text console cannot do - draw a script that is not ASCII, and
  show a game's icon - and writing both against the shared fonts and a GL context is
  months of work that this library has already done and had run on a lot of consoles.
  It is xfangfang's fork rather than natinusala's original because that is the one
  that is maintained and the one wiliwili ships.
- It is the copy the Switch build needs and no more. Removed from the upstream tree:
  `demo/`, the Android, PS4, PSVita and WinRT projects, the empty `glfw` and `SDL`
  submodules (this build links the devkitPro glfw port), `libromfs` (resources come
  out of the NRO's own romfs), and the tests, docs and packaging of the vendored
  `fmt` and `yoga`. What is left of `resources/` is the icon font and the touch
  cursor; the shipped `i18n/` is not used, because
  `platform/nx/romfs/i18n` is generated from `shared/lang` by `make gen` so that
  borealis's own six hint strings are translated in the same place as everything
  else.
- Its build is `platform/nx/CMakeLists.txt`, which is why the Switch target is CMake
  and every other target is a Makefile. That is not a preference: borealis carries a
  large set of compile options and include paths that follow from which graphics
  backend it was built with, and a second copy of that list in a Makefile would be
  two places to keep one truth.
- `cacert/` is not source. It is the root certificates the consoles trust, compiled
  into the binary as a byte array so that https works on a console somebody
  installed rather than only on one whose owner put a `.pem` on the card by hand.
  `ca_bundle` in `config.txt` still overrides it, which is how a private CA is
  reached; a private CA cannot be in a list shipped to everybody.
- It is a subset, not the whole Mozilla bundle. curl creates a fresh easy handle per
  request and mbedTLS parses the entire CA file for each one, so all 121 roots would
  be ~185 KB of PEM re-parsed on a 268 MHz ARM11 on every request of a sync that
  makes one per title. `roots.txt` names the 36 a self hosted server is actually
  likely to use and `mkbundle.sh` regenerates `data/cacert.bin` from it, failing if
  a named root has left upstream. `ALL_ROOTS=1` takes everything if that trade ever
  stops being worth it.
- Regenerating is a person's decision, not a build step. A build that reaches the
  network to decide what it trusts is a build nobody can reproduce.
