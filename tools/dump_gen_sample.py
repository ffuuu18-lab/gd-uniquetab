"""Python side of tools\\build_test_catalogue.bat: the same counts and the same deterministic
sample of decoded records and text tags that test_catalogue.exe --readers dumps, produced by
the reference readers arz.py / arc.py so the two dumps can be diffed.

    python dump_gen_sample.py <out.txt>        game folder from %GD_DIR% (gdpath.py)
"""

from __future__ import annotations

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import arc  # noqa: E402
import arz  # noqa: E402
import gdpath  # noqa: E402


def main(out_path: str) -> int:
    game = gdpath.game_dir()
    db = arz.ArzDatabase.load_game(game)
    tags = arc.load_text_tags(game)
    items = arc.ArcSet.load(game, os.path.join("resources", "Items.arc"))
    ui = arc.ArcSet.load(game, os.path.join("resources", "UI.arc"))
    with open(out_path, "w", encoding="utf-8", newline="\n") as fh:
        for a in db.archives:
            fh.write("arz\t%s\t%d\t%d\n" % (a.tag, len(a), len(a.strings)))
        fh.write("merged\t%d\n" % len(db))
        fh.write("tags\t%d\n" % len(tags))
        fh.write("items.arc\t%d\n" % len(items))
        fh.write("ui.arc\t%d\n" % len(ui))
        keys = list(db.keys())
        step = max(1, len(keys) // 200)
        for n, i in enumerate(range(0, len(keys), step)):
            if n >= 200:
                break
            key = keys[i]
            src = db.source_of(key)
            rec = db.get(key)
            fh.write("record\t%s\t%s\n" % (key, src))
            arch = db.archives[db._index[key]]
            entry = arch.entries[key]
            fh.write("".join(_fields(arch, entry)))
        tkeys = sorted(tags)
        tstep = max(1, len(tkeys) // 200)
        for i in range(0, len(tkeys), tstep):
            fh.write("tag\t%s=%s\n" % (tkeys[i], tags[tkeys[i]]))
    return 0


def _fields(arch, entry):
    """Field lines with the raw types: the last field of a name wins, sorted by name."""
    import lz4.block

    raw = lz4.block.decompress(arch._blob[24 + entry.offset: 24 + entry.offset + entry.csize],
                               uncompressed_size=entry.dsize)
    last = {}
    p = 0
    while p + 8 <= len(raw):
        ftype, count, name_idx = struct.unpack_from("<HHI", raw, p)
        p += 8
        vals = struct.unpack_from("<%dI" % count, raw, p) if count else ()
        p += 4 * count
        name = arch.strings[name_idx] if name_idx < len(arch.strings) else "?%d" % name_idx
        last[name] = (ftype, vals)
    out = []
    for name in sorted(last):
        ftype, vals = last[name]
        if ftype == arz.FT_STRING:
            txt = ",".join(arch.strings[v] if v < len(arch.strings) else "" for v in vals)
        elif ftype == arz.FT_FLOAT:
            txt = ",".join("%08x" % v for v in vals)
        else:
            txt = ",".join(str(struct.unpack("<i", struct.pack("<I", v))[0]) for v in vals)
        out.append("  %s\t%d\t%s\n" % (name, ftype, txt))
    return out


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
