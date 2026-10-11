// test_buttons.cpp - the buttons file (src\ut_buttons.h), offline: the grammar and every refusal,
// the buttons -> groups mapping and its errors, the union pages, [unassigned] and its byte-
// preserving rewrite, the generated default file against the Python twin's fixtures (vanilla, ROT,
// DGD) and the identity gates, the fold of an old uniquetab-text.ini, the font that follows the
// letters, the size/font per button. And the words' engine it shares (src\ut_textcfg.h, once
// test_textcfg.cpp): the old strings file's parser (the fold reads it) and every refusal,
// the label templates against 1.1.0's printf output, the fallback chain with its glyph and tag
// callbacks, the UTF-8 helper, the default template and its writer, the reload test. Links the
// REAL header (src\ut_textcfg.h), checks its built-in table against src\gen\ut_groups.h, and
// runs the default label through it against a copy of 1.1.0's label code over the real width
// table (src\ut_layout.h + ut_fontmetrics.h).
// No game, no Windows API.
//
//   test_buttons.exe <scratch dir> [src\ut_textcfg.h] [src\ut_buttons.h] [repo root]
#include "ut_textcfg.h"
#include "ut_buttons.h"
#include "gen/ut_groups.h"
#include "gen/ut_groups_default.h"   // the template's per-class groups (the late list)
#include "ut_layout.h"
#include "ut_padpage.h"   // the pad keyed by buttons: the wheel skip, the label counts, one grid per button
#include "ut_rowmath.h"   // the row window the live page runs (one grid per button's rows)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace {

int g_fail = 0, g_pass = 0;
// 1.1.0's groups: the first rows of utg::builtinGroups (the blueprints of 1.2.0 follow them).
const std::size_t kGroups110 = 24;

void check(bool ok, const std::string& what) {
    if (ok) { ++g_pass; return; }
    ++g_fail;
    std::printf("[test] FAIL: %s\n", what.c_str());
}

// Cyrillic and other non-Latin text, spelled as bytes so the source stays ASCII.
const char* const kSha = "\xD0\xA8";                                          // U+0428
const char* const kShLM = "\xD0\xA8\xD0\x9B\xD0\x9C";                         // 3 letters
const char* const kShlemy = "\xD0\xA8\xD0\xBB\xD0\xB5\xD0\xBC\xD1\x8B";       // 5 letters
const char* const kEuro = "\xE2\x82\xAC";                                     // U+20AC
const char* const kZhong = "\xE4\xB8\xAD";                                    // U+4E2D
const char* const kGrin = "\xF0\x9F\x98\x80";                                 // U+1F600

std::string rep(const char* s, int n) {
    std::string r;
    for (int i = 0; i < n; ++i) r += s;
    return r;
}

utt::UtTextFile mustParse(const std::string& text, const char* what) {
    utt::UtTextFile f;
    std::string err;
    const bool ok = utt::utTextParse(text, f, &err);
    check(ok, std::string(what) + " parses (" + err + ")");
    return f;
}

// The file must be refused, and the error must start with "line <n>:" and contain `needle`.
void mustRefuse(const std::string& text, int line, const char* needle, const char* what) {
    utt::UtTextFile f;
    std::string err;
    const bool ok = utt::utTextParse(text, f, &err);
    const std::string head = "line " + std::to_string(line) + ":";
    check(!ok, std::string(what) + ": refused");
    check(err.compare(0, head.size(), head) == 0, std::string(what) + ": names " + head + " (got \"" + err + "\")");
    check(err.find(needle) != std::string::npos, std::string(what) + ": says \"" + needle + "\" (got \"" + err + "\")");
    check(f.common.empty() && f.lang.empty() && f.keys == 0, std::string(what) + ": nothing kept");
}

// ---------------------------------------------------------------- UTF-8
void testUtf8() {
    auto valid = [](const std::string& s) { return utt::utf8Valid(s); };
    check(valid(""), "utf8: empty");
    check(valid("HEL"), "utf8: ascii");
    check(valid(kShlemy) && valid(kEuro) && valid(kZhong) && valid(kGrin), "utf8: 2/3/4-byte forms");
    check(utt::utf8Length(kShlemy) == 5, "utf8: length in code points");
    check(utt::utf16Units(kShlemy) == 5, "utf8: Cyrillic is one UTF-16 unit each");
    check(utt::utf16Units(kGrin) == 2, "utf8: U+1F600 is two UTF-16 units");
    check(utt::utf8Length(std::string("A") + kGrin + kZhong) == 3, "utf8: mixed length");
    const char* const bad[] = {
        "\x80", "\xBF", "A\x80", "\xC0\x80", "\xC1\xBF", "\xE0\x80\x80", "\xE0\x9F\xBF",
        "\xF0\x80\x80\x80", "\xF0\x8F\xBF\xBF", "\xED\xA0\x80", "\xED\xBF\xBF", "\xF4\x90\x80\x80",
        "\xF5\x80\x80\x80", "\xFF", "\xFE", "\xD0", "\xE2\x82", "\xF0\x9F\x98", "\xD0\x41",
        "\xE2\x28\xA1",
    };
    for (const char* b : bad) {
        char hex[64] = {0};
        for (std::size_t i = 0; b[i] && i < 8; ++i) std::snprintf(hex + i * 3, 4, "%02X ", (unsigned char)b[i]);
        check(!valid(b), std::string("utf8: refuses ") + hex);
        check(utt::utf8Length(b) == utt::kNpos && utt::utf16Units(b) == utt::kNpos,
              std::string("utf8: no length for ") + hex);
    }
    check(valid("\xED\x9F\xBF") && valid("\xEE\x80\x80") && valid("\xF4\x8F\xBF\xBF") && valid("\xC2\x80"),
          "utf8: the edges next to the refused ranges");
    // Encode -> decode round trip over every boundary and a sample of the planes.
    const char32_t edges[] = {0, 0x7F, 0x80, 0x7FF, 0x800, 0xD7FF, 0xE000, 0xFFFD, 0xFFFF, 0x10000,
                              0x10FFFF, 0x428, 0x4E2D, 0x1F600};
    bool rt = true;
    for (char32_t cp : edges) {
        std::string s;
        if (!utt::utf8Append(cp, s)) { rt = false; continue; }
        std::u32string back;
        if (!utt::utf8Decode(s, back) || back.size() != 1 || back[0] != cp) rt = false;
    }
    for (char32_t cp = 1; cp < 0x110000; cp += 97) {
        if (cp >= 0xD800 && cp <= 0xDFFF) continue;
        std::string s;
        std::u32string back;
        if (!utt::utf8Append(cp, s) || !utt::utf8Decode(s, back) || back.size() != 1 || back[0] != cp) rt = false;
    }
    check(rt, "utf8: encode/decode round trip");
    std::string s;
    check(!utt::utf8Append(0xD800, s) && !utt::utf8Append(0xDFFF, s) && !utt::utf8Append(0x110000, s) && s.empty(),
          "utf8: no surrogate or out-of-range code point is encoded");
    std::u16string w;
    check(utt::utf8ToUtf16(std::string("A") + kSha + kGrin, w) && w.size() == 4 && w[0] == u'A' && w[1] == 0x428
              && w[2] == 0xD83D && w[3] == 0xDE00,
          "utf16: A, U+0428 and a surrogate pair");
    std::string back;
    check(utt::utf16ToUtf8(w, back) && back == std::string("A") + kSha + kGrin, "utf16: back to UTF-8");
    check(!utt::utf16ToUtf8(std::u16string(1, char16_t(0xD83D)), back), "utf16: a lone high surrogate is refused");
    check(!utt::utf16ToUtf8(std::u16string(1, char16_t(0xDE00)), back), "utf16: a lone low surrogate is refused");
    check(!utt::utf8ToUtf16("\xD0", w), "utf16: ill-formed UTF-8 is refused");
    check(utt::utf8Prefix(kShlemy, 2) == "\xD0\xA8\xD0\xBB", "utf8: prefix never cuts inside a sequence");
    check(utt::utf8Prefix(kShlemy, 9) == kShlemy && utt::utf8Prefix(kShlemy, 0).empty(), "utf8: prefix bounds");
}

// ---------------------------------------------------------------- parsing
void testParse() {
    utt::UtTextFile f = mustParse("", "an empty file");
    check(f.keys == 0, "empty: no keys");
    f = mustParse("; only\n# comments\n   ; indented\n\n", "comments only");
    check(f.keys == 0, "comments: no keys");

    f = mustParse("\xEF\xBB\xBF[text]\r\ncaption.head = ABC\r\nname.head=  Head gear  \r\n", "BOM + CRLF");
    check(f.common.count("caption.head") && f.common["caption.head"].text == "ABC", "BOM: the first key is read");
    check(f.common["name.head"].text == "Head gear", "trimmed value");
    check(f.common["caption.head"].line == 2, "the line number is kept");

    f = mustParse("[text]\nname.head = A;B # C\ntooltip.yes = \"  kept  \"\nlabel.unknown = \"\"\"\n", "';' '#' and quotes");
    check(f.common["name.head"].text == "A;B # C", "';' and '#' are text inside a value");
    check(f.common["tooltip.yes"].text == "  kept  ", "double quotes keep the blanks");
    check(f.common["label.unknown"].text == "\"", "only the outer quotes go");

    f = mustParse("[TEXT]\nCaption.Head = h\n[Text.RU]\ncaption.head = " + std::string(kShLM) +
                      "\n[ text.zh-tw ]\nname.relic = " + kZhong + "\n[STYLE]\nColor.Label = #dbc794\n",
                  "case and spacing of sections and keys");
    check(f.common["caption.head"].text == "h", "[TEXT] is [text], Caption.Head is caption.head");
    check(f.lang.count("ru") && f.lang["ru"]["caption.head"].text == kShLM, "[Text.RU] is stored as ru");
    check(f.lang.count("zh-tw") && f.lang["zh-tw"]["name.relic"].text == kZhong, "[ text.zh-tw ]");
    check(f.style.color[utt::kStyleColorLabel] == 0xDBC794, "#rrggbb colour");

    f = mustParse("[text]\nlabel.mode_unknown =\ncaption.rot.runeword = RUN\ncaption.auto.itemrelic = CMP\n"
                  "caption.other = OTH\nname.retired = Gone\n",
                  "ids of any view; an empty mode prefix");
    check(f.common["label.mode_unknown"].text.empty(), "label.mode_unknown may be empty");
    check(f.common.count("caption.rot.runeword") && f.common.count("caption.auto.itemrelic"), "mod and auto ids");

    // a repeated section merges; the same key in two sections is fine
    f = mustParse("[text]\ncaption.head = A\n[text.ru]\ncaption.head = B\n[text]\ncaption.feet = C\n", "a reopened section");
    check(f.common.size() == 2 && f.lang["ru"].size() == 1, "reopened [text] merges");

    // twelve units is the most a caption takes, whatever the script
    mustParse("[text]\ncaption.head = " + rep(kSha, 12) + "\n", "12 Cyrillic letters");
    mustParse("[text]\ncaption.head = " + rep(kGrin, 6) + "\n", "6 emoji = 12 units");
    mustParse("[text]\nname.head = " + rep("n", 48) + "\ntooltip.no = " + rep("t", 96) + "\nlabel.unknown = " +
                  rep("u", 8) + "\nlabel.format.1 = " + rep("f", 160) + "\n",
              "every maximum length");

    f = mustParse("[style]\nfont.caption = fonts/ru/savapromedium.fnt\nfont.label = Fonts/Jura.fnt\n"
                  "size.label = 8\nsize.caption_max = 36\nsize.caption = 0\ncolor.ring = 3FD46A\ncolor.mark=3a6288\n",
                  "[style]");
    check(f.style.font[utt::kStyleFontCaption] == "fonts/ru/savapromedium.fnt", "a font path naming a folder");
    check(f.style.font[utt::kStyleFontLabel] == "fonts/jura.fnt", "a font path is lower-cased");
    check(f.style.size[utt::kStyleSizeLabel] == 8 && f.style.size[utt::kStyleSizeCaptionMax] == 36 &&
              f.style.size[utt::kStyleSizeCaption] == 0,
          "sizes at their edges and size.caption = 0");
    check(f.style.color[utt::kStyleColorRing] == 0x3FD46A && f.style.color[utt::kStyleColorMark] == 0x3A6288 &&
              f.style.color[utt::kStyleColorCaption] == -1,
          "colours; an unset one stays -1");
    check(utt::utStyleSizeOr(f.style, utt::kStyleSizeLabel, 13) == 8, "size.label overrides the ini");
    utt::UtTextStyle none;
    check(utt::utStyleSizeOr(none, utt::kStyleSizeLabel, 13) == 13 && utt::utStyleSizeOr(none, utt::kStyleSizeCaptionMax, 14) == 14,
          "unset sizes take the ini's");
    f = mustParse("[style]\nsize.caption = 6\n", "size.caption = 6");
    check(f.style.size[utt::kStyleSizeCaption] == 6, "size.caption fixed");

    // the key grammar
    utt::UtTextKind k;
    std::string arg;
    check(utt::utTextParseKey("label.format.4", &k, &arg) && k == utt::kTextFormat && arg == "4", "label.format.4");
    check(!utt::utTextParseKey("label.format.5", &k, &arg) && !utt::utTextParseKey("label.format.0", &k, &arg) &&
              !utt::utTextParseKey("label.format.", &k, &arg) && !utt::utTextParseKey("label.format.11", &k, &arg),
          "label.format outside 1..4");
    check(!utt::utTextParseKey("caption.", &k, &arg) && !utt::utTextParseKey("caption.a b", &k, &arg) &&
              !utt::utTextParseKey("caption." + rep("x", 41), &k, &arg) && !utt::utTextParseKey("caption.a/b", &k, &arg),
          "caption ids outside the grammar");
    check(utt::utTextParseKey("caption." + rep("x", 40), &k, &arg) && arg.size() == 40, "a 40-character id");
    check(!utt::utTextParseKey("tooltip.scale", &k, &arg), "tooltip.scale is not a key yet");
}

// ---------------------------------------------------------------- every refusal
void testRefusals() {
    mustRefuse("[text]\n; fine\ncaption.head = \xD0\n", 3, "not UTF-8", "a truncated sequence");
    mustRefuse("; caf\xE9 in Latin-1\n", 1, "not UTF-8", "a Latin-1 comment");
    mustRefuse("[text]\ncaption.head = \xED\xA0\x80\n", 2, "not UTF-8", "an encoded surrogate");
    mustRefuse("[text]\ncaption.head = A\x01" "B\n", 2, "control character", "a control character");
    mustRefuse("[text]\nname.head = A\tB\n", 2, "control character", "a tab inside a value");
    mustRefuse("[text]\njust words\n", 2, "key = value", "a line without '='");
    mustRefuse("[text]\n= value\n", 2, "without a key", "a value without a key");
    mustRefuse("caption.head = HEL\n", 1, "outside a section", "a key before any section");
    mustRefuse("[text]\n[texts]\n", 2, "unknown section", "an unknown section");
    mustRefuse("[style.ru]\n", 1, "unknown section", "[style.<lang>] is not a section");
    mustRefuse("[text.]\n", 1, "unknown section", "an empty language");
    mustRefuse("[text.r u]\n", 1, "unknown section", "a blank inside a language");
    mustRefuse("[text\n", 1, "must end with ']'", "an unclosed section header");
    mustRefuse("[text]\ncapton.head = HEL\n", 2, "unknown key", "a misspelt key");
    mustRefuse("[text]\nfont.caption = fonts/a.fnt\n", 2, "unknown key", "a style key under [text]");
    mustRefuse("[style]\ncaption.head = HEL\n", 2, "unknown key", "a text key under [style]");
    mustRefuse("[style]\ntooltip.scale = 150\n", 2, "unknown key", "an experimental key");
    mustRefuse("[text]\ncaption.head = A\n; x\ncaption.head = B\n", 4, "first at line 2", "a key set twice");
    mustRefuse("[text]\ncaption.head = A\n[text]\nCAPTION.HEAD = B\n", 4, "set twice", "a key set twice across a reopened section");
    mustRefuse("[style]\nsize.label = 9\nsize.label = 10\n", 3, "first at line 2", "a style key set twice");
    mustRefuse("[text]\ncaption.head =\n", 2, "empty", "an empty caption");
    mustRefuse("[text]\nname.head = \"\"\n", 2, "empty", "an empty quoted name");
    mustRefuse("[style]\ncolor.label =\n", 2, "empty", "an empty style value");
    mustRefuse("[text]\nname.head = \"abc\n", 2, "unbalanced '\"'", "an open quote");
    mustRefuse("[text]\nname.head = \"\n", 2, "unbalanced '\"'", "a lone quote");
    mustRefuse("[text]\ncaption.head = " + rep(kSha, 13) + "\n", 2, "longer than 12", "13 Cyrillic letters");
    mustRefuse("[text]\ncaption.head = " + rep(kGrin, 6) + "A\n", 2, "longer than 12", "13 units with surrogate pairs");
    mustRefuse("[text]\nname.head = " + rep("n", 49) + "\n", 2, "longer than 48", "a 49-unit name");
    mustRefuse("[text]\ntooltip.yes = " + rep("t", 97) + "\n", 2, "longer than 96", "a 97-unit tooltip");
    mustRefuse("[text]\nlabel.unknown = " + rep("u", 9) + "\n", 2, "longer than 8", "a 9-unit unknown mark");
    mustRefuse("[text]\nlabel.mode_unknown = " + rep("m", 49) + "\n", 2, "longer than 48", "a 49-unit mode prefix");
    mustRefuse("[text]\nlabel.format.1 = " + rep("f", 161) + "\n", 2, "longer than 160", "a 161-unit template");
    mustRefuse("[text]\nlabel.format.2 = {group} {owend}\n", 2, "unknown placeholder {owend}", "an unknown placeholder");
    mustRefuse("[text]\nlabel.format.1 = {Group}\n", 2, "unknown placeholder {Group}", "placeholders are lower case");
    mustRefuse("[text]\nlabel.format.1 = {}\n", 2, "unknown placeholder {}", "an empty placeholder");
    mustRefuse("[text]\nlabel.format.1 = {group\n", 2, "unbalanced '{'", "an open brace");
    mustRefuse("[text]\nlabel.format.1 = {gr{oup}\n", 2, "unbalanced '{'", "a brace inside a placeholder");
    mustRefuse("[text]\nlabel.format.1 = group}\n", 2, "unbalanced '}'", "a lone closing brace");
    const std::string drive = std::string(1, 'C') + ":/fonts/a.fnt";
    const char* const badFonts[] = {"textures/a.fnt", "/fonts/a.fnt", "fonts/../a.fnt", "fonts//a.fnt",
                                    "fonts/a.tex", "fonts/.fnt", "fonts\\a.fnt", "fonts/a*.fnt"};
    for (const char* bf : badFonts)
        mustRefuse(std::string("[style]\nfont.label = ") + bf + "\n", 2, "fonts/<name>.fnt", bf);
    mustRefuse("[style]\nfont.caption = " + drive + "\n", 2, "fonts/<name>.fnt", "a drive path");
    mustRefuse("[style]\nfont.caption = fonts/" + rep("a", 87) + ".fnt\n", 2, "fonts/<name>.fnt", "a 97-byte font path");
    const char* const badColours[] = {"12345", "1234567", "GG0000", "#12345", "##123456", "0x123456", "12 345"};
    for (const char* bc : badColours)
        mustRefuse(std::string("[style]\ncolor.mark = ") + bc + "\n", 2, "six hex digits", bc);
    mustRefuse("[style]\nsize.label = 7\n", 2, "8..32", "size.label 7");
    mustRefuse("[style]\nsize.label = 33\n", 2, "8..32", "size.label 33");
    mustRefuse("[style]\nsize.caption_max = 5\n", 2, "6..36", "size.caption_max 5");
    mustRefuse("[style]\nsize.caption_max = 37\n", 2, "6..36", "size.caption_max 37");
    mustRefuse("[style]\nsize.caption = 5\n", 2, "or 0 (auto)", "size.caption 5");
    mustRefuse("[style]\nsize.caption = -1\n", 2, "whole number", "a negative size");
    mustRefuse("[style]\nsize.label = 12px\n", 2, "whole number", "a size with a unit");
    mustRefuse("[style]\nsize.label = 1e1\n", 2, "whole number", "a size in exponent form");
    mustRefuse("[style]\nsize.label = 99999999999999999999\n", 2, "whole number", "an overflowing size");
    // the line number counts every line, CRLF or not, after a BOM
    mustRefuse("\xEF\xBB\xBF; a\r\n\r\n[text]\r\nbogus\r\n", 4, "key = value", "line numbers with a BOM and CRLF");
}

