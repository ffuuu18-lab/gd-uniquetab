"""Generate a native-looking cover plate per collection cell size.

The Crafting Materials page paints its background from ONE texture, named by the
`TransferPlate` record of `records/ui/caravan/caravan_materialwindow.dbr`:

    caravan_materialcoverimage.dbr -> bitmapName = ui/caravan/caravan_transfercomponent1_bg.tex

That texture is 438 x 627, TEX v2 wrapping an uncompressed 32-bit "DDSR" surface
(no mipmaps, no DXT), so it can be decoded and re-encoded with nothing but struct.
Its 24 hand-placed box frames sit at  (itemBoxX - 2, itemBoxY - 1 - 2, cell + 4, cell + 4)
in PLATE pixels - the -1 is `bitmapPositionY = 1`, the -2/+4 is the frame's 2 px outset.
Verified against every one of the 24 vanilla reagent records.

The ground is NOT that plate: erasing its 24 hand-placed frames leaves ghosts (their dark
inner shadow is not gold, so no colour mask catches it).  The ground is the OTHER vanilla
plate of the same window family, `ui/caravan/caravan_transfercoverimage.tex` (438 x 626,
same TEX layout, same border and tab strip), whose only interior marks are a perfectly
regular 1 px lattice at x = 104 + 32k / y = 4 + 32k - trivially erased by copying the pixel
3 px to the side.  It is padded by one row to the material plate's 438 x 627, because the
widget froze its destination rectangle from THAT texture's size at HUD build (see below).

What this script builds, for each distinct cell size used by data/oracle/uniq-groups.txt:

  1. the cover image with its lattice erased and one row of padding -> a clean 438x627 ground,
  2. new frames drawn on the collection grid (origin 105,76, rowGap 2 - the same numbers
     ut_live.cpp lays the boxes out with) by ALPHA-STAMPING a 9-slice cut from one real
     vanilla frame of the materials plate: only the gold line pixels are transferred, so no
     foreign ground colour comes with them,
  3. the vanilla ornament (border, corner flourishes, tab strip) composited back ON TOP,
     so a frame that would run under the bottom border is clipped by it exactly as a
     vanilla frame would be.

Output: data/plates/uniq_plate_<W>x<H>.tex  (+ a _view.png preview of each).
deploy.bat installs them: into the mod folder, and under the game's own
settings\ui\caravan\, one of the engine's texture override roots.

    python tools/make_plates.py [--png-only]
"""

from __future__ import annotations

import os
import struct
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gdpath  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
GAME = gdpath.find_game_dir() or ""
OUT = os.path.join(ROOT, "data", "plates")
SRC = os.path.join(OUT, "src")
GROUPS = os.path.join(ROOT, "data", "oracle", "uniq-groups.txt")

VANILLA_TEX = "caravan/caravan_transfercomponent1_bg.tex"
PLAIN_TEX = "caravan/caravan_transfercoverimage.tex"

# The plate is drawn at the window's (bitmapPositionX, bitmapPositionY) = (0, 1).
PLATE_DX, PLATE_DY = 0, 1
FRAME_OUTSET = 2  # a frame is the cell grown by 2 px on every side


# ---------------------------------------------------------------------------- TEX v2
def tex_decode(blob: bytes):
    """-> (width, height, RGBA ndarray). Raises on anything but an uncompressed 32-bit TEX."""
    if blob[:4] != b"TEX\x02":
        raise ValueError("not a TEX v2 file")
    payload = blob[12:]
    if payload[:4] != b"DDSR":
        raise ValueError("not a DDSR surface")
    hsize, _flags, h, w, _pitch, _depth, mips = struct.unpack_from("<7I", payload, 4)
    if hsize != 124:
        raise ValueError("unexpected DDS header size %d" % hsize)
    pf = struct.unpack_from("<8I", payload, 4 + 72)
    if pf[2] != 0 or pf[3] != 32:
        raise ValueError("only uncompressed 32-bit surfaces are handled (fourCC=%r bpp=%d)"
                         % (pf[2], pf[3]))
    if mips not in (0, 1):
        raise ValueError("mipmapped source (%d) - not handled" % mips)
    off = 4 + hsize
    need = w * h * 4
    raw = payload[off:off + need]
    if len(raw) != need:
        raise ValueError("short surface: %d of %d bytes" % (len(raw), need))
    bgra = np.frombuffer(raw, dtype=np.uint8).reshape(h, w, 4)
    rgba = bgra[..., [2, 1, 0, 3]].copy()
    return w, h, rgba


