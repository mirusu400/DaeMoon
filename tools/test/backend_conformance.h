/* The contract every save backend has to satisfy.
 *
 * backend.h says what the function pointers mean in prose. This says the same
 * thing in a form that can be run, against any implementation, on the machine or
 * the console it will actually run on.
 *
 * The point is Phase 1 and Phase 6. The posix backend is the only one that exists
 * today, and core is written against how it behaves. When the 3DS and Switch
 * backends are written, "it works" needs to mean something more specific than a
 * sync that appeared to succeed once: link this suite into the console build, run
 * it against a dummy title, and every assumption core makes is checked on hardware
 * before a real save is anywhere near it.
 *
 * Everything here is written in terms of the interface only. It never touches a
 * filesystem, a path, or anything else a particular backend happens to be built
 * on, because the console backends are built on none of those.
 */
#ifndef DAEMOON_TEST_BACKEND_CONFORMANCE_H
#define DAEMOON_TEST_BACKEND_CONFORMANCE_H

#include <daemoon/backend.h>

typedef struct {
    /* Shown in failure messages. */
    const char *name;

    const daemoon_save_backend_t *backend;
    void                         *ctx;

    /* A title whose save may be created, written, cleared and committed. On a
     * console this is a dummy title and never one that is actually played. */
    const daemoon_title_t *title;

    /* A second title, used to check that two saves do not see each other. NULL
     * skips those cases. */
    const daemoon_title_t *other;

    /* Scratch for the streaming cases. 4 KiB is enough. */
    void  *scratch;
    size_t scratch_len;
} daemoon_backend_under_test_t;

/* Runs the whole battery. Failures are reported through the test harness, so the
 * caller only has to check the global failure count afterwards. */
void daemoon_backend_conformance(const daemoon_backend_under_test_t *ut);

#endif /* DAEMOON_TEST_BACKEND_CONFORMANCE_H */
