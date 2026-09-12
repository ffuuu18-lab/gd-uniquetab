"""Reference implementation; the DLL generates these files itself now (src/gen/catalogue_gen).

Build data/oracle/catalogue.json + catalogue-stats.md from the four .arz databases.

    python build_catalogue.py [--game <dir>] [--out <dir>]

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

Shipped-record filter (reproduces the grimtools 1.3.0.8 totals exactly:
equipment Epic 1,650 vs 1,649 and Legendary 1,546 vs 1,547, sum 3,196 = 3,196; relics Epic+
Legendary 70 = 70 - grimtools lists no Rare relics, the 21 are measured from the game DB):
  1. record path starts with `records/items/`      -> drops records/sandbox/** (dev test items),
                                                      records/storyelements/**, records/endlessdungeon/**
  2. path is not under `records/items/enemygear/`  -> monster-worn gear, never dropped
  3. the item's name tag resolves in Text_EN       -> drops leftovers with dangling tags
  4. FileDescription does not contain "BLANK"      -> drops the `?000/?100/?200/?300` template records
     ("BASE BLANK EPIC 2H AXE" etc.), exactly 4 per equipment slot per classification

EXTRA_RECORDS is a small CURATED INCLUSION LIST that bypasses the four rules above
for named items a player really owns but the rules drop - today exactly one, Leovinus' Ring.
EXTRA_EXCLUDE names the developer test records that live beside it and must never be collectible.
Both lists are asserted, so a game patch that moves either is loud instead of silent.
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
# Records the four shipped-filter rules DROP but that a player can really own, so
# the tab must have a box for them. Each entry bypasses `is_shipped()` entirely and is asserted
# below: it must exist in the merged database, its name tag must resolve in Text_EN, and its
# bitmap must be in `resources/Items.arc`. One line per record, with the reason it is here.
# THIS IS A CURATED LIST, NOT A RULE - do not turn it into a path prefix. The folder it reaches
# into also holds developer test items (see EXTRA_EXCLUDE), which is exactly why the shipped
# filter drops the whole folder in the first place.
EXTRA_RECORDS = {
    # Leovinus' Ring - the fixed named ring the Shattered Realm hands out.
    #   Class ArmorJewelry_Ring, itemClassification Common, itemNameTag
    #   tagGDX2EndlessDungeon_S203, bitmap items/gearaccessories/rings/a001_ring.tex (32x32,
    #   gdx2), levelRequirement 10.
    # Without this entry the ring cannot be deposited at all: it is neither Epic nor Legendary
    # AND it does not live under records/items/, so it would never reach the catalogue, the gate
    # byte would never be set on it, the drag would be refused and the shift-click would fall
    # through to the normal transfer stash.
    "records/endlessdungeon/items/a001_ring.dbr",
}

# The two DEVELOPER TEST records that share that folder. Both are `Legendary`
# equipment and would sail through rules 2-4 if the path rule were ever loosened into a prefix,
# so they are named and excluded explicitly and the build ASSERTS neither ever reaches `items`.
EXTRA_EXCLUDE = {
    # "Mildly Amusing Box" (tagGDX2ItemTest2) - ArmorProtective_Head, Legendary, bitmap
    # system/textures/z000_test.tex, which is not even in resources/Items.arc. GDX2 test record.
    "records/endlessdungeon/items/z001_test.dbr",
    # "Miss Gazer Man" (tagGDX2ItemTest) - ArmorProtective_Chest, Legendary, no
    # levelRequirement, itemLevel 1. The other GDX2 test record.
    "records/endlessdungeon/items/q001_torso.dbr",
}
EXTRA_EXPECT_TOTAL = 1                                        # == len(EXTRA_RECORDS), asserted

EQUIPMENT_EPICLEG_EXPECT = 3196                               # Epic/Legendary equipment records
COLLECTION_EXPECT_TOTAL = 3288    # 3,196 Epic/Legendary equipment + 91 relics + 1 curated extra


BLUEPRINT_CLASSES = ("ItemArtifactFormula", "ItemAscensionFormula", "AscendantAltarFormula",
                     "ItemRandomSetFormula", "ItemRerollFormula", "ItemSetFormula")


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
    """ItemArtifact -> artifactBitmap, ItemRelic -> relicBitmap, everything else -> bitmap."""
    for f in ("bitmap", "artifactBitmap", "relicBitmap"):
        v = arz.sfield(rec, f)
        if v:
            return v
    return ""


def is_shipped(key: str, rec, cls: str, tags, db=None) -> bool:
    if not key.startswith("records/items/"):
        return False
    if key.startswith("records/items/enemygear/"):
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


def build(game_dir: str, out_dir: str):
    db = arz.ArzDatabase.load_game(game_dir)
    tags = arc.load_text_tags(game_dir)
    items_arc = arc.ArcSet.load(game_dir, os.path.join("resources", "Items.arc"))
    ui_arc = arc.ArcSet.load(game_dir, os.path.join("resources", "UI.arc"))

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

    def entry(key, src, rec, cls):
        tag = name_tag_of(rec, cls)
        bmp = bitmap_of(rec, cls)
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
            "record": db.real_name(key) or key,
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
            # On the curated inclusion list, so build_uniq_db.py can page it
            # without re-deriving the rule (it is neither Epic/Legendary nor a relic).
            "isExtra": key.lower() in EXTRA_RECORDS,
        }

    items, mis, faction_rares, rejected = [], [], [], []
    relic_seen = collections.Counter()
    extra_seen = set()
    for key, src, rec in db.items():
        cls = arz.sfield(rec, "Class")
        cla = arz.sfield(rec, "itemClassification")
        low = key.lower()
        if low in EXTRA_EXCLUDE:               # named dev test records, never in
            continue
        is_extra = low in EXTRA_RECORDS        # curated inclusion, bypasses is_shipped
        if cls not in EQUIP_SLOT and cls not in OTHER_SLOT:
            continue
        if cls == "LootRandomizer":            # affixes, not items
            continue
        # A relic is in the collection at EVERY classification, so it must not be
        # pre-filtered by the rarity word the way equipment is.
        is_relic = (cls == RELIC_CLASS)
        if not is_relic and not is_extra and cla not in ("Epic", "Legendary", "Rare"):
            continue
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
        elif cla in ("Epic", "Legendary"):
            if shipped:
                items.append(entry(key, src, rec, cls))
            else:
                rejected.append((key, cls, cla))
        elif cls in EQUIP_SLOT and shipped:    # Rare *record* = MI or unique rare or faction item
            e = entry(key, src, rec, cls)
            (faction_rares if key.startswith("records/items/faction/") else mis).append(e)

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
        "shippedFilter": ["path startswith records/items/", "not records/items/enemygear/",
                          "name tag resolves in Text_EN", "FileDescription has no BLANK"],
        "collectionRule": "equipment: itemClassification Epic or Legendary; "
                          "relics (Class ItemArtifact): EVERY classification; "
                          "plus the EXTRA_RECORDS curated inclusion list",
        "relicsByClassification": dict(sorted(relic_seen.items())),
        "extraRecords": sorted(EXTRA_RECORDS),
        "extraExcluded": sorted(EXTRA_EXCLUDE),
        "bitmapRule": "resource path 'items/<rest>' -> Items.arc entry '<rest>'; "
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
    A("  the shipped filter drops but a player can really own. It bypasses the four rules below")
    A("  and is asserted the same way (the record must exist, its name tag must resolve, its")
    A("  bitmap must be in `resources/Items.arc`).\n")
    extras = [e for e in items if e.get("isExtra")]
    A("| curated inclusion | class | classification | group | why |")
    A("|---|---|---|---|---|")
    for e in sorted(extras, key=lambda x: x["record"]):
        A("| `%s` (%s) | `%s` | %s | %s | Shattered Realm named reward a player can own but the"
          " shipped filter drops |"
          % (e["record"], e["name"], e["class"], e["itemClassification"], slot_of(e["class"])))
    A("")
    A("`EXTRA_EXCLUDE` names the developer test records in the same folder that must NEVER be")
    A("collectible, and the build asserts none of them reaches the collection:")
    A("")
    for r in sorted(EXTRA_EXCLUDE):
        A("- `%s`" % r)
    A("")
    A("## Shipped-record filter (unchanged)\n")
    A("A raw scan of the merged database finds far more Epic/Legendary records than grimtools lists.")
    A("Four rules bring the two into agreement; each was verified by inspecting the records it drops.")
    A("They apply to relics too - they are what keeps the 12 `records/sandbox/jakub/**` test relics out.\n")
    A("| # | Rule | What it removes |")
    A("|---|------|-----------------|")
    A("| 1 | path starts with `records/items/` | dev/test items under `records/sandbox/**` (44 Epic+Leg equipment records, e.g. `records/sandbox/jim/legendary sets/...`), `records/storyelements/signs/signh.dbr`, `records/endlessdungeon/items/z001_test.dbr` |")
    A("| 2 | path not under `records/items/enemygear/` | monster-worn gear (`gear_witchgod_boss01_head.dbr` etc.), never dropped as loot |")
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
    A("The two headline sums agree (Epic +1, Legendary -1, sum exactly 3,196), but a few slots still")
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
    A(f"| `records/items/**` Rare equipment, non-faction (MI + unique rares) | {len(mis)} | {len({e['name'] for e in mis})} |")
    A(f"| `records/items/faction/**` Rare equipment (faction store) | {len(faction_rares)} | {len({e['name'] for e in faction_rares})} |")
    A(f"| grimtools 'Monster Infrequents' | {GRIMTOOLS_TOTALS['monster_infrequent_records']} | 562 (unverified) |")
    A(f"| grimtools 'Unique rare items' | {GRIMTOOLS_TOTALS['unique_rare_items']} | |")
    A("")
    A(f"So the non-faction bucket ({len(mis)}) is {GRIMTOOLS_TOTALS['monster_infrequent_records']+GRIMTOOLS_TOTALS['unique_rare_items']-len(mis)} short of grimtools' MI + unique-rare total")
    A(f"({GRIMTOOLS_TOTALS['monster_infrequent_records']} + {GRIMTOOLS_TOTALS['unique_rare_items']} = {GRIMTOOLS_TOTALS['monster_infrequent_records']+GRIMTOOLS_TOTALS['unique_rare_items']}), and adding the faction bucket overshoots it")
    A(f"({len(mis)+len(faction_rares)}). Two heuristics were tried and rejected:")
    A("names with exactly one record (72 hits, and their slot spread - 9 belts, 8 off-hands, 0 rings -")
    A("does not match the researcher's unique-rare spread of 10 rings / 4 amulets / 1 medal), and")
    A("`itemStyleTag` (it carries cosmetic style words like Mythical/Empowered/Elite, not a rarity role).")
    A("**`uniqueRares` in catalogue.json is therefore `null`, not a guess.** `monsterInfrequents`")
    A("holds the whole non-faction Rare-record bucket and is a superset of the real MI list.")
    A("Resolving this properly means walking the monster loot tables, which is out of scope for M3.\n")
    A("## Icons\n")
    miss = [e for e in items if not e["bitmapFound"]]
    nobmp = [e for e in items if not e["bitmap"]]
    A("Rule (verified, now also in ENGINE-FACTS): a record's bitmap field is a *resource* path whose")
    A("first segment names the archive and is **not** part of the archive entry name -")
    A("`items/gearhead/bitmaps/d026_head.tex` is entry `gearhead/bitmaps/d026_head.tex` in `resources/Items.arc`,")
    A("`ui/caravan/caravan_tabup.tex` is entry `caravan/caravan_tabup.tex` in `resources/UI.arc`.")
    A("The four copies of each archive (base, gdx1, gdx2, gdx3) stack in the same load order as the .arz files.\n")
    have = len(items) - len(nobmp)
    missing = len([e for e in miss if e["bitmap"]])
    A(f"- Epic/Legendary entries with a bitmap: {have} / {len(items)}")
    A(f"- of those, the .tex resource was found in Items.arc / UI.arc: {have - missing} (missing: {missing})")
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", default=os.environ.get("GD_DIR", DEFAULT_GAME))
    ap.add_argument("--out", default=DEFAULT_OUT)
    a = ap.parse_args()
    p, n_items, n_mi, n_fac = build(a.game, a.out)
    size = os.path.getsize(p)
    print(f"{p}  {size/1048576:.2f} MB")
    print(f"  items (collectible)        {n_items}   (Epic/Legendary equipment + ALL relics"
          f" + {EXTRA_EXPECT_TOTAL} curated extra)")
    print(f"  monsterInfrequents         {n_mi}")
    print(f"  factionRares               {n_fac}")


if __name__ == "__main__":
    main()
