// catalogue_gen.cpp - see catalogue_gen.h.
#include "gen/catalogue_gen.h"
#include "gen/arc_reader.h"
#include "gen/arz_reader.h"
#include "gen/ui_geometry.h"
#include "gen/ut_groups.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>

namespace gen {

namespace {

struct ClassSlot { const char* cls; const char* slot; };

const ClassSlot kEquipSlot[] = {
    {"ArmorProtective_Head", "head"}, {"ArmorProtective_Shoulders", "shoulders"},
    {"ArmorProtective_Chest", "chest"}, {"ArmorProtective_Hands", "hands"},
    {"ArmorProtective_Legs", "legs"}, {"ArmorProtective_Feet", "feet"},
    {"ArmorProtective_Waist", "waist"}, {"ArmorJewelry_Ring", "ring"},
    {"ArmorJewelry_Amulet", "amulet"}, {"ArmorJewelry_Medal", "medal"},
    {"WeaponMelee_Sword", "sword1h"}, {"WeaponMelee_Axe", "axe1h"},
    {"WeaponMelee_Mace", "mace1h"}, {"WeaponMelee_Dagger", "dagger"},
    {"WeaponMelee_Scepter", "scepter"}, {"WeaponMelee_Sword2h", "sword2h"},
    {"WeaponMelee_Axe2h", "axe2h"}, {"WeaponMelee_Mace2h", "mace2h"},
    {"WeaponMelee_Spear2h", "spear2h"}, {"WeaponHunting_Ranged1h", "ranged1h"},
    {"WeaponHunting_Ranged2h", "ranged2h"}, {"WeaponArmor_Offhand", "offhand"},
    {"WeaponArmor_Shield", "shield"},
};
// The two equipment classes the base game has no slot for, each with a per-class group of its own
// in the built-in groups ([group staff], [group spear1h]: Reign of Terror's Bows and Hunting
// Spears, Grimarillion's Staves). A mod layer's record of either is admitted as "other" (the mod
// rule); with collect_mi=1 its Rare records are monster infrequents too (isEquipmentClass).
const char* const kPerClassEquipment[] = {"WeaponMagical_Staff", "WeaponHunting_Spear"};
const ClassSlot kOtherSlot[] = {
    {"ItemArtifact", "relic"}, {"ItemArtifactFormula", "blueprint"}, {"ItemRelic", "component"},
    {"ItemEnchantment", "augment"}, {"ItemAscensionFormula", "blueprint"},
    {"AscendantAltarFormula", "blueprint"}, {"ItemRandomSetFormula", "blueprint"},
    {"ItemRerollFormula", "blueprint"}, {"ItemSetFormula", "blueprint"},
    {"ItemFactionBooster", "consumable"}, {"ItemFactionWarrant", "consumable"},
    {"ItemDifficultyUnlock", "consumable"}, {"ItemAttributeReset", "consumable"},
    {"ItemDevotionReset", "consumable"}, {"OneShot_SkillUnlock", "consumable"},
    {"QuestItem", "quest"}, {"LootRandomizer", "affix"},
};
const char* const kBlueprintClasses[] = {
    "ItemArtifactFormula", "ItemAscensionFormula", "AscendantAltarFormula",
    "ItemRandomSetFormula", "ItemRerollFormula", "ItemSetFormula",
};
const char* const kRelicClass = "ItemArtifact";
// The blueprint a player learns at the blacksmith: collected at EVERY classification (a recipe is
// a recipe - Magical and Rare ones too), base and mod alike, with its own scroll icon. Its five
// siblings in kBlueprintClasses (the Ascendant altar's formulas, the ascension / reroll / set
// ones) are not recipes and are never collected.
const char* const kFormulaClass = "ItemArtifactFormula";
const char* const kRelicPath = "records/items/gearrelic/";

// The curated inclusion list: records the rank rule drops (neither Epic nor Legendary) but a
// player really owns, admitted at their own classification.
const char* const kExtraRecords[] = {
    "records/endlessdungeon/items/a001_ring.dbr",       // Leovinus' Ring (Common)
};
// The developer test records that share that folder; never collectible.
const char* const kExtraExclude[] = {
    "records/endlessdungeon/items/z001_test.dbr",
    "records/endlessdungeon/items/q001_torso.dbr",
};

// Where a record is never collected from, in the base game and a mod layer alike: Crate's test
// records, creatures (the gear NPCs wear), test maps, and monster-worn gear wherever it is kept.
// Anywhere else a record of the right class and rank is collected - the base game keeps a few
// real items outside records/items/ too (the Lokarr set in records/storyelements/signs/, the
// quest rewards in records/storyelements/rewards/, the Totally Normal Shield's three versions
// among the Shattered Realm's script entities, records/endlessdungeon/scriptentities/
// portal_s01..03.dbr - the folder's other records are ScriptEntity and Decoration props, which
// no rule collects).
const char* const kJunkPrefix[] = {
    "records/sandbox/", "records/creatures/", "records/mod_testgrounds/",
};
const char* const kJunkPart = "/enemygear/";

const char* equipSlot(const std::string& cls) {
    for (const auto& e : kEquipSlot) if (cls == e.cls) return e.slot;
    return nullptr;
}
const char* otherSlot(const std::string& cls) {
    for (const auto& e : kOtherSlot) if (cls == e.cls) return e.slot;
    return nullptr;
}
bool isBlueprintClass(const std::string& cls) {
    for (const char* b : kBlueprintClasses) if (cls == b) return true;
    return false;
}
bool startsWith(const std::string& s, const char* p) {
    std::size_t n = std::strlen(p);
    return s.size() >= n && s.compare(0, n, p) == 0;
}
bool endsWith(const std::string& s, const char* p) {
    std::size_t n = std::strlen(p);
    return s.size() >= n && s.compare(s.size() - n, n, p) == 0;
}
bool inList(const std::string& key, const char* const* list, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) if (key == list[i]) return true;
    return false;
}

std::string nameTagOf(const ArzRecord& rec, const std::string& cls) {
    if (equipSlot(cls)) return rec.str("itemNameTag");
    std::string t = rec.str("itemNameTag");
    return t.empty() ? rec.str("description") : t;
}
std::string craftedRecord(const ArzRecord& rec, const std::string& cls) {
    return isBlueprintClass(cls) ? rec.str("artifactName") : std::string();
}
// A blueprint's icon is artifactFormulaBitmapName (the 64 x 32 scroll in its rarity's colour);
// an empty one takes its rarity's scroll (scrollOf, makeEntry) - never the crafted item's icon.
std::string bitmapOf(const ArzRecord& rec, const std::string& cls) {
    std::string v;
    if (cls == kFormulaClass) v = rec.str("artifactFormulaBitmapName");
    if (v.empty()) v = rec.str("bitmap");
    if (v.empty()) v = rec.str("artifactBitmap");
    if (v.empty()) v = rec.str("relicBitmap");
    return v;
}
// The scroll the base game draws for a blueprint of this classification (every base blueprint
// names one of these four, 64 x 32), for a blueprint whose own artifactFormulaBitmapName is empty
// (a custom game's: Grimarillion has 1,987). The crafted item's icon - up to 64 x 128 - would make
// the whole Blueprints cell that big. Any other word (Common, Quest) takes the plain blueprint
// scroll of items/misc.
const char* scrollOf(const std::string& classification) {
    if (classification == "Magical") return "items/craftingparts/materials/blueprint_a001_magical.tex";
    if (classification == "Rare") return "items/craftingparts/materials/blueprint_a002_rare.tex";
    if (classification == "Epic") return "items/craftingparts/materials/blueprint_a003_epic.tex";
    if (classification == "Legendary") return "items/craftingparts/materials/blueprint_a004_legendary.tex";
    return "items/misc/blueprint01.tex";
}
bool hasTag(const std::unordered_map<std::string, std::string>& tags, const std::string& t) {
    return tags.find(t) != tags.end();
}
std::string tagText(const std::unordered_map<std::string, std::string>& tags, const std::string& t) {
    auto it = tags.find(t);
    return it == tags.end() ? std::string() : it->second;
}

bool isJunk(const std::string& key) {
    for (const char* p : kJunkPrefix) if (startsWith(key, p)) return true;
    return key.find(kJunkPart) != std::string::npos;
}

// The shipped rules, the same for every layer: not in a junk folder (isJunk), no BLANK in the
// FileDescription (the designer templates), and a name tag the view's text resolves (a
// blueprint's: the crafted item's).
bool isShipped(const std::string& key, const ArzRecord& rec, const std::string& cls,
               const GameData& g) {
    if (isJunk(key)) return false;
    std::string fd = rec.str("FileDescription");
    for (char& c : fd) if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    if (fd.find("BLANK") != std::string::npos) return false;
    if (hasTag(g.tags, nameTagOf(rec, cls))) return true;
    std::string tgt = craftedRecord(rec, cls);
    if (!tgt.empty()) {
        ArzRecord trec;
        if (g.record(tgt, trec) && hasTag(g.tags, nameTagOf(trec, trec.str("Class")))) return true;
    }
    return false;
}

// 'items/<rest>' -> Items.arc entry '<rest>'; 'ui/<rest>' -> UI.arc entry '<rest>';
// 'level art/<rest>' -> Level Art.arc entry '<rest>' (the engine's rule: the first segment names
// the archive).
bool texExists(const GameData& g, const std::string& bitmap) {
    if (bitmap.empty()) return false;
    std::string b = bitmap;
    for (char& c : b) if (c == '\\') c = '/';
    std::size_t s = b.find('/');
    std::string head = s == std::string::npos ? b : b.substr(0, s);
    std::string rest = s == std::string::npos ? std::string() : b.substr(s + 1);
    for (char& c : head) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    if (head == "items" && g.items->contains(rest)) return true;
    if (head == "ui" && g.ui->contains(rest)) return true;
    if (head == "level art" && g.levelArt && g.levelArt->contains(rest)) return true;
    int w = 0, h = 0;
    return g.layered() && g.view->texSize(bitmap, w, h);
}

bool storableClassification(const std::string& c) {
    return c == "Common" || c == "Rare" || c == "Epic" || c == "Legendary";
}
// What catalogue.bin can hold for a record of this class: a blueprint is stored at Magical and
// Quest too (both packed as Epic, like a Rare relic - packCatalogue): a recipe is a recipe, and
// Reign of Terror's 170 runeword-craft blueprints are Quest-classified.
bool storable(const std::string& cls, const std::string& c) {
    return storableClassification(c) || (cls == kFormulaClass && (c == "Magical" || c == "Quest"));
}

// A mod's text archive for a language: text_<lang>*.arc or *_text_<lang>*.arc, any case.
bool isTextArc(const std::string& path, const std::string& lang) {
    std::size_t cut = path.find_last_of("\\/");
    std::string leaf = cut == std::string::npos ? path : path.substr(cut + 1);
    for (char& c : leaf) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    std::string want = "text_" + lang;
    for (char& c : want) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    if (!endsWith(leaf, ".arc")) return false;
    return startsWith(leaf, want.c_str()) || leaf.find("_" + want) != std::string::npos;
}

} // namespace

