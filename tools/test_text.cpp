// test_text.cpp - the text engine's pure half, offline (src\ut_textmeasure.h).
//
//   1  the table as a probe: utTmTableStyle = ut_layout.h's utFontStyleFor; the cache filled from
//      ut_fontmetrics.h measures EXACTLY what utTextWidthPx measures - the 26 captions and the
//      label lines at every size 1..64, the pad caption and the label at the seven UI scales
//      test_layout uses, and 20000 random strings of bytes 1..127. This is the "nothing moved"
//      proof: with the live route on, an English game measures the same pixels as before.
//   2  the cross-check finds every kind of difference (a size mapped elsewhere, a slack, one glyph,
//      a width) and names the first.
//   3  the font FILE as a probe (optional, read only: Fonts.arc from UNIQUETAB_TEST_FONTS_ARC,
//      else UNIQUETAB_TEST_GAME_DIR\resources, else UNIQUETAB_TEST_DBMODS_DIR\..\resources): the
//      root savapromedium.fnt read with its xOffset and right bearing apart, styles picked the engine's way, agrees with the table - the cross-check
//      the mod runs per world, run on the real data; ru/savapromedium.fnt measures Cyrillic; the
//      language policy over the real fonts: no file on a Russian game = the built-in English in
//      the root font; a file with [style] language = ru + [text.ru] + font.caption/font.label =
//      ru/ draws its Cyrillic captions on an English and a Russian game alike (and they fit the
//      34 x 14 cell at full size); without language = ru the section is ignored; on the root font
//      the Cyrillic candidates are passed over, named.
//   4  UTF-8 for the wide call: ASCII is the identity, Cyrillic/CJK/astral decode, every ill-formed
//      class falls back to byte-wise Latin-1, the cut never splits a character.
//   5  the language font path (GetLocalizationFontFile's rule), the pad's per-caption clip (whole
//      code points, nothing allocated), the label lines, the header purity; a role's own font
//      ([style] font.caption / font.label): the Fonts.arc entry a key names and when it is used.
//   3b with Fonts.arc: every folder the template names holds savapromedium.fnt; the fonts per role
//      over the real ru/ and root copies - an English game with Cyrillic under [text].
//   6  the sfnt cmap reader (src\ut_sfnt.h): formats 4 and 12, the TTC face 0, the FNTD
//      header, every truncation refused with a guard page behind the buffer, 4000 corrupted files;
//      with Fonts.arc the FNTD copies' counts (zh 34,600 / ja 16,732 / ko 17,666 = fontTools'),
//      th and vi = zh, U+8235 in all three, Thai in none; the derivation over the real tables
//      with the FNTD copies: Han zh, kana ja, Hangul ko, Thai zh (Windows draws it), the identity.
//   7  the live probe's plan: a baked font every size, a dynamic one only the sizes in use
//      (the nominal one, then each size the caption fit or the label asks - never the 64).
//
// Writes nothing. Exit code 0 = all pass.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "../src/gen/arc_reader.h"
#include "../src/ut_layout.h"
#include "../src/ut_textmeasure.h"
#include "../src/ut_buttons.h"
#include "../src/ut_sfnt.h"

// After every project header: the guard page of the sfnt reader's bounds test only.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace {

int g_ran = 0, g_fail = 0;

void ok(bool cond, const char* what, const char* detail = "") {
    ++g_ran;
    if (cond) return;
    ++g_fail;
    printf("[text] FAIL: %s %s\n", what, detail);
}

unsigned g_seed = 12345u;
unsigned rnd() {
    g_seed = g_seed * 1103515245u + 12345u;
    return (g_seed >> 16) & 0x7FFF;
}

// The 26 pad captions in pad order (ut_textcfg.h's built-ins; test_textcfg ties them to kButtons
// through ut_groups.h).
std::vector<std::string> captions() {
    size_t n = 0;
    const utt::UtTextBuiltin* b = utt::utTextBuiltins(&n);
    std::vector<std::string> out;
    for (size_t i = 0; i < n && i < (size_t)utt::kTextPadButtons; ++i) out.push_back(b[i].caption);
    return out;
}

std::vector<std::string> labelLines() {
    static char buf[64][128];
    const int n = ut::utTmCheckLabels(buf, 64);
    std::vector<std::string> out;
    for (int i = 0; i < n; ++i) out.push_back(buf[i]);
    return out;
}

// ---- 1 -------------------------------------------------------------------------------------------
void tableHalf() {
    int bad = 0;
    for (int size = -2; size <= 90; ++size) {
        if (ut::utTmTableStyle(size) != ut::utFontStyleFor(size)) ++bad;
    }
    ok(bad == 0, "utTmTableStyle equals utFontStyleFor for sizes -2..90");

    static ut::UtFontCache table;
    ok(ut::utFontCacheFromTable(&table) == 1, "the table fills a valid cache");
    ok(table.nStyles == ut::fontmetrics::kStyles, "the cache holds the 16 baked styles");
    bad = 0;
    for (int s = 1; s <= ut::kUtTmMaxSize; ++s) {
        if (table.sizeStyle[s] < 0 ||
            table.styleSize[table.sizeStyle[s]] !=
                ut::fontmetrics::kStyleSize[ut::utFontStyleFor(s)])
            ++bad;
    }
    ok(bad == 0, "every requested size 1..64 maps to the table's nearest baked size");

    // The captions and the label lines at every cached size.
    std::vector<std::string> strs = captions();
    const std::vector<std::string> lab = labelLines();
    strs.insert(strs.end(), lab.begin(), lab.end());
    char detail[200] = "";
    bad = 0;
    int cmp = 0;
    for (const std::string& s : strs) {
        for (int size = 1; size <= ut::kUtTmMaxSize; ++size) {
            ++cmp;
            const int a = ut::utFontCacheWidth(table, s.c_str(), size);
            const int b = ut::utTextWidthPx(s.c_str(), size);
            if (a != b && !bad++) {
                _snprintf_s(detail, sizeof(detail), _TRUNCATE, "\"%s\" at %d: %d vs %d", s.c_str(),
                            size, a, b);
            }
        }
    }
    printf("[text] %d captions + %d label lines x sizes 1..64 = %d widths compared\n",
           (int)captions().size(), (int)lab.size(), cmp);
    ok(bad == 0 && cmp == (int)strs.size() * ut::kUtTmMaxSize,
       "the pen rule over the cache = utTextWidthPx for the captions and label lines", detail);
    ok(captions().size() == 26, "26 captions");

    // The seven UI scales of test_layout: the derived pad's 34 x 14 cell scaled with the draw's
    // rounding, the caption size the fitter picks (widest of the 26) and every label size 8..32.
    const float scales[] = {0.7f, 0.8f, 0.928f, 1.0f, 1.25f, 1.5f, 2.0f};
    std::vector<const char*> capPtrs;
    const std::vector<std::string> caps = captions();
    for (const std::string& c : caps) capPtrs.push_back(c.c_str());
    const ut::UtCaptions uc = {capPtrs.data(), (int)capPtrs.size()};
    struct Ctx {
        const ut::UtFontCache* c;
        const ut::UtCaptions* caps;
    } ctx = {&table, &uc};
    auto widestCache = [](int size, const void* p) -> int {
        const Ctx* x = (const Ctx*)p;
        int w = 0;
        for (int i = 0; i < x->caps->n; ++i) {
            const int t = ut::utFontCacheWidth(*x->c, x->caps->tags[i], size);
            if (t > w) w = t;
        }
        return w;
    };
    bad = 0;
    int scaleCases = 0;
    for (float s : scales) {
        const int w = (int)(34.0f * s + 0.5f), h = (int)(14.0f * s + 0.5f);
        const int a = ut::utCaptionSize(w, h, ut::kUtCaptionMaxDefault, widestCache, &ctx);
        const int b = ut::utCaptionSize(w, h, ut::kUtCaptionMaxDefault, ut::utWidestCaptionPx, &uc);
        ++scaleCases;
        if (a != b || (a > 0 && widestCache(a, &ctx) != ut::utWidestCaptionPx(a, &uc))) ++bad;
        for (int ls = 8; ls <= 32; ++ls) {
            const int fs = (int)((float)ls * s + 0.5f);
            for (const std::string& l : lab) {
                ++scaleCases;
                if (ut::utFontCacheWidth(table, l.c_str(), fs) != ut::utTextWidthPx(l.c_str(), fs))
                    ++bad;
            }
        }
    }
    printf("[text] seven UI scales: %d caption/label cases\n", scaleCases);
    ok(bad == 0, "the caption size and every label width agree at the seven UI scales");

    // Random strings of bytes 1..127: control characters and DEL are '?' on both sides.
    bad = 0;
    for (int k = 0; k < 20000; ++k) {
        char s[48];
        const int n = 1 + (int)(rnd() % 46);
        for (int i = 0; i < n; ++i) s[i] = (char)(1 + rnd() % 127);
        s[n] = 0;
        const int size = 1 + (int)(rnd() % ut::kUtTmMaxSize);
        if (ut::utFontCacheWidth(table, s, size) != ut::utTextWidthPx(s, size)) ++bad;
    }
    ok(bad == 0, "20000 random byte strings 1..127 measure the same on both routes");

    ok(ut::utFontCacheWidth(table, "", 10) == 0 && ut::utFontCacheWidth(table, nullptr, 10) == 0,
       "an empty string is 0 px");
    ok(ut::utFontCacheWidth(table, "HEL", 65) == -1, "a size above 64 is not the cache's answer");
    ut::UtFontCache empty = {};
    ok(ut::utFontCacheWidth(empty, "HEL", 10) == -1, "an empty cache gives no answer");

    // A glyph from outside the cache comes from the extra function, and a first glyph further
    // left than the style's slack widens the line by the difference.
    auto extra = [](const void*, char32_t cp, ut::UtGlyph* g, void*) -> bool {
        if (cp != 0x0428) return false;
        g->xOff = -9;
        g->rb = 1;
        g->w = 12;
        return true;
    };
    const int st = table.sizeStyle[12];
    const int q = ut::utFontCacheWidth(table, "?", 12);
    const int sh = ut::utFontCacheWidth(table, "\xD0\xA8", 12, extra, nullptr);   // U+0428
    const int expect = -9 + 1 + 12 + (9 > table.slack[st] ? 9 : table.slack[st]);
    ok(sh == expect, "a non-ASCII glyph is measured from the live font, with its own clamp");
    ok(ut::utFontCacheWidth(table, "\xD0\xA8", 12) == q,
       "without the live font a non-ASCII character measures as '?'");
    ok(ut::utFontCacheWidth(table, "\xD0", 12) == ut::utTextWidthPx("\xD0", 12),
       "a string that is not UTF-8 measures byte by byte, as before");
}