// ---------------------------------------------------------------- templates
void testTemplates() {
    // printf directives are text, never a format: the file above refused nothing for them
    {
        utt::UtTextFile f = mustParse("[text]\nlabel.format.1 = %s%n%d {group}\n", "printf directives as text");
        utt::UtLabelValues v = utt::utLabelValues(true, "", "Helms", 1, 2, 3, 4, 5, 6, 7, "?");
        std::string out;
        check(utt::utTextExpand(f.common["label.format.1"].text, v, out) && out == "%s%n%d Helms",
              "a '%' is copied, never interpreted");
    }
    // 1.1.0's two printf forms, byte for byte, over 1000 random tuples
    std::mt19937 rng(12345);
    std::size_t nb = 0;
    const utg::BuiltinGroup* bg = utg::builtinGroups(&nb);
    int same = 0;
    for (int t = 0; t < 1000; ++t) {
        const bool modeKnown = rng() % 4 != 0;
        const char* label = bg[rng() % nb].label;
        const int owned = int(rng() % 400) - 40;      // negative = unknown
        const int entries = int(rng() % 400);
        const int first = int(rng() % 60) - 1;
        const int last = int(rng() % 60) - 1;
        const int rows = int(rng() % 60);
        const int all = int(rng() % 4000) - 400;
        const int coll = int(rng() % 4000);
        char ownS[16], allS[16];
        if (owned >= 0) std::snprintf(ownS, sizeof ownS, "%d", owned); else std::strcpy(ownS, "?");
        if (all >= 0) std::snprintf(allS, sizeof allS, "%d", all); else std::strcpy(allS, "?");
        const char* modeS = modeKnown ? "" : "mode unknown - ";
        char full[192], shortForm[192];
        std::snprintf(full, sizeof full, "%s%s - %s/%d owned - row %d-%d/%d - coll %s/%d", modeS, label, ownS,
                      entries, first, last, rows, allS, coll);
        std::snprintf(shortForm, sizeof shortForm, "%s%s - %s/%d - row %d-%d/%d - coll %s/%d", modeS, label, ownS,
                      entries, first, last, rows, allS, coll);
        const utt::UtLabelValues v = utt::utLabelValues(modeKnown, utt::utTextBuiltinPlain(utt::kTextModeUnknown),
                                                        label, owned, entries, first, last, rows, all, coll,
                                                        utt::utTextBuiltinPlain(utt::kTextUnknown));
        std::string a, b;
        if (utt::utTextExpand(utt::utTextDefaultFormat(0), v, a) && a == full &&
            utt::utTextExpand(utt::utTextDefaultFormat(1), v, b) && b == shortForm)
            ++same;
        else if (same == t)
            std::printf("[test] first difference: \"%s\" vs \"%s\"\n", a.c_str(), full);
    }
    check(same == 1000, "the two default templates = 1.1.0's printf output for 1000 tuples (" + std::to_string(same) + ")");

    utt::UtLabelValues v = utt::utLabelValues(true, "x", "Helms", 3, 288, 1, 5, 35, 100, 3288, "?");
    std::string out;
    check(utt::utTextExpand("{group}", v, out) && out == "Helms", "a template with only the group");
    check(utt::utTextExpand("{{{group}}} {{x}}", v, out) && out == "{Helms} {x}", "{{ and }} are braces");
    check(utt::utTextExpand("{owned}{owned}{coll}", v, out) && out == "333288", "a placeholder twice, adjacent ones");
    check(utt::utTextExpand("no counts at all", v, out) && out == "no counts at all", "missing placeholders are fine");
    check(utt::utTextExpand("", v, out) && out.empty(), "an empty template expands to nothing");
    check(!utt::utTextExpand("{group} {extra}", v, out) && out.empty(), "an extra placeholder is refused");
    check(!utt::utTextExpand("{group", v, out) && !utt::utTextExpand("}", v, out), "unbalanced braces are refused");
    std::string why;
    check(!utt::utTextTemplateCheck("{mode}{bogus}", &why) && why.find("{bogus}") != std::string::npos,
          "the refusal names the placeholder");
    for (int i = 0; i < utt::kSlotCount; ++i)
        check(utt::utTextTemplateCheck(std::string("{") + utt::utLabelSlotName(i) + "}", nullptr),
              std::string("placeholder {") + utt::utLabelSlotName(i) + "}");
    v = utt::utLabelValues(false, kShLM, kShlemy, -1, 2, 3, 4, 5, -1, 7, kEuro);
    check(utt::utTextExpand("{mode}{group} {owned}/{all}", v, out) &&
              out == std::string(kShLM) + kShlemy + " " + kEuro + "/" + kEuro,
          "UTF-8 values pass through untouched");
}

// widths: 6 px per ASCII byte, 9 px per other code point
int testWidth(const std::string& s, const void*) {
    std::u32string cps;
    if (!utt::utf8Decode(s, cps)) return 1 << 20;
    int w = 0;
    for (char32_t c : cps) w += c < 0x80 ? 6 : 9;
    return w;
}

void testFit() {
    const std::vector<std::string> lines = {"aaaaaaaaaa", "aaaaa", "aa"};   // 60, 30, 12
    bool fits = false;
    check(utt::utLabelPick(lines, 60, testWidth, nullptr, &fits) == 0 && fits, "pick: the first that fits");
    check(utt::utLabelPick(lines, 59, testWidth, nullptr, &fits) == 1 && fits, "pick: the second");
    check(utt::utLabelPick(lines, 12, testWidth, nullptr, &fits) == 2 && fits, "pick: the last");
    check(utt::utLabelPick(lines, 11, testWidth, nullptr, &fits) == 0 && !fits, "pick: none fits -> the first (1.1.0)");
    check(utt::utLabelPick({}, 100, testWidth, nullptr, &fits) == -1, "pick: no lines");
    // the clip cuts code points, never bytes, and trims a dangling separator
    const std::string ru = std::string(kShlemy) + " - 3/288";
    check(utt::utTextClipToWidth(ru, 45, testWidth, nullptr) == kShlemy, "clip: Cyrillic, then ' -' trimmed");
    check(utt::utTextClipToWidth(ru, 44, testWidth, nullptr) == "\xD0\xA8\xD0\xBB\xD0\xB5\xD0\xBC", "clip: inside the word");
    check(utt::utTextClipToWidth(ru, 8, testWidth, nullptr).empty(), "clip: nothing fits");
    check(utt::utTextClipToWidth("Helms - 3/288", 1000, testWidth, nullptr) == "Helms - 3/288", "clip: fits as is");
    check(utt::utTextClipToWidth("Helms - 3", 6 * 8, testWidth, nullptr) == "Helms", "clip: 'Helms - ' trimmed");
}

// ---------------------------------------------------------------- nothing moved
// 1.1.0's label line, copied from ut_panel.cpp (format, the one concession, textClipToWidth, the
// separator trim) with the same pen rule.
std::string oldLabel(bool modeKnown, const char* label, int owned, int entries, int first, int last,
                     int rows, int all, int coll, int fs, int limitPx) {
    char ownS[16], allS[16];
    if (owned >= 0) std::snprintf(ownS, sizeof ownS, "%d", owned); else std::strcpy(ownS, "?");
    if (all >= 0) std::snprintf(allS, sizeof allS, "%d", all); else std::strcpy(allS, "?");
    const char* modeS = modeKnown ? "" : "mode unknown - ";
    char line[192];
    std::snprintf(line, sizeof line, "%s%s - %s/%d owned - row %d-%d/%d - coll %s/%d", modeS, label, ownS,
                  entries, first, last, rows, allS, coll);
    if (ut::utTextWidthPx(line, fs) > limitPx) {
        char alt[192];
        std::snprintf(alt, sizeof alt, "%s%s - %s/%d - row %d-%d/%d - coll %s/%d", modeS, label, ownS, entries,
                      first, last, rows, allS, coll);
        if (ut::utTextWidthPx(alt, fs) <= limitPx) std::strcpy(line, alt);
    }
    if (ut::utTextWidthPx(line, fs) > limitPx) {
        bool done = false;
        for (int n = (int)std::strlen(line) - 1; n > 0 && !done; --n) {
            const char keep = line[n];
            line[n] = 0;
            if (ut::utTextWidthPx(line, fs) <= limitPx) done = true;
            else line[n] = keep;
        }
        if (!done) line[0] = 0;
    }
    for (int n = (int)std::strlen(line); n > 0 && (line[n - 1] == ' ' || line[n - 1] == '-'); --n) line[n - 1] = 0;
    return line;
}

int tableWidth(const std::string& s, const void* ctx) {
    return ut::utTextWidthPx(s.c_str(), *static_cast<const int*>(ctx));
}

// The same line through the strings file with no file: the chain, the templates, the pick, the
// clip. Must be byte-identical for every group, count, size and width.
std::string newLabel(const utt::UtTextContext& c, const std::string& id, bool modeKnown, int owned,
                     int entries, int first, int last, int rows, int all, int coll, int fs, int limitPx) {
    std::vector<std::string> formats, lines;
    utt::utTextResolveFormats(c, formats, nullptr);
    const utt::UtLabelValues v = utt::utLabelValues(
        modeKnown, utt::utTextResolve(c, utt::kTextModeUnknown, "").text, utt::utTextResolve(c, utt::kTextName, id).text,
        owned, entries, first, last, rows, all, coll, utt::utTextResolve(c, utt::kTextUnknown, "").text);
    for (const std::string& f : formats) {
        std::string out;
        utt::utTextExpand(f, v, out);
        lines.push_back(out);
    }
    bool fits = false;
    const int pick = utt::utLabelPick(lines, limitPx, tableWidth, &fs, &fits);
    return utt::utTextClipToWidth(lines[std::size_t(pick)], limitPx, tableWidth, &fs);
}

void testNothingMoved() {
    std::mt19937 rng(4242);
    std::size_t nb = 0;
    const utg::BuiltinGroup* bg = utg::builtinGroups(&nb);
    nb = kGroups110;   // 1.1.0's 24 groups (the blueprints are 1.2.0's: the G line names them)
    utt::UtTextContext c;
    int same = 0, total = 0, clipped = 0, shortForm = 0, fullForm = 0;
    for (int t = 0; t < 4000; ++t) {
        const utg::BuiltinGroup& g = bg[rng() % nb];
        const bool modeKnown = rng() % 5 != 0;
        const int owned = int(rng() % 200) - 10, entries = int(rng() % 200), first = int(rng() % 40);
        const int last = first + int(rng() % 6), rows = last + int(rng() % 4);
        const int all = int(rng() % 3400) - 100, coll = 3288;
        const int fs = 8 + int(rng() % 25);                  // plate_label_size 8..32
        const int limitPx = 60 + int(rng() % 900);           // the band at every UI scale, and wider
        const std::string o = oldLabel(modeKnown, g.label, owned, entries, first, last, rows, all, coll, fs, limitPx);
        const std::string n = newLabel(c, g.id, modeKnown, owned, entries, first, last, rows, all, coll, fs, limitPx);
        ++total;
        if (o == n) ++same;
        else if (total - same == 1) std::printf("[test] first difference: \"%s\" vs \"%s\"\n", o.c_str(), n.c_str());
        if (o.find(" owned ") == std::string::npos && o.find(" - row ") != std::string::npos) ++shortForm;
        if (o.find("coll ") == std::string::npos) ++clipped;
        if (o.find(" owned - row ") != std::string::npos && o.find("coll ") != std::string::npos) ++fullForm;
    }
    std::printf("[test] label: %d/%d lines identical to 1.1.0 (%d full, %d short, %d clipped)\n", same, total,
                fullForm, shortForm, clipped);
    check(same == total && fullForm > 100 && shortForm > 100 && clipped > 100,
          "no file: the label is 1.1.0's byte for byte (" + std::to_string(same) + "/" + std::to_string(total) +
              ", " + std::to_string(shortForm) + " short forms, " + std::to_string(clipped) + " clipped)");
    // the widest line of the catalogue at UI scale 1.000 (367 px against 317, 1.1.0's comment)
    const int fs13 = 13;
    const std::string wide = newLabel(c, "offhand", true, 172, 172, 31, 35, 35, 3288, 3288, fs13, 317);
    check(wide == oldLabel(true, "Off-hands", 172, 172, 31, 35, 35, 3288, 3288, fs13, 317),
          "the widest line at 1.000 (" + wide + ")");
}

// ---------------------------------------------------------------- the chain
struct Font {
    bool cyrillic[3];   // per UtTextFont: the font has U+0400..U+04FF
};
bool glyphOf(utt::UtTextFont font, char32_t cp, const void* ctx) {
    const Font* f = static_cast<const Font*>(ctx);
    if (cp >= 0x20 && cp <= 0x7E) return true;
    if (cp >= 0x400 && cp <= 0x4FF) return f->cyrillic[font];
    return false;
}
struct Tags {
    int calls = 0;
};
bool tagOf(utt::UtTextKind kind, const std::string& id, std::string* out, const void* ctx) {
    ++const_cast<Tags*>(static_cast<const Tags*>(ctx))->calls;
    if (kind == utt::kTextName && id == "head") { *out = kShlemy; return true; }
    if (kind == utt::kTextName && id == "chest") { *out = rep(kSha, 49); return true; }   // too long
    if (kind == utt::kTextName && id == "feet") { *out = kZhong; return true; }           // no glyph
    return false;
}

void testChain() {
    utt::UtTextContext c;   // no file, English
    std::size_t n = 0;
    const utt::UtTextBuiltin* b = utt::utTextBuiltins(&n);
    bool all = true;
    for (std::size_t i = 0; i < n; ++i) {
        const utt::UtTextPick cap = utt::utTextResolve(c, utt::kTextCaption, b[i].id);
        const utt::UtTextPick name = utt::utTextResolve(c, utt::kTextName, b[i].id);
        if (cap.text != b[i].caption || cap.source != utt::kSrcBuiltin || name.text != b[i].name ||
            name.source != utt::kSrcBuiltin || !cap.skipped.empty())
            all = false;
    }
    check(all, "no file: every built-in caption and name");
    check(utt::utTextResolve(c, utt::kTextTooltipYes, "").text == "In your collection" &&
              utt::utTextResolve(c, utt::kTextTooltipNo, "").text == "Not in your collection" &&
              utt::utTextResolve(c, utt::kTextModeUnknown, "").text == "mode unknown - " &&
              utt::utTextResolve(c, utt::kTextUnknown, "").text == "?",
          "no file: the tooltip lines and the label words");
    std::vector<std::string> fm;
    std::string skipped;
    check(utt::utTextResolveFormats(c, fm, &skipped) == utt::kSrcBuiltin && fm.size() == 2 &&
              fm[0] == utt::utTextDefaultFormat(0) && fm[1] == utt::utTextDefaultFormat(1),
          "no file: 1.1.0's two templates");

    // an id nothing names
    utt::UtTextPick p = utt::utTextResolve(c, utt::kTextCaption, "rot.runeword");
    check(p.text == "RUN" && p.source == utt::kSrcDerived, "derived caption RUN for rot.runeword");
    p = utt::utTextResolve(c, utt::kTextName, "rot.rune_word");
    check(p.text == "Rune word" && p.source == utt::kSrcDerived, "derived name");
    check(utt::utTextDerivedCaption("x.a1") == "A1X" && utt::utTextDerivedCaption("x.--") == "XXX",
          "derived caption padding");
    // the category's own text (the G line) for an id the built-ins do not know
    p = utt::utTextResolve(c, utt::kTextCaption, "auto.itemrelic", "CMP");
    check(p.text == "CMP" && p.source == utt::kSrcCategory, "the category's tag");
    p = utt::utTextResolve(c, utt::kTextName, "head", "Helms");
    check(p.source == utt::kSrcBuiltin, "a category text equal to the built-in one is not a category source");
    p = utt::utTextResolve(c, utt::kTextName, "head", "Hats");
    check(p.text == "Hats" && p.source == utt::kSrcCategory, "a renamed group keeps its groups-file label");

    // the file: [text.<lang>] over [text] over the rest - the section in force is the one the
    // file's [style] language names, never the game's language
    const std::string chainText = std::string("[text]\ncaption.head = HAT\nname.head = Hats\ntooltip.yes = Got it\n"
                                              "[text.ru]\ncaption.head = ") + kShLM + "\nname.feet = " + kShlemy + "\n";
    utt::UtTextFile f0 = mustParse(chainText, "a chain file without [style] language");
    c.file = &f0;
    c.language = "ru";
    c.english = false;
    p = utt::utTextResolve(c, utt::kTextCaption, "head");
    check(p.text == "HAT" && p.source == utt::kSrcCommon,
          "no [style] language: [text.ru] is not applied, even on a game running in Russian");
    check(utt::utTextLangSectionOf(&f0) == nullptr, "no [style] language: no language section in force");
    utt::UtTextFile f = mustParse(chainText + "[style]\nlanguage = RU\n", "a chain file with [style] language = RU");
    check(f.style.language == "ru", "[style] language is kept lower-cased");
    c.file = &f;
    c.language = "english";
    c.english = true;
    p = utt::utTextResolve(c, utt::kTextCaption, "head");
    check(p.text == kShLM && p.source == utt::kSrcLanguage, "[style] language = RU applies [text.ru] on an English game");
    c.language = "de";
    c.english = false;
    p = utt::utTextResolve(c, utt::kTextCaption, "head");
    check(p.text == kShLM && p.source == utt::kSrcLanguage, "... and on a German one: the file decides, not the game");
    p = utt::utTextResolve(c, utt::kTextName, "head", "Helmets");
    check(p.text == "Hats" && p.source == utt::kSrcCommon, "[text] when [text.ru] lacks the key, before the category");
    p = utt::utTextResolve(c, utt::kTextTooltipYes, "");
    check(p.text == "Got it" && p.source == utt::kSrcCommon, "tooltip.yes from [text]");
    utt::UtTextFile fde = mustParse(chainText + "[style]\nlanguage = de\n", "a chain file naming [text.de]");
    c.file = &fde;
    c.language = "ru";
    check(utt::utTextResolve(c, utt::kTextCaption, "head").text == "HAT",
          "[style] language naming a section the file lacks: [text] (a Russian game changes nothing)");
    c.file = &f;
    c.language = "";
    check(utt::utTextResolve(c, utt::kTextCaption, "head").text == kShLM, "an unknown game language: the same");

    // the one log line per file load about the language sections
    bool warn = true;
    std::string note = utt::utTextSectionNote(f0, &warn);
    check(!warn && note.find("[text.ru] is not applied") == 0 && note.find("[style] language is unset") != std::string::npos,
          "no language key: one DEBUG line names [text.ru] (" + note + ")");
    note = utt::utTextSectionNote(fde, &warn);
    check(warn && note.find("[style] language = de names no [text.de] section") == 0 &&
              note.find("[text.ru] is not applied") != std::string::npos,
          "a language naming a missing section: a WARN (" + note + ")");
    note = utt::utTextSectionNote(f, &warn);
    check(!warn && note.empty(), "the one section, named: nothing to say");
    utt::UtTextFile f2 = mustParse(chainText + "[text.de]\ncaption.head = HLM\n[style]\nlanguage = ru\n", "two sections");
    note = utt::utTextSectionNote(f2, &warn);
    check(!warn && note == "[text.de] is not applied - [style] language = ru applies [text.ru] only",
          "another section beside the named one: DEBUG (" + note + ")");
    mustRefuse("[style]\nlanguage = r u\n", 2, "\"language\" must name a [text.<language>] section", "a language with a blank");
    mustRefuse("[style]\nlanguage = abcdefghijklmnopq\n", 2, "\"language\"", "a language of 17 letters");

    // the glyph check: an ASCII-only caption font passes the Cyrillic caption over
    Font ascii = {{false, false, false}};
    Font ruCaption = {{true, false, false}};
    c.language = "ru";
    c.glyph = glyphOf;
    c.glyphCtx = &ascii;
    p = utt::utTextResolve(c, utt::kTextCaption, "head");
    check(p.text == "HAT" && p.source == utt::kSrcCommon, "a missing glyph: the chain goes on to [text]");
    check(p.skipped.find("caption.head") != std::string::npos && p.skipped.find("U+0428") != std::string::npos &&
              p.skipped.find("caption font") != std::string::npos,
          "the skip names the key, the code point and the font (" + p.skipped + ")");
    c.glyphCtx = &ruCaption;
    p = utt::utTextResolve(c, utt::kTextCaption, "head");
    check(p.text == kShLM && p.skipped.empty(), "a caption font with Cyrillic draws it");
    p = utt::utTextResolve(c, utt::kTextName, "feet");
    check(p.text == "Boots" && p.source == utt::kSrcBuiltin && p.skipped.find("label font") != std::string::npos,
          "the name is checked against the LABEL font");

    // the game's own words: ONLY behind @game, in any game language; never automatic
    Tags tags;
    utt::UtTextContext g;
    g.tag = tagOf;
    g.tagCtx = &tags;
    g.english = false;
    g.language = "russian";
    p = utt::utTextResolve(g, utt::kTextName, "head");
    check(p.text == "Helms" && p.source == utt::kSrcBuiltin && tags.calls == 0,
          "a Russian game, no file: the built-in English, the game's tag never asked");
    p = utt::utTextResolve(g, utt::kTextName, "head", "Helms");
    check(p.text == "Helms" && tags.calls == 0, "... with the groups file's default label too");
    utt::UtTextFile gf = mustParse("[text]\nname.head = @game\nname.chest = @game\n", "name.<id> = @game");
    g.file = &gf;
    p = utt::utTextResolve(g, utt::kTextName, "head");
    check(p.text == kShlemy && p.source == utt::kSrcGameTag && tags.calls == 1, "name.head = @game: the game's own word");
    g.english = true;
    g.language = "english";
    p = utt::utTextResolve(g, utt::kTextName, "head");
    check(p.text == kShlemy && p.source == utt::kSrcGameTag && tags.calls == 2,
          "@game on an English game too: the file decides");
    p = utt::utTextResolve(g, utt::kTextName, "chest");
    check(p.text == "Chest" && p.source == utt::kSrcBuiltin && p.skipped.find("longer than 48") != std::string::npos,
          "an over-long game word is passed over");
    tags.calls = 0;
    p = utt::utTextResolve(g, utt::kTextName, "feet");
    check(p.text == "Boots" && p.source == utt::kSrcBuiltin && tags.calls == 0 && p.skipped.empty(),
          "a name without @game: the game is not asked");
    utt::UtTextFile gw = mustParse("[text]\nname.* = @game\nname.legs = Pants\n", "name.* = @game");
    g.file = &gw;
    p = utt::utTextResolve(g, utt::kTextName, "head");
    check(p.text == kShlemy && p.source == utt::kSrcGameTag, "name.* = @game: the game's word for head");
    p = utt::utTextResolve(g, utt::kTextName, "legs");
    check(p.text == "Pants" && p.source == utt::kSrcCommon, "name.legs beats name.* in its section");
    p = utt::utTextResolve(g, utt::kTextName, "dagger");
    check(p.text == "Daggers" && p.source == utt::kSrcBuiltin && p.skipped.empty(),
          "no exact tag: the built-in English, and no WARN for it");
    p = utt::utTextResolve(g, utt::kTextName, "rot.charm", "Charms");
    check(p.text == "Charms" && p.source == utt::kSrcCategory, "a mod's group under name.*: its own label");
    p = utt::utTextResolve(g, utt::kTextCaption, "head");
    check(p.text == "HEL" && p.source == utt::kSrcBuiltin, "never for a caption");
    utt::UtTextFile gl = mustParse(std::string("[text]\nname.head = Hats\n[text.ru]\nname.* = @game\n[style]\nlanguage = ru\n"),
                                   "@game in the language section");
    g.file = &gl;
    p = utt::utTextResolve(g, utt::kTextName, "head");
    check(p.text == kShlemy && p.source == utt::kSrcGameTag, "[text.ru] name.* = @game beats [text] name.head");
    p = utt::utTextResolve(g, utt::kTextName, "dagger");
    check(p.text == "Daggers", "... and without a tag the chain goes on");
    g.file = &gw;
    g.glyph = glyphOf;
    Font ruLabel = {{false, true, false}};
    g.glyphCtx = &ruLabel;
    p = utt::utTextResolve(g, utt::kTextName, "feet");
    check(p.text == "Boots" && p.skipped.find("U+4E2D") != std::string::npos, "a game word the font cannot draw");
    g.glyphCtx = &ascii;
    p = utt::utTextResolve(g, utt::kTextName, "head");
    check(p.text == "Helms" && p.source == utt::kSrcBuiltin, "@game on the root font: back to English");
    mustRefuse("[text]\ncaption.head = @game\n", 2, "@game is for name.<id> and name.* only", "@game for a caption");
    mustRefuse("[text]\ntooltip.yes = @game\n", 2, "@game is for name.<id>", "@game for a rollover line");
    mustRefuse("[text]\nname.* = Things\n", 2, "\"name.*\" takes only @game", "name.* with a word");
    mustRefuse("[text]\ncaption.* = @game\n", 2, "unknown key", "caption.*");

    // the tooltip lines are checked against the tooltip font
    utt::UtTextFile tf = mustParse(std::string("[text]\ntooltip.no = ") + kShlemy + "\n", "a Cyrillic tooltip");
    utt::UtTextContext t;
    t.file = &tf;
    t.glyph = glyphOf;
    Font ruTip = {{false, false, true}};
    t.glyphCtx = &ruTip;
    check(utt::utTextResolve(t, utt::kTextTooltipNo, "").text == kShlemy, "the tooltip font has the letters");
    t.glyphCtx = &ruCaption;
    p = utt::utTextResolve(t, utt::kTextTooltipNo, "");
    check(p.text == "Not in your collection" && p.skipped.find("tooltip font") != std::string::npos,
          "the tooltip font lacks them");

    // a glyph callback that refuses even ASCII: the built-in text all the same
    struct None { static bool fn(utt::UtTextFont, char32_t, const void*) { return false; } };
    utt::UtTextContext z;
    z.glyph = None::fn;
    check(utt::utTextResolve(z, utt::kTextCaption, "head").text == "HEL", "nothing drawable: still the built-in");
    check(utt::utTextResolve(z, utt::kTextCaption, "rot.charm").text == "CHA", "nothing drawable: still the derived");
    check(utt::utTextResolve(z, utt::kTextModeUnknown, "").text == "mode unknown - ", "nothing drawable: mode prefix");

    // an empty mode prefix from the file is kept
    utt::UtTextFile mf = mustParse("[text]\nlabel.mode_unknown = \"\"\n", "an empty mode prefix");
    utt::UtTextContext m;
    m.file = &mf;
    p = utt::utTextResolve(m, utt::kTextModeUnknown, "");
    check(p.text.empty() && p.source == utt::kSrcCommon, "an empty mode prefix");

    // the template list is taken whole from the most specific section
    utt::UtTextFile lf = mustParse(std::string("[text]\nlabel.format.1 = A {group}\nlabel.format.3 = C\n"
                                               "[text.ru]\nlabel.format.2 = ") + kShlemy + " {group}\n",
                                   "template lists");
    utt::UtTextContext l;
    l.file = &lf;
    skipped.clear();
    check(utt::utTextResolveFormats(l, fm, &skipped) == utt::kSrcCommon && fm.size() == 2 && fm[0] == "A {group}" &&
              fm[1] == "C",
          "[text]: .1 and .3, the gap skipped");
    l.language = "ru";
    l.english = false;
    check(utt::utTextResolveFormats(l, fm, &skipped) == utt::kSrcCommon,
          "a Russian game without [style] language: still the [text] list");
    utt::UtTextFile lfr = mustParse(std::string("[text]\nlabel.format.1 = A {group}\nlabel.format.3 = C\n"
                                                "[text.ru]\nlabel.format.2 = ") + kShlemy + " {group}\n[style]\nlanguage = ru\n",
                                    "template lists with [style] language = ru");
    l.file = &lfr;
    check(utt::utTextResolveFormats(l, fm, &skipped) == utt::kSrcLanguage && fm.size() == 1 &&
              fm[0] == std::string(kShlemy) + " {group}",
          "[text.ru]: its own list replaces the whole [text] list");
    l.glyph = glyphOf;
    l.glyphCtx = &ascii;
    skipped.clear();
    check(utt::utTextResolveFormats(l, fm, &skipped) == utt::kSrcCommon && fm.size() == 2 &&
              skipped.find("U+0428") != std::string::npos,
          "a template list the label font cannot draw is passed over whole");
    utt::UtTextFile bf = mustParse("[text]\nlabel.format.1 = {group}\n", "a placeholder-only template");
    struct NoCurly { static bool fn(utt::UtTextFont, char32_t cp, const void*) { return cp != '{' && cp != '}'; } };
    utt::UtTextContext nc;
    nc.file = &bf;
    nc.glyph = NoCurly::fn;
    check(utt::utTextResolveFormats(nc, fm, nullptr) == utt::kSrcCommon, "placeholders are not glyphs");

    // distinct derived captions, duplicates reported
    std::vector<std::string> caps = {"MAT", "RUN", "RUN", "RUN", "CHA"};
    std::vector<bool> derived = {false, false, true, true, true};
    utt::utTextDistinctDerived(caps, derived);
    check(caps[1] == "RUN" && caps[2] == "RU0" && caps[3] == "RU1" && caps[4] == "CHA", "derived duplicates get a digit");
    caps = {"AB0", "ABC", "ABC"};
    derived = {false, true, false};
    utt::utTextDistinctDerived(caps, derived);
    check(caps[1] == "AB1" && caps[2] == "ABC", "a derived caption yields to a later user caption and skips a taken digit");
    const auto d = utt::utTextDuplicates({"HEL", "X", "HEL", "X", ""});
    check(d.size() == 2 && d[0].first == 0 && d[0].second == 2 && d[1].first == 1 && d[1].second == 3,
          "duplicate captions are reported, never fatal");
}

