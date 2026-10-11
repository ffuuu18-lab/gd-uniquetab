// ut_textcfg.h - the words of the tab: the label templates, the fallback chain, the UTF-8 helper,
// the [style] keys - and the parser of the RETIRED strings file uniquetab-text.ini, kept to fold an
// old file into the buttons file once (ut_buttons.h utbFoldText). The buttons file's [tab] keys
// are this file's [text] / [style] keys one to one (utbTabKeyToText), so the chain below serves it.
//
// Pure C++17: no Windows, no engine, no globals, no other project header. The engine side
// supplies two callbacks (does a font have a glyph; the game's own tag for an id) and the file's
// bytes; tools\test_textcfg.cpp links this very header. Meant to be copied verbatim (Titan Quest).
//
// THE FILE (uniquetab-text.ini beside uniquetab.ini; UTF-8, an optional BOM, CRLF or LF):
//   ; comment            a line whose first non-blank character is ';' or '#'. There are NO
//                        inline comments: ';' and '#' are ordinary characters inside a value.
//   [text]               keys that always apply, whatever language the game runs in
//   [text.<lang>]        keys of one language ([a-z0-9_-]{1,16}, any case); applied ONLY when
//                        [style] language = <lang> names it - never picked by the game's own
//                        language - and then it wins over [text] key by key
//   [style]              the language section to apply, fonts, sizes and colours
//   key = value          blanks around both are trimmed; "value" in double quotes keeps them.
//                        One line per value; no escapes.
//
// THE KEYS ([text] and [text.<lang>]; <id> = a category id, grammar [a-z0-9_.-]{1,40}: the 24
// slot words, vanilla, owned, other, next, auto.<class>, <modkey>.<name>; an id no group
// of the current view has is accepted, because the one file serves every view):
//   caption.<id>         the pad caption, 1..12 UTF-16 units
//   name.<id>            the group name ({group} in the label), 1..48 units; the value @game
//                        means the game's own word for that group in the game's language where
//                        an exact tag exists (utTextGameTagFor), else the next source
//   name.*               only = @game: the same for every group without a name.<id> of its own
//                        in that section
//   label.format.1..4    the label templates, tried in order (1..160 units); named
//                        placeholders only: {mode} {group} {owned} {entries} {first} {last}
//                        {rows} {all} {coll}; {{ and }} are literal braces. Never a printf
//                        format. The list is taken whole from the most specific section that
//                        sets any of the four.
//   label.mode_unknown   the {mode} text while the mode is unknown, 0..48 units
//   label.unknown        a count the mod does not have, 1..8 units
//   tooltip.yes / .no    the rollover line, 1..96 units
//   tooltip.roll         the words after tooltip.yes on a monster infrequent the collection holds
//                        in another roll, 1..96 units, appended as written (built in
//                        " (another roll)" - quote a value to keep its leading blank)
// [style]:
//   language             the [text.<lang>] section to apply ([a-z0-9_-]{1,16}, any case);
//                        unset = no language section applies (they are ignored, one DEBUG line)
//   font.caption / font.label    fonts/<name>.fnt, [a-z0-9 _./-], no "..", 8..96 bytes; unset =
//                        the root font fonts/savapromedium.fnt in every game language
//   size.label           8..32 record px (overrides plate_label_size of uniquetab.ini)
//   size.caption_max     6..36 record px (overrides pad_caption_max of uniquetab.ini)
//   size.caption         0 = auto, else 6..36 record px (still bounded by the cell)
//   color.caption / color.label / color.ring / color.mark    RRGGBB or #RRGGBB
//
// ANY error refuses the WHOLE file and names the line: not UTF-8, a control character, a line
// that is neither a comment, a section nor key = value, a key outside a section, an unknown
// section or key, a key set twice in one section, an empty value, an unbalanced quote, a value
// too long or too short, an unknown placeholder or an unbalanced brace, a font path outside
// fonts/, a colour that is not six hex digits, a size outside its range. The caller then uses
// the built-in strings (and logs the error once per file load).
//
// THE POLICY: the tab is English by default in every language of the game; the user's file
// changes that, nothing else does. The game's language never picks a section and the game's own
// words are used only where the file says @game.
//
// THE CHAIN (utTextResolve), per key: [text.<lang>] when [style] language = <lang> -> [text]
// (in each the exact key, then name.* for a name; a value of @game is the game's own word, or
// nothing) -> the category's own text (the G line's label or tag; skipped when it equals the
// built-in default) -> the built-in English -> derived from the id. Every candidate must be
// valid for its key and every code point must pass the glyph callback for the font that draws
// it, or the chain goes on; the first candidate passed over is reported once.
#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace utt {

const std::size_t kNpos = static_cast<std::size_t>(-1);

// ================================================================= UTF-8
// One code point at s[0..n): its length 1..4, or 0 when the bytes are not well-formed UTF-8
// (a stray continuation byte, an overlong form, a surrogate, a value above U+10FFFF, a truncated
// sequence).
inline int utf8Decode(const unsigned char* s, std::size_t n, char32_t* cp) {
    if (n == 0) return 0;
    const unsigned c = s[0];
    if (c < 0x80) { if (cp) *cp = c; return 1; }
    int len = 0;
    unsigned lo = 0x80, hi = 0xBF;   // the allowed range of the SECOND byte
    char32_t v = 0;
    if (c >= 0xC2 && c <= 0xDF) { len = 2; v = c & 0x1F; }
    else if (c == 0xE0) { len = 3; v = 0; lo = 0xA0; }
    else if ((c >= 0xE1 && c <= 0xEC) || c == 0xEE || c == 0xEF) { len = 3; v = c & 0x0F; }
    else if (c == 0xED) { len = 3; v = 0x0D; hi = 0x9F; }
    else if (c == 0xF0) { len = 4; v = 0; lo = 0x90; }
    else if (c >= 0xF1 && c <= 0xF3) { len = 4; v = c & 0x07; }
    else if (c == 0xF4) { len = 4; v = 4; hi = 0x8F; }
    else return 0;
    if (n < static_cast<std::size_t>(len)) return 0;
    for (int i = 1; i < len; ++i) {
        const unsigned b = s[i];
        if (i == 1 ? (b < lo || b > hi) : (b < 0x80 || b > 0xBF)) return 0;
        v = (v << 6) | (b & 0x3F);
    }
    if (cp) *cp = v;
    return len;
}

// The code points of s; false (and `out` partial) on any ill-formed byte.
inline bool utf8Decode(const std::string& s, std::u32string& out) {
    out.clear();
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
    std::size_t i = 0;
    while (i < s.size()) {
        char32_t cp = 0;
        const int len = utf8Decode(p + i, s.size() - i, &cp);
        if (!len) return false;
        out += cp;
        i += static_cast<std::size_t>(len);
    }
    return true;
}

inline bool utf8Valid(const std::string& s) {
    std::u32string cps;
    return utf8Decode(s, cps);
}

// Length in code points; kNpos when s is not UTF-8.
inline std::size_t utf8Length(const std::string& s) {
    std::u32string cps;
    return utf8Decode(s, cps) ? cps.size() : kNpos;
}

