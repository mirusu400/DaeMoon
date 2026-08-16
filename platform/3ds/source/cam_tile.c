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

/* Where the camera keeps the pixel that appears at (x, y).
 *
 * Rows, left to right, top to bottom - the ordinary thing. It is written down here
 * because the first two attempts assumed otherwise, on the strength of the camera
 * being "in framebuffer order", and both drew a scrambled screen.
 *
 * The evidence that settles it is not a document. quirc is handed this buffer as a
 * 400 wide image and decodes real codes from it. If the rows were really columns,
 * reading them as 400 wide rows would not rotate the picture - it would shear it,
 * one pixel further along on every line - and no QR code survives that. It
 * decoded, so the layout is rows of 400.
 */
size_t daemoon_3ds_cam_index(unsigned x, unsigned y, unsigned cam_w)
{
    return (size_t)y * (size_t)cam_w + (size_t)x;
}

/* The same question, asked on the console instead of guessed at.
 *
 * I have been wrong about this twice, and both times the answer arrived as "still
 * broken" - which is one bit of information for one trip across the room. The
 * preview can cycle through the four layouts a sensor could plausibly use, so a
 * person looking at the screen picks the one that is a picture. That converts an
 * argument into a fact in a single run.
 *
 * Layout 0 is what the evidence says and what ships as the default. The rest exist
 * to be ruled out.
 */
size_t daemoon_3ds_cam_index_as(unsigned x, unsigned y, unsigned cam_w,
                                unsigned cam_h, int layout)
{
    switch (layout) {
    case 1: /* columns, bottom left first - the 3DS framebuffer's own order */
        return (size_t)x * (size_t)cam_h + (size_t)(cam_h - 1u - y);
    case 2: /* columns, top left first */
        return (size_t)x * (size_t)cam_h + (size_t)y;
    case 3: /* rows, bottom up */
        return (size_t)(cam_h - 1u - y) * (size_t)cam_w + (size_t)x;
    default:
        return (size_t)y * (size_t)cam_w + (size_t)x;
    }
}

void daemoon_3ds_cam_to_tiled(const unsigned short *frame, unsigned cam_w,
                              unsigned cam_h, unsigned short *tex, unsigned tex_w,
                              unsigned tex_h, int layout)
{
    unsigned x;
    unsigned y;

    memset(tex, 0, (size_t)tex_w * (size_t)tex_h * sizeof(*tex));
    for (y = 0; y < cam_h && y < tex_h; ++y) {
        for (x = 0; x < cam_w && x < tex_w; ++x) {
            tex[daemoon_3ds_tile_index(x, y, tex_w)] =
                frame[daemoon_3ds_cam_index_as(x, y, cam_w, cam_h, layout)];
        }
    }
}