// ---------------------------------------------------------------- monster infrequents
bool isEquipmentClass(const std::string& cls) {
    if (equipSlot(cls)) return true;
    for (const char* c : kPerClassEquipment) if (cls == c) return true;
    return false;
}

bool isMiCandidate(const std::string& /*key*/, const std::string& cls,
                   const std::string& classification, const std::string& /*fileDescription*/) {
    // Every Rare record of the equipment classes - the faction-store and the blacksmith-crafted
    // rares included, by design: the tab collects every Rare a player can own. The key and the
    // FileDescription stay in the signature: the callers pass them, and a later record-local rule
    // needs no caller change.
    return classification == "Rare" && isEquipmentClass(cls);
}

// ---------------------------------------------------------------- GameData
GameData::~GameData() {
    delete db;
    delete items;
    delete ui;
    delete levelArt;
}

bool GameData::layered() const { return view && !view->empty(); }

bool GameData::record(const std::string& name, ArzRecord& out) const {
    if (layered() && view->get(name, out)) return true;
    return db && db->get(name, out);
}

bool GameData::load(const std::string& gameDir, const std::string& lang, std::string* error,
                    const ModView* layers) {
    delete db; delete items; delete ui; delete levelArt;
    db = new ArzDatabase;
    items = new ArcSet;
    ui = new ArcSet;
    levelArt = nullptr;
    view = layers;
    tags.clear();
    modTags = 0;
    if (!db->loadGame(gameDir, error)) return false;
    if (!loadTextTags(gameDir, lang, tags, error)) return false;
    if (!items->load(gameDir, "resources\\Items.arc", error)) return false;
    if (!ui->load(gameDir, "resources\\UI.arc", error)) return false;
    // optional: a game folder without it (a trimmed copy) only leaves those few icons unfound
    levelArt = new ArcSet;
    if (!levelArt->load(gameDir, "resources\\Level Art.arc", nullptr)) { delete levelArt; levelArt = nullptr; }
    if (!layered()) return true;
    // The mod's own names on top of the base game's, in its load order (the last file wins):
    // its English set first, then its set in this language over it, so a tag the translation
    // lacks keeps its English name instead of dropping the item.
    std::vector<const ArcArchive*> text;
    for (const ArcArchive& a : view->arcLayers()) if (isTextArc(a.path(), "en")) text.push_back(&a);
    if (!isTextArc("text_en.arc", lang))
        for (const ArcArchive& a : view->arcLayers()) if (isTextArc(a.path(), lang)) text.push_back(&a);
    std::unordered_map<std::string, std::string> mod;
    std::vector<std::uint8_t> data;
    for (const ArcArchive* a : text)
        for (const ArcEntry& e : a->entries()) {
            if (e.key.size() < 4 || e.key.compare(e.key.size() - 4, 4, ".txt") != 0) continue;
            if (a->read(e, data) && !data.empty()) parseTagFile(data, mod);
        }
    modTags = mod.size();
    for (auto& kv : mod) tags[kv.first] = std::move(kv.second);
    return true;
}

