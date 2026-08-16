/* Icons live in their own header so that including them means asking for the GPU.
 *
 * daemoon_3ds.h is included by the save and filesystem backends, which have no
 * business knowing that a graphics API exists.
 */
#ifndef DAEMOON_3DS_ICONS_H
#define DAEMOON_3DS_ICONS_H

#include <daemoon/result.h>

#include <citro2d.h>

typedef struct {
    C2D_Image image;
    int       loaded;
} daemoon_3ds_icon_t;

/* Uploads a 48x48 tiled RGB565 icon - DAEMOON_3DS_ICON_BYTES of it, exactly as an
 * SMDH stores it - to a texture.
 *
 * The pixels are supplied rather than read here on purpose. They come out of the
 * same SMDH the name does, and reading that file twice per title was half of what
 * made the loading screen slow. The caller reads it once and both users are fed
 * from that. */
daemoon_result_t daemoon_3ds_icon_upload(const void *pixels, daemoon_3ds_icon_t *out);
void daemoon_3ds_icon_free(daemoon_3ds_icon_t *icon);

/* The live camera frame while a scan is running, or NULL. Declared here because
 * this is the header that is allowed to know a GPU exists. */
const C2D_Image *daemoon_3ds_qr_preview(void);

#endif /* DAEMOON_3DS_ICONS_H */
