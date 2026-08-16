/* A whole daemoon_env_t wired to the posix backend inside a temp directory.
 *
 * Every test that touches the sync path builds one of these, which is also the
 * cheapest possible demonstration that the platform abstraction holds: the tests
 * assemble the same struct a 3DS build assembles, and core cannot tell the
 * difference.
 */
#ifndef DAEMOON_TEST_FIXTURE_ENV_H
#define DAEMOON_TEST_FIXTURE_ENV_H

#include "daemoon_posix.h"
#include "fake_server.h"

#include <daemoon/archive.h>

typedef struct {
    char                     root[256];
    char                     saves[320];
    char                     work[320];
    daemoon_posix_save_ctx_t save;
    daemoon_posix_fs_ctx_t   fs;
    daemoon_posix_ui_ctx_t   ui;
    fake_server_t            server;
    daemoon_env_t            env;
    daemoon_archive_ctx_t    actx;
} fixture_t;

/* Returns 0 on success. The scratch buffer is static and shared: core never
 * allocates one, so a caller has to supply it exactly like a real app does. */
int  fixture_open(fixture_t *f, const char *tag);
void fixture_close(fixture_t *f);

/* Registers a title. The returned pointer stays valid for the fixture's lifetime. */
const daemoon_title_t *fixture_add_title(fixture_t *f, const char *title_id,
                                         daemoon_platform_t platform);

int fixture_write_save_file(fixture_t *f, const daemoon_title_t *t, const char *rel,
                            const char *content);
int fixture_remove_save_file(fixture_t *f, const daemoon_title_t *t, const char *rel);
/* Returns 0 when the file exists and was read, -1 otherwise. */
int fixture_read_save_file(fixture_t *f, const daemoon_title_t *t, const char *rel, char *buf,
                           size_t cap);
int fixture_save_file_exists(fixture_t *f, const daemoon_title_t *t, const char *rel);

/* Number of files under work_dir/backups. */
size_t fixture_backup_count(fixture_t *f);

/* Packs a title's current save into a standalone package buffer, the way another
 * console would have uploaded it. The caller frees *out. */
daemoon_result_t fixture_pack_blob(fixture_t *f, const daemoon_title_t *t,
                                   unsigned int parent_version, unsigned char **out,
                                   size_t *out_len);

#endif /* DAEMOON_TEST_FIXTURE_ENV_H */
