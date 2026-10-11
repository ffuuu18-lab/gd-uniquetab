// ut_groups.h - the collection's groups as data: the uniquetab-groups.ini parser, the matcher,
// the auto groups, the journal count scan (the generator's report of the owned records no group
// takes), and the G line of uniq-groups.txt.
//
// Pure C++17: no Windows, no engine, no globals. src\gen\pages_gen.cpp groups the collection
// with it and tools\test_groups.cpp links this very header.
//
// THE FILE (uniquetab-groups.ini, UTF-8, an optional BOM, CRLF or LF):
//   ; comment          a line whose first non-blank character is ';' or '#'; " ;" or " #"
//                      after a value starts a comment too
//   [group <id>]       a rule block feeding group <id> (grammar [a-z0-9_.-]{1,40}). The first
//                      block of an id declares the group: its position among the groups is the
//                      position of that block. A later block of the same id adds rules to it.
//     label = Helms    the caption text (1..48 bytes, no tab); default: the built-in label of
//                      that id, else DERIVED (below), else the id's last part
//     tag   = HEL      exactly 3 printable ASCII characters; default: the built-in tag, else
//                      DERIVED (below)
//     class = A, B     filters; values in one key are alternatives (OR), keys combine (AND).
//     path  = glob     A block needs at least one filter. class and rank compare without case;
//     rank  = Epic     a path glob is matched against the lower-cased record path: '*' is one
//     flag  = set      path segment or part of one, '**' any run including '/', '?' one
//     source = mod     character. flag: set, relic, equipment, faction, blueprint, augment,
//                      extra. source: base (the base game has the record) or mod (only a mod
//                      layer has it). rank: an itemClassification word, or any.
//   [exclude]          a rule block whose match keeps the record off every page
//   [view:<key>]       the blocks that follow apply only in a view that answers to <key>
//                      (see viewKeysFor); [view:*] returns to every view
//   [auto]             min = N (1..1000, default 5), enabled = yes|no (default yes)
//
// THE MATCH. Every candidate record is tried against the blocks active in the view, in file
// order; the first block that matches decides: its group, or no page for an [exclude]. A record
// no block matches and whose class the generator does not know gets an AUTO group named from
// its class (auto.<class>); classes with fewer than `min` such records share one group "other".
// A record no block matches and whose class the generator knows is not paged - an owned one
// too: the journal keeps its row, no page shows it, and it is back the moment a group takes it
// again (the generator's report and the label's "+N not on these pages" count such rows). Groups
// that receive no record emit nothing. Display order: the declared groups, then the auto groups
// by class name, then "other".
//
// A file with any error is refused WHOLE (the error names the line); the caller then uses the
// built-in template alone.
//
// THE USER'S FILE IS ADDITIVE (mergeGroupSets): ut_groups_default.h's template is the built-in
// set, compiled in and never written; an optional uniquetab-groups.ini is parsed on its own (its
// errors name ITS lines) and put IN FRONT of it: its rules (an [exclude] included) are tried
// before the built-in ones, its new groups come first, a block of a built-in id adds rules to
// that group and its label/tag replace the built-in words, and its [auto] keys apply last.
// Two groups with the same label stay two groups (the buttons file puts groups under one button).
//
// THE DERIVED WORDS (resolveNames, after the match, over the groups that received a record): a
// group with no label and no built-in one, whose first class has an engine type word
// (classTypeTag: Game.dll's class -> tag switches), is labelled with the VIEW's English text for
// that tag (GroupSet::words, filled by catalogue_gen's classWords; the class's last part when
// the text has none), pluralised (pluralWord); a label another group of the view already has gets
// the class family word in front ("Hunting Spears"). A tag no key and no built-in sets is the
// first of: the first three letters of the word (or of the label), the first letter and the next
// two consonants, the first two characters and 0-9 then A-Z - the first one no other group of
// the view holds (MAT and OWN never). Deterministic in group order. An [auto] group is named the
// same way; a class with no type word keeps the fixed table (autoNaming).
#pragma once

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace utg {

// ---------------------------------------------------------------- the id and the tag
const std::size_t kIdMax = 40;
const std::size_t kLabelMax = 48;

inline bool validId(const std::string& s) {
    if (s.empty() || s.size() > kIdMax) return false;
    for (unsigned char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-'))
            return false;
    return true;
}
inline bool validTag(const std::string& s) {
    if (s.size() != 3) return false;
    for (unsigned char c : s) if (c < 0x21 || c > 0x7E) return false;
    return true;
}
inline bool validLabel(const std::string& s) {
    if (s.empty() || s.size() > kLabelMax) return false;
    for (unsigned char c : s) if (c < 0x20 || c == 0x7F) return false;
    return true;
}
// A view key in [view:<key>]: the id grammar plus '+' (basemods+custom-<name>).
inline bool validViewKey(const std::string& s) {
    if (s.empty() || s.size() > 80) return false;
    for (unsigned char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-'
              || c == '+'))
            return false;
    return true;
}

inline std::string lowerAscii(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

// ---------------------------------------------------------------- the built-in table
// The 24 groups 1.1.0 writes, in its order, then the blueprints 1.2.0 added after the relics:
// id = the generator's slot word, label = the G line's label, tag = the pad's caption, cls = the
// item class the slot comes from. A 7-field G line (1.1.0's) only ever carries the first 24.
struct BuiltinGroup { const char* id; const char* label; const char* tag; const char* cls; };
inline const BuiltinGroup* builtinGroups(std::size_t* count) {
    static const BuiltinGroup kTable[] = {
        {"head", "Helms", "HEL", "ArmorProtective_Head"},
        {"shoulders", "Shoulders", "SHO", "ArmorProtective_Shoulders"},
        {"chest", "Chest", "CHE", "ArmorProtective_Chest"},
        {"hands", "Gloves", "GLO", "ArmorProtective_Hands"},
        {"waist", "Belts", "BEL", "ArmorProtective_Waist"},
        {"legs", "Pants", "PAN", "ArmorProtective_Legs"},
        {"feet", "Boots", "BOO", "ArmorProtective_Feet"},
        {"amulet", "Amulets", "AMU", "ArmorJewelry_Amulet"},
        {"medal", "Medals", "MED", "ArmorJewelry_Medal"},
        {"ring", "Rings", "RNG", "ArmorJewelry_Ring"},
        {"offhand", "Off-hands", "OFF", "WeaponArmor_Offhand"},
        {"shield", "Shields", "SHD", "WeaponArmor_Shield"},
        {"axe1h", "1H Axes", "AX1", "WeaponMelee_Axe"},
        {"dagger", "Daggers", "DAG", "WeaponMelee_Dagger"},
        {"mace1h", "1H Maces", "MC1", "WeaponMelee_Mace"},
        {"scepter", "Scepters", "SCP", "WeaponMelee_Scepter"},
        {"sword1h", "1H Swords", "SW1", "WeaponMelee_Sword"},
        {"ranged1h", "Guns", "GUN", "WeaponHunting_Ranged1h"},
        {"axe2h", "2H Axes", "AX2", "WeaponMelee_Axe2h"},
        {"mace2h", "2H Maces", "MC2", "WeaponMelee_Mace2h"},
        {"spear2h", "Spears", "SPR", "WeaponMelee_Spear2h"},
        {"sword2h", "2H Swords", "SW2", "WeaponMelee_Sword2h"},
        {"ranged2h", "2H Guns", "GN2", "WeaponHunting_Ranged2h"},
        {"relic", "Relics", "REL", "ItemArtifact"},
        {"blueprint", "Blueprints", "BLU", "ItemArtifactFormula"},
    };
    if (count) *count = sizeof kTable / sizeof *kTable;
    return kTable;
}
inline const BuiltinGroup* builtinById(const std::string& id) {
    std::size_t n = 0;
    const BuiltinGroup* t = builtinGroups(&n);
    for (std::size_t i = 0; i < n; ++i) if (id == t[i].id) return &t[i];
    return nullptr;
}
inline const BuiltinGroup* builtinByLabel(const std::string& label) {
    std::size_t n = 0;
    const BuiltinGroup* t = builtinGroups(&n);
    for (std::size_t i = 0; i < n; ++i) if (label == t[i].label) return &t[i];
    return nullptr;
}
// Ids no [group] may take: the pad's non-group buttons (the strings file's caption.next is the
// NEXT button of a crowded pad) and the generator's own groups.
inline bool reservedId(const std::string& id) {
    return id == "vanilla" || id == "owned" || id == "next" || id == "other"
        || id.compare(0, 5, "auto.") == 0;
}
// Tags no group may take: the materials page and the owned-only filter keep theirs.
inline bool reservedTag(const std::string& tag) { return tag == "MAT" || tag == "OWN"; }

// The first three letters or digits of a text, upper-cased, padded with 'X'.
inline std::string tagFrom(const std::string& text) {
    std::string t;
    for (unsigned char c : text) {
        if (t.size() == 3) break;
        if (c >= 'a' && c <= 'z') t += char(c - 'a' + 'A');
        else if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) t += char(c);
    }
    while (t.size() < 3) t += 'X';
    return t;
}

// ---------------------------------------------------------------- the class word
// The engine's own type word per item class: Game.dll 0x576090 (a switch on the weapon type) and
// 0x3145B0 (armour and jewellery) print these tags under an item's name. The class -> type
// pairing follows the tag names. Null for any other class.
struct ClassTag { const char* cls; const char* tag; };   // cls lower-cased
inline const ClassTag* classTypeTags(std::size_t* count) {
    static const ClassTag kTable[] = {
        {"weaponmelee_axe", "tagOneHandedAxe"},       {"weaponmelee_sword", "tagOneHandedSword"},
        {"weaponmelee_mace", "tagOneHandedMace"},     {"weaponmelee_dagger", "tagOneHandedDagger"},
        {"weaponmelee_scepter", "tagOneHandedScepter"}, {"weaponhunting_spear", "tagSpear"},
        {"weaponhunting_ranged2h", "tagTwoHandedRifle"}, {"weaponmagical_staff", "tagStaff"},
        {"weaponarmor_shield", "tagShield"},          {"weaponarmor_offhand", "tagOffHand"},
        {"weaponhunting_ranged1h", "tagOneHandedPistol"}, {"weaponmelee_axe2h", "tagTwoHandedAxe"},
        {"weaponmelee_sword2h", "tagTwoHandedSword"}, {"weaponmelee_mace2h", "tagTwoHandedMace"},
        {"weaponmelee_spear2h", "tagTwoHandedSpear"},
        {"armorprotective_head", "tagHead"},          {"armorjewelry_amulet", "tagAmulet"},
        {"armorprotective_chest", "tagChest"},        {"armorprotective_legs", "tagLegs"},
        {"armorprotective_feet", "tagFeet"},          {"armorjewelry_ring", "tagRing"},
        {"armorprotective_hands", "tagGloves"},       {"armorprotective_waist", "tagBelt"},
        {"armorprotective_shoulders", "tagShoulders"}, {"armorjewelry_medal", "tagMedal"},
    };
    if (count) *count = sizeof kTable / sizeof *kTable;
    return kTable;
}
inline const char* classTypeTag(const std::string& cls) {
    std::size_t n = 0;
    const ClassTag* t = classTypeTags(&n);
    const std::string c = lowerAscii(cls);
    for (std::size_t i = 0; i < n; ++i) if (c == t[i].cls) return t[i].tag;
    return nullptr;
}

// The class's last '_' part, as written ("WeaponMagical_Staff" -> "Staff").
inline std::string classLastPart(const std::string& cls) {
    std::size_t u = cls.rfind('_');
    return u == std::string::npos ? cls : cls.substr(u + 1);
}

// The class family word: the part before the first '_' without its Weapon/Armor/Item prefix,
// first letter upper-cased ("WeaponHunting_Spear" -> "Hunting"). Empty when there is none.
inline std::string classFamilyWord(const std::string& cls) {
    std::size_t u = cls.find('_');
    if (u == std::string::npos || u == 0) return std::string();
    std::string f = cls.substr(0, u);
    const std::string low = lowerAscii(f);
    for (const char* p : {"weapon", "armor", "item"}) {
        const std::size_t n = std::string(p).size();
        if (low.size() > n && low.compare(0, n, p) == 0) { f = f.substr(n); break; }
    }
    if (!f.empty() && f[0] >= 'a' && f[0] <= 'z') f[0] = char(f[0] - 'a' + 'A');
    return f;
}

// A word as a label: colour codes ({^X}, ^X) and a leading gender marker ([ms]) removed, blanks
// trimmed; "" when nothing is left or it is no valid label.
inline std::string cleanWord(const std::string& text) {
    std::string o;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '{' && i + 3 < text.size() && text[i + 1] == '^' && text[i + 3] == '}') { i += 3; continue; }
        if (text[i] == '^' && i + 1 < text.size()) { ++i; continue; }
        const unsigned char c = (unsigned char)text[i];
        o += (c < 0x20 || c == 0x7F) ? ' ' : char(c);
    }
    std::size_t a = 0, b = o.size();
    while (a < b && o[a] == ' ') ++a;
    while (b > a && o[b - 1] == ' ') --b;
    o = o.substr(a, b - a);
    if (o.size() > 2 && o[0] == '[') {
        const std::size_t close = o.find(']');
        bool marker = close != std::string::npos && close >= 2 && close <= 4;
        for (std::size_t k = 1; marker && k < close; ++k)
            marker = (o[k] >= 'a' && o[k] <= 'z') || (o[k] >= 'A' && o[k] <= 'Z');
        if (marker) {
            o = o.substr(close + 1);
            while (!o.empty() && o[0] == ' ') o.erase(0, 1);
        }
    }
    if (o.size() > kLabelMax) o.resize(kLabelMax);
    while (!o.empty() && o.back() == ' ') o.pop_back();
    return validLabel(o) ? o : std::string();
}

