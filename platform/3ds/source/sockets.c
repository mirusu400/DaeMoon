/* Sockets on the 3DS: soc:U.
 *
 * The one part of the network backend that is not shared. The curl request loop lives
 * in platform/common because it is identical on both consoles; this is not.
 */
#include "daemoon_newlib.h"

#include <3ds.h>

#include <malloc.h>
#include <stdlib.h>

/* soc:U wants a page aligned buffer it keeps for the lifetime of the session. 128 KiB
 * is what the 3DS examples use and is enough for one connection at a time, which is
 * all this ever opens. */
#define SOC_BUFFER_SIZE  (128 * 1024)
#define SOC_BUFFER_ALIGN 0x1000

static u32 *g_soc_buffer;

daemoon_result_t daemoon_net_sockets_init(void)
{
    Result res;

    g_soc_buffer = (u32 *)memalign(SOC_BUFFER_ALIGN, SOC_BUFFER_SIZE);
    if (g_soc_buffer == NULL) {
        return DAEMOON_ERR_OUT_OF_MEMORY;
    }
    res = socInit(g_soc_buffer, SOC_BUFFER_SIZE);
    if (R_FAILED(res)) {
        free(g_soc_buffer);
        g_soc_buffer = NULL;
        return DAEMOON_ERR_NETWORK_ERROR;
    }
    return DAEMOON_OK;
}

void daemoon_net_sockets_exit(void)
{
    socExit();
    free(g_soc_buffer);
    g_soc_buffer = NULL;
}