// ---- 2 -------------------------------------------------------------------------------------------
void compareHalf() {
    static ut::UtFontCache a, b;
    ut::utFontCacheFromTable(&a);
    std::vector<std::string> strs = captions();
    std::vector<const char*> p;
    for (const std::string& s : strs) p.push_back(s.c_str());
    p.push_back("mode unknown - Shoulders - 288/288 owned - row 31-35/35 - coll 3288/3288");
    ut::UtTmCheck r = ut::utFontCacheCompare(a, a, p.data(), (int)p.size(), 5, 36);
    printf("[text] self-check: %d sizes, %d styles, %d glyphs, %d widths\n", r.sizes, r.styles,
           r.glyphs, r.widths);
    ok(r.agree == 1 && r.sizes == 32 && r.styles == 16 && r.glyphs == 16 * 95 && r.maxDelta == 0,
       "the table agrees with itself over sizes 5..36 (all 16 styles)");

    b = a;
    b.glyph[b.sizeStyle[10]]['H' - 32].w += 1;
    r = ut::utFontCacheCompare(b, a, p.data(), (int)p.size(), 5, 36);
    ok(!r.agree && r.glyphDiff == 1 && r.widthDiff > 0 && strstr(r.first, "'H'") != nullptr,
       "one wider glyph is found and named", r.first);

    b = a;
    b.slack[b.sizeStyle[18]] += 1;
    r = ut::utFontCacheCompare(b, a, p.data(), (int)p.size(), 5, 36);
    ok(!r.agree && r.styleDiff == 1 && r.maxDelta == 1, "a different slack is found", r.first);

    b = a;
    b.sizeStyle[11] = b.sizeStyle[12];
    r = ut::utFontCacheCompare(b, a, p.data(), (int)p.size(), 5, 36);
    ok(!r.agree && r.sizeDiff == 1 && strstr(r.first, "size 11") != nullptr,
       "a size the engine maps to another baked style is found", r.first);

    b = a;
    b.have[b.sizeStyle[14]]['Q' - 32] = 0;
    r = ut::utFontCacheCompare(b, a, p.data(), (int)p.size(), 5, 36);
    ok(!r.agree && r.glyphDiff == 1, "a glyph the live font lacks is found", r.first);

    ut::UtFontCache none = {};
    for (int s = 0; s <= ut::kUtTmMaxSize; ++s) none.sizeStyle[s] = -1;
    r = ut::utFontCacheCompare(none, a, p.data(), (int)p.size(), 5, 36);
    ok(!r.agree && r.sizeDiff == 32, "a font that gave no styles never agrees");
}

// ---- 3 -------------------------------------------------------------------------------------------
// FNTX v2 (tools\font_metrics.py parse): header 8 x u32 (magic 'FNTX', 2, ..., styles at +0x18);
// per style 7 x u32 (size, flags, ascent, line, glyphs, kerns, 0), then 28-byte glyph records
// (u16 code, s16 xOffset, s16 rightBearing, u16 w, ...), then 8-byte kerning pairs. The glyph
// record is what GetMetric returns a pointer to: +2 xOffset, +4 right bearing, +6 w.
struct FntStyle {
    int size = 0;
    std::vector<unsigned> code;
    std::vector<ut::UtGlyph> g;
};
struct Fnt {
    std::vector<FntStyle> styles;
    bool parse(const std::vector<unsigned char>& d) {
        if (d.size() < 32) return false;
        auto u32 = [&](size_t o) { return *(const unsigned*)&d[o]; };
        if (u32(0) != 0x58544E46u || u32(4) != 2) return false;
        const unsigned ns = u32(0x18);
        size_t off = 0x20;
        for (unsigned s = 0; s < ns; ++s) {
            if (off + 28 > d.size()) return false;
            FntStyle st;
            st.size = (int)u32(off);
            const unsigned ng = u32(off + 16), nk = u32(off + 20);
            off += 28;
            if (off + (size_t)ng * 28 + (size_t)nk * 8 > d.size()) return false;
            for (unsigned i = 0; i < ng; ++i) {
                const unsigned char* r = &d[off + (size_t)i * 28];
                st.code.push_back(*(const unsigned short*)r);
                ut::UtGlyph g;
                g.xOff = *(const short*)(r + 2);
                g.rb = *(const short*)(r + 4);
                g.w = *(const unsigned short*)(r + 6);
                st.g.push_back(g);
            }
            off += (size_t)ng * 28 + (size_t)nk * 8;
            styles.push_back(st);
        }
        return !styles.empty();
    }
};

// The engine's way: the baked size nearest the requested one, ties to the first (smaller).
bool fntStyleOf(int size, const void** key, int* ss, void* ctx) {
    const Fnt* f = (const Fnt*)ctx;
    int best = -1, bestD = 1 << 24;
    for (size_t i = 0; i < f->styles.size(); ++i) {
        int d = size - f->styles[i].size;
        if (d < 0) d = -d;
        if (d < bestD) {
            bestD = d;
            best = (int)i;
        }
    }
    if (best < 0) return false;
    *key = &f->styles[(size_t)best];
    *ss = f->styles[(size_t)best].size;
    return true;
}
bool fntGlyphOf(const void* key, char32_t cp, ut::UtGlyph* out, void*) {
    const FntStyle* st = (const FntStyle*)key;
    for (size_t i = 0; i < st->code.size(); ++i) {
        if (st->code[i] == (unsigned)cp) {
            *out = st->g[i];
            return true;
        }
    }
    return false;
}

