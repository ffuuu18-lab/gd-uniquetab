"""Reference implementation; the DLL generates these files itself now (src/gen/catalogue_gen).

Build data/oracle/catalogue.json + catalogue-stats.md from the four .arz databases.

    python build_catalogue.py [--game <dir>] [--out <dir>] [--basemods] [--custom <mod dir>]
                              [--lang EN]

THE VIEW (--basemods, --custom): the catalogue is built from the database the engine will have -
the four base databases, then mods/database.arz, then the custom game's database/*.arz, the
last layer that has a record winning (src/gen/catalogue_gen.cpp does the same). A record a MOD
layer wins is judged by the mod rules: a class the slot table does not know admitted as slot
"other" (Epic/Legendary only), and a classification catalogue.bin cannot store skipped with a
warning; a mod's own augments are admitted at every classification. The path rule below
(JUNK_PREFIX/JUNK_PART) is every layer's. Names: the base
Text_<lang>.arc, then the mod's text_en*.arc, then its text_<lang>*.arc over it; icons: the
mod's .arc files, then the base archives. The
asserts below are the vanilla database's and are not checked in a view.

One entry per shipped item record that the collection rule accepts, plus separate lists for
Rare-classification equipment records (Monster Infrequents / unique rares) and faction store
items. Nothing is written outside <out>.

THE COLLECTION RULE:
  * equipment (the EQUIP_SLOT classes): itemClassification Epic or Legendary;
  * relics (Class `ItemArtifact`, which in the shipped database means `records/items/gearrelic/*`):
    EVERY classification, so the 21 Rare b-series relics count alongside the 21 Epic c-series and
    the 49 Legendary d-series -> 91 relics, and the collection total 3,287.
    Rationale: a relic is a unique-tier item to the player whatever the record's rarity word says,
    and the b-series would otherwise be the one tier of relic with no box in the tab.

Shipped-record filter (the records/items/ part of it reproduces the grimtools 1.3.0.8 totals:
equipment Epic 1,650 vs 1,649 and Legendary 1,546 vs 1,547, sum 3,196 = 3,196; relics Epic+
Legendary 70 = 70 - grimtools lists no Rare relics, the 21 are measured from the game DB; the nine
Legendary records outside records/items/ come on top, 3,205):
  1. the path is in no junk folder (JUNK_PREFIX) -> drops records/sandbox/** (dev test items),
     records/creatures/** (the gear NPCs wear), records/mod_testgrounds/**; anywhere else is in,
     so the real items outside records/items/ are collected (the Lokarr set in
     records/storyelements/signs/, the quest rewards in records/storyelements/rewards/, Wilhelm's
     Wondrous Wargem, Gazer Man, the Totally Normal Shield's three versions in
     records/endlessdungeon/scriptentities/ - the folder's other records are ScriptEntity and
     Decoration props, which no rule collects)
  2. no `/enemygear/` in the path (JUNK_PART)     -> monster-worn gear, never dropped
  3. the item's name tag resolves in Text_EN       -> drops leftovers with dangling tags
  4. FileDescription does not contain "BLANK"      -> drops the `?000/?100/?200/?300` template records
     ("BASE BLANK EPIC 2H AXE" etc.), exactly 4 per equipment slot per classification

EXTRA_RECORDS is a small CURATED INCLUSION LIST that bypasses the rank rule for named items a
player really owns that are neither Epic nor Legendary - today exactly one, Leovinus' Ring
(Common). EXTRA_EXCLUDE names the developer test records the four rules would admit and that must
never be collectible. Both lists are asserted, so a game patch that moves either is loud instead
of silent.

MONSTER INFREQUENTS (--collect-mi = uniquetab.ini's collect_mi=1, the DLL's default; this tool's
default stays off, so the flag-off cut needs no switch): every equipment
record (MI_CLASSES: the EQUIP_SLOT classes and the two per-class groups' WeaponMagical_Staff and
WeaponHunting_Spear) whose own itemClassification is Rare - the engine's MI rule - through the same
shipped filter (a mod layer's: the mod rule), the faction-store rares and the blacksmith-crafted
ones included: is_mi_candidate,
catalogue_gen.cpp isMiCandidate. Each entry carries "isMi" (the KIND mark; catalogue.bin flag bit 8,
pack_catalogue.py F_MI). Without the flag no MI is admitted and the output is what it always was
(a stored MI keeps its journal row and is on no page). With it the vanilla asserts do not
apply and only the items are written (write_view), like a view.
"""

from __future__ import annotations

import argparse
import collections
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import arc  # noqa: E402
import gdpath  # noqa: E402
import arz  # noqa: E402
import ui_geometry  # noqa: E402

DEFAULT_GAME = gdpath.find_game_dir() or ""
DEFAULT_OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "data", "oracle")

# ---------------------------------------------------------------- class -> slot
EQUIP_SLOT = {
    "ArmorProtective_Head": "head",
    "ArmorProtective_Shoulders": "shoulders",
    "ArmorProtective_Chest": "chest",
    "ArmorProtective_Hands": "hands",
    "ArmorProtective_Legs": "legs",
    "ArmorProtective_Feet": "feet",
    "ArmorProtective_Waist": "waist",
    "ArmorJewelry_Ring": "ring",
    "ArmorJewelry_Amulet": "amulet",
    "ArmorJewelry_Medal": "medal",
    "WeaponMelee_Sword": "sword1h",
    "WeaponMelee_Axe": "axe1h",
    "WeaponMelee_Mace": "mace1h",
    "WeaponMelee_Dagger": "dagger",
    "WeaponMelee_Scepter": "scepter",
    "WeaponMelee_Sword2h": "sword2h",
    "WeaponMelee_Axe2h": "axe2h",
    "WeaponMelee_Mace2h": "mace2h",
    "WeaponMelee_Spear2h": "spear2h",
    "WeaponHunting_Ranged1h": "ranged1h",
    "WeaponHunting_Ranged2h": "ranged2h",
    "WeaponArmor_Offhand": "offhand",
    "WeaponArmor_Shield": "shield",
}
# catalogue_gen.cpp isEquipmentClass: the slot classes and the two equipment classes the base game
# has no slot for, each with a per-class group of its own (staff, spear1h) - only a mod layer has
# them, admitted as "other". "isEquipment" stays EQUIP_SLOT (catalogue.bin unchanged); the KIND
# mark and the MI rule use this set.
MI_CLASSES = frozenset(EQUIP_SLOT) | {"WeaponMagical_Staff", "WeaponHunting_Spear"}
OTHER_SLOT = {
    "ItemArtifact": "relic",
    "ItemArtifactFormula": "blueprint",
    "ItemRelic": "component",
    "ItemEnchantment": "augment",
    "ItemAscensionFormula": "blueprint",
    "AscendantAltarFormula": "blueprint",
    "ItemRandomSetFormula": "blueprint",
    "ItemRerollFormula": "blueprint",
    "ItemSetFormula": "blueprint",
    "ItemFactionBooster": "consumable",
    "ItemFactionWarrant": "consumable",
    "ItemDifficultyUnlock": "consumable",
    "ItemAttributeReset": "consumable",
    "ItemDevotionReset": "consumable",
    "OneShot_SkillUnlock": "consumable",
    "QuestItem": "quest",
    "LootRandomizer": "affix",
}
EQUIP_PREFIXES = ("ArmorJewelry_", "ArmorProtective_", "WeaponArmor_", "WeaponHunting_", "WeaponMelee_")

