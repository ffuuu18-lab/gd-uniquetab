// catalogue_gen.h - builds catalogue.bin from the game's own archives.
//
// Port of tools/build_catalogue.py (the shipped-record rules, the collection rule, the
// curated lists, names from Text_<lang>.arc, the bitmap check) and tools/pack_catalogue.py
// (the deterministic "GDUT" v1 layout). The Python tools stay the reference: for the same
// game files this produces the same bytes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace gen {

class ArzDatabase;
class ArcSet;

// One accepted record, the fields catalogue.json carries.
struct CatalogueItem {
    std::string record;            // real path (original case, '/' separators)
    std::string cls;               // Class
    std::string slot;              // head, ring, relic, blueprint ... or "other"
    std::string craftsRecord;
    std::string nameTag;
    std::string name;              // UTF-8 display name, "" when the tag has no text
    std::string classification;    // itemClassification word
    std::string setRecord;         // itemSetName
    std::string setDisplayName;
    std::string bitmap;
    std::string source;            // archive tag
    std::int32_t levelRequirement = 0;
    std::int32_t itemLevel = 0;
    bool bitmapFound = false;
    bool isBlueprint = false, isAugment = false, isRelic = false, isSetPiece = false;
    bool isEquipment = false, isExtra = false;
};

struct CatalogueStats {
    std::size_t items = 0, sets = 0, members = 0, strings = 0, stringBytes = 0;
    std::size_t equipment = 0, relics = 0, defaultVisible = 0, bytes = 0;
    std::size_t relicsRare = 0, relicsEpic = 0, relicsLegendary = 0, extras = 0;
};

// The game data one generation reads; loaded once, shared by the catalogue and the pages.
struct GameData {
    bool load(const std::string& gameDir, const std::string& lang, std::string* error);
    ArzDatabase* db = nullptr;                                   // owned
    ArcSet* items = nullptr;                                     // owned
    ArcSet* ui = nullptr;                                        // owned
    std::unordered_map<std::string, std::string> tags;
    GameData() = default;
    ~GameData();
    GameData(const GameData&) = delete;
    GameData& operator=(const GameData&) = delete;
};

// The collection: every record build_catalogue.py puts in catalogue.json's "items".
// `warnings` receives the checks the Python tool asserts (they do not stop a generation).
bool collectItems(const GameData& g, std::vector<CatalogueItem>& items,
                  std::vector<std::string>& warnings, std::string* error);

// pack_catalogue.py: items -> catalogue.bin bytes. False (with the reason) on bad input.
bool packCatalogue(std::vector<CatalogueItem>& items, std::vector<std::uint8_t>& out,
                   CatalogueStats* stats, std::string* error);

// Slot word -> the on-disk group index of model/catalogue.h (28 = Other).
int slotGroupOf(const std::string& slot);

} // namespace gen
