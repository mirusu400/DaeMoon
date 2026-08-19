#!/usr/bin/env python3
"""Fill in the pixel format a banner texture forgot to declare.

The banner model came back textured, correctly shaped, with the right number of
bytes behind a 64x64 RGBA4444 texture - and the HOME Menu still drew nothing.
Read against a banner known to work, three fields in the texture object were the
only things left set to zero:

    +0x20 glFormat   0x6752   GL_RGBA_NATIVE_DMP
    +0x24 glType     0x8033   GL_UNSIGNED_SHORT_4_4_4_4
    +0x50 bpp        16       bits per texel

The hardware format beside them says 4, which is RGBA4444, so the file is not
self contradictory - it is under-specified, and the loader appears to want the
GL pair. This writes them.

The CGFX lives LZ11 compressed inside the CBMD, so this decompresses it, patches
the three fields, compresses it again and rebuilds the container. The compressor
here emits literals only: it is the shape of an LZ11 stream without the search,
which any decompressor reads, and a banner being 250 KB rather than 20 KB costs
nothing that matters. Every write is read back before the file is replaced.

    tools/fix-banner-texture.py platform/3ds/assets/banner.bnr
"""
import pathlib
import struct
import sys

GL_FORMAT, GL_TYPE, BITS = 0x6752, 0x8033, 16
OFF_GL_FORMAT, OFF_GL_TYPE, OFF_BITS = 0x20, 0x24, 0x50


def lz11_decompress(data):
    if not data or data[0] != 0x11:
        raise ValueError("not an LZ11 stream")
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


def lz11_store(raw):
    """An LZ11 stream of nothing but literals."""
    if len(raw) >= 1 << 24:
        raise ValueError("too large for an LZ11 header")
    out = bytearray(struct.pack("<I", (len(raw) << 8) | 0x11))
    for i in range(0, len(raw), 8):
        out.append(0x00)                 # eight literals
        out += raw[i:i + 8]
    return bytes(out)


def texture_object(cgfx):
    """Where the one texture the banner samples is described."""
    data_off = struct.unpack_from("<H", cgfx, 0x06)[0]
    field = data_off + 8 + 1 * 8         # slot 1 is the textures dictionary
    count, rel = struct.unpack_from("<Ii", cgfx, field)
    if not count:
        raise ValueError("the banner has no texture to fix")
    entry = field + 4 + rel + 0x0C + 0x10
    obj = entry + 0x0C + struct.unpack_from("<i", cgfx, entry + 0x0C)[0]
    if cgfx[obj + 4:obj + 8] != b"TXOB":
        raise ValueError("the textures dictionary does not point at a TXOB")
    return obj


def main(path):
    path = pathlib.Path(path)
    banner = bytearray(path.read_bytes())
    if banner[:4] != b"CBMD":
        sys.exit(f"{path}: not a CBMD banner")

    cgfx_off = struct.unpack_from("<I", banner, 0x08)[0]
    cwav_off = struct.unpack_from("<I", banner, 0x84)[0]
    cwav = bytes(banner[cwav_off:])
    cgfx = bytearray(lz11_decompress(bytes(banner[cgfx_off:cwav_off])))

    obj = texture_object(cgfx)
    before = struct.unpack_from("<I", cgfx, obj + OFF_GL_FORMAT)[0], \
        struct.unpack_from("<I", cgfx, obj + OFF_GL_TYPE)[0], \
        struct.unpack_from("<I", cgfx, obj + OFF_BITS)[0]
    if all(before):
        print(f"{path}: the texture already declares its format {before}; nothing to do")
        return

    struct.pack_into("<I", cgfx, obj + OFF_GL_FORMAT, GL_FORMAT)
    struct.pack_into("<I", cgfx, obj + OFF_GL_TYPE, GL_TYPE)
    struct.pack_into("<I", cgfx, obj + OFF_BITS, BITS)

    packed = lz11_store(bytes(cgfx))
    if lz11_decompress(packed) != bytes(cgfx):
        sys.exit("the compressor did not round trip; refusing to write")

    # The audio starts on a 32 byte boundary, as it did before.
    body = bytearray(packed)
    while (cgfx_off + len(body)) % 32:
        body.append(0)

    out = bytearray(banner[:0x88])
    struct.pack_into("<I", out, 0x08, cgfx_off)
    struct.pack_into("<I", out, 0x84, cgfx_off + len(body))
    out += body
    out += cwav

    # Read the whole thing back the way the console would.
    check = lz11_decompress(bytes(out[struct.unpack_from("<I", out, 0x08)[0]:
                                      struct.unpack_from("<I", out, 0x84)[0]]))
    if check != bytes(cgfx):
        sys.exit("the rebuilt banner does not decompress to what was patched")
    if bytes(out[struct.unpack_from("<I", out, 0x84)[0]:]) != cwav:
        sys.exit("the rebuilt banner lost its audio")

    path.write_bytes(bytes(out))
    print(f"{path}: glFormat/glType/bpp were {before}, now "
          f"({GL_FORMAT:#x}, {GL_TYPE:#x}, {BITS}); {len(banner)} -> {len(out)} bytes")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(f"usage: {sys.argv[0]} <banner.bnr>")
    main(sys.argv[1])