# grimtools 1.3.0.8 per-slot Epic/Legendary counts, read off the site
GRIMTOOLS = {
    "head": (135, 153), "shoulders": (75, 133), "chest": (100, 135), "hands": (75, 61),
    "waist": (82, 85), "legs": (53, 60), "feet": (56, 50),
    "sword1h": (55, 42), "axe1h": (48, 36), "mace1h": (49, 53), "dagger": (52, 62),
    "scepter": (51, 55), "ranged1h": (74, 56), "offhand": (87, 85), "shield": (68, 49),
    "sword2h": (28, 20), "axe2h": (30, 17), "mace2h": (18, 21), "ranged2h": (79, 48),
    "spear2h": (4, 2), "ring": (165, 97), "amulet": (150, 138), "medal": (115, 89),
    "relic": (21, 49),
}
GRIMTOOLS_TOTALS = {
    "equipment_epic": 1649, "equipment_legendary": 1547, "equipment_total": 3196,
    "relics_epic_legendary": 70, "item_sets": 199, "total_items": 8301,
    "monster_infrequent_records": 2489, "unique_rare_items": 66, "awakened_items": 92,
}

# Relics of EVERY classification are collectible. grimtools' relic page lists
# only the Epic + Legendary tiers (70); the Rare b-series count is measured from the game
# database itself (records/items/gearrelic/b*.dbr) and asserted here so a game patch that adds
# or removes one is LOUD instead of silent.
RELIC_CLASS = "ItemArtifact"
RELIC_PATH = "records/items/gearrelic/"
RELIC_EXPECT ={"Rare": 21, "Epic": 21, "Legendary": 49}     # 1.3.0.8 build 24825149
RELIC_EXPECT_TOTAL = 91

# ------------------------------------------------------------ curated inclusion list
# Records the rank rule DROPS (neither Epic nor Legendary, nor a relic) but that a player can
# really own, so the tab must have a box for them. Each entry bypasses `is_shipped()` and is asserted
# below: it must exist in the merged database, its name tag must resolve in Text_EN, and its
# bitmap must be in `resources/Items.arc`. One line per record, with the reason it is here.
# THIS IS A CURATED LIST, NOT A RULE - do not turn it into a path prefix or a rank. The folder it
# reaches into also holds developer test items (see EXTRA_EXCLUDE).
EXTRA_RECORDS = {
    # Leovinus' Ring - the fixed named ring the Shattered Realm hands out.
    #   Class ArmorJewelry_Ring, itemClassification Common, itemNameTag
    #   tagGDX2EndlessDungeon_S203, bitmap items/gearaccessories/rings/a001_ring.tex (32x32,
    #   gdx2), levelRequirement 10.
    # Without this entry the ring cannot be deposited at all: it is neither Epic nor Legendary,
    # so it would never reach the catalogue, the gate
    # byte would never be set on it, the drag would be refused and the shift-click would fall
    # through to the normal transfer stash.
    "records/endlessdungeon/items/a001_ring.dbr",
}

# The two DEVELOPER TEST records that share that folder. Both are `Legendary`
# equipment and would sail through the four rules, so they are named and excluded explicitly and
# the build ASSERTS neither ever reaches `items`.
EXTRA_EXCLUDE = {
    # "Mildly Amusing Box" (tagGDX2ItemTest2) - ArmorProtective_Head, Legendary, bitmap
    # system/textures/z000_test.tex, which is not even in resources/Items.arc. GDX2 test record.
    "records/endlessdungeon/items/z001_test.dbr",
    # "Miss Gazer Man" (tagGDX2ItemTest) - ArmorProtective_Chest, Legendary, no
    # levelRequirement, itemLevel 1. The other GDX2 test record.
    "records/endlessdungeon/items/q001_torso.dbr",
}
EXTRA_EXPECT_TOTAL = 1                                        # == len(EXTRA_RECORDS), asserted

EQUIPMENT_EPICLEG_EXPECT = 3205                               # Epic/Legendary equipment records
COLLECTION_EXPECT_TOTAL = 3297    # 3,205 Epic/Legendary equipment + 91 relics + 1 curated extra


BLUEPRINT_CLASSES = ("ItemArtifactFormula", "ItemAscensionFormula", "AscendantAltarFormula",
                     "ItemRandomSetFormula", "ItemRerollFormula", "ItemSetFormula")
# The blueprint a player learns at the blacksmith. It is collected at EVERY classification (a
# recipe is a recipe: Magical and Rare ones too), base and mod alike; its five siblings above
# (the altar's formulas, the ascension/reroll/set ones) are not recipes and never collected.
FORMULA_CLASS = "ItemArtifactFormula"


# Where a record is never collected from, in the base game and a mod layer alike (the path rule).
JUNK_PREFIX = ("records/sandbox/", "records/creatures/", "records/mod_testgrounds/")
JUNK_PART = "/enemygear/"
STORABLE = ("Common", "Rare", "Epic", "Legendary")


def storable(cls: str, cla: str) -> bool:
    """catalogue_gen.cpp storable: a blueprint is stored at Magical and Quest too (packed as Epic)."""
    return cla in STORABLE or (cls == FORMULA_CLASS and cla in ("Magical", "Quest"))


