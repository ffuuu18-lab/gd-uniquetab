// ut_groups_default.h - the built-in groups (compiled in, never written to disk), the optional
// user file that ADDS to them (loadGroupsFile -> mergeGroupSets), and the retirement of a file
// that is an unedited copy of a template an earlier build wrote (renamed .old, one INFO line).
// Its slot groups are exactly the 24 the generator has always written, in the same order with
// the same labels, then the blueprints (every ItemArtifactFormula, base and mod, Crate's test
// blueprints excluded); the two per-class blocks (staff, spear1h) are empty in vanilla, so nothing
// is emitted for them there; the [view:...] blocks only apply inside those custom games. The last
// section, [view:collect_mi], adds a Rare rule to DeathGrimDawn's Bows & Crossbows (its crossbow
// folders, first, so its MI crossbows do not reach the gun rules), to each of the 23 slot groups
// and to the two per-class groups (staff, spear1h); generate.cpp makes
// every view answer to that key only while collect_mi=1, so with the flag off it is inert and no
// monster infrequent is on a page (a stored one keeps its journal row, hidden until the flag is 1). The per-game [exclude] blocks (rank = Rare,
// flag = equipment) keep a mod's MI-classified junk gear (NPC gear, cosmetics, replicas) off the
// pages; a Rare blueprint on the same path (Grimarillion's 152 replica recipes) stays where it was.
//
// tools/build_uniq_db.py reads the text between the raw-string markers below, so this header is
// the one source of the template for both implementations.
#pragma once

#include "gen/ut_groups.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <string>