// Length in UTF-16 units (a code point above U+FFFF takes two); kNpos when s is not UTF-8.
inline std::size_t utf16Units(const std::string& s) {
    std::u32string cps;
    if (!utf8Decode(s, cps)) return kNpos;
    std::size_t n = 0;
    for (char32_t c : cps) n += c > 0xFFFF ? 2 : 1;
    return n;
}

// Appends cp as UTF-8; false (nothing appended) for a surrogate or a value above U+10FFFF.
inline bool utf8Append(char32_t cp, std::string& out) {
    if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) return false;
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return true;
}

// UTF-8 -> UTF-16 (what the engine's wide text call takes); false on ill-formed input.
inline bool utf8ToUtf16(const std::string& s, std::u16string& out) {
    out.clear();
    std::u32string cps;
    if (!utf8Decode(s, cps)) return false;
    for (char32_t c : cps) {
        if (c > 0xFFFF) {
            const char32_t v = c - 0x10000;
            out += static_cast<char16_t>(0xD800 + (v >> 10));
            out += static_cast<char16_t>(0xDC00 + (v & 0x3FF));
        } else {
            out += static_cast<char16_t>(c);
        }
    }
    return true;
}

// UTF-16 -> UTF-8; false on a lone surrogate.
inline bool utf16ToUtf8(const std::u16string& s, std::string& out) {
    out.clear();
    for (std::size_t i = 0; i < s.size(); ++i) {
        char32_t c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF) {
            if (i + 1 >= s.size() || s[i + 1] < 0xDC00 || s[i + 1] > 0xDFFF) return false;
            c = 0x10000 + ((c - 0xD800) << 10) + (static_cast<char32_t>(s[i + 1]) - 0xDC00);
            ++i;
        } else if (c >= 0xDC00 && c <= 0xDFFF) {
            return false;
        }
        utf8Append(c, out);
    }
    return true;
}

// The first `codePoints` code points of a valid UTF-8 string (never cuts inside a sequence).
inline std::string utf8Prefix(const std::string& s, std::size_t codePoints) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
    std::size_t i = 0;
    for (std::size_t n = 0; n < codePoints && i < s.size(); ++n) {
        const int len = utf8Decode(p + i, s.size() - i, nullptr);
        if (!len) break;
        i += static_cast<std::size_t>(len);
    }
    return s.substr(0, i);
}

// ================================================================= measuring callbacks
// The width of a UTF-8 string in screen px (the engine side: the live font's pen rule; the test
// and the fallback: ut_fontmetrics.h).
typedef int (*UtTextWidthFn)(const std::string& utf8, const void* ctx);

// Cuts s (valid UTF-8) at a code point boundary until widthFn says it fits `limitPx`, then trims
// a dangling separator (trailing blanks and '-'), the way 1.1.0 trims its label. Empty when not
// even one code point fits.
inline std::string utTextClipToWidth(const std::string& s, int limitPx, UtTextWidthFn widthFn,
                                     const void* ctx) {
    std::size_t n = utf8Length(s);
    if (n == kNpos) return std::string();
    std::string cut = s;
    while (n > 0 && widthFn(cut, ctx) > limitPx) cut = utf8Prefix(s, --n);
    while (!cut.empty() && (cut.back() == ' ' || cut.back() == '-')) cut.pop_back();
    return cut;
}

// ================================================================= ids and keys
const std::size_t kIdMax = 40;