def scroll_of(cla: str) -> str:
    """catalogue_gen.cpp scrollOf: the base game's scroll for a blueprint of this classification,
    for one whose artifactFormulaBitmapName is empty - never the crafted item's icon."""
    return {
        "Magical": "items/craftingparts/materials/blueprint_a001_magical.tex",
        "Rare": "items/craftingparts/materials/blueprint_a002_rare.tex",
        "Epic": "items/craftingparts/materials/blueprint_a003_epic.tex",
        "Legendary": "items/craftingparts/materials/blueprint_a004_legendary.tex",
    }.get(cla, "items/misc/blueprint01.tex")


def is_mi_candidate(key: str, cls: str, cla: str, file_description: str) -> bool:
    """catalogue_gen.cpp isMiCandidate: an MI_CLASSES class and itemClassification Rare - the
    faction-store and the blacksmith-crafted rares included. key and file_description are
    unused, kept as the C++ signature keeps them."""
    del key, file_description
    return cla == "Rare" and cls in MI_CLASSES


def load_level_art(game_dir: str):
    """resources/Level Art.arc (., gdx1..3), where the Lokarr set's icons live ('level art/...'),
    or None when the game folder has none (catalogue_gen.cpp GameData::load: optional)."""
    try:
        return arc.ArcSet.load(game_dir, os.path.join("resources", "Level Art.arc"))
    except FileNotFoundError:
        return None


def is_junk(key: str) -> bool:
    return key.startswith(JUNK_PREFIX) or JUNK_PART in key


def strip_colour_codes(s: str) -> str:
    """A name without its colour codes ({^X} and ^X) - the sort key; the stored name keeps them."""
    o, i, n = [], 0, len(s)
    while i < n:
        c = s[i]
        if c == "{" and i + 3 < n and s[i + 1] == "^" and s[i + 3] == "}":
            i += 4
            continue
        if c == "^" and i + 1 < n:
            i += 2
            continue
        o.append(c)
        i += 1
    return "".join(o)


def is_text_arc(path: str, lang: str) -> bool:
    leaf = os.path.basename(path).lower()
    want = ("text_" + lang).lower()
    return leaf.endswith(".arc") and (leaf.startswith(want) or ("_" + want) in leaf)


def tex_wh(data):
    """ui_geometry.cpp texWH: a cover-sized bound (4096)."""
    import struct
    if not data or len(data) < 32 or data[:3] != b"TEX":
        return None
    h = struct.unpack_from("<I", data, 24)[0]
    w = struct.unpack_from("<I", data, 28)[0]
    if not (0 < w <= 4096 and 0 < h <= 4096):
        return None
    return (w, h)


def view_tex_size(view, bitmap: str):
    """ModView::texSize: every mod .arc in load order, the name whole, without its first
    element, and by leaf; the first readable header wins."""
    if not bitmap or view is None:
        return None
    key = bitmap.replace("\\", "/").lower()
    keys = [key, key.split("/", 1)[1] if "/" in key else key, key.rsplit("/", 1)[-1]]
    for a in view.arcs:
        for k in keys:
            if k in a:
                wh = tex_wh(a.read(k))
                if wh:
                    return wh
    return None


def slot_of(cls: str) -> str:
    return EQUIP_SLOT.get(cls) or OTHER_SLOT.get(cls) or "other"


def name_tag_of(rec, cls: str) -> str:
    """Equipment records use itemNameTag; every other item class uses `description`."""
    if cls in EQUIP_SLOT:
        return arz.sfield(rec, "itemNameTag")
    return arz.sfield(rec, "itemNameTag") or arz.sfield(rec, "description")


def crafted_record(rec, cls: str) -> str:
    """Blueprints carry no name or bitmap of their own; both come from the item they craft."""
    if cls in BLUEPRINT_CLASSES:
        return arz.sfield(rec, "artifactName")
    return ""


def bitmap_of(rec, cls: str) -> str:
    """ItemArtifact -> artifactBitmap, ItemRelic -> relicBitmap, everything else -> bitmap; a
    blueprint (ItemArtifactFormula) -> artifactFormulaBitmapName, the scroll the game draws for it
    (64 x 32), before the others. An empty one takes the rarity's scroll (scroll_of, entry())."""
    fields = ("bitmap", "artifactBitmap", "relicBitmap")
    if cls == FORMULA_CLASS:
        fields = ("artifactFormulaBitmapName",) + fields
    for f in fields:
        v = arz.sfield(rec, f)
        if v:
            return v
    return ""


def is_shipped(key: str, rec, cls: str, tags, db=None) -> bool:
    """catalogue_gen.cpp isShipped: the same for every layer - not in a junk folder (is_junk), no
    BLANK in the FileDescription, and a name tag the view's text resolves (a blueprint's: the
    crafted item's)."""
    if is_junk(key):
        return False
    if "BLANK" in arz.sfield(rec, "FileDescription").upper():
        return False
    if name_tag_of(rec, cls) in tags:
        return True
    # a blueprint has no name tag of its own: accept it when the item it crafts has one
    tgt = crafted_record(rec, cls)
    if tgt and db is not None:
        trec = db.get(tgt)
        if trec is not None and name_tag_of(trec, arz.sfield(trec, "Class")) in tags:
            return True
    return False


class LayeredDb:
    """The base database with a view's mod layers on top (get/real_name as ArzDatabase)."""

    def __init__(self, base, layers):
        self.base, self.layers = base, layers

    def get(self, name):
        for a in reversed(self.layers):
            r = a.get(name)
            if r is not None:
                return r
        return self.base.get(name)