// ---------------------------------------------------------------- the built-in table
void testBuiltins() {
    std::size_t n = 0, ng = 0;
    const utt::UtTextBuiltin* b = utt::utTextBuiltins(&n);
    const utg::BuiltinGroup* g = utg::builtinGroups(&ng);
    check(ng == kGroups110 + 1 && std::string(g[kGroups110].id) == "blueprint",
          "the groups' built-in table: 1.1.0's 24, then the blueprints");
    check(n == kGroups110 + 3 && utt::kTextPadButtons == kGroups110 + 2, "27 built-ins: the 26 pad buttons, other");
    ng = kGroups110;
    bool noNext = true;
    for (std::size_t i = 0; i < n; ++i) noNext = noNext && std::string(b[i].id) != "next";
    check(noNext && utt::utTextBuiltinById("next") == utt::utTextBuiltinNext() &&
              std::string(utt::utTextBuiltinNext()->caption) == ">>",
          "caption.next: NEXT's '>>', outside the 27-row table");
    check(std::string(b[0].id) == "vanilla" && std::string(b[0].caption) == "MAT" &&
              std::string(b[0].name) == "Crafting Materials",
          "button 0: the game's materials page");
    bool same = true;
    for (std::size_t i = 0; i < ng && i + 1 < n; ++i)
        if (std::string(b[i + 1].id) != g[i].id || std::string(b[i + 1].caption) != g[i].tag ||
            std::string(b[i + 1].name) != g[i].label)
            same = false;
    check(same, "buttons 1..24 = the groups file's built-in ids, tags and labels, in order");
    // the blueprints have no row of their own here: their caption and name are the G line's
    utt::UtTextContext en;
    check(utt::utTextBuiltinById("blueprint") == nullptr &&
              utt::utTextResolve(en, utt::kTextCaption, "blueprint", "BLU").text == "BLU" &&
              utt::utTextResolve(en, utt::kTextName, "blueprint", "Blueprints").text == "Blueprints",
          "Blueprints / BLU on the pad: the G line's tag and label (the category)");
    check(std::string(b[25].id) == "owned" && std::string(b[25].caption) == "OWN", "button 25: the owned filter");
    check(std::string(b[26].id) == "other" && n == 27 && utt::utTextBuiltinById("retired") == nullptr,
          "the generator's own group, Other; no Retired");
    std::vector<std::string> caps;
    bool grammar = true;
    for (std::size_t i = 0; i < n; ++i) {
        caps.push_back(b[i].caption);
        if (!utt::utTextValidId(b[i].id)) grammar = false;
        std::string why;
        if (!utt::utTextValueOk(utt::kTextCaption, b[i].caption, &why) || !utt::utTextValueOk(utt::kTextName, b[i].name, &why))
            grammar = false;
    }
    check(grammar, "every built-in id, caption and name is valid");
    check(utt::utTextDuplicates(caps).empty(), "the built-in captions are distinct");
    check(utg::reservedTag("MAT") && utg::reservedTag("OWN") && utg::reservedId("vanilla") && utg::reservedId("owned") &&
              utg::reservedId("other") && !utg::reservedId("retired"),
          "the pad's own ids and tags are the groups file's reserved ones");
    check(utt::utTextValidId("auto.itemrelic") && utt::utTextValidId("reignofterror.runeword") &&
              !utt::utTextValidId("Head") && !utt::utTextValidId(""),
          "the id grammar is the groups file's");
    std::string s1 = "a.b-c_9", s2 = std::string(41, 'a');
    check(utt::utTextValidId(s1) == utg::validId(s1) && utt::utTextValidId(s2) == utg::validId(s2), "the same grammar");
}

// ---------------------------------------------------------------- reload
void testReload() {
    utt::UtFileStamp none, missing, a, b, c;
    missing.known = true;
    a.known = b.known = c.known = true;
    a.exists = b.exists = c.exists = true;
    a.mtime = 100; a.size = 10;
    b.mtime = 200; b.size = 10;
    c.mtime = 100; c.size = 11;
    check(utt::utTextChanged(none, missing), "reload: the first look always reads");
    check(!utt::utTextChanged(missing, missing), "reload: still missing is no change");
    check(utt::utTextChanged(missing, a), "reload: the file appears");
    check(utt::utTextChanged(a, missing), "reload: the file goes away");
    check(!utt::utTextChanged(a, a), "reload: same mtime and size");
    check(utt::utTextChanged(a, b), "reload: a new mtime");
    check(utt::utTextChanged(a, c), "reload: a new size with the same mtime");
}

// ---------------------------------------------------------------- the template and its writer
std::string readAll(const std::string& path) {
    std::string s;
    if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
        char buf[4096];
        std::size_t r;
        while ((r = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, r);
        std::fclose(f);
    }
    return s;
}

// Every "; key = value" and "; [section]" line of the template, uncommented.
std::string uncommentAll(const std::string& t) {
    std::string out, line;
    std::size_t pos = 0;
    while (pos < t.size()) {
        std::size_t nl = t.find('\n', pos);
        if (nl == std::string::npos) nl = t.size();
        line = t.substr(pos, nl - pos);
        pos = nl + 1;
        if (line.compare(0, 2, "; ") == 0) {
            const std::string rest = line.substr(2);
            const std::size_t eq = rest.find(" = ");
            utt::UtTextKind k;
            std::string arg;
            const std::string key = eq == std::string::npos ? std::string() : rest.substr(0, eq);
            const bool textKey = !key.empty() && utt::utTextParseKey(key, &k, &arg);
            const bool styleKey = !key.empty() && utt::utStyleDefaultText(key) != nullptr;
            if ((rest.size() > 2 && rest[0] == '[' && rest.back() == ']') || textKey || styleKey) line = rest;
        }
        out += line + "\n";
    }
    return out;
}

void testPurity(const std::string& header) {
    const std::string src = readAll(header);
    check(!src.empty(), "purity: the header is readable (" + header + ")");
    static const char* const kAllowed[] = {"<cerrno>", "<cstddef>", "<cstdint>", "<cstdio>", "<cstdlib>",
                                           "<map>", "<string>", "<utility>", "<vector>"};
    std::size_t pos = 0;
    int includes = 0;
    bool pure = true;
    while ((pos = src.find("#include", pos)) != std::string::npos) {
        const std::size_t eol = src.find('\n', pos);
        const std::string what = utt::utTextTrim(src.substr(pos + 8, eol - pos - 8));
        bool ok = false;
        for (const char* a : kAllowed) if (what == a) ok = true;
        if (!ok) { pure = false; std::printf("[test] not a standard header: %s\n", what.c_str()); }
        ++includes;
        pos = eol;
    }
    check(pure && includes == 9, "purity: standard headers only (" + std::to_string(includes) + ")");
    bool ascii = true;
    for (unsigned char ch : src) if (ch > 0x7E || (ch < 0x20 && ch != '\n' && ch != '\r')) ascii = false;
    check(ascii, "purity: the header is plain ASCII (any compiler code page)");
}

// Random files from troublesome pieces: the parser never fails without naming a line, and a
// refused file leaves nothing behind.
void testFuzz() {
    static const char* const kPieces[] = {
        "[text]", "[style]", "[text.ru]", "[bogus]", "[", "]", "\n", "\r\n", "\n", " ", "=", ";", "#", "\"",
        "caption.head", "name.head", "label.format.1", "label.format.4", "tooltip.yes", "size.label",
        "color.mark", "font.label", "{group}", "{", "}", "{{", "13", "3FD46A", "fonts/a.fnt", "\xD0\xA8",
        "\xF0\x9F\x98\x80", "\xD0", "\x80", "\xEF\xBB\xBF", "\t", "HEL",
    };
    const std::size_t nPieces = sizeof kPieces / sizeof *kPieces;
    std::mt19937 rng(777);
    int accepted = 0, refused = 0;
    bool named = true, clean = true;
    for (int t = 0; t < 3000; ++t) {
        std::string text;
        const int len = int(rng() % 40);
        for (int i = 0; i < len; ++i) text += kPieces[rng() % nPieces];
        utt::UtTextFile f;
        std::string err;
        if (utt::utTextParse(text, f, &err)) {
            ++accepted;
        } else {
            ++refused;
            if (err.compare(0, 5, "line ") != 0) named = false;
            if (f.keys || !f.common.empty() || !f.lang.empty()) clean = false;
        }
    }
    check(named && clean && accepted > 0 && refused > 0,
          "fuzz: 3000 files, " + std::to_string(accepted) + " accepted, " + std::to_string(refused) +
              " refused, every refusal names its line");
}

} // namespace

// ---------------------------------------------------------------- the live chain's pure half
// Text_RU's words for exact tags (Helms, Chest, Feet, two-handed ranged), one with a synthetic
// gender marker, and none for the rest.
const char* const kRuShlemy = "\xD0\xA8\xD0\xBB\xD0\xB5\xD0\xBC\xD1\x8B";
const char* const kRuNagrud = "\xD0\x9D\xD0\xB0\xD0\xB3\xD1\x80\xD1\x83\xD0\xB4\xD0\xBD\xD0\xB0\xD1\x8F\x20"
                              "\xD0\xB1\xD1\x80\xD0\xBE\xD0\xBD\xD1\x8F";
const char* const kRuObuv = "\xD0\x9E\xD0\xB1\xD1\x83\xD0\xB2\xD1\x8C";
const char* const kRuRuzhya = "\xD0\xA0\xD1\x83\xD0\xB6\xD1\x8C\xD1\x8F\x20\xD0\xB8\x20\xD0\xB0\xD1\x80\xD0\xB1"
                              "\xD0\xB0\xD0\xBB\xD0\xB5\xD1\x82\xD1\x8B";
const char* const kRuPonozhi = "\xD0\x9F\xD0\xBE\xD0\xBD\xD0\xBE\xD0\xB6\xD0\xB8";
const char* const kRuNGR = "\xD0\x9D\xD0\x93\xD0\xA0";
const char* const kRuSobrano = "\xD1\x81\xD0\xBE\xD0\xB1\xD1\x80\xD0\xB0\xD0\xBD\xD0\xBE";
const char* const kRuRyad = "\xD1\x80\xD1\x8F\xD0\xB4";
const char* const kRuVsego = "\xD0\xB2\xD1\x81\xD0\xB5\xD0\xB3\xD0\xBE";

struct RuGame {
    int calls = 0;
};
bool ruLocalize(const char* tag, std::string* out, const void* ctx) {
    ++const_cast<RuGame*>(static_cast<const RuGame*>(ctx))->calls;
    const std::string t = tag;
    if (t == "tagCraftingHelms") *out = kRuShlemy;
    else if (t == "tagCraftingTorso") *out = kRuNagrud;
    else if (t == "tagCraftingFeet") *out = kRuObuv;
    else if (t == "tagCraftingTwoHanded") *out = kRuRuzhya;
    else if (t == "tagCraftingLegs") *out = std::string("[np]") + kRuPonozhi + " [fs]x";
    else return false;
    return true;
}
RuGame g_ruGame;
bool ruTag(utt::UtTextKind kind, const std::string& id, std::string* out, const void*) {
    return utt::utTextGameWord(kind, id, ruLocalize, &g_ruGame, out);
}

void testGameWords() {
    const char* const exact[] = {"head", "shoulders", "chest", "hands", "waist", "legs",
                                 "feet", "shield", "spear2h", "ranged1h", "ranged2h"};
    const char* const inexact[] = {"offhand", "axe1h", "axe2h", "mace1h", "mace2h", "sword1h", "sword2h",
                                   "dagger", "scepter", "amulet", "medal", "ring", "relic", "vanilla",
                                   "owned", "next", "rot.runeword"};
    bool ok = true;
    for (const char* id : exact) ok = ok && utt::utTextGameTagFor(id) && utt::utTextBuiltinById(id);
    check(ok, "the eleven exact tags, each a built-in id");
    ok = true;
    for (const char* id : inexact) ok = ok && !utt::utTextGameTagFor(id);
    check(ok, "no tag where the game's word is not exact (FOCI, AXES, BLUNT, SWORDS, IMPLEMENTS, singulars)");
    check(std::string(utt::utTextGameTagFor("head")) == "tagCraftingHelms" &&
              std::string(utt::utTextGameTagFor("chest")) == "tagCraftingTorso" &&
              std::string(utt::utTextGameTagFor("ranged2h")) == "tagCraftingTwoHanded",
          "head, chest and 2H ranged map to their crafting tags");
    // gender markers
    const std::string shlem = "\xD1\x88\xD0\xBB\xD0\xB5\xD0\xBC";
    check(utt::utTextStripGenderMarkers("[ms]" + shlem) == shlem, "a leading [ms] goes");
    check(utt::utTextStripGenderMarkers(std::string("[ms]") + kRuNagrud + " [fs]" + kRuObuv) == kRuNagrud,
          "of [ms]A [fs]B only A stays");
    check(utt::utTextStripGenderMarkers(std::string("[np]") + kRuPonozhi + "[fs]x") == kRuPonozhi,
          "a second marker without a blank ends the first alternative");
    check(utt::utTextStripGenderMarkers("  HELMS ") == "HELMS" && utt::utTextStripGenderMarkers(kRuShlemy) == kRuShlemy,
          "a word without markers is kept (trimmed)");
    check(utt::utTextStripGenderMarkers("[x]a") == "[x]a" && utt::utTextStripGenderMarkers("[MS]a") == "[MS]a" &&
              utt::utTextStripGenderMarkers("[ms]").empty(),
          "only [two lower-case letters] is a marker");
    // the game word is for NAMES only
    std::string w;
    g_ruGame.calls = 0;
    check(utt::utTextGameWord(utt::kTextName, "legs", ruLocalize, &g_ruGame, &w) && w == kRuPonozhi,
          "the game's word for a name, markers stripped");
    check(!utt::utTextGameWord(utt::kTextCaption, "head", ruLocalize, &g_ruGame, &w) &&
              !utt::utTextGameWord(utt::kTextName, "dagger", ruLocalize, &g_ruGame, &w) &&
              !utt::utTextGameWord(utt::kTextName, "shoulders", ruLocalize, &g_ruGame, &w) && g_ruGame.calls == 2,
          "never for a caption; no tag, nothing asked; a tag the game lacks, nothing");
}

// 1.1.0's 24 groups (head .. relic), the built-in table's first rows.
std::vector<utt::UtTextGroupIn> builtinTable() {
    std::size_t ng = 0;
    const utg::BuiltinGroup* g = utg::builtinGroups(&ng);
    if (ng > kGroups110) ng = kGroups110;
    std::vector<utt::UtTextGroupIn> in;
    for (std::size_t i = 0; i < ng; ++i) in.push_back({g[i].id, g[i].tag, g[i].label});
    return in;
}