std::map<std::string, std::string> classWords(const GameData& g) {
    std::map<std::string, std::string> out;
    std::size_t n = 0;
    const utg::ClassTag* t = utg::classTypeTags(&n);
    for (std::size_t i = 0; i < n; ++i) {
        auto it = g.tags.find(t[i].tag);
        if (it != g.tags.end() && !it->second.empty()) out[t[i].cls] = it->second;
    }
    return out;
}

std::string stripColourCodes(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '{' && i + 3 < s.size() && s[i + 1] == '^' && s[i + 3] == '}') { i += 3; continue; }
        if (s[i] == '^' && i + 1 < s.size()) { ++i; continue; }
        o += s[i];
    }
    return o;
}

// ---------------------------------------------------------------- the collection
bool collectItems(const GameData& g, std::vector<CatalogueItem>& items,
                  std::vector<std::string>& warnings, std::string* error,
                  bool collectMi) {
    items.clear();
    if (!g.db || !g.items || !g.ui) { if (error) *error = "game data not loaded"; return false; }
    std::map<std::string, std::string> setNames;
    auto setDisplayName = [&](const std::string& setRec) -> std::string {
        if (setRec.empty()) return std::string();
        auto it = setNames.find(setRec);
        if (it != setNames.end()) return it->second;
        std::string nm;
        ArzRecord sr;
        if (g.record(setRec, sr)) {
            nm = tagText(g.tags, sr.str("setName"));
            if (nm.empty()) nm = tagText(g.tags, sr.str("description"));
        }
        setNames[setRec] = nm;
        return nm;
    };
    std::vector<char> modLayerOf;   // per item: a mod layer won the record
    auto makeEntry = [&](const std::string& key, const ArzEntry& e, const std::string& src,
                         const ArzRecord& rec, const std::string& cls, bool isExtra,
                         bool modLayer) {
        CatalogueItem it;
        it.record = e.name;
        it.cls = cls;
        const char* s = equipSlot(cls);
        if (!s) s = otherSlot(cls);
        it.slot = s ? s : "other";
        it.craftsRecord = craftedRecord(rec, cls);
        it.nameTag = nameTagOf(rec, cls);
        it.name = tagText(g.tags, it.nameTag);
        it.bitmap = bitmapOf(rec, cls);
        if (it.bitmap.empty() && cls == kFormulaClass) it.bitmap = scrollOf(rec.str("itemClassification"));
        if (!it.craftsRecord.empty()) {
            ArzRecord trec;
            if (g.record(it.craftsRecord, trec)) {
                std::string tcls = trec.str("Class");
                std::string ttag = nameTagOf(trec, tcls);
                if (it.name.empty()) {
                    it.name = tagText(g.tags, ttag);
                    if (it.nameTag.empty()) it.nameTag = ttag;
                }
                if (it.bitmap.empty()) it.bitmap = bitmapOf(trec, tcls);
            }
        }
        it.bitmapFound = texExists(g, it.bitmap);
        it.classification = rec.str("itemClassification");
        it.setRecord = rec.str("itemSetName");
        it.setDisplayName = setDisplayName(it.setRecord);
        it.levelRequirement = rec.i32("levelRequirement");
        it.itemLevel = rec.i32("itemLevel");
        it.source = src;
        it.isBlueprint = endsWith(cls, "Formula") || key.find("/crafting/blueprints/") != std::string::npos;
        it.isAugment = cls == "ItemEnchantment";
        it.isRelic = cls == kRelicClass;
        it.isSetPiece = !it.setRecord.empty();
        it.isEquipment = equipSlot(cls) != nullptr;
        it.isMi = isEquipmentClass(cls) && it.classification == "Rare";   // the KIND mark
        it.isExtra = isExtra;
        it.fromMod = modLayer && !g.db->lookup(key, nullptr, nullptr);
        items.push_back(std::move(it));
        modLayerOf.push_back(modLayer ? 1 : 0);
    };

    std::size_t relicRare = 0, relicEpic = 0, relicLeg = 0, relicOther = 0, extras = 0;
    std::size_t unstorable = 0;
    std::string unstorableSample;
    ArzRecord rec;
    // One record through the rule. modLayer: a mod layer won it, so the mod rules apply - an
    // unknown class as "other" (Epic/Legendary only), and a classification catalogue.bin cannot
    // store skipped instead of failing the generation. The path rule (the junk list) is every
    // layer's.
    auto consider = [&](const ArzArchive& a, const ArzEntry& e, bool modLayer) -> bool {
        const std::string& key = e.key;
        if (inList(key, kExtraExclude, sizeof kExtraExclude / sizeof *kExtraExclude)) return true;
        bool isExtra = inList(key, kExtraRecords, sizeof kExtraRecords / sizeof *kExtraRecords);
        if (!a.decode(e, rec)) {
            if (error) *error = "record " + e.name + " in " + a.path() + " does not decode";
            return false;
        }
        std::string cls = rec.str("Class");
        const bool known = equipSlot(cls) || otherSlot(cls);
        if (!known && (!modLayer || cls.empty())) return true;
        if (cls == "LootRandomizer") return true;
        if (isBlueprintClass(cls) && cls != kFormulaClass) return true;   // not recipes
        std::string cla = rec.str("itemClassification");
        bool isRelic = cls == kRelicClass;
        const bool isFormula = cls == kFormulaClass;
        bool epicLeg = cla == "Epic" || cla == "Legendary";
        // a mod's own augments and components (no base archive has them: DGD's augments, the
        // D2 gems and runes of ROT and DGD) are collectible at every classification, like relics
        const bool modAugment = modLayer && (cls == "ItemEnchantment" || cls == "ItemRelic") &&
                                !g.db->lookup(key, nullptr, nullptr);
        if (!isRelic && !isExtra && !modAugment && !isFormula && !epicLeg && cla != "Rare") return true;
        bool admit = false;
        if (isExtra) {
            admit = true;
        } else if (isRelic || epicLeg || modAugment || isFormula) {
            admit = isShipped(key, rec, cls, g);
        } else if (collectMi && isMiCandidate(key, cls, cla, rec.str("FileDescription"))) {
            // a monster infrequent (collect_mi=1): the same shipped filter / mod rule
            admit = isShipped(key, rec, cls, g);
        }
        // Without collect_mi, Rare equipment (monster infrequents, faction and crafted rares) is
        // not part of the collection; with it (the default), every such record is.
        if (!admit) return true;
        // (a base blueprint too: every classification is admitted, so one catalogue.bin cannot
        // store - Quest - must be skipped here, never fail the whole generation in packCatalogue)
        if ((modLayer || isFormula) && !storable(cls, cla)) {
            if (unstorable++ < 3) unstorableSample += (unstorableSample.empty() ? "" : ", ") + e.name + " (" + cla + ")";
            return true;
        }
        makeEntry(key, e, a.tag(), rec, cls, isExtra, modLayer);
        if (isExtra) {
            ++extras;
        } else if (isRelic && !modLayer) {
            if (cla == "Rare") ++relicRare;
            else if (cla == "Epic") ++relicEpic;
            else if (cla == "Legendary") ++relicLeg;
            else ++relicOther;
        }
        return true;
    };

    // The mod layers' winners: the last layer that has a record wins it.
    const std::vector<ArzArchive>* layers = g.layered() ? &g.view->arzLayers() : nullptr;
    auto modHas = [&](const std::string& key) {
        if (!layers) return false;
        for (const ArzArchive& l : *layers) if (l.entry(key)) return true;
        return false;
    };
    for (const auto& w : g.db->winners()) {
        if (modHas(w.second->key)) continue;
        if (!consider(g.db->archives()[w.first], *w.second, false)) return false;
    }
    if (layers) {
        for (std::size_t li = 0; li < layers->size(); ++li) {
            const ArzArchive& a = (*layers)[li];
            for (const ArzEntry& e : a.entries()) {
                bool later = false;
                for (std::size_t lj = li + 1; lj < layers->size() && !later; ++lj)
                    later = (*layers)[lj].entry(e.key) != nullptr;
                if (later) continue;
                if (!consider(a, e, true)) return false;
            }
        }
    }
    if (unstorable)
        warnings.push_back(std::to_string(unstorable) + " mod record(s) of a classification the "
                           "catalogue cannot store are skipped: " + unstorableSample +
                           (unstorable > 3 ? ", ..." : ""));

    // The checks build_catalogue.py asserts, reported rather than fatal: a game patch may
    // legitimately move these numbers.
    for (std::size_t ii = 0; ii < items.size(); ++ii) {
        const CatalogueItem& it = items[ii];
        if (modLayerOf[ii]) continue;   // the base game's checks
        std::string low = ArzArchive::normKey(it.record);
        if (it.isRelic && !startsWith(low, kRelicPath))
            warnings.push_back("relic outside " + std::string(kRelicPath) + ": " + it.record);
        if (it.isRelic && !it.bitmapFound)
            warnings.push_back("relic bitmap missing: " + it.record);
        if (it.isExtra && it.name.empty())
            warnings.push_back("curated extra has no name: " + it.record);
        if (it.isExtra && !it.bitmapFound)
            warnings.push_back("curated extra bitmap missing: " + it.record);
    }
    if (extras != sizeof kExtraRecords / sizeof *kExtraRecords)
        warnings.push_back("curated extras found: " + std::to_string(extras) + " of "
                           + std::to_string(sizeof kExtraRecords / sizeof *kExtraRecords));
    if (relicOther)
        warnings.push_back("relics of an unexpected classification: " + std::to_string(relicOther));
    return true;
}