// The plural of an English word (its last word): -ff -> -ves, -s/-x/-ch/-sh -> +es, a consonant
// before a final y -> -ies, any other final letter -> +s; anything else is left as it is.
inline std::string pluralWord(const std::string& w) {
    auto low = [](char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; };
    auto letter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    const std::size_t n = w.size();
    if (n == 0 || !letter(w[n - 1])) return w;
    const char z = low(w[n - 1]), y = n >= 2 ? low(w[n - 2]) : ' ';
    const bool yVowel = y == 'a' || y == 'e' || y == 'i' || y == 'o' || y == 'u';
    std::string p;
    if (z == 'f' && y == 'f') p = w.substr(0, n - 2) + "ves";
    else if (z == 's' || z == 'x' || (z == 'h' && (y == 'c' || y == 's'))) p = w + "es";
    else if (z == 'y' && n >= 2 && letter(w[n - 2]) && !yVowel) p = w.substr(0, n - 1) + "ies";
    else p = w + "s";
    return p.size() > kLabelMax ? w : p;
}

// The tag candidates for a word, in order: the first three letters or digits; the first letter
// and the next two consonants (when there are two); the first two characters and 0-9, A-Z.
inline std::vector<std::string> tagCandidates(const std::string& word) {
    std::vector<std::string> c;
    const std::string t1 = tagFrom(word);
    c.push_back(t1);
    std::string letters;
    for (unsigned char ch : word) {
        if (ch >= 'a' && ch <= 'z') letters += char(ch - 'a' + 'A');
        else if (ch >= 'A' && ch <= 'Z') letters += char(ch);
    }
    if (!letters.empty()) {
        std::string t2(1, letters[0]);
        for (std::size_t i = 1; i < letters.size() && t2.size() < 3; ++i) {
            const char ch = letters[i];
            if (ch != 'A' && ch != 'E' && ch != 'I' && ch != 'O' && ch != 'U') t2 += ch;
        }
        if (t2.size() == 3) c.push_back(t2);
    }
    static const char kAlt[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    for (std::size_t k = 0; k + 1 < sizeof kAlt; ++k) c.push_back(t1.substr(0, 2) + kAlt[k]);
    return c;
}
inline bool hasAsciiAlnum(const std::string& s) {
    for (unsigned char c : s)
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return true;
    return false;
}

// ---------------------------------------------------------------- the parsed file
enum : unsigned {
    kFlagSet = 1, kFlagRelic = 2, kFlagEquipment = 4, kFlagFaction = 8, kFlagBlueprint = 16,
    kFlagAugment = 32, kFlagExtra = 64,
};
enum Source { kSourceAny = 0, kSourceBase = 1, kSourceMod = 2 };

struct GroupDef {
    std::string id, label, tag;
    int line = 0;                       // the declaring block's line; 0 = generated
    bool labelSet = false, tagSet = false;          // the file said label = / tag =
    bool labelDerived = false, tagDerived = false;  // neither the file nor the built-in table
                                                    // names it: resolveNames derives it
    std::string cls;                    // the first class a block of the group names (lower case)
};

struct Rule {
    bool exclude = false;
    int group = -1;                     // index into GroupSet::groups (a group block)
    std::string view;                   // "" = every view
    std::vector<std::string> classes;   // lower-cased
    std::vector<std::string> paths;     // lower-cased globs
    std::vector<std::string> ranks;     // lower-cased
    unsigned flags = 0;                 // any of these (OR); 0 = no flag filter
    int source = kSourceAny;
    int line = 0;
};

struct GroupSet {
    std::vector<GroupDef> groups;       // declaration order
    std::vector<Rule> rules;            // file order
    bool autoEnabled = true;
    int autoMin = 5;
    bool autoEnabledSet = false, autoMinSet = false;   // the file gave the key
    // The view's word per class (lower-cased class -> the English text of classTypeTag(class));
    // a class without one uses its last part. Data, filled by the generator (classWords).
    std::map<std::string, std::string> words;
};

// What the matcher reads of one candidate record.
struct Item {
    std::string record;                 // the record path, any case, '/' or '\'
    std::string cls;                    // Class
    std::string rank;                   // itemClassification
    unsigned flags = 0;
    bool fromMod = false;               // only a mod layer has the record
    bool knownClass = true;             // the generator's class table has the class
};

// ---------------------------------------------------------------- the glob
// '*' any run without '/', '**' any run (and "**/" also matches no segment), '?' one non-'/'.
inline bool globMatch(const char* p, const char* s) {
    while (*p) {
        if (p[0] == '*' && p[1] == '*') {
            p += 2;
            if (!*p) return true;
            if (*p == '/' && globMatch(p + 1, s)) return true;
            for (const char* t = s;; ++t) {
                if (globMatch(p, t)) return true;
                if (!*t) return false;
            }
        }
        if (*p == '*') {
            ++p;
            for (const char* t = s;; ++t) {
                if (globMatch(p, t)) return true;
                if (!*t || *t == '/') return false;
            }
        }
        if (!*s) return false;
        if (*p == '?') { if (*s == '/') return false; }
        else if (*p != *s) return false;
        ++p;
        ++s;
    }
    return !*s;
}
inline std::string normPath(const std::string& r) {
    std::string s = lowerAscii(r);
    for (char& c : s) if (c == '\\') c = '/';
    return s;
}

// ---------------------------------------------------------------- the parser
namespace detail {

inline std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}
// A value up to an inline comment: ';' or '#' preceded by a blank.
inline std::string stripComment(const std::string& s) {
    for (std::size_t i = 1; i < s.size(); ++i)
        if ((s[i] == ';' || s[i] == '#') && (s[i - 1] == ' ' || s[i - 1] == '\t'))
            return s.substr(0, i);
    return s;
}
inline std::vector<std::string> splitList(const std::string& v) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while (at <= v.size()) {
        std::size_t c = v.find(',', at);
        if (c == std::string::npos) c = v.size();
        std::string part = trim(v.substr(at, c - at));
        if (!part.empty()) out.push_back(part);
        at = c + 1;
    }
    return out;
}
inline std::string lineErr(int line, const std::string& what) {
    return "line " + std::to_string(line) + ": " + what;
}

} // namespace detail

