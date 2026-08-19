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


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(f"usage: {sys.argv[0]} <banner.bnr>")
    try:
        check(sys.argv[1])
    except Exception as err:
        sys.exit(f"{sys.argv[1]}: {err}")
