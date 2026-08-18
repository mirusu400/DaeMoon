/* Sockets on the Switch: bsd:u through libnx's defaults.
 *
 * The one part of the network backend that is not shared. No aligned buffer to hand
 * over and no size to choose, which is the whole of the difference from the 3DS.
 */
#include "daemoon_newlib.h"

#include <switch.h>

daemoon_result_t daemoon_net_sockets_init(void)
{
    return R_SUCCEEDED(socketInitializeDefault()) ? DAEMOON_OK : DAEMOON_ERR_NETWORK_ERROR;
}

void daemoon_net_sockets_exit(void)
{
    socketExit();
}
