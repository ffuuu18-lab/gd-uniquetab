"""The collection grid's rectangle and its plate, derived from the database the engine merges.

Reference implementation; src/gen/ui_geometry.cpp does the same job inside the DLL and must
agree with it number for number.

WHY THIS IS NOT FIVE CONSTANTS.  A stash-UI mod moves the caravan through ordinary .dbr
records, and the two families do it by opposite means: the X-Mod / Grim UI family leaves the
materials sub-window where it is and moves all 24 reagent boxes individually, while the Baul
family leaves every box alone and moves the whole sub-window.  A reader that looks at only one
of the two signals is blind to one of the two families, so all of them are read:

  records/ui/caravan/caravan_window.dbr
      windowDefaultExtentX/Y  the window; tabSpacingX/Y says where the tab strip sits -
      only Y set: strip DOWN THE LEFT (vanilla), X set: strip ACROSS THE TOP (both mods).
  records/ui/caravan/caravan_materialwindow.dbr
      WindowLocationX/Y and reagentBoxes - the page we borrow.
  records/ui/caravan/reagents/materials/caravan_reagent0NN.dbr
      itemBoxX/Y of every box; their bounding box IS the usable rectangle.
  the TransferPlate record of that page
      bitmapName and bitmapPositionX/Y, plus that bitmap's REAL pixel size read out of the
      .arc that holds it.  THE TEXTURE, NOT THE WINDOW, IS THE PLATE SIZE: vanilla's cover is
      438x627 inside a 438x715 window, and X-Mod's is 975x968 inside 975x1030 - the "window
      minus the header" relation does not survive a mod.
  records/ui/caravan/caravan_window.dbr -> StashWindow -> InventoryDBR
      WHERE A NORMAL TAB'S ITEM GRID STARTS.  The stash page's own grid is the thing a player
      sees on every other tab, and it is the only record that says where the page is free:
      inventoryX/Y are relative to the stash page, so they are translated into the materials
      page's space by the two WindowLocations.  The reagent boxes say where the MATERIALS are,
      which is not the same thing - X-Mod moves them into a decorative cluster in the middle of
      a page whose whole top-left is free.

THE ORIGIN RULE.  The grid starts where the stash page's grid starts:

    anchor = stashWindow.WindowLocation + inventoryXY - materialWindow.WindowLocation

taken when that anchor falls INSIDE the page's own plate, and the page's reagent-box bounding
box otherwise.  The test is what separates the two families: X-Mod leaves both windows alone and
widens the stash grid, so its anchor lands on the plate (8,-26 - the plate's own top-left corner
plus the 8 px the game itself insets by); Baul moves the materials page to 513,70 and turns the
stash grid into the whole 929 px window, so its anchor lands 503 px LEFT of the page and the
boxes are the honest signal there.  A database that cannot be read that far falls back the same
way, and the geometry line says which signal was used.

Then the grid clears OUR OWN header, which no database knows about: the one-line label sits in
the page's y 8..24 and the 26-button pad in y 25..68 (kFreeY0/kFreeY1, src/ut_panel.cpp), so the
first box row starts at y = 76.  On the vanilla page that is exactly where Crate put its own 24
reagent boxes, which is why the vanilla derivation does not move.

THE CONVENTION, stated once because one pixel hangs on it.  The origin is in WINDOW units and
the plate is a bitmap placed at bitmapPositionX/Y, so the usable rectangle is measured from the
PLATE's own top-left:

    usableW = plateW - (x0 - bitmapPositionX)
    usableH = plateH - (y0 - bitmapPositionY)

Vanilla's cover sits at 0,1, which is where the extra pixel comes from: 627 - (76 - 1) = 552,
one more than the 627 - 76 = 551 this tool used to hold as a constant.  Neither number changes
a single box - every cell height in play (32, 64, 96, 128 on a 2 px row gap) divides the two
the same way - and build_uniq_db.py asserts exactly that, so the shipped layout is unchanged.
The pad is kept because it is the same arithmetic that reproduces the two modded families.

    python ui_geometry.py                       the vanilla install
    python ui_geometry.py --basemods            + mods\\database.arz and mods\\*.arc
    python ui_geometry.py --custom X-Mod        + mods\\X-Mod\\database\\*.arz and its .arc
"""

from __future__ import annotations

import argparse
import glob
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gdpath                                                       # noqa: E402
from arc import ArcArchive, ArcSet                                  # noqa: E402
from arz import ArzArchive, ArzDatabase                             # noqa: E402

CARAVAN = "records/ui/caravan/"
WINDOW_REC = CARAVAN + "caravan_window.dbr"
MATERIAL_REC = CARAVAN + "caravan_materialwindow.dbr"
STASHWIN_REC = CARAVAN + "caravan_stashwindow.dbr"
STASHINV_REC = CARAVAN + "caravan_stashinventory.dbr"

