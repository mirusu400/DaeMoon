/* Remembers the first failure of a run.
 *
 * Split out so both the desktop harness and the console build get it: on a 3DS
 * there is no scrollback, and an unattended run has nobody watching at all, so the
 * one line this keeps is what ends up in a file somebody can read afterwards.
 */
#include "test.h"

#include <stdio.h>

char daemoon_test_last_failure[192];

void daemoon_test_record_failure(const char *file, int line, const char *test,
                                 const char *what)
{
    const char *base;

    /* Only the first: it is the one that explains the rest. */
    if (daemoon_test_last_failure[0] != '\0') {
        return;
    }

    base = file;
    for (; *file != '\0'; ++file) {
        if (*file == '/') {
            base = file + 1;
        }
    }

    (void)snprintf(daemoon_test_last_failure, sizeof(daemoon_test_last_failure),
                   "%s:%d in %s: %s", base, line, test, what);
}
