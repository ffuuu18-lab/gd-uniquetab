// ut_buttons.h - the buttons file: uniquetab-buttons.ini, one per game (the main campaign's
// beside uniquetab.ini, a custom game's in mods\<name>\). Its parser, the resolver that
// maps buttons -> groups -> pages against the view's group table, the default file the mod writes
// populated at a game's first launch (tools\build_uniq_db.py writes the same bytes), the
// [unassigned] rewrite-in-place, the one-time fold of an old uniquetab-text.ini, and the font a
// role derives from its letters.
//
// Pure C++17: no Windows, no engine, no globals; only ut_textcfg.h (the UTF-8 helper, the label
// templates, the chain's kinds and limits, the [style] keys). tools\test_buttons.cpp links it.
//
// THE FILE (UTF-8, an optional BOM, CRLF or LF):
//   ; comment        a line whose first non-blank character is ';' or '#'. A ';' after a blank
//                    or a tab also starts a comment ("groups = head   ; helms"); a ';' with no
//                    blank before it is text ("a;b"). A value in double quotes keeps everything
//                    inside them (blanks, ';'), and only a comment may follow the closing quote.
//   [tab]            the tab's words: label.format.1..4, label.mode_unknown, label.unknown,
//                    tooltip.yes, tooltip.no, tooltip.roll, mat.caption, own.caption, next.caption,
//                    edit.caption (tooltip.roll and edit.caption are read, never written: the
//                    generated file does not list them) and the
//                    look: font.caption, font.label, size.label, size.caption_max, size.caption,
//                    color.caption, color.label, color.ring, color.mark (as the old text file's
//                    [text] / [style] keys, one to one - see utbTabKeyToText).
//   [TAG]            one button. The section name IS the tag: 1..3 printable ASCII characters
//                    (not blank [ ] ; = , " #), unique ignoring case; MAT, OWN and TAB are taken.
//                    Keys: name (the page name in the label line, 1..48 UTF-16 units), caption
//                    (the button's text when it is not the tag, 1..12 units), groups (the item
//                    groups on it: ids from the groups file, comma-separated; several ids = one
//                    button whose ONE page holds all of their items, in group order - ut_live's
//                    union, ut_padpage.h utUnionPlan), size (this button's
//                    largest caption size 6..36, over pad_caption_max / size.caption_max), font
//                    (this button's font, fonts/<name>.fnt, over the derived one).
//   [unassigned]     groups = <ids>: every group of this game no button holds. Read for nothing:
//                    the mod REWRITES this section (and only it) whenever the set differs. Its
//                    comment line "; added once by the mod ...: <ids>" names the built-in groups
//                    that came after 1.1.0's 24 and have been given a button ONCE (utbOfferNew):
//                    a file written before such a group existed gets its [TAG] appended one time,
//                    and a user who deletes that button keeps it deleted.
// Button order = file order. ANY error refuses the WHOLE file and names the line (the caller
// then uses the built-in buttons - one per group of the view - and logs it once per load):
// not UTF-8, a control character, a line that is neither a comment, a section nor key = value,
// a key outside a section, an unknown key, a key set twice in one section, a bad tag, a tag
// twice, a button without groups, an empty or bad value; and, against the view's groups
// (utbResolve), an id that names no group the game can have, an id on two buttons.
#pragma once

#include "ut_textcfg.h"

#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace utb {

using utt::UtTextFile;

// ================================================================= the parsed file
struct UtbButton {
    std::string tag;                    // the section name as written
    int line = 0;                       // the section header's line
    std::string name, caption, font;    // "" = unset
    int nameLine = 0, captionLine = 0, fontLine = 0;
    int size = -1, sizeLine = 0;        // -1 = unset
    std::vector<std::string> groups;    // lower-cased ids, as listed
    int groupsLine = 0;
};

struct UtbFile {
    std::vector<UtbButton> buttons;     // file order
    UtTextFile words;                   // [tab], as the old text file's [text] + [style]
    std::vector<std::string> unassigned;   // as listed (read for nothing)
    int unassignedLine = 0;             // the [unassigned] header's line, 0 = none
    int keys = 0;                       // key lines read
};

// A [tab] key -> the old text file's key (and whether it was a [style] key); false = unknown.
inline bool utbTabKeyToText(const std::string& key, std::string* textKey, bool* style) {
    *style = false;
    if (key == "mat.caption") { *textKey = "caption.vanilla"; return true; }
    if (key == "own.caption") { *textKey = "caption.owned"; return true; }
    if (key == "next.caption") { *textKey = "caption.next"; return true; }
    if (key == "edit.caption") { *textKey = "caption.edit"; return true; }
    if (key == "label.mode_unknown" || key == "label.unknown" || key == "tooltip.yes" ||
        key == "tooltip.no" || key == "tooltip.roll") {
        *textKey = key;
        return true;
    }
    if (key.size() == 14 && key.compare(0, 13, "label.format.") == 0 && key[13] >= '1' &&
        key[13] < '1' + utt::kFormatMax) {
        *textKey = key;
        return true;
    }
    for (int i = 0; i < utt::kStyleFontCount; ++i)
        if (key == utt::utStyleFontKey(i)) { *textKey = key; *style = true; return true; }
    for (int i = 0; i < utt::kStyleSizeCount; ++i)
        if (key == utt::utStyleSizeKey(i)) { *textKey = key; *style = true; return true; }
    for (int i = 0; i < utt::kStyleColorCount; ++i)
        if (key == utt::utStyleColorKey(i)) { *textKey = key; *style = true; return true; }
    return false;
}

// The other way: an old text-file key -> the [tab] key ("" = not a [tab] key).
inline std::string utbTextKeyToTab(const std::string& textKey) {
    if (textKey == "caption.vanilla") return "mat.caption";
    if (textKey == "caption.owned") return "own.caption";
    if (textKey == "caption.next") return "next.caption";
    if (textKey == "caption.edit") return "edit.caption";
    std::string k;
    bool st = false;
    if (utbTabKeyToText(textKey, &k, &st) && k == textKey) return textKey;
    return std::string();
}

inline std::string utbUpper(std::string s) {
    for (char& c : s) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return s;
}

// A button tag: 1..3 printable ASCII, not blank [ ] ; = , " #.
inline bool utbValidTag(const std::string& t) {
    if (t.empty() || t.size() > 3) return false;
    for (unsigned char c : t)
        if (c <= 0x20 || c >= 0x7F || c == '[' || c == ']' || c == ';' || c == '=' || c == ',' ||
            c == '"' || c == '#')
            return false;
    return true;
}
inline bool utbReservedTag(const std::string& t) {
    const std::string u = utbUpper(t);
    return u == "MAT" || u == "OWN" || u == "TAB";
}

// A value with its comment cut: a ';' after a blank or a tab starts it (outside double quotes);
// a quoted value keeps what is inside. False with `why` on an unbalanced quote or text after one.
inline bool utbValue(const std::string& raw, std::string* out, std::string* why) {
    const std::size_t first = raw.find_first_not_of(" \t");
    if (first != std::string::npos && first > 0 && raw[first] == ';') {   // "key = ; a comment"
        out->clear();
        return true;
    }
    std::string v = utt::utTextTrim(raw);
    if (!v.empty() && v[0] == '"') {
        const std::size_t close = v.find('"', 1);
        if (close == std::string::npos) { *why = "an unbalanced '\"'"; return false; }
        const std::string rest = utt::utTextTrim(v.substr(close + 1));
        if (!rest.empty() && rest[0] != ';') {
            *why = "text after the closing '\"' (a comment needs a ';')";
            return false;
        }
        *out = v.substr(1, close - 1);
        return true;
    }
    for (std::size_t i = 1; i < v.size(); ++i) {
        if (v[i] == ';' && (v[i - 1] == ' ' || v[i - 1] == '\t')) {
            v = utt::utTextTrim(v.substr(0, i));
            break;
        }
    }
    *out = v;
    return true;
}

