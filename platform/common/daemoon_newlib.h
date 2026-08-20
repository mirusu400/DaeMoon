/* What both consoles share.
 *
 * A 3DS and a Switch differ in save access and in nothing else this project needs -
 * that is the premise the whole design rests on. This directory is where the code
 * that turned out to be genuinely identical lives, so there is one copy of it rather
 * than two that drift.
 *
 * Nothing here may include 3ds.h or switch.h. The same rule core/ has, for the same
 * reason: the moment it does, it belongs to one platform again.
 */
#ifndef DAEMOON_NEWLIB_H
#define DAEMOON_NEWLIB_H

#include <daemoon/backend.h>

/* SD card storage over newlib stdio, which is what both toolchains give. ctx is
 * unused and may be NULL. */
extern const daemoon_fs_backend_t daemoon_fs_newlib_backend;

/* Free bytes on the card, which is the one part of the above that is not stdio: a
 * service call on the 3DS, a different one on the Switch. Each platform supplies it.
 *
 * Zero means "not known" and is a legitimate answer. The caller uses this only to
 * refuse a restore that obviously will not fit, so a console that cannot say is a
 * console that finds out the ordinary way. */
unsigned long long daemoon_newlib_free_bytes(void);

/* A directory tree, walked and cleared.
 *
 * A Switch save is a mounted filesystem, so once it is mounted the two things a save
 * backend does to it - list every file, clear it before a restore - are an ordinary
 * tree walk. Paths handed to the callback are relative to root and use forward
 * slashes, which is what daemoon_entry_cb is specified to receive and what goes into
 * a package.
 *
 * daemoon_dir_remove_all empties root and leaves root itself: the archive is being
 * cleared, not deleted. */
daemoon_result_t daemoon_dir_walk(const char *root, daemoon_entry_cb cb, void *user);
daemoon_result_t daemoon_dir_remove_all(const char *root);

/* -------------------------------------------------------------------- config */

/* One key=value pair from a settings file. */
typedef void (*daemoon_config_pair_fn)(void *user, const char *key, const char *value);

/* Reads a key=value file and hands each pair over. Which keys exist is the platform's
 * business; reading the file is not, and one parser is one place for a trailing
 * whitespace bug to live. not_found when the file is absent, which is a console that
 * has not been pointed at a server rather than a failure. */
daemoon_result_t daemoon_config_read_lines(const char *path, daemoon_config_pair_fn cb,
                                          void *user);

/* ---------------------------------------------------------------- diagnostics */

/* One line per step, on the card, opened and closed per line so it survives a crash.
 * Each platform points this at its own file: the shared code has things worth
 * recording and no business knowing where a console keeps them. */
void daemoon_newlib_trace(const char *step, const char *detail);

/* ------------------------------------------------------------------- network */

/* libcurl over the devkitPro curl and mbedtls ports, shared by both consoles. The
 * request loop is identical on either; only bringing sockets up is not. */
typedef struct {
    /* A CA bundle on the card, and the one compiled into the build.
     *
     * Verification is never turned off: a save is not something to hand to whoever
     * answers the connection. What that used to mean in practice was that https
     * did not work at all unless somebody put a file on the SD card by hand, which
     * is fine for one console and is not a thing that can be shipped. So a bundle
     * of public roots is built in (vendor/cacert), and the card is the override
     * rather than the only source.
     *
     * ca_bundle wins when set, because the reason to set it is a private CA that
     * by definition cannot be in a bundle shipped to everybody. */
    const char *ca_bundle;
    const void *ca_blob;
    size_t      ca_blob_len;
    /* Where to spill the built in bundle when the curl being linked predates
     * CURLOPT_CAINFO_BLOB, which the Switch port does by a wide margin (7.69).
     * Written by the app, not by a person, and rewritten whenever it does not
     * match the bundle in the binary. */
    const char *ca_cache_path;
    /* The last curl code, for a diagnostic that says more than "network error". */
    int last_curl_code;
} daemoon_net_curl_ctx_t;

extern const daemoon_net_backend_t daemoon_net_curl_backend;

daemoon_result_t daemoon_net_curl_init(void);
void             daemoon_net_curl_exit(void);

/* Sockets, which is the one platform specific part: soc:U with a page aligned buffer
 * on the 3DS, socketInitializeDefault on the Switch. Each platform supplies these. */
daemoon_result_t daemoon_net_sockets_init(void);
void             daemoon_net_sockets_exit(void);

#endif /* DAEMOON_NEWLIB_H */