// ---------------------------------------------------------------- packing
int slotGroupOf(const std::string& slot) {
    static const char* const kSlots[] = {
        "head", "shoulders", "chest", "hands", "waist", "legs", "feet", "axe1h", "mace1h",
        "sword1h", "dagger", "scepter", "axe2h", "mace2h", "sword2h", "spear2h", "ranged1h",
        "ranged2h", "offhand", "shield", "ring", "amulet", "medal", "relic", "blueprint",
        "augment", "consumable", "quest",
    };
    for (int i = 0; i < int(sizeof kSlots / sizeof *kSlots); ++i) if (slot == kSlots[i]) return i;
    return 28;
}

namespace {

const std::uint32_t kFormatVersion = 1, kHeaderSize = 80, kItemEntrySize = 24, kSetEntrySize = 16;
const std::uint32_t kGroupCount = 29;
enum : std::uint16_t {
    kFSetPiece = 1, kFBlueprint = 2, kFAugment = 4, kFRelic = 8, kFFaction = 16,
    kFEquipment = 32, kFDefaultVisible = 64, kFHasBitmap = 128,
    kFMi = 256,   // the KIND mark: a monster infrequent (model/catalogue.h ItemFlag::MonsterInfrequent)
};

struct StringTable {
    std::vector<std::uint8_t> blob{0};
    std::unordered_map<std::string, std::uint32_t> refs{{std::string(), 0}};
    std::uint32_t add(const std::string& s) {
        auto it = refs.find(s);
        if (it != refs.end()) return it->second;
        std::uint32_t ref = std::uint32_t(blob.size());
        blob.insert(blob.end(), s.begin(), s.end());
        blob.push_back(0);
        refs.emplace(s, ref);
        return ref;
    }
};

void put32(std::vector<std::uint8_t>& o, std::uint32_t v) {
    o.push_back(std::uint8_t(v)); o.push_back(std::uint8_t(v >> 8));
    o.push_back(std::uint8_t(v >> 16)); o.push_back(std::uint8_t(v >> 24));
}
void put16(std::vector<std::uint8_t>& o, std::uint16_t v) {
    o.push_back(std::uint8_t(v)); o.push_back(std::uint8_t(v >> 8));
}

bool clampU16(std::int32_t v, std::uint16_t& out) {
    if (v < 0) v = 0;
    if (v > 0xFFFF) return false;
    out = std::uint16_t(v);
    return true;
}

} // namespace