// 3b - the [style] font keys over the real fonts. ut_panel.cpp's glyph callback asks each role's
// own font (font.caption for the pad, font.label for the label; the tooltip keeps the engine's),
// and each role measures with a cache filled from its own font.
void fontRolesHalf(const gen::ArcArchive& arc, const ut::UtFontCache& root, const ut::UtFontCache& ru,
                   const std::vector<utt::UtTextGroupIn>& groups) {
    // the folders the template names are the ones the game ships (a baked FNTX copy each)
    std::string missing;
    const std::string folders = utt::utTextFontFolders();
    int named = 0;
    for (size_t at = 0; at < folders.size();) {
        size_t sp = folders.find(' ', at);
        if (sp == std::string::npos) sp = folders.size();
        const std::string dir = folders.substr(at, sp - at);
        at = sp + 1;
        ++named;
        char entry[96];
        const std::string key = "fonts/" + dir + "/savapromedium.fnt";
        const gen::ArcEntry* en =
            ut::utFontArcEntry(key.c_str(), entry, sizeof(entry)) ? arc.entry(entry) : nullptr;
        std::vector<std::uint8_t> d;
        static Fnt f;
        f = Fnt();
        if (!en || !arc.read(*en, d) || !f.parse(d)) missing += " " + dir;
    }
    ok(named == 14 && missing.empty(), "every folder the template names holds a baked savapromedium.fnt",
       missing.c_str());
    int fntd = 0;
    for (const char* dir : {"ja", "ko", "th", "vi", "zh"}) {
        const gen::ArcEntry* en = arc.entry(std::string(dir) + "/savapromedium.fnt");
        std::vector<std::uint8_t> d;
        static Fnt f;
        f = Fnt();
        if (en && arc.read(*en, d) && !f.parse(d)) ++fntd;
    }
    ok(fntd == 5, "ja ko th vi zh are there but not baked (the template says they may not measure)");

    // the per-role glyph check: caption / label / tooltip each over its own real font
    struct RoleFonts {
        const void* key[3];
        static bool glyph(utt::UtTextFont role, char32_t cp, const void* ctx) {
            if (cp >= 0x20 && cp <= 0x7E) return true;
            ut::UtGlyph g;
            return fntGlyphOf(((const RoleFonts*)ctx)->key[role], cp, &g, nullptr);
        }
    };
    const void* ru13 = ru.key[ru.sizeStyle[13]];
    const void* root13 = root.key[root.sizeStyle[13]];
    const std::string shlm = "\xD0\xA8\xD0\x9B\xD0\x9C", helms = "\xD0\xA8\xD0\xBB\xD0\xB5\xD0\xBC\xD1\x8B";
    utt::UtTextFile tf;
    std::string perr;
    ok(utt::utTextParse("[text]\ncaption.head = " + shlm + "\nname.head = " + helms + "\n", tf, &perr),
       "a player's file parses ([text] caption.head and name.head in Cyrillic)", perr.c_str());
    utt::UtTextContext c;
    c.file = &tf;
    c.language = "english";
    c.english = true;
    c.glyph = RoleFonts::glyph;
    const RoleFonts noKeys = {{root13, root13, root13}};   // an English game: the root font everywhere
    c.glyphCtx = &noKeys;
    utt::UtTextPad pad = utt::utTextResolvePad(c, groups);
    std::string adv = utt::utTextFontAdvice(pad.skipped);
    printf("[text] that player's case, no font key: %s; advice: %s\n", pad.skipped.empty() ? "-" : pad.skipped[0].c_str(),
           adv.c_str());
    ok(pad.caps[1] == "HEL" && pad.names[0] == "Helms" && pad.skipped.size() == 2 &&
           adv.find("font.caption = fonts/ru/savapromedium.fnt") != std::string::npos &&
           adv.find("font.label = fonts/ru/savapromedium.fnt") != std::string::npos,
       "no key: HEL and Helms, and the WARN says to add font.caption and font.label = fonts/ru/...", adv.c_str());
    const RoleFonts capKey = {{ru13, root13, root13}};   // font.caption = fonts/ru/savapromedium.fnt
    c.glyphCtx = &capKey;
    pad = utt::utTextResolvePad(c, groups);
    ok(pad.caps[1] == shlm && pad.names[0] == "Helms" && pad.skipped.size() == 1 &&
           utt::utTextFontAdvice(pad.skipped).find("font.label = fonts/ru/") != std::string::npos,
       "font.caption = ru/: the caption is drawn, the name waits for font.label");
    const RoleFonts bothKeys = {{ru13, ru13, root13}};
    c.glyphCtx = &bothKeys;
    pad = utt::utTextResolvePad(c, groups);
    ok(pad.caps[1] == shlm && pad.names[0] == helms && pad.skipped.empty(),
       "font.caption and font.label = ru/: both drawn on the English game");

    // measured per role: the caption with ru/'s cache fits today's 34 x 14 cell at full size; the
    // label line with the label role's cache measures the Cyrillic name with its own glyphs
    struct RoleMeasure {
        const ut::UtFontCache* c;
        const std::vector<std::string>* caps;
        static bool extra(const void* key, char32_t cp, ut::UtGlyph* g, void*) { return fntGlyphOf(key, cp, g, nullptr); }
        static int widest(int size, const void* p) {
            const RoleMeasure* m = (const RoleMeasure*)p;
            int w = 0;
            for (const std::string& s : *m->caps) {
                const int t = ut::utFontCacheWidth(*m->c, s.c_str(), size, extra, nullptr);
                if (t > w) w = t;
            }
            return w;
        }
        static int line(const std::string& s, const void* p) {
            return ut::utFontCacheWidth(*((const RoleMeasure*)p)->c, s.c_str(), 13, extra, nullptr);
        }
    };
    const RoleMeasure capM = {&ru, &pad.caps};
    const int capSize = ut::utCaptionSize(34, 14, ut::kUtCaptionMaxDefault, RoleMeasure::widest, &capM);
    const int shlmPx = ut::utFontCacheWidth(ru, shlm.c_str(), capSize > 0 ? capSize : 10, RoleMeasure::extra, nullptr);
    printf("[text] font.caption = ru/: the pad's caption size %d in the 34 x 14 cell, the Cyrillic caption = %d px\n",
           capSize, shlmPx);
    ok(capSize >= 9 && shlmPx > 0 && shlmPx <= 33, "the Cyrillic caption keeps a full-size caption in today's cell");
    const RoleMeasure labRu = {&ru, nullptr}, labRoot = {&root, nullptr};
    const std::string labelLine = helms + " - 3/288 owned - row 1-6/36 - coll 40/1520";
    const int ruPx = RoleMeasure::line(labelLine, &labRu), rootPx = RoleMeasure::line(labelLine, &labRoot);
    printf("[text] font.label = ru/: the Cyrillic label line at 13 = %d px (on the root font, as '?': %d px)\n",
           ruPx, rootPx);
    ok(ruPx > 100 && ruPx != rootPx, "the label role measures the Cyrillic name with ru/'s own glyphs");
    // the identity: a key naming the root font (an English game's own) measures exactly the header
    std::vector<std::string> caps = captions();
    const std::vector<std::string> lab = labelLines();
    caps.insert(caps.end(), lab.begin(), lab.end());
    int diff = 0;
    for (const std::string& s : caps)
        for (int size = 1; size <= ut::kUtTmMaxSize; ++size)
            diff += ut::utFontCacheWidth(root, s.c_str(), size) != ut::utTextWidthPx(s.c_str(), size);
    ok(diff == 0, "font.caption = fonts/savapromedium.fnt on an English game: today's widths, every size");
}

// ---- the font follows the letters (ut_buttons.h utbDeriveFolder) over the REAL glyph tables ------
struct GlyphTables {
    std::vector<std::string> folder;
    std::vector<std::vector<char32_t>> glyphs;
};
bool tablesCover(const std::string& folder, char32_t cp, const void* ctx) {
    const GlyphTables* t = (const GlyphTables*)ctx;
    for (size_t i = 0; i < t->folder.size(); ++i) {
        if (t->folder[i] != folder) continue;
        return ut::utCodePointIn(t->glyphs[i], cp);
    }
    return false;
}
void deriveHalf(const gen::ArcArchive& arc) {
    GlyphTables t;
    std::vector<std::string> folders(1, std::string());
    for (const std::string& f : utb::utbFontFolderList()) folders.push_back(f);
    int baked = 0;
    for (const std::string& f : folders) {
        const std::string name = (f.empty() ? std::string() : f + "/") + "savapromedium.fnt";
        const gen::ArcEntry* en = arc.entry(name);
        std::vector<std::uint8_t> d;
        std::vector<char32_t> gl;
        if (!(en && arc.read(*en, d) && ut::utFntxGlyphs(d.data(), d.size(), 13, &gl))) gl.clear();
        baked += !gl.empty();
        t.folder.push_back(f);
        t.glyphs.push_back(gl);
    }
    ok(baked == (int)folders.size(), "derive: every folder's savapromedium has an FNTX glyph table (utFntxGlyphs)");
    bool sorted = true, ascii = true;
    for (size_t i = 1; i < t.glyphs[0].size(); ++i) sorted &= t.glyphs[0][i - 1] < t.glyphs[0][i];
    for (char32_t c = 0x21; c <= 0x7E; ++c) ascii &= tablesCover("", c, &t);
    ok(sorted && ascii, "derive: the root table is sorted and has every printable ASCII letter");
    auto derive = [&](const char* utf8) {
        std::u32string cps;
        utt::utf8Decode(std::string(utf8), cps);
        return utb::utbDeriveFolder(cps, tablesCover, &t);
    };
    ok(derive("HEL").folder.empty(), "derive: ASCII -> the root font (the identity)");
    const utb::UtbDerived ru = derive("\xD0\xA8\xD0\x9B\xD0\x9C \xD0\xA8\xD0\xBB\xD0\xB5\xD0\xBC\xD1\x8B");
    ok(ru.folder == "ru" && ru.coversAll, "derive: Cyrillic letters -> fonts/ru/savapromedium.fnt over the real tables");
    const utb::UtbDerived el = derive("\xCE\xA9\xCE\xB1");
    ok(el.folder == "el" && el.coversAll, "derive: Greek -> el");
    const utb::UtbDerived de = derive("\xC3\x9F\xC3\xBC");   // sz, u-umlaut
    ok(!de.folder.empty() && de.coversAll && tablesCover(de.folder, 0xDF, &t), "derive: a Latin-1 letter -> a copy that has it",
       de.folder.c_str());
    printf("[text] derive: Cyrillic -> %s, Greek -> %s, sz/u-umlaut -> %s (%zu+%zu+%zu glyphs in root/ru/el at 13)\n",
           ru.folder.c_str(), el.folder.c_str(), de.folder.c_str(), t.glyphs[0].size(), t.glyphs[12].size(),
           t.glyphs[5].size());
    const utb::UtbDerived hanOld = derive("\xE8\x88\xB5");
    ok(hanOld.folder.empty() && !hanOld.coversAll, "derive: U+8235 is in no baked copy (FNTX tables only)");

    // The FNTD copies' cmaps beside the FNTX tables, as glyphSetsLoad reads them
    for (const std::string& f : utb::utbDynamicFolderList()) {
        const gen::ArcEntry* en = arc.entry(f + "/savapromedium.fnt");
        std::vector<std::uint8_t> d;
        std::vector<char32_t> cm;
        if (!(en && arc.read(*en, d) && ut::utFntdIs(d.data(), d.size()) && ut::utSfntCodePoints(d.data(), d.size(), &cm)))
            cm.clear();
        ok(!cm.empty(), "derive: the FNTD copy has a cmap", f.c_str());
        t.folder.push_back(f);
        t.glyphs.push_back(cm);
    }
    auto deriveF = [&](const char* utf8) {
        std::u32string cps;
        utt::utf8Decode(std::string(utf8), cps);
        return utb::utbDeriveFolder(cps, tablesCover, &t, true);
    };
    ok(deriveF("HEL").folder.empty() && deriveF("\xD0\xA8\xD0\x9B\xD0\x9C").folder == "ru" &&
           deriveF("\xCE\xA9\xCE\xB1").folder == "el" && deriveF("\xC3\x9F\xC3\xBC").folder == de.folder,
       "derive: the identity over the real tables - ASCII root, Cyrillic ru, Greek el, Latin-1 as before");
    const utb::UtbDerived zh = deriveF("\xE8\x88\xB5\xE6\x89\x8B");   // U+8235 U+624B
    ok(zh.folder == "zh" && zh.coversAll && zh.dynamic, "derive: Han -> fonts/zh/savapromedium.fnt (WenQuanYi has it)");
    const utb::UtbDerived ja = deriveF("\xE3\x81\x8B\xE3\x81\xAA\xE8\x88\xB5");   // ka na + U+8235
    ok(ja.folder == "ja" && ja.coversAll && ja.dynamic, "derive: kana (with Han) -> ja (Noto Sans JP has both)");
    const utb::UtbDerived ko = deriveF("\xED\x95\x9C\xEA\xB8\x80");   // U+D55C U+AE00
    ok(ko.folder == "ko" && ko.coversAll && ko.dynamic, "derive: Hangul -> ko (NanumGothic)");
    const utb::UtbDerived th = deriveF("\xE0\xB8\x81\xE0\xB8\x82\xE0\xB8\x84");   // Thai, in no font
    ok(th.folder == "zh" && !th.coversAll && th.dynamic && th.lacking == 3 && th.missing == 0xE01,
       "derive: Thai -> zh, its three letters drawn by Windows' fallback (one INFO)");
    const utb::UtbDerived mix = deriveF("\xD0\xA8\xD0\xBB\xD0\xB5\xD0\xBC \xE8\x88\xB5");
    ok(mix.folder == "zh" && mix.coversAll, "derive: Cyrillic with Han -> zh (WenQuanYi has both)");
    printf("[text] derive: Han -> %s, kana -> %s, Hangul -> %s, Thai -> %s (%s)\n", zh.folder.c_str(),
           ja.folder.c_str(), ko.folder.c_str(), th.folder.c_str(),
           utb::utbDerivedFallbackText(th, "the captions").c_str());
}