// Parses a whole file. False with "line N: <reason>" on the first error; `out` is then empty.
inline bool parseGroups(const std::string& text, GroupSet& out, std::string* error) {
    using namespace detail;
    out = GroupSet();
    std::size_t at = 0;
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB
        && (unsigned char)text[2] == 0xBF)
        at = 3;
    enum { kNone, kGroup, kExclude, kAuto } section = kNone;
    std::string view;
    Rule cur;
    bool inRule = false, ruleHasFilter = false;
    std::map<std::string, int> index;    // id -> groups[]
    std::set<std::string> blockKeys;
    auto fail = [&](int line, const std::string& what) {
        if (error) *error = lineErr(line, what);
        out = GroupSet();
        return false;
    };
    auto closeRule = [&]() -> bool {
        if (!inRule) return true;
        if (!ruleHasFilter)
            return fail(cur.line, std::string(cur.exclude ? "[exclude]" : "the [group] block")
                                  + " has no filter (class, path, rank, flag or source)");
        if (!cur.exclude && !cur.classes.empty()) {
            GroupDef& d = out.groups[std::size_t(cur.group)];
            if (d.cls.empty()) d.cls = cur.classes[0];
        }
        out.rules.push_back(cur);
        inRule = false;
        return true;
    };
    int lineNo = 0;
    while (at <= text.size()) {
        std::size_t nl = text.find('\n', at);
        if (nl == std::string::npos) nl = text.size();
        std::string raw = text.substr(at, nl - at);
        at = nl + 1;
        ++lineNo;
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        std::string line = trim(raw);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            line = trim(stripComment(line));
            if (line.back() != ']') return fail(lineNo, "a section header must end with ']'");
            if (!closeRule()) return false;
            std::string h = trim(line.substr(1, line.size() - 2));
            blockKeys.clear();
            if (h.compare(0, 5, "view:") == 0) {
                std::string k = trim(h.substr(5));
                if (k == "*") view.clear();
                else if (!validViewKey(k)) return fail(lineNo, "bad view key \"" + k + "\"");
                else view = k;
                section = kNone;
            } else if (h == "exclude") {
                section = kExclude;
                cur = Rule();
                cur.exclude = true;
                cur.view = view;
                cur.line = lineNo;
                inRule = true;
                ruleHasFilter = false;
            } else if (h == "auto") {
                if (!view.empty()) return fail(lineNo, "[auto] applies to every view; put it before any [view:...]");
                section = kAuto;
            } else if (h.compare(0, 6, "group ") == 0) {
                std::string id = trim(h.substr(6));
                if (!validId(id)) return fail(lineNo, "bad group id \"" + id + "\" (a-z 0-9 _ . -, 1..40)");
                if (reservedId(id)) return fail(lineNo, "the group id \"" + id + "\" is reserved");
                auto it = index.find(id);
                int gi;
                if (it == index.end()) {
                    GroupDef d;
                    d.id = id;
                    d.line = lineNo;
                    gi = int(out.groups.size());
                    out.groups.push_back(d);
                    index[id] = gi;
                } else {
                    gi = it->second;
                }
                section = kGroup;
                cur = Rule();
                cur.group = gi;
                cur.view = view;
                cur.line = lineNo;
                inRule = true;
                ruleHasFilter = false;
            } else {
                return fail(lineNo, "unknown section [" + h + "]");
            }
            continue;
        }
        std::size_t eq = line.find('=');
        if (eq == std::string::npos) return fail(lineNo, "expected key = value");
        std::string key = lowerAscii(trim(line.substr(0, eq)));
        std::string val = trim(stripComment(line.substr(eq + 1)));
        if (section == kNone) return fail(lineNo, "\"" + key + "\" is outside a [group], [exclude] or [auto] block");
        if (section == kAuto) {
            if (key == "min") {
                char* end = nullptr;
                long n = std::strtol(val.c_str(), &end, 10);
                if (val.empty() || *end || n < 1 || n > 1000) return fail(lineNo, "min must be 1..1000");
                out.autoMin = int(n);
                out.autoMinSet = true;
            } else if (key == "enabled") {
                std::string v = lowerAscii(val);
                if (v == "yes" || v == "1" || v == "true" || v == "on") out.autoEnabled = true;
                else if (v == "no" || v == "0" || v == "false" || v == "off") out.autoEnabled = false;
                else return fail(lineNo, "enabled must be yes or no");
                out.autoEnabledSet = true;
            } else {
                return fail(lineNo, "unknown key \"" + key + "\" in [auto]");
            }
            continue;
        }
        if (key == "label" || key == "tag") {
            if (section != kGroup) return fail(lineNo, key + " belongs to a [group] block");
            if (blockKeys.count(key)) return fail(lineNo, key + " given twice");
            blockKeys.insert(key);
            GroupDef& d = out.groups[std::size_t(cur.group)];
            std::string& slot = key == "label" ? d.label : d.tag;
            if (key == "label" && !validLabel(val)) return fail(lineNo, "a label is 1..48 bytes, no tab");
            if (key == "tag" && !validTag(val)) return fail(lineNo, "a tag is exactly 3 printable ASCII characters");
            if (key == "tag" && reservedTag(val)) return fail(lineNo, "the tag " + val + " is the pad's own");
            if (!slot.empty() && slot != val) return fail(lineNo, "group " + d.id + " already has the " + key + " \"" + slot + "\"");
            slot = val;
            (key == "label" ? d.labelSet : d.tagSet) = true;
            continue;
        }
        std::vector<std::string> vals = splitList(val);
        if (vals.empty()) return fail(lineNo, key + " has no value");
        if (key == "class") {
            for (const std::string& v : vals) cur.classes.push_back(lowerAscii(v));
        } else if (key == "path") {
            for (const std::string& v : vals) cur.paths.push_back(normPath(v));
        } else if (key == "rank") {
            for (const std::string& v : vals) {
                std::string r = lowerAscii(v);
                if (r == "any") { cur.ranks.clear(); cur.ranks.push_back("*"); break; }
                cur.ranks.push_back(r);
            }
        } else if (key == "flag") {
            for (const std::string& v : vals) {
                std::string f = lowerAscii(v);
                if (f == "set") cur.flags |= kFlagSet;
                else if (f == "relic") cur.flags |= kFlagRelic;
                else if (f == "equipment") cur.flags |= kFlagEquipment;
                else if (f == "faction") cur.flags |= kFlagFaction;
                else if (f == "blueprint") cur.flags |= kFlagBlueprint;
                else if (f == "augment") cur.flags |= kFlagAugment;
                else if (f == "extra") cur.flags |= kFlagExtra;
                else return fail(lineNo, "unknown flag \"" + v + "\"");
            }
        } else if (key == "source") {
            if (vals.size() != 1) return fail(lineNo, "source takes one value: base, mod or any");
            std::string s = lowerAscii(vals[0]);
            if (s == "base") cur.source = kSourceBase;
            else if (s == "mod") cur.source = kSourceMod;
            else if (s == "any") cur.source = kSourceAny;
            else return fail(lineNo, "source must be base, mod or any");
        } else {
            return fail(lineNo, "unknown key \"" + key + "\"");
        }
        ruleHasFilter = true;
    }
    if (!closeRule()) return false;
    // defaults for the label and the tag: the built-in words of the id, else derived after the
    // match (resolveNames); until then the id's last part and three letters of it stand in
    for (GroupDef& d : out.groups) {
        const BuiltinGroup* b = builtinById(d.id);
        if (d.label.empty()) {
            if (b) d.label = b->label;
            else {
                d.labelDerived = true;
                std::size_t dot = d.id.rfind('.');
                d.label = dot == std::string::npos ? d.id : d.id.substr(dot + 1);
                if (!d.label.empty() && d.label[0] >= 'a' && d.label[0] <= 'z')
                    d.label[0] = char(d.label[0] - 'a' + 'A');
            }
        }
        if (d.tag.empty()) {
            if (b) d.tag = b->tag;
            else { d.tagDerived = true; d.tag = tagFrom(d.label); }
        }
        if (reservedTag(d.tag)) return fail(d.line, "the tag " + d.tag + " is the pad's own");
    }
    return true;
}

