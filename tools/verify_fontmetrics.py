"""Read the SHIPPED src\\ut_fontmetrics.h back and prove the arithmetic ut_panel.cpp does with it
never under-estimates what Engine.dll's pen will draw.

It re-implements `textWidthPx` in Python with the same INTEGER maths the C does, runs it against
the exact pen from font_metrics.py (which models kerning and the line-start clamp as well), and
reports the worst error over every string the mod can put on screen: all 24 group labels in both
label rungs, all 25 button tags, at every requested size from 5 to 36.

    python verify_fontmetrics.py            (from tools\\; exit 0 = pass)
"""

from __future__ import annotations

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import font_metrics as fm  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
HEADER = os.path.join(HERE, "..", "src", "ut_fontmetrics.h")
GROUPS = os.path.join(HERE, "..", "data", "oracle", "uniq-groups.txt")
TAGS = ("MAT HEL SHO CHE GLO BEL PAN BOO AMU MED RNG OFF SHD AX1 DAG MC1 SCP SW1 GUN AX2 MC2 "
        "SPR SW2 GN2 REL").split()
TOTAL = 3288


def load_header(path: str):
    txt = open(path, encoding="utf-8").read()
    txt = re.sub(r"//[^\n]*", "", txt)   # the "// style size N" comments carry digits too

    def arr1(name):
        m = re.search(r"\b%s\[[^\]]*\]\s*=\s*\{([^}]*)\}" % name, txt)
        return [int(v) for v in re.findall(r"-?\d+", m.group(1))]

    def arr2(name):
        m = re.search(r"\b%s\[[^\]]*\]\[[^\]]*\]\s*=\s*\{(.*?)\n\};" % name, txt, re.S)
        blocks = re.findall(r"\{(.*?)\}", m.group(1), re.S)
        return [[int(v) for v in re.findall(r"-?\d+", b)] for b in blocks]

    return arr1("kStyleSize"), arr1("kStyleSlack"), arr2("kGlyphW"), arr2("kGlyphBearing")


def c_width(text, size, sizes, slack, gw, gb):
    """byte-for-byte what ut_panel.cpp's textWidthPx does"""
    best, bestd = 0, 1 << 24
    for i, s in enumerate(sizes):
        d = abs(size - s)
        if d < bestd:
            bestd, best = d, i
    ss = sizes[best]
    pen = 0
    for ch in text:
        c = ord(ch)
        if c < 32 or c >= 32 + 95:
            c = ord("?")
        i = c - 32
        pen += gb[best][i] + (gw[best][i] * size + ss // 2) // ss
    return pen + slack[best]


def main() -> int:
    sizes, slack, gw, gb = load_header(HEADER)
    if len(sizes) != len(gw) or len(sizes) != len(gb) or any(len(r) != 95 for r in gw + gb):
        print("FAIL: the header's tables do not have 95 entries per style")
        return 1
    styles = fm.parse(fm.read_font("savapromedium.fnt"))

    texts = list(TAGS)
    for ln in open(GROUPS, encoding="utf-8"):
        p = ln.rstrip("\n").split("\t")
        if p[0] != "G":
            continue
        label, cols, rows, ent = p[2], int(p[3]), int(p[4]), int(p[7])
        tot = max(1, (ent + cols - 1) // cols)
        for own, coll in ((0, 6), (ent, TOTAL)):
            for first, last in ((1, min(rows, tot)), (max(1, tot - rows + 1), tot)):
                texts.append("%s - %d/%d owned - row %d-%d/%d - coll %d/%d"
                             % (label, own, ent, first, last, tot, coll, TOTAL))
                texts.append("%s - %d/%d - row %d-%d/%d - coll %d/%d"
                             % (label, own, ent, first, last, tot, coll, TOTAL))
                texts.append("%s - %d records - row %d-%d/%d" % (label, ent, first, last, tot))

    under = 0
    worst = (0.0, None)
    for t in texts:
        for size in range(5, 37):
            est = c_width(t, size, sizes, slack, gw, gb)
            real = fm.width(styles, size, t)
            if est < real:
                under += 1
                if under <= 5:
                    print("UNDER size %d est %d real %d  \"%s\"" % (size, est, real, t))
            d = (est - real) / max(1, real)
            if d > worst[0]:
                worst = (d, (t, size, est, real))
    print("%d strings x sizes 5..36 = %d measurements" % (len(texts), len(texts) * 32))
    print("under-estimates: %d" % under)
    print("worst over-estimate: %.1f%%  %s" % (worst[0] * 100, worst[1]))
    return 1 if under else 0


if __name__ == "__main__":
    sys.exit(main())
