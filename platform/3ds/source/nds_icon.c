/* The icon a DS cartridge carries, out of its banner.
 *
 * The 3DS titles in the grid have had icons since the SMDH read was fixed, and
 * the nds ones next to them were flat tiles - which is the half of the list where
 * the names are least useful, because a `.sav` beside a ROM called
 * "5684(Dsi0143) 포켓몬화이트 (K)" tells nobody anything at a glance.
 *
 * A DS ROM points at a banner from offset 0x68 of its header. The banner holds a
 * 32x32 icon as sixteen 8x8 tiles of 4 bit indices, then a sixteen entry BGR555
 * palette. None of that is the layout a 3DS texture wants, so this converts, and
 * writes into the same 48x48 buffer the SMDH path produces: the icon goes in the
 * middle and the upload code stays one function rather than two.
 *
 * All of it is ordinary file reading and bit shuffling, so it is tested on a
 * desktop against a ROM built by the test - which matters more here than usual,
 * because a swizzle that is wrong does not fail, it just draws something that
 * looks like noise, and noise on a console is a photograph and a guess.
 */
#include "daemoon_3ds.h"

#include <daemoon/util/strbuf.h>

#include <stdio.h>
#include <string.h>

/* Offsets in the DS cartridge header and in the banner it points at. */
#define NDS_BANNER_PTR_OFF 0x068
#define NDS_BANNER_BITMAP  0x020 /* 16 tiles * 32 bytes */
#define NDS_BANNER_PALETTE 0x220 /* 16 entries * 2 bytes */
#define NDS_BANNER_BYTES   0x240

#define NDS_ICON_DIM 32

/* Where a 32x32 icon sits inside the 48x48 the 3DS path uses. */
#define ICON_INSET ((DAEMOON_3DS_SMDH_ICON_DIM - NDS_ICON_DIM) / 2)

/* Index 0 of a DS palette is transparent, and the texture this ends up in is
 * RGB565 with nowhere to say so. The grid draws a flat panel tile where a title
 * has no icon at all, so transparent pixels become that same colour and an icon
 * with a cut out corner still reads as one tile rather than a hole. Kept in step
 * with GFX_PANEL by hand: this file must not pull in citro2d, because the tests
 * that make the swizzle worth trusting run without a GPU. */
#define PANEL_R5 (0x24 >> 3)
#define PANEL_G6 (0x28 >> 2)
#define PANEL_B5 (0x32 >> 3)
#define PANEL_RGB565 \
    (unsigned short)((PANEL_R5 << 11) | (PANEL_G6 << 5) | PANEL_B5)

/* Position of (x, y) inside one 8x8 tile of a 3DS texture.
 *
 * The GPU stores a tile in Morton order - the bits of x and y interleaved - which
 * is the one piece of this that cannot be worked out by looking at the result. */
static unsigned morton8(unsigned x, unsigned y)
{
    return (x & 1u) | ((y & 1u) << 1) | ((x & 2u) << 1) | ((y & 2u) << 2) |
           ((x & 4u) << 2) | ((y & 4u) << 3);
}

/* Index into a 48 wide, 8x8 tiled image, tiles in row major order. The same
 * layout an SMDH stores its icon in, which is why the result can go through the
 * same upload. */
static size_t tiled_index(unsigned x, unsigned y)
{
    unsigned tiles_across = DAEMOON_3DS_SMDH_ICON_DIM / 8u;
    unsigned tile = (y / 8u) * tiles_across + (x / 8u);

    return (size_t)tile * 64u + morton8(x % 8u, y % 8u);
}

static unsigned short bgr555_to_rgb565(unsigned short v)
{
    unsigned r = v & 0x1fu;
    unsigned g = (v >> 5) & 0x1fu;
    unsigned b = (v >> 10) & 0x1fu;

    /* Five bits of green into six: repeat the top bit rather than shifting in a
     * zero, so full green stays full rather than landing one short. */
    unsigned g6 = (g << 1) | (g >> 4);

    return (unsigned short)((r << 11) | (g6 << 5) | b);
}

static unsigned short le16(const unsigned char *p)
{
    return (unsigned short)((unsigned)p[0] | ((unsigned)p[1] << 8));
}

static unsigned long le32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

/* One pixel of the DS bitmap: sixteen 8x8 tiles in row major order, four bits per
 * pixel, low nibble first, rows within a tile laid out one after another. */
static unsigned nds_pixel(const unsigned char *bitmap, unsigned x, unsigned y)
{
    unsigned tile = (y / 8u) * (NDS_ICON_DIM / 8u) + (x / 8u);
    unsigned within = (y % 8u) * 8u + (x % 8u);
    unsigned char byte = bitmap[tile * 32u + within / 2u];

    return (within & 1u) ? (unsigned)(byte >> 4) : (unsigned)(byte & 0x0fu);
}

daemoon_result_t daemoon_3ds_nds_icon_read(const char *rom_dir, const char *base,
                                           void *out)
{
    char path[DAEMOON_PATH_MAX * 2];
    daemoon_strbuf_t sb;
    unsigned char header[NDS_BANNER_PTR_OFF + 4];
    unsigned char banner[NDS_BANNER_BYTES];
    unsigned short palette[16];
    unsigned short *dst = (unsigned short *)out;
    unsigned long banner_off;
    FILE *fp;
    size_t i;
    unsigned x;
    unsigned y;

    daemoon_strbuf_init(&sb, path, sizeof(path));
    daemoon_strbuf_add(&sb, rom_dir);
    daemoon_strbuf_addc(&sb, '/');
    daemoon_strbuf_add(&sb, base);
    daemoon_strbuf_add(&sb, ".nds");
    DAEMOON_TRY(daemoon_strbuf_result(&sb));

    fp = fopen(path, "rb");
    if (fp == NULL) {
        return DAEMOON_ERR_NOT_FOUND;
    }
    if (fread(header, 1, sizeof(header), fp) != sizeof(header)) {
        (void)fclose(fp);
        return DAEMOON_ERR_NOT_FOUND;
    }

    banner_off = le32(header + NDS_BANNER_PTR_OFF);
    /* Zero means the cartridge has no banner, which is normal for homebrew. A
     * seek past the end of a truncated ROM is caught by the read below. */
    if (banner_off == 0) {
        (void)fclose(fp);
        return DAEMOON_ERR_NOT_FOUND;
    }
    if (fseek(fp, (long)banner_off, SEEK_SET) != 0 ||
        fread(banner, 1, sizeof(banner), fp) != sizeof(banner)) {
        (void)fclose(fp);
        return DAEMOON_ERR_NOT_FOUND;
    }
    (void)fclose(fp);

    for (i = 0; i < 16; ++i) {
        palette[i] = bgr555_to_rgb565(le16(banner + NDS_BANNER_PALETTE + i * 2));
    }

    /* The whole 48x48 first, so the border around a 32x32 icon is the same flat
     * tile the grid draws for a title that has no icon at all. */
    for (i = 0; i < (size_t)DAEMOON_3DS_ICON_BYTES / 2u; ++i) {
        dst[i] = PANEL_RGB565;
    }

    for (y = 0; y < NDS_ICON_DIM; ++y) {
        for (x = 0; x < NDS_ICON_DIM; ++x) {
            unsigned idx = nds_pixel(banner + NDS_BANNER_BITMAP, x, y);

            dst[tiled_index(x + ICON_INSET, y + ICON_INSET)] =
                idx == 0 ? PANEL_RGB565 : palette[idx];
        }
    }
    return DAEMOON_OK;
}