// ---------------------------------------------------------------- the user's file over the built-in set
struct MergeStats {
    std::size_t builtin = 0;    // groups the built-in set declares
    std::size_t yours = 0;      // groups only the user's file declares
    std::size_t extended = 0;   // built-in groups the user's file adds blocks to
};

// The user's set IN FRONT of the built-in one: its new groups first (in its order), then the
// built-in groups in theirs (a built-in id the user's file names keeps its place, takes the
// user's label/tag when the file gave one, and the user's first class); every user rule, then
// every built-in rule; the user's [auto] keys over the built-in ones. No label merges groups.
inline void mergeGroupSets(const GroupSet& user, const GroupSet& base, GroupSet& out,
                           MergeStats* stats) {
    out = GroupSet();
    std::map<std::string, std::size_t> baseIdx, userIdx;
    for (std::size_t i = 0; i < base.groups.size(); ++i) baseIdx[base.groups[i].id] = i;
    for (std::size_t i = 0; i < user.groups.size(); ++i) userIdx[user.groups[i].id] = i;
    std::vector<int> userMap(user.groups.size(), -1), baseMap(base.groups.size(), -1);
    MergeStats st;
    st.builtin = base.groups.size();
    for (std::size_t i = 0; i < user.groups.size(); ++i) {
        if (baseIdx.count(user.groups[i].id)) continue;
        userMap[i] = int(out.groups.size());
        out.groups.push_back(user.groups[i]);
        ++st.yours;
    }
    for (std::size_t i = 0; i < base.groups.size(); ++i) {
        GroupDef d = base.groups[i];
        baseMap[i] = int(out.groups.size());
        auto u = userIdx.find(d.id);
        if (u != userIdx.end()) {
            const GroupDef& ud = user.groups[u->second];
            userMap[u->second] = baseMap[i];
            ++st.extended;
            if (ud.labelSet) { d.label = ud.label; d.labelSet = true; d.labelDerived = false; }
            if (ud.tagSet) { d.tag = ud.tag; d.tagSet = true; d.tagDerived = false; }
            if (!ud.cls.empty()) d.cls = ud.cls;
        }
        out.groups.push_back(d);
    }
    for (const Rule& r : user.rules) {
        Rule c = r;
        if (!c.exclude) c.group = userMap[std::size_t(c.group)];
        out.rules.push_back(c);
    }
    for (const Rule& r : base.rules) {
        Rule c = r;
        if (!c.exclude) c.group = baseMap[std::size_t(c.group)];
        out.rules.push_back(c);
    }
    out.autoEnabled = user.autoEnabledSet ? user.autoEnabled : base.autoEnabled;
    out.autoMin = user.autoMinSet ? user.autoMin : base.autoMin;
    out.autoEnabledSet = user.autoEnabledSet || base.autoEnabledSet;
    out.autoMinSet = user.autoMinSet || base.autoMinSet;
    out.words = base.words;
    for (const auto& kv : user.words) out.words[kv.first] = kv.second;
    if (stats) *stats = st;
}

