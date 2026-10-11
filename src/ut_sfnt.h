// ut_sfnt.h - the code points a TrueType / OpenType font maps to a glyph (its cmap), read from the
// bytes of an sfnt file, of a TrueType collection (face 0), or of the game's FNTD font file (the
// sfnt after a 28-byte header). Every offset and length is checked against the buffer: a file
// that points outside it is refused, never read past.
//
// Pure: standard headers only, no Windows, no engine. ut_panel.cpp's worker reads the game's FNTD
// copies of savapromedium (zh, ja, ko - th and vi carry zh's font) out of resources\Fonts.arc with
// it, so a role's font can follow its letters to a font the engine rasterises at run time through
// GDI, which draws a letter the font lacks from Windows' own fallback font; tools/test_text.cpp
// proves it over the real Fonts.arc.
//
// The FNTD file (GraphicsFont2::Initialize, Engine.dll 1.3.0.8 0xAFB80): 'FNTD', version 1, five
// u32 (the outline switch first), then the raw sfnt at +0x1C. Face 0 is the face the engine names
// to CreateFontIndirectA (the 'name' family of the first face). The subtable read is the one
// fontTools' getBestCmap picks, among formats 4 and 12: (3,10) (0,6) (0,4) (3,1) (0,3) (0,2)
// (0,1) (0,0). A code point counts when it maps to a glyph other than 0 (.notdef).
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ut {

const std::uint32_t kUtFntdMagic = 0x44544E46u;   // "FNTD" read as a little-endian u32
const std::size_t kUtFntdHeader = 0x1C;           // 7 x u32, then the sfnt

inline std::uint32_t utSfntBe32(const unsigned char* p) {
    return ((std::uint32_t)p[0] << 24) | ((std::uint32_t)p[1] << 16) | ((std::uint32_t)p[2] << 8) |
           (std::uint32_t)p[3];
}
inline unsigned utSfntBe16(const unsigned char* p) { return ((unsigned)p[0] << 8) | (unsigned)p[1]; }

// Is `d` the game's FNTD font file (a TrueType body the engine draws through GDI at run time)?
// Eight bytes are enough to tell: a caller may pass only the head of the file.
inline bool utFntdIs(const unsigned char* d, std::size_t n) {
    if (!d || n < 8) return false;
    const std::uint32_t magic = (std::uint32_t)d[0] | ((std::uint32_t)d[1] << 8) |
                                ((std::uint32_t)d[2] << 16) | ((std::uint32_t)d[3] << 24);
    const std::uint32_t ver = (std::uint32_t)d[4] | ((std::uint32_t)d[5] << 8) |
                              ((std::uint32_t)d[6] << 16) | ((std::uint32_t)d[7] << 24);
    return magic == kUtFntdMagic && ver == 1;
}

// The sorted code points face 0 maps to a glyph. `d` is an FNTD file, a TTC or a bare sfnt.
// False (and `out` empty) for anything else, for a buffer that ends early, or when face 0 has no
// cmap subtable of format 4 or 12; `format` (optional) gets the format read.
inline bool utSfntCodePoints(const unsigned char* d, std::size_t n, std::vector<char32_t>* out,
                             int* format = nullptr) {
    if (format) *format = 0;
    if (!out) return false;
    out->clear();
    if (!d) return false;
    if (utFntdIs(d, n)) {
        if (n < kUtFntdHeader) return false;
        d += kUtFntdHeader;
        n -= kUtFntdHeader;
    }
    if (n < 12) return false;
    std::size_t face = 0;
    if (utSfntBe32(d) == 0x74746366u) {   // 'ttcf': the first face's offset table
        if (n < 16 || utSfntBe32(d + 8) < 1) return false;
        face = utSfntBe32(d + 12);
        if (face > n || n - face < 12) return false;
    }
    const std::uint32_t ver = utSfntBe32(d + face);
    if (ver != 0x00010000u && ver != 0x4F54544Fu /* OTTO */ && ver != 0x74727565u /* true */)
        return false;
    const std::size_t nt = utSfntBe16(d + face + 4);
    if (nt * 16 > n - face - 12) return false;
    std::size_t cmap = 0, cmapEnd = 0;
    for (std::size_t i = 0; i < nt; ++i) {
        const unsigned char* r = d + face + 12 + i * 16;
        if (utSfntBe32(r) != 0x636D6170u) continue;   // 'cmap'
        const std::size_t off = utSfntBe32(r + 8), len = utSfntBe32(r + 12);
        if (off > n || len > n - off || len < 4) return false;
        cmap = off;
        cmapEnd = off + len;
        break;
    }
    if (!cmapEnd) return false;
    const std::size_t ns = utSfntBe16(d + cmap + 2);
    if (ns * 8 > cmapEnd - cmap - 4) return false;
    static const unsigned kPrefer[8][2] = {{3, 10}, {0, 6}, {0, 4}, {3, 1}, {0, 3}, {0, 2}, {0, 1}, {0, 0}};
    for (const auto& want : kPrefer) {
        for (std::size_t i = 0; i < ns; ++i) {
            const unsigned char* r = d + cmap + 4 + i * 8;
            if (utSfntBe16(r) != want[0] || utSfntBe16(r + 2) != want[1]) continue;
            const std::size_t sub = cmap + utSfntBe32(r + 4);
            if (sub < cmap || sub > cmapEnd || cmapEnd - sub < 8) continue;
            const std::size_t room = cmapEnd - sub;   // a subtable never reaches past its cmap
            const unsigned char* s = d + sub;
            const unsigned fmt = utSfntBe16(s);
            if (fmt == 12) {
                if (room < 16) continue;
                const std::size_t ng = utSfntBe32(s + 12);
                if (ng > (room - 16) / 12) continue;
                std::size_t total = 0;
                bool bad = false;
                for (std::size_t g = 0; g < ng; ++g) {
                    const unsigned char* q = s + 16 + g * 12;
                    const std::uint32_t a = utSfntBe32(q), b = utSfntBe32(q + 4);
                    if (a > b || b > 0x10FFFFu) {
                        bad = true;
                        break;
                    }
                    total += (std::size_t)(b - a) + 1;
                    if (total > 0x110000u) {
                        bad = true;
                        break;
                    }
                }
                if (bad) continue;
                out->reserve(total);
                for (std::size_t g = 0; g < ng; ++g) {
                    const unsigned char* q = s + 16 + g * 12;
                    const std::uint32_t a = utSfntBe32(q), b = utSfntBe32(q + 4), gid = utSfntBe32(q + 8);
                    for (std::uint32_t c = a; c <= b; ++c)
                        if (gid + (c - a) != 0) out->push_back((char32_t)c);
                }
            } else if (fmt == 4) {
                const std::size_t seg2 = utSfntBe16(s + 6);
                if (seg2 & 1 || room < 16 + 4 * seg2) continue;
                const std::size_t segs = seg2 / 2;
                const unsigned char* endA = s + 14;
                const unsigned char* startA = s + 16 + seg2;
                const unsigned char* deltaA = s + 16 + 2 * seg2;
                const unsigned char* roA = s + 16 + 3 * seg2;
                bool bad = false;
                for (std::size_t k = 0; k < segs && !bad; ++k) {
                    const unsigned a = utSfntBe16(startA + 2 * k), b = utSfntBe16(endA + 2 * k);
                    const unsigned delta = utSfntBe16(deltaA + 2 * k), ro = utSfntBe16(roA + 2 * k);
                    if (a > b) continue;
                    for (unsigned c = a; c <= b; ++c) {
                        if (c == 0xFFFF) break;   // the terminating segment
                        unsigned gid;
                        if (ro == 0) {
                            gid = (c + delta) & 0xFFFF;
                        } else {
                            // idRangeOffset is relative to its own slot in the array
                            const std::size_t at = 16 + 3 * seg2 + 2 * k + ro + 2 * (std::size_t)(c - a);
                            if (at + 2 > room) {
                                bad = true;
                                break;
                            }
                            const unsigned g = utSfntBe16(s + at);
                            gid = g ? (g + delta) & 0xFFFF : 0;
                        }
                        if (gid) out->push_back((char32_t)c);
                    }
                }
                if (bad) {
                    out->clear();
                    continue;
                }
            } else {
                continue;
            }
            std::sort(out->begin(), out->end());
            out->erase(std::unique(out->begin(), out->end()), out->end());
            if (format) *format = (int)fmt;
            return true;
        }
    }
    return false;
}

// Is cp in a sorted code-point list (utSfntCodePoints' or utFntxGlyphs' output)?
inline bool utCodePointIn(const std::vector<char32_t>& v, char32_t cp) {
    return std::binary_search(v.begin(), v.end(), cp);
}

}  // namespace ut
