// test_groups.cpp - the groups as data, offline: the uniquetab-groups.ini parser, the matcher,
// the auto groups, no Retired group, the journal count scan, the G line, the user's file merged
// in front of the built-in set, the class words (plural, family prefix, tag ladder) over the six
// views' words, and the retirement of a stock template copy (.old). Links the REAL headers the
// generator compiles (src\gen\ut_groups.h and src\gen\ut_groups_default.h). No game, no
// Windows API.
//
//   test_groups.exe <data\oracle\uniq-groups.txt> <scratch dir> [<folder of old templates>]
#include "gen/ut_groups.h"
#include "gen/ut_groups_default.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

int g_fail = 0, g_pass = 0;

void check(bool ok, const std::string& what) {
    if (ok) { ++g_pass; return; }
    ++g_fail;
    std::printf("[test] FAIL: %s\n", what.c_str());
}

utg::GroupSet mustParse(const std::string& text, const char* what) {
    utg::GroupSet gs;
    std::string err;
    bool ok = utg::parseGroups(text, gs, &err);
    check(ok, std::string(what) + " parses (" + err + ")");
    return gs;
}

// The id of the group a record lands in, "-" for no page.
std::string groupIdOf(const utg::GroupSet& gs, const utg::Item& it, const std::vector<std::string>& keys) {
    std::vector<utg::Item> one{it};
    utg::Assignment a;
    utg::assignGroups(gs, one, keys, a);
    return a.groupOf[0] < 0 ? std::string("-") : a.groups[std::size_t(a.groupOf[0])].id;
}

utg::Item item(const char* rec, const char* cls, const char* rank, unsigned flags = 0,
               bool fromMod = false, bool known = true) {
    utg::Item it;
    it.record = rec;
    it.cls = cls;
    it.rank = rank;
    it.flags = flags;
    it.fromMod = fromMod;
    it.knownClass = known;
    return it;
}

bool readFile(const std::string& path, std::string& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[65536];
    std::size_t n;
    out.clear();
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    std::fclose(f);
    return true;
}
bool writeFile(const std::string& path, const std::string& text) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    return std::fclose(f) == 0 && ok;
}

// The default template with its commented family examples switched on.
std::string withFamilies() {
    std::string t = utg::defaultGroupsIni(), out;
    bool in = false;
    std::size_t at = 0;
    while (at < t.size()) {
        std::size_t nl = t.find('\n', at);
        std::string line = t.substr(at, nl - at);
        at = nl + 1;
        if (line.compare(0, 20, "; ---- Family groups") == 0) in = true;
        else if (line.compare(0, 22, "; ---- The slot groups") == 0) in = false;
        else if (in && line.compare(0, 2, "; ") == 0 && line.compare(0, 6, "; ----") != 0
                 && (line[2] == '[' || line.find(" = ") != std::string::npos))
            line = line.substr(2);
        out += line + "\n";
    }
    return out;
}