// ---------------------------------------------------------------- views
// The keys a view answers to in [view:<key>]: its full name, the custom game's key (the part
// after "custom-") and "basemods" for a /basemods view. "vanilla" -> {vanilla};
// "basemods+custom-reignofterror" -> {basemods+custom-reignofterror, reignofterror, basemods}.
inline std::vector<std::string> viewKeysFor(const std::string& viewName) {
    std::vector<std::string> k;
    if (viewName.empty()) return k;
    k.push_back(viewName);
    std::size_t c = viewName.find("custom-");
    if (c != std::string::npos && c + 7 < viewName.size()) k.push_back(viewName.substr(c + 7));
    if (viewName.compare(0, 8, "basemods") == 0 && viewName != "basemods") k.push_back("basemods");
    return k;
}
inline bool viewActive(const std::string& ruleView, const std::vector<std::string>& keys) {
    if (ruleView.empty()) return true;
    for (const std::string& k : keys) if (k == ruleView) return true;
    return false;
}

// ---------------------------------------------------------------- the match
const int kExcluded = -1;   // an [exclude] block matched
const int kUnmatched = -2;  // no block matched

inline bool ruleMatches(const Rule& r, const Item& it, const std::string& lowCls,
                        const std::string& lowRank, const std::string& path) {
    if (!r.classes.empty()
        && std::find(r.classes.begin(), r.classes.end(), lowCls) == r.classes.end())
        return false;
    if (!r.ranks.empty() && r.ranks[0] != "*"
        && std::find(r.ranks.begin(), r.ranks.end(), lowRank) == r.ranks.end())
        return false;
    if (r.flags && !(r.flags & it.flags)) return false;
    if (r.source == kSourceBase && it.fromMod) return false;
    if (r.source == kSourceMod && !it.fromMod) return false;
    if (!r.paths.empty()) {
        bool any = false;
        for (const std::string& g : r.paths)
            if (globMatch(g.c_str(), path.c_str())) { any = true; break; }
        if (!any) return false;
    }
    return true;
}

