#!/usr/bin/env python3
"""Refuse a banner the HOME Menu will not draw.

A .bnr can be structurally perfect and still show nothing. One did: a CBMD with
a valid LZ11 stream, a valid CGFX inside it, a model correctly named COMMON, and
a valid CWAV beside it. The console drew an empty top screen and there was no
error anywhere to explain it.

What it was missing is what this checks. The banner the HOME Menu renders is a
textured quad: a model plus a texture object, with the material sampling that
texture. The file that failed was a lit 3D scene export instead - five meshes,
five colour-only materials, a light and a LUT set, and not one texture object.
Nothing in the container format says that is wrong, so nothing rejected it.

Reading it back here turns that into a build failure rather than a photograph of
a blank screen.

    tools/check-banner.py platform/3ds/assets/banner.bnr
"""
import struct
import sys


def lz11(data):
    """Nintendo LZ11, which is how a CBMD stores its CGFX."""
    if not data or data[0] != 0x11:
        raise ValueError(f"not an LZ11 stream (first byte {data[0]:#x})")
    size = data[1] | (data[2] << 8) | (data[3] << 16)
    i, out = 4, bytearray()
    while len(out) < size:
        flags = data[i]
        i += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if not flags & (0x80 >> bit):
                out.append(data[i])
                i += 1
                continue
            b = data[i]
            i += 1
            kind = b >> 4
            if kind == 0:
                count = (b & 0xF) << 4
                b2 = data[i]
                i += 1
                count = (count | b2 >> 4) + 0x11
                disp = (b2 & 0xF) << 8 | data[i]
                i += 1
            elif kind == 1:
                count = (b & 0xF) << 12
                b2, b3 = data[i], data[i + 1]
                i += 2
                count = (count | b2 << 4 | b3 >> 4) + 0x111
                disp = (b3 & 0xF) << 8 | data[i]
                i += 1
            else:
                count = kind + 1
                disp = (b & 0xF) << 8 | data[i]
                i += 1
            start = len(out) - disp - 1
            for k in range(count):
                out.append(out[start + k])
    return bytes(out[:size])


def dict_names(cgfx, data_off, slot):
    """The names in one of the DATA block's dictionaries, or []."""
    field = data_off + 8 + slot * 8
    count, rel = struct.unpack_from("<Ii", cgfx, field)
    if count == 0 or count > 64:
        return []
    off = field + 4 + rel
    if cgfx[off:off + 4] != b"DICT":
        return []
    names = []
    for i in range(struct.unpack_from("<I", cgfx, off + 8)[0]):
        entry = off + 0x0C + 0x10 + i * 0x10
        name_rel = struct.unpack_from("<i", cgfx, entry + 8)[0]
        at = entry + 8 + name_rel
        names.append(cgfx[at:cgfx.index(b"\0", at)].decode("ascii", "replace"))
    return names


# The order of the dictionaries in a CGFX DATA block. Only the first two matter
# here; the rest are named so a failure can say what the file has instead.
SLOTS = ["models", "textures", "luts", "materials", "shaders", "cameras",
         "lights", "fogs", "scenes", "skeletons", "skeletalAnims",
         "materialAnims", "visibilityAnims", "cameraAnims", "lightAnims"]


def check(path):
    raw = open(path, "rb").read()
    if raw[:4] != b"CBMD":
        raise ValueError(f"not a CBMD banner (magic {raw[:4]!r})")

    cgfx_off, cwav_off = struct.unpack_from("<I", raw, 0x08)[0], struct.unpack_from("<I", raw, 0x84)[0]
    if not cgfx_off or not cwav_off:
        raise ValueError("CBMD names no common banner or no audio")
    if raw[cwav_off:cwav_off + 4] != b"CWAV":
        raise ValueError("the audio the CBMD points at is not a CWAV")

    cgfx = lz11(raw[cgfx_off:cwav_off])
    if cgfx[:4] != b"CGFX":
        raise ValueError(f"the compressed banner is not a CGFX (magic {cgfx[:4]!r})")

    data_off = struct.unpack_from("<H", cgfx, 0x06)[0]
    if cgfx[data_off:data_off + 4] != b"DATA":
        raise ValueError("the CGFX has no DATA block")

    found = {name: dict_names(cgfx, data_off, i) for i, name in enumerate(SLOTS)}
    have = {k: v for k, v in found.items() if v}

    # The HOME Menu draws a model called COMMON. Anything else is not the banner.
    if "COMMON" not in found["models"]:
        raise ValueError(f"no model named COMMON; models are {found['models']}")

    # And it draws it textured. This is the one that a lit scene export passes
    # every other check and fails.
    if not found["textures"]:
        raise ValueError(
            "the banner model has no texture object, so the HOME Menu draws "
            f"nothing. What the file has instead: {have}")

    print(f"banner: ok ({path})")
    for k, v in have.items():
        print(f"  {k}: {v}")
    for note in texture_notes(cgfx, data_off):
        print(f"  {note}")
    for note in model_notes(cgfx, data_off):
        print(f"  {note}")


