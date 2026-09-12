#!/usr/bin/env python3
# gds_read.py - a REFERENCE READER for GD Stash's .gds import/export file.
#
#   python tools\gds_read.py <file.gds> [--json] [--quiet]
#
# It is written from the decompiled spec, not from the example file, and it is deliberately
# STRICTER than GD Stash: it insists on reading the whole file, so a writer that gets one field
# wrong ends with "trailing bytes" or "unexpected EOF" instead of a plausible-looking dump.
# Nothing here writes anything, and nothing here goes near GD Stash's Derby database.
#
# THE SPEC (GDStash.jar 1.90b, javap -c -p -constants):
#   org.gdstash.db.DBStashItem.writeDBStashItemListGDS  ->  int version (always 3), int count,
#       then count x writeGDS
#   org.gdstash.db.DBStashItem.loadGDS  ->  int version, int count, then count x readGDS, and
#       the version is NOT validated: it only gates which fields are present (>=2, >=3)
#   org.gdstash.file.GDWriter.intToBytes4 / GDReader.readInt   4 bytes LITTLE ENDIAN
#   org.gdstash.file.GDWriter.writeStringUByte                 1 length byte, then the UTF-8
#       bytes; a null string is a single 0x00 and reads back as None (GDReader.readStringUByte)
#   org.gdstash.util.GDConstants.STR_CHARSET_STASH = "UTF-8"
#
# Field order (readGDS), v = the file's version int:
#   itemID, prefixID, suffixID, modifierID, transmuteID : string
#   seed : int
#   relicID, relicBonusID : string        relicSeed : int
#   enchantmentID : string                enchantmentLevel, enchantmentSeed : int
#   ascendantID, ascendant2hID : string   (v >= 2 only)
#   var1, stackCount : int                rerollsUsed : int (v >= 2)   affixRerollsUsed : int (v >= 3)
#   hardcore : 1 byte bool                charname : string
#
# The mod fills those from one journal entry's ItemReplicaInfo - see src\ut_gds.h for the
# offset-by-offset mapping and for what the journal cannot fill.
import json
import sys


class GdsError(Exception):
    pass


class _Reader:
    def __init__(self, data):
        self.d = data
        self.p = 0

    def need(self, n, what):
        if self.p + n > len(self.d):
            raise GdsError(
                "unexpected EOF reading %s at offset 0x%X: wanted %d byte(s), %d left"
                % (what, self.p, n, len(self.d) - self.p))

    def u32(self, what):
        self.need(4, what)
        v = int.from_bytes(self.d[self.p:self.p + 4], "little", signed=False)
        self.p += 4
        return v

    def i32(self, what):
        self.need(4, what)
        v = int.from_bytes(self.d[self.p:self.p + 4], "little", signed=True)
        self.p += 4
        return v

    def byte(self, what):
        self.need(1, what)
        v = self.d[self.p]
        self.p += 1
        return v

    def string(self, what):
        n = self.byte(what + " length")
        if n == 0:
            return None            # GDReader.readStringUByte returns null, not ""
        self.need(n, what)
        raw = self.d[self.p:self.p + n]
        self.p += n
        try:
            return raw.decode("utf-8")
        except UnicodeDecodeError as e:
            raise GdsError("%s is not valid UTF-8 at offset 0x%X: %s" % (what, self.p - n, e))


# The 20 fields in file order. (name, kind, min_version); kind is s/i/b.
FIELDS = [
    ("itemID", "s", 1), ("prefixID", "s", 1), ("suffixID", "s", 1),
    ("modifierID", "s", 1), ("transmuteID", "s", 1),
    ("seed", "i", 1),
    ("relicID", "s", 1), ("relicBonusID", "s", 1), ("relicSeed", "i", 1),
    ("enchantmentID", "s", 1), ("enchantmentLevel", "i", 1), ("enchantmentSeed", "i", 1),
    ("ascendantID", "s", 2), ("ascendant2hID", "s", 2),
    ("var1", "i", 1), ("stackCount", "i", 1),
    ("rerollsUsed", "i", 2), ("affixRerollsUsed", "i", 3),
    ("hardcore", "b", 1), ("charname", "s", 1),
]


def read_gds(data):
    """Parse .gds bytes -> {"version":int, "count":int, "items":[dict...]}. Raises GdsError."""
    r = _Reader(data)
    version = r.i32("the version int")
    count = r.i32("the item count")
    if count < 0:
        raise GdsError("the item count is negative (%d)" % count)
    items = []
    for idx in range(count):
        it = {}
        for name, kind, minv in FIELDS:
            if version < minv:
                continue
            what = "item %d's %s" % (idx + 1, name)
            if kind == "s":
                it[name] = r.string(what)
            elif kind == "i":
                it[name] = r.i32(what)
            else:
                it[name] = r.byte(what) != 0
        items.append(it)
    if r.p != len(data):
        raise GdsError("trailing bytes: %d item(s) consumed %d of %d bytes"
                       % (count, r.p, len(data)))
    return {"version": version, "count": count, "items": items}


def describe(doc, quiet=False):
    out = []
    out.append("version %d, %d item(s)" % (doc["version"], doc["count"]))
    for i, it in enumerate(doc["items"]):
        out.append("")
        out.append("[%d] %s" % (i + 1, it.get("itemID") or "(no record!)"))
        for name, kind, minv in FIELDS:
            if name == "itemID" or name not in it:
                continue
            v = it[name]
            if quiet and (v is None or v == 0 or v is False):
                continue
            out.append("      %-17s %s" % (name, "(null)" if v is None else v))
    return "\n".join(out)


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    flags = set(a for a in argv[1:] if a.startswith("--"))
    if len(args) != 1:
        print(__doc__ or "usage: gds_read.py <file.gds> [--json] [--quiet]")
        print("usage: gds_read.py <file.gds> [--json] [--quiet]")
        return 2
    with open(args[0], "rb") as f:
        data = f.read()
    try:
        doc = read_gds(data)
    except GdsError as e:
        print("PARSE FAILED (%d bytes): %s" % (len(data), e))
        return 1
    if "--json" in flags:
        print(json.dumps(doc, indent=2, ensure_ascii=False))
    else:
        print("%s  (%d bytes)" % (args[0], len(data)))
        print(describe(doc, "--quiet" in flags))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