// The first active block that matches: a group index, kExcluded or kUnmatched.
inline int matchItem(const GroupSet& gs, const Item& it, const std::vector<std::string>& viewKeys) {
    const std::string lowCls = lowerAscii(it.cls), lowRank = lowerAscii(it.rank);
    const std::string path = normPath(it.record);
    for (const Rule& r : gs.rules) {
        if (!viewActive(r.view, viewKeys)) continue;
        if (ruleMatches(r, it, lowCls, lowRank, path)) return r.exclude ? kExcluded : r.group;
    }
    return kUnmatched;
}

// An auto group's label and tag before resolveNames, from the class. A class with an engine type
// word (classTypeTag) is named from the view's word there; the classes without one that the
// database mods are known to add keep a word of their own; anything else is the class's last
// '_' part.
inline void autoNaming(const std::string& cls, std::string& label, std::string& tag) {
    struct K { const char* cls; const char* label; const char* tag; };
    static const K kKnown[] = {
        {"ItemEquipment", "Equipment", "EQP"},     {"ItemUsableSkill", "Usable Skills", "USK"},
        {"ItemRelic", "Components", "CMP"},        {"ItemEnchantment", "Augments", "AUG"},
        {"ItemArtifactFormula", "Blueprints", "BLU"},
    };
    for (const K& k : kKnown)
        if (lowerAscii(cls) == lowerAscii(k.cls)) { label = k.label; tag = k.tag; return; }
    std::size_t u = cls.rfind('_');
    label = u == std::string::npos ? cls : cls.substr(u + 1);
    if (label.empty()) label = cls.empty() ? std::string("Other") : cls;
    if (label.size() > kLabelMax) label.resize(kLabelMax);
    tag = tagFrom(label);
}
inline std::string autoId(const std::string& cls) {
    std::string id = "auto.";
    for (unsigned char c : lowerAscii(cls)) {
        if (id.size() >= kIdMax) break;
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
        id += ok ? char(c) : '-';
    }
    return id;
}

// The whole assignment for one view.
struct Assignment {
    std::vector<GroupDef> groups;       // display order; may hold groups that got no record
    std::vector<int> groupOf;           // per item: index into groups, or -1 (no page)
    std::vector<char> used;             // per group: it received at least one record
};

