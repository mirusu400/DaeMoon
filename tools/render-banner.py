#!/usr/bin/env python3
"""Draw platform/3ds/assets/banner.png from the banner's 3D model.

The model is here because the 3DS could not use it directly. A HOME Menu banner
is a textured quad - one model named COMMON, one texture, a material that
samples it - and this model is a lit scene export with no texture and no UVs at
all, so the console drew an empty top screen. See tools/check-banner.py.

What the model is good for is the picture. This rasterises it once, at build
time on a desktop, and the result goes into the banner as an image. It needs
numpy and pillow, and it is not part of any build: the PNG it writes is
committed, so nobody needs either to build a CIA.

    tools/render-banner.py

The top of the model is a blocky 3D "DaeMoon" that is unreadable at the 256x128
a banner gets, so it is left out and the name is set in type instead.
"""
import base64
import json
import pathlib
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = pathlib.Path(__file__).resolve().parent.parent
MODEL = ROOT / "platform/3ds/assets/banner-model.gltf"
OUT = ROOT / "platform/3ds/assets/banner.png"

W, H = 256, 128          # what a 3DS banner is
SS = 4                   # supersampling, then averaged down
TOP, BOTTOM = (0x0B, 0x1A, 0x46), (0x05, 0x0B, 0x22)

COMPONENT = {5120: "i1", 5121: "u1", 5122: "i2", 5123: "u2", 5125: "u4", 5126: "f4"}
COUNT = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}


def load(path):
    g = json.loads(pathlib.Path(path).read_text())
    buf = base64.b64decode(g["buffers"][0]["uri"].split(",", 1)[1])

    def accessor(i):
        a = g["accessors"][i]
        view = g["bufferViews"][a["bufferView"]]
        off = view.get("byteOffset", 0) + a.get("byteOffset", 0)
        n = a["count"] * COUNT[a["type"]]
        raw = np.frombuffer(buf, dtype=np.dtype("<" + COMPONENT[a["componentType"]]),
                            count=n, offset=off)
        return raw.astype(np.float64).reshape(a["count"], COUNT[a["type"]])

    # The model colours itself from a stripe atlas: every material samples one
    # band of a 64x64 texture. One lookup per mesh is enough here, because a mesh
    # and a colour are the same thing in this model - and it keeps this a
    # rasteriser rather than a texture unit.
    atlas = None
    if g.get("images"):
        import io
        uri = g["images"][0]["uri"]
        atlas = np.asarray(Image.open(io.BytesIO(
            base64.b64decode(uri.split(",", 1)[1]))).convert("RGB")) / 255.0

    parts = []
    for node in g["nodes"]:
        for prim in g["meshes"][node["mesh"]]["primitives"]:
            pos = accessor(prim["attributes"]["POSITION"])
            nrm = accessor(prim["attributes"]["NORMAL"])
            # No index buffer in this file: the primitives are plain triangle lists.
            idx = (accessor(prim["indices"]).ravel().astype(int)
                   if "indices" in prim else np.arange(len(pos)))
            pbr = g["materials"][prim["material"]]["pbrMetallicRoughness"]
            if atlas is not None and "baseColorTexture" in pbr:
                uv = accessor(prim["attributes"]["TEXCOORD_0"]).mean(axis=0)
                h, w = atlas.shape[:2]
                colour = atlas[min(int(uv[1] * h), h - 1), min(int(uv[0] * w), w - 1)]
            else:
                colour = np.array(pbr["baseColorFactor"][:3])
            parts.append((pos, nrm, idx.reshape(-1, 3), colour))
    return parts