def build(game_dir: str, out_dir: str, basemods=False, custom=None, lang="EN", collect_mi=False):
    layered = bool(basemods or custom)
    view = ui_geometry.View(game_dir, basemods, custom) if layered else None
    db = view.db if view else arz.ArzDatabase.load_game(game_dir)
    layers = [a for a, _ in view.layers] if view else []
    tags = arc.load_text_tags(game_dir, lang)
    if view and (layers or view.arcs):
        # the mod's English set, then its set in this language over it (catalogue_gen.cpp)
        text = [a for a in view.arcs if is_text_arc(a.path, "en")]
        if not is_text_arc("text_en.arc", lang):
            text += [a for a in view.arcs if is_text_arc(a.path, lang)]
        for a in text:
            for key in a.keys():
                if key.endswith(".txt"):
                    data = a.read(key)
                    if data:
                        tags.update(arc.parse_tag_file(data))
    items_arc = arc.ArcSet.load(game_dir, os.path.join("resources", "Items.arc"))
    ui_arc = arc.ArcSet.load(game_dir, os.path.join("resources", "UI.arc"))
    level_art_arc = load_level_art(game_dir)
    base_db = db
    if layers:
        db = LayeredDb(base_db, layers)

    def tex_exists(bitmap: str):
        """Bitmap paths are resource paths; the .arc entries drop the leading segment.
        'items/gearhead/bitmaps/x.tex' -> Items.arc entry 'gearhead/bitmaps/x.tex'."""
        if not bitmap:
            return (False, "")
        head, _, rest = bitmap.replace("\\", "/").partition("/")
        head = head.lower()
        if head == "items" and rest in items_arc:
            return (True, "Items.arc:" + (items_arc.source_of(rest) or ""))
        if head == "ui" and rest in ui_arc:
            return (True, "UI.arc:" + (ui_arc.source_of(rest) or ""))
        if head == "level art" and level_art_arc is not None and rest in level_art_arc:
            return (True, "Level Art.arc:" + (level_art_arc.source_of(rest) or ""))
        if view is not None and (layers or view.arcs) and view_tex_size(view, bitmap):
            return (True, "mod")
        return (False, "")

    set_name_cache = {}

    def set_display_name(set_record: str) -> str:
        if not set_record:
            return ""
        if set_record in set_name_cache:
            return set_name_cache[set_record]
        sr = db.get(set_record)
        nm = ""
        if sr:
            nm = tags.get(arz.sfield(sr, "setName"), "") or tags.get(arz.sfield(sr, "description"), "")
        set_name_cache[set_record] = nm
        return nm

    def entry(key, src, rec, cls, real=None, mod_layer=False):
        tag = name_tag_of(rec, cls)
        bmp = bitmap_of(rec, cls)
        if not bmp and cls == FORMULA_CLASS:
            bmp = scroll_of(arz.sfield(rec, "itemClassification"))
        name = tags.get(tag, "")
        crafts = crafted_record(rec, cls)
        if crafts:                                  # blueprint: name + icon come from the target
            trec = db.get(crafts)
            if trec is not None:
                tcls = arz.sfield(trec, "Class")
                ttag = name_tag_of(trec, tcls)
                if not name:
                    name = tags.get(ttag, "")
                    tag = tag or ttag
                if not bmp:
                    bmp = bitmap_of(trec, tcls)
        ok, where = tex_exists(bmp)
        set_rec = arz.sfield(rec, "itemSetName")
        return {
            "record": real or base_db.real_name(key) or key,
            "class": cls,
            "slot": slot_of(cls),
            "craftsRecord": crafts,
            "itemNameTag": tag,
            "name": name,
            "itemClassification": arz.sfield(rec, "itemClassification"),
            "itemSetName": set_rec,
            "setDisplayName": set_display_name(set_rec),
            "levelRequirement": arz.ifield(rec, "levelRequirement"),
            "itemLevel": arz.ifield(rec, "itemLevel"),
            "bitmap": bmp,
            "bitmapFound": ok,
            "bitmapArchive": where,
            "source": src,
            "isBlueprint": cls.endswith("Formula") or "/crafting/blueprints/" in key,
            "isRelicOrComponent": cls == "ItemRelic",
            "isAugment": cls == "ItemEnchantment",
            "isRelic": cls == "ItemArtifact",
            "isSetPiece": bool(set_rec),
            "isEquipment": cls in EQUIP_SLOT,
            # the KIND mark: a Rare equipment record, a monster infrequent (catalogue_gen.cpp isMi)
            "isMi": cls in MI_CLASSES and arz.sfield(rec, "itemClassification") == "Rare",
            # On the curated inclusion list, so build_uniq_db.py can page it
            # without re-deriving the rule (it is neither Epic/Legendary nor a relic).
            "isExtra": key.lower() in EXTRA_RECORDS,
            # Only a mod layer has the record (the groups file's `source = mod`).
            "fromMod": bool(mod_layer and key not in base_db),
        }

    items, mis, faction_rares, rejected = [], [], [], []
    relic_seen = collections.Counter()
    extra_seen = set()
    unstorable = []

    def walk():
        """(key, src, rec, real name, mod layer) for every winning record of the view."""
        for key, src, rec in base_db.items():
            if any(key in a.entries for a in layers):
                continue
            yield key, src, rec, None, False
        for li, a in enumerate(layers):
            for key in a.keys():
                if any(key in b.entries for b in layers[li + 1:]):
                    continue
                yield key, a.tag, a.get(key), a.real_name(key), True

    for key, src, rec, real, mod_layer in walk():
        cls = arz.sfield(rec, "Class")
        cla = arz.sfield(rec, "itemClassification")
        low = key.lower()
        if low in EXTRA_EXCLUDE:               # named dev test records, never in
            continue
        is_extra = low in EXTRA_RECORDS        # curated inclusion, bypasses is_shipped
        known = cls in EQUIP_SLOT or cls in OTHER_SLOT
        if not known and (not mod_layer or not cls):
            continue
        if cls == "LootRandomizer":            # affixes, not items
            continue
        if cls in BLUEPRINT_CLASSES and cls != FORMULA_CLASS:   # not recipes (the altar's ...)
            continue
        is_relic = (cls == RELIC_CLASS)
        is_formula = (cls == FORMULA_CLASS)
        # a mod's own augments and components (no base archive has them: DGD's augments, the
        # D2 gems and runes of ROT and DGD): every classification, like relics
        mod_augment = mod_layer and cls in ("ItemEnchantment", "ItemRelic") and key not in base_db
        if (not is_relic and not is_extra and not mod_augment and not is_formula
                and cla not in ("Epic", "Legendary", "Rare")):
            continue
        if mod_layer:
            # a mod layer's record: the mod rules, and no Rare lists
            admit = is_extra or ((is_relic or mod_augment or is_formula or cla in ("Epic", "Legendary")
                                  or (collect_mi and is_mi_candidate(
                                      key, cls, cla, arz.sfield(rec, "FileDescription"))))
                                 and is_shipped(key, rec, cls, tags, db))
            if not admit:
                continue
            if not storable(cls, cla):
                unstorable.append((real or key, cla))
                continue
            items.append(entry(key, src, rec, cls, real, True))
            if is_extra:
                extra_seen.add(low)
            continue
        # A relic is in the collection at EVERY classification, so it must not be
        # pre-filtered by the rarity word the way equipment is.
        shipped = is_shipped(key, rec, cls, tags, db)
        if is_extra:
            items.append(entry(key, src, rec, cls))
            extra_seen.add(low)
        elif is_relic:
            if shipped:
                items.append(entry(key, src, rec, cls))
                relic_seen[cla] += 1
            else:
                rejected.append((key, cls, cla))
        elif is_formula:                       # a blueprint: every classification, like relics
            if not shipped:
                rejected.append((key, cls, cla))
            elif not storable(cls, cla):       # (Quest) catalogue_gen.cpp skips it, never fails
                unstorable.append((real or key, cla))
            else:
                items.append(entry(key, src, rec, cls))
        elif cla in ("Epic", "Legendary"):
            if shipped:
                items.append(entry(key, src, rec, cls))
            else:
                rejected.append((key, cls, cla))
        elif cls in EQUIP_SLOT and shipped:    # Rare *record* = MI or unique rare or faction item
            e = entry(key, src, rec, cls)
            if collect_mi and is_mi_candidate(key, cls, cla, arz.sfield(rec, "FileDescription")):
                items.append(e)                # collect_mi=1: a monster infrequent is collected
            (faction_rares if key.startswith("records/items/faction/") else mis).append(e)
    if unstorable:
        print("warning: %d mod record(s) of a classification the catalogue cannot store are "
              "skipped: %s" % (len(unstorable), ", ".join("%s (%s)" % u for u in unstorable[:3])),
              file=sys.stderr)

    if layered or collect_mi:
        return write_view(out_dir, items, view, tags)
    # ---- relic asserts. These are the collection rule, not decoration.
    stray = [e["record"] for e in items
             if e["class"] == RELIC_CLASS and not e["record"].lower().startswith(RELIC_PATH)]
    assert not stray, "shipped ItemArtifact outside %s: %r" % (RELIC_PATH, stray[:5])
    n_relics = sum(relic_seen.values())
    assert dict(relic_seen) == RELIC_EXPECT, (
        "relic classification counts changed: %r, expected %r. The game database moved - "
        "re-read them and update RELIC_EXPECT/RELIC_EXPECT_TOTAL/COLLECTION_EXPECT_TOTAL; "
        "do NOT silence this." % (dict(relic_seen), RELIC_EXPECT))
    assert n_relics == RELIC_EXPECT_TOTAL, (n_relics, RELIC_EXPECT_TOTAL)
    no_icon = [e["record"] for e in items if e["class"] == RELIC_CLASS and not e["bitmapFound"]]
    assert not no_icon, "relic bitmap missing from the game resources: %r" % (no_icon[:5],)

    # ---- curated-inclusion asserts. Same standard as the relic asserts.
    assert extra_seen == EXTRA_RECORDS, (
        "EXTRA_RECORDS not all found in the merged database: missing %r, unexpected %r. The game "
        "database moved - re-read the records and update EXTRA_RECORDS; do NOT silence this."
        % (sorted(EXTRA_RECORDS - extra_seen), sorted(extra_seen - EXTRA_RECORDS)))
    assert len(EXTRA_RECORDS) == EXTRA_EXPECT_TOTAL, (len(EXTRA_RECORDS), EXTRA_EXPECT_TOTAL)
    extras = [e for e in items if e["isExtra"]]
    assert len(extras) == EXTRA_EXPECT_TOTAL, (len(extras), EXTRA_EXPECT_TOTAL)
    bad = [e["record"] for e in extras if not e["name"]]
    assert not bad, "curated extra has no name in Text_EN: %r" % (bad,)
    bad = [e["record"] for e in extras if not e["bitmapFound"]]
    assert not bad, "curated extra's bitmap is missing from the game resources: %r" % (bad,)
    banned = [e["record"] for e in items if e["record"].lower() in EXTRA_EXCLUDE]
    assert not banned, "EXCLUDED developer test record reached the collection: %r" % (banned,)

    n_extra = len(extras)
    n_eq = sum(1 for e in items if e["isEquipment"])
    assert n_eq - n_extra == EQUIPMENT_EPICLEG_EXPECT, (
        "Epic/Legendary equipment is %d, expected %d (curated extras counted separately: %d)"
        % (n_eq - n_extra, EQUIPMENT_EPICLEG_EXPECT, n_extra))
    assert n_eq + n_relics == COLLECTION_EXPECT_TOTAL, (
        "collectible total is %d (%d equipment incl. %d curated extra + %d relics), expected %d"
        % (n_eq + n_relics, n_eq, n_extra, n_relics, COLLECTION_EXPECT_TOTAL))

    items.sort(key=lambda e: (e["slot"], e["itemClassification"], e["record"]))
    mis.sort(key=lambda e: (e["slot"], e["record"]))
    faction_rares.sort(key=lambda e: (e["slot"], e["record"]))

    sets = sorted({e["itemSetName"] for e in items if e["itemSetName"]})

    meta = {
        "gameVersion": "1.3.0.8 (build 24825149)",
        "gameDir": game_dir,
        "generated": "tools/build_catalogue.py",
        "archives": [{"tag": a.tag, "records": len(a), "path": a.path} for a in db.archives],
        "mergedRecords": len(db),
        "textTags": len(tags),
        "shippedFilter": ["path in no junk folder (JUNK_PREFIX)", "no /enemygear/ in the path",
                          "name tag resolves in Text_EN", "FileDescription has no BLANK"],
        "collectionRule": "equipment: itemClassification Epic or Legendary; "
                          "relics (Class ItemArtifact): EVERY classification; "
                          "blueprints (Class ItemArtifactFormula, icon artifactFormulaBitmapName): "
                          "EVERY classification, the other *Formula classes never; "
                          "plus the EXTRA_RECORDS curated inclusion list",
        "relicsByClassification": dict(sorted(relic_seen.items())),
        "extraRecords": sorted(EXTRA_RECORDS),
        "extraExcluded": sorted(EXTRA_EXCLUDE),
        "bitmapRule": "resource path 'items/<rest>' -> Items.arc entry '<rest>', 'level art/<rest>' -> "
                      "Level Art.arc entry '<rest>'; "
                      "'ui/<rest>' -> UI.arc entry '<rest>' (leading segment dropped)",
        "uniqueRaresIdentified": False,
        "uniqueRaresNote": "No DBR field distinguishes a Monster Infrequent from a 'unique rare'. "
                           "Both are plain Rare-classification equipment records; the game does the "
                           "split at runtime (loot filter tag tagLootFilter04 'Monster Infrequent'). "
                           "See catalogue-stats.md.",
    }

    os.makedirs(out_dir, exist_ok=True)
    cat_path = os.path.join(out_dir, "catalogue.json")
    with open(cat_path, "w", encoding="utf-8") as fh:
        fh.write("{\n")
        fh.write('"meta": ' + json.dumps(meta, indent=1) + ",\n")
        for label, lst in (("items", items), ("monsterInfrequents", mis), ("factionRares", faction_rares)):
            fh.write(f'"{label}": [\n')
            fh.write(",\n".join(json.dumps(e, sort_keys=True) for e in lst))
            fh.write("\n],\n")
        fh.write('"uniqueRares": null,\n')
        fh.write('"itemSets": ' + json.dumps(sets, indent=1) + "\n}\n")

    write_stats(os.path.join(out_dir, "catalogue-stats.md"), db, tags, items, mis,
                faction_rares, sets, rejected, meta)
    return cat_path, len(items), len(mis), len(faction_rares)