// The ids of a groups line: comma-separated, blanks around each dropped, lower-cased. An empty
// list is allowed only when `emptyOk` ([unassigned]).
inline bool utbIdList(const std::string& v, bool emptyOk, std::vector<std::string>* ids,
                      std::string* why) {
    ids->clear();
    if (utt::utTextTrim(v).empty()) {
        if (emptyOk) return true;
        *why = "\"groups\" is empty (a button needs at least one group id)";
        return false;
    }
    std::size_t b = 0;
    while (b <= v.size()) {
        std::size_t e = v.find(',', b);
        if (e == std::string::npos) e = v.size();
        const std::string id = utt::utTextLower(utt::utTextTrim(v.substr(b, e - b)));
        if (!utt::utTextValidId(id)) {
            *why = id.empty() ? std::string("an empty id between two commas")
                              : "\"" + id + "\" is not a group id ([a-z0-9_.-], up to 40)";
            return false;
        }
        for (const std::string& x : *ids)
            if (x == id) { *why = "\"" + id + "\" is listed twice"; return false; }
        ids->push_back(id);
        b = e + 1;
    }
    return true;
}

// Parses the whole file (the grammar; the ids are checked against a view by utbResolve). Any
// error refuses it: `out` is left empty and `error` reads "line N: <reason>".
inline bool utbParse(const std::string& bytes, UtbFile& out, std::string* error) {
    out = UtbFile();
    UtbFile f;
    auto fail = [&](int line, const std::string& why) {
        if (error) *error = "line " + std::to_string(line) + ": " + why;
        return false;
    };
    enum { kNone, kTab, kButton, kUnassigned } sec = kNone;
    std::map<std::string, int> styleSeen, keySeen;
    int tabLine = 0;
    std::size_t pos = 0;
    int lineNo = 0;
    if (bytes.compare(0, 3, "\xEF\xBB\xBF") == 0) pos = 3;
    while (pos <= bytes.size()) {
        std::size_t nl = bytes.find('\n', pos);
        if (nl == std::string::npos) nl = bytes.size();
        std::string raw = bytes.substr(pos, nl - pos);
        pos = nl + 1;
        ++lineNo;
        const bool last = nl == bytes.size();
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        if (!utt::utf8Valid(raw)) return fail(lineNo, "not UTF-8 (save the file as UTF-8)");
        const std::string line = utt::utTextTrim(raw);
        if (line.empty() || line[0] == ';' || line[0] == '#') {
            if (last) break;
            continue;
        }
        for (unsigned char c : line)
            if ((c < 0x20 && c != '\t') || c == 0x7F) return fail(lineNo, "a control character");
        if (line[0] == '[') {
            const std::string& head = line;
            const std::size_t close = head.find(']');
            if (close == std::string::npos) return fail(lineNo, "a section header must end with ']'");
            const std::string rest = utt::utTextTrim(head.substr(close + 1));
            if (!rest.empty() && rest[0] != ';')
                return fail(lineNo, "text after the section header (a comment needs a ';')");
            const std::string name = utt::utTextTrim(head.substr(1, close - 1));
            const std::string low = utt::utTextLower(name);
            keySeen.clear();
            if (low == "tab") {
                if (tabLine)
                    return fail(lineNo, "[tab] appears twice (first at line " + std::to_string(tabLine) + ")");
                tabLine = lineNo;
                sec = kTab;
            } else if (low == "unassigned") {
                if (f.unassignedLine)
                    return fail(lineNo, "[unassigned] appears twice (first at line " +
                                            std::to_string(f.unassignedLine) + ")");
                f.unassignedLine = lineNo;
                sec = kUnassigned;
            } else {
                if (!utbValidTag(name))
                    return fail(lineNo, "[" + name + "] is not a button tag (1 to 3 characters, "
                                        "letters, digits or signs; or [tab], [unassigned])");
                if (utbReservedTag(name))
                    return fail(lineNo, "[" + name + "] is taken (MAT, OWN and TAB are the tab's "
                                        "own; their words are mat.caption / own.caption under [tab])");
                for (const UtbButton& b : f.buttons)
                    if (utbUpper(b.tag) == utbUpper(name))
                        return fail(lineNo, "the button [" + name + "] appears twice (first at line " +
                                                std::to_string(b.line) + ")");
                UtbButton b;
                b.tag = name;
                b.line = lineNo;
                f.buttons.push_back(b);
                sec = kButton;
            }
            if (last) break;
            continue;
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) return fail(lineNo, "not a comment, a [section] or key = value");
        const std::string key = utt::utTextLower(utt::utTextTrim(line.substr(0, eq)));
        std::string val, why;
        if (key.empty()) return fail(lineNo, "a value without a key");
        if (sec == kNone)
            return fail(lineNo, "\"" + key + "\" is outside a section (put it under [tab] or a [TAG])");
        if (!utbValue(line.substr(eq + 1), &val, &why)) return fail(lineNo, why);
        for (unsigned char c : val)
            if (c < 0x20) return fail(lineNo, "a control character in the value");
        ++f.keys;
        if (sec == kTab) {
            std::string tk;
            bool style = false;
            if (!utbTabKeyToText(key, &tk, &style))
                return fail(lineNo, "unknown key \"" + key + "\" in [tab]");
            const auto seen = keySeen.find(key);
            if (seen != keySeen.end())
                return fail(lineNo, "\"" + key + "\" is set twice in [tab] (first at line " +
                                        std::to_string(seen->second) + ")");
            keySeen[key] = lineNo;
            if (!utt::utTextApplyKey(f.words, style, &f.words.common, styleSeen, tk, val, lineNo, &why)) {
                // name the [tab] key, not the old one
                const std::string from = "\"" + tk + "\"", to = "\"" + key + "\"";
                for (std::size_t at = 0; key != tk && (at = why.find(from, at)) != std::string::npos; at += to.size())
                    why.replace(at, from.size(), to);
                return fail(lineNo, why);
            }
            ++f.words.keys;
        } else if (sec == kUnassigned) {
            if (key != "groups")
                return fail(lineNo, "unknown key \"" + key + "\" in [unassigned] (only groups =)");
            std::vector<std::string> ids;
            if (!utbIdList(val, true, &ids, &why)) return fail(lineNo, why);
            for (const std::string& id : ids) f.unassigned.push_back(id);
        } else {
            UtbButton& b = f.buttons.back();
            const auto seen = keySeen.find(key);
            if (seen != keySeen.end())
                return fail(lineNo, "\"" + key + "\" is set twice in [" + b.tag + "] (first at line " +
                                        std::to_string(seen->second) + ")");
            keySeen[key] = lineNo;
            if (key == "name" || key == "caption") {
                const utt::UtTextKind kind = key == "name" ? utt::kTextName : utt::kTextCaption;
                if (val.empty())
                    return fail(lineNo, "\"" + key + "\" is empty (remove the line to use the default)");
                if (!utt::utTextValueOk(kind, val, &why)) return fail(lineNo, "\"" + key + "\": " + why);
                (key == "name" ? b.name : b.caption) = val;
                (key == "name" ? b.nameLine : b.captionLine) = lineNo;
            } else if (key == "groups") {
                if (!utbIdList(val, false, &b.groups, &why)) return fail(lineNo, why);
                b.groupsLine = lineNo;
            } else if (key == "size") {
                const bool num = !val.empty() && val.size() <= 3 &&
                                 val.find_first_not_of("0123456789") == std::string::npos;
                const int n = num ? std::atoi(val.c_str()) : -1;
                if (n < 6 || n > 36) return fail(lineNo, "\"size\" must be a whole number 6..36");
                b.size = n;
                b.sizeLine = lineNo;
            } else if (key == "font") {
                if (!utt::utStyleFontOk(val, &b.font))
                    return fail(lineNo, "\"font\" must be fonts/<name>.fnt ([a-z0-9 _./-], no \"..\")");
                b.fontLine = lineNo;
            } else {
                return fail(lineNo, "unknown key \"" + key + "\" in [" + b.tag +
                                        "] (name, caption, groups, size, font)");
            }
        }
        if (last) break;
    }
    for (const UtbButton& b : f.buttons)
        if (!b.groupsLine)
            return fail(b.line, "the button [" + b.tag + "] has no groups = line");
    out = f;
    return true;
}

