/* The three globals the test harness owns on a desktop.
 *
 * tools/test/backend_conformance.c is compiled into this build so the contract can
 * be checked on hardware, but tools/test/test_main.c is not: it has a main() and
 * expects a filesystem full of fixtures. These are the only symbols it provided
 * that the conformance suite actually uses.
 */
#include "../../../tools/test/test.h"

int daemoon_test_failures = 0;
int daemoon_test_checks = 0;
int daemoon_test_quiet = 0;
const char *daemoon_test_current = "";