namespace utg {

inline const char* defaultGroupsIni() {
    return R"UTG(; The built-in groups of the Unique Collection tab. Compiled into the mod; never written.
;
; Your own uniquetab-groups.ini beside uniquetab.ini is optional and ADDS to these: its blocks
; are tried first, so a block of yours takes its records off their usual page, and its [exclude]
; keeps records off every page; a block with a built-in id adds rules to that group (its label or
; tag replaces the built-in one); its [auto] applies last. A file with an error is ignored as a
; whole (the log names the file and the line). README.md, "Your own groups".
;
; [group <id>]   a block of rules feeding one group. The id (a-z 0-9 _ . -, up to 40) is how
;                other files name the group; the first block of an id places the group.
;   label  = the group's name              tag    = the 3-character button caption
;   class  = item classes                  path   = record path patterns: * is one folder
;   rank   = Legendary, Epic, Rare, any             level or part of one, ** any depth
;   flag   = set, relic, equipment, faction, blueprint, augment, extra
;   source = base (the base game has the record) or mod (only the mod has it)
;   Values in one key are alternatives; every key of a block must match.
; [exclude]      a block of rules whose records stay off every page.
; [view:<key>]   the blocks after it apply only in that view: vanilla, basemods, or a custom
;                game's folder name in lower case (reignofterror); [view:*] ends it. While
;                collect_mi=1 (uniquetab.ini) every view also answers to [view:collect_mi].
; [auto]         a class no block names and the mod does not know gets a group of its own
;                when it has at least `min` records; smaller ones share the group "Other".
;
; Every record is tried against the blocks from the top; the first block that matches wins.
; An item you own that no group takes is kept in the collection but not shown; the label ends
; in "+N not on these pages" while there are such items, and they are back once a group takes them.
; A group with no label is named with the game's own word for its class in that game, plural
; (Reign of Terror calls the staff class "Bow": "Bows"); a tag is derived from that word.

[auto]
enabled = yes
min     = 5

; ---- Family groups, as examples. Copy a block and the [view:...] above it, without the ';',
; ---- into your own uniquetab-groups.ini to collect that family on a page of its own: your
; ---- blocks are tried first, so a family takes its records before they reach their slot.
; [view:reignofterror]
; [group reignofterror.runewords]
; label = Runewords
; tag   = RWD
; path  = records/0_rot/items/runeword/**
; [group reignofterror.charms]
; label = Charms
; tag   = CHM
; path  = records/0_rot/items/jewelery/charm/**
; [group reignofterror.titanquest]
; label = Titan Quest
; tag   = TQU
; path  = records/xpack/item/**
; [group reignofterror.diablo2]
; label = Diablo II
; tag   = D2U
; path  = records/items/d2items/**
; [group reignofterror.diablo1]
; label = Diablo
; tag   = D1U
; path  = records/items/d1items/**
; [view:deathgrimdawn]
; [group deathgrimdawn.diablo]
; label = Diablo
; tag   = DIA
; path  = records/items/diabloitems/**, records/items/diablosets/**
; [group deathgrimdawn.skillrunes]
; label = Skill Runes
; tag   = SKR
; class = ItemRelic
; path  = records/reskill/components/**, records/bladevortex/skillrunes/**
; [view:*]

; ---- Crate's own test blueprints never drop ("Blueprint: Item Name of the Awesomesauce" and a
; ---- second copy of the Elixir of the Void recipe, beside the blueprint folders).
[exclude]
class = ItemArtifactFormula
path  = records/items/crafting/blueprints/blueprint_*test*

; ---- What a mod keeps for its own machinery stays off the pages (remove a block to collect it).
[view:deathgrimdawn]
[exclude]
path  = records/shatteredaffixes/items/crafting/**

; ---- The Rare gear a mod keeps for its own machinery, as cosmetics or as copies (only
; ---- collect_mi=1 admits a Rare equipment record; no unique, relic or blueprint is touched).
[view:reignofterror]
[exclude]
rank  = Rare
flag  = equipment
path  = records/0_rot/items/armor/npc/**, records/0_rot/fluffy/**
[view:deathgrimdawn]
[exclude]
rank  = Rare
flag  = equipment
path  = records/pogd/cosmetics/**, records/playtest/**, records/skills/**, records/items/f001a_sword.dbr
[view:grimarmory]
[exclude]
rank  = Rare
flag  = equipment
path  = records/items/crafting/crafting_x*
[view:grimarillion]
[exclude]
rank  = Rare
flag  = equipment
path  = records/skills/playerclasstempest/**, records/items/replicas/**
[view:dom]
[exclude]
rank  = Rare
flag  = equipment
path  = records/cosmetics/items/wings/**, records/tq/monster/creatures/**, records/cat/skills/**

; ---- The slot groups, in every view.
[view:*]
[group head]
label = Helms
tag   = HEL
class = ArmorProtective_Head
rank  = Epic, Legendary
[group shoulders]
label = Shoulders
tag   = SHO
class = ArmorProtective_Shoulders
rank  = Epic, Legendary
[group chest]
label = Chest
tag   = CHE
class = ArmorProtective_Chest
rank  = Epic, Legendary
[group hands]
label = Gloves
tag   = GLO
class = ArmorProtective_Hands
rank  = Epic, Legendary
[group waist]
label = Belts
tag   = BEL
class = ArmorProtective_Waist
rank  = Epic, Legendary
[group legs]
label = Pants
tag   = PAN
class = ArmorProtective_Legs
rank  = Epic, Legendary
[group feet]
label = Boots
tag   = BOO
class = ArmorProtective_Feet
rank  = Epic, Legendary
[group amulet]
label = Amulets
tag   = AMU
class = ArmorJewelry_Amulet
rank  = Epic, Legendary
[group medal]
label = Medals
tag   = MED
class = ArmorJewelry_Medal
rank  = Epic, Legendary
[group ring]
label = Rings
tag   = RNG
class = ArmorJewelry_Ring
rank  = Epic, Legendary
[group ring]          ; the curated extras (Leovinus' Ring) at any rank
class = ArmorJewelry_Ring
flag  = extra
[group offhand]
label = Off-hands
tag   = OFF
class = WeaponArmor_Offhand
rank  = Epic, Legendary
[group shield]
label = Shields
tag   = SHD
class = WeaponArmor_Shield
rank  = Epic, Legendary
[group axe1h]
label = 1H Axes
tag   = AX1
class = WeaponMelee_Axe
rank  = Epic, Legendary
[group dagger]
label = Daggers
tag   = DAG
class = WeaponMelee_Dagger
rank  = Epic, Legendary
[group mace1h]
label = 1H Maces
tag   = MC1
class = WeaponMelee_Mace
rank  = Epic, Legendary
[group scepter]
label = Scepters
tag   = SCP
class = WeaponMelee_Scepter
rank  = Epic, Legendary
[group sword1h]
label = 1H Swords
tag   = SW1
class = WeaponMelee_Sword
rank  = Epic, Legendary
[view:deathgrimdawn]
[group deathgrimdawn.bows]
label = Bows & Crossbows
tag   = BOW
path  = records/items/diabloitems/crossbows/**, records/items/diablosets/*crossbow*
rank  = Epic, Legendary
[view:*]
[group ranged1h]
label = Guns
tag   = GUN
class = WeaponHunting_Ranged1h
rank  = Epic, Legendary
[group axe2h]
label = 2H Axes
tag   = AX2
class = WeaponMelee_Axe2h
rank  = Epic, Legendary
[group mace2h]
label = 2H Maces
tag   = MC2
class = WeaponMelee_Mace2h
rank  = Epic, Legendary
[group spear2h]
label = Spears
tag   = SPR
class = WeaponMelee_Spear2h
rank  = Epic, Legendary
[group sword2h]
label = 2H Swords
tag   = SW2
class = WeaponMelee_Sword2h
rank  = Epic, Legendary
[group ranged2h]
label = 2H Guns
tag   = GN2
class = WeaponHunting_Ranged2h
rank  = Epic, Legendary
; ---- The classes the base game has no uniques of: one group per class, named by the game's
; ---- word for the class in that game (Reign of Terror: Bows and Hunting Spears; Grimarillion:
; ---- Staves and Hunting Spears). No label or tag here: both are derived.
[group staff]
class = WeaponMagical_Staff
rank  = Epic, Legendary
[group spear1h]
class = WeaponHunting_Spear
rank  = Epic, Legendary
[group relic]
label = Relics
tag   = REL
class = ItemArtifact
rank  = any
; ---- The blueprints the blacksmith teaches, at every rarity, a mod's too: one scroll per record.
[group blueprint]
label = Blueprints
tag   = BLU
class = ItemArtifactFormula
rank  = any
[view:reignofterror]
[group reignofterror.jewels]
label = Jewels, Gems & Runes
tag   = JWL
class = ItemRelic
rank  = Legendary
source = mod
[group reignofterror.jewels]
class = ItemRelic
path  = records/0_rot/items/socket/**, records/items/d2items/materia/gems/**
source = mod
[view:deathgrimdawn]
[group deathgrimdawn.gems]
label = Gems & Runes
tag   = GEM
class = ItemRelic
path  = records/items/diabloitems/gems/**, records/items/diabloitems/runes/**
source = mod
[group deathgrimdawn.augments]
label = Augments
tag   = AUG
class = ItemEnchantment
path  = records/items/remaugments/**
[view:*]
; ---- Monster infrequents (collect_mi=1 in uniquetab.ini, the default): every Rare item record of a
; ---- slot - the game's MI symbol, faction-store and blacksmith-crafted rares included - on that
; ---- slot's page after its uniques (Rare ranks after Epic), one box per record; a mod's Rare
; ---- staves and hunting spears on their class pages likewise. With collect_mi=0 these blocks are
; ---- off: an MI you stored earlier stays in the collection, hidden until collect_mi=1 again.
[view:collect_mi]
; DeathGrimDawn's MI crossbows (its own folders) join its Bows & Crossbows, before the gun rules
[group deathgrimdawn.bows]
path  = records/items/diabloitems/crossbows/**, records/items/diablosets/*crossbow*
rank  = Rare
[group head]
class = ArmorProtective_Head
rank  = Rare
[group shoulders]
class = ArmorProtective_Shoulders
rank  = Rare
[group chest]
class = ArmorProtective_Chest
rank  = Rare
[group hands]
class = ArmorProtective_Hands
rank  = Rare
[group waist]
class = ArmorProtective_Waist
rank  = Rare
[group legs]
class = ArmorProtective_Legs
rank  = Rare
[group feet]
class = ArmorProtective_Feet
rank  = Rare
[group amulet]
class = ArmorJewelry_Amulet
rank  = Rare
[group medal]
class = ArmorJewelry_Medal
rank  = Rare
[group ring]
class = ArmorJewelry_Ring
rank  = Rare
[group offhand]
class = WeaponArmor_Offhand
rank  = Rare
[group shield]
class = WeaponArmor_Shield
rank  = Rare
[group axe1h]
class = WeaponMelee_Axe
rank  = Rare
[group dagger]
class = WeaponMelee_Dagger
rank  = Rare
[group mace1h]
class = WeaponMelee_Mace
rank  = Rare
[group scepter]
class = WeaponMelee_Scepter
rank  = Rare
[group sword1h]
class = WeaponMelee_Sword
rank  = Rare
[group ranged1h]
class = WeaponHunting_Ranged1h
rank  = Rare
[group axe2h]
class = WeaponMelee_Axe2h
rank  = Rare
[group mace2h]
class = WeaponMelee_Mace2h
rank  = Rare
[group spear2h]
class = WeaponMelee_Spear2h
rank  = Rare
[group sword2h]
class = WeaponMelee_Sword2h
rank  = Rare
[group ranged2h]
class = WeaponHunting_Ranged2h
rank  = Rare
[group staff]
class = WeaponMagical_Staff
rank  = Rare
[group spear1h]
class = WeaponHunting_Spear
rank  = Rare
[view:*]
)UTG";
}

// ---------------------------------------------------------------- the stock copies
// FNV-1a 64 of a text, the stamp's hash family.
inline std::uint64_t groupsTextHash(const std::string& s) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned char c : s) { h ^= c; h *= 0x100000001b3ull; }
    return h;
}
inline std::string foldCrlf(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i)
        if (!(s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n')) o += s[i];
    return o;
}
// True when the bytes are, CRLF folded, one of the templates an earlier build wrote as
// uniquetab-groups.ini (every earlier version of defaultGroupsIni(): the LF size and the FNV-1a 64
// of the LF text) or this build's own template.
inline bool isStockGroupsText(const std::string& bytes) {
    struct S { std::size_t size; std::uint64_t hash; };
    static const S kStock[] = {
        {5626, 0x5ABB060AC91B91B8ull},   // earlier template 1: the first built-in set
        {5652, 0xA10A4879978FBB18ull},   // earlier template 2: the view groups before the slots
        {5833, 0xB54E243C5052E1E4ull},   // earlier template 3 (6,053 B with CRLF)
        {6523, 0x22ABDC9408D83BCEull},   // earlier template 4: bows and D2 gems collectible
    };
    const std::string lf = foldCrlf(bytes);
    const std::uint64_t h = groupsTextHash(lf);
    for (const S& k : kStock)
        if (lf.size() == k.size && h == k.hash) return true;
    return lf == defaultGroupsIni();
}

// The built-in set, parsed once. Null (with the reason) only if the template is broken.
inline const GroupSet* builtinGroupSet(std::string* why) {
    struct Parsed { GroupSet set; std::string why; bool ok = false; };
    static const Parsed p = [] {
        Parsed r;
        r.ok = parseGroups(defaultGroupsIni(), r.set, &r.why);
        return r;
    }();
    if (!p.ok) {
        if (why) *why = "the built-in template: " + p.why;
        return nullptr;
    }
    return &p.set;
}

enum class GroupsFile {
    Default,    // no user file: the built-in set
    Loaded,     // the user's file parsed and was merged in front of the built-in set
    Refused,    // a user file that cannot be read, renamed or parsed: the built-in set alone
    Retired,    // an unedited copy of an old template, renamed .old: the built-in set
};

struct GroupsLoad {
    GroupsFile from = GroupsFile::Default;
    std::string bytes;        // Loaded: the user's file as read, for the stamp
    std::string why;          // Refused: the reason, naming the file (and the line)
    std::string renamedTo;    // Retired: the .old name it has now
    MergeStats stats;         // Loaded: what the merge counted
};

// The rule set a generation uses. `out` is always usable: the built-in set, or the user's file
// merged in front of it. A file at `path` that is byte-identical (CRLF folded) to a template an
// earlier build wrote is renamed to <path>.old (.old1 ... .old9 when taken) and not read.
inline GroupsFile loadGroupsFile(const std::string& path, GroupSet& out, GroupsLoad* info) {
    GroupsLoad local;
    GroupsLoad& r = info ? *info : local;
    r = GroupsLoad();
    std::string err;
    const GroupSet* base = builtinGroupSet(&err);
    out = base ? *base : GroupSet();
    auto refuse = [&](const std::string& why) {
        r.from = GroupsFile::Refused;
        r.why = base ? why : err;
        return r.from;
    };
    if (!base) return refuse(err);
    r.stats.builtin = base->groups.size();
    if (path.empty()) return r.from;
    errno = 0;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        if (errno == ENOENT) return r.from;
        return refuse("cannot open " + path);
    }
    std::string text;
    char buf[16384];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && text.size() <= (std::size_t(1) << 20))
        text.append(buf, n);
    const bool bad = std::ferror(f) != 0;
    std::fclose(f);
    if (bad || text.size() > (std::size_t(1) << 20))
        return refuse(bad ? "cannot read " + path : path + " is over 1 MB");
    if (isStockGroupsText(text)) {
        for (int k = 0; k < 10; ++k) {
            const std::string to = path + ".old" + (k ? std::to_string(k) : std::string());
            if (std::FILE* t = std::fopen(to.c_str(), "rb")) { std::fclose(t); continue; }
            if (std::rename(path.c_str(), to.c_str()) == 0) {
                r.from = GroupsFile::Retired;
                r.renamedTo = to;
                return r.from;
            }
            break;
        }
        return refuse(path + " is an unedited copy of an old built-in template and could not be "
                      "renamed to .old - it is not read");
    }
    GroupSet user;
    if (!parseGroups(text, user, &err)) return refuse(path + ": " + err);
    GroupSet merged;
    mergeGroupSets(user, *base, merged, &r.stats);
    out = merged;
    r.bytes = text;
    r.from = GroupsFile::Loaded;
    return r.from;
}

} // namespace utg
