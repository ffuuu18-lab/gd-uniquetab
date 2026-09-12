"""Read Grim Dawn's own bitmap fonts (Fonts.arc, "FNTX" v2) and measure text EXACTLY the way
`GraphicsCanvas::RenderText2d` measures it.  OFFLINE ONLY - it never touches the game process;
it reads the shipped archive through `arc.py`, the same way `arz.py` reads the database.

FNTX v2 layout, decoded by stepping savapromedium.fnt:

  0x00 u32 'FNTX'   0x04 u32 version(2)   0x08 u32 texW(708)   0x0C u32 texH(708)
  0x10 u32 ?        0x14 u32 texture blob offset                0x18 u32 numStyles(16)
  0x1C u32 0
  then numStyles blocks, each:
      u32 size, u32 flags, u32 ascent, u32 lineHeight, u32 numGlyphs, u32 numKern, u32 0
      numGlyphs * 28 B: u16 code, s16 xOffset, s16 rightBearing, u16 w, u16 h, u16 yTop,
                        float uv[4]
      numKern  *  8 B: u16 first, u16 second, s16 amount, s16 pad
  Proven twice over: the codes are exactly {0} + 32..126 ascending, and uv[2]-uv[0] times the
  texture width equals `w` for every single glyph (the parser asserts it).

THE PEN RULE IS NOT GUESSED - it is read out of Engine.dll's own layout loop
(`GraphicsCanvas::RenderText2d` -> 0x9EEC0 -> 0xA6140 -> the left-aligned line builder 0xA7630,
the loop at 0xA76C5..0xA7819):

    0A7763  add ebx, r12d                 ; pen += PREVIOUS glyph's metric+0x04   (right bearing,
                                          ;   skipped only between two spaces, 0A7754)
    0A7779  call GetKerningAmount         ; pen += kerning(prev, cur)             (0A7782)
    0A7796  movsx eax, cx / add ebx, eax  ; pen += metric+0x02                    (x offset)
    0A779B  movzx eax, word ptr [rdi+6]   ; w
    0A77A6  mulss xmm1, xmm6              ; * scale, xmm6 = requestedSize / styleSize (0A6370)
    0A77AA  addss xmm1, xmm7              ; + 0.5      (the constant at Engine.dll rva 0x34BABC)
    0A77EA  addss xmm1, (float)pen        ; pen += (int)(w * scale + 0.5)
    0A77D8  movsx r12d, word ptr [rdi+4]  ; remember this glyph's right bearing
    0A7831  add ebx, r12d                 ; after the last glyph, add its right bearing

  so   advance(c) = xOffset + rightBearing + round(w * size/styleSize) + kerning(prev,c)

`GetStyle` (Engine.dll 0xAE550) scores the baked styles by |requestedSize - style.size| and
takes the nearest, so `styleSize` is the baked size closest to the size the caller asked for -
and the glyph bitmap is SCALED by size/styleSize while the two bearings are NOT.

Usage:
    python font_metrics.py sizes <font.fnt>
    python font_metrics.py adv   <font.fnt> <size>
    python font_metrics.py width <font.fnt> <size> <text...>
    python font_metrics.py table <font.fnt> <lo> <hi>   C table, 1/1024 em, sizes lo..hi
"""

from __future__ import annotations

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import arc  # noqa: E402
import gdpath  # noqa: E402

GAME = gdpath.find_game_dir() or ""
FONTS_ARC = os.path.join(GAME, "resources", "Fonts.arc")

GLYPH = struct.Struct("<HhhHHH")


def read_font(name: str) -> bytes:
    return arc.ArcArchive(FONTS_ARC).read(name)


