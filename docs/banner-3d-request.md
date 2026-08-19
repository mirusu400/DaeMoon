# What a DaeMoon 3D banner has to contain

Three `.bnr` files have been produced for this project and none of them drew on
hardware. Each layer of each one was valid, which is why nothing rejected them:
a correct CBMD, a correct LZ11 stream, a correct CGFX, a model correctly named
`COMMON`, and a correct CWAV beside it. The HOME Menu still showed an empty top
screen, with no error anywhere.

This is what was read out of a banner that *does* draw - one built by
`bannertool makebanner -i banner.png -a audio.wav` - beside the ones that do
not. `tools/check-banner.py` in this repository checks the parts of it that can
be checked automatically.

## Already fixed, do not undo

Attempt 2 corrected these, and they must stay:

- every mesh carries `TEXCOORD_0`
- there is a texture object, and the materials reference it (verified: five
  relative offsets inside the five `MTOB`s resolve to the `TXOB`)
- no `LutSet`, no light, no `Scene` - a banner is not a lit scene
- the texture is a power of two and carries exactly `width * height * 2` bytes
  for RGBA4444

## Still missing

### 1. The texture's pixel format fields

The texture object leaves three fields at zero. The hardware format beside them
says `4` (RGBA4444), so the file is not contradictory, only under-specified.

| offset | field | must be | meaning |
|---|---|---|---|
| `+0x20` | `glFormat` | `0x6752` | `GL_RGBA_NATIVE_DMP` |
| `+0x24` | `glType` | `0x8033` | `GL_UNSIGNED_SHORT_4_4_4_4` |
| `+0x50` | bits per texel | `16` | RGBA4444 |

(`tools/fix-banner-texture.py` patches these after the fact. Writing them in the
first place is better.)

### 2. Animation groups on the model

The model object (`CMDL`) declares how many animation groups it has at `+0x28`,
with a dictionary of them at `+0x2c`. A working banner declares **three**, even
though nothing in it moves:

    SkeletalAnimation
    VisibilityAnimation
    MaterialAnimation

Ours declares zero and points at nothing. This is the largest remaining
structural difference between a banner that draws and one that does not.

### 3. `CMDL +0x1c`

`1` in a working banner, `0` in ours. It sits between the flags field at `+0x18`
and the animation group count at `+0x28`, both of which are understood; this one
is not, but it is one of only two fields left that differ.

## Geometry: size and centring

A working banner's geometry sits inside roughly **26 x 15 units, centred on the
origin**. That is the shape of the view the HOME Menu draws it into.

The current model is **19.03 wide x 26.12 tall**, and its vertical centre is at
**y = +4.46**, not zero:

    x  -9.52 .. 9.52
    y  -8.60 .. 17.52
    z  -1.40 .. 1.37

So it is portrait where the banner is landscape, about 1.7x too tall for the
frame, and pushed upward within it. Even once it draws, most of it will be
outside the view.

**Please rebuild the geometry to fit inside 26 x 15 centred on the origin**, in
landscape. The current model is the icon stacked vertically - wordmark, moon,
gate - which does not fit a banner. Something laid out sideways would: the gate
and moon on one side, the name on the other.

## How to check before sending it

    tools/check-banner.py <file>.bnr

It reads the file the way the renderer would and reports what is missing. A
banner that passes with no notes is one worth putting on a console.
