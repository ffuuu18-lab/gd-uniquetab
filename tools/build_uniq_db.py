"""Reference implementation; the DLL generates the three txt files and uniq-pages.arz itself
(src/gen/pages_gen, src/gen/arz_writer). What this tool writes is the oracle the catalogue
harness compares the DLL's output against; none of it ships.

Build the mod's page database and its lists into data/oracle/.

THE MECHANISM.  The exe builds a reagent sub-window by calling
`ObjectManager::LoadTableFile` / `GetLoadTable` - two Engine.dll exports - with the record
path, so the DLL can simply hand back a DIFFERENT record.  Overriding
`records/ui/caravan/caravan_materialwindow.dbr` would cost the player their real Crafting
Materials page; the vanilla record is therefore never touched at all, and every collection
page is a NEW record of its own:

    records/ui/caravan/uniq_pNN.dbr                     one reagent window per page
    records/ui/caravan/reagents/uniq/pNN/box_MM.dbr     40 boxes per page

What is emitted
  data/oracle/uniq-pages.arz   ALL pages and their boxes in one archive.  One archive, not
      one per page: with the path substitution above a page is selected by NAME, so separate
      archives would buy nothing and cost ~80 memory-mapped files and 80 LoadDatabase calls.
  data/oracle/uniq-pages.json  the manifest: per page the record path, the slot group, the
      number of boxes and every item on it.
  data/oracle/uniq-records.txt every item record that appears on any page, one per line - the
      list the DLL's gate uses to recognise our items.

  data/oracle/uniq-pages.txt   one line per page: index, record, label k/m, box count.
  data/oracle/uniq-groups.txt  the group model the DLL scrolls: F/V/G/E lines, an E line per
      entry carrying BOTH the box record and the item record. A G line ends with the group's
      id and its 3-character tag, read out of the built-in groups template
      (src/gen/ut_groups_default.h); readers also take the older line without them.

Selection - THE COLLECTION RULE:
  * every Epic/Legendary EQUIPMENT record in data/oracle/catalogue.json (unchanged), and
  * every RELIC (`isRelic`, Class ItemArtifact) at ANY classification - the 21 Rare b-series
    relics joined the 21 Epic and 49 Legendary ones, so the Relics group holds 91 and the
    collection 3,287 records (was 70 and 3,266), and
  * every record on build_catalogue.py's curated inclusion list (`isExtra`,
    EXTRA_RECORDS) whatever its classification - today exactly Leovinus' Ring, a `Common`
    ArmorJewelry_Ring, so the Rings group holds 264 and the collection 3,288, and
  * every BLUEPRINT (Class ItemArtifactFormula) at ANY classification, on its own 64 x 32
    scroll icon (artifactFormulaBitmapName), in the group Blueprints after the Relics - Crate's
    test blueprints (blueprint_*test*) excluded by the template.
Grouped by the built-in groups template (src/gen/ut_groups_default.h: the 24 slot groups and
the blueprints of SLOT_ORDER, in that order, and the two per-class groups whose words come from the view's text;
--groups names a user's file, merged IN FRONT of it as the DLL does); each
group gets its own cell size (the largest icon in the group) and therefore its own column / row
count, so a page holds as many boxes as the band really fits.  Inside a group the order is Legendary, then Epic, then Rare, then Common, each by
name - so adding a rarity tier APPENDS boxes and never renumbers the ones that already existed.

    python build_uniq_db.py [--limit-pages N] [--groups <ini>]
    python build_uniq_db.py --annotate-groups    (uniq-groups.txt E lines only, no game access)
    python build_uniq_db.py --stock-templates <dir>   (every template an earlier build wrote:
                                                       data/oracle/templates/<n>.ini, CRLF)

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
import ui_geometry                                                  # noqa: E402
import build_catalogue                                             # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ORACLE = os.path.join(ROOT, "data", "oracle")     # fixtures: the archive, the txt lists, the manifest, sample dbr
ARZ_DIR = ORACLE
SRC_DIR = os.path.join(ORACLE, "src")
GAME = gdpath.find_game_dir() or ""
ARGS = None          # the command line, for the buttons file's view
VIEW = None          # ui_geometry.View of --basemods / --custom; None = the base game

# Kept in step with build_catalogue.py's constants of the same name, and asserted
# against catalogue.json in pick_items() so a stale catalogue.json cannot be paged silently.
RELIC_EXPECT_TOTAL = 91           # 21 Rare + 21 Epic + 49 Legendary, records/items/gearrelic/
EXTRA_EXPECT_TOTAL = 1            # build_catalogue.py's EXTRA_RECORDS
BLUEPRINT_EXPECT_TOTAL = 914      # every shipped ItemArtifactFormula (925) but Crate's 11 test ones
COLLECTION_EXPECT_TOTAL = 4202    # 3,196 Epic/Leg equipment + 91 relics + 1 curated extra + 914 blueprints

MATERIAL_WINDOW = "records/ui/caravan/caravan_materialwindow.dbr"
PLAIN_PLATE = "records/ui/caravan/caravan_transfercoverimage.dbr"
PAGE_FMT = "records/ui/caravan/uniq_p%02d.dbr"
BOX_FMT = "records/ui/caravan/reagents/uniq/p%02d/box_%02d.dbr"
BOX_TPL = "database/templates/ingameui/uireagentitem.tpl"

# THE LIVE RE-POINT.  The HUD always builds ONE record - the frame - and the DLL then
# re-points its boxes in place with the engine's own UIReagentItem::Load / SetItem /
# SetPosition.  The frame lists the 24 REAL vanilla box records
# first, so the page a character loads with is bit-identical to the vanilla Crafting Materials
# page, then FRAME_BOXES-24 filler boxes parked far outside the window.
FRAME_REC = "records/ui/caravan/uniq_frame.dbr"
HIDE_FMT = "records/ui/caravan/reagents/uniq/hide_%03d.dbr"
# THE FRAME'S BOX COUNT is the view's own (ut_layout.h utFrameBoxes): the most boxes any grid on
# the COVER can show at the view's cell sizes (the whole plate, uncapped), clamped to
# FRAME_FLOOR..FRAME_MAX. 160 is the vanilla page's own fit and every view's count through 1.2;
# 512 is about 1.4 x the largest window a shipping mod builds (Reign of Terror's 364 boxes).
# frame_boxes() sets FRAME_BOXES in main() before any grid is fitted under it.
FRAME_FLOOR = 160
FRAME_MAX = 512
FRAME_BOXES = FRAME_FLOOR
HIDE_XY = -4000      # record pixels; multiplied by the UI scale, so always off-screen

# THE GRID is not one size for every page.  A box takes the item bitmap's
# NATURAL size (there is no box-size field and no scaling), so the cell only has to be as big as
# the LARGEST icon in that slot group: rings / amulets / medals / relics are 32x32 and fit
# 10 x 16 = 160 to a page, while the 1,076 records with a taller icon (chests, off-hands,
# two-handers - 96 and 128 px) get pages of their own with a bigger cell.
#
# The rectangle is NOT a constant: ui_geometry.derive() reads it out of the merged database and
# out of the cover texture, so a stash-UI mod that moves or enlarges the caravan is followed.
# geometry() assigns these four in main() before anything reads them; the vanilla derivation is
# the value here so --annotate-groups needs no game access.
COL_X0, ROW_Y0 = ui_geometry.VANILLA[0], ui_geometry.VANILLA[1]
USABLE_W, USABLE_H = ui_geometry.VANILLA[2], ui_geometry.VANILLA[3]
ROW_GAP = 2          # the vanilla 64 px rows sit on a 66 px pitch

# The height the shipped layout was built with, before the cover's one-pixel
# offset joined the derivation. Only used by geometry()'s assert.
SHIPPED_USABLE_H = 551

# The order the pages are laid out in: armour, then jewellery, then off-hands, then weapons.
SLOT_ORDER = ["head", "shoulders", "chest", "hands", "waist", "legs", "feet",
              "amulet", "medal", "ring", "offhand", "shield",
              "axe1h", "dagger", "mace1h", "scepter", "sword1h", "ranged1h",
              "axe2h", "mace2h", "spear2h", "sword2h", "ranged2h", "relic", "blueprint"]

SLOT_LABEL = {
    "head": "Helms", "shoulders": "Shoulders", "chest": "Chest", "hands": "Gloves",
    "waist": "Belts", "legs": "Pants", "feet": "Boots", "amulet": "Amulets",
    "medal": "Medals", "ring": "Rings", "offhand": "Off-hands", "shield": "Shields",
    "axe1h": "1H Axes", "dagger": "Daggers", "mace1h": "1H Maces", "scepter": "Scepters",
    "sword1h": "1H Swords", "ranged1h": "Guns", "axe2h": "2H Axes", "mace2h": "2H Maces",
    "spear2h": "Spears", "sword2h": "2H Swords", "ranged2h": "2H Guns", "relic": "Relics",
    "blueprint": "Blueprints",
}


GROUPS_TEMPLATE_H = os.path.join(ROOT, "src", "gen", "ut_groups_default.h")


def template_text():
    """The built-in uniquetab-groups.ini. It lives in ONE place - the raw string of
    src/gen/ut_groups_default.h, which the DLL compiles - and is read from there."""
    src = open(GROUPS_TEMPLATE_H, encoding="utf-8").read()
    a = src.index('R"UTG(') + len('R"UTG(')
    return src[a:src.index(')UTG"', a)]


# ---- the groups as data: a port of src/gen/ut_groups.h (parser, matcher, auto groups)
BUILTIN = [(s, SLOT_LABEL[s], t) for s, t in zip(SLOT_ORDER, (
    "HEL", "SHO", "CHE", "GLO", "BEL", "PAN", "BOO", "AMU", "MED", "RNG", "OFF", "SHD",
    "AX1", "DAG", "MC1", "SCP", "SW1", "GUN", "AX2", "MC2", "SPR", "SW2", "GN2", "REL", "BLU"))]
FLAG_BITS = {"set": 1, "relic": 2, "equipment": 4, "faction": 8, "blueprint": 16,
             "augment": 32, "extra": 64}
AUTO_NAMES = {"itemequipment": ("Equipment", "EQP"), "itemusableskill": ("Usable Skills", "USK"),
              "itemrelic": ("Components", "CMP"), "itemenchantment": ("Augments", "AUG"),
              "itemartifactformula": ("Blueprints", "BLU")}
ID_CHARS = set("abcdefghijklmnopqrstuvwxyz0123456789_.-")
# The engine's type word per item class (Game.dll 0x576090 / 0x3145B0) - utg::classTypeTags.
CLASS_TYPE_TAGS = [
    ("weaponmelee_axe", "tagOneHandedAxe"), ("weaponmelee_sword", "tagOneHandedSword"),
    ("weaponmelee_mace", "tagOneHandedMace"), ("weaponmelee_dagger", "tagOneHandedDagger"),
    ("weaponmelee_scepter", "tagOneHandedScepter"), ("weaponhunting_spear", "tagSpear"),
    ("weaponhunting_ranged2h", "tagTwoHandedRifle"), ("weaponmagical_staff", "tagStaff"),
    ("weaponarmor_shield", "tagShield"), ("weaponarmor_offhand", "tagOffHand"),
    ("weaponhunting_ranged1h", "tagOneHandedPistol"), ("weaponmelee_axe2h", "tagTwoHandedAxe"),
    ("weaponmelee_sword2h", "tagTwoHandedSword"), ("weaponmelee_mace2h", "tagTwoHandedMace"),
    ("weaponmelee_spear2h", "tagTwoHandedSpear"),
    ("armorprotective_head", "tagHead"), ("armorjewelry_amulet", "tagAmulet"),
    ("armorprotective_chest", "tagChest"), ("armorprotective_legs", "tagLegs"),
    ("armorprotective_feet", "tagFeet"), ("armorjewelry_ring", "tagRing"),
    ("armorprotective_hands", "tagGloves"), ("armorprotective_waist", "tagBelt"),
    ("armorprotective_shoulders", "tagShoulders"), ("armorjewelry_medal", "tagMedal")]
CLASS_TYPE_TAG = dict(CLASS_TYPE_TAGS)
LABEL_MAX = 48
TAG_ALT = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"


def low_ascii(s):
    return "".join(chr(ord(c) + 32) if "A" <= c <= "Z" else c for c in s)


def tag_from(text):
    t = "".join(c.upper() for c in text if c.isascii() and c.isalnum())[:3]
    return (t + "XXX")[:3]


def cap(w):
    return w[:1].upper() + w[1:] if w[:1].isascii() and w[:1].islower() else w


def class_last_part(cls):
    return cls.rsplit("_", 1)[-1]


def class_family_word(cls):
    """utg::classFamilyWord: the part before the first '_' without Weapon/Armor/Item, capitalised."""
    u = cls.find("_")
    if u <= 0:
        return ""
    f = cls[:u]
    for p in ("weapon", "armor", "item"):
        if len(f) > len(p) and low_ascii(f).startswith(p):
            f = f[len(p):]
            break
    return cap(f)


def valid_label(s):
    b = s.encode("utf-8")
    return 0 < len(b) <= LABEL_MAX and all(c >= 0x20 and c != 0x7F for c in b)


def clean_word(text):
    """utg::cleanWord: colour codes and a leading gender marker removed, blanks trimmed."""
    b = text.encode("utf-8")
    o = bytearray()
    i = 0
    while i < len(b):
        if b[i:i + 1] == b"{" and i + 3 < len(b) and b[i + 1:i + 2] == b"^" and b[i + 3:i + 4] == b"}":
            i += 4
            continue
        if b[i:i + 1] == b"^" and i + 1 < len(b):
            i += 2
            continue
        o.append(0x20 if (b[i] < 0x20 or b[i] == 0x7F) else b[i])
        i += 1
    o = bytes(o).strip(b" ")
    if len(o) > 2 and o[:1] == b"[":
        close = o.find(b"]")
        if 2 <= close <= 4 and all(chr(c).isascii() and chr(c).isalpha() for c in o[1:close]):
            o = o[close + 1:].lstrip(b" ")
    o = o[:LABEL_MAX].rstrip(b" ")
    w = o.decode("utf-8", "replace")
    return w if valid_label(w) and w.encode("utf-8") == o else ""


def plural_word(w):
    """utg::pluralWord."""
    if not w or not (w[-1].isascii() and w[-1].isalpha()):
        return w
    z, y = w[-1].lower(), (w[-2].lower() if len(w) >= 2 else " ")
    if z == "f" and y == "f":
        p = w[:-2] + "ves"
    elif z in "sx" or (z == "h" and y in "cs"):
        p = w + "es"
    elif z == "y" and len(w) >= 2 and w[-2].isascii() and w[-2].isalpha() and y not in "aeiou":
        p = w[:-1] + "ies"
    else:
        p = w + "s"
    return w if len(p.encode("utf-8")) > LABEL_MAX else p


def tag_candidates(word):
    """utg::tagCandidates: three letters, the letter + two consonants, two letters + 0-9 A-Z."""
    t1 = tag_from(word)
    c = [t1]
    letters = [ch.upper() for ch in word if ch.isascii() and ch.isalpha()]
    if letters:
        t2 = letters[0] + "".join(ch for ch in letters[1:] if ch not in "AEIOU")[:2]
        if len(t2) == 3:
            c.append(t2)
    c += [t1[:2] + k for k in TAG_ALT]
    return c


def resolve_names(words, groups, used):
    """utg::resolveNames: the derived labels and tags of the groups that received a record.
    groups: [id, label, tag, labelDerived, tagDerived, cls] lists, changed in place."""
    special = lambda d: d[0] == "other"
    tags, labels = set(), set()
    for d, u in zip(groups, used):
        if not u or special(d):
            continue
        if not d[4]:
            tags.add(d[2])
        if not d[3]:
            labels.add(low_ascii(d[1]))
    free = lambda t: t not in tags and t not in ("MAT", "OWN")
    for d, u in zip(groups, used):
        if not u:
            continue
        if special(d):
            t = d[2]
            for k in TAG_ALT:
                if free(t):
                    break
                t = t[:2] + k
            d[2] = t
            tags.add(t)
            continue
        word = ""
        if d[3]:
            if d[5] and d[5] in CLASS_TYPE_TAG:
                word = clean_word(words.get(d[5], ""))
                if not word:
                    word = clean_word(cap(class_last_part(d[5])))
            if word:
                label = plural_word(word)
                if low_ascii(label) in labels:
                    fam = class_family_word(d[5])
                    if fam and len(fam) + 1 + len(label) <= LABEL_MAX:
                        label = fam + " " + label
                    stem = label
                    n = 2
                    while low_ascii(label) in labels and n < 100:
                        sfx = " %d" % n
                        label = stem[:LABEL_MAX - len(sfx)] + sfx
                        n += 1
                d[1] = label
            labels.add(low_ascii(d[1]))
        if d[4]:
            src = word or d[1]
            cand = [] if word else [d[2]]
            if not any(ch.isascii() and ch.isalnum() for ch in src):
                src = class_last_part(d[5]) if d[5] else d[0].rsplit(".", 1)[-1]
            cand += tag_candidates(src)
            for c in cand:
                if free(c):
                    d[2] = c
                    break
            tags.add(d[2])


def view_group_table(groups, group_of):
    """utg::viewGroupTable: [(id, label, tag)] of every group that received a record, in order -
    the default words the buttons file is generated with."""
    used = {g for g in group_of if g >= 0}
    return [tuple(d[:3]) for n, d in enumerate(groups) if n in used]


def _strip_comment(s):
    for i in range(1, len(s)):
        if s[i] in ";#" and s[i - 1] in " \t":
            return s[:i]
    return s


def parse_groups(text):
    """uniquetab-groups.ini -> {"groups": [[id, label, tag]], "rules": [...], "auto", "min"}.
    Raises ValueError("line N: ...") on the first error, like utg::parseGroups."""
    if text.startswith("﻿"):
        text = text[1:]
    gs = {"groups": [], "rules": [], "auto": True, "min": 5, "autoSet": False, "minSet": False,
          "words": {}}
    index, view, section, cur, block_keys = {}, "", None, None, set()

    def fail(n, what):
        raise ValueError("line %d: %s" % (n, what))

    def close():
        if cur is not None:
            if not cur["filter"]:
                fail(cur["line"], ("[exclude]" if cur["exclude"] else "the [group] block")
                     + " has no filter (class, path, rank, flag or source)")
            if not cur["exclude"] and cur["classes"] and not gs["groups"][cur["group"]][8]:
                gs["groups"][cur["group"]][8] = cur["classes"][0]
            gs["rules"].append(cur)

    for n, raw in enumerate(text.split("\n"), 1):
        line = raw.rstrip("\r").strip(" \t")
        if not line or line[0] in ";#":
            continue
        if line[0] == "[":
            line = _strip_comment(line).strip(" \t")
            if not line.endswith("]"):
                fail(n, "a section header must end with ']'")
            close()
            cur, block_keys = None, set()
            h = line[1:-1].strip(" \t")
            if h.startswith("view:"):
                k = h[5:].strip(" \t")
                if k != "*" and not (0 < len(k) <= 80 and set(k) <= ID_CHARS | {"+"}):
                    fail(n, 'bad view key "%s"' % k)
                view, section = ("" if k == "*" else k), None
            elif h == "exclude":
                section = "rule"
                cur = {"exclude": True, "group": -1, "view": view, "line": n, "filter": False,
                       "classes": [], "paths": [], "ranks": [], "flags": 0, "source": "any"}
            elif h == "auto":
                if view:
                    fail(n, "[auto] applies to every view; put it before any [view:...]")
                section = "auto"
            elif h.startswith("group "):
                gid = h[6:].strip(" \t")
                if not (0 < len(gid) <= 40 and set(gid) <= ID_CHARS):
                    fail(n, 'bad group id "%s" (a-z 0-9 _ . -, 1..40)' % gid)
                if gid in ("vanilla", "owned", "next", "other") or gid.startswith("auto."):
                    fail(n, 'the group id "%s" is reserved' % gid)
                if gid not in index:
                    index[gid] = len(gs["groups"])
                    # id, label, tag, line, labelSet, tagSet, labelDerived, tagDerived, cls
                    gs["groups"].append([gid, "", "", n, False, False, False, False, ""])
                section = "rule"
                cur = {"exclude": False, "group": index[gid], "view": view, "line": n,
                       "filter": False, "classes": [], "paths": [], "ranks": [], "flags": 0,
                       "source": "any"}
            else:
                fail(n, "unknown section [%s]" % h)
            continue
        if "=" not in line:
            fail(n, "expected key = value")
        key, val = line.split("=", 1)
        key, val = key.strip(" \t").lower(), _strip_comment(val).strip(" \t")
        if section is None:
            fail(n, '"%s" is outside a [group], [exclude] or [auto] block' % key)
        if section == "auto":
            if key == "min":
                if not val.isdigit() or not 1 <= int(val) <= 1000:
                    fail(n, "min must be 1..1000")
                gs["min"] = int(val)
                gs["minSet"] = True
            elif key == "enabled":
                v = val.lower()
                if v not in ("yes", "1", "true", "on", "no", "0", "false", "off"):
                    fail(n, "enabled must be yes or no")
                gs["auto"] = v in ("yes", "1", "true", "on")
                gs["autoSet"] = True
            else:
                fail(n, 'unknown key "%s" in [auto]' % key)
            continue
        if key in ("label", "tag"):
            if cur["exclude"]:
                fail(n, key + " belongs to a [group] block")
            if key in block_keys:
                fail(n, key + " given twice")
            block_keys.add(key)
            d = gs["groups"][cur["group"]]
            slot = 1 if key == "label" else 2
            if key == "label" and not (0 < len(val.encode("utf-8")) <= 48
                                       and all(ord(c) >= 0x20 and ord(c) != 0x7F for c in val)):
                fail(n, "a label is 1..48 bytes, no tab")
            if key == "tag" and not (len(val) == 3 and all(0x21 <= ord(c) <= 0x7E for c in val)):
                fail(n, "a tag is exactly 3 printable ASCII characters")
            if key == "tag" and val in ("MAT", "OWN"):
                fail(n, "the tag %s is the pad's own" % val)
            if d[slot] and d[slot] != val:
                fail(n, 'group %s already has the %s "%s"' % (d[0], key, d[slot]))
            d[slot] = val
            d[3 + slot] = True
            continue
        vals = [v.strip(" \t") for v in val.split(",") if v.strip(" \t")]
        if not vals:
            fail(n, key + " has no value")
        if key == "class":
            cur["classes"] += [v.lower() for v in vals]
        elif key == "path":
            cur["paths"] += [v.lower().replace("\\", "/") for v in vals]
        elif key == "rank":
            for v in vals:
                if v.lower() == "any":
                    cur["ranks"] = ["*"]
                    break
                cur["ranks"].append(v.lower())
        elif key == "flag":
            for v in vals:
                if v.lower() not in FLAG_BITS:
                    fail(n, 'unknown flag "%s"' % v)
                cur["flags"] |= FLAG_BITS[v.lower()]
        elif key == "source":
            if len(vals) != 1 or vals[0].lower() not in ("base", "mod", "any"):
                fail(n, "source must be base, mod or any")
            cur["source"] = vals[0].lower()
        else:
            fail(n, 'unknown key "%s"' % key)
        cur["filter"] = True
    close()
    builtin = {b[0]: b for b in BUILTIN}
    for d in gs["groups"]:
        b = builtin.get(d[0])
        if not d[1]:
            d[1] = b[1] if b else d[0].rsplit(".", 1)[-1][:1].upper() + d[0].rsplit(".", 1)[-1][1:]
            d[6] = not b
        if not d[2]:
            d[2] = b[2] if b else tag_from(d[1])
            d[7] = not b
        if d[2] in ("MAT", "OWN"):
            fail(d[3], "the tag %s is the pad's own" % d[2])
    return gs


def merge_groups(user, base):
    """utg::mergeGroupSets: the user's set IN FRONT of the built-in one - its new groups first,
    then the built-in groups (a built-in id the user names keeps its place and takes the user's
    label/tag/first class), every user rule, then every built-in rule, the user's [auto] keys
    over the built-in ones. -> (merged, (builtin, yours, extended))."""
    base_idx = {d[0]: n for n, d in enumerate(base["groups"])}
    user_idx = {d[0]: n for n, d in enumerate(user["groups"])}
    out = {"groups": [], "rules": [], "words": dict(base["words"], **user["words"])}
    umap, bmap = {}, {}
    yours = extended = 0
    for n, d in enumerate(user["groups"]):
        if d[0] in base_idx:
            continue
        umap[n] = len(out["groups"])
        out["groups"].append(list(d))
        yours += 1
    for n, d in enumerate(base["groups"]):
        d = list(d)
        bmap[n] = len(out["groups"])
        if d[0] in user_idx:
            u = user["groups"][user_idx[d[0]]]
            umap[user_idx[d[0]]] = bmap[n]
            extended += 1
            if u[4]:
                d[1], d[4], d[6] = u[1], True, False
            if u[5]:
                d[2], d[5], d[7] = u[2], True, False
            if u[8]:
                d[8] = u[8]
        out["groups"].append(d)
    for r in user["rules"]:
        out["rules"].append(dict(r, group=r["group"] if r["exclude"] else umap[r["group"]]))
    for r in base["rules"]:
        out["rules"].append(dict(r, group=r["group"] if r["exclude"] else bmap[r["group"]]))
    out["auto"] = user["auto"] if user["autoSet"] else base["auto"]
    out["min"] = user["min"] if user["minSet"] else base["min"]
    out["autoSet"] = user["autoSet"] or base["autoSet"]
    out["minSet"] = user["minSet"] or base["minSet"]
    return out, (len(base["groups"]), yours, extended)


def glob_match(p, s):
    """'*' any run without '/', '**' any run (and "**/" also matches no segment), '?' one
    non-'/' character - utg::globMatch."""
    i = j = 0
    while i < len(p):
        if p.startswith("**", i):
            i += 2
            if i == len(p):
                return True
            if p[i] == "/" and glob_match(p[i + 1:], s[j:]):
                return True
            return any(glob_match(p[i:], s[t:]) for t in range(j, len(s) + 1))
        if p[i] == "*":
            i += 1
            t = j
            while True:
                if glob_match(p[i:], s[t:]):
                    return True
                if t >= len(s) or s[t] == "/":
                    return False
                t += 1
        if j >= len(s):
            return False
        if p[i] == "?":
            if s[j] == "/":
                return False
        elif p[i] != s[j]:
            return False
        i += 1
        j += 1
    return j == len(s)


def view_keys(name):
    """The keys a view answers to in [view:<key>] - utg::viewKeysFor."""
    if not name:
        return []
    keys = [name]
    c = name.find("custom-")
    if c >= 0 and c + 7 < len(name):
        keys.append(name[c + 7:])
    if name.startswith("basemods") and name != "basemods":
        keys.append("basemods")
    return keys


def group_item(i):
    """What the group rules read of a catalogue.json item (pages_gen.cpp groupItem)."""
    flags = 0
    for key, bit in (("isSetPiece", 1), ("isRelic", 2), ("isEquipment", 4), ("isBlueprint", 16),
                     ("isAugment", 32), ("isExtra", 64)):
        if i.get(key):
            flags |= bit
    if i.get("isMi"):        # a Rare staff / hunting spear is equipment to the rules too
        flags |= 4
    if i["record"].lower().replace("\\", "/").startswith("records/items/faction/"):
        flags |= 8
    return {"record": i["record"].lower().replace("\\", "/"), "class": (i.get("class") or "").lower(),
            "rank": (i.get("itemClassification") or "").lower(), "flags": flags,
            "fromMod": bool(i.get("fromMod")), "known": i.get("slot") != "other",
            "class_orig": i.get("class") or ""}


def match_item(gs, it, keys):
    """The first active block that matches: a group index, -1 (an [exclude]) or -2 (none)."""
    for r in gs["rules"]:
        if r["view"] and r["view"] not in keys:
            continue
        if r["classes"] and it["class"] not in r["classes"]:
            continue
        if r["ranks"] and r["ranks"][0] != "*" and it["rank"] not in r["ranks"]:
            continue
        if r["flags"] and not r["flags"] & it["flags"]:
            continue
        if r["source"] == "base" and it["fromMod"] or r["source"] == "mod" and not it["fromMod"]:
            continue
        if r["paths"] and not any(glob_match(g, it["record"]) for g in r["paths"]):
            continue
        return -1 if r["exclude"] else r["group"]
    return -2


def assign_groups(gs, items, keys):
    """-> (groups [[id, label, tag]] in display order, group index per item or -1): the declared
    groups, then the auto groups by class name, then "other" - utg::assignGroups."""
    # id, label, tag, labelDerived, tagDerived, cls
    groups = [[d[0], d[1], d[2], d[6], d[7], d[8]] for d in gs["groups"]]
    group_of = [-1] * len(items)
    auto = {}
    for n, it in enumerate(items):
        m = match_item(gs, it, keys)
        if m >= 0:
            group_of[n] = m
        elif m == -2 and not it["known"] and gs["auto"]:
            auto.setdefault(it["class_orig"], []).append(n)
    other = []
    for cls in sorted(auto):
        if len(auto[cls]) < gs["min"]:
            other += auto[cls]
            continue
        label, tag = AUTO_NAMES.get(cls.lower(), (None, None))
        if label is None:
            label = cls.rsplit("_", 1)[-1] or cls or "Other"
            label = label[:48]
            tag = tag_from(label)
        gid = "auto." + "".join(c if c in ID_CHARS else "-" for c in cls.lower())[:35]
        groups.append([gid, label, tag, True, True, low_ascii(cls)])
        for n in auto[cls]:
            group_of[n] = len(groups) - 1
    if other:
        groups.append(["other", "Other", "OTH", False, False, ""])
        for n in sorted(other):
            group_of[n] = len(groups) - 1
    used = [False] * len(groups)
    for g in group_of:
        if g >= 0:
            used[g] = True
    resolve_names(gs.get("words", {}), groups, used)
    return [d[:3] for d in groups], group_of


def class_words():
    """catalogue_gen.cpp classWords: lower-cased class -> the view's English text for its type
    tag - the base Text_EN.arc, then the view's text_en*.arc over it (build_catalogue.py's
    order). A tag with no text is left out."""
    import arc as arcmod
    tags = arcmod.load_text_tags(GAME, "EN")
    if VIEW is not None:
        for a in VIEW.arcs:
            if build_catalogue.is_text_arc(a.path, "en"):
                for key in a.keys():
                    if key.endswith(".txt"):
                        data = a.read(key)
                        if data:
                            tags.update(arcmod.parse_tag_file(data))
    return {c: tags[t] for c, t in CLASS_TYPE_TAGS if tags.get(t)}


def stock_templates(out_dir):
    """Every groups template an earlier build wrote as uniquetab-groups.ini, byte for byte as the
    DLL wrote it (CRLF), copied into out_dir for test_groups' .old retirement. The fixtures are
    data/oracle/templates/<n>.ini, n = the "earlier template n" of src/gen/ut_groups_default.h
    (each one is the raw string of that header in the build that wrote it). 0 = copied or
    SKIPPED (no fixtures: the public tree leaves data/oracle out)."""
    src_dir = os.path.join(ROOT, "data", "oracle", "templates")
    names = []
    if os.path.isdir(src_dir):
        names = sorted((n for n in os.listdir(src_dir) if n.endswith(".ini") and n[:-4].isdigit()),
                       key=lambda n: int(n[:-4]))
    if not names:
        print("stock templates: SKIPPED (no fixtures in %s)" % src_dir)
        return 0
    os.makedirs(out_dir, exist_ok=True)
    for n in names:
        with open(os.path.join(src_dir, n), "rb") as fh:
            data = fh.read()
        with open(os.path.join(out_dir, n), "wb") as fh:
            fh.write(data)
    print("stock templates: %d copied to %s" % (len(names), out_dir))
    return 0


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
        # the mod's own icon first, as the engine loads it (pages_gen.cpp: ModView::texSize)
        if VIEW is not None and (VIEW.layers or VIEW.arcs):
            wh = build_catalogue.view_tex_size(VIEW, bitmap)
            if wh:
                size = wh if (wh[0] <= 1024 and wh[1] <= 1024) else None
                self.cache[bitmap] = size
                return size
        key = bitmap.replace("\\", "/").lower()
        data = self.arc.read(key)
        if data is None and key.startswith("items/"):
            data = self.arc.read(key[len("items/"):])
        size = tex_size(data)
        self.cache[bitmap] = size
        return size


def pick_items(args, bitmaps):
    cat = json.load(open(os.path.join(ORACLE, "catalogue.json"), encoding="utf-8"))
    vanilla = VIEW is None
    # The collection rule. Equipment is Epic/Legendary only; a relic is in at EVERY
    # classification (the Rare b-series included), which is why the test is not one predicate.
    # build_catalogue.py already asserted the relic counts and the total; this is the same
    # rule restated on its output, and the assert below proves the two agree.
    # `isExtra` is the curated inclusion list. It is a THIRD predicate on purpose -
    # a curated record is neither Epic/Legendary equipment nor a relic, and folding it into
    # either test would silently widen that rule.
    # The groups as data: every catalogue record is matched against the built-in
    # uniquetab-groups.ini (src/gen/ut_groups_default.h), first block wins. Its slot groups ARE
    # the collection rule above, which the asserts below prove on the vanilla catalogue.
    gs = parse_groups(template_text())
    tpl = [d[:3] for d in gs["groups"] if d[0] in SLOT_LABEL]
    assert tpl == [list(b) for b in BUILTIN], "the groups template's slot groups moved: %r" % (tpl,)
    if args.groups:
        gs, counts = merge_groups(parse_groups(open(args.groups, encoding="utf-8").read()), gs)
        print("groups: %d built-in + %d of yours (%d built-in group(s) given more rules)" % counts)
    gs["words"] = class_words()
    # collect_mi=1: every view also answers to [view:collect_mi], the template's MI blocks
    # (generate.cpp kCollectMiViewKey); off, no MI is in the catalogue and none is paged
    keys = view_keys(view_name(args)) + (["collect_mi"] if args.collect_mi else [])
    gdefs, group_of = assign_groups(gs, [group_item(i) for i in cat["items"]], keys)
    print("groups of the view (the buttons file's defaults): " + "; ".join(
        "%s=%s" % (t, l) for _, l, t in view_group_table(gdefs, group_of)))
    items = []
    for i, g in zip(cat["items"], group_of):
        if g >= 0:
            i["group"] = g
            items.append(i)
    if args.collect_mi:
        vanilla = False     # the vanilla counts below are the flag-off collection's
    n_relics = sum(1 for i in items if i.get("isRelic"))
    assert args.groups or not vanilla or n_relics == RELIC_EXPECT_TOTAL, (
        "catalogue.json carries %d relics, expected %d - rebuild catalogue.json first "
        "(python build_catalogue.py)" % (n_relics, RELIC_EXPECT_TOTAL))
    n_extra = sum(1 for i in items if i.get("isExtra"))
    assert args.groups or not vanilla or n_extra == EXTRA_EXPECT_TOTAL, (
        "catalogue.json carries %d curated extras, expected %d - rebuild catalogue.json first "
        "(python build_catalogue.py)" % (n_extra, EXTRA_EXPECT_TOTAL))
    n_bp = sum(1 for i in items if i.get("class") == "ItemArtifactFormula")
    assert args.groups or not vanilla or n_bp == BLUEPRINT_EXPECT_TOTAL, (
        "catalogue.json carries %d paged blueprints, expected %d - rebuild catalogue.json first "
        "(python build_catalogue.py)" % (n_bp, BLUEPRINT_EXPECT_TOTAL))
    assert args.groups or not vanilla or len(items) == COLLECTION_EXPECT_TOTAL, (
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
    # Rare ranks after Epic, so the 21 b-series relics land at the END of the Relics group and
    # every box above them keeps its index and its position - a tier added later must never
    # renumber the boxes a player has already seen.  Common ranks after Rare for the same
    # reason: Leovinus' Ring is appended to the END of the Rings group (box 103 of the group's
    # second page) and no ring box moves.  Ordering INSIDE a tier is name, then record.
    rank = {"Legendary": 0, "Epic": 1, "Rare": 2, "Common": 3}
    fitted.sort(key=lambda i: (i["group"],
                               rank.get(i["itemClassification"], 9),
                               build_catalogue.strip_colour_codes(i["name"]).lower(), i["record"]))
    return fitted, dropped, gdefs


def slot_grid(group):
    """(cellW, pitchY, cols, rows) for one slot group - the cell is its largest icon."""
    cw = max(i["bitmapWH"][0] for i in group)
    ch = max(i["bitmapWH"][1] for i in group)
    cols = max(1, USABLE_W // cw)
    rows = max(1, USABLE_H // (ch + ROW_GAP))
    while cols * rows > FRAME_BOXES and rows > 1:
        rows -= 1
    return cw, ch + ROW_GAP, cols, rows


def frame_boxes(items, plate_w, plate_h):
    """The frame's box count - ut_layout.h utFrameBoxes: over the groups' distinct cells (the
    largest icon of each group), the most boxes the whole cover holds, uncapped, then clamped
    to FRAME_FLOOR..FRAME_MAX."""
    cell = {}
    for it in items:
        w, h = it["bitmapWH"]
        cw, ch = cell.get(it["group"], (0, 0))
        cell[it["group"]] = (max(cw, w), max(ch, h))
    best = 0
    for cw, ch in set(cell.values()):
        cols = max(1, max(1, plate_w) // max(1, cw))
        rows = max(1, max(1, plate_h) // (max(1, ch) + ROW_GAP))
        best = max(best, cols * rows)
    return min(max(best, FRAME_FLOOR), FRAME_MAX)


def paginate(items, limit_pages):
    """[(items, cellW, pitchY, cols, rows), ...] - a new page at every group boundary."""
    groups, order = {}, []
    for it in items:
        s = it["group"]
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


# ---- the buttons file: a port of src/ut_buttons.h utbDefaultText (byte for byte) ------------
BUTTONS_FILE = "uniquetab-buttons.ini"
# The built-in groups that came after 1.1.0's 24 (ut_buttons.h utbLateBuiltinIds: the blueprints,
# then the template's per-class staff and spear1h): the default file names those the game has on
# its "added once" line, so a button the user deletes is never offered back.
BUTTONS_LATE = ["blueprint", "staff", "spear1h"]
BUTTONS_OFFERED = "; added once by the mod - delete its button and it stays deleted:"
BUTTONS_TAB = [
    ("label.format.1", "{mode}{group} - {owned}/{entries} owned - row {first}-{last}/{rows} - coll {all}/{coll}"),
    ("label.format.2", "{mode}{group} - {owned}/{entries} - row {first}-{last}/{rows} - coll {all}/{coll}"),
    ("label.mode_unknown", "mode unknown - "),
    ("label.unknown", "?"),
    ("tooltip.yes", "In your collection"),
    ("tooltip.no", "Not in your collection"),
    ("mat.caption", "MAT"),
    ("own.caption", "OWN"),
    ("next.caption", ">>"),
]
BUTTONS_HEADER = (
    "; uniquetab-buttons.ini - the buttons of the Unique Collection tab in %s.\n"
    "; Written once by the mod; edit it freely (it is re-read within a second); delete it to\n"
    "; get the default back. UTF-8. A line starting with ; is a comment, and so is the rest of\n"
    "; a line after a blank and a ;. Put a value in \"double quotes\" to keep blanks or a ; in it.\n"
    ";\n"
    "; [TAG]    one button; the section name is its tag (1 to 3 characters; MAT, OWN and TAB\n"
    ";          are taken). Button order = file order.\n"
    ";   name    = the page name in the label line\n"
    ";   caption = the button's text when it is not the tag (optional)\n"
    ";   groups  = the item groups on it (ids from the groups file); several ids = one button,\n"
    ";             all of their items on one page, in group order\n"
    ";   size    = this button's largest caption size, 6..36 (optional)\n"
    ";   font    = this button's font, for example fonts/ru/savapromedium.fnt (optional; by\n"
    ";             default a font follows the letters it has to draw)\n"
    "; [tab]    the label line's templates and words, the rollover lines, the MAT, OWN and >>\n"
    ";          captions; it also takes font.caption, font.label, size.label,\n"
    ";          size.caption_max, size.caption and color.caption, color.label, color.ring,\n"
    ";          color.mark (RRGGBB). Label placeholders: {mode} {group} {owned} {entries}\n"
    ";          {first} {last} {rows} {all} {coll}.\n"
    "; [unassigned] the groups no button holds - their items have no page. The mod rewrites\n"
    ";          that section (only it) whenever the set changes: move an id from it into a\n"
    ";          button's groups line to show them.\n"
    "; A mistake anywhere makes the mod use the built-in buttons; the log names the line.\n"
    "\n"
    "[tab]\n")


def strip_colour_codes(s):
    """ut_panel.cpp stripColourCodesTo: {^X} and ^X go."""
    out, i = [], 0
    while i < len(s):
        if s[i] == "{" and s[i + 1:i + 2] == "^" and i + 2 < len(s) and s[i + 3:i + 4] == "}":
            i += 4
            continue
        if s[i] == "^" and i + 1 < len(s):
            i += 2
            continue
        out.append(s[i])
        i += 1
    return "".join(out)


def buttons_quote(v):
    """utbQuote: double quotes when blanks around it or a ';' after a blank would be lost."""
    q = (not v) or v[0] in " \t\"" or v[-1] in " \t"
    q = q or any(v[i] == ";" and v[i - 1] in " \t" for i in range(1, len(v)))
    q = q or (bool(v) and v[0] in ";#")
    return '"%s"' % v if q else v


def buttons_valid_tag(t):
    return 1 <= len(t) <= 3 and all(0x20 < ord(c) < 0x7F and c not in '[];=,"#' for c in t)


def buttons_name(label):
    """utbNameOf: at most 48 UTF-16 units, cut at a code point."""
    n = label
    while len(n.encode("utf-16-le")) // 2 > 48:
        n = n[:-1]
    return n


def buttons_view_title(custom_key):
    return "the custom game " + custom_key if custom_key else "the main campaign"


def buttons_default_text(view_title, rows):
    """rows: (id, tag, label) per group of the view, in group order -> the file, LF."""
    t = BUTTONS_HEADER % view_title
    for k, v in BUTTONS_TAB:
        t += "%-18s = %s\n" % (k, buttons_quote(v))
    unassigned, seen = [], []
    for gid, tag, label in rows:
        if (not buttons_valid_tag(tag) or tag.upper() in ("MAT", "OWN", "TAB")
                or tag.upper() in [x.upper() for x in seen]):
            unassigned.append(gid)
            continue
        seen.append(tag)
        t += "\n[%s]\n" % tag
        t += "name    = %s\n" % buttons_quote(buttons_name(label))
        t += "groups  = %s\n" % gid
    t += "\n[unassigned]\n; rewritten by the mod: every group of this game that no button lists\n"
    late = [gid for gid in BUTTONS_LATE if gid in [r[0] for r in rows]]
    if late:
        t += BUTTONS_OFFERED + " " + ", ".join(late) + "\n"
    t += "groups ="
    t += "".join((", " if i else " ") + u for i, u in enumerate(unassigned))
    return t + "\n"


def buttons_rows_from_groups(path):
    """(id, tag, label) of every G line of a uniq-groups.txt, colour codes stripped."""
    rows = []
    for line in open(path, encoding="latin-1"):
        f = line.rstrip("\r\n").split("\t")
        if f[0] == "G" and len(f) >= 10:
            try:   # the label's bytes: UTF-8 when they are, else latin-1 (utbNameOf's rule)
                label = f[2].encode("latin-1").decode("utf-8")
            except UnicodeDecodeError:
                label = f[2]
            rows.append((f[8], f[9], strip_colour_codes(label)))
    return rows


def write_buttons_file(folder, custom_key):
    """The default buttons file beside a view's uniq-groups.txt (CRLF, UTF-8): what the DLL writes
    at the game's first launch."""
    rows = buttons_rows_from_groups(os.path.join(folder, "uniq-groups.txt"))
    text = buttons_default_text(buttons_view_title(custom_key), rows)
    with open(os.path.join(folder, BUTTONS_FILE), "wb") as fh:
        fh.write(text.replace("\n", "\r\n").encode("utf-8"))
    print("buttons: %s written - %d button(s)" % (os.path.join(folder, BUTTONS_FILE), len(rows)))
    return 0