// ---- 6 the FNTD copies' cmaps (src\ut_sfnt.h) - pure, then over the real Fonts.arc ------
void be16(std::vector<unsigned char>& v, unsigned x) {
    v.push_back((unsigned char)(x >> 8));
    v.push_back((unsigned char)x);
}
void be32(std::vector<unsigned char>& v, std::uint32_t x) {
    be16(v, x >> 16);
    be16(v, x & 0xFFFF);
}
void le32(std::vector<unsigned char>& v, std::uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back((unsigned char)(x >> (8 * i)));
}
// A one-table sfnt whose cmap has a format-4 subtable (3,1) - A..C by delta, 0x50 -> glyph 0 and
// 0x51 -> 7 through the glyph array, U+8235 -> 5, the 0xFFFF terminator - and, when asked, a
// format-12 one (3,10): A..C, 0x60 -> glyph 0, U+8235, U+1F600. `base` = where the sfnt starts in
// the file (a TTC's table offsets count from the file's start).
std::vector<unsigned char> toySfnt(bool fmt12, std::uint32_t base) {
    std::vector<unsigned char> f4;
    be16(f4, 4);
    be16(f4, 0);   // length, patched
    be16(f4, 0);
    be16(f4, 8);   // segCountX2: 4 segments
    be16(f4, 0);
    be16(f4, 0);
    be16(f4, 0);
    for (unsigned e : {0x43u, 0x51u, 0x8235u, 0xFFFFu}) be16(f4, e);
    be16(f4, 0);
    for (unsigned a : {0x41u, 0x50u, 0x8235u, 0xFFFFu}) be16(f4, a);
    for (unsigned d : {(1u - 0x41u) & 0xFFFFu, 0u, 0u, 1u}) be16(f4, d);
    for (unsigned r : {0u, 6u, 8u, 0u}) be16(f4, r);   // to glyphs[0] and glyphs[2]
    for (unsigned g : {0u, 7u, 5u}) be16(f4, g);
    f4[2] = (unsigned char)(f4.size() >> 8);
    f4[3] = (unsigned char)f4.size();
    std::vector<unsigned char> f12;
    be16(f12, 12);
    be16(f12, 0);
    be32(f12, 16 + 4 * 12);
    be32(f12, 0);
    be32(f12, 4);
    const std::uint32_t groups[4][3] = {{0x41, 0x43, 1}, {0x60, 0x60, 0}, {0x8235, 0x8235, 5}, {0x1F600, 0x1F600, 9}};
    for (const auto& g : groups)
        for (std::uint32_t x : g) be32(f12, x);
    std::vector<unsigned char> cmap;
    const unsigned nt = fmt12 ? 2 : 1;
    be16(cmap, 0);
    be16(cmap, nt);
    be16(cmap, 3);
    be16(cmap, 1);
    be32(cmap, 4 + 8 * nt);
    if (fmt12) {
        be16(cmap, 3);
        be16(cmap, 10);
        be32(cmap, 4 + 8 * nt + (std::uint32_t)f4.size());
    }
    cmap.insert(cmap.end(), f4.begin(), f4.end());
    if (fmt12) cmap.insert(cmap.end(), f12.begin(), f12.end());
    std::vector<unsigned char> s;
    be32(s, 0x00010000);
    be16(s, 1);
    be16(s, 0);
    be16(s, 0);
    be16(s, 0);
    be32(s, 0x636D6170);   // 'cmap'
    be32(s, 0);
    be32(s, base + 28);
    be32(s, (std::uint32_t)cmap.size());
    s.insert(s.end(), cmap.begin(), cmap.end());
    return s;
}