def write_stats(path, db, tags, items, mis, faction_rares, sets, rejected, meta):
    eq = [e for e in items if e["isEquipment"]]
    per = collections.Counter((e["slot"], e["itemClassification"]) for e in items)
    lines = []
    A = lines.append
    A("# Catalogue statistics (Grim Dawn 1.3.0.8)\n")
    A("Generated by `tools/build_catalogue.py` from the game's own database (set GD_DIR, or let it find Steam).")
    A(f"Merged database: {meta['mergedRecords']} records over "
      + ", ".join(f"{a['tag']} ({a['records']})" for a in meta["archives"])
      + f"; {meta['textTags']} English text tags.\n")
    A("## The collection rule\n")
    A("Two rules decide what the mod's tab has a box for:\n")
    A("- **equipment** (`ArmorProtective_*`, `ArmorJewelry_*`, `Weapon*`): `itemClassification`")
    A("  `Epic` or `Legendary`, and nothing else.")
    A("- **relics** (`Class = ItemArtifact`, i.e. `records/items/gearrelic/*`): **every**")
    A("  classification. The b-series relics are `Rare`")
    A("  records but they are unique-tier items to a player, and they were the only relic tier")
    A("  the tab had no box for.\n")
    A("| relic classification | records | file prefix |")
    A("|---|---:|---|")
    relic_prefix = {"Rare": "b", "Epic": "c", "Legendary": "d"}
    n_relic_all = 0
    for cla in ("Rare", "Epic", "Legendary"):
        n_relic_all += per[("relic", cla)]
        A("| %s | %d | `%s*_relic*.dbr` |" % (cla, per[("relic", cla)], relic_prefix[cla]))
    A(f"| **total relics** | **{n_relic_all}** | `records/items/gearrelic/` |")
    A("")
    A("`build_catalogue.py` ASSERTS those three counts (`RELIC_EXPECT`), that every accepted")
    A("relic sits under `records/items/gearrelic/`, that every relic bitmap resolves in")
    A("`resources/Items.arc`, and that equipment + relics comes to `COLLECTION_EXPECT_TOTAL`.")
    A("A game patch that changes any of them fails the build loudly; the fix is to re-read the")
    A("counts and update the constants, never to silence the assert.\n")
    A("- **curated inclusions** (`EXTRA_RECORDS`): a hand-written list of records")
    A("  the rank rule drops (neither Epic nor Legendary) but a player can really own. It bypasses the four rules below")
    A("  and is asserted the same way (the record must exist, its name tag must resolve, its")
    A("  bitmap must be in `resources/Items.arc`).\n")
    extras = [e for e in items if e.get("isExtra")]
    A("| curated inclusion | class | classification | group | why |")
    A("|---|---|---|---|---|")
    for e in sorted(extras, key=lambda x: x["record"]):
        A("| `%s` (%s) | `%s` | %s | %s | Shattered Realm named reward a player can own but the"
          " rank rule drops |"
          % (e["record"], e["name"], e["class"], e["itemClassification"], slot_of(e["class"])))
    A("")
    A("`EXTRA_EXCLUDE` names the developer test records in the same folder that must NEVER be")
    A("collectible, and the build asserts none of them reaches the collection:")
    A("")
    for r in sorted(EXTRA_EXCLUDE):
        A("- `%s`" % r)
    A("")
    A("## Shipped-record filter\n")
    A("A raw scan of the merged database finds far more Epic/Legendary records than grimtools lists.")
    A("Four rules bring the two into agreement; each was verified by inspecting the records it drops.")
    A("They apply to relics too - they are what keeps the 12 `records/sandbox/jakub/**` test relics out.\n")
    A("| # | Rule | What it removes |")
    A("|---|------|-----------------|")
    A("| 1 | path in no junk folder (`records/sandbox/`, `records/creatures/`, `records/mod_testgrounds/`) | dev/test items under `records/sandbox/**` (44 Epic+Leg equipment records, e.g. `records/sandbox/jim/legendary sets/...`), the gear NPCs wear (`records/creatures/npcs/npcgear/*`). Anywhere else is in: the real items outside `records/items/` - the Lokarr set (`records/storyelements/signs/sign{h,t,s,f}.dbr`), Wilhelm's Wondrous Wargem, Gazer Man, the Totally Normal Shield (`records/endlessdungeon/scriptentities/portal_s01..03.dbr`), and the Rare quest rewards - are collected like every other record |")
    A("| 2 | no `/enemygear/` in the path | monster-worn gear (`gear_witchgod_boss01_head.dbr` etc.), never dropped as loot |")
    A("| 3 | the record's name tag resolves in Text_EN | records whose `itemNameTag` / `description` is a dangling tag |")
    A("| 4 | `FileDescription` contains no \"BLANK\" | the designer template records, exactly 4 per equipment slot per classification: `c000/c100/c200/c300_axe2h.dbr` all say `BASE BLANK EPIC 2H AXE`, share tag `tagWeaponMelee2hC000` and sit at level 1 |")
    A("")
    A("Name tag field: `itemNameTag` for equipment classes, `description` for every other item class")
    A("(`ItemArtifact`, `ItemRelic`, `ItemEnchantment`, `ItemArtifactFormula`, ...).")
    A("Bitmap field: `bitmap`, except `ItemArtifact` -> `artifactBitmap` and `ItemRelic` -> `relicBitmap`.\n")
    A("## Epic / Legendary per slot vs grimtools 1.3.0.8\n")
    A("| slot | Epic (this) | Epic (GT) | d | Legendary (this) | Legendary (GT) | d |")
    A("|------|------:|------:|--:|------:|------:|--:|")
    te = tl = ge = gl = 0
    for slot in sorted(GRIMTOOLS):
        e, l = per[(slot, "Epic")], per[(slot, "Legendary")]
        gte, gtl = GRIMTOOLS[slot]
        te += e; tl += l; ge += gte; gl += gtl
        A(f"| {slot} | {e} | {gte} | {e-gte:+d} | {l} | {gtl} | {l-gtl:+d} |")
    A(f"| **total (incl. relics)** | **{te}** | **{ge}** | **{te-ge:+d}** | **{tl}** | **{gl}** | **{tl-gl:+d}** |")
    A("")
    A(f"This table is the Epic/Legendary comparison against grimtools and is deliberately left as")
    A(f"it was: the {per[('relic', 'Rare')]} Rare relics in the collection have no grimtools")
    A("column to be compared against and are counted in the relic table above and in the headline")
    A("totals below.\n")
    eq_e = sum(per[(s, "Epic")] for s in EQUIP_SLOT.values())
    eq_l = sum(per[(s, "Legendary")] for s in EQUIP_SLOT.values())
    A("### Headline totals")
    A("")
    A("| quantity | this catalogue | grimtools | note |")
    A("|---|---:|---:|---|")
    A(f"| equipment Epic | {eq_e} | {GRIMTOOLS_TOTALS['equipment_epic']} | {eq_e-GRIMTOOLS_TOTALS['equipment_epic']:+d} |")
    A(f"| equipment Legendary | {eq_l} | {GRIMTOOLS_TOTALS['equipment_legendary']} | {eq_l-GRIMTOOLS_TOTALS['equipment_legendary']:+d} |")
    A(f"| **equipment Epic+Legendary** | **{eq_e+eq_l}** | **{GRIMTOOLS_TOTALS['equipment_total']}** | **{eq_e+eq_l-GRIMTOOLS_TOTALS['equipment_total']:+d}** |")
    rel = per[("relic", "Epic")] + per[("relic", "Legendary")]
    rel_rare = per[("relic", "Rare")]
    A(f"| relics (`ItemArtifact`) Epic+Legendary | {rel} | {GRIMTOOLS_TOTALS['relics_epic_legendary']} | {rel-GRIMTOOLS_TOTALS['relics_epic_legendary']:+d} |")
    A(f"| relics (`ItemArtifact`) Rare (b-series) | {rel_rare} | - | grimtools lists no Rare relics; measured from the game DB |")
    A(f"| **relics, all classifications** | **{rel+rel_rare}** | - | `RELIC_EXPECT_TOTAL` = {RELIC_EXPECT_TOTAL} |")
    n_extra = len(extras)
    A(f"| curated inclusions (`EXTRA_RECORDS`) | {n_extra} | - | `EXTRA_EXPECT_TOTAL` = {EXTRA_EXPECT_TOTAL} |")
    A(f"| **collectible (equipment Epic+Leg + ALL relics + curated)** | **{eq_e+eq_l+rel+rel_rare+n_extra}** | - | `COLLECTION_EXPECT_TOTAL` = {COLLECTION_EXPECT_TOTAL} |")
    A(f"| distinct item sets referenced by Epic/Legendary pieces | {len(sets)} | {GRIMTOOLS_TOTALS['item_sets']} | |")
    A(f"| set pieces (Epic/Legendary records with `itemSetName`) | {sum(1 for e in items if e['isSetPiece'])} | 749 (unverified) | |")
    A("")
    A("### Remaining per-slot differences")
    A("")
    A("Over `records/items/` the two headline sums agree (Epic +1, Legendary -1, sum exactly 3,196;")
    A("the +6 Legendary above are the six records outside that folder), but a few slots still")
    A("disagree by a handful of records. They cancel out, which points at grimtools filing some items")
    A("under a different slot than the record's `Class` (its class ids c10..c49 are its own taxonomy),")
    A("not at extra or missing records here. The biggest single gap is daggers (-6 Legendary) against")
    A("scepters/off-hands/amulets being over; every one of the extra records here has a real name, a")
    A("real bitmap present in Items.arc and a level requirement. Not resolved; grimtools has no public")
    A("API to diff item-by-item against, so per-slot equality could not be checked directly.\n")
    A("## Other Epic/Legendary item classes (not equipment, not relics)\n")
    other = collections.Counter((e["class"], e["itemClassification"]) for e in items if not e["isEquipment"] and e["class"] != "ItemArtifact")
    A("| class | slot | Epic | Legendary |")
    A("|---|---|---:|---:|")
    for cls in sorted({c for c, _ in other}):
        A(f"| {cls} | {slot_of(cls)} | {other[(cls,'Epic')]} | {other[(cls,'Legendary')]} |")
    A("")
    A(f"Flag counts over all {len(items)} collectible entries "
      f"(Epic/Legendary equipment + relics of every classification + {len(extras)} curated): "
      f"isBlueprint {sum(1 for e in items if e['isBlueprint'])}, "
      f"isRelic {sum(1 for e in items if e['isRelic'])}, "
      f"isRelicOrComponent {sum(1 for e in items if e['isRelicOrComponent'])}, "
      f"isAugment {sum(1 for e in items if e['isAugment'])}, "
      f"isSetPiece {sum(1 for e in items if e['isSetPiece'])}.\n")
    A("Note: `ItemRelic` is the class of **components** (Mark of Divinity, Seal of Blades, ...); the")
    A("things the game calls relics are `ItemArtifact`. No component is Epic or Legendary, so")
    A("`isRelicOrComponent` is 0 in the Epic/Legendary list; the flag is kept for callers that\n"
      "care about components.\n")
    A("## Monster Infrequents and unique rares - NOT reliably separable\n")
    A("No DBR field marks a Monster Infrequent. What exists in the database is:")
    A("")
    A("- Ordinary Rare loot is generated at runtime: a Common base record plus a `LootRandomizer`")
    A("  prefix/suffix. Such items have no record of their own.")
    A("- Every equipment record whose `itemClassification` is `Rare` is therefore a *fixed* rare -")
    A("  a Monster Infrequent, a quest/boss 'unique rare', or a faction-store item.")
    A("- The only clean split inside that group is the path (and the matching filename prefix):")
    A(f"  `records/items/faction/**` = `f*` files, {len(faction_rares)} records, all with `itemCost`")
    A(f"  and `soulbound`; everything else = `b*` files, {len(mis)} records.")
    A("- The game does have an MI concept - the loot filter has a `Monster Infrequent` checkbox")
    A("  (`tagLootFilter04`) described as \"items that drop from specific enemies\" - but that")
    A("  classification is made in engine code from the monster loot tables, not from a record field.")
    A("")
    A(f"| bucket | records | distinct names |")
    A("|---|---:|---:|")
    A(f"| Rare equipment outside `records/items/faction/` (MI + unique rares) | {len(mis)} | {len({e['name'] for e in mis})} |")
    A(f"| `records/items/faction/**` Rare equipment (faction store) | {len(faction_rares)} | {len({e['name'] for e in faction_rares})} |")
    A(f"| grimtools 'Monster Infrequents' | {GRIMTOOLS_TOTALS['monster_infrequent_records']} | 562 (unverified) |")
    A(f"| grimtools 'Unique rare items' | {GRIMTOOLS_TOTALS['unique_rare_items']} | |")
    A("")
    A(f"So the non-faction bucket ({len(mis)}) is {GRIMTOOLS_TOTALS['monster_infrequent_records']+GRIMTOOLS_TOTALS['unique_rare_items']-len(mis)} short of grimtools' MI + unique-rare total")
    A(f"({GRIMTOOLS_TOTALS['monster_infrequent_records']} + {GRIMTOOLS_TOTALS['unique_rare_items']} = {GRIMTOOLS_TOTALS['monster_infrequent_records']+GRIMTOOLS_TOTALS['unique_rare_items']}), and adding the faction bucket overshoots it")
    A(f"({len(mis)+len(faction_rares)}). Two heuristics were tried and rejected:")
    A("names with exactly one record (72 hits, and their slot spread - 9 belts, 8 off-hands, 0 rings -")
    A("does not match the known unique-rare spread of 10 rings / 4 amulets / 1 medal), and")
    A("`itemStyleTag` (it carries cosmetic style words like Mythical/Empowered/Elite, not a rarity role).")
    A("**`uniqueRares` in catalogue.json is therefore `null`, not a guess.** `monsterInfrequents`")
    A("holds the whole non-faction Rare-record bucket and is a superset of the real MI list.")
    A("Resolving this properly means walking the monster loot tables, which is out of scope for this census.\n")
    A("## Icons\n")
    miss = [e for e in items if not e["bitmapFound"]]
    nobmp = [e for e in items if not e["bitmap"]]
    A("Rule (verified): a record's bitmap field is a *resource* path whose")
    A("first segment names the archive and is **not** part of the archive entry name -")
    A("`items/gearhead/bitmaps/d026_head.tex` is entry `gearhead/bitmaps/d026_head.tex` in `resources/Items.arc`,")
    A("`ui/caravan/caravan_tabup.tex` is entry `caravan/caravan_tabup.tex` in `resources/UI.arc`.")
    A("The four copies of each archive (base, gdx1, gdx2, gdx3) stack in the same load order as the .arz files.\n")
    have = len(items) - len(nobmp)
    missing = len([e for e in miss if e["bitmap"]])
    A(f"- Epic/Legendary entries with a bitmap: {have} / {len(items)}")
    A(f"- of those, the .tex resource was found in Items.arc / UI.arc / Level Art.arc: {have - missing} (missing: {missing})")
    A(f"- entries with no bitmap field at all: {len(nobmp)} "
      f"({', '.join(sorted({e['class'] for e in nobmp})) if nobmp else '-'})")
    if [e for e in miss if e["bitmap"]][:10]:
        A("")
        A("First bitmaps not found:")
        for e in [x for x in miss if x["bitmap"]][:10]:
            A(f"  - `{e['record']}` -> `{e['bitmap']}`")
    A("")
    A("## Records rejected by the shipped filter\n")
    rej = collections.Counter("/".join(k.split("/")[:3]) for k, _, _ in rejected)
    A(f"{len(rejected)} candidate records (Epic/Legendary equipment, plus relics of any"
      f" classification) were dropped. By path:\n")
    A("| path prefix | records |")
    A("|---|---:|")
    for p, n in rej.most_common(20):
        A(f"| `{p}` | {n} |")
    A("")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))