def parse(blob: bytes):
    magic, ver, texw, _texh, _u, _texoff, nstyles, _z = struct.unpack_from("<8I", blob, 0)
    if magic != 0x58544E46 or ver != 2:
        raise ValueError("not an FNTX v2 font")
    out = {}
    off = 0x20
    for _ in range(nstyles):
        size, _flags, ascent, line, ng, nk, _z2 = struct.unpack_from("<7I", blob, off)
        off += 28
        glyphs = {}
        prev = -1
        for i in range(ng):
            code, xoff, rb, w, h, ytop = GLYPH.unpack_from(blob, off + 28 * i)
            uv = struct.unpack_from("<4f", blob, off + 28 * i + 12)
            if code < prev:
                raise ValueError("glyph codes not ascending at %d" % i)
            prev = code
            if w and abs((uv[2] - uv[0]) * texw - w) > 0.51:
                raise ValueError("glyph %d: uv width != w" % code)
            glyphs[code] = (xoff, rb, w, h, ytop)
        off += ng * 28
        kern = {}
        for i in range(nk):
            a, b, amt, _p = struct.unpack_from("<HHhh", blob, off + 8 * i)
            kern[(a, b)] = amt
        off += nk * 8
        out[size] = {"ascent": ascent, "line": line, "glyphs": glyphs, "kern": kern}
    return out


def style_for(styles, want: int) -> int:
    """Engine.dll 0xAE550: the baked size nearest to the requested one (ties to the first)."""
    return min(sorted(styles), key=lambda s: (abs(s - want), s))


def adv(styles, want: int, ch: str) -> int:
    """One glyph's pen advance, mid-string: xOffset + rightBearing + round(w * size/styleSize).
    No kerning (it is only ever <= 0 in these fonts, so leaving it out over-estimates) and no
    line-start clamp (that one applies to the first glyph of a line only)."""
    ss = style_for(styles, want)
    m = styles[ss]["glyphs"].get(ord(ch)) or styles[ss]["glyphs"].get(ord("?")) or (0, 0, 0, 0, 0)
    xoff, rb, w, _h, _y = m
    return xoff + rb + int(w * (float(want) / float(ss)) + 0.5)


def width(styles, want: int, text: str) -> int:
    """Exactly Engine.dll's pen, in screen pixels, for `RenderText2d(..., size=want)`."""
    ss = style_for(styles, want)
    g = styles[ss]["glyphs"]
    kern = styles[ss]["kern"]
    scale = float(want) / float(ss)
    pen = 0
    prev = None
    for ch in text:
        m = g.get(ord(ch)) or g.get(ord("?")) or (0, 0, 0, 0, 0)
        xoff, rb, w, _h, _y = m
        if prev is not None:
            pm = g.get(ord(prev)) or (0, 0, 0, 0, 0)
            if not (ch == " " and prev == " "):
                pen += pm[1]
            pen += kern.get((ord(prev), ord(ch)), 0)
        if xoff < 0 and pen < -xoff:
            pen = -xoff
        pen += xoff
        pen += int(w * scale + 0.5)
        prev = ch
    if prev is not None:
        pen += (g.get(ord(prev)) or (0, 0, 0, 0, 0))[1]
    return pen


def cap_box(styles, want: int):
    """(w, h) of the 'H' bitmap as the engine will draw it at `want`."""
    ss = style_for(styles, want)
    xoff, rb, w, h, _y = styles[ss]["glyphs"][ord("H")]
    sc = float(want) / float(ss)
    return int(w * sc + 0.5), int(h * sc + 0.5)