// ================================================================= against the view's groups
// One group of the view's table, in group order: its id, its G line's tag and its label (colour
// codes removed).
struct UtbGroupIn {
    std::string id, tag, label;
};

struct UtbResolved {
    std::string tag, name, caption, font;   // as the file has them ("" = unset)
    int size = -1;
    std::vector<int> groups;                // indexes into the view's table, ascending
};
struct UtbSet {
    std::vector<UtbResolved> buttons;       // file order; a button none of whose ids is in the
                                            // view is left out (the main file serves two views)
    std::vector<int> buttonOfGroup;         // per group: the button index, -1 = unassigned
    std::vector<std::string> unassigned;    // the ids no button holds, in group order
    int dropped = 0;                        // buttons left out (no group of theirs in this view)
};

// The built-in buttons: one per group, captioned by its tag, named by its label - what no file
// (or the generated default) comes to.
inline UtbSet utbBuiltinSet(const std::vector<UtbGroupIn>& groups) {
    UtbSet s;
    for (std::size_t g = 0; g < groups.size(); ++g) {
        UtbResolved b;
        b.tag = groups[g].tag;
        b.groups.push_back(static_cast<int>(g));
        s.buttons.push_back(b);
        s.buttonOfGroup.push_back(static_cast<int>(g));
    }
    return s;
}

// The file against the view's groups. `alsoKnown` = ids a game served by the same file may have
// (the main file serves the base game and the base game with mods; the built-in group ids): such
// an id is no error and simply holds nothing here. False with `error` ("line N: ...") on an id
// no group can have or an id on two buttons; `out` is then the built-in set.
inline bool utbResolve(const UtbFile& f, const std::vector<UtbGroupIn>& groups,
                       const std::vector<std::string>& alsoKnown, UtbSet& out, std::string* error) {
    out = UtbSet();
    UtbSet s;
    s.buttonOfGroup.assign(groups.size(), -1);
    std::map<std::string, int> index;
    for (std::size_t g = 0; g < groups.size(); ++g) index[groups[g].id] = static_cast<int>(g);
    std::map<std::string, std::pair<std::string, int>> owner;   // id -> (tag, line)
    for (const UtbButton& b : f.buttons) {
        UtbResolved r;
        r.tag = b.tag;
        r.name = b.name;
        r.caption = b.caption;
        r.font = b.font;
        r.size = b.size;
        for (const std::string& id : b.groups) {
            const auto o = owner.find(id);
            if (o != owner.end()) {
                if (error)
                    *error = "line " + std::to_string(b.groupsLine) + ": the group \"" + id +
                             "\" is on [" + o->second.first + "] already (line " +
                             std::to_string(o->second.second) + ") - a group can be on one button only";
                out = utbBuiltinSet(groups);
                return false;
            }
            owner[id] = std::make_pair(b.tag, b.groupsLine);
            const auto it = index.find(id);
            if (it != index.end()) {
                r.groups.push_back(it->second);
                continue;
            }
            bool known = false;
            for (const std::string& k : alsoKnown) known |= k == id;
            if (!known) {
                if (error)
                    *error = "line " + std::to_string(b.groupsLine) + ": [" + b.tag + "] names \"" + id +
                             "\", which is no group of this game (the ids are listed under [unassigned] "
                             "and in the groups file)";
                out = utbBuiltinSet(groups);
                return false;
            }
        }
        if (r.groups.empty()) {
            ++s.dropped;
            continue;
        }
        for (std::size_t i = 1; i < r.groups.size(); ++i)   // group order
            for (std::size_t j = i; j > 0 && r.groups[j - 1] > r.groups[j]; --j)
                std::swap(r.groups[j - 1], r.groups[j]);
        for (int g : r.groups) s.buttonOfGroup[static_cast<std::size_t>(g)] = static_cast<int>(s.buttons.size());
        s.buttons.push_back(r);
    }
    for (std::size_t g = 0; g < groups.size(); ++g)
        if (s.buttonOfGroup[g] < 0) s.unassigned.push_back(groups[g].id);
    out = s;
    return true;
}

// Is the set the built-in one (one button per group, in group order, tag = the G line's tag, no
// caption/size/font, name unset or the label)? The identity gate's question.
inline bool utbIsBuiltin(const UtbSet& s, const std::vector<UtbGroupIn>& groups) {
    if (s.buttons.size() != groups.size()) return false;
    for (std::size_t i = 0; i < groups.size(); ++i) {
        const UtbResolved& b = s.buttons[i];
        if (b.groups.size() != 1 || b.groups[0] != static_cast<int>(i)) return false;
        if (b.tag != groups[i].tag || !b.caption.empty() || !b.font.empty() || b.size >= 0) return false;
        if (!b.name.empty() && b.name != groups[i].label) return false;
    }
    return true;
}

// ================================================================= the pad, resolved
// The pad's captions in LOGICAL order (MAT, the buttons, OWN, NEXT, EDIT), the names per button, and
// per logical button the size cap (-1 = none) and the font path ("" = the role's); each caption
// and name through the glyph check of the font that draws it. A caption falls back to the tag
// (ASCII, so always drawable), a name to its first group's label, then the id's word.
struct UtbPad {
    std::vector<std::string> caps;
    std::vector<int> sizes;
    std::vector<std::string> fonts;
    std::vector<std::string> names;              // per button
    std::vector<std::string> skipped;            // every candidate passed over (one WARN)
    int capFile = 0, nameFile = 0;               // how many came from the file
};
inline UtbPad utbResolvePad(const utt::UtTextContext& c, const UtbSet& s,
                            const std::vector<UtbGroupIn>& groups) {
    UtbPad p;
    auto fixed = [&](const char* id, const char* dflt) {
        const utt::UtTextPick k = utt::utTextResolve(c, utt::kTextCaption, id, dflt);
        p.caps.push_back(k.text);
        p.sizes.push_back(-1);
        p.fonts.push_back(std::string());
        if (!k.skipped.empty()) p.skipped.push_back(k.skipped);
        if (k.source <= utt::kSrcCommon) ++p.capFile;
    };
    auto covered = [&](utt::UtTextFont font, const std::string& text, utt::UtTextKind kind,
                       const std::string& what) {
        std::string why;
        char32_t missing = 0;
        if (!utt::utTextValueOk(kind, text, &why)) {
            p.skipped.push_back(what + ": " + why);
            return false;
        }
        if (!utt::utTextCovered(c, font, text, &missing)) {
            p.skipped.push_back(what + ": " + utt::utTextCodePointName(missing) + " is not in the " +
                                (font == utt::kFontCaption ? "caption" : "label") + " font");
            return false;
        }
        return true;
    };
    fixed("vanilla", "MAT");
    for (const UtbResolved& b : s.buttons) {
        // A button's own font draws its caption: the glyph callback cannot ask it, so a caption
        // with a font line is taken as it is (the live side checks it against that font).
        if (!b.caption.empty() && (!b.font.empty() ||
                                   covered(utt::kFontCaption, b.caption, utt::kTextCaption,
                                           "[" + b.tag + "] caption"))) {
            p.caps.push_back(b.caption);
            ++p.capFile;
        } else {
            p.caps.push_back(b.tag);
        }
        p.sizes.push_back(b.size);
        p.fonts.push_back(b.font);
        const UtbGroupIn* g0 = b.groups.empty() ? nullptr : &groups[static_cast<std::size_t>(b.groups[0])];
        std::string name;
        if (!b.name.empty() && covered(utt::kFontLabel, b.name, utt::kTextName, "[" + b.tag + "] name")) {
            name = b.name;
            ++p.nameFile;
        } else if (g0 && !g0->label.empty() &&
                   covered(utt::kFontLabel, g0->label, utt::kTextName, "the label of " + g0->id)) {
            name = g0->label;
        } else {
            name = g0 ? utt::utTextDerivedName(g0->id) : b.tag;
        }
        p.names.push_back(name);
    }
    fixed("owned", "OWN");
    fixed("next", ">>");
    fixed("edit", "EDIT");
    return p;
}