void testDefaultTemplate() {
    utg::GroupSet gs = mustParse(utg::defaultGroupsIni(), "the built-in template");
    std::size_t n = 0;
    const utg::BuiltinGroup* b = utg::builtinGroups(&n);
    check(n == 25 && std::string(b[24].id) == "blueprint" && std::string(b[24].label) == "Blueprints"
              && std::string(b[24].tag) == "BLU" && std::string(b[24].cls) == "ItemArtifactFormula",
          "the built-in table holds the 24 slot groups and the blueprints (Blueprints / BLU)");
    // the built-in ids, in declaration order, are exactly the built-in 25
    std::vector<const utg::GroupDef*> global;
    for (const utg::GroupDef& d : gs.groups) if (utg::builtinById(d.id)) global.push_back(&d);
    check(global.size() == 25, "the template declares the 25 built-in groups");
    for (std::size_t i = 0; i < global.size() && i < n; ++i) {
        check(global[i]->id == b[i].id, "global group " + std::to_string(i) + " is " + b[i].id);
        check(global[i]->label == b[i].label, "group " + global[i]->id + " is labelled " + b[i].label);
        check(global[i]->tag == b[i].tag, "group " + global[i]->id + " is tagged " + b[i].tag);
    }
    for (const char* id : {"staff", "spear1h", "reignofterror.jewels", "deathgrimdawn.bows",
                           "deathgrimdawn.gems", "deathgrimdawn.augments"}) {
        bool have = false;
        for (const utg::GroupDef& d : gs.groups) have = have || d.id == id;
        check(have, std::string("the template declares ") + id);
    }
    for (const char* id : {"reignofterror.bows", "reignofterror.staves", "reignofterror.javelins"}) {
        bool have = false;
        for (const utg::GroupDef& d : gs.groups) have = have || d.id == id;
        check(!have, std::string("Reign of Terror's old block is gone: ") + id);
    }
    // the two per-class blocks: in every view, right after 2H Guns, no label and no tag
    {
        std::size_t r2 = 0, st = 0, sp = 0, rel = 0;
        for (std::size_t i = 0; i < gs.groups.size(); ++i) {
            if (gs.groups[i].id == "ranged2h") r2 = i;
            if (gs.groups[i].id == "staff") st = i;
            if (gs.groups[i].id == "spear1h") sp = i;
            if (gs.groups[i].id == "relic") rel = i;
        }
        check(st == r2 + 1 && sp == st + 1 && rel == sp + 1, "staff and spear1h sit between 2H Guns and Relics");
        check(gs.groups[st].labelDerived && gs.groups[st].tagDerived && !gs.groups[st].labelSet
                  && gs.groups[st].cls == "weaponmagical_staff", "staff: no label or tag, derived from its class");
        check(gs.groups[sp].labelDerived && gs.groups[sp].tagDerived && gs.groups[sp].cls == "weaponhunting_spear",
              "spear1h: derived from its class");
        bool global2 = true;
        for (const utg::Rule& r : gs.rules)
            if (r.group == int(st) || r.group == int(sp))
                global2 = global2 && (r.view.empty() ||
                                      (r.view == "collect_mi" && r.ranks.size() == 1 && r.ranks[0] == "rare"));
        check(global2, "the per-class blocks apply in every view (their Rare rules: collect_mi's)");
    }
    // the blueprints: right after the relics, in every view, every rank, the class alone; the
    // game's own word for the class is "Blueprint", and the built-in label stays (never derived)
    {
        std::size_t rel = 0, bp = 0, nbp = 0;
        for (std::size_t i = 0; i < gs.groups.size(); ++i) {
            if (gs.groups[i].id == "relic") rel = i;
            if (gs.groups[i].id == "blueprint") { bp = i; ++nbp; }
        }
        check(nbp == 1 && bp == rel + 1, "blueprint sits right after Relics");
        const utg::GroupDef& d = gs.groups[bp];
        check(d.label == "Blueprints" && d.tag == "BLU" && !d.labelDerived && !d.tagDerived
                  && d.cls == "itemartifactformula", "blueprint: Blueprints / BLU, class ItemArtifactFormula, not derived");
        std::size_t rules = 0;
        bool global1 = true;
        for (const utg::Rule& r : gs.rules)
            if (r.group == int(bp)) { ++rules; global1 = global1 && r.view.empty(); }
        check(rules == 1 && global1, "one blueprint block, in every view");
        // what it takes: every rank, base and mod; Crate's test blueprints are excluded above it
        const char* bpDir = "records/items/crafting/blueprints/";
        struct Case { std::string rec; const char* rank; bool mod; const char* view; const char* want; };
        const Case cases[] = {
            {std::string(bpDir) + "armor/craft_head_d001.dbr", "Legendary", false, "vanilla", "blueprint"},
            {std::string(bpDir) + "armor/craft_head_c001.dbr", "Epic", false, "vanilla", "blueprint"},
            {std::string(bpDir) + "component/craft_component_x.dbr", "Rare", false, "vanilla", "blueprint"},
            {std::string(bpDir) + "faction/craft_x.dbr", "Magical", false, "vanilla", "blueprint"},
            {std::string(bpDir) + "other/craft_elixir_chaos.dbr", "Legendary", false, "vanilla", "blueprint"},
            {std::string(bpDir) + "blueprint_test_weapon1.dbr", "Epic", false, "vanilla", "-"},
            {std::string(bpDir) + "blueprint_loottabletest.dbr", "Epic", false, "vanilla", "-"},
            {std::string(bpDir) + "blueprint_test_potion.dbr", "Legendary", false, "vanilla", "-"},
            {"records/0_rot/items/crafting/blueprints/rot_bp_x.dbr", "Magical", true, "custom-reignofterror", "blueprint"},
            {"records/items/diabloitems/blueprints/bp_windforce.dbr", "Legendary", true, "basemods+custom-deathgrimdawn", "blueprint"},
        };
        for (const Case& c : cases) {
            const std::string got = groupIdOf(gs, item(c.rec.c_str(), "ItemArtifactFormula", c.rank, utg::kFlagBlueprint, c.mod),
                                              utg::viewKeysFor(c.view));
            check(got == c.want, std::string(c.view) + ": " + c.rec + " (" + c.rank + ") -> " + c.want + " (got " + got + ")");
        }
    }
    check(gs.autoEnabled && gs.autoMin == 5, "auto groups on, min 5");
    for (const utg::GroupDef& d : gs.groups) {
        check(utg::validId(d.id), "id " + d.id + " is valid");
        check(utg::validTag(d.tag) && !utg::reservedTag(d.tag), "tag of " + d.id + " is valid");
    }

    // vanilla: every slot class at Epic/Legendary lands in its slot group
    const std::vector<std::string> none;
    for (std::size_t i = 0; i < n; ++i) {
        const char* rank = std::strcmp(b[i].id, "relic") == 0 ? "Rare" : "Legendary";
        check(groupIdOf(gs, item("records/items/x/a.dbr", b[i].cls, rank), none) == b[i].id,
              std::string(b[i].cls) + " -> " + b[i].id);
        check(groupIdOf(gs, item("records/items/x/a.dbr", b[i].cls, "Epic"), none) == b[i].id,
              std::string(b[i].cls) + " Epic -> " + b[i].id);
    }
    check(groupIdOf(gs, item("records/items/x/a.dbr", "ArmorProtective_Head", "Rare"), none) == "-",
          "Rare gear is not paged");
    check(groupIdOf(gs, item("records/endlessdungeon/items/a001_ring.dbr", "ArmorJewelry_Ring", "Common",
                             utg::kFlagExtra | utg::kFlagEquipment), none) == "ring",
          "the curated extra (a Common ring) is in Rings");
    check(groupIdOf(gs, item("records/items/crafting/blueprints/a.dbr", "ItemArtifactFormula", "Legendary",
                             utg::kFlagBlueprint), none) == "blueprint", "a blueprint is paged in Blueprints");
    check(groupIdOf(gs, item("records/items/materia/compa_a.dbr", "ItemRelic", "Legendary"), none) == "-",
          "a component is not paged in vanilla");
    check(groupIdOf(gs, item("records/items/enchants/a.dbr", "ItemEnchantment", "Epic",
                             utg::kFlagAugment), none) == "-", "an augment is not paged in vanilla");

    // Reign of Terror: by slot by default, its own classes in their own groups
    const std::vector<std::string> rot = utg::viewKeysFor("custom-reignofterror");
    check(groupIdOf(gs, item("records/0_rot/items/runeword/helm_rw01.dbr", "ArmorProtective_Head",
                             "Legendary", 0, true), rot) == "head", "a ROT runeword helm is a helm");
    check(groupIdOf(gs, item("records/0_rot/items/jewelery/charm/charm01.dbr", "ItemArtifact",
                             "Epic", utg::kFlagRelic, true), rot) == "relic", "a ROT charm is a relic");
    check(groupIdOf(gs, item("records/xpack/item/equipmentweapon/axe/u_axe.dbr", "WeaponMelee_Axe",
                             "Legendary", 0, true), rot) == "axe1h", "a TQ unique axe is a 1H axe");
    check(groupIdOf(gs, item("records/0_rot/items/weapons/bow/bow01.dbr", "WeaponMagical_Staff",
                             "Legendary", 0, true, false), rot) == "staff", "a ROT bow is in the staff-class group");
    check(groupIdOf(gs, item("records/0_rot/items/weapons/xbow/xbow01.dbr", "WeaponMagical_Staff",
                             "Epic", 0, true, false), rot) == "staff", "a ROT staff-class crossbow too");
    check(groupIdOf(gs, item("records/0_rot/items/weapons/staff/st01.dbr", "WeaponMagical_Staff",
                             "Legendary", 0, true, false), rot) == "staff", "a ROT staff too");
    check(groupIdOf(gs, item("records/0_rot/items/weapons/spear/jav01.dbr", "WeaponHunting_Spear",
                             "Legendary", 0, true, false), rot) == "spear1h", "a ROT javelin is in the hunting-spear group");
    check(groupIdOf(gs, item("records/items/d2items/legend_gems/gem_legendary_01.dbr", "ItemRelic",
                             "Legendary", 0, true), rot) == "reignofterror.jewels", "a ROT jewel");
    check(groupIdOf(gs, item("records/0_rot/items/runeword/weapon/rot_rw_weapon_ice_bow_item.dbr",
                             "WeaponMagical_Staff", "Legendary", 0, true, false), rot) == "staff",
          "a ROT runeword bow (Mist's folder) is in the staff-class group");
    check(groupIdOf(gs, item("records/items/d2items/tormented/gearweapons/bow_u02.dbr", "WeaponMagical_Staff",
                             "Legendary", 0, true, false), rot) == "staff", "a ROT Tormented bow too");
    check(groupIdOf(gs, item("records/0_rot/items/weapon/enemy/staff/cain_staff01.dbr", "WeaponMagical_Staff",
                             "Legendary", 0, true, false), rot) == "staff", "Cain's staff (Memory) too: the game calls it a Bow");
    check(groupIdOf(gs, item("records/mod/staff/st01.dbr", "WeaponMagical_Staff", "Epic", 0, true, false), none)
              == "staff", "any view's staff-class unique lands in the staff group");
    check(groupIdOf(gs, item("records/mod/staff/st01.dbr", "WeaponMagical_Staff", "Rare", 0, true, false), none)
              == "other", "a Rare staff-class record is not the staff group's without collect_mi (the catalogue admits one only as an MI)");
    check(groupIdOf(gs, item("records/items/d2items/gearweapons/xbow/xbow_u01.dbr", "WeaponHunting_Ranged2h",
                             "Legendary", 0, true), rot) == "ranged2h", "a ROT crossbow-class xbow stays in 2H Guns");
    check(groupIdOf(gs, item("records/0_rot/items/socket/runes/rot_rune05.dbr", "ItemRelic", "Rare", 0, true), rot)
              == "reignofterror.jewels", "a Rare ROT rune is in Jewels, Gems & Runes");
    check(groupIdOf(gs, item("records/items/d2items/materia/gems/gem_ruby_01.dbr", "ItemRelic", "Common", 0, true),
                    rot) == "reignofterror.jewels", "a Common D2 gem is in Jewels, Gems & Runes");
    check(groupIdOf(gs, item("records/items/d2items/materia/gems/gem_ruby_01.dbr", "ItemRelic", "Common", 0, true),
                    none) == "-", "the D2 gem rule is ROT's only");
    check(groupIdOf(gs, item("records/items/misc/potions/potion_healtha01.dbr", "ItemRelic", "Common", 0, true),
                    rot) == "-", "a Common mod component outside the gem folders stays off the page");
    check(groupIdOf(gs, item("records/items/materia/compa_a.dbr", "ItemRelic", "Legendary", 0, false), rot)
              == "-", "a base component stays off the page in ROT");
    check(groupIdOf(gs, item("records/items/d2items/legend_gems/gem_legendary_01.dbr", "ItemRelic",
                             "Legendary", 0, true), none) == "-", "the ROT jewel rule is ROT's only");
    // Death Grim Dawn: its augments
    const std::vector<std::string> dgd = utg::viewKeysFor("basemods+custom-deathgrimdawn");
    check(groupIdOf(gs, item("records/items/remaugments/aug_01.dbr", "ItemEnchantment", "Common",
                             utg::kFlagAugment, true), dgd) == "deathgrimdawn.augments", "a DGD augment");
    check(groupIdOf(gs, item("records/items/enchants/a.dbr", "ItemEnchantment", "Epic",
                             utg::kFlagAugment), dgd) == "-", "a base augment stays off the page in DGD");
    check(groupIdOf(gs, item("records/items/remaugments/aug_01.dbr", "ItemEnchantment", "Common",
                             utg::kFlagAugment, true), rot) == "-", "the DGD augment rule is DGD's only");
    // Death Grim Dawn: its bows and crossbows (crossbow-class records in one folder) before Guns,
    // its D2 gems and runes at every rank
    check(groupIdOf(gs, item("records/items/diabloitems/crossbows/crossbow2h_windforce.dbr",
                             "WeaponHunting_Ranged2h", "Legendary", 0, true), dgd) == "deathgrimdawn.bows",
          "Windforce is in Bows & Crossbows in DGD");
    check(groupIdOf(gs, item("records/items/diabloitems/crossbows/crossbow1h_dawn.dbr",
                             "WeaponHunting_Ranged1h", "Epic", 0, true), dgd) == "deathgrimdawn.bows",
          "a DGD hand crossbow too");
    check(groupIdOf(gs, item("records/items/diablosets/natalyas_crossbow2h.dbr", "WeaponHunting_Ranged2h",
                             "Legendary", utg::kFlagSet, true), dgd) == "deathgrimdawn.bows",
          "Natalya's crossbow too");
    check(groupIdOf(gs, item("records/items/diabloitems/crossbows/crossbow2h_windforce.dbr",
                             "WeaponHunting_Ranged2h", "Legendary", 0, true), rot) == "ranged2h",
          "the DGD bow rule is DGD's only");
    check(groupIdOf(gs, item("records/items/gearweapons/guns2h/c001_gun2h.dbr", "WeaponHunting_Ranged2h",
                             "Legendary"), dgd) == "ranged2h", "a base 2H gun stays in 2H Guns in DGD");
    check(groupIdOf(gs, item("records/items/diabloitems/crossbows/crossbow2h_x.dbr", "WeaponHunting_Ranged2h",
                             "Rare", 0, true), dgd) == "-", "a Rare DGD crossbow is not paged");
    check(groupIdOf(gs, item("records/items/diabloitems/gems/gem_diamond_01.dbr", "ItemRelic", "Common", 0, true),
                    dgd) == "deathgrimdawn.gems", "a Common DGD gem is in Gems & Runes");
    check(groupIdOf(gs, item("records/items/diabloitems/runes/rune_16.dbr", "ItemRelic", "Rare", 0, true), dgd)
              == "deathgrimdawn.gems", "a Rare DGD rune too");
    check(groupIdOf(gs, item("records/reskill/components/reskill_windforce.dbr", "ItemRelic", "Legendary", 0, true),
                    dgd) == "-", "a DGD skill rune stays off the page by default");
    check(groupIdOf(gs, item("records/items/diabloitems/gems/gem_diamond_01.dbr", "ItemRelic", "Common", 0, true),
                    rot) == "-", "the DGD gem rule is DGD's only");

    // one family switched on with its [view:...] line and nothing else: the slot groups still
    // apply in every view, because the template reopens every view before them
    {
        std::string t = utg::defaultGroupsIni(), one;
        bool in = false;
        std::size_t at = 0;
        while (at < t.size()) {
            std::size_t nl = t.find('\n', at);
            std::string line = t.substr(at, nl - at);
            at = nl + 1;
            if (line == "; [view:reignofterror]" && one.find("[view:reignofterror]\n") == std::string::npos) in = true;
            if (line == "; [group reignofterror.charms]") in = false;
            if (in && line.compare(0, 2, "; ") == 0) line = line.substr(2);
            one += line + "\n";
        }
        utg::GroupSet gs1 = mustParse(one, "the template with one family block on");
        check(!gs1.groups.empty() && gs1.groups[0].id == "reignofterror.runewords", "the one family is declared first");
        check(groupIdOf(gs1, item("records/items/x/a.dbr", "ArmorProtective_Head", "Epic"), none) == "head",
              "with one family on, a vanilla helm is still a helm");
        check(groupIdOf(gs1, item("records/0_rot/items/runeword/helm_rw01.dbr", "ArmorProtective_Head",
                                  "Legendary", 0, true), rot) == "reignofterror.runewords",
              "and the family takes its records in its view");
    }

    // the family examples, switched on
    utg::GroupSet fam = mustParse(withFamilies(), "the template with its family examples on");
    check(fam.groups.size() == gs.groups.size() + 7, "seven family groups declared");
    check(!fam.groups.empty() && fam.groups[0].id == "reignofterror.runewords", "families come first");
    check(groupIdOf(fam, item("records/0_rot/items/runeword/helm_rw01.dbr", "ArmorProtective_Head",
                              "Legendary", 0, true), rot) == "reignofterror.runewords", "families: a runeword");
    check(groupIdOf(fam, item("records/0_rot/items/jewelery/charm/charm01.dbr", "ItemArtifact",
                              "Epic", utg::kFlagRelic, true), rot) == "reignofterror.charms", "families: a charm");
    check(groupIdOf(fam, item("records/xpack/item/equipmentweapon/axe/u_axe.dbr", "WeaponMelee_Axe",
                              "Legendary", 0, true), rot) == "reignofterror.titanquest", "families: a TQ unique");
    check(groupIdOf(fam, item("records/items/d2items/uniques/u01.dbr", "ArmorProtective_Chest",
                              "Legendary", 0, true), rot) == "reignofterror.diablo2", "families: a D2 unique");
    check(groupIdOf(fam, item("records/0_rot/items/weapons/bow/bow01.dbr", "WeaponMagical_Staff",
                              "Legendary", 0, true, false), rot) == "staff", "families: bows stay in the staff group");
    check(groupIdOf(fam, item("records/0_rot/items/runeword/helm_rw01.dbr", "ArmorProtective_Head",
                              "Legendary", 0, true), none) == "head", "families: only in their view");
    check(groupIdOf(fam, item("records/items/diabloitems/helm/h01.dbr", "ArmorProtective_Head",
                              "Legendary", 0, true), dgd) == "deathgrimdawn.diablo", "families: DGD Diablo items");
    check(groupIdOf(fam, item("records/reskill/components/reskill_windforce.dbr", "ItemRelic", "Legendary", 0, true),
                    dgd) == "deathgrimdawn.skillrunes", "families: DGD skill runes");
}

