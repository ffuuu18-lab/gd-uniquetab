r"""Generate a native-looking cover plate per collection cell size.

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

  1. the stash cover with its lattice erased and one row of padding -> a clean 438x627 ground,
  2. new frames drawn on the collection grid (origin 105,76, rowGap 2 - the same numbers
     ut_live.cpp lays the boxes out with) by ALPHA-STAMPING a 9-slice cut from one real
     vanilla frame of the materials plate: only the gold line pixels are transferred, so no
     foreign ground colour comes with them. EVERY cell of the fit gets one (lattice_frame =
     ut_layout.h utLatticeFrame): a frame whose 2 px outset crosses the plate's edge is cut to
     the plate, never dropped, so the lattice has the page's columns and rows,
  3. the vanilla ornament (border, corner flourishes, tab strip) composited back ON TOP,
     so a frame that would run under the bottom border is clipped by it exactly as a
     vanilla frame would be.

Output: data/oracle/plates/uniq_plate_<W>x<H>.tex  (+ a _view.png preview of each).

A USER GRID (--grid x,y,w,h, record px, window-local - uniquetab.ini's grid_<W>x<H>): the frames
are stamped from that origin, each cell size fitted to that rectangle by the same rule as
pages_gen.cpp's grid() (cols = w // cw, rows = h // (ch + gap), rows walked back to the frame's
box count, the F line's), and every name carries the cover, the tag and _L<hash> (--layout-hash, the eight hex digits
gen::layoutPlateHash computed for the set) - the reference test_catalogue --layout-plates compares
the DLL's layout plates with, byte for byte.
A ZOOM (--zoom <pct>, 100..200, with --grid - uniquetab.ini's zoom_pct): the set is painted for
the MAGNIFIED cells - every cell size times pct / 100, rounded, lowered per cell size until one
fits the rectangle (ut_layout.h utZoomFit), two that come out the same painted once - fitted to
--grid like any user grid. The derived grid at a zoom passes its own x,y,w,h (the P line's).
Nothing ships from here: the DLL paints the same set at run time (src/gen/plate_gen) into the
game's own settings\ui\caravan\, one of the engine's texture override roots, and the
catalogue harness compares its vanilla plates with these byte for byte.

THE GROUND RULE, in one sentence: the ground is what a NORMAL STASH TAB shows under the same
screen rectangle.  That is the stash cover - the TransferPlate of caravan_window's TransferWindow
(`caravan_transfercoverimage.dbr`, the record our own frame already uses as its plate) - read
through the merged view, never the materials cover, whose art is the 24 hand-placed frames on
vanilla and a whole baked reagent cluster under X-Mod.  Which pixels of it:

  * the two covers cover the same rectangle (same width, same absolute left edge): the WHOLE
    stash texture, padded by repeating its last row (or cropped) to the materials cover's
    height, because the widget froze its destination rectangle from THAT size.  Vanilla
    (438x626 -> 438x627) and X-Mod (975x968, both at absolute 0,62) take this branch;
  * otherwise the stash cover's pixels UNDER the materials cover's rectangle, translated
    through the two windows' WindowLocationX/Y and the two covers' bitmapPositionX/Y:
        off = (matWindow + matBitmapPos) - (stashWindow + stashBitmapPos)
    Baul takes this branch - a 346x625 crop of the 952x1007 whole-window art at 605,71;
  * and when that rectangle is not inside the stash cover, the materials cover itself, with a
    line in the log saying so.

The lattice erasure and the ornament pass below are MEASURED ON THE VANILLA TEXTURE and run
only when the ground IS that texture.

A MODDED GROUND IS A FLAT FILL.  Whenever the rule above does not land on the vanilla stash
texture, not one pixel of the mod's art is copied: the plate is filled with a single opaque
colour and the frames are stamped on that.  A mod's cover carries its own baked box lattice
(X-Mod's is 32 px) that our frames are not aligned to and cannot be, so the two grids interleave;
the mod's own window frame surrounds the page anyway.  The colour is the vanilla ground's own
tone, sampled from ui/caravan/caravan_transfercoverimage.tex (the VANILLA copy out of UI.arc,
never a mod's) at GROUND_SAMPLE = (312, 180) - the centre of a lattice cell, 16 px from every
line and the calmest such point on the texture: RGB 06 06 06, which is also the median of the
whole non-gold interior.  --plate-ground RRGGBB (uniquetab.ini's `plate_ground` key) replaces it;
it changes only this fill and never a vanilla plate.

The name a plate is written under carries the cover size when it is not the vanilla 438x627, and
the VIEW's tag as well when one is given: every view's plates live in the same override folder,
and the cover size alone collides when two mods share it.  --plate-tag <name> is that tag; the
DLL passes the custom game's mod folder leaf and nothing on the start-up view, which is why the
shipped names are unchanged.

    python tools/make_plates.py [--png-only]
    python tools/make_plates.py --custom <mod dir> --out <dir>      one mod's set, offline
    python tools/make_plates.py --custom <mod dir> --plate-ground 001133 --out <dir>
    python tools/make_plates.py --custom <mod dir> --plate-tag lootascension --out <dir>
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
OUT = os.path.join(ROOT, "data", "oracle", "plates")
SRC = os.path.join(OUT, "src")
GROUPS = os.path.join(ROOT, "data", "oracle", "uniq-groups.txt")

VANILLA_TEX = "caravan/caravan_transfercomponent1_bg.tex"
PLAIN_TEX = "caravan/caravan_transfercoverimage.tex"

CARAVAN = "records/ui/caravan/"
WINDOW_REC = CARAVAN + "caravan_window.dbr"
MATERIAL_REC = CARAVAN + "caravan_materialwindow.dbr"
TRANSFERWIN_REC = CARAVAN + "caravan_transferwindow.dbr"

# Where a MODDED plate's flat ground takes its colour from, on the VANILLA stash cover: the
# centre of the lattice cell bounded by x = 296/328 and y = 164/196 - 16 px from every line and
# the calmest cell centre on the texture. It reads (6, 6, 6), the median of the non-gold
# interior. GROUND_FALLBACK is that value, used only if UI.arc's copy cannot be read.
GROUND_SAMPLE = (312, 180)
GROUND_FALLBACK = (6, 6, 6)

# The plate is drawn at the window's (bitmapPositionX, bitmapPositionY) = (0, 1).
PLATE_DX, PLATE_DY = 0, 1
FRAME_OUTSET = 2  # a frame is the cell grown by 2 px on every side (ut_layout.h kUtFrameOutset)


def lattice_frame(x0, y0, cw, ch, gap, col, row, dx, dy, w, h):
    """ut_layout.h utLatticeFrame: the frame under cell (col, row) in PLATE px, the cell grown by
    FRAME_OUTSET and cut to the w x h plate; None only when the cell itself is not on the plate.

    Every cell of the fit (cols x rows, the page's) is framed: a cell flush with the plate's edge
    keeps its frame, its outer line on the plate's last pixels. The old rule dropped a cell whose
    OUTSET did not fit, which left Reign of Terror's 29th column (grid 46,72,928,520) unframed.
    """
    gap = max(0, gap)
    bx = x0 + cw * col - dx
    by = y0 + (ch + gap) * row - dy
    if cw < 1 or ch < 1 or bx < 0 or by < 0 or bx + cw > w or by + ch > h:
        return None
    fx, fy = bx - FRAME_OUTSET, by - FRAME_OUTSET
    fw, fh = cw + 2 * FRAME_OUTSET, ch + 2 * FRAME_OUTSET
    if fx < 0:
        fw, fx = fw + fx, 0
    if fy < 0:
        fh, fy = fh + fy, 0
    fw = min(fw, w - fx)
    fh = min(fh, h - fy)
    return fx, fy, fw, fh


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
    bpp = pf[3]
    if pf[2] != 0 or bpp not in (24, 32):
        raise ValueError("only uncompressed 24/32-bit surfaces are handled (fourCC=%r bpp=%d)"
                         % (pf[2], bpp))
    if mips not in (0, 1):
        raise ValueError("mipmapped source (%d) - not handled" % mips)
    off = 4 + hsize
    n = bpp // 8
    need = w * h * n
    raw = payload[off:off + need]
    if len(raw) != need:
        raise ValueError("short surface: %d of %d bytes" % (len(raw), need))
    px = np.frombuffer(raw, dtype=np.uint8).reshape(h, w, n)
    if n == 3:                                   # BGR, opaque (Baul's whole-window art)
        rgba = np.empty((h, w, 4), dtype=np.uint8)
        rgba[..., 0:3] = px[..., [2, 1, 0]]
        rgba[..., 3] = 255
        return w, h, rgba
    return w, h, px[..., [2, 1, 0, 3]].copy()


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
def read_cover(view, bitmap):
    """(w, h, RGBA) of a bitmapName, from a mod .arc first and then from UI.arc."""
    leaf = bitmap.split("/", 1)[1] if "/" in bitmap else bitmap
    blob = None
    for a in view.arcs:
        for key in (leaf, bitmap, os.path.basename(bitmap)):
            if key in a:
                blob = a.read(key)
                break
        if blob:
            break
    if blob is None:
        key = bitmap[len("ui/"):] if bitmap.startswith("ui/") else bitmap
        blob = view.ui.read(key) or view.ui.read(bitmap)
    if not blob:
        raise SystemExit("cannot read the cover %s" % bitmap)
    return tex_decode(blob)


def _rec(view, name):
    r, _src = view.rec(name)
    return r


def stash_cover(geo):
    """(bitmapName, absX, absY, w, h, RGBA) of the plate a NORMAL TAB draws.

    caravan_window -> TransferWindow -> TransferPlate, through the merged view; its absolute
    origin is that window's WindowLocation plus the cover's bitmapPosition.
    """
    from ui_geometry import _i, _s                                          # noqa: E402
    win = _rec(geo.src, WINDOW_REC)
    tw = _rec(geo.src, _s(win, "TransferWindow") or TRANSFERWIN_REC)
    if not tw:
        raise SystemExit("no transfer window record - cannot find the stash cover")
    crec = _rec(geo.src, _s(tw, "TransferPlate"))
    bitmap = _s(crec, "bitmapName")
    if not bitmap:
        raise SystemExit("the transfer window's TransferPlate has no bitmapName")
    try:
        w, h, rgba = read_cover(geo.src, bitmap)
    except ValueError as e:
        # A compressed or mipmapped surface: refused here, and the caller falls back to the
        # materials cover with the reason in the log.
        return (bitmap, 0, 0, 0, 0, str(e))
    return (bitmap,
            _i(tw, "WindowLocationX") + _i(crec, "bitmapPositionX"),
            _i(tw, "WindowLocationY") + _i(crec, "bitmapPositionY"),
            w, h, rgba)


def fit(img, w, h):
    """The plate's own size: extra rows are the last one repeated, extra columns are cut."""
    if img.shape[0] < h:
        img = np.concatenate([img, np.repeat(img[-1:], h - img.shape[0], axis=0)], axis=0)
    return img[:h, :w]


def ground_for(geo, dx, dy):
    """(RGBA, one line for the log, is-it-the-vanilla-texture) - THE GROUND RULE, see the header.

    dx, dy are the materials cover's own bitmapPosition.
    """
    from ui_geometry import _i                                              # noqa: E402
    page = _rec(geo.src, MATERIAL_REC)
    mat_x = _i(page, "WindowLocationX") + dx
    mat_y = _i(page, "WindowLocationY") + dy
    bitmap, sx, sy, sw, sh, rgba = stash_cover(geo)
    if not sw:
        w, h, cover = read_cover(geo.src, geo.plate)
        return cover, ("ground: the materials cover itself - the stash cover %s cannot be "
                       "decoded (%s)" % (bitmap, rgba)), False
    # The lattice constants and the ornament pass below are measured on THIS texture.
    vanilla_ground = bitmap == "ui/" + PLAIN_TEX and (sw, sh) == (438, 626)
    off_x, off_y = mat_x - sx, mat_y - sy
    if sw == geo.plateW and off_x == 0:
        return rgba, ("ground: the whole stash cover %s %dx%d" % (bitmap, sw, sh)), vanilla_ground
    if 0 <= off_x and off_x + geo.plateW <= sw and 0 <= off_y and off_y + geo.plateH <= sh:
        cut = rgba[off_y:off_y + geo.plateH, off_x:off_x + geo.plateW].copy()
        # the lattice is measured on the whole vanilla texture, never on a cut of one
        return cut, ("ground: the stash cover %s %dx%d cut at %d,%d"
                     % (bitmap, sw, sh, off_x, off_y)), False
    w, h, cover = read_cover(geo.src, geo.plate)
    return cover, ("ground: the materials cover itself - the page's rectangle (%d,%d %dx%d) is "
                   "not inside the stash cover %s %dx%d"
                   % (off_x, off_y, geo.plateW, geo.plateH, bitmap, sw, sh)), False


def parse_hex_rgb(text):
    """(r, g, b) from six hex digits with an optional # or 0x, else None."""
    t = (text or "").strip()
    if t.startswith("#"):
        t = t[1:]
    elif t[:2].lower() == "0x":
        t = t[2:]
    if len(t) != 6:
        return None
    try:
        v = int(t, 16)
    except ValueError:
        return None
    return ((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF)


def flat_ground(w, h, override):
    """(RGBA plate filled with one colour, the info line) - see the header."""
    rgb = parse_hex_rgb(override)
    if rgb is not None:
        note = ("plate ground: a flat %02X%02X%02X, from plate_ground - no pixel of the mod's "
                "own cover is copied" % rgb)
    else:
        try:
            extract_sources()
            with open(os.path.join(SRC, os.path.basename(PLAIN_TEX)), "rb") as fh:
                _w, _h, van = tex_decode(fh.read())
            x, y = GROUND_SAMPLE
            rgb = tuple(int(v) for v in van[y, x, 0:3])
            note = ("plate ground: a flat %02X%02X%02X, sampled from the vanilla ui/"
                    % rgb) + PLAIN_TEX + (" at %d,%d - no pixel of the mod's own cover is copied"
                                          % GROUND_SAMPLE)
        except (OSError, ValueError, SystemExit):
            rgb = GROUND_FALLBACK
            note = ("plate ground: a flat %02X%02X%02X - the vanilla stash cover could not be "
                    "sampled, so the built-in tone is used" % rgb)
    img = np.empty((h, w, 4), dtype=np.uint8)
    img[..., 0], img[..., 1], img[..., 2], img[..., 3] = rgb[0], rgb[1], rgb[2], 255
    return img, note


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


# src/gen/pages_gen.cpp: the frame's box count, and the cap the row count is walked back to. It is
# the view's own (ut_layout.h utFrameBoxes, build_uniq_db.py frame_boxes): read_groups() takes it
# from the F line; regrid() computes it for the derived cover, as pages_gen does.
FRAME_FLOOR, FRAME_MAX = 160, 512
FRAME_BOX_CAP = FRAME_FLOOR


def frame_boxes(cover_w, cover_h, cells, gap):
    """ut_layout.h utFrameBoxes: the whole cover's uncapped fit at the largest-count cell size,
    clamped to FRAME_FLOOR..FRAME_MAX."""
    best = 0
    for (cw, ch, _cols, _rows, _labels) in cells:
        best = max(best, max(1, max(1, cover_w) // max(1, cw)) *
                   max(1, max(1, cover_h) // (max(1, ch) + gap)))
    return min(max(best, FRAME_FLOOR), FRAME_MAX)


def fit_rect(cells, w, h, gap, cap=None):
    """ut_layout.h utGridFit over a w x h rectangle, for each cell size, under the frame's count."""
    cap = FRAME_BOX_CAP if cap is None else cap
    out = []
    for (cw, ch, _cols, _rows, labels) in cells:
        cols = max(1, w // cw)
        rows = max(1, h // (ch + gap))
        while cols * rows > cap and rows > 1:
            rows -= 1
        out.append((cw, ch, cols, rows, labels))
    return out


def zoom_pct(pct):
    """ut_layout.h utZoomPct: below 100 is 100, above 200 is 200."""
    return 100 if pct < 100 else (200 if pct > 200 else pct)


def zoom_cell(cell, pct):
    """ut_layout.h utZoomCell: round(cell * pct / 100), half up, at least 1."""
    cell = max(1, cell)
    pct = zoom_pct(pct)
    return cell if pct == 100 else (cell * pct + 50) // 100


def zoom_fit(w, h, cw, ch, gap, pct):
    """ut_layout.h utZoomFit: the key's zoom, lowered (never below 100) until one magnified cell
    fits the w x h grid rectangle (width <= w, height + gap <= h) -> (pct, cellW, cellH)."""
    cw, ch, gap = max(1, cw), max(1, ch), max(0, gap)
    p = zoom_pct(pct)
    while p > 100 and (zoom_cell(cw, p) > w or zoom_cell(ch, p) + gap > h):
        p -= 1
    return p, zoom_cell(cw, p), zoom_cell(ch, p)


def zoom_cells(cells, w, h, gap, pct):
    """gen::planLayoutPlates at a zoom: every cell size magnified and clamped to the rectangle,
    two that come out the same are one plate, ascending by (cellW, cellH)."""
    out = {}
    for (cw, ch, cols, rows, labels) in cells:
        _p, zw, zh = zoom_fit(w, h, cw, ch, gap, pct)
        e = out.setdefault((zw, zh), [cols, rows, []])
        e[2].extend(labels)
    return [(cw, ch, v[0], v[1], v[2]) for (cw, ch), v in sorted(out.items())]


def regrid(geo, cells, gap):
    """The cell grid a DERIVED rectangle gives, the way pages_gen.cpp's grid() computes it.

    The oracle's cols/rows belong to the vanilla 333x552; a mod's page is a different size, so
    a set painted with the vanilla counts leaves most of it empty. The cap is the frame that
    view's generation writes: its own cover's fit (frame_boxes).
    """
    return fit_rect(cells, geo.usableW, geo.usableH, gap,
                    frame_boxes(geo.plateW, geo.plateH, cells, gap))


def read_groups(path=None):
    """-> (x0, y0, rowGap, [(cellW, cellH, cols, rows, [labels...])])

    The F line still carries the origin the DLL lays boxes out with; the P line beside it
    carries the plate, and main() reads that through ui_geometry instead.
    """
    global FRAME_BOX_CAP
    x0, y0, gap = 105, 76, 2
    cells = {}
    with open(path or GROUPS, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            f = line.rstrip("\r\n").split("\t")
            if f[0] == "F" and len(f) >= 7:
                x0, y0, gap = int(f[4]), int(f[5]), int(f[6])
                FRAME_BOX_CAP = int(f[2]) if int(f[2]) > 0 else FRAME_FLOOR
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
    args = sys.argv[1:]
    custom = args[args.index("--custom") + 1] if "--custom" in args else None
    basemods = "--basemods" in args
    out_dir = args[args.index("--out") + 1] if "--out" in args else OUT
    # uniquetab.ini's plate_ground: the flat colour a MODDED plate is filled with.
    plate_ground = args[args.index("--plate-ground") + 1] if "--plate-ground" in args else ""
    # The view tag a custom game's plates carry, so two mods with one cover size cannot
    # collide. Empty for the start-up view, which keeps the shipped names exactly.
    plate_tag = args[args.index("--plate-tag") + 1] if "--plate-tag" in args else ""
    # A user grid: x,y,w,h in record px, and the hash its set is named with.
    user_grid = None
    if "--grid" in args:
        user_grid = [int(v) for v in args[args.index("--grid") + 1].split(",")]
        if len(user_grid) != 4 or user_grid[2] < 1 or user_grid[3] < 1:
            raise SystemExit("--grid wants x,y,w,h")
    layout_hash = args[args.index("--layout-hash") + 1] if "--layout-hash" in args else ""
    if user_grid and not layout_hash:
        raise SystemExit("--grid needs --layout-hash (the eight hex digits of the set)")
    # uniquetab.ini's zoom_pct: the set of a zoom is painted for the magnified cells of --grid
    # (the user's grid, or the derived one's x,y,w,h - a zoom is a layout of its own).
    zoom = int(args[args.index("--zoom") + 1]) if "--zoom" in args else 100
    if zoom != 100 and not user_grid:
        raise SystemExit("--zoom needs --grid (the grid in force, the derived one's included)")
    groups_path = args[args.index("--groups") + 1] if "--groups" in args else None
    os.makedirs(out_dir, exist_ok=True)

    sys.path.insert(0, HERE)
    from arz import ArzDatabase  # noqa: E402
    import ui_geometry  # noqa: E402

    geo = ui_geometry.derive(GAME, basemods, custom)
    vanilla_cover = geo.tuple() == ui_geometry.VANILLA
    print("cover: %s %dx%d (%s)" % (geo.plate, geo.plateW, geo.plateH, geo.view))

    w, h = geo.plateW, geo.plateH
    # The cover's own bitmapPosition, back out of the convention ui_geometry.py states:
    # usableW = plateW - (x0 - bitmapPositionX). Vanilla 0,1; X-Mod 0,-26; Baul 92,1.
    dx, dy = geo.x0 - (w - geo.usableW), geo.y0 - (h - geo.usableH)
    src_ground, note, vanilla_ground = ground_for(geo, dx, dy)
    print(note)
    if not vanilla_ground:
        # A MODDED GROUND IS A FLAT FILL - not one pixel of the mod's art. See the header.
        src_ground, fill_note = flat_ground(w, h, plate_ground)
        print(fill_note)
    if vanilla_ground:
        # The lattice and the ornament are measured on THIS texture, so they run only here.
        cov = src_ground
        lm = lattice_mask(cov, halo=1)
        ground = fit(erase_lines(cov, lm), w, h)
        ornament = fit(gold_mask(cov) & ~lattice_mask(cov), w, h)
        cov = fit(cov, w, h)
        if not user_grid:
            Image.fromarray(ground).save(os.path.join(SRC, "plate_ground.png"))
        print("erased %d lattice pixels, kept %d ornament pixels"
              % (int(lm.sum()), int(ornament.sum())))
    else:
        # A mod's own plain tab background: it carries the mod's border already, and nothing
        # was erased from it, so there is no ornament to composite back.
        ground = fit(src_ground, w, h)
        cov, ornament = None, None

    db = ArzDatabase.load_game(GAME)
    boxes = vanilla_frames(db)
    # The donor frame: the small square one at box 3 (32x32 cell), the cleanest on the plate.
    # It is cut from the VANILLA materials plate in every view - the box records that locate it
    # are vanilla, and a mod's cover has no frame of ours at those coordinates, so cutting from
    # the mod's own art yields an empty stamp and a plate with no grid on it at all.
    extract_sources()
    with open(os.path.join(SRC, os.path.basename(VANILLA_TEX)), "rb") as fh:
        _dw, _dh, donor_plate = tex_decode(fh.read())
    bx, by = boxes[2]
    donor = (bx - FRAME_OUTSET, by - FRAME_OUTSET, 32 + 2 * FRAME_OUTSET, 32 + 2 * FRAME_OUTSET)
    sl = frame_slices(donor_plate, donor)

    _x0, _y0, gap, cells = read_groups(groups_path)
    x0, y0 = geo.x0, geo.y0
    if user_grid:
        x0, y0 = user_grid[0], user_grid[1]
        if zoom_pct(zoom) != 100:
            cells = zoom_cells(cells, user_grid[2], user_grid[3], gap, zoom)
        cells = fit_rect(cells, user_grid[2], user_grid[3], gap)
    elif not vanilla_cover:
        cells = regrid(geo, cells, gap)
    print("grid origin (%d,%d) rowGap %d, cover at %d,%d; %d distinct cell sizes"
          % (x0, y0, gap, dx, dy, len(cells)))

    made = []
    for (cw, ch, cols, rows, labels) in cells:
        img = ground.copy()
        n = 0
        for rr in range(rows):
            for cc in range(cols):
                f = lattice_frame(x0, y0, cw, ch, gap, cc, rr, dx, dy, w, h)
                if f is None:
                    continue
                draw_frame(img, sl, f[0], f[1], f[2], f[3])
                n += 1
        if ornament is not None:
            img[ornament] = cov[ornament]      # the vanilla border always wins
        # The name ut_plate.cpp asks for: a custom view (a plate tag) always carries the cover
        # size and its tag, whatever the cover size; otherwise the shipped set keeps the plain
        # name and any other cover size carries it, because the same cell on a bigger caravan
        # is a different plate.
        if layout_hash:
            name = "uniq_plate_%dx%d_%dx%d%s_L%s" % (cw, ch, geo.plateW, geo.plateH,
                                                     "_" + plate_tag if plate_tag else "",
                                                     layout_hash)
        elif plate_tag:
            name = "uniq_plate_%dx%d_%dx%d_%s" % (cw, ch, geo.plateW, geo.plateH, plate_tag)
        elif geo.is_vanilla_plate():
            name = "uniq_plate_%dx%d" % (cw, ch)
        else:
            name = "uniq_plate_%dx%d_%dx%d" % (cw, ch, geo.plateW, geo.plateH)
        Image.fromarray(img).save(os.path.join(out_dir, name + "_view.png"))
        if not png_only:
            blob = tex_encode(img)
            with open(os.path.join(out_dir, name + ".tex"), "wb") as fh:
                fh.write(blob)
            made.append((name, len(blob), cols, rows, n, labels))
        print("  %-22s %2dx%-2d = %3d frames   groups: %s"
              % (name, cols, rows, n, ", ".join(labels)))

    if not png_only:
        # read the headers back the way the engine will
        print("\nread-back:")
        for (name, size, cols, rows, n, _labels) in made:
            p = os.path.join(out_dir, name + ".tex")
            with open(p, "rb") as fh:
                blob = fh.read()
            rw, rh, _px = tex_decode(blob)
            ok = (rw, rh) == (w, h) and len(blob) == size
            print("  %-22s %d bytes  %dx%d  %s" % (name + ".tex", len(blob), rw, rh,
                                                   "OK" if ok else "MISMATCH"))
            if not ok:
                return 1
        with open(os.path.join(out_dir, "PLATES.txt"), "w", encoding="utf-8") as fh:
            fh.write("# generated cover plates (tools/make_plates.py)\n")
            fh.write("# resource path -> local file\n")
            for (name, _size, cols, rows, n, labels) in made:
                fh.write("ui/caravan/%s.tex\t%s.tex\t%dx%d\t%d frames\t%s\n"
                         % (name, name, cols, rows, n, ",".join(labels)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
