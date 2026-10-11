// ut_textmeasure.h - the engine's pen rule over a glyph cache, the UTF-8 widening the wide text
// call needs, the language font path, and the cross-check of a live font against the compiled
// table.
//
// Pure: no Windows, no engine, no mod state. ut_panel.cpp fills a UtFontCache from the font the
// engine really loaded (GraphicsFont2::GetStyle / GetMetric through ut_bindings.cpp) and measures
// with it; tools/test_text.cpp fills one from ut_fontmetrics.h and proves that the rule below
// equals utTextWidthPx (ut_layout.h) byte for byte. Both run THIS code.
//
// The pen rule is Engine.dll's line builder (rva 0xA7630, see ut_layout.h / font_metrics.py): per
// glyph the pen moves by xOffset + rightBearing + round(w * size / styleSize); the style is the
// baked size the engine picks for the requested one (GetStyle); kerning is left out, so the width
// is an upper bound; `slack` covers the clamp of a negative xOffset on the first glyph. A code
// point the font has no metric for is measured as '?', as the table does.
//
// UTF-8: the decoder is ut_textcfg.h's (utt::utf8Decode). A string that is not well-formed UTF-8
// is taken byte by byte as Latin-1 - what the mod did before it knew UTF-8 - so nothing that drew
// before draws differently now.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "ut_fontmetrics.h"
#include "ut_textcfg.h"