# The page's top band is the mod's own: the label in y 8..24 and the 26-button pad in y 25..68
# (kFreeY0/kFreeY1, src/ut_panel.cpp). No database knows about it, so it is a constant here.
PANEL_BAND_H = 76

# What the vanilla 1.3.0.8 database derives to. Asserted by build_uniq_db.py and by
# tools/test_catalogue.cpp so a database change cannot move the shipped grid unnoticed.
VANILLA = (105, 76, 333, 552, 438, 627, "ui/caravan/caravan_transfercomponent1_bg.tex")


class Geometry:
    """x0/y0 the grid origin in window units, usableW/H the rectangle, plate the cover."""

    __slots__ = ("x0", "y0", "usableW", "usableH", "plateW", "plateH", "plate",
                 "tabStrip", "view", "notes", "src")

    def __init__(self, x0, y0, usableW, usableH, plateW, plateH, plate, tabStrip, view, notes,
                 src=None):
        self.x0, self.y0 = x0, y0
        self.usableW, self.usableH = usableW, usableH
        self.plateW, self.plateH = plateW, plateH
        self.plate = plate
        self.tabStrip = tabStrip
        self.view = view
        self.notes = notes
        self.src = src            # the View the numbers came out of; make_plates.py reads it

    def tuple(self):
        return (self.x0, self.y0, self.usableW, self.usableH,
                self.plateW, self.plateH, self.plate)

    def line(self):
        """The P line of uniq-groups.txt, without its line ending."""
        return "P\t%d\t%d\t%d\t%d\t%d\t%d\t%s" % self.tuple()

    def is_vanilla_plate(self):
        return (self.plateW, self.plateH) == (VANILLA[4], VANILLA[5])

    def __str__(self):
        return ("x0=%d y0=%d usable=%dx%d plate=%dx%d %s (%s, tabs %s)"
                % (self.x0, self.y0, self.usableW, self.usableH, self.plateW, self.plateH,
                   self.plate, self.view, self.tabStrip))


def tex_size(blob):
    """(w, h) of a Grim Dawn .tex - 'TEX' then a DDS header at +12 (height 24, width 28)."""
    if not blob or len(blob) < 32 or blob[:3] != b"TEX":
        return None
    h, w = struct.unpack_from("<I", blob, 24)[0], struct.unpack_from("<I", blob, 28)[0]
    if not (0 < w <= 4096 and 0 < h <= 4096):
        return None
    return (w, h)


class View:
    """The merged database and texture set, in the engine's own load order, mod last.

    database.arz < gdx1 < gdx2 < gdx3            always (ArzDatabase.load_game)
      < mods\\database.arz                        only with /basemods
      < mods\\<name>\\database\\*.arz              only in that custom game
    """

    def __init__(self, game, basemods=False, custom=None):
        self.game = game
        self.db = ArzDatabase.load_game(game)
        self.layers = []                 # (ArzArchive, label), last wins
        self.arcs = []                   # mod .arc files, searched before UI.arc
        self.view = "vanilla"
        if basemods:
            self._add_layer(os.path.join(game, "mods", "database.arz"), "/basemods")
            for p in sorted(glob.glob(os.path.join(game, "mods", "*.arc"))):
                self._add_arc(p)
            self.view = "/basemods"
        if custom:
            # a name under <game>\mods, or a full path to an unpacked mod folder
            base = custom if os.path.isdir(custom) else os.path.join(game, "mods", custom)
            for p in sorted(glob.glob(os.path.join(base, "database", "*.arz"))):
                self._add_layer(p, custom)
            for p in sorted(glob.glob(os.path.join(base, "resources", "*.arc"))):
                self._add_arc(p)
            self.view = "custom game %s" % custom
        self.ui = ArcSet.load(game, os.path.join("resources", "UI.arc"))

    def _add_layer(self, path, label):
        if os.path.isfile(path):
            self.layers.append((ArzArchive(path, label), label))

    def _add_arc(self, path):
        if os.path.isfile(path):
            self.arcs.append(ArcArchive(path, os.path.basename(path)))

    def rec(self, name):
        """(record, source label); a record no layer overrides keeps its vanilla value."""
        for arz, label in reversed(self.layers):
            r = arz.get(name)
            if r is not None:
                return r, label
        return self.db.get(name), "vanilla"

    def tex(self, name):
        """((w, h), source) of a bitmapName. A mod .arc is rooted at its own prefix."""
        leaf = name.split("/", 1)[1] if "/" in name else name
        for a in self.arcs:
            for key in (leaf, name, os.path.basename(name)):
                if key in a:
                    return tex_size(a.read(key)), a.tag
        for key in (name[len("ui/"):] if name.startswith("ui/") else name, name):
            if key in self.ui:
                return tex_size(self.ui.read(key)), "UI.arc"
        return None, None


