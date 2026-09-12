"""Reference implementation; the DLL generates the three txt files itself now (src/gen/pages_gen),
uniq-pages.arz stays the shipped file this tool writes.

Build the mod's page databases into data/uniq/ (archives) and data/oracle/ (lists).

THE MECHANISM.  The exe builds a reagent sub-window by calling
`ObjectManager::LoadTableFile` / `GetLoadTable` - two Engine.dll exports - with the record
path, so the DLL can simply hand back a DIFFERENT record.  Overriding
`records/ui/caravan/caravan_materialwindow.dbr` would cost the player their real Crafting
Materials page; the vanilla record is therefore never touched at all, and every collection
page is a NEW record of its own:

    records/ui/caravan/uniq_pNN.dbr                     one reagent window per page
    records/ui/caravan/reagents/uniq/pNN/box_MM.dbr     40 boxes per page

What is emitted
  data/uniq/uniq-pages.arz     ALL pages and their boxes in one archive.  One archive, not
      one per page: with the path substitution above a page is selected by NAME, so separate
      archives would buy nothing and cost ~80 memory-mapped files and 80 LoadDatabase calls.
  data/oracle/uniq-pages.json  the manifest: per page the record path, the slot group, the
      number of boxes and every item on it.
  data/oracle/uniq-records.txt every item record that appears on any page, one per line - the
      list the DLL's gate uses to recognise our items.

  data/oracle/uniq-pages.txt   one line per page: index, record, label k/m, box count.
  data/oracle/uniq-groups.txt  the group model the DLL scrolls: F/V/G/E lines, an E line per
      entry carrying BOTH the box record and the item record.

Selection - THE COLLECTION RULE:
  * every Epic/Legendary EQUIPMENT record in data/oracle/catalogue.json (unchanged), and
  * every RELIC (`isRelic`, Class ItemArtifact) at ANY classification - the 21 Rare b-series
    relics joined the 21 Epic and 49 Legendary ones, so the Relics group holds 91 and the
    collection 3,287 records (was 70 and 3,266), and
  * every record on build_catalogue.py's curated inclusion list (`isExtra`,
    EXTRA_RECORDS) whatever its classification - today exactly Leovinus' Ring, a `Common`
    ArmorJewelry_Ring, so the Rings group holds 264 and the collection 3,288.
Grouped by slot in SLOT_ORDER; each slot group gets its own cell size (the largest icon in the
group) and therefore its own column / row count, so a page holds as many boxes as the band
really fits.  Inside a group the order is Legendary, then Epic, then Rare, then Common, each by
name - so adding a rarity tier APPENDS boxes and never renumbers the ones that already existed.

    python build_uniq_db.py [--limit-pages N]
    python build_uniq_db.py --annotate-groups    (uniq-groups.txt E lines only, no game access)

The full build emits BOTH the .arz and an already-annotated uniq-groups.txt, so
--annotate-groups is only for repairing an existing tree without touching the archive.
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gdpath                                                       # noqa: E402
from arc import ArcSet                                              # noqa: E402
from arz import ArzArchive, ArzDatabase, FT_INT, FT_STRING          # noqa: E402
from arzw import ArzWriter, dbr_text, read_typed                    # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ARZ_DIR = os.path.join(ROOT, "data", "uniq")       # shipped: the two archives
ORACLE = os.path.join(ROOT, "data", "oracle")     # fixtures: the txt lists, the manifest, sample dbr
SRC_DIR = os.path.join(ORACLE, "src")
GAME = gdpath.find_game_dir() or ""

# Kept in step with build_catalogue.py's constants of the same name, and asserted
# against catalogue.json in pick_items() so a stale catalogue.json cannot be paged silently.
RELIC_EXPECT_TOTAL = 91           # 21 Rare + 21 Epic + 49 Legendary, records/items/gearrelic/
EXTRA_EXPECT_TOTAL = 1            # build_catalogue.py's EXTRA_RECORDS
COLLECTION_EXPECT_TOTAL = 3288    # 3,196 Epic/Leg equipment + 91 relics + 1 curated extra

MATERIAL_WINDOW = "records/ui/caravan/caravan_materialwindow.dbr"
PLAIN_PLATE = "records/ui/caravan/caravan_transfercoverimage.dbr"
PAGE_FMT = "records/ui/caravan/uniq_p%02d.dbr"
BOX_FMT = "records/ui/caravan/reagents/uniq/p%02d/box_%02d.dbr"
BOX_TPL = "database/templates/ingameui/uireagentitem.tpl"

# THE LIVE RE-POINT.  The HUD always builds ONE record - the frame - and the DLL then
# re-points its boxes in place with the engine's own UIReagentItem::Load / SetItem /
# SetPosition.  The frame lists the 24 REAL vanilla box records
# first, so the page a character loads with is bit-identical to the vanilla Crafting Materials
# page, then N_MAX-24 filler boxes parked far outside the window.
FRAME_REC = "records/ui/caravan/uniq_frame.dbr"
HIDE_FMT = "records/ui/caravan/reagents/uniq/hide_%03d.dbr"
N_MAX = 160          # the largest page this tool produces; also the exe's proven box count
HIDE_XY = -4000      # record pixels; multiplied by the UI scale, so always off-screen

# THE GRID is not one size for every page.  A box takes the item bitmap's
# NATURAL size (there is no box-size field and no scaling), so the cell only has to be as big as
# the LARGEST icon in that slot group: rings / amulets / medals / relics are 32x32 and fit
# 10 x 16 = 160 to a page, while the 1,076 records with a taller icon (chests, off-hands,
# two-handers - 96 and 128 px) get pages of their own with a bigger cell.  The margins:
#   x from 105 (the transfer-tab column ends at 100) to the caravan window's width, 438 -> 333 px
#   y from  76 (the vanilla materials page's first row) to 715-88 = 627                 -> 551 px
COL_X0, ROW_Y0 = 105, 76
USABLE_W, USABLE_H = 438 - COL_X0, 627 - ROW_Y0
ROW_GAP = 2          # the vanilla 64 px rows sit on a 66 px pitch
MAX_PER_PAGE = 160   # the vanilla components page carries 108 boxes; stay in that order

# The order the pages are laid out in: armour, then jewellery, then off-hands, then weapons.
SLOT_ORDER = ["head", "shoulders", "chest", "hands", "waist", "legs", "feet",
              "amulet", "medal", "ring", "offhand", "shield",
              "axe1h", "dagger", "mace1h", "scepter", "sword1h", "ranged1h",
              "axe2h", "mace2h", "spear2h", "sword2h", "ranged2h", "relic"]

SLOT_LABEL = {
    "head": "Helms", "shoulders": "Shoulders", "chest": "Chest", "hands": "Gloves",
    "waist": "Belts", "legs": "Pants", "feet": "Boots", "amulet": "Amulets",
    "medal": "Medals", "ring": "Rings", "offhand": "Off-hands", "shield": "Shields",
    "axe1h": "1H Axes", "dagger": "Daggers", "mace1h": "1H Maces", "scepter": "Scepters",
    "sword1h": "1H Swords", "ranged1h": "Guns", "axe2h": "2H Axes", "mace2h": "2H Maces",
    "spear2h": "Spears", "sword2h": "2H Swords", "ranged2h": "2H Guns", "relic": "Relics",
}


def tex_size(data):
    """(width, height) of a Grim Dawn .tex: 'TEX\\x02', then a DDS header at +12."""
    if not data or len(data) < 32 or data[:3] != b"TEX":
        return None
    h = struct.unpack_from("<I", data, 24)[0]
    w = struct.unpack_from("<I", data, 28)[0]
    if not (0 < w <= 1024 and 0 < h <= 1024):
        return None
    return (w, h)


class Bitmaps:
    """Natural icon sizes, read out of resources/Items.arc (keys drop the 'items/' prefix)."""

    def __init__(self):
        self.arc = ArcSet.load(GAME, "resources/Items.arc")
        self.cache = {}

    def size(self, bitmap):
        if not bitmap:
            return None
        if bitmap in self.cache:
            return self.cache[bitmap]
        key = bitmap.replace("\\", "/").lower()
        data = self.arc.read(key)
        if data is None and key.startswith("items/"):
            data = self.arc.read(key[len("items/"):])
        size = tex_size(data)
        self.cache[bitmap] = size
        return size


def pick_items(args, bitmaps):
    cat = json.load(open(os.path.join(ORACLE, "catalogue.json"), encoding="utf-8"))
    # The collection rule. Equipment is Epic/Legendary only; a relic is in at EVERY
    # classification (the Rare b-series included), which is why the test is not one predicate.
    # build_catalogue.py already asserted the relic counts and the total; this is the same
    # rule restated on its output, and the assert below proves the two agree.
    # `isExtra` is the curated inclusion list. It is a THIRD predicate on purpose -
    # a curated record is neither Epic/Legendary equipment nor a relic, and folding it into
    # either test would silently widen that rule.
    items = [i for i in cat["items"]
             if i.get("isRelic")
             or i.get("isExtra")
             or (i.get("isEquipment") and i.get("itemClassification") in ("Epic", "Legendary"))]
    n_relics = sum(1 for i in items if i.get("isRelic"))
    assert n_relics == RELIC_EXPECT_TOTAL, (
        "catalogue.json carries %d relics, expected %d - rebuild catalogue.json first "
        "(python build_catalogue.py)" % (n_relics, RELIC_EXPECT_TOTAL))
    n_extra = sum(1 for i in items if i.get("isExtra"))
    assert n_extra == EXTRA_EXPECT_TOTAL, (
        "catalogue.json carries %d curated extras, expected %d - rebuild catalogue.json first "
        "(python build_catalogue.py)" % (n_extra, EXTRA_EXPECT_TOTAL))
    assert len(items) == COLLECTION_EXPECT_TOTAL, (
        "collection is %d records, expected %d" % (len(items), COLLECTION_EXPECT_TOTAL))
    fitted, dropped = [], 0
    for i in items:
        wh = bitmaps.size(i.get("bitmap"))
        # Only an icon too big for the whole band has to go now.
        if wh and wh[0] <= USABLE_W and wh[1] <= USABLE_H:
            i["bitmapWH"] = wh
            fitted.append(i)
        else:
            dropped += 1
    order = {s: n for n, s in enumerate(SLOT_ORDER)}
    # Rare ranks after Epic, so the 21 b-series relics land at the END of the Relics group and
    # every box above them keeps its index and its position - a tier added later must never
    # renumber the boxes a player has already seen.  Common ranks after Rare for the same
    # reason: Leovinus' Ring is appended to the END of the Rings group (box 103 of the group's
    # second page) and no ring box moves.  Ordering INSIDE a tier is name, then record.
    rank = {"Legendary": 0, "Epic": 1, "Rare": 2, "Common": 3}
    fitted.sort(key=lambda i: (order.get(i.get("slot"), 99),
                               rank.get(i["itemClassification"], 9),
                               i["name"].lower(), i["record"]))
    return fitted, dropped


def slot_grid(group):
    """(cellW, pitchY, cols, rows) for one slot group - the cell is its largest icon."""
    cw = max(i["bitmapWH"][0] for i in group)
    ch = max(i["bitmapWH"][1] for i in group)
    cols = max(1, USABLE_W // cw)
    rows = max(1, USABLE_H // (ch + ROW_GAP))
    while cols * rows > MAX_PER_PAGE and rows > 1:
        rows -= 1
    return cw, ch + ROW_GAP, cols, rows


def paginate(items, limit_pages):
    """[(items, cellW, pitchY, cols, rows), ...] - a new page at every slot boundary."""
    groups, order = {}, []
    for it in items:
        s = it.get("slot")
        if s not in groups:
            groups[s] = []
            order.append(s)
        groups[s].append(it)
    pages = []
    for s in order:
        group = groups[s]
        cw, ph, cols, rows = slot_grid(group)
        per = cols * rows
        for n in range(0, len(group), per):
            pages.append((group[n:n + per], cw, ph, cols, rows))
    if limit_pages:
        pages = pages[:limit_pages]
    return pages


def annotate_groups() -> int:
    """Add the ITEM record to every E line of an EXISTING uniq-groups.txt.

    The full build needs the game database and rewrites uniq-pages.arz and every .dbr source;
    this mode touches nothing but uniq-groups.txt, reads no game file, and is provably
    equivalent to a full rebuild for the E lines: the box -> item mapping comes from
    uniq-pages.json, which the build that produced the shipped .arz wrote alongside it.
    Every E line must be matched or nothing is written.
    """
    gpath = os.path.join(ORACLE, "uniq-groups.txt")
    jpath = os.path.join(ORACLE, "uniq-pages.json")
    manifest = json.load(open(jpath, encoding="utf-8"))
    box2item = {}
    for m in manifest["pages"]:
        for it in m["items"]:
            box2item[BOX_FMT % (m["page"], it["box"])] = it["record"]
    print("uniq-pages.json: %d pages, %d box -> item records"
          % (len(manifest["pages"]), len(box2item)))

    out = []
    seen = annotated = already = 0
    with open(gpath, encoding="latin-1") as fh:
        for line in fh:
            line = line.rstrip("\r\n")
            if not line.startswith("E\t"):
                out.append(line)
                continue
            seen += 1
            parts = line.split("\t")
            if len(parts) >= 3 and parts[2]:
                already += 1
                out.append(line)
                continue
            item = box2item.get(parts[1])
            assert item, "no item record for box %r" % (parts[1],)
            out.append("E\t%s\t%s" % (parts[1], item))
            annotated += 1
    with open(gpath, "w", encoding="latin-1", newline="\r\n") as fh:
        for line in out:
            fh.write(line + "\n")
    print("uniq-groups.txt: %d E lines, %d annotated, %d already carried an item record"
          % (seen, annotated, already))
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--annotate-groups", action="store_true",
                    help="only add the item record to uniq-groups.txt (no game access, "
                         "nothing else is rewritten)")
    ap.add_argument("--limit-pages", type=int, default=0, help="stop after N pages (debug)")
    args = ap.parse_args()
    if args.annotate_groups:
        return annotate_groups()

    os.makedirs(SRC_DIR, exist_ok=True)
    db = ArzDatabase.load_game(GAME)
    src_tag = db.source_of(MATERIAL_WINDOW)
    arc = db.archives[db._index[MATERIAL_WINDOW]]
    vanilla = read_typed(arc, MATERIAL_WINDOW)
    print("vanilla %s from %s: %d fields" % (MATERIAL_WINDOW, src_tag, len(vanilla)))

    van_plate = db.get(MATERIAL_WINDOW).get("TransferPlate")
    plate_old = db.get(van_plate) if van_plate else {}
    plate_new = db.get(PLAIN_PLATE)
    assert plate_new, "%s is not in the database" % PLAIN_PLATE
    assert plate_new["templateName"] == plate_old["templateName"], (
        "plate template mismatch: %r vs %r" % (plate_new["templateName"],
                                               plate_old["templateName"]))

    bitmaps = Bitmaps()
    items, dropped = pick_items(args, bitmaps)
    pages = paginate(items, args.limit_pages)
    print("selection: %d collectible records - %d Epic/Legendary equipment + %d relics of every"
          " classification + %d curated (%d skipped, icon bigger than the %dx%d band) -> %d pages,"
          " per-slot grids"
          % (len(items),
             sum(1 for i in items if not i.get("isRelic") and not i.get("isExtra")),
             sum(1 for i in items if i.get("isRelic")),
             sum(1 for i in items if i.get("isExtra")),
             dropped, USABLE_W, USABLE_H, len(pages)))

    writer = ArzWriter()
    manifest_pages = []
    all_records = []
    for p, (page_items, cellW, pitchY, cols, rows) in enumerate(pages):
        box_records = []
        page_manifest = []
        for n, it in enumerate(page_items):
            col, row = n % cols, n // cols
            x, y = COL_X0 + cellW * col, ROW_Y0 + pitchY * row
            rec = BOX_FMT % (p, n + 1)
            fields = [
                ("templateName", FT_STRING, [BOX_TPL]),
                ("FileDescription", FT_STRING, ["UIItemReagent"]),
                ("itemBoxX", FT_INT, [x]),
                ("itemBoxY", FT_INT, [y]),
                ("reagentName", FT_STRING, [it["record"]]),
            ]
            writer.add(rec, fields)
            box_records.append(rec)
            wh = it.get("bitmapWH") or (0, 0)
            page_manifest.append({
                "box": n + 1, "record": it["record"], "name": it["name"],
                "slot": it.get("slot"), "classification": it["itemClassification"],
                "bitmapW": wh[0], "bitmapH": wh[1], "x": x, "y": y,
            })
            all_records.append(it["record"])

        page_rec = PAGE_FMT % p
        fields = []
        for name, ftype, vals in vanilla:
            if name == "reagentBoxes":
                fields.append((name, FT_STRING, box_records))
            elif name == "TransferPlate":
                fields.append((name, FT_STRING, [PLAIN_PLATE]))
            else:
                fields.append((name, ftype, vals))
        if not any(n == "reagentBoxes" for n, _, _ in fields):
            fields.append(("reagentBoxes", FT_STRING, box_records))
        writer.add(page_rec, fields, rtype=arc.entries[MATERIAL_WINDOW].rtype,
                   mtime=arc.entries[MATERIAL_WINDOW].mtime)
        if p == 0:
            open(os.path.join(SRC_DIR, "uniq_p00.dbr"), "w",
                 encoding="latin-1").write(dbr_text(fields))
            open(os.path.join(SRC_DIR, "p00_box_01.dbr"), "w", encoding="latin-1").write(
                dbr_text([("templateName", FT_STRING, [BOX_TPL]),
                          ("FileDescription", FT_STRING, ["UIItemReagent"]),
                          ("itemBoxX", FT_INT, [COL_X0]), ("itemBoxY", FT_INT, [ROW_Y0]),
                          ("reagentName", FT_STRING, [page_items[0]["record"]])]))
        slot = page_items[0].get("slot")
        same = [id(q[0]) for q in pages if q[0][0].get("slot") == slot]
        manifest_pages.append({
            "page": p, "record": page_rec, "slot": slot,
            "label": SLOT_LABEL.get(slot, slot or "?"),
            "sub": same.index(id(page_items)) + 1, "subOf": len(same),
            "cols": cols, "rows": rows, "cellW": cellW, "cellH": pitchY - ROW_GAP,
            "boxes": len(box_records), "items": page_manifest,
        })

    # ---- the frame record + the hidden filler boxes -------------------------------------
    van_boxes = list(arc.get(MATERIAL_WINDOW).get("reagentBoxes") or [])
    assert van_boxes, "the vanilla materials page has no reagentBoxes"
    assert len(van_boxes) <= N_MAX, "%d vanilla boxes > N_MAX" % len(van_boxes)
    hide_item = db.get(van_boxes[0]).get("reagentName")
    assert hide_item, "cannot read reagentName out of %s" % van_boxes[0]
    hidden_recs = []
    for n in range(N_MAX - len(van_boxes)):
        rec = HIDE_FMT % n
        writer.add(rec, [
            ("templateName", FT_STRING, [BOX_TPL]),
            ("FileDescription", FT_STRING, ["UIItemReagent"]),
            ("itemBoxX", FT_INT, [HIDE_XY]),
            ("itemBoxY", FT_INT, [HIDE_XY]),
            ("reagentName", FT_STRING, [hide_item]),
        ])
        hidden_recs.append(rec)
    frame_fields = []
    for name, ftype, vals in vanilla:
        if name == "reagentBoxes":
            frame_fields.append((name, FT_STRING, van_boxes + hidden_recs))
        else:
            frame_fields.append((name, ftype, vals))
    if not any(n == "reagentBoxes" for n, _, _ in frame_fields):
        frame_fields.append(("reagentBoxes", FT_STRING, van_boxes + hidden_recs))
    writer.add(FRAME_REC, frame_fields, rtype=arc.entries[MATERIAL_WINDOW].rtype,
               mtime=arc.entries[MATERIAL_WINDOW].mtime)
    open(os.path.join(SRC_DIR, "uniq_frame.dbr"), "w",
         encoding="latin-1").write(dbr_text(frame_fields))
    print("frame %s: %d vanilla boxes + %d hidden (filler item %s)"
          % (FRAME_REC, len(van_boxes), len(hidden_recs), hide_item))

    # ---- the group model the DLL scrolls through ----------------------------------------
    # A GROUP is one slot: every page built for that slot, concatenated, so the DLL
    # can scroll it by rows through the frame's N_MAX boxes.
    groups = []
    for m in manifest_pages:
        if groups and groups[-1]["slot"] == m["slot"]:
            g = groups[-1]
        else:
            g = {"slot": m["slot"], "label": m["label"], "cellW": m["cellW"],
                 "cellH": m["cellH"], "cols": m["cols"], "entries": []}
            groups.append(g)
        for it in m["items"]:
            # The BOX record is what the widget shows, the ITEM record is what the engine's
            # reagent map is keyed by. The DLL needs both.
            g["entries"].append((BOX_FMT % (m["page"], it["box"]), it["record"]))
    for g in groups:
        rows = max(1, USABLE_H // (g["cellH"] + ROW_GAP))
        while g["cols"] * rows > N_MAX and rows > 1:
            rows -= 1
        g["rows"] = rows

    with open(os.path.join(ORACLE, "uniq-groups.txt"), "w", encoding="latin-1", newline="\r\n") as fh:
        fh.write("F\t%s\t%d\t%d\t%d\t%d\t%d\n"
                 % (FRAME_REC, N_MAX, len(van_boxes), COL_X0, ROW_Y0, ROW_GAP))
        for r in van_boxes:
            fh.write("V\t%s\n" % r)
        for n, g in enumerate(groups):
            fh.write("G\t%d\t%s\t%d\t%d\t%d\t%d\t%d\n"
                     % (n, g["label"], g["cols"], g["rows"], g["cellW"], g["cellH"],
                        len(g["entries"])))
            for r in g["entries"]:
                fh.write("E\t%s\t%s\n" % (r[0], r[1]))
    print("uniq-groups.txt: %d groups, %d entries, frame %d boxes"
          % (len(groups), sum(len(g["entries"]) for g in groups), N_MAX))
    for g in groups:
        print("  group %-12s %4d items  %2d cols x %2d rows  cell %dx%d"
              % (g["label"], len(g["entries"]), g["cols"], g["rows"], g["cellW"], g["cellH"]))

    path = os.path.join(ARZ_DIR, "uniq-pages.arz")
    size = writer.write(path)
    print("wrote %s (%d bytes, %d pages + %d boxes)"
          % (path, size, len(pages), len(all_records)))

    # ---- verification -----------------------------------------------------------------
    back = ArzArchive(path, "uniq")
    assert len(back) == len(pages) + len(all_records) + 1 + len(hidden_recs), "read-back count"
    fr = back.get(FRAME_REC)
    assert len(fr["reagentBoxes"]) == N_MAX, len(fr["reagentBoxes"])
    assert fr["reagentBoxes"][:len(van_boxes)] == van_boxes, "frame lost the vanilla boxes"
    p0 = back.get(PAGE_FMT % 0)
    assert p0["reagentBoxes"] == [BOX_FMT % (0, n + 1) for n in range(len(pages[0][0]))]
    assert p0["TransferPlate"] == PLAIN_PLATE
    for key in ("templateName", "WindowLocationX", "WindowLocationY",
                "backgroundShadeReduction", "pickUpSoundName"):
        van = arc.get(MATERIAL_WINDOW).get(key)
        assert p0.get(key) == van, "field %s changed: %r -> %r" % (key, van, p0.get(key))
    assert MATERIAL_WINDOW not in back.entries, "the page archive must NOT touch the vanilla record"
    b1 = back.get(BOX_FMT % (0, 1))
    assert b1["itemBoxX"] == COL_X0 and b1["itemBoxY"] == ROW_Y0, b1
    assert b1["reagentName"] == pages[0][0][0]["record"]

    probe = "records/ui/caravan/reagents/materials/caravan_reagent001.dbr"
    parc = db.archives[db._index[probe]]
    tw = ArzWriter()
    tw.add(probe, read_typed(parc, probe), rtype=parc.entries[probe].rtype,
           mtime=parc.entries[probe].mtime)
    tmp = os.path.join(ARZ_DIR, "_roundtrip.arz")
    tw.write(tmp)
    rt = ArzArchive(tmp, "rt")
    assert rt.get(probe) == parc.get(probe), "round-trip differs"
    os.remove(tmp)
    print("verify OK: read-back, vanilla fields preserved on every page, vanilla record untouched")

    manifest = {
        "mechanism": "ObjectManager::LoadTableFile/GetLoadTable path substitution",
        "vanillaRecord": MATERIAL_WINDOW,
        "pageArchive": "uniq-pages.arz",
        "grid": "per slot group: the cell is the largest icon in the group",
        "x0": COL_X0, "y0": ROW_Y0, "usableW": USABLE_W, "usableH": USABLE_H,
        "rowGap": ROW_GAP, "maxPerPage": MAX_PER_PAGE,
        "transferPlate": PLAIN_PLATE, "transferPlateWas": van_plate,
        "pageCount": len(pages), "itemCount": len(all_records), "skipped": dropped,
        "pages": manifest_pages,
    }
    with open(os.path.join(ORACLE, "uniq-pages.json"), "w", encoding="utf-8") as fh:
        json.dump(manifest, fh, indent=1)

    # The DLL reads two plain text files: the gate list, and the page index (one line per
    # page: "<n>\t<record>\t<label> k/m\t<boxes>") so the button can show a label.
    with open(os.path.join(ORACLE, "uniq-records.txt"), "w", encoding="latin-1", newline="\r\n") as fh:
        for r in all_records:
            fh.write(r + "\n")
    with open(os.path.join(ORACLE, "uniq-pages.txt"), "w", encoding="latin-1", newline="\r\n") as fh:
        for m in manifest_pages:
            fh.write("%d\t%s\t%s %d/%d\t%d\n"
                     % (m["page"], m["record"], m["label"], m["sub"], m["subOf"], m["boxes"]))
    print("manifest + uniq-records.txt (%d) + uniq-pages.txt (%d) written"
          % (len(all_records), len(manifest_pages)))
    for m in manifest_pages:
        print("  page %2d  %-12s %d/%d  %3d boxes  %2dx%-2d of %dx%-3d  %s"
              % (m["page"], m["label"], m["sub"], m["subOf"], m["boxes"], m["cols"], m["rows"],
                 m["cellW"], m["cellH"], m["record"]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