// Category id grammar (the groups file's): [a-z0-9_.-]{1,40}.
inline bool utTextValidId(const std::string& s) {
    if (s.empty() || s.size() > kIdMax) return false;
    for (unsigned char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-'))
            return false;
    return true;
}

inline std::string utTextLower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

enum UtTextKind {
    kTextCaption = 0,     // caption.<id>
    kTextName,            // name.<id>
    kTextFormat,          // label.format.1..4
    kTextModeUnknown,     // label.mode_unknown
    kTextUnknown,         // label.unknown
    kTextTooltipYes,      // tooltip.yes
    kTextTooltipNo,       // tooltip.no
    kTextTooltipRoll,     // tooltip.roll (the "(another roll)" words of an MI's third line)
    kTextKindCount
};

// The font that draws a kind; the glyph callback is asked against it.
enum UtTextFont { kFontCaption = 0, kFontLabel, kFontTooltip };

inline UtTextFont utTextFontOf(UtTextKind k) {
    if (k == kTextCaption) return kFontCaption;
    if (k == kTextTooltipYes || k == kTextTooltipNo || k == kTextTooltipRoll) return kFontTooltip;
    return kFontLabel;
}

// Length limits in UTF-16 units.
inline void utTextLimits(UtTextKind k, std::size_t* minU, std::size_t* maxU) {
    static const std::size_t kMin[kTextKindCount] = {1, 1, 1, 0, 1, 1, 1, 1};
    static const std::size_t kMax[kTextKindCount] = {12, 48, 160, 48, 8, 96, 96, 96};
    *minU = kMin[k];
    *maxU = kMax[k];
}

const int kFormatMax = 4;   // label.format.1 .. label.format.4

// The one value that is not literal text: under name.<id> (or name.*) it asks for the game's own
// word for the group, in the game's language (an explicit opt-in - never used without it).
const char* const kTextGameValue = "@game";

// A [text] key -> its kind and its id (caption/name; "*" for name.*) or "1".."4" (format). The key
// is compared lower-cased.
inline bool utTextParseKey(const std::string& key, UtTextKind* kind, std::string* arg) {
    const std::string k = utTextLower(key);
    arg->clear();
    if (k.compare(0, 8, "caption.") == 0 && utTextValidId(k.substr(8))) {
        *kind = kTextCaption; *arg = k.substr(8); return true;
    }
    if (k.compare(0, 5, "name.") == 0 && utTextValidId(k.substr(5))) {
        *kind = kTextName; *arg = k.substr(5); return true;
    }
    if (k == "name.*") { *kind = kTextName; *arg = "*"; return true; }   // only = @game
    if (k.size() == 14 && k.compare(0, 13, "label.format.") == 0 && k[13] >= '1'
        && k[13] < '1' + kFormatMax) {
        *kind = kTextFormat; *arg = k.substr(13); return true;
    }
    if (k == "label.mode_unknown") { *kind = kTextModeUnknown; return true; }
    if (k == "label.unknown") { *kind = kTextUnknown; return true; }
    if (k == "tooltip.yes") { *kind = kTextTooltipYes; return true; }
    if (k == "tooltip.no") { *kind = kTextTooltipNo; return true; }
    if (k == "tooltip.roll") { *kind = kTextTooltipRoll; return true; }
    return false;
}

// ================================================================= the label templates
enum UtLabelSlot {
    kSlotMode = 0, kSlotGroup, kSlotOwned, kSlotEntries, kSlotFirst, kSlotLast, kSlotRows,
    kSlotAll, kSlotColl, kSlotCount
};
inline const char* utLabelSlotName(int i) {
    static const char* const kNames[kSlotCount] = {"mode", "group", "owned", "entries", "first",
                                                   "last", "rows", "all", "coll"};
    return i >= 0 && i < kSlotCount ? kNames[i] : "";
}
inline int utLabelSlotOf(const std::string& name) {
    for (int i = 0; i < kSlotCount; ++i) if (name == utLabelSlotName(i)) return i;
    return -1;
}

struct UtLabelValues {
    std::string v[kSlotCount];
};

// The values exactly as 1.1.0 prints them: counts with %d, the two owned counts as `unknown`
// when negative, {mode} empty while the mode is known.
inline UtLabelValues utLabelValues(bool modeKnown, const std::string& modeUnknownText,
                                   const std::string& group, int owned, int entries, int first,
                                   int last, int rows, int ownedAll, int collection,
                                   const std::string& unknownText) {
    UtLabelValues lv;
    lv.v[kSlotMode] = modeKnown ? std::string() : modeUnknownText;
    lv.v[kSlotGroup] = group;
    lv.v[kSlotOwned] = owned >= 0 ? std::to_string(owned) : unknownText;
    lv.v[kSlotEntries] = std::to_string(entries);
    lv.v[kSlotFirst] = std::to_string(first);
    lv.v[kSlotLast] = std::to_string(last);
    lv.v[kSlotRows] = std::to_string(rows);
    lv.v[kSlotAll] = ownedAll >= 0 ? std::to_string(ownedAll) : unknownText;
    lv.v[kSlotColl] = std::to_string(collection);
    return lv;
}

// Walks a template: literal text into `literal` (braces unescaped), every placeholder through
// `slots`. False with `why` on an unknown placeholder or an unbalanced brace.
inline bool utTextTemplateWalk(const std::string& t, const UtLabelValues* values, std::string* out,
                               std::string* literal, std::vector<int>* slots, std::string* why) {
    for (std::size_t i = 0; i < t.size(); ++i) {
        const char c = t[i];
        if (c == '{' && i + 1 < t.size() && t[i + 1] == '{') {
            if (out) *out += '{';
            if (literal) *literal += '{';
            ++i;
        } else if (c == '}' && i + 1 < t.size() && t[i + 1] == '}') {
            if (out) *out += '}';
            if (literal) *literal += '}';
            ++i;
        } else if (c == '{') {
            const std::size_t close = t.find('}', i + 1);
            const std::size_t reopen = t.find('{', i + 1);
            if (close == std::string::npos || (reopen != std::string::npos && reopen < close)) {
                if (why) *why = "an unbalanced '{' (write {{ for a literal brace)";
                return false;
            }
            const std::string name = t.substr(i + 1, close - i - 1);
            const int slot = utLabelSlotOf(name);
            if (slot < 0) {
                if (why) *why = "an unknown placeholder {" + name + "}";
                return false;
            }
            if (slots) slots->push_back(slot);
            if (out && values) *out += values->v[slot];
            i = close;
        } else if (c == '}') {
            if (why) *why = "an unbalanced '}' (write }} for a literal brace)";
            return false;
        } else {
            if (out) *out += c;
            if (literal) *literal += c;
        }
    }
    return true;
}

inline bool utTextTemplateCheck(const std::string& t, std::string* why) {
    return utTextTemplateWalk(t, nullptr, nullptr, nullptr, nullptr, why);
}

// The template with every placeholder replaced; false (out empty) on a bad template.
inline bool utTextExpand(const std::string& t, const UtLabelValues& v, std::string& out) {
    out.clear();
    if (utTextTemplateWalk(t, &v, &out, nullptr, nullptr, nullptr)) return true;
    out.clear();
    return false;
}

// Which expanded line to draw: the first that fits; when none fits, the FIRST one (the caller
// clips it by width - 1.1.0 clips its full form, not the short one, and the default English
// install must not move a pixel). `fits` says whether the pick fits as it is.
inline int utLabelPick(const std::vector<std::string>& lines, int limitPx, UtTextWidthFn widthFn,
                       const void* ctx, bool* fits) {
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (widthFn(lines[i], ctx) <= limitPx) {
            if (fits) *fits = true;
            return static_cast<int>(i);
        }
    }
    if (fits) *fits = false;
    return lines.empty() ? -1 : 0;
}

// ================================================================= the built-in strings
struct UtTextBuiltin { const char* id; const char* caption; const char* name; };

// The 26 pad buttons in pad order (vanilla, the 24 slot groups, owned), then the generator's own
// group. Captions and names are 1.1.0's (kButtons, the G line labels, the groups file's other). The paged pad's NEXT button (id `next`) is utTextBuiltinNext, outside the table.
inline const UtTextBuiltin* utTextBuiltins(std::size_t* count) {
    static const UtTextBuiltin kTable[] = {
        {"vanilla", "MAT", "Crafting Materials"},
        {"head", "HEL", "Helms"},           {"shoulders", "SHO", "Shoulders"},
        {"chest", "CHE", "Chest"},          {"hands", "GLO", "Gloves"},
        {"waist", "BEL", "Belts"},          {"legs", "PAN", "Pants"},
        {"feet", "BOO", "Boots"},           {"amulet", "AMU", "Amulets"},
        {"medal", "MED", "Medals"},         {"ring", "RNG", "Rings"},
        {"offhand", "OFF", "Off-hands"},    {"shield", "SHD", "Shields"},
        {"axe1h", "AX1", "1H Axes"},        {"dagger", "DAG", "Daggers"},
        {"mace1h", "MC1", "1H Maces"},      {"scepter", "SCP", "Scepters"},
        {"sword1h", "SW1", "1H Swords"},    {"ranged1h", "GUN", "Guns"},
        {"axe2h", "AX2", "2H Axes"},        {"mace2h", "MC2", "2H Maces"},
        {"spear2h", "SPR", "Spears"},       {"sword2h", "SW2", "2H Swords"},
        {"ranged2h", "GN2", "2H Guns"},     {"relic", "REL", "Relics"},
        {"owned", "OWN", "Owned only"},
        {"other", "OTH", "Other"},
    };
    if (count) *count = sizeof kTable / sizeof *kTable;
    return kTable;
}
const std::size_t kTextPadButtons = 26;   // the first 26 rows of utTextBuiltins

// The NEXT button of a paged pad: caption.next.
inline const UtTextBuiltin* utTextBuiltinNext() {
    static const UtTextBuiltin kNext = {"next", ">>", "Next page"};
    return &kNext;
}

inline const UtTextBuiltin* utTextBuiltinById(const std::string& id) {
    std::size_t n = 0;
    const UtTextBuiltin* t = utTextBuiltins(&n);
    for (std::size_t i = 0; i < n; ++i) if (id == t[i].id) return &t[i];
    if (id == utTextBuiltinNext()->id) return utTextBuiltinNext();
    return nullptr;
}