// ================================================================= the default file
inline std::string utbPad(const std::string& k, std::size_t w) {
    return k.size() >= w ? k : k + std::string(w - k.size(), ' ');
}

// A value as the file writes it: in double quotes when blanks around it or a ';' after a blank
// would otherwise be lost, or when it starts with a quote.
inline std::string utbQuote(const std::string& v) {
    bool q = v.empty() || v[0] == ' ' || v[0] == '\t' || v.back() == ' ' || v.back() == '\t' || v[0] == '"';
    for (std::size_t i = 1; i < v.size() && !q; ++i)
        q = v[i] == ';' && (v[i - 1] == ' ' || v[i - 1] == '\t');
    if (!q && !v.empty() && (v[0] == ';' || v[0] == '#')) q = true;
    return q ? "\"" + v + "\"" : v;
}

// The words the default file starts from: the built-in ones, or an old text file's (the fold).
struct UtbDefaults {
    // [tab] key -> value, in utbTabOrder; a missing key is not written (the built-in applies)
    std::map<std::string, std::string> tab;
    std::map<std::string, std::string> caption;   // group id -> caption override
    std::map<std::string, std::string> name;      // group id -> name override
};

// The [tab] keys in the order the file lists them. tooltip.roll and edit.caption are not among
// them: [tab] keys the README documents and the parser takes, never written into the generated
// file (so the generated bytes stay what they were before the keys existed).
inline const std::vector<std::string>& utbTabOrder() {
    static const std::vector<std::string> k = {
        "label.format.1", "label.format.2", "label.format.3", "label.format.4",
        "label.mode_unknown", "label.unknown", "tooltip.yes", "tooltip.no",
        "mat.caption", "own.caption", "next.caption",
        "font.caption", "font.label", "size.label", "size.caption_max", "size.caption",
        "color.caption", "color.label", "color.ring", "color.mark"};
    return k;
}

inline UtbDefaults utbBuiltinDefaults() {
    UtbDefaults d;
    for (int i = 0; i < utt::kTextDefaultFormats; ++i)
        d.tab["label.format." + std::to_string(i + 1)] = utt::utTextDefaultFormat(i);
    d.tab["label.mode_unknown"] = utt::utTextBuiltinPlain(utt::kTextModeUnknown);
    d.tab["label.unknown"] = utt::utTextBuiltinPlain(utt::kTextUnknown);
    d.tab["tooltip.yes"] = utt::utTextBuiltinPlain(utt::kTextTooltipYes);
    d.tab["tooltip.no"] = utt::utTextBuiltinPlain(utt::kTextTooltipNo);
    d.tab["mat.caption"] = "MAT";
    d.tab["own.caption"] = "OWN";
    d.tab["next.caption"] = ">>";
    return d;
}

// The view as the header names it: "the main campaign" for the base game (with or without its
// mods), "the custom game <key>" for one.
inline std::string utbViewTitle(const std::string& customKey) {
    return customKey.empty() ? std::string("the main campaign") : "the custom game " + customKey;
}

// A G line's label as the tab shows it: the game's colour codes ({^X} and ^X) removed, and the
// bytes taken as UTF-8 when they are, else as latin-1 (each byte one code point).
inline std::string utbGroupLabel(const std::string& raw) {
    std::string s;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '{' && i + 3 < raw.size() && raw[i + 1] == '^' && raw[i + 2] && raw[i + 3] == '}') {
            i += 3;
            continue;
        }
        if (raw[i] == '^' && i + 1 < raw.size()) {
            ++i;
            continue;
        }
        s += raw[i];
    }
    if (utt::utf8Valid(s)) return s;
    std::string u;
    for (unsigned char c : s) utt::utf8Append(c, u);
    return u;
}

// A G line label as a button name: at most 48 UTF-16 units (a longer one is cut at a code point).
inline std::string utbNameOf(const std::string& label) {
    std::string n = label;
    while (!n.empty() && utt::utf16Units(n) > 48) n = utt::utf8Prefix(n, utt::utf8Length(n) - 1);
    return n;
}

// ================================================================= the built-ins after 1.1.0
// The BUILT-IN groups that came after 1.1.0's 24: gen/ut_groups.h builtinGroups from row 25 on, in
// order (the blueprints), then the built-in template's two per-class groups (staff, spear1h - a
// game's Bows / Staves and Hunting Spears / Artifacts, which collect_mi=1 can bring to a game that
// had none: DeathGrimDawn's 12 Rare crystal weapons); test_buttons checks the list against both.
// A buttons file written before one of them existed in that game has no button for it -
// utbOfferNew appends one, once - and the default file names them on its "added once" line, so a
// button the user deletes from it is never offered back.
inline const std::vector<std::string>& utbLateBuiltinIds() {
    static const std::vector<std::string> v = {"blueprint", "staff", "spear1h"};
    return v;
}

// The [unassigned] comment line that remembers them. Everything after its ':' is the id list.
inline const char* utbOfferedPrefix() {
    return "; added once by the mod - delete its button and it stays deleted:";
}