void testGlob() {
    struct G { const char* pat; const char* path; bool want; };
    const G cases[] = {
        {"records/items/*.dbr", "records/items/a.dbr", true},
        {"records/items/*.dbr", "records/items/x/a.dbr", false},
        {"records/items/**", "records/items/x/y/a.dbr", true},
        {"records/items/**", "records/itemsx/a.dbr", false},
        {"**/bow/**", "records/0_rot/items/weapons/bow/b.dbr", true},
        {"**/bow/**", "records/0_rot/items/weapons/rainbow/b.dbr", false},
        {"records/**/bow/*", "records/bow/b.dbr", true},
        {"records/**/bow/*", "records/a/b/bow/b.dbr", true},
        {"records/items/gear?/a.dbr", "records/items/gear1/a.dbr", true},
        {"records/items/gear?/a.dbr", "records/items/gear/a.dbr", false},
        {"records/items/gear?a.dbr", "records/items/gear/a.dbr", false},
        {"*", "records", true},
        {"*", "records/a", false},
        {"**", "", true},
        {"a*b*c", "axxbyyc", true},
        {"a*b*c", "axxbyy", false},
    };
    for (const G& c : cases)
        check(utg::globMatch(c.pat, c.path) == c.want,
              std::string("glob ") + c.pat + " vs " + c.path + (c.want ? " matches" : " does not match"));
    utg::GroupSet gs = mustParse("[group a]\npath = Records/Items/**\n", "a mixed-case glob");
    check(groupIdOf(gs, item("RECORDS\\ITEMS\\X\\A.DBR", "X", "Epic"), {}) == "a",
          "paths compare without case and with either separator");
}

void testPrecedenceAndViews() {
    const std::string text =
        "[group first]\nclass = A\nrank = Epic\n"
        "[exclude]\nclass = A\n"
        "[group second]\nclass = A\n"
        "[view:modx]\n"
        "[group modx.special]\npath = records/special/**\n"
        "[view:*]\n"
        "[group catchall]\npath = **\n";
    utg::GroupSet gs = mustParse(text, "the precedence file");
    check(gs.groups.size() == 4, "four groups declared");
    check(groupIdOf(gs, item("records/x/a.dbr", "A", "Epic"), {}) == "first", "the first block wins");
    check(groupIdOf(gs, item("records/x/a.dbr", "A", "Legendary"), {}) == "-", "an exclude above wins");
    check(groupIdOf(gs, item("records/special/a.dbr", "B", "Epic"), {}) == "catchall",
          "a view block is off outside its view");
    check(groupIdOf(gs, item("records/special/a.dbr", "B", "Epic"), utg::viewKeysFor("custom-modx"))
              == "modx.special", "a view block is on in its view");
    check(groupIdOf(gs, item("records/special/a.dbr", "B", "Epic"), utg::viewKeysFor("basemods+custom-modx"))
              == "modx.special", "and in its /basemods view");
    check(groupIdOf(gs, item("records/other/a.dbr", "B", "Epic"), utg::viewKeysFor("custom-modx"))
              == "catchall", "[view:*] returns to every view");
    // keys
    std::vector<std::string> k = utg::viewKeysFor("basemods+custom-reignofterror");
    check(k.size() == 3 && k[0] == "basemods+custom-reignofterror" && k[1] == "reignofterror"
              && k[2] == "basemods", "the keys of basemods+custom-<name>");
    check(utg::viewKeysFor("vanilla") == std::vector<std::string>{"vanilla"}, "the keys of vanilla");
    check(utg::viewKeysFor("basemods") == std::vector<std::string>{"basemods"}, "the keys of basemods");
    // AND across keys, OR within one
    utg::GroupSet ao = mustParse("[group g]\nclass = A, B\nrank = Epic\nflag = set, relic\nsource = mod\n", "AND/OR");
    check(groupIdOf(ao, item("r/a.dbr", "b", "EPIC", utg::kFlagRelic, true), {}) == "g", "all keys match");
    check(groupIdOf(ao, item("r/a.dbr", "C", "Epic", utg::kFlagRelic, true), {}) == "-", "class fails");
    check(groupIdOf(ao, item("r/a.dbr", "A", "Epic", utg::kFlagEquipment, true), {}) == "-", "flag fails");
    check(groupIdOf(ao, item("r/a.dbr", "A", "Epic", utg::kFlagSet, false), {}) == "-", "source fails");
    // a later block of the same id adds rules, keeps the position
    utg::GroupSet re = mustParse("[group a]\nclass = A\n[group b]\nclass = B\n[group a]\nclass = C\n", "re-opened id");
    check(re.groups.size() == 2 && re.groups[0].id == "a", "a re-opened id keeps its place");
    check(groupIdOf(re, item("r/a.dbr", "C", "Epic"), {}) == "a", "a re-opened id takes its new rule");
    // defaults
    utg::GroupSet df = mustParse("\xEF\xBB\xBF; bom\r\n[group head]\r\nclass = X\r\n[group mymod.long_bows]\r\nclass = Y ; note\r\n", "defaults");
    check(df.groups.size() == 2 && df.groups[0].label == "Helms" && df.groups[0].tag == "HEL",
          "a built-in id takes its built-in label and tag");
    check(df.groups.size() == 2 && df.groups[1].label == "Long_bows" && df.groups[1].tag == "LON",
          "another id takes its last part and three letters of it");
    check(df.rules.size() == 2 && df.rules[1].classes.size() == 1 && df.rules[1].classes[0] == "y",
          "an inline comment is not part of the value");
}

void testErrors() {
    struct E { const char* text; int line; const char* what; };
    const E cases[] = {
        {"[group a]\nclass = A\nwhat = 1\n", 3, "an unknown key"},
        {"[nope]\n", 1, "an unknown section"},
        {"[group A]\nclass = A\n", 1, "an upper-case id"},
        {"[group aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa]\nclass = A\n", 1, "a 41-character id"},
        {"[group other]\nclass = A\n", 1, "a reserved id"},
        {"[group auto.x]\nclass = A\n", 1, "an auto. id"},
        {"[group next]\nclass = A\n", 1, "next (the strings file's caption.next)"},
        {"[group a]\ntag = AB\nclass = A\n", 2, "a 2-character tag"},
        {"[group a]\ntag = MAT\nclass = A\n", 2, "the MAT tag"},
        {"[group a]\nlabel = A\n", 1, "a block with no filter"},
        {"[exclude]\n[group a]\nclass = A\n", 1, "an exclude with no filter"},
        {"class = A\n", 1, "a key outside a block"},
        {"[group a]\nlabel = A\nclass = A\n[group a]\nlabel = B\nclass = B\n", 5, "a second label"},
        {"[group a]\nflag = shiny\n", 2, "an unknown flag"},
        {"[group a]\nsource = elsewhere\n", 2, "a bad source"},
        {"[auto]\nmin = 0\n", 2, "min 0"},
        {"[group a\nclass = A\n", 1, "a header without ']'"},
        {"[group a]\nclass\n", 2, "a line without '='"},
        {"[group a]\nclass = ,\n", 2, "an empty list"},
        {"[view:Bad Key]\n", 1, "a bad view key"},
        {"[view:x]\n[auto]\nmin = 3\n", 2, "[auto] inside a view"},
    };
    for (const E& c : cases) {
        utg::GroupSet gs;
        std::string err;
        bool ok = utg::parseGroups(c.text, gs, &err);
        std::string want = "line " + std::to_string(c.line) + ":";
        check(!ok && err.compare(0, want.size(), want) == 0 && gs.groups.empty() && gs.rules.empty(),
              std::string("refused whole, naming the line: ") + c.what + " (got \"" + err + "\")");
    }
}

