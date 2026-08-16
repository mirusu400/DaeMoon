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
 * Columns, top left first: index = x * height + y. The console said so - the
 * preview was made to cycle through the four plausible layouts and this is the one
 * that is a picture.
 *
 * It took three attempts, and the argument that made the first two look right is
 * worth writing down because it was wrong. quirc was being handed this buffer as a
 * 400 wide image and decoding real codes from it, and I read that as proof the rows
 * were 400 wide: reading columns as rows shears the picture, one pixel further
 * along on every line, and surely no QR code survives that.
 *
 * A QR code survives that. quirc fits a perspective transform from the four
 * corners of the finder pattern, and a shear is affine, which is inside what a
 * perspective transform can undo. So decoding proved the buffer held an image and
 * nothing at all about its shape.
 *
 * **A decoder that corrects for something cannot be used to detect it.**
 */
size_t daemoon_3ds_cam_index(unsigned x, unsigned y, unsigned cam_h)
{
    return (size_t)x * (size_t)cam_h + (size_t)y;
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
/* A name for each candidate, because a number is not one.
 *
 * The layouts were renumbered between two builds and the answer that came back was
 * "layout 2", which by then meant two different things. A diagnostic whose answer
 * depends on which build asked the question is not a diagnostic. */
const char *daemoon_3ds_cam_layout_name(int layout)
{
    switch (layout) {
    case 1:  return "cols/BL";
    case 2:  return "rows/TD";
    case 3:  return "rows/BU";
    default: return "cols/TL";
    }
}

size_t daemoon_3ds_cam_index_as(unsigned x, unsigned y, unsigned cam_w,
                                unsigned cam_h, int layout)
{
    switch (layout) {
    case 1: /* columns, bottom left first - the 3DS framebuffer's own order */
        return (size_t)x * (size_t)cam_h + (size_t)(cam_h - 1u - y);
    case 2: /* rows of the frame width, top down */
        return (size_t)y * (size_t)cam_w + (size_t)x;
    case 3: /* rows, bottom up */
        return (size_t)(cam_h - 1u - y) * (size_t)cam_w + (size_t)x;
    default: /* columns, top left first - what the console says it uses */
        return daemoon_3ds_cam_index(x, y, cam_h);
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