def view_name(args):
    """vanilla | basemods | custom-<name> | basemods+custom-<name> - gen::ViewSpec::name."""
    leaf = ""
    if args.custom:
        leaf = os.path.basename(os.path.normpath(args.custom)).lower()
        leaf = "".join(c if c in ID_CHARS else "-" for c in leaf)
    if args.basemods:
        return "basemods+custom-" + leaf if leaf else "basemods"
    return "custom-" + leaf if leaf else "vanilla"


def geometry(args):
    """Derive the grid rectangle and the plate, and install the four module globals.

    The vanilla derivation must not move the shipped layout. It is one pixel taller than the
    constants this tool used to hold (the cover sits at y=1, and the usable rectangle is
    measured from the cover, not from the window - see ui_geometry.py), so the assert below
    proves that every cell height in play still yields the same number of rows.
    """
    global COL_X0, ROW_Y0, USABLE_W, USABLE_H
    geo = ui_geometry.derive(GAME, args.basemods, args.custom, view=VIEW)
    if not args.basemods and not args.custom:
        assert geo.tuple() == ui_geometry.VANILLA, (
            "the vanilla derivation moved: %r, expected %r" % (geo.tuple(), ui_geometry.VANILLA))
        for ch in (32, 64, 96, 128):
            assert (SHIPPED_USABLE_H // (ch + ROW_GAP)) == (geo.usableH // (ch + ROW_GAP)), (
                "a %d px cell would change the row count" % ch)
    COL_X0, ROW_Y0 = geo.x0, geo.y0
    USABLE_W, USABLE_H = geo.usableW, geo.usableH
    print("geometry: %s" % geo)
    for note in geo.notes:
        print("  %s" % note)
    return geo


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--annotate-groups", action="store_true",
                    help="only add the item record to uniq-groups.txt (no game access, "
                         "nothing else is rewritten)")
    ap.add_argument("--limit-pages", type=int, default=0, help="stop after N pages (debug)")
    ap.add_argument("--basemods", action="store_true",
                    help="derive the geometry from the view the /basemods command line builds")
    ap.add_argument("--custom", help="derive the geometry from a custom game (a name under "
                                     "<game>" + chr(92) + "mods, or a full path to one)")
    ap.add_argument("--groups", help="a user's uniquetab-groups.ini, merged in front of the "
                                     "built-in template as the DLL does")
    ap.add_argument("--stock-templates", help="copy every template an earlier build wrote "
                                              "(data" + chr(92) + "oracle" + chr(92) +
                                              "templates, CRLF) into this folder and stop")
    ap.add_argument("--out", help="write the outputs (and read catalogue.json) here instead of "
                                  "data" + chr(92) + "oracle - a view's fixtures")
    ap.add_argument("--collect-mi", action="store_true",
                    help="uniquetab.ini's collect_mi=1: the groups' [view:collect_mi] blocks (the "
                         "monster infrequents on their slot pages) apply; catalogue.json must "
                         "have been built with build_catalogue.py --collect-mi")
    ap.add_argument("--buttons-from", help="write the default uniquetab-buttons.ini beside this "
                                           "folder's uniq-groups.txt (no game access) and stop")
    ap.add_argument("--buttons-view", default="", help="with --buttons-from: the custom game's key "
                                                        "(empty = the main campaign)")
    args = ap.parse_args()
    global ARGS
    ARGS = args
    if args.buttons_from:
        return write_buttons_file(args.buttons_from, args.buttons_view)
    if args.stock_templates:
        return stock_templates(args.stock_templates)
    if args.annotate_groups:
        return annotate_groups()
    global ORACLE, ARZ_DIR, SRC_DIR, VIEW
    if args.out:
        ORACLE = ARZ_DIR = os.path.abspath(args.out)
        SRC_DIR = os.path.join(ORACLE, "src")
    if args.basemods or args.custom:
        VIEW = ui_geometry.View(GAME, args.basemods, args.custom)

    os.makedirs(SRC_DIR, exist_ok=True)
    geo = geometry(args)
    db = VIEW.db if VIEW is not None else ArzDatabase.load_game(GAME)
    src_tag = db.source_of(MATERIAL_WINDOW)
    arc = db.archives[db._index[MATERIAL_WINDOW]]
    # the materials window the engine will build: the last mod layer that has it
    for layer, label in (VIEW.layers if VIEW is not None else []):
        if MATERIAL_WINDOW in layer:
            arc, src_tag = layer, label
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
    items, dropped, gdefs = pick_items(args, bitmaps)
    global FRAME_BOXES
    FRAME_BOXES = frame_boxes(items, geo.plateW, geo.plateH)
    print("frame: %d boxes (the %dx%d cover's fit at the view's cell sizes, %d..%d)"
          % (FRAME_BOXES, geo.plateW, geo.plateH, FRAME_FLOOR, FRAME_MAX))
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
    page_group = []      # the group index of every page (not in the manifest, which keeps its keys)
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
        grp = page_items[0]["group"]
        page_group.append(grp)
        same = [id(q[0]) for q in pages if q[0][0]["group"] == grp]
        manifest_pages.append({
            "page": p, "record": page_rec, "slot": slot,
            "label": gdefs[grp][1],
            "sub": same.index(id(page_items)) + 1, "subOf": len(same),
            "cols": cols, "rows": rows, "cellW": cellW, "cellH": pitchY - ROW_GAP,
            "boxes": len(box_records), "items": page_manifest,
        })

    # ---- the frame record + the hidden filler boxes -------------------------------------
    van_boxes = list(arc.get(MATERIAL_WINDOW).get("reagentBoxes") or [])
    assert van_boxes, "the vanilla materials page has no reagentBoxes"
    assert len(van_boxes) <= FRAME_BOXES, "%d vanilla boxes > FRAME_BOXES" % len(van_boxes)
    hide_item = (VIEW.rec(van_boxes[0])[0] if VIEW is not None else db.get(van_boxes[0])).get("reagentName")
    assert hide_item, "cannot read reagentName out of %s" % van_boxes[0]
    hidden_recs = []
    for n in range(FRAME_BOXES - len(van_boxes)):
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
    # can scroll it by rows through the frame's FRAME_BOXES boxes.
    groups = []
    for m, grp in zip(manifest_pages, page_group):
        if groups and groups[-1]["group"] == grp:
            g = groups[-1]
        else:
            g = {"group": grp, "id": gdefs[grp][0], "tag": gdefs[grp][2], "label": m["label"],
                 "cellW": m["cellW"], "cellH": m["cellH"], "cols": m["cols"], "entries": []}
            groups.append(g)
        for it in m["items"]:
            # The BOX record is what the widget shows, the ITEM record is what the engine's
            # reagent map is keyed by. The DLL needs both.
            g["entries"].append((BOX_FMT % (m["page"], it["box"]), it["record"]))
    for g in groups:
        rows = max(1, USABLE_H // (g["cellH"] + ROW_GAP))
        while g["cols"] * rows > FRAME_BOXES and rows > 1:
            rows -= 1
        g["rows"] = rows

    # The G line ends with the group's id and its 3-character tag (src/gen/ut_groups.h).
    with open(os.path.join(ORACLE, "uniq-groups.txt"), "w", encoding="latin-1", newline="\r\n") as fh:
        fh.write("F\t%s\t%d\t%d\t%d\t%d\t%d\n"
                 % (FRAME_REC, FRAME_BOXES, len(van_boxes), COL_X0, ROW_Y0, ROW_GAP))
        fh.write(geo.line() + "\n")
        for r in van_boxes:
            fh.write("V\t%s\n" % r)
        for n, g in enumerate(groups):
            fh.write("G\t%d\t%s\t%d\t%d\t%d\t%d\t%d\t%s\t%s\n"
                     % (n, g["label"], g["cols"], g["rows"], g["cellW"], g["cellH"],
                        len(g["entries"]), g["id"], g["tag"]))
            for r in g["entries"]:
                fh.write("E\t%s\t%s\n" % (r[0], r[1]))
    print("uniq-groups.txt: %d groups, %d entries, frame %d boxes"
          % (len(groups), sum(len(g["entries"]) for g in groups), FRAME_BOXES))
    vn = view_name(ARGS) if ARGS is not None else "vanilla"
    write_buttons_file(ORACLE, vn[vn.find("custom-") + 7:] if "custom-" in vn else "")
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
    assert len(fr["reagentBoxes"]) == FRAME_BOXES, len(fr["reagentBoxes"])
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
        "rowGap": ROW_GAP, "maxPerPage": FRAME_BOXES,
        "plate": geo.plate, "plateW": geo.plateW, "plateH": geo.plateH,
        "geometryView": geo.view,
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