def emit_cxx(fname: str, styles) -> None:
    """Emit src\\ut_fontmetrics.h - the shipped font's own numbers, so the mod can run the
    engine's pen rule itself instead of guessing an em."""
    sizes = sorted(styles)
    def rows(vals, per=24, fmt="%4d"):
        out = []
        for i in range(0, len(vals), per):
            out.append("    " + " ".join((fmt + ",") % v for v in vals[i:i + per]))
        return "\n".join(out)
    print("// GENERATED by tools\\font_metrics.py cxx %s - DO NOT EDIT BY HAND." % fname)
    print("// Grim Dawn's own %s, every baked style, ASCII 32..126." % fname)
    print("// The pen rule is Engine.dll's (see the header comment of font_metrics.py):")
    print("//     advance(c) = bearing[c] + round(w[c] * size / styleSize)")
    print("// plus, for the FIRST glyph of a line only, up to `slack` px because the engine")
    print("// clamps a negative xOffset to 0 there (Engine.dll 0xA7789).")
    print("#pragma once")
    print("")
    print("namespace ut {")
    print("namespace fontmetrics {")
    print("")
    print("const int kStyles = %d;" % len(sizes))
    print("const int kFirstChar = 32;")
    print("const int kChars = 95;")
    print("const short kStyleSize[kStyles] = { %s };" % ", ".join(str(s) for s in sizes))
    slack = []
    for s in sizes:
        mn = min(g[0] for g in styles[s]["glyphs"].values())
        slack.append(max(0, -mn))
    print("const short kStyleSlack[kStyles] = { %s };" % ", ".join(str(v) for v in slack))
    print("")
    print("const unsigned char kGlyphW[kStyles][kChars] = {")
    for s in sizes:
        g = styles[s]["glyphs"]
        vals = [min(255, g[c][2] if c in g else 0) for c in range(32, 127)]
        print("    {   // style size %d" % s)
        print(rows(vals))
        print("    },")
    print("};")
    print("")
    print("const signed char kGlyphBearing[kStyles][kChars] = {")
    for s in sizes:
        g = styles[s]["glyphs"]
        vals = [max(-128, min(127, (g[c][0] + g[c][1]) if c in g else 0)) for c in range(32, 127)]
        print("    {   // style size %d" % s)
        print(rows(vals))
        print("    },")
    print("};")
    print("")
    print("}  // namespace fontmetrics")
    print("}  // namespace ut")


def main() -> None:
    if len(sys.argv) < 3:
        print(__doc__)
        return
    cmd, fname = sys.argv[1], sys.argv[2]
    styles = parse(read_font(fname))
    if cmd == "sizes":
        for s in sorted(styles):
            print("size %2d  ascent %2d  lineHeight %2d  glyphs %d  kern %d" %
                  (s, styles[s]["ascent"], styles[s]["line"], len(styles[s]["glyphs"]),
                   len(styles[s]["kern"])))
    elif cmd == "adv":
        want = int(sys.argv[3])
        ss = style_for(styles, want)
        print("requested %d -> baked style %d, scale %.4f" % (want, ss, want / ss))
        for c in range(32, 127):
            print("  %3d %r  %2d px  (%.3f em)" % (c, chr(c), width(styles, want, chr(c)),
                                                   width(styles, want, chr(c)) / want))
    elif cmd == "width":
        want = int(sys.argv[3])
        text = " ".join(sys.argv[4:])
        w = width(styles, want, text)
        print("size %d (style %d)  %d chars  %d px  %.3f em/char  \"%s\"" %
              (want, style_for(styles, want), len(text), w, w / want / max(1, len(text)), text))
    elif cmd == "table":
        lo, hi = int(sys.argv[3]), int(sys.argv[4])
        vals = []
        for c in range(32, 127):
            m = max(adv(styles, s, chr(c)) / s for s in range(lo, hi + 1))
            vals.append(max(0, min(2047, int(m * 1024 + 0.999))))
        print("// %s: per-glyph advance in 1/1024 em, the LARGEST over requested sizes %d..%d"
              % (fname, lo, hi))
        for i in range(0, len(vals), 10):
            print("    " + " ".join("%4d," % v for v in vals[i:i + 10]))
    elif cmd == "cxx":
        emit_cxx(fname, styles)
    elif cmd == "check":
        # prove the baked table never under-estimates a real string
        lo, hi = int(sys.argv[3]), int(sys.argv[4])
        tab = {}
        for c in range(32, 127):
            tab[c] = max(0, min(2047, int(max(adv(styles, s, chr(c)) / s
                                              for s in range(lo, hi + 1)) * 1024 + 0.999)))
        worst = 0.0
        texts = sys.argv[5:]
        for t in texts:
            for s in range(lo, hi + 1):
                est = sum(tab[ord(ch)] for ch in t) * s // 1024
                real = width(styles, s, t)
                if real > est:
                    print("UNDER-ESTIMATE size %d: est %d < real %d  \"%s\"" % (s, est, real, t))
                worst = max(worst, (est - real) / max(1, real))
        print("worst over-estimate %.1f%% over %d strings x sizes %d..%d"
              % (worst * 100, len(texts), lo, hi))
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
