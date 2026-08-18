/* Free space on the SD card, the 3DS answer.
 *
 * The rest of the card side is shared with the Switch in platform/common; this is
 * the one call in it that is a service rather than stdio, so it is the one part that
 * lives here.
 */
#include "daemoon_newlib.h"

#include <3ds.h>

unsigned long long daemoon_newlib_free_bytes(void)
{
    FS_ArchiveResource resource;

    if (R_FAILED(FSUSER_GetArchiveResource(&resource, SYSTEM_MEDIATYPE_SD))) {
        return 0;
    }
    return (unsigned long long)resource.freeClusters *
           (unsigned long long)resource.clusterSize;
}