void testAutoAndOffPage() {
    const std::string text = "[exclude]\npath = records/junk/**\n"
                             "[group head]\nclass = ArmorProtective_Head\n"
                             "[auto]\nmin = 3\n";
    utg::GroupSet gs = mustParse(text, "the auto file");
    std::vector<utg::Item> items;
    for (int i = 0; i < 4; ++i) items.push_back(item("records/m/staff.dbr", "WeaponMagical_Staff", "Epic", 0, true, false));
    for (int i = 0; i < 3; ++i) items.push_back(item("records/m/sta.dbr", "WeaponX_Stab", "Epic", 0, true, false));
    items.push_back(item("records/m/odd.dbr", "Weird_Thing", "Epic", 0, true, false));        // 8
    items.push_back(item("records/m/odd2.dbr", "Strange_Thing", "Epic", 0, true, false));     // 9
    items.push_back(item("records/m/hat.dbr", "ArmorProtective_Head", "Epic"));               // 10
    items.push_back(item("records/junk/hat.dbr", "ArmorProtective_Head", "Epic"));            // 11 (excluded)
    items.push_back(item("records/items/bp.dbr", "ItemArtifactFormula", "Epic"));             // 12 (known, unmatched)
    items.push_back(item("records/items/bp2.dbr", "ItemArtifactFormula", "Epic"));            // 13
    items.push_back(item("records/junk/staff.dbr", "WeaponMagical_Staff", "Epic", 0, true, false)); // 14 excluded
    utg::Assignment a;
    utg::assignGroups(gs, items, {}, a);
    auto idOf = [&](std::size_t i) { return a.groupOf[i] < 0 ? std::string("-") : a.groups[std::size_t(a.groupOf[i])].id; };
    check(idOf(0) == "auto.weaponmagical_staff", "a class with min records gets its own auto group");
    check(idOf(4) == "auto.weaponx_stab", "the second auto class too");
    check(idOf(7) == "other" && idOf(8) == "other", "classes under min share Other");
    check(idOf(9) == "head", "a declared group keeps its records");
    check(idOf(10) == "-", "a record an exclude drops is on no page");
    check(idOf(11) == "-", "a record of a known class no rule takes is on no page");
    check(idOf(12) == "-", "another record no rule takes is on no page");
    check(idOf(13) == "-", "a record of an unknown class that is excluded is on no page");
    std::vector<std::string> order;
    for (const utg::GroupDef& d : a.groups) order.push_back(d.id);
    check(order == std::vector<std::string>{"head", "auto.weaponmagical_staff", "auto.weaponx_stab", "other"},
          "display order: declared, auto by class, Other - and no Retired");
    check(a.groups.size() == 4 && a.groups[1].label == "Staves" && a.groups[1].tag == "STA",
          "a class with a type word and no text: its last part, plural - Staves / STA (" +
              (a.groups.size() == 4 ? a.groups[1].label + "/" + a.groups[1].tag : std::string()) + ")");
    check(a.groups.size() == 4 && a.groups[2].label == "Stab" && a.groups[2].tag == "STB",
          "an unknown class is named from its last part; STA is taken, so the consonant step gives STB (" +
              (a.groups.size() == 4 ? a.groups[2].tag : std::string()) + ")");
    check(a.groups.size() == 4 && a.groups[3].tag == "OTH", "Other OTH");
    // auto tags never repeat a declared one
    utg::GroupSet clash = mustParse("[group staves]\ntag = STF\nclass = Nothing\n", "a clashing tag");
    utg::assignGroups(clash, std::vector<utg::Item>(5, item("r/s.dbr", "WeaponMagical_Staff", "Epic", 0, true, false)), {}, a);
    check(a.groups.size() == 2 && a.groups[1].tag != "STF" && utg::validTag(a.groups[1].tag),
          "an auto tag that clashes is changed (" + (a.groups.size() == 2 ? a.groups[1].tag : std::string()) + ")");
    // auto off: unknown classes stay off the page
    utg::GroupSet off = mustParse("[auto]\nenabled = no\n[group h]\nclass = H\n", "auto off");
    utg::assignGroups(off, {item("r/s.dbr", "S", "Epic", 0, true, false), item("r/t.dbr", "S", "Epic", 0, true, false)}, {}, a);
    check(a.groupOf[0] == -1 && a.groupOf[1] == -1 && a.groups.size() == 1,
          "with auto off an unknown class is off the page, and no group is made for it");
    // `retired` is no longer the generator's: a groups file may name a group so
    utg::GroupSet ret = mustParse("[group retired]\nclass = H\n", "a group named retired");
    utg::assignGroups(ret, {item("r/h.dbr", "H", "Epic", 0, true, false)}, {}, a);
    check(!utg::reservedId("retired") && a.groupOf[0] == 0 && a.groups[0].id == "retired",
          "retired is an ordinary id now (not reserved)");
}

void testJournalScan(const std::string& scratch) {
    const std::string j =
        "{\"journal\":\"grim dawn uniquetab\",\"format\":4,\"written\":\"2000-01-01T00:00:00Z\",\"entries\":5}\n"
        "{\"record\":\"records/items/a.dbr\",\"deposited\":\"x\",\"count\":1,\"stack\":1,\"flags\":0,\"raw\":\"010:00000001\"}\n"
        "{\"record\":\"records/items/b.dbr\",\"count\":2}\r\n"
        "{\"record\":\"Records/Items/C.dbr\"}\n"
        "{\"record\":\"records/items/b.dbr\",\"count\":0}\n"
        "{\"record\":\"records/items/d\\u00e9.dbr\",\"name@010\":\"say \\\"hi\\\"\",\"count\":3}\n"
        "not json at all\n"
        "{\"record\":\"records/items/e.dbr\",\"count\":\"many\"}\n"
        "\n";
    utg::JournalCounts jc;
    std::string err;
    check(utg::scanJournalText(j, jc, &err), "the journal scans (" + err + ")");
    check(jc.rows == 7 && jc.badRows == 2, "seven rows, two bad");
    check(jc.counts.size() == 4, "four records");
    check(jc.counts["records/items/a.dbr"] == 1, "a counts 1");
    check(jc.counts["records/items/b.dbr"] == 0, "the last line of b wins");
    check(jc.counts.count("records/items/c.dbr") && jc.counts["records/items/c.dbr"] == 0,
          "an absent count is 0 and the key is lower-cased");
    check(jc.counts["records/items/d\xC3\xA9.dbr"] == 3, "escapes decode");
    std::unordered_set<std::string> owned;
    utg::JournalCounts hc;
    check(utg::scanJournalText("{\"journal\":\"grim dawn uniquetab\",\"format\":3}\n{\"record\":\"records/items/h.dbr\",\"count\":1}\n", hc, &err),
          "a second journal scans");
    utg::ownedFrom({jc, hc}, owned);
    check(owned.size() == 3 && owned.count("records/items/a.dbr") && owned.count("records/items/h.dbr")
              && !owned.count("records/items/b.dbr"), "owned = count >= 1 in either file");
    check(!utg::scanJournalText("{\"record\":\"records/items/a.dbr\",\"count\":1}\n", jc, &err),
          "a file without the header is not a journal");
    check(!utg::scanJournalText("", jc, &err), "an empty file is not a journal");
    // the file reader
    const std::string path = scratch + "/test-journal.jsonl";
    check(writeFile(path, j), "the journal fixture is written");
    check(utg::scanJournalFile(path, jc, &err) == utg::JournalRead::Read && jc.counts.size() == 4,
          "the file reads");
    check(utg::scanJournalFile(scratch + "/no-such-journal.jsonl", jc, &err) == utg::JournalRead::Absent,
          "a missing file is Absent");
    const std::string junk = scratch + "/test-journal-junk.jsonl";
    check(writeFile(junk, "\x01\x02garbage\n"), "a junk fixture is written");
    check(utg::scanJournalFile(junk, jc, &err) == utg::JournalRead::Unreadable, "a junk file is Unreadable");
}

void testGLine() {
    utg::GLine g;
    g.index = 3; g.label = "Gloves"; g.cols = 10; g.rows = 8; g.cellW = 64; g.cellH = 64; g.count = 151;
    g.id = "hands"; g.tag = "GLO";
    const std::string s = utg::formatGLine(g);
    check(s == "G\t3\tGloves\t10\t8\t64\t64\t151\thands\tGLO", "the 9-field form");
    utg::GLine r;
    check(utg::parseGLine(s, r) && r.nineFields && r.id == "hands" && r.tag == "GLO" && r.count == 151
              && r.cellH == 64 && r.label == "Gloves", "the 9-field form reads back");
    check(utg::parseGLine("G\t3\tGloves\t10\t8\t64\t64\t151", r) && !r.nineFields && r.id == "hands"
              && r.tag == "GLO", "a 7-field line takes its id and tag from its label");
    check(utg::parseGLine("G\t23\tSomething\t10\t8\t32\t32\t9", r) && r.id == "relic" && r.tag == "REL",
          "an unknown label takes the built-in entry at its position");
    check(utg::parseGLine("G\t30\tSomething\t10\t8\t32\t32\t9", r) && r.id == "group30" && r.tag == "SOM",
          "past the table it gets a made-up id");
    check(!utg::parseGLine("G\t3\tGloves\t10\t8\t64\t64\t151\thands", r), "8 fields are refused");
    check(!utg::parseGLine("G\t3\tGloves\t10\t8\t64\t64", r), "6 fields are refused");
    check(!utg::parseGLine("G\t3\tGloves\t10\t8\t64\t64\t151\tHands\tGLO", r), "a bad id is refused");
    check(!utg::parseGLine("G\t3\tGloves\t10\t8\t64\t64\t151\thands\tGLOV", r), "a bad tag is refused");
    check(!utg::parseGLine("E\tx\ty", r), "an E line is not a G line");
}