def tex_encode(rgba: np.ndarray) -> bytes:
    """The exact byte layout of the vanilla plate, with our pixels."""
    h, w = rgba.shape[0], rgba.shape[1]
    dds = bytearray(4 + 124)
    dds[0:4] = b"DDSR"
    struct.pack_into("<7I", dds, 4, 124, 0x1007, h, w, 0, 0, 1)
    # 11 reserved dwords are already zero; pixel format at +72 of the header
    struct.pack_into("<8I", dds, 4 + 72, 32, 0x40, 0, 32, 0, 0, 0, 0)
    bgra = rgba[..., [2, 1, 0, 3]].astype(np.uint8)
    payload = bytes(dds) + bgra.tobytes()
    return b"TEX\x02" + struct.pack("<II", 0, len(payload)) + payload


# ---------------------------------------------------------------------------- sources
def extract_sources() -> None:
    sys.path.insert(0, HERE)
    import arc  # noqa: E402  (tools/ is not a package)

    os.makedirs(SRC, exist_ok=True)
    aset = arc.ArcSet.load(GAME, os.path.join("resources", "UI.arc"))
    for key in (VANILLA_TEX, PLAIN_TEX):
        dst = os.path.join(SRC, os.path.basename(key))
        if os.path.exists(dst):
            continue
        data = aset.read(key)
        if not data:
            raise SystemExit("cannot read %s out of resources/UI.arc" % key)
        with open(dst, "wb") as fh:
            fh.write(data)
        print("extracted %s (%d bytes)" % (dst, len(data)))