// 1.1.0's two label forms as templates; byte-identical output to its two printf formats.
inline const char* utTextDefaultFormat(int i) {
    static const char* const kFormats[] = {
        "{mode}{group} - {owned}/{entries} owned - row {first}-{last}/{rows} - coll {all}/{coll}",
        "{mode}{group} - {owned}/{entries} - row {first}-{last}/{rows} - coll {all}/{coll}",
    };
    return i >= 0 && i < 2 ? kFormats[i] : nullptr;
}
const int kTextDefaultFormats = 2;

// The built-in English text of a key without an id; nullptr for caption/name/format.
inline const char* utTextBuiltinPlain(UtTextKind k) {
    switch (k) {
    case kTextModeUnknown: return "mode unknown - ";
    case kTextUnknown: return "?";
    case kTextTooltipYes: return "In your collection";
    case kTextTooltipNo: return "Not in your collection";
    case kTextTooltipRoll: return " (another roll)";
    default: return nullptr;
    }
}

// The last part of an id (after its last '.').
inline std::string utTextIdTail(const std::string& id) {
    const std::size_t dot = id.rfind('.');
    return dot == std::string::npos ? id : id.substr(dot + 1);
}

// Step 6 for an id nothing names: the caption is the first three letters or digits of the id's
// last part, upper-cased (padded with X); the name is that part with '_'/'-' as blanks and the
// first letter upper-cased.
inline std::string utTextDerivedCaption(const std::string& id) {
    std::string t;
    for (unsigned char c : utTextIdTail(id)) {
        if (t.size() == 3) break;
        if (c >= 'a' && c <= 'z') t += static_cast<char>(c - 'a' + 'A');
        else if (c >= '0' && c <= '9') t += static_cast<char>(c);
    }
    while (t.size() < 3) t += 'X';
    return t;
}
inline std::string utTextDerivedName(const std::string& id) {
    std::string n = utTextIdTail(id);
    for (char& c : n) if (c == '_' || c == '-') c = ' ';
    if (n.empty()) n = "Other";
    if (n[0] >= 'a' && n[0] <= 'z') n[0] = static_cast<char>(n[0] - 'a' + 'A');
    return n;
}

// Makes derived captions distinct: a derived caption equal to any caption before it, or to a
// later non-derived one, gets its last character replaced by 0..9 then A..Z. Captions the user
// or the game supplied are never changed (two equal ones are a WARN, not an error: the pad
// clicks by index).
inline void utTextDistinctDerived(std::vector<std::string>& caps, const std::vector<bool>& derived) {
    static const char kAlt[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    for (std::size_t i = 0; i < caps.size(); ++i) {
        if (i >= derived.size() || !derived[i] || caps[i].empty()) continue;
        auto taken = [&](const std::string& c) {
            for (std::size_t j = 0; j < caps.size(); ++j) {
                if (j == i) continue;
                if (j < i || (j < derived.size() && !derived[j])) { if (caps[j] == c) return true; }
            }
            return false;
        };
        std::string c = caps[i];
        for (std::size_t n = 0; taken(c) && n < sizeof kAlt - 1; ++n)
            c = caps[i].substr(0, caps[i].size() - 1) + kAlt[n];
        caps[i] = c;
    }
}

// Index pairs (i < j) of equal captions, for the one WARN.
inline std::vector<std::pair<std::size_t, std::size_t>> utTextDuplicates(
    const std::vector<std::string>& caps) {
    std::vector<std::pair<std::size_t, std::size_t>> d;
    for (std::size_t i = 0; i < caps.size(); ++i)
        for (std::size_t j = i + 1; j < caps.size(); ++j)
            if (!caps[i].empty() && caps[i] == caps[j]) d.push_back({i, j});
    return d;
}

// ================================================================= [style]
enum UtStyleFont { kStyleFontCaption = 0, kStyleFontLabel, kStyleFontCount };
enum UtStyleSize { kStyleSizeLabel = 0, kStyleSizeCaptionMax, kStyleSizeCaption, kStyleSizeCount };
enum UtStyleColor { kStyleColorCaption = 0, kStyleColorLabel, kStyleColorRing, kStyleColorMark,
                    kStyleColorCount };

inline const char* utStyleFontKey(int i) {
    static const char* const k[kStyleFontCount] = {"font.caption", "font.label"};
    return k[i];
}
inline const char* utStyleSizeKey(int i) {
    static const char* const k[kStyleSizeCount] = {"size.label", "size.caption_max", "size.caption"};
    return k[i];
}
inline const char* utStyleColorKey(int i) {
    static const char* const k[kStyleColorCount] = {"color.caption", "color.label", "color.ring",
                                                    "color.mark"};
    return k[i];
}
// The size ranges; size.caption also takes 0 (auto).
inline void utStyleSizeRange(int i, int* lo, int* hi) {
    static const int kLo[kStyleSizeCount] = {8, 6, 6};
    static const int kHi[kStyleSizeCount] = {32, 36, 36};
    *lo = kLo[i];
    *hi = kHi[i];
}
// The built-in defaults as the template shows them (0xRRGGBB). The live side keeps its own
// float constants while a key is unset; these are only what the file documents.
inline const char* utStyleDefaultText(const std::string& key) {
    if (key == "font.caption" || key == "font.label") return "fonts/savapromedium.fnt";
    if (key == "size.label") return "13";
    if (key == "size.caption_max") return "14";
    if (key == "size.caption") return "0";
    if (key == "color.caption") return "E8EAED";
    if (key == "color.label") return "DBC794";
    if (key == "color.ring") return "3FD46A";
    if (key == "color.mark") return "3A6288";
    return nullptr;
}

struct UtTextStyle {
    std::string language;                    // [text.<language>] to apply, lower-cased; "" = none
    std::string font[kStyleFontCount];       // empty = unset
    int size[kStyleSizeCount] = {-1, -1, -1};   // -1 = unset
    long color[kStyleColorCount] = {-1, -1, -1, -1};   // 0xRRGGBB, -1 = unset
};

// The size in force: the text file's when set, else the main ini's.
inline int utStyleSizeOr(const UtTextStyle& s, UtStyleSize which, int iniValue) {
    return s.size[which] >= 0 ? s.size[which] : iniValue;
}

// fonts/<name>.fnt: [a-z0-9 _./-] (compared lower-cased), no "..", no "//", 8..96 bytes.
inline bool utStyleFontOk(const std::string& v, std::string* norm) {
    const std::string p = utTextLower(v);
    if (p.size() < 8 || p.size() > 96) return false;
    if (p.compare(0, 6, "fonts/") != 0 || p.compare(p.size() - 4, 4, ".fnt") != 0) return false;
    if (p.find("..") != std::string::npos || p.find("//") != std::string::npos) return false;
    if (p[p.size() - 5] == '/') return false;   // fonts/.fnt, fonts/ru/.fnt
    for (unsigned char c : p)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' || c == '_' || c == '.'
              || c == '/' || c == '-'))
            return false;
    if (norm) *norm = p;
    return true;
}

inline bool utStyleColorParse(const std::string& v, long* rgb) {
    std::string h = v;
    if (!h.empty() && h[0] == '#') h = h.substr(1);
    if (h.size() != 6) return false;
    long r = 0;
    for (char c : h) {
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        r = r * 16 + d;
    }
    *rgb = r;
    return true;
}

