// catalogue_gen.cpp - see catalogue_gen.h.
#include "gen/catalogue_gen.h"
#include "gen/arc_reader.h"
#include "gen/arz_reader.h"

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
const char* const kRelicPath = "records/items/gearrelic/";

// The curated inclusion list: records the four shipped rules drop but a player really owns.
const char* const kExtraRecords[] = {
    "records/endlessdungeon/items/a001_ring.dbr",       // Leovinus' Ring
};
// The developer test records that share that folder; never collectible.
const char* const kExtraExclude[] = {
    "records/endlessdungeon/items/z001_test.dbr",
    "records/endlessdungeon/items/q001_torso.dbr",
};

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
std::string bitmapOf(const ArzRecord& rec) {
    std::string v = rec.str("bitmap");
    if (v.empty()) v = rec.str("artifactBitmap");
    if (v.empty()) v = rec.str("relicBitmap");
    return v;
}
bool hasTag(const std::unordered_map<std::string, std::string>& tags, const std::string& t) {
    return tags.find(t) != tags.end();
}
std::string tagText(const std::unordered_map<std::string, std::string>& tags, const std::string& t) {
    auto it = tags.find(t);
    return it == tags.end() ? std::string() : it->second;
}

bool isShipped(const std::string& key, const ArzRecord& rec, const std::string& cls,
               const GameData& g) {
    if (!startsWith(key, "records/items/")) return false;
    if (startsWith(key, "records/items/enemygear/")) return false;
    std::string fd = rec.str("FileDescription");
    for (char& c : fd) if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    if (fd.find("BLANK") != std::string::npos) return false;
    if (hasTag(g.tags, nameTagOf(rec, cls))) return true;
    std::string tgt = craftedRecord(rec, cls);
    if (!tgt.empty()) {
        ArzRecord trec;
        if (g.db->get(tgt, trec) && hasTag(g.tags, nameTagOf(trec, trec.str("Class")))) return true;
    }
    return false;
}

// 'items/<rest>' -> Items.arc entry '<rest>'; 'ui/<rest>' -> UI.arc entry '<rest>'.
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
    return false;
}

} // namespace

// ---------------------------------------------------------------- GameData
GameData::~GameData() {
    delete db;
    delete items;
    delete ui;
}

bool GameData::load(const std::string& gameDir, const std::string& lang, std::string* error) {
    delete db; delete items; delete ui;
    db = new ArzDatabase;
    items = new ArcSet;
    ui = new ArcSet;
    tags.clear();
    if (!db->loadGame(gameDir, error)) return false;
    if (!loadTextTags(gameDir, lang, tags, error)) return false;
    if (!items->load(gameDir, "resources\\Items.arc", error)) return false;
    if (!ui->load(gameDir, "resources\\UI.arc", error)) return false;
    return true;
}