void testLivePad() {
    // English, no file: the pad is the G lines' tags and the label names are the G lines' labels.
    const std::vector<utt::UtTextGroupIn> in = builtinTable();
    utt::UtTextContext en;
    en.tag = ruTag;   // never asked in English
    g_ruGame.calls = 0;
    utt::UtTextPad p = utt::utTextResolvePad(en, in);
    bool same = p.caps.size() == in.size() + 3 && p.names.size() == in.size() && p.caps[0] == "MAT" &&
                p.caps[in.size() + 1] == "OWN" && p.caps[in.size() + 2] == ">>";
    for (std::size_t i = 0; same && i < in.size(); ++i) same = p.caps[i + 1] == in[i].tag && p.names[i] == in[i].label;
    check(same && p.skipped.empty() && g_ruGame.calls == 0,
          "English, no file: MAT, the 24 tags, OWN, >>; the names are the labels");
    // The identity gate: a game in another language with no file draws the same built-in
    // English - the game's words are never asked, whatever its fonts have
    utt::UtTextContext rn;
    rn.language = "russian";
    rn.english = false;
    rn.tag = ruTag;
    rn.glyph = glyphOf;
    Font cyrAll = {{true, true, true}};
    rn.glyphCtx = &cyrAll;
    g_ruGame.calls = 0;
    const utt::UtTextPad pr = utt::utTextResolvePad(rn, in);
    bool sameRu = pr.caps == p.caps && pr.names == p.names && pr.skipped.empty() && g_ruGame.calls == 0;
    for (std::size_t i = 0; sameRu && i < pr.names.size(); ++i) sameRu = pr.nameSrc[i] != utt::kSrcGameTag;
    std::vector<std::string> fmRu;
    sameRu = sameRu && utt::utTextResolveFormats(rn, fmRu, nullptr) == utt::kSrcBuiltin &&
             utt::utTextResolve(rn, utt::kTextTooltipYes, "").text == "In your collection";
    check(sameRu, "a Russian game, no file: the same built-in English pad, names, templates and rollover lines; "
                  "the game's words never asked");
    // a custom game's table: the category's own tag and label; a derived caption for a tagless id
    std::vector<utt::UtTextGroupIn> rot = in;
    rot.push_back({"reignofterror.runeword", "RUN", "Runewords"});
    rot.push_back({"reignofterror.charm", "", "Charms"});
    rot.push_back({"retired", "RET", "Retired"});
    p = utt::utTextResolvePad(en, rot);
    check(p.caps[25] == "RUN" && p.names[24] == "Runewords" && p.capSrc[25] == utt::kSrcCategory &&
              p.caps[26] == "CHA" && p.capSrc[26] == utt::kSrcDerived && p.caps[27] == "RET" && p.caps.back() == ">>",
          "a mod's groups: the G line's tag and label; a missing tag derived from the id");

    // [text.ru] by [style] language = ru: Cyrillic captions and names through the glyph callback,
    // the game's own words where it says name.* = @game
    const std::string ruText = std::string("[text]\ncaption.next = NXT\nname.relic = Relics!\n"
                                           "[text.ru]\nname.* = @game\ncaption.head = ") + kShLM + "\ncaption.chest = " +
                               kRuNGR + "\nname.dagger = " + kRuObuv + "\ncaption.next = " + kRuNGR + "\n";
    utt::UtTextFile f = mustParse(ruText + "[style]\nlanguage = ru\n", "a [text.ru] fixture");
    utt::UtTextContext ru;
    ru.file = &f;
    ru.language = "english";   // the game's language does not matter
    ru.english = true;
    ru.tag = ruTag;
    ru.glyph = glyphOf;
    Font cyr = {{true, true, true}};
    ru.glyphCtx = &cyr;
    p = utt::utTextResolvePad(ru, in);
    ru.language = "russian";
    ru.english = false;
    const utt::UtTextPad pRu = utt::utTextResolvePad(ru, in);
    check(pRu.caps == p.caps && pRu.names == p.names, "the same file, the same pad on an English and a Russian game");
    // without [style] language the section is not applied: the pad is [text]'s and the built-ins
    utt::UtTextFile fNo = mustParse(ruText, "the [text.ru] fixture without [style] language");
    ru.file = &fNo;
    const utt::UtTextPad pNo = utt::utTextResolvePad(ru, in);
    check(pNo.caps[1] == "HEL" && pNo.caps.back() == "NXT" && pNo.names[0] == "Helms" && pNo.names[23] == "Relics!" &&
              pNo.skipped.empty(),
          "no [style] language on a Russian game: [text.ru] (and its @game) ignored, [text] and the built-ins");
    ru.file = &f;
    const std::size_t head = 1, chest = 3, feet = 7, legs = 6, dagger = 14, relic = 24;
    check(p.caps[head] == kShLM && p.caps[chest] == kRuNGR && p.capSrc[head] == utt::kSrcLanguage &&
              p.caps.back() == kRuNGR && p.caps[2] == "SHO" && p.skipped.empty(),
          "[text.ru] captions in Cyrillic, [text.ru] over [text] for NEXT, the rest the tags");
    check(p.names[head - 1] == kRuShlemy && p.nameSrc[head - 1] == utt::kSrcGameTag && p.names[chest - 1] == kRuNagrud &&
              p.names[feet - 1] == kRuObuv && p.names[legs - 1] == kRuPonozhi && p.names[dagger - 1] == kRuObuv &&
              p.nameSrc[dagger - 1] == utt::kSrcLanguage && p.names[relic - 1] == "Relics!" &&
              p.names[1] == "Shoulders" && p.names[22] == kRuRuzhya,
          "names: [text.ru], [text], the game's exact words (markers stripped), else English");
    // the same file, a font without Cyrillic (an English game's root font): every Cyrillic candidate
    // is passed over and named
    Font ascii = {{false, false, false}};
    ru.glyphCtx = &ascii;
    p = utt::utTextResolvePad(ru, in);
    check(p.caps[head] == "HEL" && p.caps[chest] == "CHE" && p.caps.back() == "NXT" && p.names[head - 1] == "Helms" &&
              p.names[dagger - 1] == "Daggers",
          "a font without the letters: back to [text], then the built-in English");
    check(p.skipped.size() >= 6 && p.skipped[0].find("U+") != std::string::npos &&
              p.skipped[0].find("caption.head from [text.<lang>]") == 0,
          "every passed-over candidate is named for the one WARN (" +
              (p.skipped.empty() ? std::string() : p.skipped[0]) + ")");
    // only the caption font lacks the letters: names stay Russian
    Font labelOnly = {{false, true, true}};
    ru.glyphCtx = &labelOnly;
    p = utt::utTextResolvePad(ru, in);
    check(p.caps[head] == "HEL" && p.names[head - 1] == kRuShlemy, "captions and names are checked against their own fonts");
}

// 1.1.0's label with D3's "+N not in this database" tail, as ut_panel.cpp drew it before the
// strings file.
std::string oldLabelTail(bool modeKnown, const char* label, int owned, int entries, int first, int last, int rows,
                         int all, int coll, const char* tail, int fs, int limitPx) {
    char ownS[16], allS[16];
    if (owned >= 0) std::snprintf(ownS, sizeof ownS, "%d", owned); else std::strcpy(ownS, "?");
    if (all >= 0) std::snprintf(allS, sizeof allS, "%d", all); else std::strcpy(allS, "?");
    const char* modeS = modeKnown ? "" : "mode unknown - ";
    char line[192];
    std::snprintf(line, sizeof line, "%s%s - %s/%d owned - row %d-%d/%d - coll %s/%d%s", modeS, label, ownS,
                  entries, first, last, rows, allS, coll, tail);
    if (ut::utTextWidthPx(line, fs) > limitPx) {
        char alt[192];
        std::snprintf(alt, sizeof alt, "%s%s - %s/%d - row %d-%d/%d - coll %s/%d%s", modeS, label, ownS, entries,
                      first, last, rows, allS, coll, tail);
        if (ut::utTextWidthPx(alt, fs) <= limitPx) std::strcpy(line, alt);
    }
    if (ut::utTextWidthPx(line, fs) > limitPx) {
        bool done = false;
        for (int n = (int)std::strlen(line) - 1; n > 0 && !done; --n) {
            const char keep = line[n];
            line[n] = 0;
            if (ut::utTextWidthPx(line, fs) <= limitPx) done = true;
            else line[n] = keep;
        }
        if (!done) line[0] = 0;
    }
    for (int n = (int)std::strlen(line); n > 0 && (line[n - 1] == ' ' || line[n - 1] == '-'); --n) line[n - 1] = 0;
    return line;
}

void testLabelCompose() {
    std::mt19937 rng(777);
    std::size_t nb = 0;
    const utg::BuiltinGroup* bg = utg::builtinGroups(&nb);
    utt::UtTextContext c;
    std::vector<std::string> formats;
    utt::utTextResolveFormats(c, formats, nullptr);
    int same = 0, total = 0, tails = 0;
    for (int t = 0; t < 4000; ++t) {
        const utg::BuiltinGroup& g = bg[rng() % nb];
        const bool modeKnown = rng() % 5 != 0;
        const int owned = int(rng() % 200) - 10, entries = int(rng() % 200), first = int(rng() % 40);
        const int last = first + int(rng() % 6), rows = last + int(rng() % 4);
        const int all = int(rng() % 3400) - 100, coll = 3288;
        const int fs = 8 + int(rng() % 25);
        const int limitPx = 60 + int(rng() % 900);
        std::string tail;
        if (rng() % 3 == 0) {
            tail = utt::utNotShownSuffix(long(1 + rng() % 40));
            ++tails;
        }
        const std::string o = oldLabelTail(modeKnown, g.label, owned, entries, first, last, rows, all, coll,
                                           tail.c_str(), fs, limitPx);
        const utt::UtLabelValues v = utt::utLabelValues(
            modeKnown, utt::utTextResolve(c, utt::kTextModeUnknown, "").text,
            utt::utTextResolve(c, utt::kTextName, g.id, g.label).text, owned, entries, first, last, rows, all, coll,
            utt::utTextResolve(c, utt::kTextUnknown, "").text);
        const std::string n = utt::utLabelCompose(formats, v, tail, limitPx, tableWidth, &fs);
        ++total;
        if (o == n) ++same;
        else if (total - same == 1) std::printf("[test] first difference: \"%s\" vs \"%s\"\n", o.c_str(), n.c_str());
    }
    check(same == total && tails > 1000,
          "utLabelCompose, no file: 1.1.0's label with D3's tail byte for byte (" + std::to_string(same) + "/" +
              std::to_string(total) + ", " + std::to_string(tails) + " with the tail)");
    // a Russian template list: the first that fits, else the first cut by width
    utt::UtTextFile f = mustParse(std::string("[text.ru]\nlabel.format.1 = {group} - {owned}/{entries} ") + kRuSobrano +
                                      " - " + kRuRyad + " {first}-{last}/{rows} - " + kRuVsego + " {all}/{coll}\n"
                                      "label.format.2 = {group} {owned}/{entries} {first}-{last}/{rows} {all}/{coll}\n"
                                      "label.format.3 = {group} {owned}/{entries}\n[style]\nlanguage = ru\n",
                                  "a Russian label");
    utt::UtTextContext ru;
    ru.file = &f;
    check(utt::utTextResolveFormats(ru, formats, nullptr) == utt::kSrcLanguage && formats.size() == 3,
          "three Russian templates");
    const utt::UtLabelValues v = utt::utLabelValues(true, "", kRuShlemy, 3, 288, 1, 5, 41, 512, 3288, "?");
    int picked = -1;
    bool fits = false;
    const std::string full = utt::utLabelCompose(formats, v, "", 10000, testWidth, nullptr, &picked, &fits);
    check(picked == 0 && fits && full.find(kRuSobrano) != std::string::npos, "wide: the first template");
    const std::string tail = utt::utNotShownSuffix(2);
    check(tail == " - +2 not on these pages" && utt::utNotShownSuffix(0).empty() &&
              utt::utNotShownSuffix(-1).empty() && utt::utNotShownSuffix(1234) == " - +1234 not on these pages",
          "the cue: \" - +N not on these pages\" for N >= 1, nothing for 0 or not counted (-1)");
    const int w2 = testWidth(std::string(kRuShlemy) + " 3/288 1-5/41 512/3288" + tail, nullptr);
    std::string line = utt::utLabelCompose(formats, v, tail, w2, testWidth, nullptr, &picked, &fits);
    check(picked == 1 && fits && line == std::string(kRuShlemy) + " 3/288 1-5/41 512/3288" + tail,
          "narrower: the second, the tail last (" + line + ")");
    line = utt::utLabelCompose(formats, v, "", w2 - 6 * (int)tail.size() - 1, testWidth, nullptr, &picked, &fits);
    check(picked == 2 && fits && line == std::string(kRuShlemy) + " 3/288", "narrower still: the third");
    line = utt::utLabelCompose(formats, v, "", 9 * 3, testWidth, nullptr, &picked, &fits);
    check(picked == 0 && !fits && line == "\xD0\xA8\xD0\xBB\xD0\xB5", "none fits: the first, cut at a code point");
}

// ---------------------------------------------------------------- the font keys per role
// [style] font.caption / font.label: the chain asks each role's own font, and the WARN about a
// missing letter says which key to add (a player's file: an English game, Cyrillic under
// [text]).
void testFontRoles() {
    check(std::string(utt::utTextFontFolders()) == "de es tr cs el fr hu it nl pt pl ru sk bg",
          "the fourteen folders with a baked savapromedium");
    check(std::string(utt::utTextFontFolderFor(0x428)) == "ru" && std::string(utt::utTextFontFolderFor(0x401)) == "ru" &&
              std::string(utt::utTextFontFolderFor(0x3A9)) == "el" && utt::utTextFontFolderFor(0xE9) == nullptr &&
              utt::utTextFontFolderFor(0x4E2D) == nullptr && utt::utTextFontFolderFor('A') == nullptr,
          "the folder by script: Cyrillic ru, Greek el, else none");

    utt::UtTextFile f;
    std::string err;
    check(utt::utTextParse(std::string("[text]\ncaption.head = ") + kShLM + "\nname.head = " + kShlemy +
                               "\n[style]\nfont.caption = fonts/ru/savapromedium.fnt\n",
                           f, &err),
          "that file plus font.caption parses (" + err + ")");
    check(f.style.font[utt::kStyleFontCaption] == "fonts/ru/savapromedium.fnt" && f.style.font[utt::kStyleFontLabel].empty(),
          "font.caption set, font.label unset");
    utt::UtTextContext c;
    c.file = &f;
    c.language = "english";
    c.english = true;
    c.glyph = glyphOf;
    // English game, no font key: the WARN, with what to do about it
    Font none = {{false, false, false}};
    c.glyphCtx = &none;
    std::vector<utt::UtTextGroupIn> in = {{"head", "HEL", "Helms"}};
    utt::UtTextPad pad = utt::utTextResolvePad(c, in);
    check(pad.caps[1] == "HEL" && pad.names[0] == "Helms" && pad.skipped.size() == 2, "no font with the letters: HEL and Helms");
    std::string adv = utt::utTextFontAdvice(pad.skipped);
    check(adv == "to draw it, add font.caption = fonts/ru/savapromedium.fnt and font.label = "
                 "fonts/ru/savapromedium.fnt under [style] of uniquetab-text.ini",
          "the advice names both keys and the ru folder (" + adv + ")");
    // font.caption in force (the caption font has Cyrillic, the label font not)
    Font capOnly = {{true, false, false}};
    c.glyphCtx = &capOnly;
    pad = utt::utTextResolvePad(c, in);
    check(pad.caps[1] == kShLM && pad.capSrc[1] == utt::kSrcCommon && pad.names[0] == "Helms" && pad.skipped.size() == 1 &&
              pad.skipped[0].find("label font") != std::string::npos,
          "font.caption: the caption draws in Cyrillic, the name still falls back");
    adv = utt::utTextFontAdvice(pad.skipped);
    check(adv == "to draw it, add font.label = fonts/ru/savapromedium.fnt under [style] of uniquetab-text.ini",
          "the advice then names font.label only (" + adv + ")");
    // both keys: nothing passed over, no advice
    Font both = {{true, true, false}};
    c.glyphCtx = &both;
    pad = utt::utTextResolvePad(c, in);
    check(pad.caps[1] == kShLM && pad.names[0] == kShlemy && pad.skipped.empty() && utt::utTextFontAdvice(pad.skipped).empty(),
          "font.caption and font.label: both drawn, nothing to advise");
    // the tooltip role keeps the engine's font: a Cyrillic tooltip line on an English game
    utt::UtTextFile t;
    check(utt::utTextParse(std::string("[text]\ntooltip.yes = ") + kShlemy + "\n", t, &err), "a Cyrillic tooltip line parses");
    c.file = &t;
    const utt::UtTextPick y = utt::utTextResolve(c, utt::kTextTooltipYes, "");
    check(y.text == "In your collection" && y.skipped.find("tooltip font") != std::string::npos,
          "the rollover line is checked against the tooltip font, not the caption/label keys");
    adv = utt::utTextFontAdvice({y.skipped});
    check(adv.find("font.") == std::string::npos && adv.find("rollover lines are drawn by the game") != std::string::npos,
          "the tooltip advice names no key (" + adv + ")");
    // a letter whose script does not name a folder; a label template; nothing missing
    adv = utt::utTextFontAdvice({"caption.head from [text]: U+00E9 is not in the caption font"});
    check(adv.find("font.caption = fonts/<lang>/savapromedium.fnt (<lang> one of de es tr cs el fr hu it nl pt pl ru sk bg)") !=
              std::string::npos,
          "an unknown script: <lang> and the folder list (" + adv + ")");
    adv = utt::utTextFontAdvice({"label.format from [text]: U+03A9 is not in the label font"});
    check(adv.find("font.label = fonts/el/savapromedium.fnt") != std::string::npos, "a Greek label template: el");
    check(utt::utTextFontAdvice({"caption.head from [text]: longer than 12 characters"}).empty() &&
              utt::utTextFontAdvice({}).empty(),
          "no missing letter: no advice");

}

// ================================================================ the buttons file (ut_buttons.h)
utb::UtbFile mustParseB(const std::string& text, const char* what) {
    utb::UtbFile f;
    std::string err;
    check(utb::utbParse(text, f, &err), std::string(what) + " parses (" + err + ")");
    return f;
}
void mustRefuseB(const std::string& text, int line, const char* needle, const char* what) {
    utb::UtbFile f;
    std::string err;
    const bool ok = utb::utbParse(text, f, &err);
    const std::string head = "line " + std::to_string(line) + ":";
    check(!ok, std::string(what) + ": refused");
    check(err.compare(0, head.size(), head) == 0, std::string(what) + ": names " + head + " (got \"" + err + "\")");
    check(err.find(needle) != std::string::npos, std::string(what) + ": says \"" + needle + "\" (got \"" + err + "\")");
    check(f.buttons.empty() && f.keys == 0, std::string(what) + ": nothing kept");
}

std::vector<utb::UtbGroupIn> bTable(const std::vector<utt::UtTextGroupIn>& in) {
    std::vector<utb::UtbGroupIn> out;
    for (const auto& g : in) out.push_back({g.id, g.tag, g.label});
    return out;
}

// The groups of a fixture's uniq-groups.txt (id, tag, label per G line).
std::vector<utb::UtbGroupIn> fixtureGroups(const std::string& path) {
    std::vector<utb::UtbGroupIn> out;
    const std::string t = readAll(path);
    std::size_t pos = 0;
    while (pos < t.size()) {
        std::size_t nl = t.find('\n', pos);
        if (nl == std::string::npos) nl = t.size();
        std::string line = t.substr(pos, nl - pos);
        pos = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.compare(0, 2, "G\t") != 0) continue;
        std::vector<std::string> f;
        std::size_t b = 0;
        while (true) {
            const std::size_t e = line.find('\t', b);
            f.push_back(line.substr(b, e == std::string::npos ? std::string::npos : e - b));
            if (e == std::string::npos) break;
            b = e + 1;
        }
        if (f.size() >= 10) out.push_back({f[8], f[9], utb::utbGroupLabel(f[2])});
    }
    return out;
}

void testButtonsParse() {
    const std::string brief =
        "; a header\n[tab]\nlabel.format.1 = {group} - {owned}/{entries}   ; one template\n"
        "tooltip.yes = In your collection\nmat.caption = MAT\nnext.caption = >>\n\n"
        "[HEL]\nname    = Helms\ngroups  = head\n\n"
        "[BOW]\nname    = Bows\ncaption = BOW                           ; optional\n"
        "groups  = staff, ranged2h     ; two groups = one button\nsize    = 12\n"
        "font    = fonts/ru/savapromedium.fnt    ; optional\n\n"
        "[unassigned]\n; a comment\ngroups  = reignofterror.javelins, retired\n";
    utb::UtbFile f = mustParseB(brief, "the brief's example");
    check(f.buttons.size() == 2 && f.buttons[0].tag == "HEL" && f.buttons[1].tag == "BOW", "two buttons in file order");
    check(f.buttons[1].groups == std::vector<std::string>({"staff", "ranged2h"}) && f.buttons[1].size == 12 &&
              f.buttons[1].font == "fonts/ru/savapromedium.fnt" && f.buttons[1].caption == "BOW" &&
              f.buttons[1].groupsLine == 15,
          "[BOW]: groups, size, font, caption; a ';' after a blank is a comment");
    check(f.words.common.count("label.format.1") && f.words.common.at("label.format.1").text == "{group} - {owned}/{entries}" &&
              f.words.common.at("caption.vanilla").text == "MAT" && f.words.common.at("caption.next").text == ">>" &&
              f.words.common.at("tooltip.yes").text == "In your collection",
          "[tab] maps onto the old [text] keys one to one (mat.caption -> caption.vanilla, next.caption -> caption.next)");
    check(f.unassigned == std::vector<std::string>({"reignofterror.javelins", "retired"}) && f.unassignedLine == 19,
          "[unassigned] read (for nothing)");
    f = mustParseB("\xEF\xBB\xBF[tab]\r\ntooltip.no = \"  a ; b  \"  ; c\r\nlabel.unknown = a;b\r\n[a1]\r\ngroups=head\r\nname = x ;y\r\n",
                   "CRLF + BOM");
    check(f.words.common.at("tooltip.no").text == "  a ; b  " && f.words.common.at("label.unknown").text == "a;b" &&
              f.buttons.size() == 1 && f.buttons[0].tag == "a1" && f.buttons[0].name == "x",
          "quotes keep blanks and ';'; 'a;b' stays text; ' ;y' is a comment");
    f = mustParseB("[tab]\nsize.caption_max = 9\ncolor.ring = FF8000\nfont.label = fonts/ru/savapromedium.fnt\n"
                   "[ X ]\ngroups = head\n[unassigned]\ngroups =\n",
                   "[tab] style keys");
    check(f.words.style.size[utt::kStyleSizeCaptionMax] == 9 && f.words.style.color[utt::kStyleColorRing] == 0xFF8000 &&
              f.words.style.font[utt::kStyleFontLabel] == "fonts/ru/savapromedium.fnt" && f.buttons.size() == 1 &&
              f.buttons[0].tag == "X" && f.unassigned.empty(),
          "[tab] takes the old [style] keys; blanks around a tag dropped; an empty [unassigned] groups line");
    mustRefuseB("[A]\ngroups = head\nname = ; only a comment\n", 3, "\"name\" is empty", "name = ; a comment is an empty name");
}