bool packCatalogue(std::vector<CatalogueItem>& items, std::vector<std::uint8_t>& out,
                   CatalogueStats* stats, std::string* error) {
    out.clear();
    struct Row {
        int grp; std::uint16_t lvl; const CatalogueItem* it; std::string key;
    };
    // (the KIND first inside a group: the uniques, then the monster infrequents)
    std::vector<Row> rows;
    rows.reserve(items.size());
    for (const CatalogueItem& it : items) {
        Row r{slotGroupOf(it.slot), 0, &it, stripColourCodes(it.name)};
        if (!clampU16(it.levelRequirement, r.lvl)) {
            if (error) *error = "levelRequirement out of range on " + it.record;
            return false;
        }
        rows.push_back(r);
    }
    // slot group, kind (an MI after the uniques), level requirement, name, record - byte order on
    // UTF-8 is code point order. With no MI admitted the kind never decides: today's order.
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        if (a.grp != b.grp) return a.grp < b.grp;
        if (a.it->isMi != b.it->isMi) return b.it->isMi;
        if (a.lvl != b.lvl) return a.lvl < b.lvl;
        int c = a.key.compare(b.key);
        if (c != 0) return c < 0;
        return a.it->record < b.it->record;
    });
    for (std::size_t i = 1; i < rows.size(); ++i)
        if (rows[i].it->record == rows[i - 1].it->record) {
            if (error) *error = "duplicate record " + rows[i].it->record;
            return false;
        }

    std::set<std::string> setRecs;
    for (const Row& r : rows) if (!r.it->setRecord.empty()) setRecs.insert(r.it->setRecord);
    std::vector<std::string> setList(setRecs.begin(), setRecs.end());
    std::unordered_map<std::string, std::uint32_t> setIndex;
    for (std::uint32_t i = 0; i < setList.size(); ++i) setIndex[setList[i]] = i;
    std::vector<std::vector<std::uint32_t>> setMembers(setList.size());
    std::unordered_map<std::string, std::string> setDisplay;
    for (std::uint32_t i = 0; i < rows.size(); ++i) {
        const CatalogueItem& it = *rows[i].it;
        if (it.setRecord.empty()) continue;
        setMembers[setIndex[it.setRecord]].push_back(i);
        setDisplay.emplace(it.setRecord, it.setDisplayName);
    }

    StringTable st;
    struct ItemRow {
        std::uint32_t rec, name, bmp; std::int32_t set; std::uint8_t cls, grp;
        std::uint16_t lvl, ilvl, flags;
    };
    std::vector<ItemRow> itemRows;
    itemRows.reserve(rows.size());
    for (const Row& r : rows) {
        const CatalogueItem& it = *r.it;
        std::uint8_t cls;
        if (it.classification == "Epic" || it.classification == "Rare"
            || it.classification == "Common" || it.classification == "Magical"
            || (it.classification == "Quest" && it.cls == kFormulaClass)) cls = 0;
        else if (it.classification == "Legendary") cls = 1;
        else { if (error) *error = "unexpected itemClassification '" + it.classification + "' on " + it.record; return false; }
        ItemRow row;
        row.rec = st.add(it.record);
        row.name = st.add(it.name);
        row.bmp = st.add(it.bitmap);
        row.set = it.setRecord.empty() ? -1 : std::int32_t(setIndex[it.setRecord]);
        row.cls = cls;
        row.grp = std::uint8_t(r.grp);
        row.lvl = r.lvl;
        if (!clampU16(it.itemLevel, row.ilvl)) { if (error) *error = "itemLevel out of range on " + it.record; return false; }
        std::uint16_t f = 0;
        if (it.isSetPiece) f |= kFSetPiece;
        if (it.isBlueprint) f |= kFBlueprint;
        if (it.isAugment) f |= kFAugment;
        if (it.isRelic) f |= kFRelic;
        if (startsWith(it.record, "records/items/faction/")) f |= kFFaction;
        if (it.isEquipment) f |= kFEquipment;
        if (it.isEquipment || it.isRelic) f |= kFDefaultVisible;
        if (!it.bitmap.empty()) f |= kFHasBitmap;
        if (it.isMi) f |= kFMi;
        row.flags = f;
        itemRows.push_back(row);
    }
    struct SetRow { std::uint32_t name, rec, first, count; };
    std::vector<SetRow> setRows;
    std::vector<std::uint32_t> members;
    for (std::uint32_t si = 0; si < setList.size(); ++si) {
        std::uint32_t first = std::uint32_t(members.size());
        members.insert(members.end(), setMembers[si].begin(), setMembers[si].end());
        setRows.push_back({st.add(setDisplay[setList[si]]), st.add(setList[si]), first,
                           std::uint32_t(setMembers[si].size())});
    }
    std::uint32_t groupFirst[kGroupCount] = {}, groupCount[kGroupCount] = {};
    for (std::uint32_t i = 0; i < itemRows.size(); ++i) {
        std::uint8_t gi = itemRows[i].grp;
        if (groupCount[gi] == 0) groupFirst[gi] = i;
        ++groupCount[gi];
    }
    std::uint32_t itemCount = std::uint32_t(itemRows.size()), setCount = std::uint32_t(setRows.size());
    std::uint32_t memberCount = std::uint32_t(members.size());
    std::uint32_t groupOff = kHeaderSize, itemOff = groupOff + kGroupCount * 8;
    std::uint32_t setOff = itemOff + itemCount * kItemEntrySize;
    std::uint32_t memberOff = setOff + setCount * kSetEntrySize;
    std::uint32_t stringOff = memberOff + memberCount * 4;
    std::uint32_t stringSize = std::uint32_t(st.blob.size());
    std::uint32_t total = stringOff + stringSize;
    if (total % 4) total += 4 - total % 4;
    std::uint32_t equipment = 0, relics = 0, visible = 0;
    for (const ItemRow& r : itemRows) {
        if (r.flags & kFEquipment) ++equipment;
        if (r.flags & kFRelic) ++relics;
        if (r.flags & kFDefaultVisible) ++visible;
    }
    out.reserve(total);
    out.insert(out.end(), {'G', 'D', 'U', 'T'});
    const std::uint32_t hdr[19] = {
        kFormatVersion, kHeaderSize, 0, itemCount, kItemEntrySize, itemOff, setCount,
        kSetEntrySize, setOff, memberCount, memberOff, kGroupCount, groupOff, stringSize,
        stringOff, total, visible, equipment, relics,
    };
    for (std::uint32_t v : hdr) put32(out, v);
    for (std::uint32_t gi = 0; gi < kGroupCount; ++gi) { put32(out, groupFirst[gi]); put32(out, groupCount[gi]); }
    for (const ItemRow& r : itemRows) {
        put32(out, r.rec); put32(out, r.name); put32(out, r.bmp); put32(out, std::uint32_t(r.set));
        out.push_back(r.cls); out.push_back(r.grp);
        put16(out, r.lvl); put16(out, r.ilvl); put16(out, r.flags);
    }
    for (const SetRow& r : setRows) { put32(out, r.name); put32(out, r.rec); put32(out, r.first); put32(out, r.count); }
    for (std::uint32_t m : members) put32(out, m);
    out.insert(out.end(), st.blob.begin(), st.blob.end());
    while (out.size() % 4) out.push_back(0);
    if (stats) {
        stats->items = itemCount; stats->sets = setCount; stats->members = memberCount;
        stats->strings = st.refs.size(); stats->stringBytes = stringSize;
        stats->equipment = equipment; stats->relics = relics; stats->defaultVisible = visible;
        stats->bytes = out.size();
        stats->relicsRare = stats->relicsEpic = stats->relicsLegendary = stats->extras = 0;
        stats->mi = 0;
        for (const CatalogueItem& it : items) {
            if (it.isExtra) ++stats->extras;
            if (it.isMi) ++stats->mi;
            if (!it.isRelic) continue;
            if (it.classification == "Rare") ++stats->relicsRare;
            else if (it.classification == "Epic") ++stats->relicsEpic;
            else if (it.classification == "Legendary") ++stats->relicsLegendary;
        }
    }
    return true;
}

} // namespace gen