namespace ut {

const int kUtTmFirst = 32;          // the cached range: printable ASCII, the table's range
const int kUtTmChars = 95;
const int kUtTmMaxStyles = 32;      // savapromedium bakes 16
const int kUtTmMaxSize = 64;        // requested sizes cached 1..64; larger ones fall back

struct UtGlyph {
    int xOff;   // FNTMetric +0x02
    int rb;     // FNTMetric +0x04, the right bearing
    int w;      // FNTMetric +0x06, the bitmap width at the style's baked size
};

// One font's styles and its ASCII glyphs, filled ONCE (per world) from a probe; afterwards every
// measurement of an ASCII string is pure arithmetic over it, nothing is called per frame. A LAZY
// cache (a dynamic font, utTmProbePlan) adds each further size once, the first time it is asked.
struct UtFontCache {
    int valid;                                   // 1 = filled and '?' present in every style
    int nStyles;
    int styleSize[kUtTmMaxStyles];               // the baked size (Style + the decoded offset)
    int slack[kUtTmMaxStyles];                   // max(0, -min xOffset over the cached glyphs)
    const void* key[kUtTmMaxStyles];             // the probe's style key (the engine's Style*)
    unsigned char have[kUtTmMaxStyles][kUtTmChars];
    UtGlyph glyph[kUtTmMaxStyles][kUtTmChars];
    signed char sizeStyle[kUtTmMaxSize + 1];     // requested size -> style index, -1 = none
                                                 // (-2: asked on a lazy cache, none)
    int lazy;                                    // 1 = sizes are filled when first measured
};

// Where a cache is filled from. styleOf: the style the engine uses for a requested size, as an
// opaque key plus its baked size. glyphOf: one code point's metric in that style; false when the
// font has none.
struct UtFontProbe {
    bool (*styleOf)(int size, const void** key, int* styleSize, void* ctx);
    bool (*glyphOf)(const void* key, char32_t cp, UtGlyph* out, void* ctx);
    void* ctx;
};

inline int utTmStyleIndex(const UtFontCache& c, const void* key) {
    for (int i = 0; i < c.nStyles; ++i) {
        if (c.key[i] == key) return i;
    }
    return -1;
}

// One requested size into `c`: the style the probe names for it - a new style gets its ASCII
// glyphs (95 glyphOf calls) - and the size mapped to it. Returns the style index, -1 when the
// probe gives none, a key the cache cannot hold, or one key with two baked sizes.
inline int utTmAddSize(UtFontCache* c, const UtFontProbe& p, int size) {
    if (size < 1 || size > kUtTmMaxSize) return -1;
    const void* key = nullptr;
    int ss = 0;
    if (!p.styleOf(size, &key, &ss, p.ctx) || !key || ss < 1 || ss > 400) return -1;
    int idx = utTmStyleIndex(*c, key);
    if (idx < 0) {
        if (c->nStyles >= kUtTmMaxStyles) return -1;
        idx = c->nStyles++;
        c->key[idx] = key;
        c->styleSize[idx] = ss;
        int minX = 0;
        for (int i = 0; i < kUtTmChars; ++i) {
            UtGlyph g = {0, 0, 0};
            if (!p.glyphOf(key, (char32_t)(kUtTmFirst + i), &g, p.ctx)) continue;
            c->have[idx][i] = 1;
            c->glyph[idx][i] = g;
            if (g.xOff < minX) minX = g.xOff;
        }
        c->slack[idx] = -minX;
    } else if (c->styleSize[idx] != ss) {
        return -1;   // one key, two sizes: not a style the rule can use
    }
    c->sizeStyle[size] = (signed char)idx;
    return idx;
}

inline void utFontCacheReset(UtFontCache* c) {
    std::memset(c, 0, sizeof(*c));
    for (int s = 0; s <= kUtTmMaxSize; ++s) c->sizeStyle[s] = -1;
}

// Fills `c` for the requested sizes lo..hi. Returns c->valid.
inline int utFontCacheFill(UtFontCache* c, const UtFontProbe& p, int lo = 1,
                           int hi = kUtTmMaxSize) {
    utFontCacheReset(c);
    if (!p.styleOf || !p.glyphOf) return 0;
    if (lo < 1) lo = 1;
    if (hi > kUtTmMaxSize) hi = kUtTmMaxSize;
    for (int size = lo; size <= hi; ++size) utTmAddSize(c, p, size);
    int ok = c->nStyles > 0;
    for (int i = 0; i < c->nStyles; ++i) {
        if (!c->have[i]['?' - kUtTmFirst]) ok = 0;
    }
    c->valid = ok;
    return ok;
}

// ---- the live probe's plan: which sizes a role font is filled for ------------------------------
// A BAKED font (FNTX, kind 0) holds its 16 styles in the file: every requested size 1..64 is
// filled when the world starts, as always - nothing is created. A DYNAMIC font (FNTD, kind 1: a
// TrueType file the engine rasterises through GDI) builds the EXACT
// size it is asked for - one GDI font per size and one TextOutW per glyph, into an atlas that
// grows (the cost: 64 sizes x 95 glyphs = 6,080 rasterisations per role per world). So a dynamic
// font - or one whose kind is not known (-1) - is LAZY: the world's start fills only the role's
// nominal size (the caption size cap / the label size in force: the proof that the font measures),
// and any other size is filled the first time the pad or the label measures at it
// (utFontCacheWidthLazy) - a world creates no style that no draw asks for.
struct UtTmProbePlan {
    int lazy;      // 1 = only lo..hi now, the rest when first measured
    int lo, hi;    // the requested sizes filled when the world starts
};
inline UtTmProbePlan utTmProbePlan(int kind, int nominal) {
    UtTmProbePlan pl = {0, 1, kUtTmMaxSize};
    if (kind == 0) return pl;
    if (nominal < 1) nominal = 1;
    if (nominal > kUtTmMaxSize) nominal = kUtTmMaxSize;
    pl.lazy = 1;
    pl.lo = pl.hi = nominal;
    return pl;
}

// Fills `c` by the plan: utFontCacheFill for a baked font; for a lazy one the plan's sizes, a
// size whose style has no '?' left unmapped. Returns c->valid (a lazy cache: the plan's sizes all
// answer).
inline int utFontCacheFillPlan(UtFontCache* c, const UtFontProbe& p, const UtTmProbePlan& pl) {
    if (!pl.lazy) return utFontCacheFill(c, p, pl.lo, pl.hi);
    utFontCacheReset(c);
    c->lazy = 1;
    if (!p.styleOf || !p.glyphOf) return 0;
    int ok = 1;
    for (int size = pl.lo; size <= pl.hi; ++size) {
        const int idx = utTmAddSize(c, p, size);
        if (idx < 0 || !c->have[idx]['?' - kUtTmFirst]) {
            if (idx >= 0) c->sizeStyle[size] = -2;
            ok = 0;
        }
    }
    c->valid = ok && c->nStyles > 0;
    return c->valid;
}

// One size into a LAZY cache, the first time it is asked (later asks are free: a size that gave
// nothing is marked -2 and never asked again). A style without '?' is not used. Returns whether
// `size` now answers. For a live probe: GAME THREAD (the engine rasterises the style then).
inline bool utFontCacheFillSize(UtFontCache* c, const UtFontProbe& p, int size) {
    if (!c || !c->lazy || size < 1 || size > kUtTmMaxSize) return false;
    if (c->sizeStyle[size] >= 0) return true;
    if (c->sizeStyle[size] == -2 || !p.styleOf || !p.glyphOf) return false;
    const int idx = utTmAddSize(c, p, size);
    if (idx < 0 || !c->have[idx]['?' - kUtTmFirst]) {
        c->sizeStyle[size] = -2;
        return false;
    }
    return true;
}

// A glyph outside the cached range, asked of the live font (render thread). May be null.
typedef bool (*UtExtraGlyphFn)(const void* key, char32_t cp, UtGlyph* out, void* ctx);

// Calls fn(cp, ctx) for every code point of `s`: UTF-8 when the whole string is well-formed,
// else every byte as Latin-1. Returns false when `s` was not UTF-8.
template <typename Fn>
inline bool utTmForEachCodePoint(const char* s, Fn fn) {
    const unsigned char* p = (const unsigned char*)s;
    const std::size_t n = std::strlen(s);
    bool utf8 = true;
    for (std::size_t i = 0; i < n;) {
        const int len = utt::utf8Decode(p + i, n - i, nullptr);
        if (len <= 0) {
            utf8 = false;
            break;
        }
        i += (std::size_t)len;
    }
    for (std::size_t i = 0; i < n;) {
        char32_t cp = p[i];
        int len = 1;
        if (utf8) len = utt::utf8Decode(p + i, n - i, &cp);
        fn(cp);
        i += (std::size_t)len;
    }
    return utf8;
}

// Width in SCREEN px of `s` (UTF-8) at `size`, by the pen rule over the cache. -1 when the cache
// cannot answer for this size (the caller keeps the compiled table).
inline int utFontCacheWidth(const UtFontCache& c, const char* s, int size,
                            UtExtraGlyphFn extra = nullptr, void* ctx = nullptr) {
    if (!s || !*s || size <= 0) return 0;
    if (!c.valid || size > kUtTmMaxSize) return -1;
    const int st = c.sizeStyle[size];
    if (st < 0) return -1;
    const int ss = c.styleSize[st];
    const UtGlyph q = c.glyph[st]['?' - kUtTmFirst];
    int pen = 0, firstX = 0;
    bool first = true;
    utTmForEachCodePoint(s, [&](char32_t cp) {
        UtGlyph g = q;
        bool cached = true;
        if (cp >= (char32_t)kUtTmFirst && cp < (char32_t)(kUtTmFirst + kUtTmChars)) {
            if (c.have[st][cp - kUtTmFirst]) g = c.glyph[st][cp - kUtTmFirst];
        } else if (cp >= 0x80 && extra) {
            UtGlyph e = {0, 0, 0};
            if (extra(c.key[st], cp, &e, ctx)) {
                g = e;
                cached = false;
            }
        }
        // The style's slack already covers every cached glyph's xOffset; only a glyph from
        // outside the cache can start a line further left than that.
        if (first) {
            firstX = cached ? 0 : g.xOff;
            first = false;
        }
        pen += g.xOff + g.rb + (g.w * size + ss / 2) / ss;
    });
    int slack = c.slack[st];
    if (-firstX > slack) slack = -firstX;
    return pen + slack;
}

// utFontCacheWidth over a cache that may be LAZY (utTmProbePlan): a size not asked before is
// filled first through `p`. A baked cache measures exactly as utFontCacheWidth. For a live probe:
// GAME THREAD (a dynamic font rasterises the new style's glyphs then).
inline int utFontCacheWidthLazy(UtFontCache* c, const UtFontProbe& p, const char* s, int size,
                                UtExtraGlyphFn extra = nullptr, void* ctx = nullptr) {
    if (!c) return -1;
    if (c->lazy && s && *s && size >= 1 && size <= kUtTmMaxSize && c->sizeStyle[size] == -1)
        utFontCacheFillSize(c, p, size);
    return utFontCacheWidth(*c, s, size, extra, ctx);
}

// ---- the compiled table as a probe (savapromedium, ut_fontmetrics.h) ---------------------------

// Engine.dll 0xAE550 over the table: the baked size nearest the requested one, the first on a tie.
// The same answer as ut_layout.h's utFontStyleFor (test_text proves it).
inline int utTmTableStyle(int size) {
    int best = 0, bestD = 1 << 24;
    for (int i = 0; i < fontmetrics::kStyles; ++i) {
        int d = size - (int)fontmetrics::kStyleSize[i];
        if (d < 0) d = -d;
        if (d < bestD) {
            bestD = d;
            best = i;
        }
    }
    return best;
}

inline bool utTmTableStyleOf(int size, const void** key, int* styleSize, void*) {
    const int st = utTmTableStyle(size);
    *key = (const void*)(std::intptr_t)(st + 1);
    *styleSize = (int)fontmetrics::kStyleSize[st];
    return true;
}

// The table stores bearing = xOffset + rightBearing; the rule only ever adds the two, so the
// whole bearing is reported as xOffset and the slack is the table's own.
inline bool utTmTableGlyphOf(const void* key, char32_t cp, UtGlyph* out, void*) {
    const int st = (int)(std::intptr_t)key - 1;
    if (st < 0 || st >= fontmetrics::kStyles) return false;
    if (cp < (char32_t)fontmetrics::kFirstChar ||
        cp >= (char32_t)(fontmetrics::kFirstChar + fontmetrics::kChars))
        return false;
    const int i = (int)cp - fontmetrics::kFirstChar;
    out->xOff = (int)fontmetrics::kGlyphBearing[st][i];
    out->rb = 0;
    out->w = (int)fontmetrics::kGlyphW[st][i];
    return true;
}

inline int utFontCacheFromTable(UtFontCache* c) {
    const UtFontProbe p = {utTmTableStyleOf, utTmTableGlyphOf, nullptr};
    const int ok = utFontCacheFill(c, p);
    for (int i = 0; i < c->nStyles; ++i) {
        const int st = (int)(std::intptr_t)c->key[i] - 1;
        if (st >= 0 && st < fontmetrics::kStyles) c->slack[i] = (int)fontmetrics::kStyleSlack[st];
    }
    return ok;
}

// ---- the cross-check -----------------------------------------------------------------------------

struct UtTmCheck {
    int agree;       // 1 = every compared number is equal
    int sizes;       // requested sizes compared
    int sizeDiff;    // mapped to another baked size (or missing on one side)
    int styles;      // styles compared
    int styleDiff;   // a different slack
    int glyphs;      // glyphs compared (as xOffset + rightBearing, and w)
    int glyphDiff;
    int widths;      // string x size widths compared
    int widthDiff;
    int maxDelta;    // the largest |live - table| width
    char first[160]; // the first difference, for the log
};

// Compares `live` against `table` for the requested sizes lo..hi: the size -> baked size map, each
// style's slack, every cached glyph, and the width of every string at every size.
inline UtTmCheck utFontCacheCompare(const UtFontCache& live, const UtFontCache& table,
                                    const char* const* strs, int n, int lo, int hi) {
    UtTmCheck r;
    std::memset(&r, 0, sizeof(r));
    bool seen[kUtTmMaxStyles] = {};
    auto note = [&](const char* fmt, auto a, auto b, auto d) {
        if (!r.first[0]) std::snprintf(r.first, sizeof(r.first), fmt, a, b, d);
    };
    if (lo < 1) lo = 1;
    if (hi > kUtTmMaxSize) hi = kUtTmMaxSize;
    for (int size = lo; size <= hi; ++size) {
        ++r.sizes;
        const int a = live.valid ? live.sizeStyle[size] : -1;
        const int b = table.valid ? table.sizeStyle[size] : -1;
        if (a < 0 || b < 0 || live.styleSize[a] != table.styleSize[b]) {
            ++r.sizeDiff;
            note("size %d: baked size %d live, %d in the table", size,
                 a < 0 ? -1 : live.styleSize[a], b < 0 ? -1 : table.styleSize[b]);
            continue;
        }
        if (seen[a]) continue;
        seen[a] = true;
        ++r.styles;
        if (live.slack[a] != table.slack[b]) {
            ++r.styleDiff;
            note("style %d: slack %d live, %d in the table", live.styleSize[a], live.slack[a],
                 table.slack[b]);
        }
        for (int i = 0; i < kUtTmChars; ++i) {
            ++r.glyphs;
            const UtGlyph& x = live.glyph[a][i];
            const UtGlyph& y = table.glyph[b][i];
            if (live.have[a][i] != table.have[b][i] || x.xOff + x.rb != y.xOff + y.rb ||
                x.w != y.w) {
                ++r.glyphDiff;
                note("style %d: glyph '%c' differs (bearing+w %d live)", live.styleSize[a],
                     (char)(kUtTmFirst + i), x.xOff + x.rb + x.w);
            }
        }
    }
    for (int k = 0; k < n; ++k) {
        if (!strs[k]) continue;
        for (int size = lo; size <= hi; ++size) {
            ++r.widths;
            const int a = utFontCacheWidth(live, strs[k], size);
            const int b = utFontCacheWidth(table, strs[k], size);
            const int d = a > b ? a - b : b - a;
            if (d > r.maxDelta) r.maxDelta = d;
            if (a != b) {
                ++r.widthDiff;
                note("\"%.60s\" at size %d: %d px live", strs[k], size, a);
            }
        }
    }
    r.agree = r.sizeDiff == 0 && r.styleDiff == 0 && r.glyphDiff == 0 && r.widthDiff == 0 &&
              r.sizes > 0 && r.styles > 0;
    return r;
}

// Whether the compiled table bounds `copy` (a language copy of the font): every caption at every
// size lo..hi measures at most the table's width. The mod does not gate on it - the pad's caption
// fit measures the font drawn (ut_panel.cpp widestCaptionLivePx); test_text reports it per copy.
struct UtTmBound {
    int ok;
    const char* caption;   // the first caption that is wider (or unmeasurable), else null
    int size, live, table;
};
inline UtTmBound utTmBoundedByTable(const UtFontCache& copy, const UtFontCache& table,
                                    const char* const* caps, int n, int lo, int hi) {
    UtTmBound r = {1, nullptr, 0, 0, 0};
    for (int i = 0; i < n; ++i) {
        for (int size = lo; size <= hi; ++size) {
            const int a = utFontCacheWidth(copy, caps[i], size);
            const int b = utFontCacheWidth(table, caps[i], size);
            if (a >= 0 && a <= b) continue;
            r.ok = 0;
            r.caption = caps[i];
            r.size = size;
            r.live = a;
            r.table = b;
            return r;
        }
    }
    return r;
}

// The label lines the cross-check measures besides the captions: every built-in group name in
// 1.1.0's full and short forms with the widest counts, the mode prefix, and every printable ASCII
// character in one line. Written into buf[k][128]; returns how many.
inline int utTmCheckLabels(char (*buf)[128], int cap) {
    std::size_t nb = 0;
    const utt::UtTextBuiltin* b = utt::utTextBuiltins(&nb);
    int k = 0;
    for (std::size_t i = 0; i < nb && k + 2 <= cap; ++i) {
        std::snprintf(buf[k++], 128, "%s%s - %d/%d owned - row %d-%d/%d - coll %d/%d",
                      "mode unknown - ", b[i].name, 288, 288, 31, 35, 35, 3288, 3288);
        std::snprintf(buf[k++], 128, "%s - %d/%d - row %d-%d/%d - coll %d/%d", b[i].name, 188,
                      888, 108, 118, 118, 8888, 8888);
    }
    if (k < cap) {
        char* p = buf[k++];
        int j = 0;
        for (int c = kUtTmFirst; c < kUtTmFirst + kUtTmChars && j < 127; ++c) p[j++] = (char)c;
        p[j] = 0;
    }
    return k;
}

// ---- UTF-8 for the wide call -------------------------------------------------------------------

// `s` as UTF-16 into out[cap] (cap >= 1; always terminated), cut at the last whole code point that
// fits. Well-formed UTF-8 is decoded (a code point above U+FFFF becomes a surrogate pair); any other
// string is widened byte by byte (Latin-1), as before. Returns the units written.
inline std::size_t utWidenUtf8(const char* s, unsigned short* out, std::size_t cap,
                               bool* wasUtf8 = nullptr) {
    if (wasUtf8) *wasUtf8 = true;
    if (!out || cap == 0) return 0;
    out[0] = 0;
    if (!s) return 0;
    std::size_t n = 0;
    bool full = false;
    const bool utf8 = utTmForEachCodePoint(s, [&](char32_t cp) {
        if (full) return;
        if (cp >= 0x10000) {
            if (n + 2 > cap - 1) {
                full = true;
                return;
            }
            const char32_t v = cp - 0x10000;
            out[n++] = (unsigned short)(0xD800 + (v >> 10));
            out[n++] = (unsigned short)(0xDC00 + (v & 0x3FF));
        } else {
            if (n + 1 > cap - 1) {
                full = true;
                return;
            }
            out[n++] = (unsigned short)cp;
        }
    });
    out[n] = 0;
    if (wasUtf8) *wasUtf8 = utf8;
    return n;
}

// Cuts `s` in place, one whole code point at a time from the end (a UTF-8 sequence is never
// split; a byte that is not UTF-8 counts as one), until widthFn says it fits `limitPx`. Nothing is
// allocated: the pad clips a caption per frame. Returns true when nothing had to go.
typedef int (*UtTmWidthFn)(const char* s, const void* ctx);
inline bool utTmClipToWidth(char* s, int limitPx, UtTmWidthFn widthFn, const void* ctx) {
    if (!s || !*s || !widthFn) return true;
    if (widthFn(s, ctx) <= limitPx) return true;
    std::size_t n = std::strlen(s);
    while (n > 0) {
        std::size_t k = n - 1;
        while (k > 0 && (((unsigned char)s[k]) & 0xC0) == 0x80 && n - k < 4) --k;
        const unsigned char lead = (unsigned char)s[k];
        const std::size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        if (need != n - k) k = n - 1;   // not one well-formed sequence: one byte
        n = k;
        s[n] = 0;
        if (n == 0 || widthFn(s, ctx) <= limitPx) break;
    }
    return false;
}

// True when every byte of `s` is printable ASCII (the only text the char* call draws right).
inline bool utTmIsPlainAscii(const char* s) {
    for (const unsigned char* p = (const unsigned char*)s; p && *p; ++p) {
        if (*p < 0x20 || *p > 0x7E) return false;
    }
    return true;
}

// The font the pad and the label draw in when no [style] font key is in force: the ROOT
// savapromedium, in every language of the game (the tab is English by default; the game's
// language never switches its font). Only the engine's own rollover lines use the language copy.
const char* const kUtRootFontPath = "fonts/savapromedium.fnt";

// StyleManager::GetLocalizationFontFile's rule (Engine.dll 0x1C3CF0): a non-empty special font
// folder is inserted after the last '/' of the path ("fonts/savapromedium.fnt" + "ru/" ->
// "fonts/ru/savapromedium.fnt"). For the log line only - the engine applies it itself.
inline bool utLocalizedFontPath(const char* path, const char* folder, char* out, std::size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = 0;
    if (!path) return false;
    const std::size_t pl = std::strlen(path), fl = folder ? std::strlen(folder) : 0;
    if (pl + fl + 1 > cap) return false;
    const char* slash = std::strrchr(path, '/');
    const std::size_t head = slash ? (std::size_t)(slash - path) + 1 : 0;
    std::memcpy(out, path, head);
    if (fl) std::memcpy(out + head, folder, fl);
    std::memcpy(out + head + fl, path + head, pl - head);
    out[pl + fl] = 0;
    return true;
}

// ---- a role's own font: [style] font.caption / font.label of the strings file ----------------
// The path is loaded EXACTLY (GraphicsEngine::LoadFont): LoadFontDirect would insert the game's
// language folder after the last '/' again (fonts/ru/ru/savapromedium.fnt on a Russian game).

// The Fonts.arc entry a font path names ("fonts/ru/savapromedium.fnt" -> "ru/savapromedium.fnt");
// false when the path is not under fonts/.
inline bool utFontArcEntry(const char* path, char* out, std::size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = 0;
    if (!path || std::strncmp(path, "fonts/", 6) != 0 || !path[6]) return false;
    const std::size_t n = std::strlen(path + 6);
    if (n + 1 > cap) return false;
    std::memcpy(out, path + 6, n + 1);
    return true;
}

// The code points one style of an FNTX v2 font file has a glyph for - the style nearest `size`
// (ties to the first, the engine's GetStyle rule), sorted. The file: 8 x u32 header (magic 'FNTX',
// version 2, the style count at +0x18), then per style 7 x u32 (size, flags, ascent, line, glyphs,
// kerns, 0), its 28-byte glyph records (u16 code first) and 8-byte kerning pairs. False for any
// other file (an FNTD font, say) or a truncated one. For the derivation of a role's font from the
// letters (utb::utbDeriveFolder): the worker reads the game's copies out of Fonts.arc.
inline bool utFntxGlyphs(const unsigned char* d, std::size_t n, int size, std::vector<char32_t>* out) {
    if (!d || !out || n < 32) return false;
    auto u32 = [&](std::size_t o) {
        unsigned v = 0;
        std::memcpy(&v, d + o, 4);
        return v;
    };
    if (u32(0) != 0x58544E46u || u32(4) != 2) return false;
    const unsigned ns = u32(0x18);
    std::size_t off = 0x20, best = 0;
    int bestD = 1 << 30;
    bool any = false;
    for (unsigned s = 0; s < ns; ++s) {
        if (off + 28 > n) return false;
        const int ssz = (int)u32(off);
        const std::size_t ng = u32(off + 16), nk = u32(off + 20);
        if (ng > n || nk > n || off + 28 + ng * 28 + nk * 8 > n) return false;
        const int dd = ssz > size ? ssz - size : size - ssz;
        if (dd < bestD) {
            bestD = dd;
            best = off;
            any = true;
        }
        off += 28 + ng * 28 + nk * 8;
    }
    if (!any) return false;
    const std::size_t ng = u32(best + 16);
    out->clear();
    for (std::size_t i = 0; i < ng; ++i) {
        unsigned short c = 0;
        std::memcpy(&c, d + best + 28 + i * 28, 2);
        out->push_back((char32_t)c);
    }
    for (std::size_t i = 1; i < out->size(); ++i) {   // sorted (no <algorithm>: the header stays <c...>)
        const char32_t v = (*out)[i];
        std::size_t j = i;
        while (j > 0 && (*out)[j - 1] > v) {
            (*out)[j] = (*out)[j - 1];
            --j;
        }
        (*out)[j] = v;
    }
    return true;
}

enum UtRoleFont {
    kRoleFontEngine = 0,    // no key: the root font kUtRootFontPath, in every game language
    kRoleFontOwn,           // the key's font draws and measures the role
    kRoleFontMissing,       // not in the game's Fonts.arc
    kRoleFontNotLoaded,     // the engine returned no font for the path
    kRoleFontNotProved,     // the live metrics were not proved this world (cannot be measured)
    kRoleFontNoStyles       // the font gave no styles or no '?' at the size it was proved at
};

// Whether a role draws in the font its key names. inArc: 1 found in the game's Fonts.arc, 0 not
// found, -1 not known (the archive could not be read; the load decides).
inline UtRoleFont utRoleFontState(bool keySet, int inArc, bool proved, bool loaded, bool cacheValid) {
    if (!keySet) return kRoleFontEngine;
    if (inArc == 0) return kRoleFontMissing;
    if (!proved) return kRoleFontNotProved;
    if (!loaded) return kRoleFontNotLoaded;
    if (!cacheValid) return kRoleFontNoStyles;
    return kRoleFontOwn;
}

// The path a role draws in: the key's font while it is in force, else the root font - never
// a language copy picked by the game's language.
inline const char* utRoleFontPath(UtRoleFont s, const char* key) {
    return s == kRoleFontOwn && key && *key ? key : kUtRootFontPath;
}

inline const char* utRoleFontWhy(UtRoleFont s) {
    switch (s) {
    case kRoleFontMissing: return "it is not in the game's resources\\Fonts.arc";
    case kRoleFontNotLoaded: return "the game did not load it";
    case kRoleFontNotProved: return "the live font metrics are off for this world, so it cannot be measured";
    case kRoleFontNoStyles: return "it gave no styles or no '?' to measure with (or the engine refused or faulted on it)";
    default: return "";
    }
}

}  // namespace ut
