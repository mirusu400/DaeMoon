/* Game icons, out of the same SMDH the names come from.
 *
 * An icon is how somebody recognises a game in under a second. A product code is
 * not, and neither is a title id, and this application asks people to point at a
 * game and say "overwrite that one's save" - so the picture matters more here
 * than it would in most places.
 *
 * The SMDH's 48x48 icon is already in the GPU's tiled RGB565 layout, so the data
 * goes to a texture with no conversion beyond copying it into a 64x64 one, which
 * is the smallest power of two that holds it.
 */
#include "icons.h"

#include "daemoon_3ds.h"

#include <3ds.h>

#include <string.h>

/* Offsets inside the SMDH, from the top of the file. */
#define SMDH_LARGE_ICON_OFFSET 0x24C0u
#define SMDH_LARGE_ICON_DIM    48
#define SMDH_LARGE_ICON_BYTES  (SMDH_LARGE_ICON_DIM * SMDH_LARGE_ICON_DIM * 2)

#define ICON_TEX_DIM 64

/* The subtexture that maps the 48x48 corner of a 64x64 texture. The v
 * coordinates are flipped because the icon is copied into the bottom of the
 * texture, which is where a 3DS texture's first row lives. */
static const Tex3DS_SubTexture k_subtexture = {
    SMDH_LARGE_ICON_DIM, SMDH_LARGE_ICON_DIM, 0.0f, 1.0f,
    (float)SMDH_LARGE_ICON_DIM / (float)ICON_TEX_DIM,
    1.0f - (float)SMDH_LARGE_ICON_DIM / (float)ICON_TEX_DIM
};

daemoon_result_t daemoon_3ds_icon_load(int media, unsigned long long title_id,
                                       daemoon_3ds_icon_t *out)
{
    u32 archive_path[4];
    static const u32 file_path[5] = {
        0x00000000u, 0x00000000u, 0x00000002u, 0x6E6F6369u /* "icon" */, 0x00000000u
    };
    FS_Path archive;
    FS_Path file;
    Handle handle = 0;
    static u8 pixels[SMDH_LARGE_ICON_BYTES];
    C3D_Tex *tex;
    u32 got = 0;
    Result res;
    int row;

    memset(out, 0, sizeof(*out));

    archive_path[0] = (u32)(title_id & 0xffffffffull);
    archive_path[1] = (u32)(title_id >> 32);
    archive_path[2] = (u32)media;
    archive_path[3] = 0;

    archive.type = PATH_BINARY;
    archive.size = sizeof(archive_path);
    archive.data = archive_path;

    file.type = PATH_BINARY;
    file.size = sizeof(file_path);
    file.data = file_path;

    res = FSUSER_OpenFileDirectly(&handle, ARCHIVE_SAVEDATA_AND_CONTENT, archive, file,
                                  FS_OPEN_READ, 0);
    if (R_FAILED(res)) {
        return DAEMOON_ERR_NOT_FOUND;
    }

    res = FSFILE_Read(handle, &got, SMDH_LARGE_ICON_OFFSET, pixels, sizeof(pixels));
    (void)FSFILE_Close(handle);
    if (R_FAILED(res) || got != sizeof(pixels)) {
        return DAEMOON_ERR_NOT_FOUND;
    }

    tex = (C3D_Tex *)linearAlloc(sizeof(*tex));
    if (tex == NULL) {
        return DAEMOON_ERR_OUT_OF_MEMORY;
    }
    if (!C3D_TexInit(tex, ICON_TEX_DIM, ICON_TEX_DIM, GPU_RGB565)) {
        linearFree(tex);
        return DAEMOON_ERR_OUT_OF_MEMORY;
    }
    memset(tex->data, 0, tex->size);

    /* Both the SMDH icon and a 3DS texture store pixels in 8x8 tiles, so this is a
     * copy rather than a conversion - but the rows have different strides, 48
     * against 64, so it goes one band of eight rows at a time. */
    {
        u16 *dst = (u16 *)tex->data;
        const u16 *src = (const u16 *)(const void *)pixels;

        for (row = 0; row < SMDH_LARGE_ICON_DIM; row += 8) {
            memcpy(dst, src, (size_t)SMDH_LARGE_ICON_DIM * 8 * sizeof(u16));
            src += SMDH_LARGE_ICON_DIM * 8;
            dst += ICON_TEX_DIM * 8;
        }
    }

    C3D_TexSetFilter(tex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    out->image.tex = tex;
    out->image.subtex = &k_subtexture;
    out->loaded = 1;
    return DAEMOON_OK;
}

void daemoon_3ds_icon_free(daemoon_3ds_icon_t *icon)
{
    if (!icon->loaded) {
        return;
    }
    C3D_TexDelete((C3D_Tex *)icon->image.tex);
    linearFree((void *)icon->image.tex);
    memset(icon, 0, sizeof(*icon));
}
