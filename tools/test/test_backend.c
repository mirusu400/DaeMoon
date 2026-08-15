/* Runs the backend conformance suite against the posix backend.
 *
 * The suite itself is in backend_conformance.c and is written against the
 * interface only. This file is the thin part: build an environment, hand it over.
 * platform/3ds and platform/nx do the same thing in Phase 1 and Phase 6, against a
 * dummy title, on hardware.
 */
#define _POSIX_C_SOURCE 200809L

#include "test.h"

#include "backend_conformance.h"
#include "daemoon_posix.h"

#include <daemoon/util/strbuf.h>

#include <stdio.h>
#include <string.h>

TEST_CASE(the_posix_backend_conforms)
{
    char root[256];
    char saves[320];
    daemoon_posix_save_ctx_t ctx;
    daemoon_backend_under_test_t ut;
    daemoon_strbuf_t sb;
    unsigned char scratch[4096];

    CHECK_EQ_INT(daemoon_test_tempdir(root, sizeof(root), "conformance"), 0);

    daemoon_strbuf_init(&sb, saves, sizeof(saves));
    daemoon_strbuf_add(&sb, root);
    daemoon_strbuf_add(&sb, "/saves");
    CHECK_OK(daemoon_strbuf_result(&sb));

    daemoon_posix_save_init(&ctx, saves);
    CHECK_OK(daemoon_posix_save_add_title(&ctx, "0004000000055D00", "Dummy One",
                                          DAEMOON_PLATFORM_3DS, DAEMOON_SAVE_SAVEDATA));
    CHECK_OK(daemoon_posix_save_add_title(&ctx, "0004000000030800", "Dummy Two",
                                          DAEMOON_PLATFORM_3DS, DAEMOON_SAVE_SAVEDATA));

    memset(&ut, 0, sizeof(ut));
    ut.name = "posix";
    ut.backend = &daemoon_posix_save_backend;
    ut.ctx = &ctx;
    ut.title = &ctx.titles[0];
    ut.other = &ctx.titles[1];
    ut.scratch = scratch;
    ut.scratch_len = sizeof(scratch);

    daemoon_backend_conformance(&ut);

    /* The suite commits several times, and every one of them has to have been
     * reported rather than skipped. */
    CHECK(ctx.commits > 0);

    (void)daemoon_posix_rmtree(root);
}

TEST_CASE(a_backend_that_fails_a_commit_is_believed)
{
    /* The suite is only worth running if a broken backend fails it. This is the
     * check on the check: fault injection makes commit fail, and the conformance
     * run has to notice rather than pass anyway.
     *
     * It counts failures itself rather than letting them reach the harness, since
     * the failure is the expected outcome here. */
    char root[256];
    char saves[320];
    daemoon_posix_save_ctx_t ctx;
    daemoon_backend_under_test_t ut;
    daemoon_strbuf_t sb;
    int before;

    CHECK_EQ_INT(daemoon_test_tempdir(root, sizeof(root), "conformance-fail"), 0);

    daemoon_strbuf_init(&sb, saves, sizeof(saves));
    daemoon_strbuf_add(&sb, root);
    daemoon_strbuf_add(&sb, "/saves");
    CHECK_OK(daemoon_strbuf_result(&sb));

    daemoon_posix_save_init(&ctx, saves);
    CHECK_OK(daemoon_posix_save_add_title(&ctx, "0004000000055D00", "Dummy One",
                                          DAEMOON_PLATFORM_3DS, DAEMOON_SAVE_SAVEDATA));
    ctx.fail_commit = DAEMOON_ERR_BACKEND_ERROR;

    memset(&ut, 0, sizeof(ut));
    ut.name = "posix with a failing commit";
    ut.backend = &daemoon_posix_save_backend;
    ut.ctx = &ctx;
    ut.title = &ctx.titles[0];

    before = daemoon_test_failures;
    daemoon_test_quiet = 1; /* the failures below are the expected outcome */
    daemoon_backend_conformance(&ut);
    daemoon_test_quiet = 0;

    if (daemoon_test_failures == before) {
        printf("  FAIL the conformance suite passed a backend whose commit fails\n");
        daemoon_test_failures = before + 1;
    } else {
        /* Swallow the expected failures so the run as a whole stays green. */
        daemoon_test_failures = before;
    }
    daemoon_test_checks++;

    (void)daemoon_posix_rmtree(root);
}

void test_backend(void)
{
    printf("backend conformance\n");
    RUN(the_posix_backend_conforms);
    RUN(a_backend_that_fails_a_commit_is_believed);
}