// ---------------------------------------------------------------- the collection
bool collectItems(const GameData& g, std::vector<CatalogueItem>& items,
                  std::vector<std::string>& warnings, std::string* error) {
    items.clear();
    if (!g.db || !g.items || !g.ui) { if (error) *error = "game data not loaded"; return false; }
    std::map<std::string, std::string> setNames;
    auto setDisplayName = [&](const std::string& setRec) -> std::string {
        if (setRec.empty()) return std::string();
        auto it = setNames.find(setRec);
        if (it != setNames.end()) return it->second;
        std::string nm;
        ArzRecord sr;
        if (g.db->get(setRec, sr)) {
            nm = tagText(g.tags, sr.str("setName"));
            if (nm.empty()) nm = tagText(g.tags, sr.str("description"));
        }
        setNames[setRec] = nm;
        return nm;
    };
    auto makeEntry = [&](const std::string& key, const ArzEntry& e, const std::string& src,
                         const ArzRecord& rec, const std::string& cls, bool isExtra) {
        CatalogueItem it;
        it.record = e.name;
        it.cls = cls;
        const char* s = equipSlot(cls);
        if (!s) s = otherSlot(cls);
        it.slot = s ? s : "other";
        it.craftsRecord = craftedRecord(rec, cls);
        it.nameTag = nameTagOf(rec, cls);
        it.name = tagText(g.tags, it.nameTag);
        it.bitmap = bitmapOf(rec);
        if (!it.craftsRecord.empty()) {
            ArzRecord trec;
            if (g.db->get(it.craftsRecord, trec)) {
                std::string tcls = trec.str("Class");
                std::string ttag = nameTagOf(trec, tcls);
                if (it.name.empty()) {
                    it.name = tagText(g.tags, ttag);
                    if (it.nameTag.empty()) it.nameTag = ttag;
                }
                if (it.bitmap.empty()) it.bitmap = bitmapOf(trec);
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
        it.isExtra = isExtra;
        items.push_back(std::move(it));
    };

    std::size_t relicRare = 0, relicEpic = 0, relicLeg = 0, relicOther = 0, extras = 0;
    ArzRecord rec;
    for (const auto& w : g.db->winners()) {
        const ArzArchive& a = g.db->archives()[w.first];
        const ArzEntry& e = *w.second;
        const std::string& key = e.key;
        if (inList(key, kExtraExclude, sizeof kExtraExclude / sizeof *kExtraExclude)) continue;
        bool isExtra = inList(key, kExtraRecords, sizeof kExtraRecords / sizeof *kExtraRecords);
        if (!a.decode(e, rec)) {
            if (error) *error = "record " + e.name + " in " + a.path() + " does not decode";
            return false;
        }
        std::string cls = rec.str("Class");
        if (!equipSlot(cls) && !otherSlot(cls)) continue;
        if (cls == "LootRandomizer") continue;
        std::string cla = rec.str("itemClassification");
        bool isRelic = cls == kRelicClass;
        bool epicLeg = cla == "Epic" || cla == "Legendary";
        if (!isRelic && !isExtra && !epicLeg && cla != "Rare") continue;
        bool shipped = isShipped(key, rec, cls, g);
        if (isExtra) {
            makeEntry(key, e, a.tag(), rec, cls, true);
            ++extras;
        } else if (isRelic) {
            if (!shipped) continue;
            makeEntry(key, e, a.tag(), rec, cls, false);
            if (cla == "Rare") ++relicRare;
            else if (cla == "Epic") ++relicEpic;
            else if (cla == "Legendary") ++relicLeg;
            else ++relicOther;
        } else if (epicLeg) {
            if (shipped) makeEntry(key, e, a.tag(), rec, cls, false);
        }
        // Rare equipment (monster infrequents, faction items) is not part of the collection.
    }

    // The checks build_catalogue.py asserts, reported rather than fatal: a game patch may
    // legitimately move these numbers.
    for (const CatalogueItem& it : items) {
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
        int grp; std::uint16_t lvl; const CatalogueItem* it;
    };
    std::vector<Row> rows;
    rows.reserve(items.size());
    for (const CatalogueItem& it : items) {
        Row r{slotGroupOf(it.slot), 0, &it};
        if (!clampU16(it.levelRequirement, r.lvl)) {
            if (error) *error = "levelRequirement out of range on " + it.record;
            return false;
        }
        rows.push_back(r);
    }
    // slot group, level requirement, name, record - byte order on UTF-8 is code point order
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        if (a.grp != b.grp) return a.grp < b.grp;
        if (a.lvl != b.lvl) return a.lvl < b.lvl;
        int c = a.it->name.compare(b.it->name);
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
            || it.classification == "Common") cls = 0;
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
        for (const CatalogueItem& it : items) {
            if (it.isExtra) ++stats->extras;
            if (!it.isRelic) continue;
            if (it.classification == "Rare") ++stats->relicsRare;
            else if (it.classification == "Epic") ++stats->relicsEpic;
            else if (it.classification == "Legendary") ++stats->relicsLegendary;
        }
    }
    return true;
}

} // namespace gen