// The default file, LF (the writer writes CRLF): the header, [tab], one [TAG] per group of the
// view in group order (name = its label, groups = its id), an empty [unassigned]. A tag the file
// grammar cannot take (reserved, or not 1..3 printable) is never produced by the groups file, but
// such a group would go to [unassigned] rather than make a file the parser refuses.
inline std::string utbDefaultText(const std::string& viewTitle, const std::vector<UtbGroupIn>& groups,
                                  const UtbDefaults& d = utbBuiltinDefaults()) {
    std::string t =
        "; uniquetab-buttons.ini - the buttons of the Unique Collection tab in " + viewTitle + ".\n"
        "; Written once by the mod; edit it freely (it is re-read within a second); delete it to\n"
        "; get the default back. UTF-8. A line starting with ; is a comment, and so is the rest of\n"
        "; a line after a blank and a ;. Put a value in \"double quotes\" to keep blanks or a ; in it.\n"
        ";\n"
        "; [TAG]    one button; the section name is its tag (1 to 3 characters; MAT, OWN and TAB\n"
        ";          are taken). Button order = file order.\n"
        ";   name    = the page name in the label line\n"
        ";   caption = the button's text when it is not the tag (optional)\n"
        ";   groups  = the item groups on it (ids from the groups file); several ids = one button,\n"
        ";             all of their items on one page, in group order\n"
        ";   size    = this button's largest caption size, 6..36 (optional)\n"
        ";   font    = this button's font, for example fonts/ru/savapromedium.fnt (optional; by\n"
        ";             default a font follows the letters it has to draw)\n"
        "; [tab]    the label line's templates and words, the rollover lines, the MAT, OWN and >>\n"
        ";          captions; it also takes font.caption, font.label, size.label,\n"
        ";          size.caption_max, size.caption and color.caption, color.label, color.ring,\n"
        ";          color.mark (RRGGBB). Label placeholders: {mode} {group} {owned} {entries}\n"
        ";          {first} {last} {rows} {all} {coll}.\n"
        "; [unassigned] the groups no button holds - their items have no page. The mod rewrites\n"
        ";          that section (only it) whenever the set changes: move an id from it into a\n"
        ";          button's groups line to show them.\n"
        "; A mistake anywhere makes the mod use the built-in buttons; the log names the line.\n"
        "\n"
        "[tab]\n";
    for (const std::string& k : utbTabOrder()) {
        const auto it = d.tab.find(k);
        if (it != d.tab.end()) t += utbPad(k, 18) + " = " + utbQuote(it->second) + "\n";
    }
    std::vector<std::string> unassigned;
    std::vector<std::string> seen;
    for (const UtbGroupIn& g : groups) {
        bool dup = false;
        for (const std::string& s : seen) dup |= utbUpper(s) == utbUpper(g.tag);
        if (!utbValidTag(g.tag) || utbReservedTag(g.tag) || dup) {
            unassigned.push_back(g.id);
            continue;
        }
        seen.push_back(g.tag);
        t += "\n[" + g.tag + "]\n";
        const auto nm = d.name.find(g.id);
        t += "name    = " + utbQuote(nm != d.name.end() ? nm->second : utbNameOf(g.label)) + "\n";
        const auto cp = d.caption.find(g.id);
        if (cp != d.caption.end()) t += "caption = " + utbQuote(cp->second) + "\n";
        t += "groups  = " + g.id + "\n";
    }
    t += "\n[unassigned]\n"
         "; rewritten by the mod: every group of this game that no button lists\n";
    std::string late;
    for (const std::string& id : utbLateBuiltinIds())
        for (const UtbGroupIn& g : groups)
            if (g.id == id) late += (late.empty() ? " " : ", ") + id;
    if (!late.empty()) t += std::string(utbOfferedPrefix()) + late + "\n";
    t += "groups =";
    for (std::size_t i = 0; i < unassigned.size(); ++i) t += (i ? ", " : " ") + unassigned[i];
    t += "\n";
    return t;
}

inline std::string utbCrlf(const std::string& lf) {
    std::string out;
    out.reserve(lf.size() + lf.size() / 20);
    for (char c : lf) {
        if (c == '\n') out += '\r';
        out += c;
    }
    return out;
}

// ================================================================= the [unassigned] rewrite
// The file with its [unassigned] section saying exactly `ids`; every other byte kept. Inside the
// section its comments and blank lines stay where they are; its key lines become ONE groups line,
// where the first of them was (else after the section's last comment, else right after the
// header). No section: one is appended, with the file's own line ending (CRLF when the file has
// one). Returns false (and `out` = bytes) when nothing would change.
inline bool utbRewriteUnassigned(const std::string& bytes, const std::vector<std::string>& ids,
                                 std::string* out) {
    const std::string eol = bytes.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::string want = "groups =";
    for (std::size_t i = 0; i < ids.size(); ++i) want += (i ? ", " : " ") + ids[i];
    struct Line { std::size_t b, next; bool ends; std::string trim; };
    std::vector<Line> lines;
    std::size_t pos = 0;
    while (pos < bytes.size()) {
        const std::size_t nl = bytes.find('\n', pos);
        const std::size_t next = nl == std::string::npos ? bytes.size() : nl + 1;
        std::size_t e = nl == std::string::npos ? bytes.size() : nl;
        if (e > pos && bytes[e - 1] == '\r') --e;
        lines.push_back({pos, next, nl != std::string::npos, utt::utTextTrim(bytes.substr(pos, e - pos))});
        pos = next;
    }
    auto isHeader = [&](std::size_t i) { return !lines[i].trim.empty() && lines[i].trim[0] == '['; };
    auto isComment = [&](std::size_t i) {
        return !lines[i].trim.empty() && (lines[i].trim[0] == ';' || lines[i].trim[0] == '#');
    };
    std::size_t head = lines.size();
    for (std::size_t i = 0; i < lines.size() && head == lines.size(); ++i) {
        if (!isHeader(i)) continue;
        const std::size_t close = lines[i].trim.find(']');
        if (close != std::string::npos &&
            utt::utTextLower(utt::utTextTrim(lines[i].trim.substr(1, close - 1))) == "unassigned")
            head = i;
    }
    std::string r;
    if (head == lines.size()) {
        r = bytes;
        if (!r.empty() && r.back() != '\n') r += eol;
        r += eol + "[unassigned]" + eol +
             "; rewritten by the mod: every group of this game that no button lists" + eol + want + eol;
    } else {
        std::size_t end = head + 1;
        while (end < lines.size() && !isHeader(end)) ++end;
        std::size_t firstKey = 0, lastComment = head;
        for (std::size_t i = head + 1; i < end; ++i) {
            if (isComment(i)) { if (!firstKey) lastComment = i; }
            else if (!lines[i].trim.empty() && !firstKey) firstKey = i;
        }
        auto copyLine = [&](std::size_t i) {
            r += bytes.substr(lines[i].b, lines[i].next - lines[i].b);
            if (!lines[i].ends) r += eol;   // the file's unterminated last line, before ours
        };
        r = bytes.substr(0, lines[head].b);
        copyLine(head);
        if (!firstKey && lastComment == head) r += want + eol;
        for (std::size_t i = head + 1; i < end; ++i) {
            const bool key = !lines[i].trim.empty() && !isComment(i);
            if (key) {
                if (i == firstKey) r += want + eol;
                continue;
            }
            if (!lines[i].ends && i + 1 == lines.size() && (firstKey || lastComment != i)) {
                r += bytes.substr(lines[i].b);   // the last line keeps its missing ending
                continue;
            }
            copyLine(i);
            if (!firstKey && i == lastComment) r += want + eol;
        }
        if (end < lines.size()) r += bytes.substr(lines[end].b);
    }
    *out = r;
    return r != bytes;
}

// ================================================================= a new built-in group's button, ONCE
struct UtbLine {
    std::size_t b, next;   // the line's first byte; the next line's first byte
    bool ends;             // it has a line ending
    std::string trim;
};
inline std::vector<UtbLine> utbLines(const std::string& bytes) {
    std::vector<UtbLine> lines;
    std::size_t pos = 0;
    while (pos < bytes.size()) {
        const std::size_t nl = bytes.find('\n', pos);
        const std::size_t next = nl == std::string::npos ? bytes.size() : nl + 1;
        std::size_t e = nl == std::string::npos ? bytes.size() : nl;
        if (e > pos && bytes[e - 1] == '\r') --e;
        lines.push_back({pos, next, nl != std::string::npos, utt::utTextTrim(bytes.substr(pos, e - pos))});
        pos = next;
    }
    return lines;
}
// A section header's name, lower-cased ("" when the line is no header).
inline std::string utbHeaderName(const std::string& trim) {
    if (trim.empty() || trim[0] != '[') return "";
    const std::size_t close = trim.find(']');
    if (close == std::string::npos) return "";
    return utt::utTextLower(utt::utTextTrim(trim.substr(1, close - 1)));
}
inline bool utbIsOfferedLine(const std::string& trim) {
    const std::string p = utbOfferedPrefix();
    const std::size_t colon = p.find(':');
    return trim.compare(0, colon, p, 0, colon) == 0;
}

