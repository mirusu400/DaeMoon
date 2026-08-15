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

/* Reads a title's 48x48 icon out of its SMDH. not_found when the title has none,
 * which is normal and is drawn as a placeholder rather than a gap. */
daemoon_result_t daemoon_3ds_icon_load(int media, unsigned long long title_id,
                                       daemoon_3ds_icon_t *out);
void daemoon_3ds_icon_free(daemoon_3ds_icon_t *icon);

#endif /* DAEMOON_3DS_ICONS_H */