void testButtonsRefusals() {
    mustRefuseB("[HEL]\nname = Helms\n", 1, "has no groups", "a button without groups");
    mustRefuseB("[HELM]\ngroups = head\n", 1, "not a button tag", "a 4-character tag");
    mustRefuseB("[mat]\ngroups = head\n", 1, "is taken", "MAT is taken");
    mustRefuseB("[A]\ngroups = head\n[a]\ngroups = feet\n", 3, "appears twice (first at line 1)", "a tag twice, any case");
    mustRefuseB("[A]\ngroups = head\nname = x\nname = y\n", 4, "set twice in [A] (first at line 3)", "a key twice");
    mustRefuseB("[A]\ngroups = head, , feet\n", 2, "empty id", "an empty id");
    mustRefuseB("[A]\ngroups = head, Feet!\n", 2, "not a group id", "a bad id");
    mustRefuseB("[A]\ngroups = head, head\n", 2, "listed twice", "an id twice on one line");
    mustRefuseB("[A]\ngroups =\n", 2, "a button needs at least one", "an empty groups line on a button");
    mustRefuseB("[A]\ngroups = head\nsize = 5\n", 3, "6..36", "a size below 6");
    mustRefuseB("[A]\ngroups = head\nfont = ../x.fnt\n", 3, "fonts/<name>.fnt", "a font outside fonts/");
    mustRefuseB("[A]\ngroups = head\ncaption = ABCDEFGHIJKLM\n", 3, "longer than 12", "a caption too long");
    mustRefuseB("[A]\ngroups = head\nname =\n", 3, "\"name\" is empty", "an empty name");
    mustRefuseB("[A]\ngroups = head\ncolor = 123456\n", 3, "unknown key \"color\" in [A]", "an unknown button key");
    mustRefuseB("groups = head\n", 1, "outside a section", "a key outside a section");
    mustRefuseB("[tab]\nlabel.format.1 = {nope}\n", 2, "unknown placeholder", "a bad template");
    mustRefuseB("[tab]\ncaption.head = HEL\n", 2, "unknown key \"caption.head\" in [tab]", "an old [text] key");
    mustRefuseB("[tab]\nsize.label = 99\n", 2, "\"size.label\" must be a whole number 8..32", "[tab] size range");
    mustRefuseB("[tab]\ntooltip.yes =\n", 2, "\"tooltip.yes\" is empty", "an empty [tab] word");
    mustRefuseB("[tab]\nmat.caption = ABCDEFGHIJKLM\n", 2, "\"mat.caption\": longer than 12", "[tab] names its own key");
    mustRefuseB("[tab]\n[tab]\n", 2, "[tab] appears twice", "[tab] twice");
    mustRefuseB("[unassigned]\nname = x\n", 2, "only groups", "[unassigned] takes groups only");
    mustRefuseB("[A]\ngroups = head\nname = \"x\" y\n", 3, "text after the closing", "text after a quote");
    mustRefuseB("[A] x\ngroups = head\n", 1, "text after the section header", "text after a header");
    mustRefuseB("[A]\ngroups = head\nname = \xC3\x28\n", 3, "not UTF-8", "not UTF-8");
}

void testResolve() {
    const std::vector<utb::UtbGroupIn> g = bTable(builtinTable());   // head .. relic, 24
    const std::vector<std::string> known = {"staff", "spear1h", "other", "retired"};
    utb::UtbSet s;
    std::string err;
    utb::UtbFile f = mustParseB("[RNG]\nname = Rings & Amulets\ngroups = ring, amulet\n[HEL]\ngroups = head\n"
                                "[BOW]\ngroups = staff\n",
                                "two groups on one button");
    check(utb::utbResolve(f, g, known, s, &err), "resolves (" + err + ")");
    check(s.buttons.size() == 2 && s.dropped == 1 && s.buttons[0].tag == "RNG" && s.buttons[0].groups == std::vector<int>({7, 9}),
          "file order; the union in GROUP order (amulet 7 before ring 9); a known id without records drops its button");
    check(s.buttonOfGroup[7] == 0 && s.buttonOfGroup[9] == 0 && s.buttonOfGroup[0] == 1 && s.buttonOfGroup[1] == -1,
          "group -> button; an unlisted group has none");
    check(s.unassigned.size() == 21 && s.unassigned[0] == "shoulders" && s.unassigned.back() == "relic",
          "[unassigned] = every group no button holds, in group order");
    f = mustParseB("[A]\ngroups = head\n[B]\ngroups = feet, head\n", "an id on two buttons");
    check(!utb::utbResolve(f, g, known, s, &err) && err.find("line 4:") == 0 &&
              err.find("on [A] already (line 2)") != std::string::npos && utb::utbIsBuiltin(s, g),
          "an id on two buttons: refused naming both lines, the built-in set instead (" + err + ")");
    f = mustParseB("[A]\ngroups = hed\n", "a typo");
    check(!utb::utbResolve(f, g, known, s, &err) && err.find("line 2:") == 0 && err.find("\"hed\"") != std::string::npos,
          "an unknown id: refused naming the line (" + err + ")");
    check(utb::utbIsBuiltin(utb::utbBuiltinSet(g), g) && utb::utbBuiltinSet(g).unassigned.empty(), "the built-in set");
    // the pad: MAT, the buttons, OWN, NEXT; names per button; a Cyrillic caption falls back to the tag
    f = mustParseB(std::string("[RNG]\nname = Rings & Amulets\ncaption = ") + kShLM + "\nsize = 9\ngroups = ring, amulet\n"
                   "[HEL]\ngroups = head\nfont = fonts/ru/savapromedium.fnt\ncaption = " + kShLM + "\n",
                   "a pad");
    check(utb::utbResolve(f, g, known, s, &err), "a pad resolves");
    utt::UtTextContext c;
    c.glyph = glyphOf;
    Font none = {{false, false, false}};
    c.glyphCtx = &none;
    utb::UtbPad p = utb::utbResolvePad(c, s, g);
    check(p.caps == std::vector<std::string>({"MAT", "RNG", kShLM, "OWN", ">>"}) && p.names.size() == 2 &&
              p.names[0] == "Rings & Amulets" && p.names[1] == "Helms" && p.sizes[1] == 9 &&
              p.fonts[2] == "fonts/ru/savapromedium.fnt" && p.skipped.size() == 1 &&
              p.skipped[0].find("[RNG] caption: U+0428") == 0,
          "the pad: a caption the font lacks falls back to the tag (named once); a button with a font line keeps its own");
}

void testUnassigned() {
    std::string out;
    const std::string lf = "[A]\ngroups = head\n\n[unassigned]\n; mine\ngroups = feet\n  \n[B]\ngroups = legs\n";
    check(utb::utbRewriteUnassigned(lf, {"feet", "ring"}, &out) &&
              out == "[A]\ngroups = head\n\n[unassigned]\n; mine\ngroups = feet, ring\n  \n[B]\ngroups = legs\n",
          "LF: the groups line replaced in place; the user's comment, the blank-with-spaces line and [B] kept");
    const std::string once = out;
    check(!utb::utbRewriteUnassigned(once, {"feet", "ring"}, &out) && out == once, "the same set: nothing to write");
    const std::string crlf = "[A]\r\ngroups = head\r\n[unassigned]\r\n; c1\r\ngroups = a\r\n; c2\r\ngroups = b\r\n\r\n";
    check(utb::utbRewriteUnassigned(crlf, {}, &out) && out == "[A]\r\ngroups = head\r\n[unassigned]\r\n; c1\r\ngroups =\r\n; c2\r\n\r\n",
          "CRLF: two groups lines become one (where the first was), the comment between kept, CRLF kept");
    check(utb::utbRewriteUnassigned("[A]\ngroups = head", {"x"}, &out) &&
              out == "[A]\ngroups = head\n\n[unassigned]\n; rewritten by the mod: every group of this game that no button lists\ngroups = x\n",
          "no section: appended (the unterminated last line ended first)");
    check(utb::utbRewriteUnassigned("[A]\r\ngroups = h\r\n[unassigned]\r\n; only a comment\r\n", {"x", "y"}, &out) &&
              out == "[A]\r\ngroups = h\r\n[unassigned]\r\n; only a comment\r\ngroups = x, y\r\n",
          "no groups line: after the section's last comment");
    check(utb::utbRewriteUnassigned("[unassigned]", {"x"}, &out) && out == "[unassigned]\ngroups = x\n",
          "a bare unterminated header");
    check(utb::utbRewriteUnassigned("[unassigned]\ngroups = a   \n[B]\ngroups = b\n", {"a"}, &out) &&
              out == "[unassigned]\ngroups = a\n[B]\ngroups = b\n",
          "trailing blanks of the rewritten line go, the rest is byte for byte");
    utb::UtbFile f = mustParseB(out, "a rewritten file");
    check(f.unassigned == std::vector<std::string>({"a"}), "the rewritten section reads back");
    // the ROT file with [BOW] deleted by hand: staff appears under [unassigned], nothing else moves
    const std::string before = "[SPE]\nname    = Hunting Spears\ngroups  = spear1h\n\n[unassigned]\n"
                               "; rewritten by the mod: every group of this game that no button lists\ngroups =\n";
    check(utb::utbRewriteUnassigned(before, {"staff"}, &out) &&
              out == before.substr(0, before.size() - 1) + " staff\n",
          "a deleted button's group lands under [unassigned]");
}

// A new BUILT-IN group gets its button ONCE: an existing buttons file (written before the
// blueprints existed) gets [BLU] appended after its last button, byte for byte otherwise, with the
// "added once" line in [unassigned]; a second load adds nothing; a user who deletes the section
// keeps it deleted; a group already under [unassigned] stays there; a taken tag gets no section;
// CRLF and LF; and the three per-game files (vanilla, ROT, DGD).
static std::string dropBlu(const std::string& file) {
    // the fixture as 1.1.0 wrote it: no [BLU] section, no "added once" line
    std::string t = file;
    const std::string eol = t.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    const std::string sec = "[BLU]" + eol + "name    = Blueprints" + eol + "groups  = blueprint" + eol + eol;
    const std::size_t a = t.find(sec);
    if (a != std::string::npos) t.erase(a, sec.size());
    // the whole "added once" line, whatever ids it names (ROT's names staff and spear1h too)
    const std::size_t m = t.find(utb::utbOfferedPrefix());
    if (m != std::string::npos) {
        const std::size_t e = t.find(eol, m);
        t.erase(m, e == std::string::npos ? std::string::npos : e + eol.size() - m);
    }
    return t;
}
static bool offer(const std::string& bytes, const std::vector<utb::UtbGroupIn>& g, std::string* out,
                  std::vector<std::string>* added = nullptr, std::vector<std::string>* refused = nullptr) {
    utb::UtbFile f = mustParseB(bytes, "a buttons file before the offer");
    return utb::utbOfferNew(bytes, f, g, utb::utbLateBuiltinIds(), out, added, refused);
}
static std::string removeSection(const std::string& t, const std::string& tag) {
    const std::string eol = t.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    const std::size_t a = t.find("[" + tag + "]" + eol);
    if (a == std::string::npos) return t;
    std::size_t b = t.find(eol + "[", a + 1);
    b = b == std::string::npos ? t.size() : b + eol.size();
    return t.substr(0, a) + t.substr(b);
}

void testOfferNew(const std::string& root) {
    // the late list IS the groups' built-in table after 1.1.0's 24, then the built-in template's
    // per-class groups (staff, spear1h - collect_mi=1 gives DGD a spear1h "Artifacts" page)
    std::size_t ng = 0;
    const utg::BuiltinGroup* bg = utg::builtinGroups(&ng);
    std::vector<std::string> late;
    for (std::size_t i = kGroups110; i < ng; ++i) late.push_back(bg[i].id);
    std::string why;
    const utg::GroupSet* tpl = utg::builtinGroupSet(&why);
    check(tpl != nullptr, "the built-in groups template parses (" + why + ")");
    std::vector<std::string> perClass;   // template groups outside the table, named by the game's word
    if (tpl)
        for (const utg::GroupDef& d : tpl->groups)
            if (!utg::builtinById(d.id) && d.labelDerived && d.tagDerived) perClass.push_back(d.id);
    check(perClass == std::vector<std::string>({"staff", "spear1h"}),
          "the template's per-class groups (no label, no tag of their own) are staff and spear1h");
    for (const std::string& id : perClass) late.push_back(id);
    check(late == utb::utbLateBuiltinIds(),
          "the late built-ins = builtinGroups after 1.1.0's 24, then the per-class groups (blueprint, staff, spear1h)");

    for (const char* view : {"", "reignofterror", "deathgrimdawn"}) {
        const std::string name = *view ? view : "vanilla";
        const std::string dir = root + "/data/oracle" + (*view ? std::string("/views/") + view : std::string());
        const std::vector<utb::UtbGroupIn> g = fixtureGroups(dir + "/uniq-groups.txt");
        const std::string fresh = readAll(dir + "/uniquetab-buttons.ini");
        std::string out;
        std::vector<std::string> want;   // the late ids this game has, in the late list's order
        for (const std::string& id : utb::utbLateBuiltinIds())
            for (const utb::UtbGroupIn& x : g)
                if (x.id == id) want.push_back(id);
        check(!want.empty() && want[0] == "blueprint" && utb::utbOfferedIds(fresh) == want &&
                  !offer(fresh, g, &out) && out == fresh,
              "a default file names the game's late built-ins on its added-once line (" +
                  std::to_string(want.size()) + ") and is offered nothing: " + name);
        // the user deletes [BLU] from a default file: it stays deleted, and the rewrite lists it
        const std::string noBlu = removeSection(fresh, "BLU");
        check(noBlu != fresh && !offer(noBlu, g, &out), "a default file whose [BLU] is deleted: not offered back: " + name);

        const std::string old = dropBlu(fresh);
        check(old != fresh && old.find("blueprint") == std::string::npos, "the old file has no blueprint at all: " + name);
        std::vector<std::string> added, refused;
        check(offer(old, g, &out, &added, &refused) && added == std::vector<std::string>({"blueprint"}) &&
                  refused.empty(),
              "an old file: blueprint offered: " + name);
        const std::string once = out;
        // byte preservation: the old file is the new one minus exactly the inserted section and line
        const std::string sec = "[BLU]\r\nname    = Blueprints\r\ngroups  = blueprint\r\n\r\n";
        const std::string mk = std::string(utb::utbOfferedPrefix()) + " blueprint\r\n";
        std::string back = once;
        const std::size_t at = back.find(sec);
        if (at != std::string::npos) back.erase(at, sec.size());
        const std::size_t ma = back.find(mk);
        if (ma != std::string::npos) back.erase(ma, mk.size());
        check(at != std::string::npos && ma != std::string::npos && back == old,
              "every other byte kept, CRLF throughout: " + name);
        check(at != std::string::npos && once.compare(at + sec.size(), 12, "[unassigned]") == 0 &&
                  ma > at && once.find("\n", once.find("[unassigned]")) + 1 < ma + 1,
              "[BLU] after the last button, right before [unassigned]; the line inside [unassigned]: " + name);
        bool lone = false;
        for (std::size_t i = 0; i < once.size(); ++i) lone |= once[i] == '\n' && (i == 0 || once[i - 1] != '\r');
        check(!lone, "no lone LF in a CRLF file: " + name);
        utb::UtbFile f = mustParseB(once, "the file after the offer");
        utb::UtbSet set;
        std::string err;
        check(utb::utbResolve(f, g, {}, set, &err) && set.unassigned.empty() && !f.buttons.empty() &&
                  f.buttons.back().tag == "BLU" && f.buttons.back().name == "Blueprints",
              "it resolves: [BLU] is the last button, nothing on no button: " + name);
        std::string rw;
        check(!utb::utbRewriteUnassigned(once, set.unassigned, &rw), "and needs no [unassigned] rewrite: " + name);
        check(!offer(once, g, &out) && out == once, "a second load adds nothing: " + name);
        // the opt-out: the user deletes [BLU]; the rewrite lists blueprint, the line stays, never re-added
        const std::string deleted = removeSection(once, "BLU");
        check(deleted != once && !offer(deleted, g, &out), "[BLU] deleted by the user: not offered again: " + name);
        utb::UtbFile fd = mustParseB(deleted, "the file without [BLU]");
        utb::UtbSet sd;
        check(utb::utbResolve(fd, g, {}, sd, &err) && sd.unassigned == std::vector<std::string>({"blueprint"}),
              "the deleted group is on no button: " + name);
        check(utb::utbRewriteUnassigned(deleted, sd.unassigned, &rw) && rw.find(mk) != std::string::npos &&
                  rw.find("groups = blueprint\r\n") != std::string::npos && !offer(rw, g, &out),
              "the rewrite lists it under [unassigned], keeps the added-once line, and it stays off: " + name);
    }

    // LF, a file with no [unassigned], a group the user already listed, a taken tag
    std::vector<utb::UtbGroupIn> g = {{"head", "HEL", "Helms"}, {"relic", "REL", "Relics"},
                                      {"blueprint", "BLU", "Blueprints"}};
    std::string out;
    const std::string lf = "; mine\n[HEL]\ngroups = head\n\n[REL]\ngroups = relic\n\n[unassigned]\n; rewritten\ngroups =\n";
    check(offer(lf, g, &out) &&
              out == "; mine\n[HEL]\ngroups = head\n\n[REL]\ngroups = relic\n\n[BLU]\nname    = Blueprints\n"
                     "groups  = blueprint\n\n[unassigned]\n; rewritten\n" + std::string(utb::utbOfferedPrefix()) +
                         " blueprint\ngroups =\n",
          "LF: the section and the line, LF");
    const std::string bare = "[HEL]\ngroups = head\n[REL]\ngroups = relic";
    check(offer(bare, g, &out) &&
              out == "[HEL]\ngroups = head\n[REL]\ngroups = relic\n\n[BLU]\nname    = Blueprints\ngroups  = blueprint\n"
                     "\n[unassigned]\n; rewritten by the mod: every group of this game that no button lists\n" +
                         std::string(utb::utbOfferedPrefix()) + " blueprint\n",
          "no [unassigned] and an unterminated last line: the section, then a new [unassigned] with the line");
    std::string rw;
    utb::UtbFile fb = mustParseB(out, "the bare file after the offer");
    utb::UtbSet sb;
    std::string err;
    check(utb::utbResolve(fb, g, {}, sb, &err) && sb.unassigned.empty() && utb::utbRewriteUnassigned(out, sb.unassigned, &rw) &&
                  rw.find(std::string(utb::utbOfferedPrefix()) + " blueprint\ngroups =\n") != std::string::npos,
          "the appended [unassigned] takes the rewrite's groups line");
    const std::string listed = "[HEL]\ngroups = head\n[REL]\ngroups = relic\n[unassigned]\ngroups = blueprint\n";
    check(!offer(listed, g, &out), "a group the file already lists under [unassigned] stays there (the opt-out)");
    std::vector<std::string> added, refused;
    const std::string taken = "[HEL]\ngroups = head\n[blu]\ngroups = relic\n\n[unassigned]\ngroups =\n";
    check(offer(taken, g, &out, &added, &refused) && added.empty() &&
              refused == std::vector<std::string>({"blueprint"}) &&
              out == "[HEL]\ngroups = head\n[blu]\ngroups = relic\n\n[unassigned]\n" +
                         std::string(utb::utbOfferedPrefix()) + " blueprint\ngroups =\n" &&
              !offer(out, g, &rw),
          "the tag BLU is a user's button already: no section, only the line (it lands in [unassigned])");
    const std::string mid = "[unassigned]\ngroups =\n\n[HEL]\ngroups = head\n";
    check(offer(mid, g, &out) &&
              out == "[unassigned]\n" + std::string(utb::utbOfferedPrefix()) +
                         " blueprint\ngroups =\n\n[HEL]\ngroups = head\n\n[BLU]\nname    = Blueprints\ngroups  = blueprint\n",
          "[unassigned] before the buttons: the section after the LAST button");
    std::vector<utb::UtbGroupIn> noBp = {{"head", "HEL", "Helms"}};
    check(!offer("[HEL]\ngroups = head\n", noBp, &out), "a game without the group: nothing");
}

// The pad keyed by buttons, live side (ut_padpage.h): Ctrl+wheel walks the
// pad's buttons in the file's order, one stop per button at its first group, the groups on no
// button skipped (ut_live liveHandleWheel), and a several-group button's label counts are summed
// over its groups (ut_panel drawLiveLabel). Both are identity with one button per group.
void testPadByButtons() {
    const std::vector<utb::UtbGroupIn> g = bTable(builtinTable());   // head .. relic, 24
    const std::vector<std::string> known = {"staff", "spear1h", "other", "retired"};
    const int n = (int)g.size();
    utb::UtbSet s;
    std::string err;
    utb::UtbFile f = mustParseB("[RNG]\nname = Rings & Amulets\ngroups = ring, amulet\n[HEL]\ngroups = head\n",
                                "the wheel's file");
    check(utb::utbResolve(f, g, known, s, &err) && s.buttonOfGroup[0] == 1 && s.buttonOfGroup[7] == 0 &&
              s.buttonOfGroup[9] == 0 && s.buttonOfGroup[3] == -1,
          "the wheel's set: head on [HEL], amulet + ring on [RNG], the rest [unassigned] (" + err + ")");
    auto buttonOf = [&](int grp) { return s.buttonOfGroup[(std::size_t)grp]; };
    check(ut::utPadWheelStops(n, buttonOf) == std::vector<int>({-1, 7, 0}),
          "the wheel's stops: materials, then the buttons in FILE order - [RNG] at its first group (amulet 7), "
          "[HEL] (head 0); ring is [RNG]'s page, the [unassigned] groups are no stop");
    // ticks < 0 (wheel down / Ctrl+PageDown) = step +1, ticks > 0 = step -1, as liveHandleWheel
    std::vector<int> fwd, back;
    int cur = -1;
    for (int k = 0; k < 5; ++k) fwd.push_back(cur = ut::utPadWheelButton(cur, 1, n, buttonOf));
    cur = -1;
    for (int k = 0; k < 5; ++k) back.push_back(cur = ut::utPadWheelButton(cur, -1, n, buttonOf));
    check(fwd == std::vector<int>({7, 0, -1, 7, 0}),
          "Ctrl+wheel on, in the pad's order: materials -> [RNG] (7) -> [HEL] (0) -> materials, though head's "
          "group index is the lower");
    check(back == std::vector<int>({0, 7, -1, 0, 7}), "Ctrl+wheel back: the same walk the other way");
    check(ut::utPadWheelButton(9, 1, n, buttonOf) == 0 && ut::utPadWheelButton(9, -1, n, buttonOf) == -1,
          "from ring (the other group of [RNG], a restored state) the step starts at [RNG]");
    check(ut::utPadWheelButton(3, 1, n, buttonOf) == 7 && ut::utPadWheelButton(3, -1, n, buttonOf) == 0,
          "from a group on no button (reached another way) the walk starts at the materials page");
    check(ut::utPadWheelButton(-1, 1, 0, buttonOf) == -1 && ut::utPadWheelButton(-1, -1, 0, buttonOf) == -1 &&
              ut::utPadWheelStops(0, buttonOf) == std::vector<int>({-1}),
          "no groups: the materials page stays");
    // identity: one button per group in group order -> exactly the old one-step wrap
    const utb::UtbSet all = utb::utbBuiltinSet(g);
    auto allOf = [&](int grp) { return all.buttonOfGroup[(std::size_t)grp]; };
    bool same = true;
    for (int c = -1; c < n; ++c) {
        for (int ticks : {1, -1}) {
            int old = c + (ticks > 0 ? -1 : 1);
            while (old < -1) old += n + 1;
            while (old > n - 1) old -= n + 1;
            if (ut::utPadWheelButton(c, ticks > 0 ? -1 : 1, n, allOf) != old) same = false;
        }
    }
    check(same, "one button per group (the built-in set, the generated file): the wheel steps as before");
    // the label's counters of a several-group button
    const int owned[24] = {4, 0, 0, 0, 0, 0, 0, 2, 0, 3};
    const int entries[24] = {40, 0, 0, 0, 0, 0, 0, 10, 0, 20};
    auto ownedOf = [&](int grp) { return grp >= 0 && grp < 24 ? owned[grp] : -1; };
    auto entriesOf = [&](int grp) { return grp >= 0 && grp < 24 ? entries[grp] : -1; };
    int o = -9, e = -9;
    const std::vector<int>& rng = s.buttons[0].groups;   // amulet 7, ring 9
    ut::utPadButtonCounts(rng.data(), (int)rng.size(), ownedOf, entriesOf, &o, &e);
    check(o == 5 && e == 30, "[RNG] (amulet + ring): owned 2 + 3, entries 10 + 20");
    const int one[1] = {0};
    ut::utPadButtonCounts(one, 1, ownedOf, entriesOf, &o, &e);
    check(o == 4 && e == 40, "a one-group button: that group's own numbers");
    auto ownedUnknown = [&](int grp) { return grp == 9 ? -1 : ownedOf(grp); };
    ut::utPadButtonCounts(rng.data(), (int)rng.size(), ownedUnknown, entriesOf, &o, &e);
    check(o == -1 && e == 30, "one group's owned count unknown: the button's is unknown ('?'), entries still summed");
}

