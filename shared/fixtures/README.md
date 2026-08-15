# shared/fixtures

Test data read by **both** the C tests (`tools/test`) and the Go tests
(`server/...`). That is the point: if the client and the server ever start
interpreting a manifest differently, one of the two test suites fails here rather
than a save being written back wrong on someone's console.

| File | Used for |
|---|---|
| `payload_digest.json` | The payload digest rule from `core/include/daemoon/archive.h`. Expected values come from a third implementation, so neither side is checked against itself. |
| `manifest_valid.json` | A normal manifest, round tripped by both parsers. |
| `manifest_first_upload.json` | `version: 0`, `parent_version: null`, non ASCII `device_label`. |
| `manifest_bad_parent.json` | `parent_version >= version`. Must be rejected: versions are server issued and strictly increase. |
| `manifest_future_format.json` | `format_version: 2`. Must be rejected rather than guessed at. |
| `error_version_conflict.json` | A 409 body, including the detail the conflict dialog needs. |

Adding a case means adding it to both suites in the same commit.
