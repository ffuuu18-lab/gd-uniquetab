"""Find the UI style records the GameTextClass names point at, and print each style's colour.

`colours` is the mode that produced the colour table in ut_tooltip.cpp, so that table can be
reproduced by running it.  Every mode below is implemented.

Usage:
  python find_textstyles.py colours         -> THE TABLE: every GameTextClass style NAME (a
                                               field of records/game/gameengine.dbr), the
                                               style record it names, its fontColor0 as
                                               0..1 floats and as RGB 0..255, and its size
  python find_textstyles.py colours <name>  -> the same, for the style names containing <name>
  python find_textstyles.py rec <path>      -> dump one record
  python find_textstyles.py grep <needle>   -> every record PATH containing <needle>
  python find_textstyles.py field <needle>  -> every records/ui/* field whose key or value
                                               contains <needle>
"""
from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gdpath  # noqa: E402
from arz import ArzDatabase  # noqa: E402

GAME = gdpath.find_game_dir() or ""


STYLE_PREFIX = "records/ui/styles/"


def colours(db, needle):
    """Every style FIELD of gameengine.dbr, resolved to its record's own fontColor0.

    This is the half of the class table the census cannot see: the census gives class -> style
    NAME out of Game.dll, and the style name is a field of records/game/gameengine.dbr whose
    value is the style record that carries the colour.  (The three
    records/sandbox/arthur/archive/gameengine*.dbr copies carry the same field names and are NOT
    the live record - ItemEnchantmentStats points at _sizen there and _sizet here.)
    """
    ge = db.get("records/game/gameengine.dbr")
    if ge is None:
        print("records/game/gameengine.dbr is not in this database")
        return 1
    n = 0
    for key in sorted(ge):
        val = ge[key]
        if not isinstance(val, str) or not val.lower().startswith(STYLE_PREFIX):
            continue
        if needle and needle not in key.lower():
            continue
        st = db.get(val)
        if st is None:
            print("%-34s %s   (no such record)" % (key, val))
            n += 1
            continue
        r = float(st.get("fontColor0.R", st.get("fontColorRed", 0.0)))
        g = float(st.get("fontColor0.G", st.get("fontColorGreen", 0.0)))
        b = float(st.get("fontColor0.B", st.get("fontColorBlue", 0.0)))
        print("%-34s %-62s %.3f %.3f %.3f = RGB (%d,%d,%d)  size %s"
              % (key, val[len("records/ui/styles/"):], r, g, b,
                 round(r * 255), round(g * 255), round(b * 255), st.get("fontSize", "?")))
        n += 1
    print("-- %d style field(s)" % n)
    return 0


def main():
    db = ArzDatabase.load_game(GAME)
    mode = sys.argv[1] if len(sys.argv) > 1 else "help"
    if mode == "colours":
        return colours(db, sys.argv[2].lower() if len(sys.argv) > 2 else "")
    if mode == "rec":
        rec = db.get(sys.argv[2])
        if rec is None:
            print("no such record")
            return 1
        for k in sorted(rec):
            print("  %-34s %r" % (k, rec[k]))
        return 0
    if mode == "grep":
        needle = sys.argv[2].lower()
        n = 0
        for path in db.paths():
            if needle in path.lower():
                print(path)
                n += 1
        print("-- %d paths" % n)
        return 0
    if mode == "field":
        needle = sys.argv[2].lower()
        n = 0
        for path in db.paths():
            if not path.lower().startswith("records/ui/"):
                continue
            rec = db.get(path)
            if rec is None:
                continue
            for k, v in rec.items():
                if needle in str(v).lower() or needle in k.lower():
                    print("%s | %s = %r" % (path, k, v))
                    n += 1
        print("-- %d hits" % n)
        return 0
    print(__doc__)
    return 1


if __name__ == "__main__":
    sys.exit(main())