void testDefaultAndIdentity(const std::string& root) {
    for (const char* view : {"", "reignofterror", "deathgrimdawn"}) {
        const std::string name = *view ? view : "vanilla";
        const std::string dir = root + "/data/oracle" + (*view ? std::string("/views/") + view : std::string());
        const std::vector<utb::UtbGroupIn> g = fixtureGroups(dir + "/uniq-groups.txt");
        check(g.size() >= 24, "fixture " + name + ": G lines read");
        const std::string text = utb::utbDefaultText(utb::utbViewTitle(view), g);
        const std::string want = readAll(dir + "/uniquetab-buttons.ini");
        check(!want.empty() && utb::utbCrlf(text) == want, "C++ = Python, byte for byte: " + name + "'s generated buttons file");
        utb::UtbFile f = mustParseB(text, "the generated file");
        utb::UtbSet s;
        std::string err;
        check(utb::utbResolve(f, g, {}, s, &err) && utb::utbIsBuiltin(s, g) && s.unassigned.empty(),
              "the generated file = the built-in buttons: " + name);
        std::string rw;
        check(!utb::utbRewriteUnassigned(want, s.unassigned, &rw), "the generated file needs no [unassigned] rewrite: " + name);
        std::vector<utt::UtTextGroupIn> in;
        for (const auto& x : g) in.push_back({x.id, x.tag, x.label});
        utt::UtTextContext none;
        const utt::UtTextPad old = utt::utTextResolvePad(none, in);
        utt::UtTextContext c;
        c.file = &f.words;
        const utb::UtbPad p = utb::utbResolvePad(c, s, g);
        check(p.caps == old.caps && p.names == old.names && p.skipped.empty(),
              "the identity: the same MAT, captions, OWN, >> and names as no file: " + name);
        std::vector<std::string> fm, fm0;
        utt::utTextResolveFormats(c, fm, nullptr);
        utt::utTextResolveFormats(none, fm0, nullptr);
        bool words = fm == fm0;
        for (utt::UtTextKind k : {utt::kTextModeUnknown, utt::kTextUnknown, utt::kTextTooltipYes, utt::kTextTooltipNo,
                                  utt::kTextTooltipRoll})
            words = words && utt::utTextResolve(c, k, "").text == utt::utTextResolve(none, k, "").text;
        check(words, "the identity: the templates, the mode/unknown words and the rollover lines: " + name);
        check(utb::utbRoleCodePoints(f, utt::kFontCaption).empty() && utb::utbRoleCodePoints(f, utt::kFontLabel).empty(),
              "the identity: no letter outside ASCII, so both roles keep the root font: " + name);
    }
    // [tab] tooltip.roll: the words the "(another roll)" line appends to tooltip.yes.
    // Read, never written: the generated file does not list it; quoted, its leading blank stays.
    {
        check(utb::utbDefaultText(utb::utbViewTitle(""), {}).find("tooltip.roll") == std::string::npos,
              "tooltip.roll: the generated buttons file does not list it");
        utt::UtTextContext none;
        const utt::UtTextPick r0 = utt::utTextResolve(none, utt::kTextTooltipRoll, "");
        check(r0.text == " (another roll)" && r0.source == utt::kSrcBuiltin,
              "tooltip.roll: no file -> the built-in \" (another roll)\"");
        utb::UtbFile rf = mustParseB("[tab]\ntooltip.roll = \" (noch ein Wurf)\"\n[HEL]\ngroups = head\n",
                                     "a quoted tooltip.roll");
        utt::UtTextContext rc;
        rc.file = &rf.words;
        const utt::UtTextPick r1 = utt::utTextResolve(rc, utt::kTextTooltipRoll, "");
        check(r1.text == " (noch ein Wurf)" && r1.source == utt::kSrcCommon,
              "tooltip.roll: [tab] sets it, the quoted leading blank kept (got \"" + r1.text + "\")");
        check(utt::utTextResolve(rc, utt::kTextTooltipYes, "").text == "In your collection",
              "tooltip.roll leaves tooltip.yes alone");
        utb::UtbFile rb = mustParseB("[tab]\ntooltip.roll = (x)\n[HEL]\ngroups = head\n", "an unquoted tooltip.roll");
        rc.file = &rb.words;
        check(utt::utTextResolve(rc, utt::kTextTooltipRoll, "").text == "(x)",
              "tooltip.roll unquoted: trimmed like every value (the README says to quote it)");
        mustRefuseB("[tab]\ntooltip.roll = \"\"\n", 2, "empty", "an empty tooltip.roll");
        utt::UtTextFile tf;
        std::string terr;
        check(utt::utTextParse("[text]\ntooltip.roll = \" (z)\"\n", tf, &terr) &&
                  utt::utTextKeyName(utt::kTextTooltipRoll, "") == "tooltip.roll" &&
                  utt::utTextFontOf(utt::kTextTooltipRoll) == utt::kFontTooltip,
              "tooltip.roll: the key, the tooltip font (" + terr + ")");
    }
    check(utb::utbDefaultText(utb::utbViewTitle("reignofterror"), {}).find("in the custom game reignofterror.") != std::string::npos &&
              utb::utbDefaultText(utb::utbViewTitle(""), {}).find("in the main campaign.") != std::string::npos,
          "the header names the game");
    const std::string odd = utb::utbDefaultText("x", {{"a", "MAT", "A"}, {"b", "BBB", "B"}, {"c", "bbb", "C"}});
    utb::UtbFile f = mustParseB(odd, "a reserved and a duplicate tag");
    check(f.buttons.size() == 1 && f.unassigned == std::vector<std::string>({"a", "c"}), "a reserved / duplicate tag: [unassigned]");
    check(utb::utbGroupLabel("{^E}Bows^w!") == "Bows!" && utb::utbGroupLabel("Caf\xE9") == "Caf\xC3\xA9" &&
              utb::utbNameOf(rep("x", 60)).size() == 48,
          "a G label as a name: colour codes, latin-1, 48 units");
    check(utb::utbQuote("mode unknown - ") == "\"mode unknown - \"" && utb::utbQuote("a;b") == "a;b" &&
              utb::utbQuote("a ;b") == "\"a ;b\"" && utb::utbQuote("; x") == "\"; x\"",
          "a value is quoted exactly when the parser would lose part of it");
}

void testFold() {
    const std::vector<utb::UtbGroupIn> g = bTable(builtinTable());
    utt::UtTextFile old = mustParse(std::string("[text]\ncaption.head = HAT\nname.head = Hats\nname.feet = @game\n"
                                                "caption.owned = MINE\ncaption.reignofterror.bows = BW\n"
                                                "label.format.1 = {group}: {owned} of {entries}\ntooltip.yes = Got it\n"
                                                "[text.ru]\nname.chest = ") + kShlemy + "\n[style]\nlanguage = ru\n"
                                                "font.caption = fonts/ru/savapromedium.fnt\nsize.label = 15\ncolor.mark = 00ff00\n",
                                    "an old text file");
    std::vector<std::string> notes;
    const utb::UtbDefaults d = utb::utbFoldText(old, g, &notes);
    const std::string text = utb::utbDefaultText("the main campaign", g, d);
    utb::UtbFile f = mustParseB(text, "the folded file");
    utb::UtbSet s;
    std::string err;
    check(utb::utbResolve(f, g, {}, s, &err), "the folded file resolves");
    check(f.buttons[0].caption == "HAT" && f.buttons[0].name == "Hats" && f.buttons[2].name == kShlemy,
          "caption.head / name.head and [text.ru]'s name.chest (by [style] language) land on their buttons");
    check(f.words.common.at("caption.owned").text == "MINE" && f.words.common.at("label.format.1").text == "{group}: {owned} of {entries}" &&
              !f.words.common.count("label.format.2") && f.words.common.at("tooltip.yes").text == "Got it",
          "caption.owned -> own.caption; the template list taken whole; tooltip.yes");
    check(f.words.style.font[utt::kStyleFontCaption] == "fonts/ru/savapromedium.fnt" && f.words.style.size[utt::kStyleSizeLabel] == 15 &&
              f.words.style.color[utt::kStyleColorMark] == 0x00FF00,
          "[style] font / size / colour keys -> [tab]");
    bool game = false, mod = false, lang = false;
    for (const std::string& n : notes) {
        game |= n.find("name.feet = @game") == 0;
        mod |= n.find("caption.reignofterror.bows") == 0;
        lang |= n.find("[style] language = ru") == 0;
    }
    check(game && mod && lang && notes.size() == 3, "what is left behind is named: @game, another game's id, the language");
    check(text.find("own.caption        = MINE\n") != std::string::npos &&
              text.find("[HEL]\nname    = Hats\ncaption = HAT\ngroups  = head\n") != std::string::npos,
          "the folded words in the file's own layout");
    utt::UtTextFile empty;
    std::vector<std::string> n2;
    check(utb::utbDefaultText("x", g, utb::utbFoldText(empty, g, &n2)) == utb::utbDefaultText("x", g) && n2.empty(),
          "an old file that sets nothing folds into the default file exactly");
}

// a toy set of fonts: the root has ASCII, ru Cyrillic, el Greek, de/fr U+00C0..U+00FF
bool toyCovers(const std::string& folder, char32_t cp, const void*) {
    if (cp >= 0x20 && cp <= 0x7E) return true;
    if (folder == "ru") return cp >= 0x400 && cp <= 0x4FF;
    if (folder == "el") return cp >= 0x370 && cp <= 0x3FF;
    if (folder == "de" || folder == "fr") return cp >= 0xC0 && cp <= 0xFF;
    return false;
}
void testDerive() {
    std::u32string cps;
    check(utb::utbDeriveFolder(cps, toyCovers, nullptr).folder.empty(), "no letter outside ASCII: the root font (the identity)");
    utt::utf8Decode(std::string(kShLM), cps);
    check(utb::utbDeriveFolder(cps, toyCovers, nullptr).folder == "ru", "Cyrillic -> ru");
    utt::utf8Decode(std::string("\xCE\xA9"), cps);
    check(utb::utbDeriveFolder(cps, toyCovers, nullptr).folder == "el", "Greek -> el");
    utt::utf8Decode(std::string("\xC3\xA9"), cps);
    check(utb::utbDeriveFolder(cps, toyCovers, nullptr).folder == "de", "a Latin-1 letter: the first copy that has it (de)");
    utt::utf8Decode(std::string(kShLM) + "\xCE\xA9", cps);
    const utb::UtbDerived d = utb::utbDeriveFolder(cps, toyCovers, nullptr);
    check(d.folder == "ru" && !d.coversAll && d.covered == 3, "no copy has them all: the one with the most, coversAll false");
    check(utb::utbFolderFont("") == "fonts/savapromedium.fnt" && utb::utbFolderFont("ru") == "fonts/ru/savapromedium.fnt",
          "folder -> font path");
    utb::UtbFile f = mustParseB(std::string("[tab]\nown.caption = ") + kShLM + "\nlabel.format.1 = {group} \xCE\xA9\n[A]\ngroups = head\nname = " +
                                    kShlemy + "\ncaption = \xC3\xA9\n[B]\ngroups = feet\ncaption = \xCE\xA9\nfont = fonts/el/savapromedium.fnt\n",
                                "role letters");
    const std::u32string cap = utb::utbRoleCodePoints(f, utt::kFontCaption);
    const std::u32string lab = utb::utbRoleCodePoints(f, utt::kFontLabel);
    check(cap.size() == 4 && cap[0] == 0xE9 && cap.find(0x3A9) == std::u32string::npos,
          "the caption role: the captions without a font line of their own and own.caption, deduplicated");
    check(lab.size() == 6 && lab.back() == 0x3A9, "the label role: the names and the templates");
}

// The FNTD copies (zh ja ko: TrueType fonts the engine rasterises through GDI, Windows
// drawing what they lack). A toy set: zh = WenQuanYi (ASCII, Cyrillic, Greek, kana, Hangul, Han -
// no Thai), ja = Noto Sans JP (kana, Han), ko = NanumGothic (Hangul, Han); the baked copies as
// toyCovers.
bool toyCoversDyn(const std::string& folder, char32_t cp, const void* ctx) {
    const bool han = cp >= 0x4E00 && cp <= 0x9FFF, kana = cp >= 0x3040 && cp <= 0x30FF,
               hangul = cp >= 0xAC00 && cp <= 0xD7AF;
    if (folder == "zh") return (cp >= 0x20 && cp <= 0x7E) || (cp >= 0x370 && cp <= 0x4FF) || han || kana || hangul;
    if (folder == "ja") return (cp >= 0x20 && cp <= 0x7E) || han || kana;
    if (folder == "ko") return (cp >= 0x20 && cp <= 0x7E) || han || hangul;
    return toyCovers(folder, cp, ctx);
}
void testDeriveDynamic() {
    auto derive = [](const std::string& utf8, bool dyn) {
        std::u32string cps;
        utt::utf8Decode(utf8, cps);
        return utb::utbDeriveFolder(cps, toyCoversDyn, nullptr, dyn);
    };
    check(utb::utbDynamicFolderList() == std::vector<std::string>({"zh", "ja", "ko"}),
          "derive: the FNTD copies tried after every baked one: zh ja ko");
    check(std::string(utb::utbCjkFolderFor(0x3042)) == "ja" && std::string(utb::utbCjkFolderFor(0x30AB)) == "ja" &&
              std::string(utb::utbCjkFolderFor(0xD55C)) == "ko" && std::string(utb::utbCjkFolderFor(0x1100)) == "ko" &&
              std::string(utb::utbCjkFolderFor(0x8235)) == "zh" && std::string(utb::utbCjkFolderFor(0x3002)) == "zh" &&
              !utb::utbCjkFolderFor('A') && !utb::utbCjkFolderFor(0x428) && !utb::utbCjkFolderFor(0xE01),
          "derive: the script folder of a CJK letter: kana ja, Hangul ko, Han zh; nothing for another letter");
    // the identity gates, with the FNTD copies known and without
    for (bool dyn : {false, true}) {
        const std::string tag = dyn ? " (FNTD known)" : " (FNTX only)";
        const utb::UtbDerived a = derive("HEL", dyn);
        check(a.folder.empty() && a.coversAll && !a.dynamic, "derive: ASCII keeps the root font" + tag);
        const utb::UtbDerived r = derive(kShLM, dyn);
        check(r.folder == "ru" && r.coversAll && !r.dynamic, "derive: Cyrillic -> ru as today" + tag);
        check(derive("\xCE\xA9", dyn).folder == "el", "derive: Greek -> el as today" + tag);
        check(derive("\xC3\xA9", dyn).folder == "de", "derive: a Latin-1 letter -> the first baked copy (de) as today" + tag);
        const utb::UtbDerived e = derive("\xF0\x9F\x98\x80", dyn);   // U+1F600
        check(e.folder.empty() && e.coversAll, "derive: a letter above U+FFFF never decides the font (the root stays)" + tag);
        check(derive(std::string("HEL\xF0\x9F\x98\x80") + kShLM, dyn).folder == "ru",
              "derive: ... nor does it keep Cyrillic from ru" + tag);
    }
    // without the FNTD copies: today's answer (the most covered, coversAll false)
    const utb::UtbDerived old = derive("\xE8\x88\xB5", false);   // U+8235
    check(old.folder.empty() && !old.coversAll && !old.dynamic, "derive: U+8235 without the FNTD copies: no copy has it (today)");
    // with them: the script's FNTD copy
    const utb::UtbDerived zh = derive("\xE8\x88\xB5", true);
    check(zh.folder == "zh" && zh.coversAll && zh.dynamic && zh.lacking == 0, "derive: Han -> zh");
    const utb::UtbDerived ja = derive("\xE3\x81\x8B\xE3\x81\xAA", true);   // kana ka na
    check(ja.folder == "ja" && ja.coversAll && ja.dynamic, "derive: kana -> ja");
    const utb::UtbDerived jah = derive("\xE8\x88\xB5\xE3\x81\x8B", true);   // Han + kana: kana decides
    check(jah.folder == "ja" && jah.coversAll, "derive: Han with kana -> ja (kana decides the script)");
    const utb::UtbDerived ko = derive("\xED\x95\x9C\xEA\xB8\x80", true);   // Hangul han geul
    check(ko.folder == "ko" && ko.coversAll && ko.dynamic, "derive: Hangul -> ko");
    const utb::UtbDerived mix = derive(std::string(kShLM) + "\xE8\x88\xB5", true);
    check(mix.folder == "zh" && mix.coversAll, "derive: Cyrillic with Han -> zh (it has both)");
    const utb::UtbDerived kj = derive("\xED\x95\x9C\xE3\x81\x8B", true);   // Hangul + kana
    check(kj.folder == "zh" && kj.coversAll, "derive: Hangul with kana: ja and ko lack one, zh has both");
    // nothing has every letter: zh, Windows drawing the rest - one INFO
    const utb::UtbDerived th = derive("\xE0\xB8\x81\xE0\xB8\x82\xE0\xB8\x84", true);   // Thai
    check(th.folder == "zh" && !th.coversAll && th.dynamic && th.lacking == 3 && th.missing == 0xE01,
          "derive: Thai (in no font of the game) -> zh, three letters drawn by Windows");
    check(utb::utbDerivedFallbackText(th, "the captions") ==
              "fonts/zh/savapromedium.fnt is used for the captions; U+0E01 and 2 more are drawn by Windows' own "
              "fallback font",
          "derive: the INFO line, several letters");
    const utb::UtbDerived th1 = derive(std::string(kShlemy) + " \xE0\xB8\x81", true);
    check(th1.folder == "zh" && th1.lacking == 1 && th1.covered == 6 &&
              utb::utbDerivedFallbackText(th1, "the label line") ==
                  "fonts/zh/savapromedium.fnt is used for the label line; U+0E01 is drawn by Windows' own fallback font",
          "derive: the INFO line, one letter (the Cyrillic and the space in zh's own font)");
    check(utb::utbDerivedFallbackText(th1, "the label line", "fonts/zh/jura.fnt").compare(0, 30, "fonts/zh/jura.fnt is used for ") == 0,
          "derive: the INFO line names another FNTD font by its path");
    check(utb::utbDerivedFallbackText(zh, "the captions").empty() && utb::utbDerivedFallbackText(old, "the captions").empty(),
          "derive: no INFO when the font has every letter, or for a baked font");

    // the glyph check's WARN advice (utbFontAdvice)
    const std::vector<std::string> han = {"[HEL] caption: U+8235 is not in the caption font"};
    check(utb::utbFontAdvice(han, toyCoversDyn, nullptr, true, true) ==
              "to draw it, add font.caption = fonts/zh/savapromedium.fnt under [tab] (or font = on the button)",
          "advice: a letter no baked copy has names the FNTD copy that draws it (zh)");
    check(utb::utbFontAdvice({"[A] caption: U+3042 is not in the caption font", "name.x: U+D55C is not in the label font"},
                             toyCoversDyn, nullptr, true, true) ==
              "to draw it, add font.caption = fonts/ja/savapromedium.fnt and font.label = fonts/ko/savapromedium.fnt "
              "under [tab] (or font = on the button)",
          "advice: kana -> ja, Hangul -> ko, per role");
    check(utb::utbFontAdvice(han, toyCoversDyn, nullptr, true, false) ==
              "no font of the game has U+8235, so no font line can draw it (Chinese, Japanese and Korean letters are "
              "not drawn - README \"Alphabets\")",
          "advice: the generic wording only while the FNTD copies are not known");
    const std::vector<std::string> astral = {"[HEL] caption: U+1F600 is not in the caption font",
                                             "tooltip.yes: U+0428 is not in the tooltip font"};
    const std::string a = utb::utbFontAdvice(astral, toyCoversDyn, nullptr, true, true);
    const std::string aHead = "no font of the game has U+1F600, so no font line can draw it (letters above U+FFFF are "
                              "not drawn - README \"Alphabets\"); ";
    check(a.compare(0, aHead.size(), aHead) == 0 && a.find("rollover lines") != std::string::npos,
          "advice: a letter above U+FFFF: no font line can draw it; the rollover lines' advice kept");
    check(utb::utbFontAdvice({"[HEL] caption: U+0428 is not in the caption font"}, toyCoversDyn, nullptr, true, true) ==
              "to draw it, add font.caption = fonts/ru/savapromedium.fnt under [tab] (or font = on the button)",
          "advice: a Cyrillic letter: the advice as today");
    check(utb::utbFontAdvice(han, toyCoversDyn, nullptr, false, false) ==
              "to draw it, add font.caption = fonts/<lang>/savapromedium.fnt (<lang> one of de es tr cs el fr hu it nl pt "
              "pl ru sk bg) under [tab] (or font = on the button)",
          "advice: the glyph tables not known: today's advice");
    check(utb::utbFontAdvice({}, toyCoversDyn, nullptr, true, true).empty(), "advice: nothing passed over, no advice");
}