def read_groups():
    """-> (x0, y0, rowGap, [(cellW, cellH, cols, rows, [labels...])])"""
    x0, y0, gap = 105, 76, 2
    cells = {}
    with open(GROUPS, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            f = line.rstrip("\r\n").split("\t")
            if f[0] == "F" and len(f) >= 7:
                x0, y0, gap = int(f[4]), int(f[5]), int(f[6])
            elif f[0] == "G" and len(f) >= 8:
                label, cols, rows, cw, ch = f[2], int(f[3]), int(f[4]), int(f[5]), int(f[6])
                cells.setdefault((cw, ch), [cols, rows, []])
                e = cells[(cw, ch)]
                e[0] = max(e[0], cols)
                e[1] = max(e[1], rows)
                e[2].append(label)
    out = [(cw, ch, v[0], v[1], v[2]) for (cw, ch), v in sorted(cells.items())]
    return x0, y0, gap, out


# ---------------------------------------------------------------------------- masks
def gold_mask(rgba: np.ndarray) -> np.ndarray:
    r = rgba[..., 0].astype(int)
    g = rgba[..., 1].astype(int)
    b = rgba[..., 2].astype(int)
    a = rgba[..., 3].astype(int)
    return (a > 40) & (r > 55) & (g > 45) & (r > b + 8)


def vanilla_frames(db):
    """The 24 vanilla frame rects in PLATE pixels, from the box records themselves."""
    rects = []
    for i in range(1, 25):
        rec = db.get("records/ui/caravan/reagents/materials/caravan_reagent%03d.dbr" % i)
        if not rec:
            raise SystemExit("vanilla reagent record %03d is missing" % i)
        bx, by = int(rec.get("itemBoxX", 0)), int(rec.get("itemBoxY", 0))
        rects.append((bx - PLATE_DX, by - PLATE_DY))
    return rects


# The cover image's interior lattice, measured on 1.3.0.8: vertical lines every 32 px from
# x = 104 to x = 424, horizontal lines every 32 px from y = 4 to y = 580, all 1 px wide, all
# inside the grid rectangle.  Everything else that is gold is ornament and must survive.
LATTICE_X0, LATTICE_STEP, LATTICE_NX = 104, 32, 11
LATTICE_Y0, LATTICE_NY = 4, 20   # 20 lines: y = 4 .. 612 (the last one is faint but real)


def lattice_mask(rgba: np.ndarray, halo: int = 0) -> np.ndarray:
    """The interior lattice. `halo` widens each line - a lattice line is 1 px of gold flanked
    by 1 px of pure black on each side (measured: x=135 (0,0,0), x=136 (106,98,64), x=137
    (0,0,0)), so the ERASE mask needs halo=1 while the ornament test needs halo=0."""
    h, w = rgba.shape[0], rgba.shape[1]
    xs = [LATTICE_X0 + LATTICE_STEP * i for i in range(LATTICE_NX)]
    ys = [LATTICE_Y0 + LATTICE_STEP * i for i in range(LATTICE_NY)]
    x0, x1 = xs[0], xs[-1]
    y0, y1 = ys[0], ys[-1]
    m = np.zeros((h, w), dtype=bool)
    for x in xs:
        for d in range(-halo, halo + 1):
            if 0 <= x + d < w:
                m[max(0, y0 - halo):min(h, y1 + halo + 1), x + d] = True
    for y in ys:
        for d in range(-halo, halo + 1):
            if 0 <= y + d < h:
                m[y + d, max(0, x0 - halo):min(w, x1 + halo + 1)] = True
    return m if halo else (m & gold_mask(rgba))


def erase_lines(rgba: np.ndarray, mask: np.ndarray) -> np.ndarray:
    """3 px bands: take the nearest clean pixel sideways, then diagonally (an intersection has
    no clean pixel on either axis)."""
    out = rgba.copy()
    todo = mask.copy()
    h, w = mask.shape
    # never sample a pixel that is itself painted (a masked pixel, or any gold: the ornament
    # sits right next to the first lattice line and would otherwise be smeared into the grid)
    forbidden = mask | gold_mask(rgba)
    for dy, dx in ((0, 5), (0, -5), (5, 0), (-5, 0), (5, 5), (5, -5), (-5, 5), (-5, -5),
                   (0, 9), (0, -9), (9, 0), (-9, 0), (13, 0), (-13, 0)):
        if not todo.any():
            break
        # src[y, x] == out[y + dy, x + dx]
        src = np.roll(np.roll(out, -dy, axis=0), -dx, axis=1)
        ok = ~np.roll(np.roll(forbidden, -dy, axis=0), -dx, axis=1)
        if dy > 0:
            ok[h - dy:, :] = False
        elif dy < 0:
            ok[:-dy, :] = False
        if dx > 0:
            ok[:, w - dx:] = False
        elif dx < 0:
            ok[:, :-dx] = False
        take = todo & ok
        out[take] = src[take]
        todo &= ~take
    if todo.any():
        print("  WARNING: %d lattice pixels could not be filled" % int(todo.sum()))
    return out


# ---------------------------------------------------------------------------- the frame
def frame_slices(rgba: np.ndarray, rect):
    """A 9-slice ALPHA STAMP cut from one real vanilla frame.

    The stamp's alpha is the gold mask of that frame, so only the line pixels travel and the
    materials plate's own (different) ground never contaminates the cover-image ground."""
    x, y, w, h = rect
    f = rgba[y:y + h, x:x + w].copy()
    f[..., 3] = np.where(gold_mask(f), 255, 0).astype(np.uint8)
    c = 6              # corner size
    t = 2              # line thickness
    return {
        "c": c,
        "t": t,
        "tl": f[0:c, 0:c],
        "tr": f[0:c, w - c:w],
        "bl": f[h - c:h, 0:c],
        "br": f[h - c:h, w - c:w],
        "top": f[0:t, c:w - c],
        "bot": f[h - t:h, c:w - c],
        "left": f[c:h - c, 0:t],
        "right": f[c:h - c, w - t:w],
    }


def tile_h(strip: np.ndarray, width: int) -> np.ndarray:
    n = int(np.ceil(width / max(1, strip.shape[1])))
    return np.concatenate([strip] * n, axis=1)[:, :width]


def tile_v(strip: np.ndarray, height: int) -> np.ndarray:
    n = int(np.ceil(height / max(1, strip.shape[0])))
    return np.concatenate([strip] * n, axis=0)[:height]


def draw_frame(dst: np.ndarray, sl, x: int, y: int, w: int, h: int) -> None:
    H, W = dst.shape[0], dst.shape[1]
    c, t = sl["c"], sl["t"]
    if w < 2 * c + 2 or h < 2 * c + 2:
        return

    def blit(src, px, py):
        sh, sw = src.shape[0], src.shape[1]
        sx0 = max(0, -px)
        sy0 = max(0, -py)
        sx1 = min(sw, W - px)
        sy1 = min(sh, H - py)
        if sx1 <= sx0 or sy1 <= sy0:
            return
        s = src[sy0:sy1, sx0:sx1]
        view = dst[py + sy0:py + sy1, px + sx0:px + sx1]
        on = s[..., 3] > 0
        view[on, 0:3] = s[on, 0:3]      # the ground stays opaque; only the line is stamped

    blit(sl["tl"], x, y)
    blit(sl["tr"], x + w - c, y)
    blit(sl["bl"], x, y + h - c)
    blit(sl["br"], x + w - c, y + h - c)
    blit(tile_h(sl["top"], w - 2 * c), x + c, y)
    blit(tile_h(sl["bot"], w - 2 * c), x + c, y + h - t)
    blit(tile_v(sl["left"], h - 2 * c), x, y + c)
    blit(tile_v(sl["right"], h - 2 * c), x + w - t, y + c)


# ---------------------------------------------------------------------------- main
def main() -> int:
    png_only = "--png-only" in sys.argv
    os.makedirs(OUT, exist_ok=True)
    extract_sources()

    sys.path.insert(0, HERE)
    from arz import ArzDatabase  # noqa: E402

    with open(os.path.join(SRC, os.path.basename(VANILLA_TEX)), "rb") as fh:
        w, h, van = tex_decode(fh.read())
    print("materials plate (frame donor + target size) %dx%d" % (w, h))
    with open(os.path.join(SRC, os.path.basename(PLAIN_TEX)), "rb") as fh:
        cw_, ch_, cov = tex_decode(fh.read())
    print("cover image (ground)                       %dx%d" % (cw_, ch_))
    if cw_ != w:
        raise SystemExit("the two plates disagree on width (%d vs %d)" % (cw_, w))

    lm = lattice_mask(cov, halo=1)
    ground = erase_lines(cov, lm)
    ornament = gold_mask(cov) & ~lattice_mask(cov)
    if ch_ < h:                                   # 626 -> 627: repeat the last row
        pad = np.repeat(ground[-1:], h - ch_, axis=0)
        ground = np.concatenate([ground, pad], axis=0)
        ornament = np.concatenate([ornament, np.repeat(ornament[-1:], h - ch_, axis=0)], axis=0)
        cov = np.concatenate([cov, np.repeat(cov[-1:], h - ch_, axis=0)], axis=0)
    Image.fromarray(ground).save(os.path.join(SRC, "plate_ground.png"))
    print("erased %d lattice pixels, kept %d ornament pixels"
          % (int(lm.sum()), int(ornament.sum())))

    db = ArzDatabase.load_game(GAME)
    boxes = vanilla_frames(db)
    # the donor frame: the small square one at box 3 (32x32 cell), the cleanest on the plate
    bx, by = boxes[2]
    donor = (bx - FRAME_OUTSET, by - FRAME_OUTSET, 32 + 2 * FRAME_OUTSET, 32 + 2 * FRAME_OUTSET)
    sl = frame_slices(van, donor)

    x0, y0, gap, cells = read_groups()
    print("grid origin (%d,%d) rowGap %d; %d distinct cell sizes" % (x0, y0, gap, len(cells)))

    made = []
    for (cw, ch, cols, rows, labels) in cells:
        img = ground.copy()
        n = 0
        for rr in range(rows):
            for cc in range(cols):
                bxx = x0 + cw * cc - PLATE_DX
                byy = y0 + (ch + gap) * rr - PLATE_DY
                draw_frame(img, sl, bxx - FRAME_OUTSET, byy - FRAME_OUTSET,
                           cw + 2 * FRAME_OUTSET, ch + 2 * FRAME_OUTSET)
                n += 1
        img[ornament] = cov[ornament]          # the border always wins
        name = "uniq_plate_%dx%d" % (cw, ch)
        Image.fromarray(img).save(os.path.join(OUT, name + "_view.png"))
        if not png_only:
            blob = tex_encode(img)
            with open(os.path.join(OUT, name + ".tex"), "wb") as fh:
                fh.write(blob)
            made.append((name, len(blob), cols, rows, n, labels))
        print("  %-22s %2dx%-2d = %3d frames   groups: %s"
              % (name, cols, rows, n, ", ".join(labels)))

    if not png_only:
        # read the headers back the way the engine will
        print("\nread-back:")
        for (name, size, cols, rows, n, _labels) in made:
            p = os.path.join(OUT, name + ".tex")
            with open(p, "rb") as fh:
                blob = fh.read()
            rw, rh, _px = tex_decode(blob)
            ok = (rw, rh) == (w, h) and len(blob) == size
            print("  %-22s %d bytes  %dx%d  %s" % (name + ".tex", len(blob), rw, rh,
                                                   "OK" if ok else "MISMATCH"))
            if not ok:
                return 1
        with open(os.path.join(OUT, "PLATES.txt"), "w", encoding="utf-8") as fh:
            fh.write("# generated cover plates (tools/make_plates.py)\n")
            fh.write("# resource path -> local file\n")
            for (name, _size, cols, rows, n, labels) in made:
                fh.write("ui/caravan/%s.tex\t%s.tex\t%dx%d\t%d frames\t%s\n"
                         % (name, name, cols, rows, n, ",".join(labels)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
