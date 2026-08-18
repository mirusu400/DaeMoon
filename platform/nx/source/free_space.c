/* Free space on the SD card, the Switch answer.
 *
 * The rest of the card side is shared with the 3DS in platform/common; this is the one
 * call in it that is a service rather than stdio.
 */
#include "daemoon_newlib.h"

#include <switch.h>

unsigned long long daemoon_newlib_free_bytes(void)
{
    FsFileSystem *sd = fsdevGetDeviceFileSystem("sdmc");
    s64 free_bytes = 0;

    if (sd == NULL) {
        return 0;
    }
    if (R_FAILED(fsFsGetFreeSpace(sd, "/", &free_bytes)) || free_bytes < 0) {
        return 0;
    }
    return (unsigned long long)free_bytes;
}