void testOracle(const std::string& path) {
    std::string text;
    if (!readFile(path, text)) { check(false, "the oracle " + path + " reads"); return; }
    std::size_t n = 0;
    const utg::BuiltinGroup* b = utg::builtinGroups(&n);
    std::size_t gi = 0, at = 0;
    bool allNine = true, allCrlf = true;
    while (at < text.size()) {
        std::size_t nl = text.find('\n', at);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(at, nl - at);
        at = nl + 1;
        if (line.empty() || line[0] != 'G') continue;
        if (line.back() == '\r') line.pop_back(); else allCrlf = false;
        utg::GLine g;
        if (!utg::parseGLine(line, g) || !g.nineFields) { allNine = false; continue; }
        if (gi < n) {
            check(g.index == int(gi) && g.id == b[gi].id && g.tag == b[gi].tag && g.label == b[gi].label,
                  "oracle G line " + std::to_string(gi) + " is " + b[gi].id + " / " + b[gi].tag);
            // the same line in the 7-field form reads to the same id and tag
            std::size_t cut = line.rfind('\t');
            cut = line.rfind('\t', cut - 1);
            utg::GLine old;
            check(utg::parseGLine(line.substr(0, cut), old) && !old.nineFields && old.id == g.id && old.tag == g.tag,
                  "oracle G line " + std::to_string(gi) + " without its two new fields reads the same");
        }
        ++gi;
    }
    check(allNine, "every oracle G line has 9 fields");
    check(allCrlf, "every oracle G line ends CRLF");
    check(gi == 25, "the oracle has 25 G lines (" + std::to_string(gi) + ")");
}

// The stock copies: a file byte-identical (CRLF folded) to a template an earlier build wrote is
// renamed .old (then .old1 ...) and not read; an edited one is read. `oldDir`, when given, holds
// the templates earlier builds wrote (<n>.ini, CRLF, n = the "earlier template n" of
// ut_groups_default.h), copied from data\oracle\templates by build_uniq_db.py --stock-templates.
void testRetire(const std::string& scratch, const std::string& oldDir) {
    const std::string path = scratch + "/retire-groups.ini";
    for (const char* sfx : {"", ".old", ".old1", ".old2"}) std::remove((path + sfx).c_str());
    std::string crlf;
    for (const char* p = utg::defaultGroupsIni(); *p; ++p) { if (*p == '\n') crlf += '\r'; crlf += *p; }
    check(utg::isStockGroupsText(crlf) && utg::isStockGroupsText(utg::defaultGroupsIni()),
          "this build's template, CRLF or LF, is a stock copy");
    check(!utg::isStockGroupsText(crlf + "; mine\r\n"), "an edited copy is not");
    check(!utg::isStockGroupsText("[group head]\r\nclass = X\r\n"), "a user file is not");
    check(utg::groupsTextHash("") == 0xcbf29ce484222325ull && utg::groupsTextHash("a") == 0xaf63dc4c8601ec8cull,
          "the hash is FNV-1a 64");
    utg::GroupSet gs;
    utg::GroupsLoad gl;
    check(writeFile(path, crlf), "a stock copy is written");
    check(utg::loadGroupsFile(path, gs, &gl) == utg::GroupsFile::Retired && gl.renamedTo == path + ".old"
              && gs.groups.size() >= 24, "a stock copy is renamed .old and the built-in groups are used");
    std::string text;
    check(!readFile(path, text) && readFile(path + ".old", text) && text == crlf, "the .old holds it byte for byte");
    check(writeFile(path, crlf), "a second stock copy is written");
    check(utg::loadGroupsFile(path, gs, &gl) == utg::GroupsFile::Retired && gl.renamedTo == path + ".old1",
          "with .old taken it becomes .old1");
    const std::string edited = crlf + "[group my.extra]\r\nlabel = Extra\r\nclass = ItemRelic\r\n";
    check(writeFile(path, edited), "an edited copy is written");
    check(utg::loadGroupsFile(path, gs, &gl) == utg::GroupsFile::Loaded && readFile(path, text) && text == edited,
          "an edited copy is read and left where it is");
    for (const char* sfx : {"", ".old", ".old1"}) std::remove((path + sfx).c_str());
    if (oldDir.empty()) { std::printf("[test] groups: the historical templates SKIPPED (no folder given)\n"); return; }
    int seen = 0;
    for (int n = 1; n <= 4; ++n) {
        const std::string c = std::to_string(n);
        std::string old;
        if (!readFile(oldDir + "/" + c + ".ini", old)) continue;
        ++seen;
        check(utg::isStockGroupsText(old), "earlier template " + c + " is recognised (" +
                                               std::to_string(old.size()) + " B)");
        check(writeFile(path, old), "it is written as a user's file");
        check(utg::loadGroupsFile(path, gs, &gl) == utg::GroupsFile::Retired, "and retired: earlier template " + c);
        std::remove((path + ".old").c_str());
    }
    if (!seen) std::printf("[test] groups: the historical templates SKIPPED (none in %s)\n", oldDir.c_str());
    else check(seen == 4, "all four earlier templates were checked");
}

void testFiles(const std::string& scratch) {
    const std::string path = scratch + "/files-groups.ini";
    std::remove(path.c_str());
    utg::GroupSet gs;
    utg::GroupsLoad gl;
    std::string why;
    const std::size_t nBuiltin = utg::builtinGroupSet(nullptr)->groups.size();
    check(utg::loadGroupsFile(path, gs, &gl) == utg::GroupsFile::Default && gs.groups.size() == nBuiltin
              && gs.groups[0].id == "head" && gl.bytes.empty(), "no file: the built-in groups");
    check(utg::loadGroupsFile("", gs, &gl) == utg::GroupsFile::Default && gs.groups.size() == nBuiltin,
          "no path: the built-in groups");
    const std::string good = "[view:reignofterror]\r\n[group my.runewords]\r\nlabel = Runewords\r\ntag = RWD\r\n"
                             "path = records/0_rot/items/runeword/**\r\n";
    check(writeFile(path, good), "a groups file is written");
    check(utg::loadGroupsFile(path, gs, &gl) == utg::GroupsFile::Loaded && gs.groups.size() == nBuiltin + 1
              && gs.groups[0].id == "my.runewords" && gs.groups[1].id == "head" && gl.bytes == good
              && gl.stats.yours == 1 && gl.stats.extended == 0 && gl.stats.builtin == nBuiltin,
          "a good file is ADDED: its group first, every built-in group kept, its bytes for the stamp");
    const std::vector<std::string> rot = utg::viewKeysFor("custom-reignofterror");
    check(groupIdOf(gs, item("records/0_rot/items/runeword/helm_rw01.dbr", "ArmorProtective_Head",
                             "Legendary", 0, true), rot) == "my.runewords", "its block takes its records off their usual page");
    check(groupIdOf(gs, item("records/0_rot/items/x/helm.dbr", "ArmorProtective_Head", "Legendary", 0, true), rot)
              == "head", "the built-in groups still take the rest");
    check(writeFile(path, "[group head]\r\nclass = ArmorProtective_Head\r\nwhat = 1\r\n"), "a bad file is written");
    check(utg::loadGroupsFile(path, gs, &gl) == utg::GroupsFile::Refused && gs.groups.size() == nBuiltin
              && gs.groups[0].id == "head" && gl.why.find(path + ": line 3:") == 0,
          "a bad file is refused whole, naming the user's file and its line, the built-in groups used (" + gl.why + ")");
    std::remove(path.c_str());
    // the view's journals
    const std::string sc = scratch + "/files-uniq-items.jsonl", hc = scratch + "/files-uniq-items-hc.jsonl";
    check(writeFile(sc, "{\"journal\":\"grim dawn uniquetab\",\"format\":4}\n{\"record\":\"records/a.dbr\",\"count\":1}\n"),
          "a softcore journal is written");
    std::remove(hc.c_str());
    std::unordered_set<std::string> owned;
    why.clear();
    check(utg::ownedFromJournals({sc, hc}, owned, &why) && owned.size() == 1 && owned.count("records/a.dbr"),
          "softcore read, a missing -hc adds nothing");
    check(writeFile(hc, "garbage\n"), "an unreadable -hc is written");
    owned.clear();
    check(!utg::ownedFromJournals({sc, hc}, owned, &why) && owned.size() == 1 && why.find("files-uniq-items-hc") != std::string::npos,
          "an unreadable journal says so and names the file; the readable one still counts");
    std::remove(sc.c_str());
    std::remove(hc.c_str());
}


// The user's file IN FRONT of the built-in set (mergeGroupSets): order, [exclude] on a built-in
// group, a built-in id given more rules and new words, the user's [auto] last, no label merge.
utg::GroupSet merged(const std::string& userText, utg::MergeStats* st = nullptr) {
    utg::GroupSet user = mustParse(userText, "a user file"), out;
    utg::mergeGroupSets(user, *utg::builtinGroupSet(nullptr), out, st);
    return out;
}

