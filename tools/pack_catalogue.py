#!/usr/bin/env python3
"""Reference implementation; the DLL generates this file itself now (src/gen/catalogue_gen).

Pack data/oracle/catalogue.json into data/oracle/catalogue.bin.

Compact little-endian binary, loadable by the mod DLL without a JSON library:
a header, a group table, a fixed-size item table, a set table, a set-member
index array and a UTF-8 string table.  Format documented in
src/model/catalogue.h; the reader is src/model/catalogue.cpp.

Deterministic: the same catalogue.json always produces a byte-identical
catalogue.bin (item order is slot group, level requirement, name, record).

Usage:
    python pack_catalogue.py [--in catalogue.json] [--out catalogue.bin] [--check]

--check re-packs into memory and compares against the file on disk instead of
writing (exit 1 on a difference).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys

FORMAT_VERSION = 1
MAGIC = b"GDUT"
HEADER_SIZE = 80
ITEM_ENTRY_SIZE = 24
SET_ENTRY_SIZE = 16

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEFAULT_IN = os.path.join(ROOT, "data", "oracle", "catalogue.json")
DEFAULT_OUT = os.path.join(ROOT, "data", "oracle", "catalogue.bin")

# ---------------------------------------------------------------- slot groups
# Display order.  Values are the on-disk enum; keep in sync with
# SlotGroup in src/model/catalogue.h.
GROUP_NAMES = [
    "Helm",         # 0
    "Shoulders",    # 1
    "Chest",        # 2
    "Gloves",       # 3
    "Belt",         # 4
    "Pants",        # 5
    "Boots",        # 6
    "Axe1H",        # 7
    "Mace1H",       # 8
    "Sword1H",      # 9
    "Dagger",       # 10
    "Scepter",      # 11
    "Axe2H",        # 12
    "Mace2H",       # 13
    "Sword2H",      # 14
    "Spear2H",      # 15
    "Ranged1H",     # 16
    "Ranged2H",     # 17
    "Offhand",      # 18
    "Shield",       # 19
    "Ring",         # 20
    "Amulet",       # 21
    "Medal",        # 22
    "Relic",        # 23
    "Blueprint",    # 24
    "Augment",      # 25
    "Consumable",   # 26
    "Quest",        # 27
    "Other",        # 28
]
GROUP_COUNT = len(GROUP_NAMES)
GROUP_OTHER = GROUP_COUNT - 1

# catalogue.json "slot" -> group index
SLOT_TO_GROUP = {
    "head": 0,
    "shoulders": 1,
    "chest": 2,
    "hands": 3,
    "waist": 4,
    "legs": 5,
    "feet": 6,
    "axe1h": 7,
    "mace1h": 8,
    "sword1h": 9,
    "dagger": 10,
    "scepter": 11,
    "axe2h": 12,
    "mace2h": 13,
    "sword2h": 14,
    "spear2h": 15,
    "ranged1h": 16,
    "ranged2h": 17,
    "offhand": 18,
    "shield": 19,
    "ring": 20,
    "amulet": 21,
    "medal": 22,
    "relic": 23,
    "blueprint": 24,
    "augment": 25,
    "consumable": 26,
    "quest": 27,
}

# On-disk classification byte.  THE READER VALIDATES IT: src/model/catalogue.cpp
# rejects the whole file with "item classification out of range" for any value >=
# kClassificationCount (src/model/catalogue.h, which is 2), so this table MUST NOT grow
# past 1 while that enum has two members.
#
# Relics are collected at EVERY classification, so catalogue.json carries 21 `Rare` relic
# entries.  They are packed as Epic (0) - a deliberate, logged narrowing, NOT a silent one: the
# true word stays in catalogue.json, catalogue.bin's classification byte is read by nothing
# outside the overlay (`g_cat` appears only in ut_panel.cpp, which runs with `enabled=0`), and
# the collection page itself is driven by data/uniq/*, not by this file.
# To carry Rare honestly, model/catalogue.h needs `Rare = 2` + `kClassificationCount = 3`, and
# layout.h's kAllClassificationsMask then follows automatically - a C++ change, not this tool's.
#
# The curated inclusion list (build_catalogue.py's EXTRA_RECORDS) adds a `Common` entry -
# Leovinus' Ring - for exactly the same reason, and it is narrowed the same way.
CLASSIFICATION = {"Epic": 0, "Legendary": 1}
CLASSIFICATION_NARROWED = {"Rare": 0, "Common": 0}

# item flags (u16)
F_SET_PIECE = 1 << 0
F_BLUEPRINT = 1 << 1
F_AUGMENT = 1 << 2
F_RELIC = 1 << 3
F_FACTION = 1 << 4
F_EQUIPMENT = 1 << 5
F_DEFAULT_VISIBLE = 1 << 6   # isEquipment || isRelic
F_HAS_BITMAP = 1 << 7

FACTION_PREFIX = "records/items/faction/"


class StringTable:
    """Deduplicating UTF-8 blob of NUL-terminated strings; ref 0 == empty."""

    def __init__(self) -> None:
        self.blob = bytearray(b"\x00")
        self.offsets = {"": 0}

    def add(self, s: str) -> int:
        ref = self.offsets.get(s)
        if ref is not None:
            return ref
        ref = len(self.blob)
        self.blob += s.encode("utf-8") + b"\x00"
        self.offsets[s] = ref
        return ref


def group_of(item: dict) -> int:
    return SLOT_TO_GROUP.get(item.get("slot", ""), GROUP_OTHER)


def flags_of(item: dict) -> int:
    f = 0
    if item.get("isSetPiece"):
        f |= F_SET_PIECE
    if item.get("isBlueprint"):
        f |= F_BLUEPRINT
    if item.get("isAugment"):
        f |= F_AUGMENT
    if item.get("isRelic"):
        f |= F_RELIC
    if item.get("record", "").startswith(FACTION_PREFIX):
        f |= F_FACTION
    if item.get("isEquipment"):
        f |= F_EQUIPMENT
    if item.get("isEquipment") or item.get("isRelic"):
        f |= F_DEFAULT_VISIBLE
    if item.get("bitmap"):
        f |= F_HAS_BITMAP
    return f


def clamp_u16(v, what: str, record: str) -> int:
    try:
        n = int(v)
    except (TypeError, ValueError):
        n = 0
    if n < 0:
        n = 0
    if n > 0xFFFF:
        raise SystemExit("%s out of range (%d) on %s" % (what, n, record))
    return n


def build(cat: dict) -> tuple[bytes, dict]:
    items = list(cat["items"])

    # ---- deterministic order: slot group, level requirement, name, record
    decorated = []
    for it in items:
        rec = it["record"]
        decorated.append(
            (group_of(it), clamp_u16(it.get("levelRequirement", 0), "levelRequirement", rec),
             it.get("name", ""), rec, it)
        )
    decorated.sort(key=lambda t: (t[0], t[1], t[2], t[3]))

    index_of_record = {t[3]: i for i, t in enumerate(decorated)}
    if len(index_of_record) != len(decorated):
        raise SystemExit("duplicate record paths in catalogue.json")

    # ---- item sets: display name + member item indices, ordered by set record
    set_records = sorted({it["itemSetName"] for _, _, _, _, it in decorated if it.get("itemSetName")})
    set_index_of = {r: i for i, r in enumerate(set_records)}
    set_display = {}
    set_members: list[list[int]] = [[] for _ in set_records]
    for i, (_, _, _, _, it) in enumerate(decorated):
        sr = it.get("itemSetName") or ""
        if not sr:
            continue
        si = set_index_of[sr]
        set_members[si].append(i)
        set_display.setdefault(sr, it.get("setDisplayName", "") or "")

    # ---- string table (populated in traversal order == deterministic)
    st = StringTable()
    item_rows = []
    narrowed = []
    for i, (grp, lvl, name, rec, it) in enumerate(decorated):
        word = it.get("itemClassification", "")
        cls = CLASSIFICATION.get(word, None)
        if cls is None:
            cls = CLASSIFICATION_NARROWED.get(word, None)
            if cls is None:
                raise SystemExit("unexpected itemClassification %r on %s" % (word, rec))
            narrowed.append((word, rec))
        sr = it.get("itemSetName") or ""
        item_rows.append((
            st.add(rec),
            st.add(name),
            st.add(it.get("bitmap", "") or ""),
            set_index_of[sr] if sr else -1,
            cls,
            grp,
            lvl,
            clamp_u16(it.get("itemLevel", 0), "itemLevel", rec),
            flags_of(it),
        ))

    set_rows = []
    member_blob = []
    for si, sr in enumerate(set_records):
        first = len(member_blob)
        member_blob.extend(set_members[si])
        set_rows.append((st.add(set_display.get(sr, "")), st.add(sr), first, len(set_members[si])))

    # ---- group ranges (items are sorted by group, so ranges are contiguous)
    group_first = [0] * GROUP_COUNT
    group_count = [0] * GROUP_COUNT
    for i, row in enumerate(item_rows):
        g = row[5]
        if group_count[g] == 0:
            group_first[g] = i
        group_count[g] += 1

    # ---- section layout
    item_count = len(item_rows)
    set_count = len(set_rows)
    member_count = len(member_blob)

    group_off = HEADER_SIZE
    group_size = GROUP_COUNT * 8
    item_off = group_off + group_size
    item_size = item_count * ITEM_ENTRY_SIZE
    set_off = item_off + item_size
    set_size = set_count * SET_ENTRY_SIZE
    member_off = set_off + set_size
    member_size = member_count * 4
    string_off = member_off + member_size
    string_size = len(st.blob)
    total = string_off + string_size
    if total % 4:
        total += 4 - (total % 4)          # pad the tail to a 4-byte multiple

    equipment_count = sum(1 for r in item_rows if r[8] & F_EQUIPMENT)
    relic_count = sum(1 for r in item_rows if r[8] & F_RELIC)
    default_count = sum(1 for r in item_rows if r[8] & F_DEFAULT_VISIBLE)

    out = bytearray()
    out += MAGIC
    out += struct.pack(
        "<19I",
        FORMAT_VERSION, HEADER_SIZE, 0,
        item_count, ITEM_ENTRY_SIZE, item_off,
        set_count, SET_ENTRY_SIZE, set_off,
        member_count, member_off,
        GROUP_COUNT, group_off,
        string_size, string_off,
        total,
        default_count, equipment_count, relic_count,
    )
    assert len(out) == HEADER_SIZE, len(out)

    for g in range(GROUP_COUNT):
        out += struct.pack("<II", group_first[g], group_count[g])
    assert len(out) == item_off

    for row in item_rows:
        out += struct.pack("<IIIiBBHHH", *row)
    assert len(out) == set_off, (len(out), set_off)

    for row in set_rows:
        out += struct.pack("<IIII", *row)
    assert len(out) == member_off

    for m in member_blob:
        out += struct.pack("<I", m)
    assert len(out) == string_off

    out += st.blob
    while len(out) % 4:
        out += b"\x00"
    assert len(out) == total, (len(out), total)

    stats = {
        "items": item_count,
        "sets": set_count,
        "setMembers": member_count,
        "strings": len(st.offsets),
        "stringBytes": string_size,
        "equipment": equipment_count,
        "relics": relic_count,
        "defaultVisible": default_count,
        "bytes": total,
        "groupFirst": group_first,
        "groupCount": group_count,
        "itemRows": item_rows,
        "decorated": decorated,
        "narrowed": narrowed,
    }
    return bytes(out), stats


def print_summary(blob: bytes, stats: dict, src: str, dst: str) -> None:
    print("pack_catalogue: %s -> %s" % (src, dst))
    print("  format version %d, header %d B, item entry %d B, set entry %d B"
          % (FORMAT_VERSION, HEADER_SIZE, ITEM_ENTRY_SIZE, SET_ENTRY_SIZE))
    print("  items            %6d" % stats["items"])
    print("  equipment        %6d   (expected 3197 = 3196 Epic/Legendary + 1 curated extra)"
          % stats["equipment"])
    print("  relics           %6d   (expected 91 = 21 Rare + 21 Epic + 49 Legendary)"
          % stats["relics"])
    print("  default visible  %6d   (equipment + relics, expected 3288)" % stats["defaultVisible"])
    print("  item sets        %6d  with %d member links" % (stats["sets"], stats["setMembers"]))
    print("  strings          %6d unique, %d B" % (stats["strings"], stats["stringBytes"]))
    print("  total size       %6d B (%.2f MB)" % (stats["bytes"], stats["bytes"] / 1048576.0))
    print("  sha256           %s" % hashlib.sha256(blob).hexdigest())
    print("  per slot group (index: name = count):")
    for g, name in enumerate(GROUP_NAMES):
        n = stats["groupCount"][g]
        if n:
            print("    %2d %-11s %5d  first=%d" % (g, name, n, stats["groupFirst"][g]))
    epics = sum(1 for r in stats["itemRows"] if r[4] == 0)
    print("  classification: Epic %d, Legendary %d" % (epics, stats["items"] - epics))
    if stats["narrowed"]:
        by_word = {}
        for word, rec in stats["narrowed"]:
            by_word[word] = by_word.get(word, 0) + 1
        print("  NARROWED classification bytes: "
              + ", ".join("%s -> %d (%d records)" % (w, CLASSIFICATION_NARROWED[w], n)
                          for w, n in sorted(by_word.items())))
        print("    (model/catalogue.h Classification has only Epic=0/Legendary=1 and "
              "catalogue.cpp:237 rejects >= 2; the true word is in catalogue.json)")
        print("    first: " + ", ".join(r for _, r in stats["narrowed"][:3]))
    flagged = {
        "setPiece": F_SET_PIECE, "blueprint": F_BLUEPRINT, "augment": F_AUGMENT,
        "relic": F_RELIC, "faction": F_FACTION, "hasBitmap": F_HAS_BITMAP,
    }
    print("  flags: " + ", ".join(
        "%s %d" % (k, sum(1 for r in stats["itemRows"] if r[8] & m)) for k, m in flagged.items()))


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--in", dest="src", default=DEFAULT_IN)
    ap.add_argument("--out", dest="dst", default=DEFAULT_OUT)
    ap.add_argument("--check", action="store_true",
                    help="compare with the existing file instead of writing")
    args = ap.parse_args(argv)

    with open(args.src, "r", encoding="utf-8") as fh:
        cat = json.load(fh)

    blob, stats = build(cat)
    print_summary(blob, stats, args.src, args.dst)

    if args.check:
        if not os.path.exists(args.dst):
            print("CHECK FAILED: %s does not exist" % args.dst)
            return 1
        with open(args.dst, "rb") as fh:
            old = fh.read()
        if old != blob:
            print("CHECK FAILED: %s differs (%d B on disk, %d B packed)"
                  % (args.dst, len(old), len(blob)))
            return 1
        print("CHECK OK: byte-identical to %s" % args.dst)
        return 0

    tmp = args.dst + ".tmp"
    with open(tmp, "wb") as fh:
        fh.write(blob)
    os.replace(tmp, args.dst)
    print("wrote %s" % args.dst)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
