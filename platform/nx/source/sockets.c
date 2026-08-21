/* Sockets on the Switch: already up before main runs.
 *
 * This used to be `socketInitializeDefault()`, which is what the 3DS side does two
 * lines away in `platform/common/net_curl.c`. It cannot be that any more: borealis
 * supplies this platform's entry point (`switch_wrapper.c`), and its `userAppInit`
 * brings the socket driver up before main is entered - with a larger configuration
 * than the default, because the interface it draws wants sessions of its own.
 *
 * Initialising it a second time does not add anything; it fails, and the failure
 * would travel up through daemoon_net_curl_init as "no network" on a console whose
 * network is fine. So this reports what is true - sockets are up - and the exit is
 * left to the wrapper that opened them. Whoever opens a thing closes it.
 */
#include "daemoon_newlib.h"

daemoon_result_t daemoon_net_sockets_init(void)
{
    return DAEMOON_OK;
}

void daemoon_net_sockets_exit(void)
{
}