void testMerge() {
    const utg::GroupSet& base = *utg::builtinGroupSet(nullptr);
    const std::vector<std::string> none, rot = utg::viewKeysFor("custom-reignofterror");
    utg::MergeStats st;
    // an empty file is the built-in set
    utg::GroupSet e = merged("; nothing\n", &st);
    check(e.groups.size() == base.groups.size() && e.rules.size() == base.rules.size() && st.yours == 0
              && st.extended == 0, "an empty file is exactly the built-in set");
    // [exclude] on a built-in group's records
    utg::GroupSet x = merged("[exclude]\nclass = ItemArtifact\n", &st);
    check(groupIdOf(x, item("records/items/gearrelic/a.dbr", "ItemArtifact", "Epic", utg::kFlagRelic), none) == "-",
          "a user [exclude] keeps a built-in group's records off every page");
    check(groupIdOf(x, item("records/items/x/a.dbr", "ArmorProtective_Head", "Epic"), none) == "head",
          "and nothing else");
    // a built-in id: more rules, its place kept, the user's words
    utg::GroupSet h = merged("[group head]\nlabel = Hats\npath = records/hats/**\n[group my.new]\nclass = Foo\n", &st);
    check(st.yours == 1 && st.extended == 1, "one group of yours, one built-in group given more rules");
    check(h.groups.size() == base.groups.size() + 1 && h.groups[0].id == "my.new" && h.groups[1].id == "head"
              && h.groups[1].label == "Hats" && h.groups[1].tag == "HEL" && h.groups[1].labelSet,
          "the new group first; head keeps its place and tag, takes the user's label");
    check(groupIdOf(h, item("records/hats/a.dbr", "ArmorJewelry_Ring", "Common"), none) == "head",
          "the user's rule for a built-in id is tried first");
    check(groupIdOf(h, item("records/items/x/a.dbr", "ArmorProtective_Head", "Epic"), none) == "head",
          "and the built-in rule still applies");
    check(groupIdOf(h, item("records/items/x/a.dbr", "Foo", "Epic"), none) == "my.new", "a new group takes its class");
    // a user block of the staff group with its own word
    utg::GroupSet sw = merged("[group staff]\nlabel = Staves\ntag = STF\n" "path = records/never/**\n");
    std::size_t si = 0;
    for (std::size_t i = 0; i < sw.groups.size(); ++i) if (sw.groups[i].id == "staff") si = i;
    check(sw.groups[si].label == "Staves" && sw.groups[si].tag == "STF" && !sw.groups[si].labelDerived
              && !sw.groups[si].tagDerived, "a user label/tag on a derived group replaces the derivation");
    // the user's [auto] applies last
    utg::GroupSet au = merged("[auto]\nmin = 2\n");
    check(au.autoMin == 2 && au.autoEnabled, "a user [auto] min wins");
    utg::GroupSet ao = merged("[auto]\nenabled = no\n");
    check(!ao.autoEnabled && ao.autoMin == base.autoMin, "a user [auto] enabled = no wins, min stays");
    // the same label on two groups: two groups, no merge, no error
    utg::GroupSet dl = merged("[group my.helms]\nlabel = Helms\nclass = Foo\n", &st);
    std::vector<utg::Item> its{item("r/a.dbr", "Foo", "Epic"), item("r/b.dbr", "ArmorProtective_Head", "Epic")};
    utg::Assignment a;
    utg::assignGroups(dl, its, none, a);
    check(a.groupOf[0] != a.groupOf[1] && a.groups[std::size_t(a.groupOf[0])].label == "Helms"
              && a.groups[std::size_t(a.groupOf[1])].label == "Helms",
          "two groups with one label stay two groups (the buttons file merges buttons)");
    // a user view block before the slot groups in its view only
    utg::GroupSet vb = merged("[view:reignofterror]\n[group my.d2]\nlabel = Diablo II\npath = records/items/d2items/**\n");
    check(groupIdOf(vb, item("records/items/d2items/u/chest.dbr", "ArmorProtective_Chest", "Legendary", 0, true), rot)
              == "my.d2", "a user family takes its records first in its view");
    check(groupIdOf(vb, item("records/items/d2items/u/chest.dbr", "ArmorProtective_Chest", "Legendary", 0, true), none)
              == "chest", "and not outside it");
}

// The six views' own words for the two classes without a built-in group, as their text has
// them (research GH; read from the 1.3.0.8 Text_EN.arc and each mod's text_en*.arc).
struct ViewWords { const char* view; const char* staff; const char* spear; const char* mace2h; };
const ViewWords kWords[] = {
    {"vanilla", "Staff", "Spear", "Two-Handed Mace"},
    {"custom-reignofterror", "Bow", "Spear", "Two-Handed Blunt"},
    {"basemods+custom-deathgrimdawn", "Bow", "Artifact", "Two-Handed Mace"},
    {"custom-grimarillion", "Staff", "Spear", "Two-Handed Mace"},
    {"custom-dom", "Staff", "Spear", "Two-Handed Mace"},
    {"custom-grimarmory", "Staff", "Spear", "Two-Handed Mace"},
};

std::map<std::string, std::string> wordsOf(const ViewWords& v) {
    // the base words of every class, then the view's three that differ
    std::map<std::string, std::string> w = {
        {"weaponmelee_axe", "One-Handed Axe"}, {"weaponmelee_sword", "One-Handed Sword"},
        {"weaponmelee_mace", "One-Handed Mace"}, {"weaponmelee_dagger", "One-Handed Dagger"},
        {"weaponmelee_scepter", "One-Handed Scepter"}, {"weaponhunting_ranged2h", "Two-Handed Ranged"},
        {"weaponarmor_shield", "Shield"}, {"weaponarmor_offhand", "Off-Hand"},
        {"weaponhunting_ranged1h", "One-Handed Ranged"}, {"weaponmelee_axe2h", "Two-Handed Axe"},
        {"weaponmelee_sword2h", "Two-Handed Sword"}, {"weaponmelee_spear2h", "Two-Handed Spear"},
        {"armorprotective_head", "Helm"}, {"armorjewelry_amulet", "Amulet"}, {"armorprotective_chest", "Chest Armor"},
        {"armorprotective_legs", "Pants"}, {"armorprotective_feet", "Boots"}, {"armorjewelry_ring", "Ring"},
        {"armorprotective_hands", "Gloves"}, {"armorprotective_waist", "Belt"},
        {"armorprotective_shoulders", "Shoulders"}, {"armorjewelry_medal", "Medal"},
    };
    w["weaponmagical_staff"] = v.staff;
    w["weaponhunting_spear"] = v.spear;
    w["weaponmelee_mace2h"] = v.mace2h;
    return w;
}