// The derived words of the groups that received a record (see the header), in group order. The
// fixed labels and tags of those groups are taken first, then each derived one claims the first
// free candidate; "other" keeps OTH unless taken (then the third character runs 0-9, A-Z). Groups that got no record keep what the parser gave them.
inline void resolveNames(const std::map<std::string, std::string>& words,
                         std::vector<GroupDef>& groups, const std::vector<char>& used) {
    auto special = [](const GroupDef& d) { return d.id == "other"; };
    std::set<std::string> tags, labels;
    for (std::size_t i = 0; i < groups.size(); ++i) {
        if (!used[i] || special(groups[i])) continue;
        if (!groups[i].tagDerived) tags.insert(groups[i].tag);
        if (!groups[i].labelDerived) labels.insert(lowerAscii(groups[i].label));
    }
    auto isFree = [&](const std::string& t) { return !tags.count(t) && !reservedTag(t); };
    auto cap = [](std::string w) {
        if (!w.empty() && w[0] >= 'a' && w[0] <= 'z') w[0] = char(w[0] - 'a' + 'A');
        return w;
    };
    for (std::size_t i = 0; i < groups.size(); ++i) {
        if (!used[i]) continue;
        GroupDef& d = groups[i];
        if (special(d)) {
            static const char kAlt[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
            std::string t = d.tag;
            for (std::size_t n = 0; !isFree(t) && n + 1 < sizeof kAlt; ++n) t = t.substr(0, 2) + kAlt[n];
            d.tag = t;
            tags.insert(t);
            continue;
        }
        std::string word;
        if (d.labelDerived) {
            if (!d.cls.empty() && classTypeTag(d.cls)) {
                auto w = words.find(lowerAscii(d.cls));
                if (w != words.end()) word = cleanWord(w->second);
                if (word.empty()) word = cleanWord(cap(classLastPart(d.cls)));
            }
            if (!word.empty()) {
                std::string label = pluralWord(word);
                if (labels.count(lowerAscii(label))) {
                    const std::string fam = cap(classFamilyWord(d.cls));
                    if (!fam.empty() && fam.size() + 1 + label.size() <= kLabelMax) label = fam + " " + label;
                    const std::string stem = label;
                    for (int n = 2; labels.count(lowerAscii(label)) && n < 100; ++n) {
                        const std::string sfx = " " + std::to_string(n);
                        label = stem.substr(0, std::min(stem.size(), kLabelMax - sfx.size())) + sfx;
                    }
                }
                d.label = label;
            }
            labels.insert(lowerAscii(d.label));
        }
        if (d.tagDerived) {
            std::vector<std::string> cand;
            std::string src = word.empty() ? d.label : word;
            if (word.empty()) cand.push_back(d.tag);       // the fallback the parser or autoNaming gave
            if (!hasAsciiAlnum(src)) {
                src = d.cls.empty() ? d.id.substr(d.id.rfind('.') == std::string::npos ? 0 : d.id.rfind('.') + 1)
                                    : classLastPart(d.cls);
            }
            for (const std::string& c : tagCandidates(src)) cand.push_back(c);
            for (const std::string& c : cand)
                if (isFree(c)) { d.tag = c; break; }
            tags.insert(d.tag);
        }
    }
}

// One row per group of the view that received a record, in group order: the words the buttons
// file is generated with (its defaults; the buttons file may put several groups under one button).
struct GroupRow { std::string id, label, tag; };
inline std::vector<GroupRow> viewGroupTable(const Assignment& a) {
    std::vector<GroupRow> rows;
    for (std::size_t i = 0; i < a.groups.size(); ++i)
        if (i < a.used.size() && a.used[i]) rows.push_back({a.groups[i].id, a.groups[i].label, a.groups[i].tag});
    return rows;
}

// The groups that end up empty stay in `groups`; the caller emits only the used ones.
inline void assignGroups(const GroupSet& gs, const std::vector<Item>& items,
                         const std::vector<std::string>& viewKeys, Assignment& out) {
    out = Assignment();
    out.groups = gs.groups;
    out.groupOf.assign(items.size(), -1);
    std::map<std::string, std::vector<std::size_t>> autoByClass;   // sorted by class
    for (std::size_t i = 0; i < items.size(); ++i) {
        int m = matchItem(gs, items[i], viewKeys);
        if (m >= 0) { out.groupOf[i] = m; continue; }
        if (m == kUnmatched && !items[i].knownClass && gs.autoEnabled)
            autoByClass[items[i].cls].push_back(i);
    }
    std::vector<std::size_t> other;
    for (const auto& kv : autoByClass) {
        if (int(kv.second.size()) < gs.autoMin) { other.insert(other.end(), kv.second.begin(), kv.second.end()); continue; }
        GroupDef d;
        d.id = autoId(kv.first);
        d.cls = lowerAscii(kv.first);
        autoNaming(kv.first, d.label, d.tag);
        d.labelDerived = d.tagDerived = true;
        int gi = int(out.groups.size());
        out.groups.push_back(d);
        for (std::size_t i : kv.second) out.groupOf[i] = gi;
    }
    if (!other.empty()) {
        GroupDef d;
        d.id = "other";
        d.label = "Other";
        d.tag = "OTH";
        int gi = int(out.groups.size());
        out.groups.push_back(d);
        std::sort(other.begin(), other.end());
        for (std::size_t i : other) out.groupOf[i] = gi;
    }
    out.used.assign(out.groups.size(), 0);
    for (int g : out.groupOf) if (g >= 0) out.used[std::size_t(g)] = 1;
    resolveNames(gs.words, out.groups, out.used);
}

// ---------------------------------------------------------------- the journal count scan
// uniq-items[-hc].jsonl (src\ut_rescue.cpp holds the format): line 1 is the header object with
// "journal":"grim dawn uniquetab"; every other line is one flat object whose "record" is the key
// (lower-cased; the last line of a record wins) and whose "count" is the number of copies the
// private table holds (absent = 0). This reads only those two keys and never writes.
namespace detail {

inline void skipWs(const std::string& s, std::size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r')) ++i;
}
inline bool readJsonString(const std::string& s, std::size_t& i, std::string& out) {
    if (i >= s.size() || s[i] != '"') return false;
    ++i;
    out.clear();
    while (i < s.size()) {
        char c = s[i++];
        if (c == '"') return true;
        if (c != '\\') { out += c; continue; }
        if (i >= s.size()) return false;
        char e = s[i++];
        switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
            if (i + 4 > s.size()) return false;
            unsigned v = 0;
            for (int k = 0; k < 4; ++k) {
                char h = s[i++];
                v <<= 4;
                if (h >= '0' && h <= '9') v |= unsigned(h - '0');
                else if (h >= 'a' && h <= 'f') v |= unsigned(h - 'a' + 10);
                else if (h >= 'A' && h <= 'F') v |= unsigned(h - 'A' + 10);
                else return false;
            }
            if (v < 0x80) out += char(v);
            else if (v < 0x800) { out += char(0xC0 | (v >> 6)); out += char(0x80 | (v & 0x3F)); }
            else { out += char(0xE0 | (v >> 12)); out += char(0x80 | ((v >> 6) & 0x3F)); out += char(0x80 | (v & 0x3F)); }
            break;
        }
        default: return false;
        }
    }
    return false;
}
// One flat object -> its string and number values (true/false/null are skipped).
inline bool readFlatObject(const std::string& s, std::map<std::string, std::string>& kv) {
    kv.clear();
    std::size_t i = 0;
    skipWs(s, i);
    if (i >= s.size() || s[i] != '{') return false;
    ++i;
    skipWs(s, i);
    if (i < s.size() && s[i] == '}') return true;
    while (i < s.size()) {
        std::string k, v;
        skipWs(s, i);
        if (!readJsonString(s, i, k)) return false;
        skipWs(s, i);
        if (i >= s.size() || s[i] != ':') return false;
        ++i;
        skipWs(s, i);
        if (i < s.size() && s[i] == '"') {
            if (!readJsonString(s, i, v)) return false;
        } else {
            std::size_t a = i;
            while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ' ' && s[i] != '\t') ++i;
            v = s.substr(a, i - a);
            if (v.empty() || v[0] == '{' || v[0] == '[') return false;
        }
        kv[k] = v;
        skipWs(s, i);
        if (i < s.size() && s[i] == ',') { ++i; continue; }
        if (i < s.size() && s[i] == '}') return true;
        return false;
    }
    return false;
}

} // namespace detail

struct JournalCounts {
    std::unordered_map<std::string, unsigned> counts;   // lower-cased record -> count (last wins)
    std::size_t rows = 0, badRows = 0;
};