// ================================================================= the parsed file
struct UtTextValue {
    std::string text;
    int line = 0;
};
typedef std::map<std::string, UtTextValue> UtTextSection;   // normalised key -> value

struct UtTextFile {
    UtTextSection common;                        // [text]
    std::map<std::string, UtTextSection> lang;   // [text.<lang>], keyed lower-cased
    UtTextStyle style;                           // [style]
    int keys = 0;                                // key lines read
};

inline std::string utTextTrim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return s.substr(b, e - b);
}

inline bool utTextValidLang(const std::string& s) {
    if (s.empty() || s.size() > 16) return false;
    for (unsigned char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    return true;
}

// Is `text` a valid value for a kind (UTF-8, no control character, its length, the template
// grammar)? `why` says what is wrong.
inline bool utTextValueOk(UtTextKind kind, const std::string& text, std::string* why) {
    const std::size_t units = utf16Units(text);
    if (units == kNpos) { if (why) *why = "not UTF-8"; return false; }
    for (unsigned char c : text)
        if (c < 0x20 || c == 0x7F) { if (why) *why = "a control character"; return false; }
    std::size_t lo = 0, hi = 0;
    utTextLimits(kind, &lo, &hi);
    if (units < lo || units > hi) {
        if (why) *why = units < lo ? std::string("empty")
                                   : "longer than " + std::to_string(hi) + " UTF-16 units";
        return false;
    }
    if (kind == kTextFormat && !utTextTemplateCheck(text, why)) return false;
    return true;
}

// One key = value of a [text]-kind section (`cur`) or of [style] (`style`) into `f`; false with
// `why` (without the line) on any refusal. utTextParse and the buttons file's [tab] share it.
inline bool utTextApplyKey(UtTextFile& f, bool style, UtTextSection* cur,
                           std::map<std::string, int>& styleSeen, const std::string& key,
                           const std::string& val, int lineNo, std::string* whyOut) {
    auto fail = [&](int, const std::string& why) {
        if (whyOut) *whyOut = why;
        return false;
    };
    if (!style) {
        UtTextKind kind;
        std::string arg;
        if (!utTextParseKey(key, &kind, &arg)) return fail(lineNo, "unknown key \"" + key + "\"");
        const auto seen = cur->find(key);
        if (seen != cur->end())
            return fail(lineNo, "\"" + key + "\" is set twice in this section (first at line " +
                                    std::to_string(seen->second.line) + ")");
        std::string why;
        if (val.empty() && kind != kTextModeUnknown)
            return fail(lineNo, "\"" + key + "\" is empty (comment the line out to use the default)");
        if (arg == "*" && val != kTextGameValue)
            return fail(lineNo, "\"name.*\" takes only @game (the game's own word for every group "
                                "that has one)");
        if (val == kTextGameValue && kind != kTextName)
            return fail(lineNo, "\"" + key + "\": @game is for name.<id> and name.* only (the game has "
                                "no word for a caption, a template or a rollover line)");
        if (!utTextValueOk(kind, val, &why)) return fail(lineNo, "\"" + key + "\": " + why);
        UtTextValue v;
        v.text = val;
        v.line = lineNo;
        (*cur)[key] = v;
        return true;
    }
    const auto seen = styleSeen.find(key);
    if (seen != styleSeen.end())
        return fail(lineNo, "\"" + key + "\" is set twice in [style] (first at line " +
                                std::to_string(seen->second) + ")");
    styleSeen[key] = lineNo;
    if (val.empty()) return fail(lineNo, "\"" + key + "\" is empty (comment the line out to use the default)");
    bool known = false;
    if (key == "language") {
        known = true;
        f.style.language = utTextLower(val);
        if (!utTextValidLang(f.style.language))
            return fail(lineNo, "\"language\" must name a [text.<language>] section: 1..16 of "
                                "[a-z0-9_-], for example ru");
    }
    for (int i = 0; i < kStyleFontCount && !known; ++i) {
        if (key != utStyleFontKey(i)) continue;
        known = true;
        if (!utStyleFontOk(val, &f.style.font[i]))
            return fail(lineNo, "\"" + key + "\" must be fonts/<name>.fnt ([a-z0-9 _./-], no \"..\")");
    }
    for (int i = 0; i < kStyleSizeCount && !known; ++i) {
        if (key != utStyleSizeKey(i)) continue;
        known = true;
        int lo = 0, hi = 0;
        utStyleSizeRange(i, &lo, &hi);
        char* end = nullptr;
        errno = 0;
        const long n = std::strtol(val.c_str(), &end, 10);
        const bool num = end && *end == 0 && errno == 0 && val.find_first_not_of("0123456789") == std::string::npos;
        const bool autoOk = i == kStyleSizeCaption && num && n == 0;
        if (!num || (!autoOk && (n < lo || n > hi)))
            return fail(lineNo, "\"" + key + "\" must be a whole number " + std::to_string(lo) + ".." +
                                    std::to_string(hi) + (i == kStyleSizeCaption ? " or 0 (auto)" : ""));
        f.style.size[i] = static_cast<int>(n);
    }
    for (int i = 0; i < kStyleColorCount && !known; ++i) {
        if (key != utStyleColorKey(i)) continue;
        known = true;
        if (!utStyleColorParse(val, &f.style.color[i]))
            return fail(lineNo, "\"" + key + "\" must be six hex digits RRGGBB");
    }
    if (!known) return fail(lineNo, "unknown key \"" + key + "\" in [style]");
    return true;
}

// Parses the whole file. Any error refuses it: `out` is left empty and `error` reads
// "line N: <reason>".
inline bool utTextParse(const std::string& bytes, UtTextFile& out, std::string* error) {
    out = UtTextFile();
    UtTextFile f;
    auto fail = [&](int line, const std::string& why) {
        if (error) *error = "line " + std::to_string(line) + ": " + why;
        return false;
    };
    enum { kNone, kText, kStyle } sec = kNone;
    UtTextSection* cur = nullptr;
    std::map<std::string, int> styleSeen;
    std::size_t pos = 0;
    int lineNo = 0;
    if (bytes.compare(0, 3, "\xEF\xBB\xBF") == 0) pos = 3;
    while (pos <= bytes.size()) {
        std::size_t nl = bytes.find('\n', pos);
        if (nl == std::string::npos) nl = bytes.size();
        std::string raw = bytes.substr(pos, nl - pos);
        pos = nl + 1;
        ++lineNo;
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        if (!utf8Valid(raw)) return fail(lineNo, "not UTF-8 (save the file as UTF-8)");
        const std::string line = utTextTrim(raw);
        if (line.empty() || line[0] == ';' || line[0] == '#') {
            if (nl == bytes.size()) break;
            continue;
        }
        for (unsigned char c : line)
            if ((c < 0x20 && c != '\t') || c == 0x7F) return fail(lineNo, "a control character");
        if (line[0] == '[') {
            if (line.back() != ']') return fail(lineNo, "a section header must end with ']'");
            const std::string name = utTextLower(utTextTrim(line.substr(1, line.size() - 2)));
            if (name == "text") { sec = kText; cur = &f.common; }
            else if (name == "style") { sec = kStyle; cur = nullptr; }
            else if (name.compare(0, 5, "text.") == 0 && utTextValidLang(name.substr(5))) {
                sec = kText;
                cur = &f.lang[name.substr(5)];
            } else {
                return fail(lineNo, "unknown section [" + name + "] (use [text], [text.<language>] or [style])");
            }
            if (nl == bytes.size()) break;
            continue;
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) return fail(lineNo, "not a comment, a [section] or key = value");
        const std::string key = utTextLower(utTextTrim(line.substr(0, eq)));
        std::string val = utTextTrim(line.substr(eq + 1));
        if (key.empty()) return fail(lineNo, "a value without a key");
        if (sec == kNone) return fail(lineNo, "\"" + key + "\" is outside a section (put it under [text] or [style])");
        if (!val.empty() && val[0] == '"') {
            if (val.size() < 2 || val.back() != '"') return fail(lineNo, "an unbalanced '\"'");
            val = val.substr(1, val.size() - 2);
        }
        for (unsigned char c : val)
            if (c < 0x20) return fail(lineNo, "a control character in the value");
        ++f.keys;
        std::string why;
        if (!utTextApplyKey(f, sec == kStyle, cur, styleSeen, key, val, lineNo, &why)) return fail(lineNo, why);
        if (nl == bytes.size()) break;
    }
    out = f;
    return true;
}

// ================================================================= the chain
// Does the font that draws `font` have a glyph for cp? (The engine side: GetMetric non-null and
// not the placeholder; nullptr = every glyph is there.)
typedef bool (*UtGlyphFn)(UtTextFont font, char32_t cp, const void* ctx);
// The game's own text for an id in the current language (UTF-8), or false. Asked only for a
// name whose value in the file is @game.
typedef bool (*UtTagFn)(UtTextKind kind, const std::string& id, std::string* out, const void* ctx);

struct UtTextContext {
    const UtTextFile* file = nullptr;   // nullptr = no file (or a refused one)
    // The game's language, for the log only: the chain never reads these two (no automatic
    // switching - the section in force is the one the file's [style] language names).
    std::string language;
    bool english = true;
    UtGlyphFn glyph = nullptr;
    const void* glyphCtx = nullptr;
    UtTagFn tag = nullptr;
    const void* tagCtx = nullptr;
};

enum UtTextSource {
    kSrcLanguage = 0,   // [text.<lang>]
    kSrcCommon,         // [text]
    kSrcCategory,       // the category's own label/tag
    kSrcGameTag,        // the game's tag
    kSrcBuiltin,        // the built-in English
    kSrcDerived,        // made from the id
    kSrcNone
};
inline const char* utTextSourceName(UtTextSource s) {
    static const char* const k[] = {"[text.<lang>]", "[text]", "the category", "the game's tag",
                                    "the built-in text", "the id", "nothing"};
    return k[s];
}

// Every code point of s has a glyph in `font`; else false with the first missing one.
inline bool utTextCovered(const UtTextContext& c, UtTextFont font, const std::string& s,
                          char32_t* missing) {
    if (!c.glyph) return true;
    std::u32string cps;
    if (!utf8Decode(s, cps)) return false;
    for (char32_t cp : cps) {
        if (!c.glyph(font, cp, c.glyphCtx)) {
            if (missing) *missing = cp;
            return false;
        }
    }
    return true;
}

inline std::string utTextCodePointName(char32_t cp) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "U+%04X", static_cast<unsigned>(cp));
    return buf;
}