// The ids on the [unassigned] section's "added once" line, lower-cased, in file order.
inline std::vector<std::string> utbOfferedIds(const std::string& bytes) {
    std::vector<std::string> ids;
    const std::vector<UtbLine> lines = utbLines(bytes);
    bool in = false;
    for (const UtbLine& l : lines) {
        const std::string h = utbHeaderName(l.trim);
        if (!h.empty()) {
            in = h == "unassigned";
            continue;
        }
        if (!in || !utbIsOfferedLine(l.trim)) continue;
        std::string rest = l.trim.substr(l.trim.find(':') + 1);
        std::size_t p = 0;
        while (p <= rest.size()) {
            std::size_t c = rest.find(',', p);
            if (c == std::string::npos) c = rest.size();
            const std::string id = utt::utTextLower(utt::utTextTrim(rest.substr(p, c - p)));
            if (!id.empty()) ids.push_back(id);
            p = c + 1;
        }
    }
    return ids;
}

// A BUILT-IN group that came after 1.1.0 (`late`, utbLateBuiltinIds) gets a button ONCE. For each
// such group of the view that no button lists, that is NOT under the file's [unassigned] (a group
// the user - or an earlier rewrite - already put there stays there: that is the opt-out) and that
// is not on the "added once" line, a [TAG] section (the default file's three lines: name = the
// label, caption when the defaults have one, groups = the id) is inserted after the LAST button
// section, and the id is added to the "added once" line of [unassigned] (a new line after the
// section's leading comments, or a new [unassigned] section at the end). Every other byte is kept;
// the file's own line ending is used. A group whose tag a button already has (ignoring case), or a
// tag the grammar cannot take, gets no section - only the "added once" line - and lands in
// [unassigned] with the next rewrite. Returns false (`out` = bytes) when nothing is new.
// `added` / `refused` name the ids, for the one INFO line.
inline bool utbOfferNew(const std::string& bytes, const UtbFile& f, const std::vector<UtbGroupIn>& groups,
                        const std::vector<std::string>& late, std::string* out,
                        std::vector<std::string>* added, std::vector<std::string>* refused,
                        const UtbDefaults& d = utbBuiltinDefaults()) {
    *out = bytes;
    auto has = [](const std::vector<std::string>& v, const std::string& x) {
        for (const std::string& y : v)
            if (y == x) return true;
        return false;
    };
    const std::vector<std::string> offered = utbOfferedIds(bytes);
    std::vector<std::string> onButton, tags, listed;
    for (const UtbButton& b : f.buttons) {
        tags.push_back(utbUpper(b.tag));
        for (const std::string& id : b.groups) onButton.push_back(utt::utTextLower(id));
    }
    for (const std::string& id : f.unassigned) listed.push_back(utt::utTextLower(id));
    std::string sections;   // LF; converted below
    std::vector<std::string> fresh;
    for (const std::string& id : late) {
        const UtbGroupIn* g = nullptr;
        for (const UtbGroupIn& x : groups)
            if (x.id == id) g = &x;
        if (!g || has(onButton, id) || has(listed, id) || has(offered, id)) continue;
        fresh.push_back(id);
        if (!utbValidTag(g->tag) || utbReservedTag(g->tag) || has(tags, utbUpper(g->tag))) {
            if (refused) refused->push_back(id);
            continue;
        }
        tags.push_back(utbUpper(g->tag));
        if (added) added->push_back(id);
        if (!sections.empty()) sections += "\n";
        sections += "[" + g->tag + "]\n";
        const auto nm = d.name.find(g->id);
        sections += "name    = " + utbQuote(nm != d.name.end() ? nm->second : utbNameOf(g->label)) + "\n";
        const auto cp = d.caption.find(g->id);
        if (cp != d.caption.end()) sections += "caption = " + utbQuote(cp->second) + "\n";
        sections += "groups  = " + g->id + "\n";
    }
    if (fresh.empty()) return false;
    const bool crlf = bytes.find("\r\n") != std::string::npos;
    const std::string eol = crlf ? "\r\n" : "\n";
    auto native = [&](const std::string& lf) { return crlf ? utbCrlf(lf) : lf; };
    std::string ids;
    for (const std::string& id : offered) ids += (ids.empty() ? " " : ", ") + id;
    for (const std::string& id : fresh) ids += (ids.empty() ? " " : ", ") + id;
    const std::string marker = std::string(utbOfferedPrefix()) + ids;

    const std::vector<UtbLine> lines = utbLines(bytes);
    const std::size_t n = lines.size();
    std::size_t head = n, lastEnd = n, markerAt = n, markerInsert = n;
    bool anyButton = false;
    for (std::size_t i = 0; i < n; ++i) {
        const std::string h = utbHeaderName(lines[i].trim);
        if (h.empty()) continue;
        std::size_t end = i + 1;
        while (end < n && utbHeaderName(lines[end].trim).empty()) ++end;
        if (h == "unassigned") {
            if (head == n) {
                head = i;
                std::size_t p = i + 1;
                while (p < end && !lines[p].trim.empty() && (lines[p].trim[0] == ';' || lines[p].trim[0] == '#')) {
                    if (utbIsOfferedLine(lines[p].trim)) markerAt = p;
                    ++p;
                }
                for (std::size_t q = p; q < end && markerAt == n; ++q)
                    if (utbIsOfferedLine(lines[q].trim)) markerAt = q;
                markerInsert = p;
            }
        } else if (h != "tab") {
            anyButton = true;
            lastEnd = end;
        }
    }
    std::size_t ins = anyButton ? lastEnd : head;   // after the last button; none: before [unassigned]
    std::vector<std::string> before(n + 1);
    // the sections: a blank line before them unless there is one already, and one after them when
    // a section follows
    if (!sections.empty()) {
        std::string blk;
        if (ins == n && n && !lines[n - 1].ends) blk += eol;
        if (ins > 0 && !lines[ins - 1].trim.empty()) blk += eol;
        blk += native(sections);
        if (ins < n) blk += eol;
        before[ins] += blk;
    }
    std::string tail;
    if (head == n) {
        if (sections.empty() && n && !lines[n - 1].ends) tail += eol;
        tail += eol + "[unassigned]" + eol + "; rewritten by the mod: every group of this game that no button lists" +
                eol + marker + eol;
    } else if (markerAt == n) {
        std::string m;
        if (markerInsert == n && n && !lines[n - 1].ends) m += eol;
        m += marker + eol;
        before[markerInsert] = m + before[markerInsert];
    }
    std::string r;
    for (std::size_t i = 0; i < n; ++i) {
        r += before[i];
        if (i == markerAt) {
            r += marker;
            const std::size_t e = lines[i].next;
            std::size_t le = e;
            if (lines[i].ends) {
                le = e - 1;
                if (le > lines[i].b && bytes[le - 1] == '\r') --le;
                r += bytes.substr(le, e - le);
            }
            continue;
        }
        r += bytes.substr(lines[i].b, lines[i].next - lines[i].b);
    }
    r += before[n];
    r += tail;
    *out = r;
    return r != bytes;
}

