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
| `quirc/` | https://github.com/dlbeer/quirc | *not vendored yet* | ISC | Phase 4, 3DS QR pairing only |

Notes:

- `jsmn` is used in `JSMN_PARENT_LINKS` mode. It is compiled once in
  `core/src/util/jsmn_impl.c`; every other translation unit includes it with
  `JSMN_HEADER` defined.
- `miniz` is compiled from the release amalgamation (`miniz.c` + `miniz.h`) with no
  configuration macros. Do not enable `MINIZ_NO_MALLOC`; the archive layer already
  bounds allocation by streaming.
- `quirc` is deliberately absent until Phase 4. It is 3DS-only (the Switch has no
  camera) and pulling it in early would put an unused dependency in both platform
  builds.