def write_view(out_dir, items, view, tags):
    """A view's catalogue.json: the items only (no Rare lists, no stats - those are vanilla's)."""
    items.sort(key=lambda e: (e["slot"], e["itemClassification"], e["record"]))
    meta = {"generated": "tools/build_catalogue.py", "view": view.view if view else "vanilla",
            "layers": [a.path for a, _ in view.layers] if view else [],
            "skippedArchives": view.skipped if view else [], "textTags": len(tags)}
    os.makedirs(out_dir, exist_ok=True)
    cat_path = os.path.join(out_dir, "catalogue.json")
    with open(cat_path, "w", encoding="utf-8") as fh:
        fh.write("{\n")
        fh.write('"meta": ' + json.dumps(meta, indent=1) + ",\n")
        fh.write('"items": [\n')
        fh.write(",\n".join(json.dumps(e, sort_keys=True) for e in items))
        fh.write("\n],\n")
        fh.write('"monsterInfrequents": [],\n"factionRares": [],\n"uniqueRares": null,\n')
        fh.write('"itemSets": ' + json.dumps(sorted({e["itemSetName"] for e in items
                                                       if e["itemSetName"]}), indent=1) + "\n}\n")
    return cat_path, len(items), 0, 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", default=os.environ.get("GD_DIR", DEFAULT_GAME))
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--basemods", action="store_true",
                    help="the view /basemods builds: mods\\database.arz on top of the base game")
    ap.add_argument("--custom", help="a custom game's folder (a name under <game>\\mods, or a "
                                     "full path) whose database and resources sit on top")
    ap.add_argument("--lang", default="EN")
    ap.add_argument("--collect-mi", action="store_true",
                    help="uniquetab.ini's collect_mi=1: admit the monster infrequents (every Rare "
                         "equipment record, faction-store and crafted rares included)")
    a = ap.parse_args()
    p, n_items, n_mi, n_fac = build(a.game, a.out, a.basemods, a.custom, a.lang, a.collect_mi)
    size = os.path.getsize(p)
    print(f"{p}  {size/1048576:.2f} MB")
    print(f"  items (collectible)        {n_items}   (Epic/Legendary equipment + ALL relics"
          f" + {EXTRA_EXPECT_TOTAL} curated extra)")
    print(f"  monsterInfrequents         {n_mi}")
    print(f"  factionRares               {n_fac}")


if __name__ == "__main__":
    main()
