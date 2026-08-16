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