void testButtonsPurity(const std::string& header) {
    const std::string src = readAll(header);
    check(!src.empty(), "purity: ut_buttons.h is readable");
    std::size_t pos = 0;
    bool pure = true;
    while ((pos = src.find("#include", pos)) != std::string::npos) {
        const std::size_t eol = src.find('\n', pos);
        const std::string what = utt::utTextTrim(src.substr(pos + 8, eol - pos - 8));
        if (what != "\"ut_textcfg.h\"" && what != "<cstddef>" && what != "<map>" && what != "<string>" &&
            what != "<utility>" && what != "<vector>")
            pure = false;
        pos = eol;
    }
    bool ascii = true;
    for (unsigned char ch : src) if (ch > 0x7E || (ch < 0x20 && ch != '\n' && ch != '\r')) ascii = false;
    check(pure && ascii, "purity: ut_buttons.h includes ut_textcfg.h and standard headers only, plain ASCII");
}

// ---- one grid per button (ut_padpage.h utUnion*; ut_live.cpp runs them) --------------
// A uniq-groups.txt fixture in full: the F line's frame count and row gap, the P line's usable
// rectangle, every G line and its E lines (box record, item record).
struct FixGroup {
    utg::GLine gl;
    std::vector<std::string> box, item;
};
struct Fixture {
    int nmax = 0, rowGap = 2, usableW = 0, usableH = 0;
    std::vector<FixGroup> groups;
};
Fixture readFixture(const std::string& path) {
    Fixture fx;
    const std::string t = readAll(path);
    std::size_t pos = 0;
    while (pos < t.size()) {
        std::size_t nl = t.find('\n', pos);
        if (nl == std::string::npos) nl = t.size();
        std::string line = t.substr(pos, nl - pos);
        pos = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> f;
        std::size_t b = 0;
        while (true) {
            const std::size_t e = line.find('\t', b);
            f.push_back(line.substr(b, e == std::string::npos ? std::string::npos : e - b));
            if (e == std::string::npos) break;
            b = e + 1;
        }
        if (f[0] == "F" && f.size() >= 7) {
            fx.nmax = std::atoi(f[2].c_str());
            fx.rowGap = std::atoi(f[6].c_str());
        } else if (f[0] == "P" && f.size() >= 5) {
            fx.usableW = std::atoi(f[3].c_str());
            fx.usableH = std::atoi(f[4].c_str());
        } else if (f[0] == "G") {
            FixGroup g;
            if (utg::parseGLine(line, g.gl)) fx.groups.push_back(g);
        } else if (f[0] == "E" && !fx.groups.empty()) {
            fx.groups.back().box.push_back(f[1]);
            fx.groups.back().item.push_back(f.size() > 2 ? f[2] : std::string());
        }
    }
    return fx;
}

// The live side's groupsOf over a resolved set: the groups of g's button (0 = on none).
struct SetGroupsOf {
    const utb::UtbSet* s;
    int operator()(int g, int* out, int cap) const {
        if (g < 0 || (std::size_t)g >= s->buttonOfGroup.size() || s->buttonOfGroup[(std::size_t)g] < 0) return 0;
        const std::vector<int>& gs = s->buttons[(std::size_t)s->buttonOfGroup[(std::size_t)g]].groups;
        for (std::size_t i = 0; i < gs.size() && (int)i < cap; ++i) out[i] = gs[i];
        return (int)gs.size();
    }
};

void testOneGridPure() {
    // the union plan over a small table: [AB] = groups 1 + 3, [C] = 2, group 0 and 4 on none
    std::vector<std::vector<int>> btn = {{1, 3}, {2}};
    std::vector<int> of = {-1, 0, 1, 0, -1};
    auto groupsOf = [&](int g, int* out, int cap) {
        if (of[(std::size_t)g] < 0) return 0;
        const std::vector<int>& gs = btn[(std::size_t)of[(std::size_t)g]];
        for (std::size_t i = 0; i < gs.size() && (int)i < cap; ++i) out[i] = gs[i];
        return (int)gs.size();
    };
    const ut::UtUnionPlan p = ut::utUnionPlan(5, groupsOf);
    check(p.rep == std::vector<int>({0, 1, 2, 1, 4}) && p.unionOf == std::vector<int>({-1, 0, -1, -1, -1}) &&
              p.members.size() == 1 && p.members[0] == std::vector<int>({1, 3}),
          "one grid per button: [AB] = groups 1 + 3 is one union keyed by 1; 0, 2, 4 are their own pages");
    check(ut::utUnionRep(p, 3) == 1 && ut::utUnionRep(p, 1) == 1 && ut::utUnionRep(p, -1) == -1 &&
              ut::utUnionRep(p, 9) == 9,
          "the rep: a member selects its union's first group; the materials page and an unknown index stay");
    check(ut::utUnionMembers(p, 3) == std::vector<int>({1, 3}) && ut::utUnionMembers(p, 2) == std::vector<int>({2}) &&
              ut::utUnionMembers(p, -1).empty(),
          "the members of a page: the union's, else the group alone, none for the materials page");
    // a caller that hands the groups out of order still gets group order
    auto shuffled = [&](int g, int* out, int cap) {
        const int k = groupsOf(g, out, cap);
        if (k == 2) std::swap(out[0], out[1]);
        return k;
    };
    check(ut::utUnionPlan(5, shuffled).members == p.members, "the union is in group order whatever the caller gave");
    // no several-group button: no union at all
    auto single = [](int g, int* out, int) { out[0] = g; return 1; };
    const ut::UtUnionPlan id = ut::utUnionPlan(5, single);
    check(id.members.empty() && id.rep == std::vector<int>({0, 1, 2, 3, 4}),
          "one group per button: no union, every group its own page (the identity)");
    check(ut::utUnionPlan(0, single).rep.empty() && ut::utUnionPlan(-3, single).members.empty(), "an empty table");
    // the wheel: one stop per button, at the union's rep
    auto buttonOf = [&](int g) { return of[(std::size_t)g]; };
    std::vector<int> walk;
    int cur = -1;
    for (int k = 0; k < 4; ++k) walk.push_back(cur = ut::utPadWheelButton(cur, 1, 5, buttonOf));
    check(walk == std::vector<int>({1, 2, -1, 1}) && ut::utUnionRep(p, walk[0]) == walk[0],
          "Ctrl+wheel: materials -> [AB] (1, the union's rep) -> [C] (2) -> materials; 3 is [AB]'s page, 0 and 4 "
          "on no button");
    check(ut::utPadWheelButton(2, -1, 5, buttonOf) == 1 && ut::utPadWheelButton(3, 1, 5, buttonOf) == 2 &&
              ut::utPadWheelButton(3, -1, 5, buttonOf) == -1,
          "back from [C] lands on [AB]'s first group; from member 3 (restored) the walk steps from [AB]");
    // the cell
    using C = ut::UtCell;
    int exact = -1;
    C c = ut::utUnionCell({{64, 128}, {64, 160}}, {{64, 64}, {64, 128}, {64, 160}, {32, 32}}, &exact);
    check(c.w == 64 && c.h == 160 && exact == 1, "the cell holds both: 64x128 + 64x160 -> 64x160, a view cell");
    c = ut::utUnionCell({{32, 96}, {64, 64}}, {{64, 128}, {128, 128}, {64, 96}}, &exact);
    check(c.w == 64 && c.h == 96 && exact == 1, "the smallest view cell that holds 32x96 and 64x64: 64x96");
    c = ut::utUnionCell({{32, 96}, {64, 64}}, {{128, 128}, {64, 128}}, &exact);
    check(c.w == 64 && c.h == 128 && exact == 1, "no 64x96 in the view: the next view cell that holds both (64x128)");
    c = ut::utUnionCell({{32, 96}, {64, 64}}, {{32, 96}, {64, 64}}, &exact);
    check(c.w == 64 && c.h == 96 && exact == 0, "no view cell holds both: 64x96 itself, no plate (exact 0)");
    c = ut::utUnionCell({{32, 32}}, {{32, 32}, {64, 64}}, &exact);
    check(c.w == 32 && c.h == 32 && exact == 1, "one member: its own cell");
    c = ut::utUnionCell({{64, 64}}, {{64, 64}, {32, 128}, {128, 32}}, &exact);
    check(c.w == 64 && c.h == 64, "equal areas never beat the exact cell");
    // the order and the row kept
    const std::vector<int> cnt = {0, 5, 0, 3};
    auto count = [&](int g) { return g >= 0 && g < 4 ? cnt[(std::size_t)g] : -1; };
    std::vector<std::pair<int, int>> order;
    for (int k = 0;; ++k) {
        int g = -1, i = -1;
        if (!ut::utUnionLocate({1, 3}, count, k, &g, &i)) break;
        order.push_back({g, i});
    }
    check(order == std::vector<std::pair<int, int>>({{1, 0}, {1, 1}, {1, 2}, {1, 3}, {1, 4}, {3, 0}, {3, 1}, {3, 2}}),
          "the union's entries: group 1's five, then group 3's three - group order, each in its own order");
    check(!ut::utUnionLocate({1, 3}, count, -1, nullptr, nullptr), "a negative entry");
    check(ut::utUnionRowOf({1, 3}, count, 3, 1, 2, 4) == 1 && ut::utUnionRowOf({1, 3}, count, 1, 2, 2, 4) == 1 &&
              ut::utUnionRowOf({1, 3}, count, 2, 3, 2, 4) == 3,
          "the row kept: group 3 row 1 (2 wide) = item 5 + 2 = 7 -> union row 1 of 4; a non-member keeps its row");
    // the search mark of a button
    const int ab[2] = {1, 3};
    check(ut::utUnionMarked(ab, 2, [](int g) { return g == 3; }) && ut::utUnionMarked(ab, 2, [](int g) { return g == 1; }) &&
              !ut::utUnionMarked(ab, 2, [](int g) { return g == 2; }),
          "the search: [AB] is marked by a match in either group (tier B marks the shown rep, A/C the record's group)");
}

void testOneGridRot(const std::string& root) {
    // the flag-off cut (data\oracle\nomi): the box counts below are its Bows / Hunting Spears
    const std::string dir = root + "/data/oracle/nomi/views/reignofterror";
    const Fixture fx = readFixture(dir + "/uniq-groups.txt");
    const std::vector<utb::UtbGroupIn> g = fixtureGroups(dir + "/uniq-groups.txt");
    check(fx.groups.size() == 29 && g.size() == 29 && fx.nmax == 512 && fx.usableW == 962 && fx.usableH == 584,
          "ROT fixture: 29 groups (Blueprints 26 after Relics), frame 512, usable 962 x 584");
    if (fx.groups.size() != 29 || g.size() != 29) return;
    check(fx.groups[23].gl.id == "staff" && fx.groups[24].gl.id == "spear1h" && fx.groups[23].box.size() == 76 &&
              fx.groups[24].box.size() == 19,
          "ROT: group 23 Bows (staff, 76), group 24 Hunting Spears (spear1h, 19)");
    // the generated file = no union (identity)
    const std::string gen = readAll(dir + "/uniquetab-buttons.ini");
    utb::UtbFile f0 = mustParseB(gen, "ROT's generated buttons file");
    utb::UtbSet s0;
    std::string err;
    check(utb::utbResolve(f0, g, {}, s0, &err), "ROT's generated file resolves (" + err + ")");
    const ut::UtUnionPlan p0 = ut::utUnionPlan(29, SetGroupsOf{&s0});
    bool ident = p0.members.empty();
    for (int i = 0; i < 29; ++i) ident = ident && p0.rep[(std::size_t)i] == i && p0.unionOf[(std::size_t)i] == -1;
    check(ident, "ROT's generated file: no union - every page the group's own, as before");
    // a recorded edit: [BOW] removed, [SPE] groups = spear1h, staff
    std::string user = gen;
    const std::size_t bow = user.find("[BOW]");
    const std::size_t next = bow == std::string::npos ? bow : user.find("\r\n[", bow);
    check(bow != std::string::npos && next != std::string::npos, "the fixture has a [BOW] section");
    if (bow == std::string::npos || next == std::string::npos) return;
    user.erase(bow, next + 2 - bow);
    const std::size_t spe = user.find("groups  = spear1h", user.find("[SPE]"));
    check(spe != std::string::npos, "the fixture's [SPE] groups line");
    if (spe == std::string::npos) return;
    user.replace(spe, 17, "groups  = spear1h, staff");
    utb::UtbFile f = mustParseB(user, "a recorded ROT file (SPE = spear1h, staff)");
    utb::UtbSet s;
    check(utb::utbResolve(f, g, {}, s, &err) && s.unassigned.empty(), "the recorded ROT file resolves (" + err + ")");
    const int spb = s.buttonOfGroup[23];
    check(spb >= 0 && s.buttonOfGroup[24] == spb && s.buttons[(std::size_t)spb].tag == "SPE" &&
              s.buttons[(std::size_t)spb].groups == std::vector<int>({23, 24}),
          "[SPE] holds Bows (23) and Hunting Spears (24), in group order");
    const ut::UtUnionPlan p = ut::utUnionPlan(29, SetGroupsOf{&s});
    bool others = true;
    for (int i = 0; i < 29; ++i)
        if (i != 23 && i != 24) others = others && p.rep[(std::size_t)i] == i && p.unionOf[(std::size_t)i] == -1;
    check(p.members.size() == 1 && p.members[0] == std::vector<int>({23, 24}) && p.rep[23] == 23 && p.rep[24] == 23 &&
              p.unionOf[23] == 0 && p.unionOf[24] == -1 && others,
          "ONE page for [SPE], keyed by Bows (23); a click on SPE always selects 23 - nothing to switch; every "
          "other button exactly as before");
    // the union as ut_live builds it: the members' entries concatenated
    std::vector<std::string> ubox, uitem;
    for (int m : p.members[0]) {
        ubox.insert(ubox.end(), fx.groups[(std::size_t)m].box.begin(), fx.groups[(std::size_t)m].box.end());
        uitem.insert(uitem.end(), fx.groups[(std::size_t)m].item.begin(), fx.groups[(std::size_t)m].item.end());
    }
    auto count = [&](int grp) { return grp >= 0 && grp < 29 ? (int)fx.groups[(std::size_t)grp].box.size() : -1; };
    bool located = ubox.size() == 95;
    for (int k = 0; k < (int)ubox.size(); ++k) {
        int gg = -1, ii = -1;
        located = located && ut::utUnionLocate(p.members[0], count, k, &gg, &ii) &&
                  fx.groups[(std::size_t)gg].box[(std::size_t)ii] == ubox[(std::size_t)k] &&
                  fx.groups[(std::size_t)gg].item[(std::size_t)ii] == uitem[(std::size_t)k];
    }
    check(located && !ut::utUnionLocate(p.members[0], count, 95, nullptr, nullptr),
          "the union's 95 entries: the 76 bows, then the 19 hunting spears, each record where utUnionLocate says");
    // the cell and the grid
    std::vector<ut::UtCell> cells, mc;
    for (const FixGroup& x : fx.groups) {
        bool have = false;
        for (const ut::UtCell& c : cells) have = have || (c.w == x.gl.cellW && c.h == x.gl.cellH);
        if (!have) cells.push_back(ut::UtCell{x.gl.cellW, x.gl.cellH});
    }
    for (int m : p.members[0]) mc.push_back(ut::UtCell{fx.groups[(std::size_t)m].gl.cellW, fx.groups[(std::size_t)m].gl.cellH});
    int exact = 0;
    const ut::UtCell c = ut::utUnionCell(mc, cells, &exact);
    check(cells.size() == 7 && c.w == 64 && c.h == 160 && exact == 1,
          "the union's cell: 64x160 (Hunting Spears'), one of ROT's 7 plate cells - the page keeps its plate");
    const ut::UtGrid dg = ut::utGridFit(ut::UtRect{0, 0, fx.usableW, fx.usableH}, c.w, c.h, fx.rowGap, fx.nmax);
    check(dg.cols == fx.groups[24].gl.cols && dg.rows == fx.groups[24].gl.rows && dg.cols == 15 && dg.rows == 3,
          "the derived grid at 64x160: 15 x 3, the G line pages_gen wrote for Hunting Spears");
    // the rows: one window over all 95
    check((95 + dg.cols - 1) / dg.cols == 7 && ut::utMaxRowOf(95, dg.cols, dg.rows) == 4,
          "derived grid: 7 rows (bows rows 1-5, the spears from row 6 on, continuously), the window's last first row 4");
    int g5 = -1, i5 = -1, g6 = -1, i6 = -1;
    ut::utUnionLocate(p.members[0], count, 5 * 15, &g5, &i5);
    ut::utUnionLocate(p.members[0], count, 5 * 15 + 1, &g6, &i6);
    check(g5 == 23 && i5 == 75 && g6 == 24 && i6 == 0,
          "row 6, slot 1 is the last bow and slot 2 the first hunting spear - no page break between the groups");
    // the grid of that session (46,72,928,520 on the 974x659 cover)
    ut::UtLayout L = {};
    L.user[ut::kUtLayGrid] = 1;
    L.rect[ut::kUtLayGrid] = ut::UtRect{46, 72, 928, 520};
    const ut::UtGrid ug = ut::utLiveGroupGrid(L, dg.cols, dg.rows, c.w, c.h, fx.rowGap, fx.nmax);
    check(ug.cols == 14 && ug.rows == 3 && ut::utMaxRowOf(95, ug.cols, ug.rows) == 4,
          "the grid 928 x 520: 14 x 3 cells of 64x160, 7 rows, last first row 4");
    // the counters: the union's are the sums (the label's {owned} / {entries})
    auto ownedOf = [](int grp) { return grp == 23 ? 5 : grp == 24 ? 2 : 0; };
    int o = -1, e = -1;
    ut::utPadButtonCounts(p.members[0].data(), (int)p.members[0].size(), ownedOf, count, &o, &e);
    check(o == 7 && e == 95 && e == (int)ubox.size(), "the label: 7 / 95 owned - the union's sums");
    // deposit / take by record: every record on the page is a member's, by record; the gate only
    // asks that a collection page is shown (the rep, >= 0)
    std::map<std::string, int> groupOf;
    for (int x = 0; x < 29; ++x)
        for (const std::string& it : fx.groups[(std::size_t)x].item) groupOf[it] = x;
    bool byRecord = true;
    for (const std::string& it : uitem) {
        const auto f2 = groupOf.find(it);
        byRecord = byRecord && f2 != groupOf.end() && ut::utUnionRep(p, f2->second) == 23;
    }
    check(byRecord && ut::utUnionRep(p, 23) >= 0,
          "every item on the [SPE] page is a bow's or a hunting spear's record (a deposit and a take are by "
          "record; the page shown is group 23 >= 0, so the gate is open as on any page)");
    // the owned-only filter over the union: the same index indirection, group order kept
    std::vector<int> vis;
    for (int k = 0; k < (int)uitem.size(); ++k)
        if (k % 3 == 0) vis.push_back(k);   // "owned": every third record
    bool ordered = true;
    for (std::size_t k = 1; k < vis.size(); ++k) ordered = ordered && vis[k] > vis[k - 1];
    check(vis.size() == 32 && ordered && ut::utMaxRowOf((int)vis.size(), 15, 3) == 0,
          "OWN on: 32 of 95 shown in group order, one window (3 rows of 15 hold them all)");
    // the wheel and the search on the recorded set
    auto buttonOf = [&](int grp) { return s.buttonOfGroup[(std::size_t)grp]; };
    check(ut::utPadWheelButton(22, 1, 29, buttonOf) == 23 && ut::utPadWheelButton(23, 1, 29, buttonOf) == 25 &&
              ut::utPadWheelButton(24, 1, 29, buttonOf) == 25 && ut::utPadWheelButton(25, -1, 29, buttonOf) == 23 &&
              ut::utPadWheelButton(25, 1, 29, buttonOf) == 26 && fx.groups[26].gl.id == "blueprint" &&
              fx.groups[26].gl.tag == "BLU" && ut::utUnionRep(p, 23) == 23,
          "Ctrl+wheel: 2H Guns -> [SPE] (23) -> Relics (25) and back - one stop for the button; Relics -> "
          "Blueprints (26, the generated file's next button)");
    const std::vector<int>& sg = s.buttons[(std::size_t)spb].groups;
    check(ut::utUnionMarked(sg.data(), (int)sg.size(), [](int grp) { return grp == 24; }) &&
              ut::utUnionMarked(sg.data(), (int)sg.size(), [](int grp) { return grp == 23; }),
          "the search: a spear's match (tier A/C, group 24) or the page's own (tier B, group 23) marks [SPE]");
    // a restored uniquetab.state on Hunting Spears row 1: the page opens on its first item
    check(ut::utUnionRowOf(p.members[0], count, 24, 1, 15, 15) == 6,
          "uniq_group=24 row 1 (spear 15) -> [SPE]'s page, row 6 (item 76 + 15 = 91)");
}

// ---- Ctrl+wheel in the pad's order ------------------------------------------------------------
// DGD's group table has Blueprints (25) right after Relics (24), but a buttons file written before
// blueprints existed gets [BLU] appended once AFTER its last button, so the pad draws ... REL GEM
// AUG OTH BLU. The wheel and Ctrl+PageUp/Down walk what the pad draws (ut_padpage.h
// utPadWheelButton over the TextWorld's button order), one stop per button at the group its click
// selects (groups[0], the union's rep). The group-index walk the mod ran before (reproduced here as
// the "before") went REL -> BLU -> GEM. The state file keeps the group index: every stop IS one.