struct UtTextPick {
    std::string text;
    UtTextSource source = kSrcNone;
    std::string skipped;   // the first candidate passed over and why ("" = none); for one WARN
};

// The [text.<lang>] section in force: the one the file's [style] language names, whatever
// language the game runs in; none without that key.
inline const UtTextSection* utTextLangSectionOf(const UtTextFile* f) {
    if (!f || f->style.language.empty()) return nullptr;
    const auto it = f->lang.find(f->style.language);
    return it == f->lang.end() ? nullptr : &it->second;
}
inline const UtTextSection* utTextLangSection(const UtTextContext& c) { return utTextLangSectionOf(c.file); }

// What a file's language sections come to, for one log line per file load: "" when every section
// it has is the one in force (or it has none). `warn` = [style] language names a section the file
// does not have (the player asked for it); else the line is for DEBUG.
inline std::string utTextSectionNote(const UtTextFile& f, bool* warn) {
    if (warn) *warn = false;
    std::string others;
    std::size_t n = 0;
    for (const auto& l : f.lang) {
        if (l.first == f.style.language) continue;
        others += (others.empty() ? "" : ", ") + std::string("[text.") + l.first + "]";
        ++n;
    }
    std::string out;
    if (!f.style.language.empty() && !f.lang.count(f.style.language)) {
        if (warn) *warn = true;
        out = "[style] language = " + f.style.language + " names no [text." + f.style.language +
              "] section in the file - only [text] applies";
        if (n) out += "; " + others + (n > 1 ? " are" : " is") + " not applied";
        return out;
    }
    if (!n) return out;
    out = others + (n > 1 ? " are" : " is") + " not applied - ";
    if (f.style.language.empty())
        out += "[style] language is unset (the tab is English by default in every language of the "
               "game; language = <x> under [style] applies [text.<x>])";
    else
        out += "[style] language = " + f.style.language + " applies [text." + f.style.language + "] only";
    return out;
}

// The key text of a kind ("caption.head", "tooltip.yes", ...).
inline std::string utTextKeyName(UtTextKind kind, const std::string& id) {
    switch (kind) {
    case kTextCaption: return "caption." + id;
    case kTextName: return "name." + id;
    case kTextFormat: return "label.format." + id;
    case kTextModeUnknown: return "label.mode_unknown";
    case kTextUnknown: return "label.unknown";
    case kTextTooltipYes: return "tooltip.yes";
    case kTextTooltipNo: return "tooltip.no";
    case kTextTooltipRoll: return "tooltip.roll";
    default: return std::string();
    }
}