def _i(rec, key, default=0):
    v = rec.get(key, default) if rec else default
    return v[0] if isinstance(v, list) else v


def _s(rec, key, default=""):
    v = rec.get(key, default) if rec else default
    return v[0] if isinstance(v, list) else v


def derive(game_dir=None, basemods=False, custom=None, view=None):
    """The one entry point. Raises ValueError naming the record that could not be read."""
    if view is None:
        view = View(game_dir or gdpath.find_game_dir() or "", basemods, custom)
    notes = []

    win, _ = view.rec(WINDOW_REC)
    if not win:
        raise ValueError("%s is not in the database" % WINDOW_REC)
    tabx, taby = _i(win, "tabSpacingX"), _i(win, "tabSpacingY")
    strip = "left" if taby and not tabx else "top"

    page, page_src = view.rec(MATERIAL_REC)
    if not page:
        raise ValueError("%s is not in the database" % MATERIAL_REC)
    boxes = page.get("reagentBoxes") or []
    if isinstance(boxes, str):
        boxes = [boxes]
    xs, ys = [], []
    for b in boxes:
        r, _ = view.rec(b)
        if r:
            xs.append(_i(r, "itemBoxX"))
            ys.append(_i(r, "itemBoxY"))
    if not xs:
        raise ValueError("%s names no readable reagent box" % MATERIAL_REC)

    cover = _s(page, "TransferPlate")
    crec, _ = view.rec(cover) if cover else (None, "-")
    bitmap = _s(crec, "bitmapName")
    if not bitmap:
        raise ValueError("no bitmapName on the materials page's TransferPlate (%r)" % cover)
    wh, tex_src = view.tex(bitmap)
    if not wh:
        raise ValueError("cannot read the pixel size of %r" % bitmap)
    bx, by = _i(crec, "bitmapPositionX"), _i(crec, "bitmapPositionY")

    plateW, plateH = wh

    # Where a normal tab's item grid starts, translated into this page's space.
    x0 = y0 = None
    stash, _ = view.rec(STASHWIN_REC)
    invname = _s(stash, "InventoryDBR") or STASHINV_REC
    inv, _ = view.rec(invname) if stash else (None, "-")
    if inv:
        ax = _i(stash, "WindowLocationX") + _i(inv, "inventoryX") - _i(page, "WindowLocationX")
        ay = _i(stash, "WindowLocationY") + _i(inv, "inventoryY") - _i(page, "WindowLocationY")
        if bx <= ax < bx + plateW and by <= ay < by + plateH:
            x0, y0 = ax, ay
            notes.append("origin from the stash grid (%s at %d,%d) translated to %d,%d"
                         % (invname.rsplit("/", 1)[1], _i(inv, "inventoryX"),
                            _i(inv, "inventoryY"), ax, ay))
    if x0 is None:
        # The stash grid is not this page's - it is the whole window (Baul) or unreadable. The
        # page's own boxes are then the only signal, and a tab strip down the left edge is
        # cleared by the stash page's left inset exactly as it was before.
        x0 = (_i(inv, "inventoryX") or min(xs)) if strip == "left" else min(xs)
        y0 = min(ys)
        notes.append("origin from the reagent boxes (the stash grid is not on this page)")
    y0 = max(y0, PANEL_BAND_H)   # the label and the button pad own the page's top band

    usableW = plateW - (x0 - bx)
    usableH = plateH - (y0 - by)
    if usableW < 32 or usableH < 32:
        raise ValueError("derived a %dx%d usable rectangle - refusing it" % (usableW, usableH))
    notes.append("%s from %s, cover %s at %d,%d from %s"
                 % (MATERIAL_REC.rsplit("/", 1)[1], page_src, bitmap, bx, by, tex_src))
    return Geometry(x0, y0, usableW, usableH, plateW, plateH, bitmap, strip, view.view, notes,
                    view)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", default=gdpath.find_game_dir() or "")
    ap.add_argument("--basemods", action="store_true",
                    help="the view the engine builds when /basemods is on the command line")
    ap.add_argument("--custom", help="a custom game under mods\\<name>")
    a = ap.parse_args()
    g = derive(a.game, a.basemods, a.custom)
    print(g)
    for n in g.notes:
        print("  %s" % n)
    print(g.line())
    if not a.basemods and not a.custom and g.tuple() != VANILLA:
        print("WARNING: the vanilla derivation moved - expected %r" % (VANILLA,))
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