def model_notes(cgfx, data_off):
    """What the model says about itself, beside what a working banner says.

    Not failures. These are the differences left between a banner that draws and
    one that does not, written down where the next person looking at a blank top
    screen will find them.
    """
    field = data_off + 8
    count, rel = struct.unpack_from("<Ii", cgfx, field)
    if not count:
        return []
    entry = field + 4 + rel + 0x0C + 0x10
    obj = entry + 0x0C + struct.unpack_from("<i", cgfx, entry + 0x0C)[0]
    if cgfx[obj + 4:obj + 8] != b"CMDL":
        return []

    notes = []
    # +0x28 is how many animation groups the model declares and +0x2c points at
    # them. bannertool writes three - SkeletalAnimation, VisibilityAnimation and
    # MaterialAnimation - even for a banner that never moves.
    groups = struct.unpack_from("<I", cgfx, obj + 0x28)[0]
    if not groups:
        notes.append("note: the model declares no animation groups; a banner "
                     "from bannertool declares SkeletalAnimation, "
                     "VisibilityAnimation and MaterialAnimation even when static")
    return notes


# Bytes per texel for the PICA formats a banner texture is likely to use. The
# format the hardware is told about lives in the texture object; the GL pair
# beside it is a mirror of the same thing for tools.
BYTES_PER_TEXEL = {0: 4, 1: 3, 2: 2, 3: 2, 4: 2, 5: 2, 6: 1, 7: 1, 8: 1}
PICA_FORMAT = {0: "RGBA8", 1: "RGB8", 2: "RGBA5551", 3: "RGB565", 4: "RGBA4444",
               5: "LA8", 6: "HILO8", 7: "L8", 8: "A8"}


def texture_notes(cgfx, data_off):
    """Look at the texture the banner will actually sample.

    A texture object can be present and still describe nothing usable, so the
    parts that decide whether a sampler can read it are checked here: the size,
    that the dimensions are powers of two, and that the image really holds
    width x height texels of the format it claims.
    """
    field = data_off + 8 + 1 * 8
    count, rel = struct.unpack_from("<Ii", cgfx, field)
    if not count:
        return []
    dic = field + 4 + rel
    entry = dic + 0x0C + 0x10
    obj = entry + 0x0C + struct.unpack_from("<i", cgfx, entry + 0x0C)[0]
    if cgfx[obj + 4:obj + 8] != b"TXOB":
        raise ValueError("the textures dictionary does not point at a TXOB")

    height, width = struct.unpack_from("<II", cgfx, obj + 0x18)
    gl_format, gl_type = struct.unpack_from("<II", cgfx, obj + 0x20)
    hw_format = struct.unpack_from("<I", cgfx, obj + 0x34)[0]
    image_bytes = struct.unpack_from("<I", cgfx, obj + 0x44)[0]

    if not width or not height:
        raise ValueError(f"the banner texture is {width}x{height}")
    for side, name in ((width, "width"), (height, "height")):
        if side & (side - 1):
            raise ValueError(f"texture {name} {side} is not a power of two, "
                             "which the GPU requires")
    if not image_bytes:
        raise ValueError("the banner texture declares no image data")

    bpp = BYTES_PER_TEXEL.get(hw_format)
    if bpp and image_bytes != width * height * bpp:
        raise ValueError(
            f"the texture says {width}x{height} in "
            f"{PICA_FORMAT.get(hw_format, hw_format)} but carries {image_bytes} "
            f"bytes, not {width * height * bpp}")

    notes = [f"texture: {width}x{height} "
             f"{PICA_FORMAT.get(hw_format, hw_format)}, {image_bytes} bytes"]
    # Not fatal on its own - the hardware format above is the field that decides
    # what the sampler reads - but a working banner from bannertool fills both,
    # and an exporter that leaves these at zero is worth knowing about before a
    # console is the thing that tells you.
    if not gl_format or not gl_type:
        notes.append(f"note: glFormat={gl_format:#x} glType={gl_type:#x} are unset; "
                     "bannertool writes 0x6752 / 0x8033 for RGBA4444")
    return notes


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(f"usage: {sys.argv[0]} <banner.bnr>")
    try:
        check(sys.argv[1])
    except Exception as err:
        sys.exit(f"{sys.argv[1]}: {err}")