// Resolves one key (not label.format.N - see utTextResolveFormats). `id` is the category id for
// caption/name and ignored otherwise; `category` is the category's own text (the G line's tag
// for a caption, its label for a name), or nullptr.
inline UtTextPick utTextResolve(const UtTextContext& c, UtTextKind kind, const std::string& id,
                                const char* category = nullptr) {
    UtTextPick pick;
    const std::string key = utTextKeyName(kind, id);
    const UtTextFont font = utTextFontOf(kind);
    const UtTextBuiltin* b = (kind == kTextCaption || kind == kTextName) ? utTextBuiltinById(id) : nullptr;
    std::string builtin;
    if (b) builtin = kind == kTextCaption ? b->caption : b->name;
    else if (const char* p = utTextBuiltinPlain(kind)) builtin = p;
    auto take = [&](const std::string& text, UtTextSource src) {
        std::string why;
        char32_t missing = 0;
        if (!utTextValueOk(kind, text, &why)) {
            if (pick.skipped.empty()) pick.skipped = key + " from " + utTextSourceName(src) + ": " + why;
            return false;
        }
        if (!utTextCovered(c, font, text, &missing)) {
            if (pick.skipped.empty())
                pick.skipped = key + " from " + utTextSourceName(src) + ": " + utTextCodePointName(missing) +
                               " is not in the " + (font == kFontCaption ? "caption" : font == kFontLabel ? "label" : "tooltip") + " font";
            return false;
        }
        pick.text = text;
        pick.source = src;
        return true;
    };
    // The file: the section [style] language names, then [text]; in each the exact key, then (a
    // name) name.*. @game is the game's own word for the group - asked of the tag callback only
    // here, never otherwise - and, without one, simply the next source (not a skip: most groups
    // have no exact tag).
    auto fromFile = [&](const UtTextSection* s, UtTextSource src) {
        if (!s) return false;
        for (int w = 0; w < (kind == kTextName ? 2 : 1); ++w) {
            const auto it = s->find(w == 0 ? key : std::string("name.*"));
            if (it == s->end()) continue;
            if (kind == kTextName && it->second.text == kTextGameValue) {
                std::string t;
                if (c.tag && c.tag(kind, id, &t, c.tagCtx) && !t.empty() && take(t, kSrcGameTag)) return true;
                continue;
            }
            if (take(it->second.text, src)) return true;
        }
        return false;
    };
    if (fromFile(utTextLangSection(c), kSrcLanguage)) return pick;
    if (c.file && fromFile(&c.file->common, kSrcCommon)) return pick;
    if (category && *category && builtin != category && take(category, kSrcCategory)) return pick;
    if (!builtin.empty() && take(builtin, kSrcBuiltin)) return pick;
    if (builtin.empty() && (kind == kTextCaption || kind == kTextName)) {
        const std::string d = kind == kTextCaption ? utTextDerivedCaption(id) : utTextDerivedName(id);
        if (take(d, kSrcDerived)) return pick;
    }
    if (kind == kTextModeUnknown) {   // may be empty, and an empty string has no glyph to miss
        pick.text = builtin;
        pick.source = kSrcBuiltin;
        return pick;
    }
    // Nothing passed the glyph check: the built-in text all the same (the font's own fallback).
    pick.text = builtin.empty() && (kind == kTextCaption || kind == kTextName)
                    ? (kind == kTextCaption ? utTextDerivedCaption(id) : utTextDerivedName(id))
                    : builtin;
    pick.source = builtin.empty() ? kSrcDerived : kSrcBuiltin;
    return pick;
}

// The label template list: taken whole from the [text.<lang>] in force when it sets any
// label.format.N, else from [text], else 1.1.0's two. A list whose literal text misses a glyph of the label font is
// passed over whole.
inline UtTextSource utTextResolveFormats(const UtTextContext& c, std::vector<std::string>& out,
                                         std::string* skipped) {
    auto fromSection = [&](const UtTextSection* s, UtTextSource src) {
        out.clear();
        if (!s) return false;
        for (int i = 1; i <= kFormatMax; ++i) {
            const auto it = s->find("label.format." + std::to_string(i));
            if (it != s->end()) out.push_back(it->second.text);
        }
        if (out.empty()) return false;
        for (std::size_t i = 0; i < out.size(); ++i) {
            std::string lit, why;
            char32_t missing = 0;
            if (!utTextTemplateWalk(out[i], nullptr, nullptr, &lit, nullptr, &why)
                || !utTextCovered(c, kFontLabel, lit, &missing)) {
                if (skipped && skipped->empty())
                    *skipped = std::string("label.format from ") + utTextSourceName(src) + ": " +
                               (why.empty() ? utTextCodePointName(missing) + " is not in the label font" : why);
                out.clear();
                return false;
            }
        }
        return true;
    };
    if (fromSection(utTextLangSection(c), kSrcLanguage)) return kSrcLanguage;
    if (c.file && fromSection(&c.file->common, kSrcCommon)) return kSrcCommon;
    out.clear();
    for (int i = 0; i < kTextDefaultFormats; ++i) out.push_back(utTextDefaultFormat(i));
    return kSrcBuiltin;
}

// ================================================================= what to do about a missing letter
// The language folders of resources\Fonts.arc with a baked (FNTX) savapromedium.fnt. ja ko th vi
// zh hold FNTD fonts instead - TrueType the engine rasterises at run time; they draw and measure
// too, but are listed apart (utb::utbDynamicFolderList, ut_buttons.h: zh ja ko, th and vi carry
// zh's font), read through their cmaps (ut_sfnt.h) and probed lazily (ut::utTmProbePlan).
inline const char* utTextFontFolders() { return "de es tr cs el fr hu it nl pt pl ru sk bg"; }

// The folder whose savapromedium has a code point's script: Cyrillic -> ru (bg lacks four Russian
// letters), Greek -> el; nullptr when the script alone does not decide it.
inline const char* utTextFontFolderFor(char32_t cp) {
    if (cp >= 0x0400 && cp <= 0x04FF) return "ru";
    if (cp >= 0x0370 && cp <= 0x03FF) return "el";
    return nullptr;
}

// What the "is not in the ... font" WARN tells the user to do, from the candidates passed over
// (as utTextResolve and utTextResolveFormats name them). "" when no letter was missing.
// Superseded for the buttons file by utb::utbFontAdvice (ut_buttons.h), which wraps this text: it
// names [tab] / a button's font = instead of the retired [style], the FNTD copy (zh / ja / ko) for
// a letter no baked copy has, and says when no font line can draw a letter (above U+FFFF). Only
// utbFontAdvice and the tests call this one.
inline std::string utTextFontAdvice(const std::vector<std::string>& skipped) {
    static const char* const kFont[3] = {"caption", "label", "tooltip"};
    std::string set, tooltip;
    for (int f = 0; f < 3; ++f) {
        const std::string tail = std::string(" is not in the ") + kFont[f] + " font";
        for (const std::string& s : skipped) {
            const std::size_t at = s.find(tail);
            const std::size_t u = at == std::string::npos ? at : s.rfind("U+", at);
            if (u == std::string::npos) continue;
            if (f == 2) {
                tooltip = "the rollover lines are drawn by the game in its own font, so their letters "
                          "need a game running in that language";
                break;
            }
            const char32_t cp = (char32_t)std::strtoul(s.c_str() + u + 2, nullptr, 16);
            const char* folder = utTextFontFolderFor(cp);
            if (!set.empty()) set += " and ";
            set += std::string("font.") + kFont[f] + " = fonts/" + (folder ? folder : "<lang>") +
                   "/savapromedium.fnt";
            if (!folder) set += std::string(" (<lang> one of ") + utTextFontFolders() + ")";
            break;
        }
    }
    std::string out;
    if (!set.empty()) out = "to draw it, add " + set + " under [style] of uniquetab-text.ini";
    if (!tooltip.empty()) out += (out.empty() ? "" : "; ") + tooltip;
    return out;
}