void testDerivation() {
    // the pieces
    check(utg::pluralWord("Staff") == "Staves" && utg::pluralWord("Bow") == "Bows" && utg::pluralWord("Box") == "Boxes"
              && utg::pluralWord("Torch") == "Torches" && utg::pluralWord("Brush") == "Brushes"
              && utg::pluralWord("Glass") == "Glasses" && utg::pluralWord("Daisy") == "Daisies"
              && utg::pluralWord("Day") == "Days" && utg::pluralWord("Two-Handed Blunt") == "Two-Handed Blunts"
              && utg::pluralWord("Ax3") == "Ax3" && utg::pluralWord("") == "" && utg::pluralWord("Spear") == "Spears",
          "the plural: -ff -ves, -s/-x/-ch/-sh +es, consonant-y -ies, else +s, a non-letter end kept");
    check(utg::cleanWord("^eMemory") == "Memory" && utg::cleanWord("{^E}Bow ") == "Bow" && utg::cleanWord("[ms]Bow") == "Bow"
              && utg::cleanWord("  ") == "" && utg::cleanWord("A\tB") == "A B" && utg::cleanWord("[Bows]") == "[Bows]",
          "the word: colour codes, a gender marker and blanks go");
    check(utg::classFamilyWord("WeaponHunting_Spear") == "Hunting" && utg::classFamilyWord("weaponmagical_staff") == "Magical"
              && utg::classFamilyWord("ArmorJewelry_Ring") == "Jewelry" && utg::classFamilyWord("ItemRelic") == ""
              && utg::classFamilyWord("Weapon_X") == "Weapon", "the class family word");
    std::vector<std::string> c = utg::tagCandidates("Spear");
    check(c.size() == 38 && c[0] == "SPE" && c[1] == "SPR" && c[2] == "SP0" && c[37] == "SPZ",
          "the tag ladder: three letters, letter + two consonants, two letters + 0-9 A-Z");
    c = utg::tagCandidates("Bow");
    check(c.size() == 37 && c[0] == "BOW" && c[1] == "BO0", "a word without two consonants skips the second step");
    std::size_t n = 0;
    utg::classTypeTags(&n);
    check(n == 25 && std::string(utg::classTypeTag("WeaponMagical_Staff")) == "tagStaff"
              && std::string(utg::classTypeTag("WEAPONHUNTING_SPEAR")) == "tagSpear"
              && std::string(utg::classTypeTag("ArmorProtective_Waist")) == "tagBelt"
              && utg::classTypeTag("ItemRelic") == nullptr, "the engine's class -> type tag table (25 classes)");

    // every class of the six views, through the built-in set: the 25 keep their words, the two
    // per-class groups take the view's
    std::size_t nb = 0;
    const utg::BuiltinGroup* b = utg::builtinGroups(&nb);
    struct Want { const char* view; const char* staffLabel; const char* staffTag; const char* spearLabel; const char* spearTag; };
    const Want want[] = {
        {"vanilla", "Staves", "STA", "Hunting Spears", "SPE"},
        {"custom-reignofterror", "Bows", "BOW", "Hunting Spears", "SPE"},
        {"basemods+custom-deathgrimdawn", "Bows", "BO0", "Artifacts", "ART"},
        {"custom-grimarillion", "Staves", "STA", "Hunting Spears", "SPE"},
        {"custom-dom", "Staves", "STA", "Hunting Spears", "SPE"},
        {"custom-grimarmory", "Staves", "STA", "Hunting Spears", "SPE"},
    };
    for (std::size_t v = 0; v < sizeof kWords / sizeof *kWords; ++v) {
        utg::GroupSet gs = *utg::builtinGroupSet(nullptr);
        gs.words = wordsOf(kWords[v]);
        const std::vector<std::string> keys = utg::viewKeysFor(kWords[v].view);
        std::vector<utg::Item> its;
        for (std::size_t i = 0; i < nb; ++i) its.push_back(item("records/items/x/a.dbr", b[i].cls, "Legendary", 0, false));
        its.push_back(item("records/mod/staff.dbr", "WeaponMagical_Staff", "Legendary", 0, true, false));
        its.push_back(item("records/mod/spear.dbr", "WeaponHunting_Spear", "Epic", 0, true, false));
        // DGD: its bows block takes a crossbow, so BOW is held by "Bows & Crossbows"
        its.push_back(item("records/items/diabloitems/crossbows/c.dbr", "WeaponHunting_Ranged2h", "Legendary", 0, true));
        utg::Assignment a;
        utg::assignGroups(gs, its, keys, a);
        const std::string vw = kWords[v].view;
        bool baseKept = true;
        for (std::size_t i = 0; i < nb; ++i) {
            const utg::GroupDef& d = a.groups[std::size_t(a.groupOf[i])];
            baseKept = baseKept && d.id == b[i].id && d.label == b[i].label && d.tag == b[i].tag;
        }
        check(baseKept, vw + ": the 25 built-in groups (the blueprints included) keep their ids, labels and tags");
        const utg::GroupDef& sg = a.groups[std::size_t(a.groupOf[nb])];
        const utg::GroupDef& pg = a.groups[std::size_t(a.groupOf[nb + 1])];
        check(sg.id == "staff" && sg.label == want[v].staffLabel && sg.tag == want[v].staffTag,
              vw + ": the staff class is " + want[v].staffLabel + " / " + want[v].staffTag + " (got " + sg.label + " / " + sg.tag + ")");
        check(pg.id == "spear1h" && pg.label == want[v].spearLabel && pg.tag == want[v].spearTag,
              vw + ": the hunting-spear class is " + want[v].spearLabel + " / " + want[v].spearTag + " (got " + pg.label + " / " + pg.tag + ")");
        // the table the buttons file is generated with: every group that got a record, in order
        std::vector<utg::GroupRow> rows = utg::viewGroupTable(a);
        std::size_t want_rows = 0;
        for (char u : a.used) want_rows += u ? 1 : 0;
        bool ordered = rows.size() == want_rows;
        std::set<std::string> tagsSeen, labelsSeen;
        for (const utg::GroupRow& r : rows) {
            ordered = ordered && utg::validTag(r.tag) && !utg::reservedTag(r.tag) && utg::validLabel(r.label);
            tagsSeen.insert(r.tag);
            labelsSeen.insert(r.label);
        }
        check(ordered && tagsSeen.size() == rows.size() && labelsSeen.size() == rows.size(),
              vw + ": the view's group table - " + std::to_string(rows.size()) + " rows, unique labels and tags");
        std::string line;
        for (const utg::GroupRow& r : rows) line += r.tag + "=" + r.label + "; ";
        std::printf("[test] groups %s: %s\n", vw.c_str(), line.c_str());
    }
    // without a two-handed spear in the view, "Spears" is free
    {
        utg::GroupSet gs = *utg::builtinGroupSet(nullptr);
        gs.words = wordsOf(kWords[3]);
        utg::Assignment a;
        utg::assignGroups(gs, {item("records/mod/spear.dbr", "WeaponHunting_Spear", "Epic", 0, true, false)}, {}, a);
        check(a.groups[std::size_t(a.groupOf[0])].label == "Spears" && a.groups[std::size_t(a.groupOf[0])].tag == "SPE",
              "a view with no two-handed spear: the hunting spears are Spears / SPE");
    }
    // the collision rules over a user's words: the family prefix, then a number; the tag ladder
    {
        utg::GroupSet gs = merged("[group my.a]\nlabel = Bows\ntag = BOW\nclass = Foo\n"
                                  "[group my.b]\nlabel = Magical Bows\ntag = SPE\nclass = Bar\n");
        gs.words = wordsOf(kWords[1]);
        utg::Assignment a;
        utg::assignGroups(gs, {item("r/a.dbr", "Foo", "Epic"), item("r/b.dbr", "Bar", "Epic"),
                               item("r/s.dbr", "WeaponMagical_Staff", "Epic", 0, true, false),
                               item("r/p.dbr", "WeaponHunting_Spear", "Epic", 0, true, false),
                               item("r/q.dbr", "WeaponMelee_Spear2h", "Epic")}, {}, a);
        const utg::GroupDef& sg = a.groups[std::size_t(a.groupOf[2])];
        const utg::GroupDef& pg = a.groups[std::size_t(a.groupOf[3])];
        check(sg.label == "Magical Bows 2" && sg.tag == "BO0",
              "Bows and Magical Bows taken: Magical Bows 2, and BOW taken: BO0 (got " + sg.label + " / " + sg.tag + ")");
        check(pg.label == "Hunting Spears" && pg.tag == "SP0",
              "SPE and SPR taken: the hunting spears are SP0 (got " + pg.label + " / " + pg.tag + ")");
    }
    // a group nobody gave a label, with no class word: the id's last part as before
    {
        utg::GroupSet gs = merged("[group mymod.charms]\npath = records/charms/**\n");
        utg::Assignment a;
        utg::assignGroups(gs, {item("records/charms/a.dbr", "ItemArtifact", "Epic", utg::kFlagRelic, true)}, {}, a);
        const utg::GroupDef& d = a.groups[std::size_t(a.groupOf[0])];
        check(d.label == "Charms" && d.tag == "CHA", "no class: the id's last part and three letters (" + d.label + " / " + d.tag + ")");
    }
    // the parsed template, unused groups keep the parser's stand-ins
    {
        utg::Assignment a;
        utg::assignGroups(*utg::builtinGroupSet(nullptr), {}, {}, a);
        check(utg::viewGroupTable(a).empty(), "no record, no row");
    }
}
} // namespace

