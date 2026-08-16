/* Turning a camera frame into something the GPU can draw.
 *
 * The first attempt handed this to GX_DisplayTransfer, which converts linear to
 * tiled on the GPU and is what most homebrew does. It produced a screen of noise,
 * and the reason it is not worth chasing is that there was no way to tell which of
 * three guesses was wrong - the buffer dimensions, the orientation, or the
 * subtexture - because the only way to look at the result was to photograph a
 * console.
 *
 * So it is done here instead, in the same 8x8 Morton tiling the nds icons already
 * use, which has a desktop test that checks specific pixels against a table
 * written out by hand. A swizzle that is wrong does not fail; it draws noise, and
 * noise on a console is a photograph and a guess. This one can be wrong on a
 * desktop, where that costs a line of output.
 *
 * The rotation is the other half. A 3DS framebuffer is stored a column at a time
 * starting from the bottom left, and the camera hands back a frame in exactly that
 * layout - which is why a naive copy looks like a picture sliced into strips. The
 * mapping from screen position to buffer index is written out once here rather
 * than being folded into an API call's argument order.
 */
#include "daemoon_3ds.h"

#include <string.h>

/* Position of (x, y) inside one 8x8 tile of a 3DS texture: the bits of x and y
 * interleaved. The one piece of this that cannot be worked out from the result. */
static unsigned morton8(unsigned x, unsigned y)
{
    return (x & 1u) | ((y & 1u) << 1) | ((x & 2u) << 1) | ((y & 2u) << 2) |
           ((x & 4u) << 2) | ((y & 4u) << 3);
}

size_t daemoon_3ds_tile_index(unsigned x, unsigned y, unsigned tex_w)
{
    unsigned tiles_across = tex_w / 8u;
    unsigned tile = (y / 8u) * tiles_across + (x / 8u);

    return (size_t)tile * 64u + morton8(x % 8u, y % 8u);
}

/* Where the camera keeps the pixel that appears at screen (x, y).
 *
 * Columns, bottom to top: index = x * height + (height - 1 - y). Getting this
 * backwards is what turns a preview into diagonal stripes, and it is the same
 * layout the 3DS framebuffer uses - the camera is designed to be copied straight
 * onto the screen.
 */
size_t daemoon_3ds_cam_index(unsigned x, unsigned y, unsigned cam_h)
{
    return (size_t)x * (size_t)cam_h + (size_t)(cam_h - 1u - y);
}

void daemoon_3ds_cam_to_tiled(const unsigned short *frame, unsigned cam_w,
                              unsigned cam_h, unsigned short *tex, unsigned tex_w,
                              unsigned tex_h)
{
    unsigned x;
    unsigned y;

    memset(tex, 0, (size_t)tex_w * (size_t)tex_h * sizeof(*tex));
    for (y = 0; y < cam_h && y < tex_h; ++y) {
        for (x = 0; x < cam_w && x < tex_w; ++x) {
            tex[daemoon_3ds_tile_index(x, y, tex_w)] =
                frame[daemoon_3ds_cam_index(x, y, cam_h)];
        }
    }
}