// ================================================================= the game's own words
// The built-in group id -> the game's tag that names exactly that group (a plural crafting tag
// covering the same items). Left out because no tag is exact: offhand (FOCI - casters only),
// axe/mace/sword 1h and 2h (AXES, BLUNT and SWORDS cover both hands), dagger and scepter
// (IMPLEMENTS covers both), amulet, medal and ring (no plural tag), relic (only the tier tags).
inline const char* utTextGameTagFor(const std::string& id) {
    static const char* const kMap[][2] = {
        {"head", "tagCraftingHelms"},       {"shoulders", "tagCraftingShoulders"},
        {"chest", "tagCraftingTorso"},      {"hands", "tagCraftingHands"},
        {"waist", "tagCraftingBelts"},      {"legs", "tagCraftingLegs"},
        {"feet", "tagCraftingFeet"},        {"shield", "tagCraftingShields"},
        {"spear2h", "tagCraftingSpears"},   {"ranged1h", "tagCraftingOneHanded"},
        {"ranged2h", "tagCraftingTwoHanded"},
    };
    for (const auto& m : kMap)
        if (id == m[0]) return m[1];
    return nullptr;
}

// A localized tag without its grammatical-gender markers: a leading "[xx]" goes, and of
// "[ms]A [fs]B" only the first alternative is kept; blanks around the result are trimmed.
inline std::string utTextStripGenderMarkers(const std::string& s) {
    auto marker = [&](std::size_t i) {
        return i + 3 < s.size() && s[i] == '[' && s[i + 1] >= 'a' && s[i + 1] <= 'z' &&
               s[i + 2] >= 'a' && s[i + 2] <= 'z' && s[i + 3] == ']';
    };
    std::size_t b = 0;
    while (b < s.size() && s[b] == ' ') ++b;
    if (marker(b)) b += 4;
    std::size_t e = b;
    while (e < s.size() && !marker(e)) ++e;
    std::string r = s.substr(b, e - b);
    while (!r.empty() && r.back() == ' ') r.pop_back();
    while (!r.empty() && r[0] == ' ') r.erase(0, 1);
    return r;
}

// The game's word for a GROUP NAME (never a caption: a pad caption is an abbreviation, and the
// game has none): the exact tag of the id, localized by `localize`, markers stripped.
typedef bool (*UtLocalizeFn)(const char* tag, std::string* out, const void* ctx);
inline bool utTextGameWord(UtTextKind kind, const std::string& id, UtLocalizeFn localize,
                           const void* ctx, std::string* out) {
    if (kind != kTextName || !localize || !out) return false;
    const char* tag = utTextGameTagFor(id);
    std::string raw;
    if (!tag || !localize(tag, &raw, ctx)) return false;
    *out = utTextStripGenderMarkers(raw);
    return !out->empty();
}

// ================================================================= the pad and the label, resolved
// One group of the table in force: its id, its G line's tag and its label (colour codes removed).
struct UtTextGroupIn {
    std::string id, tag, label;
};
// The pad's captions in logical order (vanilla, the groups, owned, next) and the groups' names,
// each through the chain; `skipped` holds every candidate passed over (for the one WARN).
struct UtTextPad {
    std::vector<std::string> caps;
    std::vector<UtTextSource> capSrc;
    std::vector<std::string> names;
    std::vector<UtTextSource> nameSrc;
    std::vector<std::string> skipped;
};
inline UtTextPad utTextResolvePad(const UtTextContext& c, const std::vector<UtTextGroupIn>& groups) {
    UtTextPad p;
    auto cap = [&](const std::string& id, const std::string& tag) {
        const UtTextPick k = utTextResolve(c, kTextCaption, id, tag.empty() ? nullptr : tag.c_str());
        p.caps.push_back(k.text);
        p.capSrc.push_back(k.source);
        if (!k.skipped.empty()) p.skipped.push_back(k.skipped);
    };
    cap("vanilla", "MAT");
    for (const UtTextGroupIn& g : groups) {
        cap(g.id, g.tag);
        const UtTextPick n =
            utTextResolve(c, kTextName, g.id, g.label.empty() ? nullptr : g.label.c_str());
        p.names.push_back(n.text);
        p.nameSrc.push_back(n.source);
        if (!n.skipped.empty()) p.skipped.push_back(n.skipped);
    }
    cap("owned", "OWN");
    cap("next", ">>");
    std::vector<bool> derived;
    for (UtTextSource s : p.capSrc) derived.push_back(s == kSrcDerived);
    utTextDistinctDerived(p.caps, derived);
    return p;
}

// The label's last part while the journal owns records no page of this view shows (the
// catalogue lacks them, or has them and no group takes them): " - +N not on these pages"; empty
// for N <= 0 (none, or not counted yet).
inline std::string utNotShownSuffix(long n) {
    return n > 0 ? " - +" + std::to_string(n) + " not on these pages" : std::string();
}

// The label line: every template expanded and `suffix` appended (utNotShownSuffix's tail stays
// last), the first that fits `limitPx`, else the first cut by width; trailing blanks
// and '-' are trimmed either way, as 1.1.0 trims. `picked` = the template drawn, `fits` = whether
// it fitted whole.
inline std::string utLabelCompose(const std::vector<std::string>& formats, const UtLabelValues& v,
                                  const std::string& suffix, int limitPx, UtTextWidthFn widthFn,
                                  const void* ctx, int* picked = nullptr, bool* fits = nullptr) {
    std::vector<std::string> lines;
    for (const std::string& f : formats) {
        std::string out;
        if (utTextExpand(f, v, out)) lines.push_back(out + suffix);
    }
    for (int i = 0; lines.empty() && i < kTextDefaultFormats; ++i) {
        std::string out;
        if (utTextExpand(utTextDefaultFormat(i), v, out)) lines.push_back(out + suffix);
    }
    bool whole = false;
    const int i = utLabelPick(lines, limitPx, widthFn, ctx, &whole);
    if (picked) *picked = i;
    if (fits) *fits = whole;
    if (i < 0) return std::string();
    std::string line =
        whole ? lines[std::size_t(i)] : utTextClipToWidth(lines[std::size_t(i)], limitPx, widthFn, ctx);
    while (!line.empty() && (line.back() == ' ' || line.back() == '-')) line.pop_back();
    return line;
}

// ================================================================= reload by mtime
// What the caller saw of the file (the engine side fills it from the file system).
struct UtFileStamp {
    bool known = false;    // false before the first look
    bool exists = false;
    std::uint64_t mtime = 0;
    std::uint64_t size = 0;
};
// Must the file be (re)read? The first look always says yes; then any change of existence,
// mtime or size. A file that is still missing is not a change.
inline bool utTextChanged(const UtFileStamp& was, const UtFileStamp& now) {
    if (!was.known) return true;
    if (was.exists != now.exists) return true;
    return now.exists && (was.mtime != now.mtime || was.size != now.size);
}

} // namespace utt