// ================================================================= the fold of an old text file
// An old uniquetab-text.ini's words, as the default file's starting words (read ONCE, at the
// first buttons file the mod writes): [text] keys, the [text.<lang>] its [style] language named
// over them key by key (as the old chain applied them), [style] font/size/colour keys. caption.<id>
// / name.<id> of a group of this view become that button's caption / name; caption.vanilla,
// caption.owned, caption.next become mat/own/next.caption. Not carried: @game names and name.*
// (the game's words are not in the buttons file), [style] language, an id no group of this view
// has. `notes` says what was left behind.
inline UtbDefaults utbFoldText(const UtTextFile& old, const std::vector<UtbGroupIn>& groups,
                               std::vector<std::string>* notes) {
    UtbDefaults d = utbBuiltinDefaults();
    std::map<std::string, std::string> keys;
    for (const auto& kv : old.common) keys[kv.first] = kv.second.text;
    if (const utt::UtTextSection* ls = utt::utTextLangSectionOf(&old))
        for (const auto& kv : *ls) keys[kv.first] = kv.second.text;
    // a template list is taken whole from the section that sets any of them
    bool langFormats = false;
    if (const utt::UtTextSection* ls = utt::utTextLangSectionOf(&old))
        for (int i = 1; i <= utt::kFormatMax; ++i) langFormats |= ls->count("label.format." + std::to_string(i)) != 0;
    bool anyFormat = false;
    for (int i = 1; i <= utt::kFormatMax; ++i) anyFormat |= keys.count("label.format." + std::to_string(i)) != 0;
    if (anyFormat) {
        for (int i = 1; i <= utt::kFormatMax; ++i) d.tab.erase("label.format." + std::to_string(i));
        const utt::UtTextSection* src = langFormats ? utt::utTextLangSectionOf(&old) : &old.common;
        for (int i = 1; i <= utt::kFormatMax; ++i) {
            const auto it = src->find("label.format." + std::to_string(i));
            if (it != src->end()) d.tab["label.format." + std::to_string(i)] = it->second.text;
        }
    }
    for (const auto& kv : keys) {
        const std::string& k = kv.first;
        if (k.compare(0, 13, "label.format.") == 0) continue;
        if (kv.second == utt::kTextGameValue) {
            if (notes) notes->push_back(k + " = @game (the game's own words are not in the buttons file)");
            continue;
        }
        const std::string tab = utbTextKeyToTab(k);
        if (!tab.empty()) { d.tab[tab] = kv.second; continue; }
        const bool cap = k.compare(0, 8, "caption.") == 0, nm = k.compare(0, 5, "name.") == 0;
        const std::string id = cap ? k.substr(8) : nm ? k.substr(5) : std::string();
        bool inView = false;
        for (const UtbGroupIn& g : groups) inView |= g.id == id;
        if (inView) {
            (cap ? d.caption : d.name)[id] = kv.second;
        } else if (notes) {
            notes->push_back(k + " (no group \"" + id + "\" in this game)");
        }
    }
    for (int i = 0; i < utt::kStyleFontCount; ++i)
        if (!old.style.font[i].empty()) d.tab[utt::utStyleFontKey(i)] = old.style.font[i];
    for (int i = 0; i < utt::kStyleSizeCount; ++i)
        if (old.style.size[i] >= 0) d.tab[utt::utStyleSizeKey(i)] = std::to_string(old.style.size[i]);
    for (int i = 0; i < utt::kStyleColorCount; ++i) {
        if (old.style.color[i] < 0) continue;
        char buf[8];
        std::snprintf(buf, sizeof buf, "%06lX", static_cast<unsigned long>(old.style.color[i]));
        d.tab[utt::utStyleColorKey(i)] = buf;
    }
    if (!old.style.language.empty() && notes)
        notes->push_back("[style] language = " + old.style.language + " (its [text." + old.style.language +
                         "] words were folded in; the buttons file has one set of words)");
    return d;
}

// ================================================================= the font follows the letters
// The language folders of resources\Fonts.arc with a baked savapromedium.fnt, in the order the
// derivation tries them after the script's own.
inline std::vector<std::string> utbFontFolderList() {
    std::vector<std::string> out;
    const std::string s = utt::utTextFontFolders();
    std::size_t b = 0;
    while (b < s.size()) {
        std::size_t e = s.find(' ', b);
        if (e == std::string::npos) e = s.size();
        if (e > b) out.push_back(s.substr(b, e - b));
        b = e + 1;
    }
    return out;
}

// The FNTD copies of savapromedium - a TrueType font the engine rasterises through GDI at run
// time, so a letter the font lacks is drawn by Windows' own fallback font (GDI font linking)
// - in the order the derivation tries them after every FNTX copy. th and vi carry zh's
// font (WenQuanYi Micro Hei); ja is Noto Sans JP, ko NanumGothic.
inline std::vector<std::string> utbDynamicFolderList() { return {"zh", "ja", "ko"}; }

// The FNTD folder a CJK letter's script names: kana -> ja, Hangul -> ko, Han (and the CJK
// punctuation, Bopomofo and fullwidth forms) -> zh; nullptr for any other letter.
inline const char* utbCjkFolderFor(char32_t cp) {
    if ((cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0x31F0 && cp <= 0x31FF) || (cp >= 0xFF65 && cp <= 0xFF9F))
        return "ja";
    if ((cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0x3130 && cp <= 0x318F) || (cp >= 0xA960 && cp <= 0xA97F) ||
        (cp >= 0xAC00 && cp <= 0xD7FF) || (cp >= 0xFFA0 && cp <= 0xFFDC))
        return "ko";
    if ((cp >= 0x3000 && cp <= 0x303F) || (cp >= 0x3100 && cp <= 0x312F) || (cp >= 0x3400 && cp <= 0x4DBF) ||
        (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFF00 && cp <= 0xFF60))
        return "zh";
    return nullptr;
}

// Does the copy of savapromedium in `folder` ("" = the root font) have a glyph for cp? For an FNTD
// folder: does its TrueType cmap map cp (ut_sfnt.h)?
typedef bool (*UtbFolderCoversFn)(const std::string& folder, char32_t cp, const void* ctx);

struct UtbDerived {
    std::string folder;        // "" = the root font
    bool coversAll = true;
    std::size_t covered = 0;
    char32_t missing = 0;      // the first code point it lacks
    bool dynamic = false;      // an FNTD copy: the letters it lacks are drawn by Windows' fallback
    std::size_t lacking = 0;   // how many of the letters it lacks
};