def render(parts, width, height, yaw, pitch, distance):
    """A z-buffered rasteriser. Flat colours and normals is all the model has."""
    everything = np.vstack([p[0] for p in parts])
    centre = (everything.min(0) + everything.max(0)) / 2
    radius = np.linalg.norm(everything.max(0) - everything.min(0)) / 2

    cy, sy = np.cos(yaw), np.sin(yaw)
    cp, sp = np.cos(pitch), np.sin(pitch)
    rot = (np.array([[1, 0, 0], [0, cp, -sp], [0, sp, cp]])
           @ np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]]))

    eye = radius * distance
    focal = 1 / np.tan(np.deg2rad(30) / 2)
    aspect = width / height
    light = rot @ np.array([0.35, 0.75, 0.9])
    light /= np.linalg.norm(light)

    rgb = np.zeros((height, width, 3))
    alpha = np.zeros((height, width))
    depth = np.full((height, width), np.inf)

    for pos, nrm, tris, colour in parts:
        v = (pos - centre) @ rot.T
        n = nrm @ rot.T
        z = eye - v[:, 2]
        with np.errstate(divide="ignore", invalid="ignore"):
            px = ((v[:, 0] * focal / aspect) / z + 1) * 0.5 * width
            py = (1 - ((v[:, 1] * focal) / z + 1) * 0.5) * height

        for a, b, c in tris:
            if min(z[a], z[b], z[c]) <= 0:
                continue
            x0, y0, x1, y1, x2, y2 = px[a], py[a], px[b], py[b], px[c], py[c]
            area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
            if area >= 0:                       # back facing
                continue
            lo_x = max(int(np.floor(min(x0, x1, x2))), 0)
            hi_x = min(int(np.ceil(max(x0, x1, x2))), width - 1)
            lo_y = max(int(np.floor(min(y0, y1, y2))), 0)
            hi_y = min(int(np.ceil(max(y0, y1, y2))), height - 1)
            if lo_x > hi_x or lo_y > hi_y:
                continue

            ys, xs = np.mgrid[lo_y:hi_y + 1, lo_x:hi_x + 1]
            xs = xs + 0.5
            ys = ys + 0.5
            e0 = (x1 - x0) * (ys - y0) - (y1 - y0) * (xs - x0)
            e1 = (x2 - x1) * (ys - y1) - (y2 - y1) * (xs - x1)
            e2 = (x0 - x2) * (ys - y2) - (y0 - y2) * (xs - x2)
            inside = (e0 <= 0) & (e1 <= 0) & (e2 <= 0)
            if not inside.any():
                continue

            b0, b1, b2 = e1 / area, e2 / area, e0 / area
            zz = 1 / np.clip(b0 / z[a] + b1 / z[b] + b2 / z[c], 1e-9, None)
            nn = b0[..., None] * n[a] + b1[..., None] * n[b] + b2[..., None] * n[c]
            nn /= np.clip(np.linalg.norm(nn, axis=-1, keepdims=True), 1e-9, None)
            lambert = np.clip(nn @ light, 0, 1)
            rim = np.clip(1 - np.abs(nn[..., 2]), 0, 1) ** 3
            shaded = (colour * (0.30 + 0.75 * lambert)[..., None]
                      + rim[..., None] * 0.28 * np.array([0.35, 0.65, 1.0]))

            win = inside & (zz < depth[lo_y:hi_y + 1, lo_x:hi_x + 1])
            rgb[lo_y:hi_y + 1, lo_x:hi_x + 1][win] = np.clip(shaded, 0, 1)[win]
            alpha[lo_y:hi_y + 1, lo_x:hi_x + 1][win] = 1.0
            depth[lo_y:hi_y + 1, lo_x:hi_x + 1][win] = zz[win]

    return rgb, alpha


def font(bold, size):
    for name in (("DejaVuSans-Bold.ttf", "LiberationSans-Bold.ttf") if bold
                 else ("DejaVuSans.ttf", "LiberationSans-Regular.ttf")):
        for base in ("/usr/share/fonts/truetype/dejavu/",
                     "/usr/share/fonts/truetype/liberation/"):
            try:
                return ImageFont.truetype(base + name, size)
            except OSError:
                continue
    sys.exit("no DejaVu or Liberation font found; install one to redraw the banner")


def main():
    parts = load(MODEL)

    # The model is the whole banner now: gate, moon and wordmark laid out
    # landscape. Nothing is left out and nothing is added - an earlier model was
    # portrait with the name stacked on top, and that one needed both.
    rgb, alpha = render(parts, W * SS, H * SS,
                        np.deg2rad(-12), np.deg2rad(5), distance=2.05)
    rgb = rgb.reshape(H, SS, W, SS, 3).mean(axis=(1, 3))
    alpha = alpha.reshape(H, SS, W, SS).mean(axis=(1, 3))

    banner = Image.new("RGB", (W, H))
    draw = ImageDraw.Draw(banner)
    for y in range(H):                          # the panel's own night gradient
        t = y / (H - 1)
        draw.line([(0, y), (W, y)],
                  fill=tuple(int(round(a + (b - a) * t)) for a, b in zip(TOP, BOTTOM)))

    model = Image.fromarray(
        (np.concatenate([rgb, alpha[..., None]], -1) * 255).astype(np.uint8), "RGBA")
    banner.paste(model, (0, 0), model)

    banner.save(OUT)
    print(f"wrote {OUT.relative_to(ROOT)} ({W}x{H})")


if __name__ == "__main__":
    main()
