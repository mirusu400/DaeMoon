/* archive.h - save packages (.zip, miniz) with manifest.json at the root.
 *
 * Layout:
 *   manifest.json          the metadata, see manifest.h
 *   payload/<path>         the save files, paths relative to the save root
 *
 * The payload digest is defined over the files, not over the zip container, so two
 * packages of the same save made on different days have the same digest and the
 * server can content address them. For every entry, sorted by path as raw bytes:
 *
 *   sha256_update(path bytes)
 *   sha256_update(one 0x00 byte)
 *   sha256_update(size as 8 bytes, big endian)
 *   sha256_update(file contents)
 *
 * Nothing here ever holds a whole save in memory. Every copy goes through
 * env->scratch.
 */
#ifndef DAEMOON_ARCHIVE_H
#define DAEMOON_ARCHIVE_H

#include <stddef.h>

#include <daemoon/backend.h>
#include <daemoon/manifest.h>
#include <daemoon/result.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A save with more entries than this is refused rather than truncated. Raising it
 * costs sizeof(daemoon_archive_entry_t) of stack or heap per entry, which is why the
 * table lives in a caller owned context. */
#define DAEMOON_ARCHIVE_MAX_ENTRIES 192

#define DAEMOON_ARCHIVE_MANIFEST_PATH "manifest.json"
#define DAEMOON_ARCHIVE_PAYLOAD_DIR   "payload/"

typedef struct {
    char               path[DAEMOON_PATH_MAX];
    unsigned long long size;
} daemoon_archive_entry_t;

/* Caller owned working set. Allocate it once for the app rather than per operation:
 * it is a few tens of kilobytes and the 3DS heap fragments badly. */
typedef struct {
    daemoon_archive_entry_t entries[DAEMOON_ARCHIVE_MAX_ENTRIES];
    size_t                  count;
} daemoon_archive_ctx_t;

/* Pack an open save archive into out. m is filled in with the payload digest, the
 * size and created_at; the caller sets the identity fields first. out must be
 * writable and seekable. */
daemoon_result_t daemoon_archive_pack(const daemoon_env_t *env, daemoon_archive_ctx_t *ctx,
                                      daemoon_save_t *save, daemoon_manifest_t *m,
                                      daemoon_stream_t *out);

/* The payload digest of an open save, computed the same way daemoon_archive_pack
 * computes it but without producing a package. This is how the sync path answers
 * "did this save change" without writing a megabyte of zip to the SD card first.
 * out_hex must hold DAEMOON_SHA256_HEX bytes. */
daemoon_result_t daemoon_archive_hash_save(const daemoon_env_t *env, daemoon_archive_ctx_t *ctx,
                                           daemoon_save_t *save, char *out_hex,
                                           unsigned long long *out_size);

/* Read only the manifest out of a package. */
daemoon_result_t daemoon_archive_read_manifest(daemoon_stream_t *pkg, daemoon_manifest_t *out);

/* Recompute the payload digest of a package and compare it with the manifest.
 * Called immediately before a restore. On mismatch nothing is written and
 * DAEMOON_ERR_CHECKSUM_MISMATCH comes back.
 *
 * ctx is supplied by the caller for the same reason pack and hash_save take one:
 * it is fifty kilobytes, and a console's stack is not the place for it. Putting it
 * in a local here was a crash on hardware and invisible on a desktop, where the
 * stack is a hundred times larger. */
daemoon_result_t daemoon_archive_verify(const daemoon_env_t *env, daemoon_archive_ctx_t *ctx,
                                        daemoon_stream_t *pkg, const daemoon_manifest_t *m);

/* Write a package into an open writable save archive. The archive is cleared first
 * so files the package does not contain cannot survive a restore.
 *
 * This does NOT commit. The caller commits, checks the result, and treats a failed
 * commit as a failed restore. */
daemoon_result_t daemoon_archive_unpack(const daemoon_env_t *env, daemoon_archive_ctx_t *ctx,
                                        daemoon_stream_t *pkg, daemoon_save_t *save);

#ifdef __cplusplus
}
#endif

#endif /* DAEMOON_ARCHIVE_H */