// The font a role draws in when no font line names one. Letters above U+FFFF never decide it: no
// font of the game can draw them (the engine's glyph key is one UTF-16 unit). Over the others:
// 1. the ROOT font when it has every one (English, or no letter outside it: the identity);
// 2. the folder the letters' script names - with `dynamicFonts`, any kana -> ja, else any Hangul
//    -> ko, else any Han -> zh; else the first letter the root lacks: Cyrillic ru, Greek el;
// 3. every FNTX copy the game ships in utbFontFolderList order - the first with them all;
// 4. with `dynamicFonts` (the FNTD copies' cmaps are known to `covers`): utbDynamicFolderList;
// 5. when none has them all: with `dynamicFonts` zh (coversAll false, dynamic: Windows draws the
//    `lacking` letters it has no glyph for - one INFO line, utbDerivedFallbackText); without, the
//    copy with the most (on a tie the earlier, the root font first), coversAll false.
inline UtbDerived utbDeriveFolder(const std::u32string& all, UtbFolderCoversFn covers, const void* ctx,
                                  bool dynamicFonts = false) {
    UtbDerived root;
    std::u32string cps;
    for (char32_t c : all) if (c <= 0xFFFF) cps += c;
    if (cps.empty() || !covers) return root;
    const std::vector<std::string> dyn = dynamicFonts ? utbDynamicFolderList() : std::vector<std::string>();
    auto measure = [&](const std::string& folder, UtbDerived* d) {
        d->folder = folder;
        d->covered = 0;
        d->coversAll = true;
        d->missing = 0;
        d->dynamic = false;
        for (const std::string& x : dyn) d->dynamic |= x == folder;
        for (char32_t cp : cps) {
            if (covers(folder, cp, ctx)) { ++d->covered; continue; }
            if (d->coversAll) d->missing = cp;
            d->coversAll = false;
        }
        d->lacking = cps.size() - d->covered;
    };
    measure(std::string(), &root);
    if (root.coversAll) return root;
    std::vector<std::string> order;
    const char* script = nullptr;
    if (dynamicFonts) {
        int rank = 0;   // ja 3, ko 2, zh 1
        for (char32_t cp : cps) {
            if (covers(std::string(), cp, ctx)) continue;
            const char* f = utbCjkFolderFor(cp);
            const int r = !f ? 0 : f[0] == 'j' ? 3 : f[0] == 'k' ? 2 : 1;
            if (r > rank) {
                rank = r;
                script = f;
            }
        }
    }
    if (!script) script = utt::utTextFontFolderFor(root.missing);
    if (script) order.push_back(script);
    std::vector<std::string> rest = utbFontFolderList();
    rest.insert(rest.end(), dyn.begin(), dyn.end());
    for (const std::string& f : rest) {
        bool dup = false;
        for (const std::string& o : order) dup |= o == f;
        if (!dup) order.push_back(f);
    }
    UtbDerived best = root;
    for (const std::string& folder : order) {
        UtbDerived d;
        measure(folder, &d);
        if (d.coversAll) return d;
        if (d.covered > best.covered) best = d;
    }
    if (dynamicFonts) {
        UtbDerived z;
        measure("zh", &z);
        return z;
    }
    return best;
}

// The one INFO line of a role drawn in an FNTD copy that lacks some of its letters ("" when it
// lacks none, or the font is a baked one): `role` is "the captions" or "the label line"; `path`
// the font when it is not the folder's savapromedium.
inline std::string utbDerivedFallbackText(const UtbDerived& d, const std::string& role,
                                          const std::string& path = std::string()) {
    if (d.coversAll || !d.dynamic || !d.lacking || !d.missing) return std::string();
    std::string s = (path.empty() ? "fonts/" + d.folder + "/savapromedium.fnt" : path) + " is used for " + role +
                    "; " + utt::utTextCodePointName(d.missing);
    if (d.lacking > 1) s += " and " + std::to_string(d.lacking - 1) + " more";
    s += d.lacking > 1 ? " are" : " is";
    return s + " drawn by Windows' own fallback font";
}

inline std::string utbFolderFont(const std::string& folder) {
    return folder.empty() ? std::string("fonts/savapromedium.fnt") : "fonts/" + folder + "/savapromedium.fnt";
}

// What the glyph check's WARN advises after "... the next source is used instead": "" for nothing.
// utt::utTextFontAdvice's text over the passed-over candidates (`skipped`), made exact with the
// copies the game ships (`covers`, when `tablesKnown`): per role, the first letter passed over -
// - above U+FFFF: no font of the game can draw it, so no font line can (said instead of advice);
// - in no baked copy (FNTX) but drawable by an FNTD copy (`dynamicFonts`): the advice names that
//   copy (fonts/zh|ja|ko/savapromedium.fnt, by the letter's script) instead of "fonts/<lang>/...";
// - in no copy at all (the FNTD cmaps not known): utTextFontAdvice's own wording - no font line
//   can draw it.
// The rollover lines' own advice, if any, is kept after it.
inline std::string utbFontAdvice(const std::vector<std::string>& skipped, UtbFolderCoversFn covers,
                                 const void* ctx, bool tablesKnown, bool dynamicFonts) {
    std::string advice = utt::utTextFontAdvice(skipped);
    const std::string from = "under [style] of uniquetab-text.ini";
    const std::size_t at = advice.find(from);
    if (at != std::string::npos) advice.replace(at, from.size(), "under [tab] (or font = on the button)");
    char32_t none = 0;
    for (const char* role : {"caption", "label"}) {
        const std::string tail = std::string(" is not in the ") + role + " font";
        for (const std::string& s : skipped) {
            const std::size_t t = s.find(tail);
            const std::size_t u = t == std::string::npos ? t : s.rfind("U+", t);
            if (u == std::string::npos) continue;
            const char32_t cp = (char32_t)std::strtoul(s.c_str() + u + 2, nullptr, 16);
            if (cp > 0xFFFF) {
                if (!none) none = cp;
                break;
            }
            if (!tablesKnown || !covers) break;
            bool baked = covers(std::string(), cp, ctx);
            for (const std::string& f : utbFontFolderList()) baked = baked || covers(f, cp, ctx);
            if (baked) break;
            if (!dynamicFonts) {
                if (!none) none = cp;
                break;
            }
            const UtbDerived d = utbDeriveFolder(std::u32string(1, cp), covers, ctx, true);
            const std::string lang = std::string("font.") + role + " = fonts/<lang>/savapromedium.fnt (<lang> one of " +
                                     utt::utTextFontFolders() + ")";
            const std::size_t l = advice.find(lang);
            if (l != std::string::npos && !d.folder.empty())
                advice.replace(l, lang.size(), std::string("font.") + role + " = " + utbFolderFont(d.folder));
            break;
        }
    }
    if (!none) return advice;
    std::vector<std::string> tip;
    for (const std::string& s : skipped)
        if (s.find(" is not in the tooltip font") != std::string::npos) tip.push_back(s);
    const std::string rest = utt::utTextFontAdvice(tip);
    const std::string why = none > 0xFFFF ? "letters above U+FFFF are not drawn"
                                          : "Chinese, Japanese and Korean letters are not drawn";
    return "no font of the game has " + utt::utTextCodePointName(none) + ", so no font line can draw it (" + why +
           " - README \"Alphabets\")" + (rest.empty() ? "" : "; " + rest);
}

// The code points outside printable ASCII a role draws from the file (deduplicated, in order):
// the caption role - the buttons' captions without a font line of their own, mat/own/next.caption;
// the label role - the buttons' names, the label templates' text and words.
inline std::u32string utbRoleCodePoints(const UtbFile& f, utt::UtTextFont role) {
    std::u32string out;
    auto add = [&](const std::string& s) {
        std::u32string cps;
        if (!utt::utf8Decode(s, cps)) return;
        for (char32_t c : cps) {
            if (c >= 0x20 && c <= 0x7E) continue;
            if (out.find(c) == std::u32string::npos) out += c;
        }
    };
    if (role == utt::kFontCaption) {
        for (const UtbButton& b : f.buttons) if (b.font.empty()) add(b.caption);
        for (const char* k : {"caption.vanilla", "caption.owned", "caption.next", "caption.edit"}) {
            const auto it = f.words.common.find(k);
            if (it != f.words.common.end()) add(it->second.text);
        }
    } else if (role == utt::kFontLabel) {
        for (const UtbButton& b : f.buttons) add(b.name);
        for (const auto& kv : f.words.common)
            if (kv.first.compare(0, 6, "label.") == 0) add(kv.second.text);
    }
    return out;
}

}  // namespace utb