// False (with the reason) when the text is not a journal: no header line first.
// A row that does not parse is counted in badRows and skipped; it never ends the scan.
inline bool scanJournalText(const std::string& text, JournalCounts& out, std::string* error) {
    out = JournalCounts();
    std::size_t at = 0;
    bool header = false;
    std::map<std::string, std::string> kv;
    while (at < text.size()) {
        std::size_t nl = text.find('\n', at);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(at, nl - at);
        at = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (detail::trim(line).empty()) continue;
        if (!header) {
            if (!detail::readFlatObject(line, kv) || kv["journal"] != "grim dawn uniquetab") {
                if (error) *error = "the first line is not a uniquetab journal header";
                return false;
            }
            header = true;
            continue;
        }
        ++out.rows;
        if (!detail::readFlatObject(line, kv)) { ++out.badRows; continue; }
        auto r = kv.find("record");
        if (r == kv.end() || r->second.empty()) { ++out.badRows; continue; }
        unsigned n = 0;
        auto c = kv.find("count");
        if (c != kv.end()) {
            char* end = nullptr;
            unsigned long v = std::strtoul(c->second.c_str(), &end, 10);
            if (c->second.empty() || *end || v > 0x10000ul) { ++out.badRows; continue; }
            n = unsigned(v);
        }
        out.counts[normPath(r->second)] = n;
    }
    if (!header) {
        if (error) *error = "the file is empty";
        return false;
    }
    return true;
}

enum class JournalRead { Absent, Read, Unreadable };

// Reads one journal file read-only. Absent: no file. Unreadable: there and not usable - the
// caller must then not shrink anything on its account ("cannot read" is never "empty").
inline JournalRead scanJournalFile(const std::string& path, JournalCounts& out, std::string* error) {
    out = JournalCounts();
    errno = 0;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        if (errno == ENOENT) return JournalRead::Absent;
        if (error) *error = "cannot open " + path;
        return JournalRead::Unreadable;
    }
    std::string text;
    char buf[65536];
    std::size_t n;
    const std::size_t cap = std::size_t(512) << 20;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
        text.append(buf, n);
        if (text.size() > cap) { std::fclose(f); if (error) *error = path + " is over 512 MB"; return JournalRead::Unreadable; }
    }
    bool bad = std::ferror(f) != 0;
    std::fclose(f);
    if (bad) { if (error) *error = "cannot read " + path; return JournalRead::Unreadable; }
    std::string why;
    if (!scanJournalText(text, out, &why)) {
        if (error) *error = path + ": " + why;
        return JournalRead::Unreadable;
    }
    return JournalRead::Read;
}

// The records any of the scans counts at least one copy of.
inline void ownedFrom(const std::vector<JournalCounts>& scans, std::unordered_set<std::string>& owned) {
    for (const JournalCounts& j : scans)
        for (const auto& kv : j.counts) if (kv.second >= 1) owned.insert(kv.first);
}

// The owned records of a view's journal files (softcore and -hc: one catalogue serves both).
// A missing file adds nothing. False when any file is there and cannot be read: `owned` then
// holds what the readable ones gave and `why` names the file. Read for the generator's report
// only (the owned records no page shows); nothing generated depends on it.
inline bool ownedFromJournals(const std::vector<std::string>& paths,
                              std::unordered_set<std::string>& owned, std::string* why) {
    bool all = true;
    for (const std::string& p : paths) {
        JournalCounts jc;
        std::string err;
        JournalRead r = scanJournalFile(p, jc, &err);
        if (r == JournalRead::Unreadable) {
            if (all && why) *why = err;
            all = false;
            continue;
        }
        if (r == JournalRead::Read) ownedFrom({jc}, owned);
    }
    return all;
}

// ---------------------------------------------------------------- the G line
// 1.1.0:  G <n> <label> <cols> <rows> <cellW> <cellH> <count>               (7 fields after G)
// 1.2.0:  G <n> <label> <cols> <rows> <cellW> <cellH> <count> <id> <tag>    (9 fields after G)
// A reader takes both. A 7-field line gets its id and tag from the built-in table by its label
// (by its position when the label is not a built-in one).
struct GLine {
    int index = 0;
    std::string label;
    int cols = 0, rows = 0, cellW = 0, cellH = 0, count = 0;
    std::string id, tag;
    bool nineFields = false;
};

inline std::string formatGLine(const GLine& g) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "\t%d\t%d\t%d\t%d\t%d", g.cols, g.rows, g.cellW, g.cellH, g.count);
    return "G\t" + std::to_string(g.index) + "\t" + g.label + buf + "\t" + g.id + "\t" + g.tag;
}

// line: one G line without its line ending. False when it is not a G line of either form.
inline bool parseGLine(const std::string& line, GLine& g) {
    g = GLine();
    std::vector<std::string> f;
    std::size_t at = 0;
    while (true) {
        std::size_t t = line.find('\t', at);
        f.push_back(line.substr(at, t == std::string::npos ? std::string::npos : t - at));
        if (t == std::string::npos) break;
        at = t + 1;
    }
    if (f.empty() || f[0] != "G" || (f.size() != 8 && f.size() != 10)) return false;
    int v[6];
    const int pos[6] = {1, 3, 4, 5, 6, 7};
    for (int k = 0; k < 6; ++k) {
        const std::string& s = f[std::size_t(pos[k])];
        char* end = nullptr;
        long n = std::strtol(s.c_str(), &end, 10);
        if (s.empty() || *end || n < 0 || n > 1000000) return false;
        v[k] = int(n);
    }
    g.index = v[0];
    g.label = f[2];
    g.cols = v[1]; g.rows = v[2]; g.cellW = v[3]; g.cellH = v[4]; g.count = v[5];
    if (f.size() == 10) {
        if (!validId(f[8]) || !validTag(f[9])) return false;
        g.id = f[8];
        g.tag = f[9];
        g.nineFields = true;
        return true;
    }
    const BuiltinGroup* b = builtinByLabel(g.label);
    if (!b) {
        std::size_t n = 0;
        const BuiltinGroup* t = builtinGroups(&n);
        if (std::size_t(g.index) < n) b = &t[g.index];
    }
    if (b) { g.id = b->id; g.tag = b->tag; }
    else { g.id = "group" + std::to_string(g.index); g.tag = tagFrom(g.label); }
    return true;
}

} // namespace utg
