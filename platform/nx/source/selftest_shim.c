/* The globals the test harness owns on a desktop.
 *
 * tools/test/backend_conformance.c is compiled into this build so the contract can be
 * checked on hardware, but tools/test/test_main.c is not: it has a main() and expects
 * a filesystem full of fixtures. These are the only symbols it provided that the
 * conformance suite actually uses.
 *
 * The same shim the 3DS build carries, for the same reason.
 */
#include "../../../tools/test/test.h"

/* daemoon_test_last_failure and the recorder live in tools/test/test_failure.c, which
 * this build compiles. */
int daemoon_test_failures = 0;
int daemoon_test_checks = 0;
int daemoon_test_quiet = 0;
const char *daemoon_test_current = "";