// ---- the monster infrequents (collect_mi) ----------------------------------------------------
// The template's last section [view:collect_mi] gives each of the 23 slot groups one Rare rule,
// then the two per-class groups (staff, spear1h: Reign of Terror's Bows and Hunting Spears);
// a view answers to that key only while collect_mi=1 (generate.cpp), so with the flag off a Rare
// helm is on no page (an owned one keeps its journal row) and with it on it is in Helms, after the
// uniques (the page order ranks Rare after Epic). The per-game [exclude] blocks with rank = Rare
// keep a mod's MI-classified junk off the pages and never touch a unique on the same path.
void testMonsterInfrequents() {
    std::printf("\n-- the monster infrequents ([view:collect_mi], the per-game Rare excludes)\n");
    std::string err;
    utg::GroupSet gs;
    check(utg::parseGroups(utg::defaultGroupsIni(), gs, &err), "the template parses (" + err + ")");
    std::size_t n = 0;
    const utg::BuiltinGroup* b = utg::builtinGroups(&n);
    // the section: DGD's crossbow-folder rule first, then 25 class rules - one per slot group
    // (rank Rare, class = the slot's), then the two per-class groups' (staff, spear1h) - the last
    std::vector<const utg::Rule*> mi;
    const utg::Rule* dgdBows = nullptr;
    for (const utg::Rule& r : gs.rules) {
        if (r.view != "collect_mi") continue;
        if (!r.paths.empty() && !dgdBows && mi.empty()) dgdBows = &r;
        else mi.push_back(&r);
    }
    check(dgdBows && !dgdBows->exclude && gs.groups[std::size_t(dgdBows->group)].id == "deathgrimdawn.bows" &&
              dgdBows->classes.empty() && dgdBows->paths.size() == 2 && dgdBows->ranks.size() == 1 &&
              dgdBows->ranks[0] == "rare",
          "[view:collect_mi] opens with DGD's Bows & Crossbows Rare rule (its two crossbow paths)");
    bool shape = mi.size() == 25;
    struct Want { std::string id, cls; };
    std::vector<Want> want;
    for (std::size_t i = 0; i < 23; ++i) want.push_back({b[i].id, b[i].cls});
    want.push_back({"staff", "WeaponMagical_Staff"});
    want.push_back({"spear1h", "WeaponHunting_Spear"});
    for (std::size_t i = 0; shape && i < mi.size(); ++i) {
        const utg::Rule& r = *mi[i];
        shape = !r.exclude && r.group >= 0 && gs.groups[std::size_t(r.group)].id == want[i].id &&
                r.classes.size() == 1 && r.classes[0] == utg::lowerAscii(want[i].cls) &&
                r.ranks.size() == 1 && r.ranks[0] == "rare" && r.paths.empty() && r.flags == 0;
    }
    check(shape, "[view:collect_mi]: then one Rare rule for each of the 23 slot groups in the built-in order, "
                 "then staff and spear1h");
    check(!mi.empty() && mi.back() == &gs.rules.back(), "the section is the template's last");
    {
        const std::string all = utg::defaultGroupsIni();
        const std::size_t cut = all.find("\n[view:collect_mi]");
        utg::GroupSet before;
        bool same = cut != std::string::npos && utg::parseGroups(all.substr(0, cut + 1), before, &err) &&
                    before.groups.size() == gs.groups.size();
        for (std::size_t i = 0; same && i < gs.groups.size(); ++i)
            same = before.groups[i].id == gs.groups[i].id && before.groups[i].label == gs.groups[i].label &&
                   before.groups[i].tag == gs.groups[i].tag;
        check(same, "the section adds no group and moves none (" + std::to_string(gs.groups.size()) + " groups)");
    }
    // placements
    std::vector<std::string> off = utg::viewKeysFor("vanilla"), on = off;
    on.push_back("collect_mi");
    for (std::size_t i = 0; i < 23; ++i) {
        const utg::Item rare = item("records/items/x/b001a.dbr", b[i].cls, "Rare", utg::kFlagEquipment);
        check(groupIdOf(gs, rare, off) == "-" && groupIdOf(gs, rare, on) == b[i].id,
              std::string("a Rare ") + b[i].cls + ": no page with the flag off, " + b[i].id + " with it on");
    }
    check(groupIdOf(gs, item("records/items/gearrelic/b001_relic.dbr", "ItemArtifact", "Rare", utg::kFlagRelic), off) == "relic",
          "a Rare relic is in Relics either way");
    check(groupIdOf(gs, item("records/items/gearhead/d001_head.dbr", "ArmorProtective_Head", "Legendary"), on) == "head",
          "a unique is where it was with the flag on");
    struct Case { const char* view; const char* rec; const char* rank; bool mod; const char* want; };
    const Case cases[] = {
        {"custom-reignofterror", "records/0_rot/items/armor/npc/korlic/korlic_axe01.dbr", "Rare", true, "-"},
        {"custom-reignofterror", "records/0_rot/fluffy/a/helm.dbr", "Rare", true, "-"},
        {"custom-reignofterror", "records/0_rot/items/armor/npc/korlic/korlic_helm.dbr", "Legendary", true, "head"},
        {"custom-reignofterror", "records/items/d2items/rare/helm01.dbr", "Rare", true, "head"},
        {"basemods+custom-deathgrimdawn", "records/pogd/cosmetics/x/helm.dbr", "Rare", true, "-"},
        {"custom-deathgrimdawn", "records/playtest/helm.dbr", "Rare", true, "-"},
        {"custom-deathgrimdawn", "records/skills/pet/helm.dbr", "Rare", true, "-"},
        {"custom-deathgrimdawn", "records/items/f001a_sword.dbr", "Rare", true, "-"},
        {"custom-deathgrimdawn", "records/items/diabloitems/mi/helm01.dbr", "Rare", true, "head"},
        {"custom-grimarmory", "records/items/crafting/crafting_x01.dbr", "Rare", true, "-"},
        {"custom-grimarillion", "records/items/replicas/gearhead/replica_d001_head.dbr", "Rare", true, "-"},
        {"custom-grimarillion", "records/skills/playerclasstempest/helm.dbr", "Rare", true, "-"},
        {"custom-grimarillion", "records/items/swampmod/vinewarped/helm.dbr", "Rare", true, "head"},
        {"custom-dom", "records/cosmetics/items/wings/wings01.dbr", "Rare", true, "-"},
        {"custom-dom", "records/tq/monster/creatures/khopesh.dbr", "Rare", true, "-"},
        {"vanilla", "records/0_rot/items/armor/npc/korlic/korlic_helm.dbr", "Rare", false, "head"},
    };
    for (const Case& c : cases) {
        std::vector<std::string> keys = utg::viewKeysFor(c.view);
        keys.push_back("collect_mi");
        const std::string got = groupIdOf(gs, item(c.rec, "ArmorProtective_Head", c.rank, utg::kFlagEquipment, c.mod), keys);
        check(got == c.want, std::string(c.view) + " +collect_mi: " + c.rec + " (" + c.rank + ") -> " + c.want +
                                 " (got " + got + ")");
    }
    // the per-class groups take a mod's Rare staves / hunting spears with the flag (
    // Reign of Terror's Rare bows and javelins are MIs too), the NPC exclude takes one as gear
    // (pages_gen.cpp: an MI carries kFlagEquipment); DGD's Bows & Crossbows takes its Rare crossbows
    {
        std::vector<std::string> rot = utg::viewKeysFor("custom-reignofterror"), rotOn = rot;
        rotOn.push_back("collect_mi");
        const utg::Item bow = item("records/items/d2items/gearweapons/bow/mi_bow_01a.dbr", "WeaponMagical_Staff",
                                   "Rare", utg::kFlagEquipment, true, false);
        const utg::Item jav = item("records/items/d2items/gearweapons/javelin/mi01a_javelin.dbr",
                                   "WeaponHunting_Spear", "Rare", utg::kFlagEquipment, true, false);
        check(groupIdOf(gs, bow, rotOn) == "staff" && groupIdOf(gs, jav, rotOn) == "spear1h",
              "ROT +collect_mi: a Rare bow (staff class) -> staff (Bows), a Rare javelin -> spear1h (Hunting Spears)");
        check(groupIdOf(gs, bow, rot) == "other" && groupIdOf(gs, jav, rot) == "other",
              "ROT without the flag: neither has its class group (never admitted then; an owned one is on Other)");
        check(groupIdOf(gs, item("records/0_rot/items/armor/npc/korlic/korlic_bow.dbr", "WeaponMagical_Staff", "Rare",
                                 utg::kFlagEquipment, true, false), rotOn) == "-",
              "ROT +collect_mi: a Rare staff-class NPC weapon is excluded like any NPC gear");
        std::vector<std::string> dgd = utg::viewKeysFor("custom-deathgrimdawn"), dgdOn = dgd;
        dgdOn.push_back("collect_mi");
        const utg::Item xbow = item("records/items/diabloitems/crossbows/mi/xbow01a.dbr", "WeaponHunting_Ranged2h",
                                    "Rare", utg::kFlagEquipment, true);
        const utg::Item setx = item("records/items/diablosets/s01_crossbow.dbr", "WeaponHunting_Ranged2h", "Rare",
                                    utg::kFlagEquipment, true);
        check(groupIdOf(gs, xbow, dgdOn) == "deathgrimdawn.bows" && groupIdOf(gs, setx, dgdOn) == "deathgrimdawn.bows",
              "DGD +collect_mi: a Rare crossbow joins Bows & Crossbows, not 2H Guns");
        check(groupIdOf(gs, item("records/items/diabloitems/crossbows/mi/xbow1h01a.dbr", "WeaponHunting_Ranged1h",
                                 "Rare", utg::kFlagEquipment, true), dgdOn) == "deathgrimdawn.bows",
              "DGD +collect_mi: a Rare hand crossbow too, not Guns");
        check(groupIdOf(gs, item("records/items/gearweapons/guns2h/b001a_gun2h.dbr", "WeaponHunting_Ranged2h", "Rare",
                                 utg::kFlagEquipment), dgdOn) == "ranged2h",
              "DGD +collect_mi: a base Rare rifle outside the crossbow folders stays on 2H Guns");
        check(groupIdOf(gs, xbow, dgd) == "-",
              "DGD without the flag: a Rare crossbow is on no page (like every MI)");
        check(groupIdOf(gs, item("records/items/d2items/gearweapons/xbow/mi_xbow01.dbr", "WeaponHunting_Ranged2h",
                                 "Rare", utg::kFlagEquipment, true), rotOn) == "ranged2h",
              "ROT +collect_mi: a Rare crossbow outside DGD's folders stays on 2H Guns by class");
    }
    // the excludes take Rare GEAR only: Grimarillion's Rare replica recipes stay on Blueprints
    {
        std::vector<std::string> gri = utg::viewKeysFor("custom-grimarillion");
        gri.push_back("collect_mi");
        check(groupIdOf(gs, item("records/items/replicas/recipes/head/recipe_replica_headb001.dbr", "ItemArtifactFormula",
                                 "Rare", utg::kFlagBlueprint, true), gri) == "blueprint" &&
                  groupIdOf(gs, item("records/items/replicas/recipes/head/recipe_replica_headb001.dbr",
                                     "ItemArtifactFormula", "Rare", utg::kFlagBlueprint, true),
                            utg::viewKeysFor("custom-grimarillion")) == "blueprint",
              "a Rare blueprint under Grimarillion's replicas/ is on Blueprints, flag on or off (flag = equipment)");
        std::vector<std::string> rot = utg::viewKeysFor("custom-reignofterror");
        check(groupIdOf(gs, item("records/0_rot/fluffy/relic.dbr", "ItemArtifact", "Rare", utg::kFlagRelic, true), rot) ==
                  "relic",
              "a Rare relic on an excluded ROT path is still a relic");
    }
    // flag off: an MI is on no page (a stored one waits in the journal), the unique stays in Helms
    {
        std::vector<utg::Item> two{item("records/items/gearhead/b002d_head.dbr", "ArmorProtective_Head", "Rare",
                                        utg::kFlagEquipment),
                                   item("records/items/gearhead/d001_head.dbr", "ArmorProtective_Head", "Legendary")};
        utg::Assignment a;
        utg::assignGroups(gs, two, off, a);
        bool noRet = true;
        for (const utg::GroupDef& d : a.groups) noRet = noRet && d.id != "retired";
        check(a.groupOf[0] == -1 && a.groups[std::size_t(a.groupOf[1])].id == "head" && noRet,
              "flag off: an MI is on no page and no Retired group is made, the unique stays in Helms");
        utg::assignGroups(gs, two, on, a);
        check(a.groups[std::size_t(a.groupOf[0])].id == "head",
              "flag on: the same MI is in Helms");
    }
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: test_groups <data\\oracle\\uniq-groups.txt> <scratch dir>\n");
        return 2;
    }
    testDefaultTemplate();
    testGlob();
    testPrecedenceAndViews();
    testErrors();
    testAutoAndOffPage();
    testJournalScan(argv[2]);
    testGLine();
    testOracle(argv[1]);
    testRetire(argv[2], argc >= 4 ? argv[3] : "");
    testFiles(argv[2]);
    testMerge();
    testDerivation();
    testMonsterInfrequents();
    std::printf("[test] groups: %d checks, %d failed\n", g_pass + g_fail, g_fail);
    if (g_fail) { std::printf("[test] FAILED\n"); return 1; }
    std::printf("[test] ALL PASS\n");
    return 0;
}