// The removed rule, for the "before": one step over the group INDEXES, skipping the groups on no
// button and the non-first groups of a several-group button.
int groupIndexWheel(int cur, int step, int n, const utb::UtbSet& s) {
    const ut::UtUnionPlan p = ut::utUnionPlan(n, SetGroupsOf{&s});
    const int d = step < 0 ? -1 : 1;
    int next = cur;
    for (int k = 0; k <= n; ++k) {
        next += d;
        while (next < -1) next += n + 1;
        while (next > n - 1) next -= n + 1;
        if (next < 0 || (s.buttonOfGroup[(std::size_t)next] >= 0 && ut::utUnionRep(p, next) == next)) return next;
    }
    return -1;
}
// What the pad draws and selects: MAT, then per button in file order the group its click selects
// (ut_panel panelButtonTarget: the button's first group).
std::vector<int> padClickStops(const utb::UtbSet& s) {
    std::vector<int> out(1, -1);
    for (const utb::UtbResolved& b : s.buttons) out.push_back(b.groups.empty() ? -2 : b.groups[0]);
    return out;
}
std::vector<int> wheelWalk(const utb::UtbSet& s, int from, int step, int count) {
    const int n = (int)s.buttonOfGroup.size();
    auto buttonOf = [&](int g) { return s.buttonOfGroup[(std::size_t)g]; };
    std::vector<int> out;
    int cur = from;
    for (int k = 0; k < count; ++k) out.push_back(cur = ut::utPadWheelButton(cur, step, n, buttonOf));
    return out;
}
// The wheel's stops are the pad's click stops; a full turn either way visits each exactly once and
// comes back; the back walk is the forward one reversed.
bool wheelIsPad(const utb::UtbSet& s) {
    const int n = (int)s.buttonOfGroup.size();
    auto buttonOf = [&](int g) { return s.buttonOfGroup[(std::size_t)g]; };
    const std::vector<int> pad = padClickStops(s);
    if (ut::utPadWheelStops(n, buttonOf) != pad) return false;
    const int turn = (int)pad.size();
    std::vector<int> fwd = wheelWalk(s, -1, 1, turn), back = wheelWalk(s, -1, -1, turn);
    std::vector<int> want(pad.begin() + 1, pad.end());
    want.push_back(-1);
    if (fwd != want) return false;
    std::vector<int> rev(pad.rbegin(), pad.rend() - 1);
    rev.push_back(-1);
    return back == rev;
}
std::string takeSection(const std::string& t, const std::string& tag) {
    const std::string eol = t.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    const std::size_t a = t.find("[" + tag + "]" + eol);
    if (a == std::string::npos) return std::string();
    std::size_t b = t.find(eol + "[", a + 1);
    b = b == std::string::npos ? t.size() : b + eol.size();
    return t.substr(a, b - a);
}
std::string moveSectionBefore(const std::string& t, const std::string& tag, const std::string& before) {
    const std::string sec = takeSection(t, tag);
    if (sec.empty()) return t;
    std::string out = removeSection(t, tag);
    const std::string eol = t.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    const std::size_t at = out.find("[" + before + "]" + eol);
    if (at == std::string::npos) return t;
    return out.insert(at, sec);
}
std::vector<std::string> lastTags(const utb::UtbSet& s, std::size_t k) {
    std::vector<std::string> out;
    for (std::size_t i = s.buttons.size() >= k ? s.buttons.size() - k : 0; i < s.buttons.size(); ++i)
        out.push_back(s.buttons[i].tag);
    return out;
}

void testWheelPadOrder(const std::string& root) {
    const int before = g_pass + g_fail;
    // DGD (the flag-off table a recorded file was written on: data\oracle\nomi)
    const std::string dgd = root + "/data/oracle/nomi/views/deathgrimdawn";
    const std::vector<utb::UtbGroupIn> g = fixtureGroups(dgd + "/uniq-groups.txt");
    const int n = (int)g.size();
    check(n == 29 && g[24].id == "relic" && g[25].id == "blueprint" && g[26].tag == "GEM" && g[27].tag == "AUG" &&
              g[28].tag == "OTH",
          "DGD's table: 24 REL, 25 BLU, 26 GEM, 27 AUG, 28 OTH");
    if (n != 29) return;
    std::string err;
    // the generated file (group order): the wheel is the group order it always was
    const std::string fresh = readAll(dgd + "/uniquetab-buttons.ini");
    utb::UtbSet s0;
    check(utb::utbResolve(mustParseB(fresh, "DGD's generated buttons file"), g, {}, s0, &err), "DGD's generated file resolves (" + err + ")");
    std::vector<int> ident(1, -1);
    for (int i = 0; i < n; ++i) ident.push_back(i);
    check(padClickStops(s0) == ident && wheelIsPad(s0) && wheelWalk(s0, 24, 1, 2) == std::vector<int>({25, 26}),
          "DGD's generated file (REL BLU GEM AUG OTH): the wheel walks the group order, REL -> BLU -> GEM");
    // the recorded file: written before blueprints, [BLU] appended once after the last button
    std::string once;
    check(offer(dropBlu(fresh), g, &once), "DGD: an old file gets [BLU] offered once");
    utb::UtbSet s;
    check(utb::utbResolve(mustParseB(once, "DGD's old file after the offer"), g, {}, s, &err) && s.unassigned.empty(),
          "DGD's old file after the offer resolves (" + err + ")");
    check(lastTags(s, 5) == std::vector<std::string>({"REL", "GEM", "AUG", "OTH", "BLU"}) &&
              s.buttonOfGroup[25] == (int)s.buttons.size() - 1,
          "the pad draws ... REL GEM AUG OTH BLU (the recorded file)");
    check(groupIndexWheel(24, 1, n, s) == 25 && groupIndexWheel(25, 1, n, s) == 26 && groupIndexWheel(28, 1, n, s) == -1,
          "BEFORE (the group-index walk): REL -> BLU -> GEM ... OTH -> materials - BLU visited before GEM (the bug)");
    check(wheelWalk(s, 24, 1, 6) == std::vector<int>({26, 27, 28, 25, -1, 0}),
          "AFTER: Ctrl+wheel down / Ctrl+PageDown from REL: GEM -> AUG -> OTH -> BLU -> materials -> HEL");
    check(wheelWalk(s, -1, -1, 5) == std::vector<int>({25, 28, 27, 26, 24}),
          "AFTER: Ctrl+wheel up / Ctrl+PageUp from materials: BLU -> OTH -> AUG -> GEM -> REL");
    check(wheelIsPad(s), "DGD, the recorded file: the wheel's stops are the pad's buttons, one full turn each way");
    check(wheelWalk(s, 28, 1, 1) == std::vector<int>({25}) && g[25].id == "blueprint",
          "the stop for BLU is group index 25 - what g_wantGroup and the state file's uniq_group hold, unchanged");
    // the user moves [BLU] back between REL and GEM by hand: the wheel follows the file
    const std::string moved = moveSectionBefore(once, "BLU", "GEM");
    utb::UtbSet sm;
    check(moved != once && utb::utbResolve(mustParseB(moved, "DGD, [BLU] moved before [GEM]"), g, {}, sm, &err) &&
              lastTags(sm, 5) == std::vector<std::string>({"REL", "BLU", "GEM", "AUG", "OTH"}) && wheelIsPad(sm) &&
              wheelWalk(sm, 24, 1, 2) == std::vector<int>({25, 26}),
          "[BLU] moved back before [GEM] in the file: REL -> BLU -> GEM, as the pad then draws it");
    // a reordered file: [OTH] first, [GEM] before [HEL], [RNG] = ring + amulet ([AMU] gone)
    std::string re = moveSectionBefore(moveSectionBefore(once, "OTH", "HEL"), "GEM", "HEL");
    re = removeSection(re, "AMU");
    const std::size_t rng = re.find("groups  = ring", re.find("[RNG]"));
    check(rng != std::string::npos, "the fixture's [RNG] groups line");
    if (rng == std::string::npos) return;
    re.replace(rng, 14, "groups  = ring, amulet");
    utb::UtbSet sr;
    check(utb::utbResolve(mustParseB(re, "DGD, a reordered user file"), g, {}, sr, &err) && sr.unassigned.empty() &&
              sr.buttons.size() >= 3 && sr.buttons[0].tag == "OTH" && sr.buttons[1].tag == "GEM" &&
              sr.buttons[2].tag == "HEL",
          "a reordered user file resolves: OTH, GEM, HEL ... (" + err + ")");
    int amulet = -1, ring = -1;
    for (int i = 0; i < n; ++i) {
        if (g[(std::size_t)i].id == "amulet") amulet = i;
        if (g[(std::size_t)i].id == "ring") ring = i;
    }
    const int lo = amulet < ring ? amulet : ring, hi = amulet < ring ? ring : amulet;
    check(amulet >= 0 && ring >= 0 && wheelIsPad(sr) && wheelWalk(sr, -1, 1, 3) == std::vector<int>({28, 26, 0}) &&
              wheelWalk(sr, 0, -1, 3) == std::vector<int>({26, 28, -1}),
          "the reordered file: materials -> OTH (28) -> GEM (26) -> HEL (0), and back - the file's order");
    const int rb = lo >= 0 ? sr.buttonOfGroup[(std::size_t)lo] : -1;
    auto srOf = [&](int grp) { return sr.buttonOfGroup[(std::size_t)grp]; };
    const std::vector<int> stops = ut::utPadWheelStops(n, srOf);
    int seenLo = 0, seenHi = 0;
    for (int x : stops) {
        seenLo += x == lo;
        seenHi += x == hi;
    }
    check(rb >= 0 && sr.buttonOfGroup[(std::size_t)hi] == rb && seenLo == 1 && seenHi == 0 &&
              ut::utPadWheelButton(hi, 1, n, srOf) == ut::utPadWheelButton(lo, 1, n, srOf) &&
              ut::utPadWheelButton(hi, -1, n, srOf) == ut::utPadWheelButton(lo, -1, n, srOf),
          "[RNG] = ring + amulet is ONE stop at its first group; from its other group the walk steps from [RNG]");

    // ROT, a recorded file: [BOW] removed, [SPE] = spear1h + staff,
    // the blueprints button appended once after the last button (... SPE REL JWL OTH BLU)
    const std::string rot = root + "/data/oracle/nomi/views/reignofterror";   // flag-off table
    const std::vector<utb::UtbGroupIn> gr = fixtureGroups(rot + "/uniq-groups.txt");
    check(gr.size() == 29 && gr[25].id == "relic" && gr[26].id == "blueprint" && gr[27].tag == "JWL" &&
              gr[28].tag == "OTH",
          "ROT's table: 25 REL, 26 BLU, 27 JWL, 28 OTH");
    if (gr.size() != 29) return;
    std::string ru;
    check(offer(dropBlu(readAll(rot + "/uniquetab-buttons.ini")), gr, &ru), "ROT: an old file gets [BLU] offered once");
    ru = removeSection(ru, "BOW");
    const std::size_t spe = ru.find("groups  = spear1h", ru.find("[SPE]"));
    check(spe != std::string::npos, "ROT: the fixture's [SPE] groups line");
    if (spe == std::string::npos) return;
    ru.replace(spe, 17, "groups  = spear1h, staff");
    utb::UtbSet srot;
    check(utb::utbResolve(mustParseB(ru, "ROT, the recorded file"), gr, {}, srot, &err) &&
              srot.unassigned.empty() &&
              lastTags(srot, 6) == std::vector<std::string>({"GN2", "SPE", "REL", "JWL", "OTH", "BLU"}),
          "ROT, the recorded file resolves: ... GN2 SPE REL JWL OTH BLU (" + err + ")");
    check(groupIndexWheel(25, 1, 29, srot) == 26 && groupIndexWheel(26, 1, 29, srot) == 27,
          "ROT BEFORE: REL -> BLU -> JWL (the group-index walk)");
    check(wheelWalk(srot, 22, 1, 6) == std::vector<int>({23, 25, 27, 28, 26, -1}) &&
              wheelWalk(srot, -1, -1, 6) == std::vector<int>({26, 28, 27, 25, 23, 22}) && wheelIsPad(srot),
          "ROT AFTER: GN2 -> SPE (23) -> REL -> JWL -> OTH -> BLU -> materials, and back");
    check(wheelWalk(srot, 24, 1, 1) == std::vector<int>({25}) && wheelWalk(srot, 24, -1, 1) == std::vector<int>({22}),
          "ROT: from Hunting Spears (24, [SPE]'s other group, a restored state) the walk steps from [SPE]");
    std::printf("[test] -- testWheelPadOrder: %d checks (Ctrl+wheel in the pad's order)\n", g_pass + g_fail - before);
}

// collect_mi=1 is the default, so data\oracle\views\deathgrimdawn is the flag-on cut - 30
// groups, the new one DGD's spear1h "Artifacts" (ART, its Rare crystal weapons). Its generated
// buttons file lists [ART] and resolves with nothing unassigned. A file written while the flag was
// off (the flag-off cut's generated file) has no [ART]; spear1h (and staff) are on the late
// list, so that file gets [ART] ONCE, after its last button, and the "added once" line remembers it
// (the rule for any built-in group a newer version adds).
// A file whose [unassigned] already lists spear1h (as an earlier build
// rewrote it there) keeps it there: the opt-out rule. And ROT: a file with no Bows / Hunting Spears
// button and no line for them gets both once; the default file names them and is offered nothing.
void testFlagOnArtifacts(const std::string& root) {
    const int before = g_pass + g_fail;
    const std::string on = root + "/data/oracle/views/deathgrimdawn";
    const std::string off = root + "/data/oracle/nomi/views/deathgrimdawn";
    const std::vector<utb::UtbGroupIn> g = fixtureGroups(on + "/uniq-groups.txt");
    check(g.size() == 30 && g[24].id == "spear1h" && g[24].tag == "ART" && g[25].id == "relic",
          "DGD's flag-on table: 30 groups, 24 ART (spear1h) before 25 REL");
    if (g.size() != 30) return;
    std::string err;
    const std::string fresh = readAll(on + "/uniquetab-buttons.ini");
    utb::UtbSet s;
    check(fresh.find("[ART]") != std::string::npos &&
              utb::utbResolve(mustParseB(fresh, "DGD's flag-on buttons file"), g, {}, s, &err) && s.unassigned.empty(),
          "DGD's flag-on generated file has [ART] and resolves with nothing unassigned (" + err + ")");
    const std::string old = readAll(off + "/uniquetab-buttons.ini");
    utb::UtbSet s2;
    check(old.find("[ART]") == std::string::npos &&
              utb::utbResolve(mustParseB(old, "DGD's flag-off buttons file"), g, {}, s2, &err) &&
              s2.unassigned == std::vector<std::string>({"spear1h"}),
          "a DGD file written with the flag off has no [ART]: spear1h is on no button");
    std::string once;
    std::vector<std::string> added, refused;
    check(offer(old, g, &once, &added, &refused) && added == std::vector<std::string>({"spear1h"}) && refused.empty(),
          "that file is offered spear1h once (the late list has it now)");
    const std::string sec = "[ART]\r\nname    = Artifacts\r\ngroups  = spear1h\r\n\r\n";
    const std::string oldMk = std::string(utb::utbOfferedPrefix()) + " blueprint\r\n";
    const std::string newMk = std::string(utb::utbOfferedPrefix()) + " blueprint, spear1h\r\n";
    std::string back = once;
    const std::size_t at = back.find(sec);
    if (at != std::string::npos) back.erase(at, sec.size());
    const std::size_t ma = back.find(newMk);
    if (ma != std::string::npos) back.replace(ma, newMk.size(), oldMk);
    check(at != std::string::npos && once.compare(at + sec.size(), 12, "[unassigned]") == 0 &&
              ma != std::string::npos && back == old,
          "[ART] after the last button, the line now 'blueprint, spear1h', every other byte kept (CRLF)");
    utb::UtbSet s3;
    check(utb::utbResolve(mustParseB(once, "DGD's old file after the offer"), g, {}, s3, &err) && s3.unassigned.empty() &&
              s3.buttons.back().tag == "ART" && s3.buttons.back().name == "Artifacts" &&
              s3.buttonOfGroup[24] == (int)s3.buttons.size() - 1,
          "it resolves with nothing unassigned: ART is the last button, its group 24 (" + err + ")");
    std::string out, rw;
    check(!offer(once, g, &out) && out == once, "a second load adds nothing");
    const std::string deleted = removeSection(once, "ART");
    utb::UtbSet sd;
    check(deleted != once && !offer(deleted, g, &out) &&
              utb::utbResolve(mustParseB(deleted, "the file without [ART]"), g, {}, sd, &err) &&
              sd.unassigned == std::vector<std::string>({"spear1h"}) &&
              utb::utbRewriteUnassigned(deleted, sd.unassigned, &rw) && rw.find(newMk) != std::string::npos &&
              rw.find("groups = spear1h\r\n") != std::string::npos && !offer(rw, g, &out),
          "[ART] deleted by the user: not offered again, the rewrite lists spear1h, the line stays");
    // the 18:04 build's start: [unassigned] already rewritten with spear1h -> the opt-out, nothing offered
    utb::UtbSet so;
    std::string listed;
    check(utb::utbResolve(mustParseB(old, "DGD's flag-off file"), g, {}, so, &err) &&
              utb::utbRewriteUnassigned(old, so.unassigned, &listed) &&
              listed.find("groups = spear1h\r\n") != std::string::npos && !offer(listed, g, &out),
          "a file whose [unassigned] already lists spear1h keeps it there (add [ART] by hand)");

    // ROT (flag on): the default file names staff and spear1h; a file without their buttons and
    // without the line gets [BOW] and [SPE] once, in the late list's order, after the last button
    const std::string rot = root + "/data/oracle/views/reignofterror";
    const std::vector<utb::UtbGroupIn> gr = fixtureGroups(rot + "/uniq-groups.txt");
    const std::string rfresh = readAll(rot + "/uniquetab-buttons.ini");
    check(utb::utbOfferedIds(rfresh) == std::vector<std::string>({"blueprint", "staff", "spear1h"}) &&
              !offer(rfresh, gr, &out),
          "ROT's default file: the line names blueprint, staff, spear1h; nothing is offered");
    check(!offer(removeSection(rfresh, "BOW"), gr, &out),
          "ROT's default file with [BOW] deleted: staff is not offered back (the line names it)");
    const std::string rold = removeSection(removeSection(dropBlu(rfresh), "BOW"), "SPE");
    added.clear();
    refused.clear();
    std::string ronce;
    check(offer(rold, gr, &ronce, &added, &refused) &&
              added == std::vector<std::string>({"blueprint", "staff", "spear1h"}) && refused.empty() &&
              utb::utbOfferedIds(ronce) == std::vector<std::string>({"blueprint", "staff", "spear1h"}),
          "a ROT file with no BLU, BOW, SPE and no line: all three offered once, the line as the default's");
    utb::UtbSet sr;
    check(utb::utbResolve(mustParseB(ronce, "ROT's file after the offer"), gr, {}, sr, &err) && sr.unassigned.empty() &&
              lastTags(sr, 3) == std::vector<std::string>({"BLU", "BOW", "SPE"}) && !offer(ronce, gr, &out),
          "it resolves: ... BLU BOW SPE last, nothing unassigned, a second load adds nothing (" + err + ")");
    printf("[test] buttons: the flag-on DGD table, [ART] and ROT's per-class groups offered once: %d checks\n",
           g_pass + g_fail - before);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: test_buttons <scratch dir> [src\\ut_textcfg.h] [src\\ut_buttons.h] [repo root]\n");
        return 2;
    }
    testUtf8();
    std::printf("[test] -- %s\n", "testParse");
    std::fflush(stdout);
    testParse();
    std::printf("[test] -- %s\n", "testRefusals");
    std::fflush(stdout);
    testRefusals();
    std::printf("[test] -- %s\n", "testTemplates");
    std::fflush(stdout);
    testTemplates();
    std::printf("[test] -- %s\n", "testFit");
    std::fflush(stdout);
    testFit();
    std::printf("[test] -- %s\n", "testNothingMoved");
    std::fflush(stdout);
    testNothingMoved();
    std::printf("[test] -- %s\n", "testChain");
    std::fflush(stdout);
    testChain();
    std::printf("[test] -- %s\n", "testBuiltins");
    std::fflush(stdout);
    testBuiltins();
    std::printf("[test] -- %s\n", "testReload");
    std::fflush(stdout);
    testReload();
    std::printf("[test] -- %s\n", "testFuzz");
    std::fflush(stdout);
    testFuzz();
    std::printf("[test] -- %s\n", "testGameWords");
    std::fflush(stdout);
    testGameWords();
    std::printf("[test] -- %s\n", "testLivePad");
    std::fflush(stdout);
    testLivePad();
    std::printf("[test] -- %s\n", "testLabelCompose");
    std::fflush(stdout);
    testLabelCompose();
    std::printf("[test] -- %s\n", "testFontRoles");
    std::fflush(stdout);
    testFontRoles();
    if (argc > 2) testPurity(argv[2]);
    std::printf("[test] -- %s\n", "testButtonsParse");
    std::fflush(stdout);
    testButtonsParse();
    std::printf("[test] -- %s\n", "testButtonsRefusals");
    std::fflush(stdout);
    testButtonsRefusals();
    std::printf("[test] -- %s\n", "testResolve");
    std::fflush(stdout);
    testResolve();
    std::printf("[test] -- %s\n", "testUnassigned");
    std::fflush(stdout);
    testUnassigned();
    std::printf("[test] -- %s\n", "testPadByButtons");
    std::fflush(stdout);
    testPadByButtons();
    std::printf("[test] -- %s\n", "testOneGridPure");
    std::fflush(stdout);
    testOneGridPure();
    std::printf("[test] -- %s\n", "testFold");
    std::fflush(stdout);
    testFold();
    std::printf("[test] -- %s\n", "testDerive");
    std::fflush(stdout);
    testDerive();
    testDeriveDynamic();
    if (argc > 3) testButtonsPurity(argv[3]);
    if (argc > 4) testDefaultAndIdentity(argv[4]);
    if (argc > 4) {
        const int before = g_pass + g_fail;
        testOfferNew(argv[4]);
        std::printf("[test] -- testOfferNew: %d checks (a new built-in group's button, once)\n",
                    g_pass + g_fail - before);
    }
    else std::printf("[test] SKIPPED the fixtures: no repo root given\n");
    if (argc > 4) {
        std::printf("[test] -- %s\n", "testOneGridRot");
        std::fflush(stdout);
        testOneGridRot(argv[4]);
        testWheelPadOrder(argv[4]);
        testFlagOnArtifacts(argv[4]);
    }
    std::printf("[test] buttons: %d checks, %d failed\n", g_pass + g_fail, g_fail);
    if (g_fail) { std::printf("[test] FAILED\n"); return 1; }
    std::printf("[test] ALL PASS\n");
    return 0;
}