// The reader over a copy that ends exactly at a page with no access after it: a read past the
// buffer faults the test (an access violation) instead of passing unseen.
struct GuardBuf {
    unsigned char* base = nullptr;
    GuardBuf() {
        base = (unsigned char*)VirtualAlloc(nullptr, 2 * 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        DWORD old = 0;
        if (base) VirtualProtect(base + 4096, 4096, PAGE_NOACCESS, &old);
    }
    ~GuardBuf() {
        if (base) VirtualFree(base, 0, MEM_RELEASE);
    }
    const unsigned char* put(const unsigned char* d, size_t n) {
        unsigned char* at = base + 4096 - n;
        if (n) memcpy(at, d, n);
        return at;
    }
};

void sfntPureHalf() {
    std::vector<char32_t> cp;
    int fmt = 0;
    const std::vector<unsigned char> f = toySfnt(true, 0);
    ok(ut::utSfntCodePoints(f.data(), f.size(), &cp, &fmt) && fmt == 12 &&
           cp == std::vector<char32_t>({0x41, 0x42, 0x43, 0x8235, 0x1F600}),
       "sfnt: (3,10) format 12 preferred; glyph 0 left out; a code point above U+FFFF kept");
    const std::vector<unsigned char> g = toySfnt(false, 0);
    ok(ut::utSfntCodePoints(g.data(), g.size(), &cp, &fmt) && fmt == 4 &&
           cp == std::vector<char32_t>({0x41, 0x42, 0x43, 0x51, 0x8235}),
       "sfnt: format 4 - by delta, through the glyph array (glyph 0 left out), no 0xFFFF");
    std::vector<unsigned char> ttc;
    le32(ttc, 0x66637474);   // "ttcf" as bytes t t c f
    ttc[0] = 't';
    ttc[1] = 't';
    ttc[2] = 'c';
    ttc[3] = 'f';
    be32(ttc, 0x00010000);
    be32(ttc, 1);
    be32(ttc, 16);
    const std::vector<unsigned char> inner = toySfnt(true, 16);
    ttc.insert(ttc.end(), inner.begin(), inner.end());
    ok(ut::utSfntCodePoints(ttc.data(), ttc.size(), &cp, &fmt) && fmt == 12 && cp.size() == 5,
       "sfnt: a TrueType collection - face 0, its table offsets from the file's start");
    std::vector<unsigned char> fntd;
    for (std::uint32_t x : {0x44544E46u, 1u, 1u, 1u, 0u, 0u, 0u}) le32(fntd, x);
    fntd.insert(fntd.end(), f.begin(), f.end());
    ok(ut::utFntdIs(fntd.data(), fntd.size()) && ut::utFntdIs(fntd.data(), 8) && !ut::utFntdIs(f.data(), f.size()) &&
           ut::utSfntCodePoints(fntd.data(), fntd.size(), &cp) && cp.size() == 5,
       "sfnt: the FNTD file - 'FNTD' version 1 told from 8 bytes, the sfnt read after its 28-byte header");
    std::vector<unsigned char> v2 = fntd;
    v2[4] = 2;
    ok(!ut::utFntdIs(v2.data(), v2.size()), "sfnt: FNTD version 2 is not the engine's FNTD");
    const unsigned char fntx[8] = {'F', 'N', 'T', 'X', 2, 0, 0, 0};
    ok(!ut::utFntdIs(fntx, 8) && !ut::utSfntCodePoints(fntx, 8, &cp) && cp.empty(),
       "sfnt: an FNTX file is neither FNTD nor an sfnt");
    GuardBuf gb;
    if (!gb.base) {
        ok(false, "sfnt: the guard page could be set up");
        return;
    }
    int refused = 0, cuts = 0;
    const std::vector<unsigned char>* srcs[4] = {&f, &g, &ttc, &fntd};
    for (const std::vector<unsigned char>* src : srcs) {
        for (size_t n = 0; n < src->size(); ++n) {
            ++cuts;
            refused += !ut::utSfntCodePoints(gb.put(src->data(), n), n, &cp) && cp.empty();
        }
    }
    ok(refused == cuts, "sfnt: every truncated file is refused, nothing read past its end (a guard page behind it)");
    int sane = 0, runs = 0;
    for (int it = 0; it < 4000; ++it) {
        std::vector<unsigned char> b = (it & 1) ? f : g;
        const int flips = 1 + (int)(rnd() % 4);
        for (int k = 0; k < flips; ++k) b[12 + rnd() % (b.size() - 12)] = (unsigned char)rnd();
        ++runs;
        const bool r = ut::utSfntCodePoints(gb.put(b.data(), b.size()), b.size(), &cp);
        bool good = true;
        for (size_t i = 1; i < cp.size(); ++i) good &= cp[i - 1] < cp[i];
        for (char32_t c : cp) good &= c <= 0x10FFFF;
        sane += good && (r || cp.empty());
    }
    ok(sane == runs, "sfnt: 4000 corrupted files - no read past the buffer, every answer sorted and in range");
}

void sfntRealHalf(const gen::ArcArchive& arc) {
    auto read = [&](const char* name, std::vector<std::uint8_t>* d) {
        const gen::ArcEntry* en = arc.entry(name);
        return en && arc.read(*en, *d);
    };
    struct Want {
        const char* name;
        size_t count;
        int fmt;
    };
    const Want want[3] = {{"zh/savapromedium.fnt", 34600, 12},
                          {"ja/savapromedium.fnt", 16732, 12},
                          {"ko/savapromedium.fnt", 17666, 4}};
    std::vector<char32_t> cm[3];
    for (int i = 0; i < 3; ++i) {
        std::vector<std::uint8_t> d;
        int fmt = 0;
        const bool r = read(want[i].name, &d) && ut::utFntdIs(d.data(), d.size()) &&
                       ut::utSfntCodePoints(d.data(), d.size(), &cm[i], &fmt);
        printf("[text] FNTD %s: %zu bytes, %zu code points (cmap format %d)\n", want[i].name, d.size(), cm[i].size(), fmt);
        char what[160];
        snprintf(what, sizeof what, "FNTD %s: %zu code points, format %d (fontTools: %zu)", want[i].name, cm[i].size(),
                 fmt, want[i].count);
        ok(r && cm[i].size() == want[i].count && fmt == want[i].fmt, what);
    }
    for (const char* other : {"th/savapromedium.fnt", "vi/savapromedium.fnt"}) {
        std::vector<std::uint8_t> d;
        std::vector<char32_t> c;
        ok(read(other, &d) && ut::utSfntCodePoints(d.data(), d.size(), &c) && c == cm[0],
           "FNTD th/ and vi/ carry zh's font (the same code points)");
    }
    std::vector<std::uint8_t> d;
    std::vector<char32_t> misc;
    ok(read("misc.fnt", &d) && ut::utFntdIs(d.data(), d.size()) && ut::utSfntCodePoints(d.data(), d.size(), &misc) &&
           misc.size() == 2188 && !ut::utCodePointIn(misc, 0x8235),
       "FNTD misc.fnt (Noto Sans): 2,188 code points, no U+8235");
    std::vector<char32_t> none;
    ok(read("savapromedium.fnt", &d) && !ut::utFntdIs(d.data(), d.size()) &&
           !ut::utSfntCodePoints(d.data(), d.size(), &none),
       "the root savapromedium.fnt is FNTX: not FNTD, no cmap");
    ok(ut::utCodePointIn(cm[0], 0x8235) && ut::utCodePointIn(cm[1], 0x8235) && ut::utCodePointIn(cm[2], 0x8235),
       "U+8235 is in zh, ja and ko");
    ok(!ut::utCodePointIn(cm[0], 0xE01) && !ut::utCodePointIn(cm[1], 0xE01) && !ut::utCodePointIn(cm[2], 0xE01),
       "U+0E01 (Thai) is in none of them - Windows draws it");
    ok(ut::utCodePointIn(cm[0], 0xAC00) && ut::utCodePointIn(cm[2], 0xAC00) && !ut::utCodePointIn(cm[1], 0xAC00) &&
           ut::utCodePointIn(cm[0], 0x3042) && ut::utCodePointIn(cm[1], 0x3042) && ut::utCodePointIn(cm[0], 0x428),
       "zh has Hangul, kana and Cyrillic; ko Hangul; ja kana and no Hangul");
}

// ---- 7 the live probe's plan on a dynamic font: only the sizes in use ----------------------
// A dynamic (FNTD) font as the engine builds it: GetStyle makes the EXACT size asked - one GDI
// font per new size - and every glyph asked of a style is one TextOutW.
struct DynFont {
    int styleCalls = 0, glyphCalls = 0;
    std::vector<int> made;   // the sizes the engine had to build
    int refuse = 0;          // a size the font gives nothing for
};
bool dynStyleOf(int size, const void** key, int* ss, void* ctx) {
    DynFont* f = (DynFont*)ctx;
    ++f->styleCalls;
    if (size == f->refuse) return false;
    bool seen = false;
    for (int m : f->made) seen |= m == size;
    if (!seen) f->made.push_back(size);
    *key = (const void*)(std::intptr_t)(0x1000 + size);
    *ss = size;
    return true;
}
bool dynGlyphOf(const void* key, char32_t cp, ut::UtGlyph* g, void* ctx) {
    DynFont* f = (DynFont*)ctx;
    ++f->glyphCalls;
    const int size = (int)((std::intptr_t)key - 0x1000);
    g->xOff = 0;
    g->rb = 1;
    g->w = size * (int)(cp % 5 + 3) / 8;
    return true;
}
struct LazyCtx {
    ut::UtFontCache* c;
    ut::UtFontProbe p;
    const std::vector<std::string>* caps;
    std::vector<int> asked;
};
int lazyWidest(int size, const void* ctx) {
    LazyCtx* l = (LazyCtx*)ctx;
    l->asked.push_back(size);
    int w = 0;
    for (const std::string& s : *l->caps) {
        const int t = ut::utFontCacheWidthLazy(l->c, l->p, s.c_str(), size);
        if (t > w) w = t;
    }
    return w;
}

void probePlanHalf() {
    ut::UtTmProbePlan pl = ut::utTmProbePlan(0, 14);
    ok(!pl.lazy && pl.lo == 1 && pl.hi == ut::kUtTmMaxSize, "probe plan: a baked (FNTX) font - every size 1..64, as always");
    pl = ut::utTmProbePlan(1, 14);
    ok(pl.lazy && pl.lo == 14 && pl.hi == 14, "probe plan: a dynamic (FNTD) font - only the nominal size now");
    pl = ut::utTmProbePlan(-1, 0);
    const ut::UtTmProbePlan big = ut::utTmProbePlan(1, 99);
    ok(pl.lazy && pl.lo == 1 && pl.hi == 1 && big.hi == ut::kUtTmMaxSize,
       "probe plan: a font of unknown kind is lazy too; the nominal size clamped to 1..64");

    // the eager fill builds 64 sizes on a dynamic font, one GDI font each (the lazy plan avoids it)
    static ut::UtFontCache eager;
    DynFont storm;
    const ut::UtFontProbe sp = {dynStyleOf, dynGlyphOf, &storm};
    ut::utFontCacheFill(&eager, sp);
    printf("[text] probe: the eager fill on a dynamic font builds %zu sizes, %d glyphs\n", storm.made.size(),
           storm.glyphCalls);
    ok(storm.made.size() == 64, "probe: the eager fill of a dynamic font builds all 64 sizes (what the lazy plan avoids)");

    // the plan: the nominal size, then only what the caption fit and the label ask
    static ut::UtFontCache lazy;
    DynFont dyn;
    const ut::UtFontProbe dp = {dynStyleOf, dynGlyphOf, &dyn};
    ok(ut::utFontCacheFillPlan(&lazy, dp, ut::utTmProbePlan(1, 14)) == 1 && lazy.lazy == 1 && dyn.made.size() == 1 &&
           dyn.made[0] == 14 && dyn.glyphCalls == ut::kUtTmChars,
       "probe: the world's start builds ONE size (14) and its 95 ASCII glyphs");
    const std::vector<std::string> caps = captions();
    LazyCtx lc = {&lazy, dp, &caps, {}};
    const int fit = ut::utCaptionSize(34, 20, 14, lazyWidest, &lc);
    std::vector<int> inUse(1, 14);
    for (int a : lc.asked) {
        bool dup = false;
        for (int u : inUse) dup |= u == a;
        if (!dup) inUse.push_back(a);
    }
    bool within = true;
    for (int m : dyn.made) {
        bool asked = false;
        for (int u : inUse) asked |= u == m;
        within &= asked;
    }
    printf("[text] probe: the caption fit in a 34x20 cell asked %zu size(s) (fit %d); %zu built, %d glyphs\n",
           lc.asked.size(), fit, dyn.made.size(), dyn.glyphCalls);
    ok(fit >= ut::kUtCaptionMin && within && dyn.made.size() == inUse.size() && dyn.made.size() <= 14 - fit + 2 &&
           dyn.glyphCalls == ut::kUtTmChars * (int)dyn.made.size(),
       "probe: the caption fit builds only the sizes it asks (the nominal one and top..fit), 95 glyphs each");
    // the whole pad fit (every cell size utPadFit tries, at UI scales 1.0 and 2.0) on a fresh lazy cache:
    // only sizes between the caption floor and the cap are ever built - never the 64
    {
        static ut::UtFontCache padc;
        DynFont pf;
        const ut::UtFontProbe pp = {dynStyleOf, dynGlyphOf, &pf};
        ut::utFontCacheFillPlan(&padc, pp, ut::utTmProbePlan(1, 14));
        LazyCtx pc = {&padc, pp, &caps, {}};
        const ut::UtPadFit f1 = ut::utPadFit(ut::utPadDefault(25, 14, 1), 26, 1, 1.0f, 14, lazyWidest, &pc);
        const ut::UtPadFit f2 = ut::utPadFit(ut::utPadDefault(25, 14, 1), 26, 1, 2.0f, 14, lazyWidest, &pc);
        bool inRange = true;
        for (int m : pf.made) inRange &= m >= ut::kUtCaptionMin && m <= 14;
        printf("[text] probe: the pad fit at scales 1 and 2 asked %zu widths; %zu size(s) built (of 64), %d glyphs\n",
               pc.asked.size(), pf.made.size(), pf.glyphCalls);
        ok(f1.ok && f2.ok && inRange && pf.made.size() <= (size_t)(14 - ut::kUtCaptionMin + 1) &&
               pf.glyphCalls == ut::kUtTmChars * (int)pf.made.size(),
           "probe: the whole pad fit builds only sizes 6..14 (the caption floor..cap), each once");
    }
    const size_t before = dyn.made.size();
    const int calls = dyn.styleCalls;
    ut::utFontCacheWidthLazy(&lazy, dp, "Unique items - 12/34", 13);   // the label's size
    ut::utFontCacheWidthLazy(&lazy, dp, "Unique items - 12/34", 13);
    ut::utCaptionSize(34, 20, 14, lazyWidest, &lc);
    ok(dyn.made.size() <= before + 1 && dyn.styleCalls <= calls + 1,
       "probe: the label's size is one more at most; measuring again builds nothing");
    dyn.refuse = 40;
    ok(ut::utFontCacheWidthLazy(&lazy, dp, "HEL", 40) == -1 && lazy.sizeStyle[40] == -2,
       "probe: a size the font gives nothing for is marked once (-2)");
    const int c40 = dyn.styleCalls;
    ut::utFontCacheWidthLazy(&lazy, dp, "HEL", 40);
    ok(dyn.styleCalls == c40, "probe: ... and never asked again");
    ok(ut::utFontCacheWidthLazy(&lazy, dp, "", 50) == 0 && lazy.sizeStyle[50] == -1,
       "probe: an empty string builds nothing");

    // a baked font measured lazily measures exactly what the eager fill measures (the identity)
    static ut::UtFontCache tEager, tLazy;
    const ut::UtFontProbe tp = {ut::utTmTableStyleOf, ut::utTmTableGlyphOf, nullptr};
    ut::utFontCacheFill(&tEager, tp);
    ut::utFontCacheFillPlan(&tLazy, tp, ut::utTmProbePlan(-1, 14));
    int diff = 0;
    for (const std::string& s : caps)
        for (int size = 1; size <= ut::kUtTmMaxSize; ++size)
            diff += ut::utFontCacheWidthLazy(&tLazy, tp, s.c_str(), size) != ut::utFontCacheWidth(tEager, s.c_str(), size);
    ok(diff == 0, "probe: a lazy cache over a baked font measures every caption at every size as the eager one");
    static ut::UtFontCache tPlan;
    ut::utFontCacheFillPlan(&tPlan, tp, ut::utTmProbePlan(0, 14));
    ok(!tPlan.lazy && tPlan.valid && !memcmp(&tPlan, &tEager, sizeof(tPlan)),
       "probe: the plan of a baked font IS the eager fill, byte for byte");
}

void fileHalf() {
    std::string arcPath;
    const char* e = getenv("UNIQUETAB_TEST_FONTS_ARC");
    if (e && *e) arcPath = e;
    const char* gd = getenv("UNIQUETAB_TEST_GAME_DIR");
    gen::ArcArchive arc;
    std::string err;
    bool loaded = !arcPath.empty() && arc.load(arcPath, &err);
    if (!loaded && gd && *gd) {
        arcPath = std::string(gd) + "\\resources\\Fonts.arc";
        loaded = arc.load(arcPath, &err);
    }
    // UNIQUETAB_TEST_DBMODS_DIR is a game's mods\ folder: its game's Fonts.arc, read only.
    const char* dbm = getenv("UNIQUETAB_TEST_DBMODS_DIR");
    if (!loaded && dbm && *dbm) {
        arcPath = std::string(dbm) + "\\..\\resources\\Fonts.arc";
        loaded = arc.load(arcPath, &err);
    }
    if (!loaded) {
        printf("[text] ***** SKIPPED the font-file half - no Fonts.arc (set UNIQUETAB_TEST_FONTS_ARC "
               "to a game's resources\\Fonts.arc; it is only READ) *****\n");
        return;
    }
    printf("[text] font file: %s (read only)\n", arcPath.c_str());
    auto readFont = [&](const char* name, Fnt* f) {
        const gen::ArcEntry* en = arc.entry(name);
        std::vector<std::uint8_t> d;
        return en && arc.read(*en, d) && f->parse(d);
    };
    static Fnt root, ru;
    ok(readFont("savapromedium.fnt", &root), "the root savapromedium.fnt parses");
    static ut::UtFontCache live, table;
    ut::utFontCacheFromTable(&table);
    const ut::UtFontProbe pr = {fntStyleOf, fntGlyphOf, &root};
    ok(ut::utFontCacheFill(&live, pr) == 1, "the font file fills a valid cache");
    std::vector<std::string> strs = captions();
    const std::vector<std::string> lab = labelLines();
    strs.insert(strs.end(), lab.begin(), lab.end());
    std::vector<const char*> p;
    for (const std::string& s : strs) p.push_back(s.c_str());
    ut::UtTmCheck r = ut::utFontCacheCompare(live, table, p.data(), (int)p.size(), 5, 36);
    printf("[text] the mod's per-world cross-check on the real root font: %d sizes, %d styles, %d "
           "glyphs, %d widths, %d differ, largest delta %d px%s%s\n",
           r.sizes, r.styles, r.glyphs, r.widths, r.sizeDiff + r.styleDiff + r.glyphDiff +
           r.widthDiff, r.maxDelta, r.first[0] ? " - first: " : "", r.first);
    ok(r.agree == 1, "the root font file agrees with the compiled table (the live route would be ON)",
       r.first);
    ut::UtTmCheck all = ut::utFontCacheCompare(live, table, p.data(), (int)p.size(), 1, 64);
    ok(all.agree == 1, "and over every cached size 1..64", all.first);

    if (!readFont("ru/savapromedium.fnt", &ru)) {
        ok(false, "ru/savapromedium.fnt parses");
        return;
    }
    static ut::UtFontCache ruc;
    const ut::UtFontProbe rp = {fntStyleOf, fntGlyphOf, &ru};
    ok(ut::utFontCacheFill(&ruc, rp) == 1, "the Russian font fills a valid cache");
    int cyr = 0, cyrRoot = 0;
    for (unsigned cp = 0x410; cp <= 0x44F; ++cp) {
        ut::UtGlyph g;
        if (fntGlyphOf(ruc.key[ruc.sizeStyle[10]], cp, &g, nullptr)) ++cyr;
        if (fntGlyphOf(live.key[live.sizeStyle[10]], cp, &g, nullptr)) ++cyrRoot;
    }
    ok(cyr == 64 && cyrRoot == 0, "U+0410..044F: all in ru/, none in the root font");
    auto extra = [](const void* key, char32_t cp, ut::UtGlyph* g, void*) {
        return fntGlyphOf(key, cp, g, nullptr);
    };
    const char* helms = "\xD0\xA8\xD0\xBB\xD0\xB5\xD0\xBC\xD1\x8B";   // U+0428 043B 0435 043C 044B
    const int w10 = ut::utFontCacheWidth(ruc, helms, 10, extra, nullptr);
    const int q10 = ut::utFontCacheWidth(ruc, helms, 10);
    printf("[text] ru: the five-letter Helms word at size 10 = %d px (as five '?': %d px)\n", w10,
           q10);
    ok(w10 > 20 && w10 < 50 && w10 != q10, "Cyrillic is measured with its own glyphs");
    r = ut::utFontCacheCompare(ruc, table, p.data(), (int)p.size(), 5, 36);
    printf("[text] ru/ ASCII against the table: %d glyphs, %d widths differ (largest %d px)\n",
           r.glyphDiff, r.widthDiff, r.maxDelta);

    // The live chain over the REAL fonts (ut_panel.cpp's glyph callback asks GetMetric of the
    // font that draws each role, at its size-13 style). The pad and the label draw the ROOT
    // font in every game language unless a [style] font key names another, and [text.ru] applies
    // only by [style] language = ru. A file with language = ru, [text.ru] and the two font keys
    // (ru/) draws its Cyrillic captions and name on an English and on a Russian game alike; with no
    // file a Russian game draws the built-in English in the root font; on the root font the
    // Cyrillic candidates are passed over, named.
    struct RealFont {
        const void* key;
        static bool glyph(utt::UtTextFont, char32_t cp, const void* ctx) {
            if (cp >= 0x20 && cp <= 0x7E) return true;
            ut::UtGlyph g;
            return fntGlyphOf(((const RealFont*)ctx)->key, cp, &g, nullptr);
        }
    };
    const std::string shlm = "\xD0\xA8\xD0\x9B\xD0\x9C", ngr = "\xD0\x9D\xD0\x93\xD0\xA0";
    const std::string ruBody = "[text.ru]\ncaption.head = " + shlm + "\ncaption.chest = " + ngr + "\nname.head = " +
                               helms + "\n";
    utt::UtTextFile tf, tfNoLang;
    std::string perr;
    ok(utt::utTextParse("[style]\nlanguage = ru\nfont.caption = fonts/ru/savapromedium.fnt\n"
                        "font.label = fonts/ru/savapromedium.fnt\n" + ruBody,
                        tf, &perr),
       "a file with language = ru, the two font keys and [text.ru] parses", perr.c_str());
    ok(utt::utTextParse(ruBody, tfNoLang, &perr), "the same [text.ru] without [style] parses", perr.c_str());
    std::vector<utt::UtTextGroupIn> groups;
    {
        size_t nb = 0;
        const utt::UtTextBuiltin* b = utt::utTextBuiltins(&nb);
        for (size_t i = 1; i <= 24 && i < nb; ++i) groups.push_back({b[i].id, b[i].caption, b[i].name});
    }
    // The font each role draws in, by the rule ut_panel.cpp loads with: a key in force, else the
    // root font - the game's language plays no part.
    const bool keyPaths =
        strcmp(ut::utRoleFontPath(ut::kRoleFontOwn, tf.style.font[utt::kStyleFontCaption].c_str()),
               "fonts/ru/savapromedium.fnt") == 0 &&
        strcmp(ut::utRoleFontPath(ut::kRoleFontEngine, nullptr), "fonts/savapromedium.fnt") == 0 &&
        strcmp(ut::utRoleFontPath(ut::kRoleFontMissing, "fonts/xx/savapromedium.fnt"), "fonts/savapromedium.fnt") == 0;
    ok(keyPaths, "a role draws its key's font while in force, else the root fonts/savapromedium.fnt");
    const RealFont ruFont = {ruc.key[ruc.sizeStyle[13]]}, rootFont = {live.key[live.sizeStyle[13]]};
    utt::UtTextContext rc;
    rc.glyph = RealFont::glyph;
    // no file, a game running in Russian: the root font, the built-in English, nothing passed over
    rc.language = "russian";
    rc.english = false;
    rc.glyphCtx = &rootFont;
    const utt::UtTextPad noFile = utt::utTextResolvePad(rc, groups);
    ok(noFile.caps[1] == "HEL" && noFile.caps[3] == "CHE" && noFile.names[0] == "Helms" && noFile.skipped.empty(),
       "a Russian game with no file: HEL, CHE, Helms in the root font - the built-in English");
    // the file with language = ru: the keys' ru/ font on either game language
    rc.file = &tf;
    rc.glyphCtx = &ruFont;
    const utt::UtTextPad ruPad = utt::utTextResolvePad(rc, groups);
    rc.language = "english";
    rc.english = true;
    const utt::UtTextPad enPad = utt::utTextResolvePad(rc, groups);
    ok(ruPad.caps[1] == shlm && ruPad.caps[3] == ngr && ruPad.names[0] == helms && ruPad.skipped.empty() &&
           enPad.caps == ruPad.caps && enPad.names == ruPad.names,
       "language = ru + [text.ru] + font.caption/font.label = ru/: Cyrillic on a Russian and on an English game");
    // without [style] language the section is ignored, even on a Russian game with the ru/ font
    rc.file = &tfNoLang;
    rc.language = "russian";
    rc.english = false;
    const utt::UtTextPad noLang = utt::utTextResolvePad(rc, groups);
    ok(noLang.caps[1] == "HEL" && noLang.names[0] == "Helms" && noLang.skipped.empty(),
       "[text.ru] without [style] language: not applied on a Russian game either");
    rc.file = &tf;
    // ...and the pad they make fits today's 34 x 14 cell, measured with ru/'s own glyphs.
    struct Widest {
        const ut::UtFontCache* c;
        const std::vector<std::string>* caps;
        static int fn(int size, const void* p) {
            const Widest* w = (const Widest*)p;
            auto extraRu = [](const void* key, char32_t cp, ut::UtGlyph* g, void*) {
                return fntGlyphOf(key, cp, g, nullptr);
            };
            int m = 0;
            for (const std::string& s : *w->caps) {
                const int t = ut::utFontCacheWidth(*w->c, s.c_str(), size, extraRu, nullptr);
                if (t > m) m = t;
            }
            return m;
        }
    };
    const Widest wr = {&ruc, &ruPad.caps};
    const int ruSize = ut::utCaptionSize(34, 14, ut::kUtCaptionMaxDefault, Widest::fn, &wr);
    printf("[text] the [text.ru] pad on ru/: caption size %d in the 34 x 14 cell (widest %d px)\n", ruSize,
           Widest::fn(ruSize > 0 ? ruSize : 6, &wr));
    ok(ruSize >= 9, "the Cyrillic pad keeps a full-size caption in today's cell");
    rc.glyphCtx = &rootFont;
    const utt::UtTextPad rootPad = utt::utTextResolvePad(rc, groups);
    ok(rootPad.caps[1] == "HEL" && rootPad.caps[3] == "CHE" && rootPad.names[0] == "Helms" &&
           rootPad.skipped.size() == 3 && rootPad.skipped[0].find("U+0428") != std::string::npos,
       "root font: passed over, named for the WARN, the built-in English drawn",
       rootPad.skipped.empty() ? "" : rootPad.skipped[0].c_str());

    // Whether the table bounds each language copy's 26 captions at every caption size - a report
    // (the mod's caption fit measures the font it draws, so nothing is gated on it). Every baked
    // (FNTX) copy in the archive passes; the FNTD ones (ja ko th vi zh: TrueType rasterised at run
    // time) cannot be read here.
    std::vector<const char*> capP;
    const std::vector<std::string> capS = captions();
    for (const std::string& c : capS) capP.push_back(c.c_str());
    int fntx = 0, fntd = 0, bounded = 0;
    std::string list;
    for (const gen::ArcEntry& en : arc.entries()) {
        const std::string& nm = en.key;
        if (nm.size() < 20 || nm.compare(nm.size() - 18, 18, "/savapromedium.fnt") != 0) continue;
        std::vector<std::uint8_t> d;
        static Fnt f;
        f = Fnt();
        if (!arc.read(en, d) || !f.parse(d)) {
            ++fntd;
            continue;
        }
        ++fntx;
        static ut::UtFontCache c;
        const ut::UtFontProbe fp = {fntStyleOf, fntGlyphOf, &f};
        ut::utFontCacheFill(&c, fp);
        const ut::UtTmBound b =
            ut::utTmBoundedByTable(c, table, capP.data(), (int)capP.size(), ut::kUtCaptionMin, 36);
        if (b.ok) ++bounded;
        list += " " + nm.substr(0, nm.size() - 18) + (b.ok ? "" : "(NOT)");
    }
    printf("[text] language copies bounded by the table:%s; %d FNTD copies not readable here\n",
           list.c_str(), fntd);
    ok(fntx >= 14 && bounded == fntx, "every baked language copy is bounded by the table");
    static ut::UtFontCache wide;
    wide = table;
    wide.glyph[wide.sizeStyle[10]]['W' - 32].w += 4;
    const ut::UtTmBound wb =
        ut::utTmBoundedByTable(wide, table, capP.data(), (int)capP.size(), ut::kUtCaptionMin, 36);
    ok(!wb.ok && wb.caption && strchr(wb.caption, 'W') && wb.live > wb.table,
       "a copy with one wider caption glyph is not bounded, naming the caption");

    fontRolesHalf(arc, live, ruc, groups);
    deriveHalf(arc);
    sfntRealHalf(arc);
}

// ---- 4 -------------------------------------------------------------------------------------------
void utf8Half() {
    unsigned short w[64];
    bool u = false;
    // ASCII: the identity, the same code units the char* call's movsx produced for 0x20..0x7E.
    char all[96];
    for (int i = 0; i < 95; ++i) all[i] = (char)(32 + i);
    all[95] = 0;
    unsigned short big[128];
    size_t n = ut::utWidenUtf8(all, big, 128, &u);
    bool same = n == 95 && u;
    for (int i = 0; i < 95 && same; ++i) {
        if (big[i] != (unsigned short)(signed short)(signed char)all[i]) same = false;
    }
    ok(same && big[95] == 0, "printable ASCII widens to exactly the char* call's code units");

    n = ut::utWidenUtf8("\xD0\xA8\xD0\xBB\xD0\xBC", w, 64, &u);
    ok(n == 3 && u && w[0] == 0x428 && w[1] == 0x43B && w[2] == 0x43C && w[3] == 0,
       "Cyrillic decodes");
    n = ut::utWidenUtf8("\xE5\xA4\xB4\xE7\x9B\x94", w, 64, &u);
    ok(n == 2 && u && w[0] == 0x5934 && w[1] == 0x76D4, "CJK decodes");
    n = ut::utWidenUtf8("a\xF0\x9F\x98\x80" "b", w, 64, &u);
    ok(n == 4 && u && w[0] == 'a' && w[1] == 0xD83D && w[2] == 0xDE00 && w[3] == 'b',
       "an astral code point becomes a surrogate pair");

    struct Bad {
        const char* s;
        const char* what;
    } bad[] = {
        {"\xC0\xAF", "an overlong '/'"},   {"\xE0\x80\xAF", "an overlong 3-byte form"},
        {"\x80", "a stray continuation"},  {"\xD0", "a truncated pair"},
        {"\xE2\x82", "a truncated triple"}, {"\xED\xA0\x80", "a UTF-16 surrogate"},
        {"\xF4\x90\x80\x80", "above U+10FFFF"}, {"\xFF", "an invalid lead byte"},
        {"ok \xC4 umlaut", "a Latin-1 byte in ASCII"},
    };
    for (const Bad& b : bad) {
        n = ut::utWidenUtf8(b.s, w, 64, &u);
        bool latin = !u && n == strlen(b.s);
        for (size_t i = 0; i < n && latin; ++i) {
            if (w[i] != (unsigned short)(unsigned char)b.s[i]) latin = false;
        }
        char what[96];
        _snprintf_s(what, sizeof(what), _TRUNCATE, "%s falls back to byte-wise Latin-1", b.what);
        ok(latin, what);
    }

    // The cut: cap 3 keeps two units and the terminator, never half a pair.
    n = ut::utWidenUtf8("a\xF0\x9F\x98\x80", w, 3, &u);
    ok(n == 1 && w[1] == 0, "a surrogate pair that does not fit is left out whole");
    n = ut::utWidenUtf8("abc", w, 3, &u);
    ok(n == 2 && w[0] == 'a' && w[1] == 'b' && w[2] == 0, "the cut keeps the terminator");
    n = ut::utWidenUtf8("abc", w, 1, &u);
    ok(n == 0 && w[0] == 0, "cap 1 is the terminator only");
    ok(ut::utWidenUtf8(nullptr, w, 4) == 0 && w[0] == 0 && ut::utWidenUtf8("x", nullptr, 4) == 0,
       "null in, nothing out");
    // The tooltip's 128-unit storage: a 200-letter Cyrillic line is cut at 127 whole letters.
    std::string longRu;
    for (int i = 0; i < 200; ++i) longRu += "\xD0\x96";
    unsigned short t[128];
    n = ut::utWidenUtf8(longRu.c_str(), t, 128, &u);
    ok(n == 127 && t[126] == 0x416 && t[127] == 0, "a long line fills the storage, terminated");

    ok(ut::utTmIsPlainAscii("HEL - 3/288") && !ut::utTmIsPlainAscii("\xD0\xA8") &&
           !ut::utTmIsPlainAscii("a\tb"),
       "utTmIsPlainAscii");

    // The pad's per-caption clip (below size 6, or a fixed size.caption): in place, whole code
    // points, measured with the table's pen at size 6 (a code point outside ASCII measures as '?').
    struct W {
        static int px(const char* s, const void* ctx) { return ut::utTextWidthPx(s, *(const int*)ctx); }
    };
    const int six = 6;
    char cap[64] = "HEL";
    ok(ut::utTmClipToWidth(cap, 100, W::px, &six) && strcmp(cap, "HEL") == 0, "a caption that fits is kept");
    strcpy(cap, "SHOULDERS");
    const bool whole = ut::utTmClipToWidth(cap, ut::utTextWidthPx("SHOU", 6), W::px, &six);
    ok(!whole && strcmp(cap, "SHOU") == 0, "a long caption is cut to its button", cap);
    strcpy(cap, "\xD0\xA8\xD0\x9B\xD0\x9C\xD0\xAB");   // four Cyrillic letters
    ut::utTmClipToWidth(cap, ut::utTextWidthPx("????", 6), W::px, &six);   // the table: a byte outside ASCII is one '?'
    ok(strcmp(cap, "\xD0\xA8\xD0\x9B") == 0, "Cyrillic is cut between letters, never inside one");
    strcpy(cap, "A\xF0\x9F\x98\x80");   // a 4-byte code point last
    ut::utTmClipToWidth(cap, ut::utTextWidthPx("A", 6), W::px, &six);
    ok(strcmp(cap, "A") == 0, "a 4-byte sequence goes whole");
    strcpy(cap, "AB\xD0");   // a stray lead byte counts as one
    ut::utTmClipToWidth(cap, ut::utTextWidthPx("AB", 6), W::px, &six);
    ok(strcmp(cap, "AB") == 0, "a byte that is not UTF-8 goes alone");
    strcpy(cap, "WWW");
    ut::utTmClipToWidth(cap, 0, W::px, &six);
    ok(cap[0] == 0, "nothing fits: empty");
}

// ---- 5 -------------------------------------------------------------------------------------------
void miscHalf(const char* header) {
    char out[64];
    ok(ut::utLocalizedFontPath("fonts/savapromedium.fnt", "ru/", out, sizeof(out)) &&
           strcmp(out, "fonts/ru/savapromedium.fnt") == 0,
       "the special font folder goes after the last '/'", out);
    ok(ut::utLocalizedFontPath("fonts/savapromedium.fnt", "", out, sizeof(out)) &&
           strcmp(out, "fonts/savapromedium.fnt") == 0,
       "an empty folder leaves the path alone", out);
    ok(ut::utLocalizedFontPath("savapromedium.fnt", "zh/", out, sizeof(out)) &&
           strcmp(out, "zh/savapromedium.fnt") == 0,
       "a path without '/' takes the folder in front", out);
    ok(!ut::utLocalizedFontPath("fonts/savapromedium.fnt", "ru/", out, 10) && out[0] == 0,
       "a buffer too small is refused");

    // a role's own font: the Fonts.arc entry a key names, and when the role uses it
    ok(ut::utFontArcEntry("fonts/ru/savapromedium.fnt", out, sizeof(out)) &&
           strcmp(out, "ru/savapromedium.fnt") == 0,
       "fonts/ru/savapromedium.fnt is the arc entry ru/savapromedium.fnt", out);
    ok(ut::utFontArcEntry("fonts/savapromedium.fnt", out, sizeof(out)) && strcmp(out, "savapromedium.fnt") == 0,
       "the root font's entry", out);
    ok(!ut::utFontArcEntry("textures/a.fnt", out, sizeof(out)) && !ut::utFontArcEntry("fonts/", out, sizeof(out)) &&
           !ut::utFontArcEntry("fonts/ru/savapromedium.fnt", out, 8) && out[0] == 0,
       "not under fonts/, nothing after it, or a buffer too small: refused");
    ok(ut::utRoleFontState(false, 0, false, false, false) == ut::kRoleFontEngine,
       "no key: the engine's font, whatever else (today's pixels)");
    ok(ut::utRoleFontState(true, 1, true, true, true) == ut::kRoleFontOwn &&
           ut::utRoleFontState(true, -1, true, true, true) == ut::kRoleFontOwn,
       "found (or the archive unreadable) + proved + loaded + measurable: the key's font");
    ok(ut::utRoleFontState(true, 0, true, true, true) == ut::kRoleFontMissing &&
           ut::utRoleFontState(true, 1, false, true, true) == ut::kRoleFontNotProved &&
           ut::utRoleFontState(true, 1, true, false, true) == ut::kRoleFontNotLoaded &&
           ut::utRoleFontState(true, 1, true, true, false) == ut::kRoleFontNoStyles,
       "missing, not proved, not loaded, no styles: each refused on its own");
    bool whys = true;
    for (int st = ut::kRoleFontMissing; st <= ut::kRoleFontNoStyles; ++st)
        whys = whys && ut::utRoleFontWhy((ut::UtRoleFont)st)[0] != 0;
    ok(whys && ut::utRoleFontWhy(ut::kRoleFontOwn)[0] == 0, "every refusal says why");

    const std::vector<std::string> lab = labelLines();
    ok(lab.size() == 55, "the label lines: two per built-in name (27, no Retired) and the ASCII line");
    size_t longest = 0;
    for (const std::string& l : lab) longest = l.size() > longest ? l.size() : longest;
    ok(longest < 127, "no label line is cut by its buffer");

    // The header includes the standard library, ut_fontmetrics.h and ut_textcfg.h only.
    FILE* f = nullptr;
    if (header && fopen_s(&f, header, "rb") == 0 && f) {
        char line[512];
        int bad = 0, n = 0;
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "#include", 8) != 0) continue;
            ++n;
            if (!strstr(line, "<c") && !strstr(line, "\"ut_fontmetrics.h\"") &&
                !strstr(line, "\"ut_textcfg.h\""))
                ++bad;
        }
        fclose(f);
        ok(bad == 0 && n > 0, "ut_textmeasure.h includes no Windows or engine header");
        // ut_sfnt.h beside it includes the standard library only, and is plain ASCII
        std::string sp(header);
        const size_t at = sp.rfind("ut_textmeasure.h");
        if (at != std::string::npos) sp.replace(at, 16, "ut_sfnt.h");
        FILE* g = nullptr;
        int sbad = 0, sn = 0, nonAscii = 0;
        if (fopen_s(&g, sp.c_str(), "rb") == 0 && g) {
            int ch;
            while ((ch = fgetc(g)) != EOF) nonAscii += ch > 0x7E || (ch < 0x20 && ch != '\n' && ch != '\r');
            rewind(g);
            while (fgets(line, sizeof(line), g)) {
                if (strncmp(line, "#include", 8) != 0) continue;
                ++sn;
                if (!strchr(line, '<')) ++sbad;
            }
            fclose(g);
        }
        ok(sn > 0 && sbad == 0 && nonAscii == 0, "ut_sfnt.h includes standard headers only, plain ASCII", sp.c_str());
    } else {
        ok(false, "ut_textmeasure.h can be read for the purity check", header ? header : "(null)");
    }
}

}  // namespace

int main(int argc, char** argv) {
    printf("[text] the text engine's pure half (src\\ut_textmeasure.h)\n");
    tableHalf();
    compareHalf();
    fileHalf();
    sfntPureHalf();
    probePlanHalf();
    utf8Half();
    miscHalf(argc > 1 ? argv[1] : nullptr);
    printf("[text] %d check(s), %d failed\n", g_ran, g_fail);
    printf(g_fail ? "[text] FAILURES\n" : "[text] ALL PASS\n");
    return g_fail ? 1 : 0;
}
