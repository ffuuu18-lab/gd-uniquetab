// catalogue_gen.h - builds catalogue.bin from the game's own archives.
//
// Port of tools/build_catalogue.py (the shipped-record rules, the collection rule, the
// curated lists, names from Text_<lang>.arc, the bitmap check) and tools/pack_catalogue.py
// (the deterministic "GDUT" v1 layout). The Python tools stay the reference: for the same
// game files this produces the same bytes.
//
// The catalogue is built from the VIEW: the base game's four databases plus the override layers
// a ModView carries (/basemods, a custom game), last layer wins - the database the engine will
// have. A record a mod layer wins is judged by the mod rules: a junk list instead of the
// records/items/ path rule, a class the slot table does not know admitted as slot "other"
// (Epic/Legendary only), a mod's own augments and components (ItemEnchantment, ItemRelic) at every
// classification, and a classification catalogue.bin cannot store skipped with a warning.
// Blueprints (ItemArtifactFormula, never the five other *Formula classes) are collected at every
// classification in base and mod alike (Magical stored, packed as Epic), with their own scroll
// icon (artifactFormulaBitmapName; the crafted item's only when that is empty). Every other base
// record keeps the 1.1.0 rules exactly, so a view that overrides no item record writes the
// vanilla bytes. Names come from the base Text_<lang>.arc, then the mod's text_en*.arc, then its
// text_<lang>*.arc over that; icons from the mod's archives or the base Items/UI ones.
//
// MONSTER INFREQUENTS (collect_mi=1, the default). The engine's own MI rule
// (Item::GetSymbolBitmapName, Game.dll 0x3115F0): an equipment record whose OWN
// itemClassification is Rare - random rares have no record. With the flag on, every such record of
// the 25 equipment classes (the 23 slot classes and the two per-class groups' WeaponMagical_Staff
// and WeaponHunting_Spear, which only a mod layer has) is admitted (base: the shipped filter; a mod
// layer's: the mod rule), the faction-store rares (vanilla's records/items/faction/) and the
// blacksmith-crafted ones included - isMiCandidate below, which the view probe (ut_viewprobe.h)
// calls too. Each one is stored as Rare (packed as Epic, like a Rare relic) with the KIND mark
// isMi (catalogue.bin flag bit 8, model/catalogue.h catIsMi), sorted after the uniques of its
// group. With the flag off no MI is admitted and every byte is what it was - an MI you stored
// keeps its journal row and is simply not in the catalogue (the label counts it as not on these
// pages) until the flag is 1 again.
// The per-game junk folders a mod keeps MI-classified records in are [exclude] blocks of the
// built-in groups (ut_groups_default.h, data), not rules here.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gen {

class ArzDatabase;
class ArcSet;
class ModView;
struct ArzRecord;

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
    bool isMi = false;             // KIND mark: a Rare equipment record (a monster infrequent)
    bool fromMod = false;          // only a mod layer has the record (no base archive does)
};

struct CatalogueStats {
    std::size_t items = 0, sets = 0, members = 0, strings = 0, stringBytes = 0;
    std::size_t equipment = 0, relics = 0, defaultVisible = 0, bytes = 0;
    std::size_t relicsRare = 0, relicsEpic = 0, relicsLegendary = 0, extras = 0;
    std::size_t mi = 0;            // entries with the KIND mark MI (flag bit 8)
};

// The game data one generation reads; loaded once, shared by the catalogue and the pages.
// view: the override layers (not owned, must outlive this); null or empty = the base game.
struct GameData {
    bool load(const std::string& gameDir, const std::string& lang, std::string* error,
              const ModView* view = nullptr);
    ArzDatabase* db = nullptr;                                   // owned: the base game
    ArcSet* items = nullptr;                                     // owned
    ArcSet* ui = nullptr;                                        // owned
    const ModView* view = nullptr;                               // the layers above db
    std::unordered_map<std::string, std::string> tags;
    std::size_t modTags = 0;                                     // tags a mod layer set
    // The record as the view has it: the last mod layer that has it, else the base game.
    bool record(const std::string& name, ArzRecord& out) const;
    bool layered() const;                                        // a non-empty view
    GameData() = default;
    ~GameData();
    GameData(const GameData&) = delete;
    GameData& operator=(const GameData&) = delete;
};

// The collection: every record build_catalogue.py puts in catalogue.json's "items".
// `warnings` receives the checks the Python tool asserts (they do not stop a generation).
// collectMi: uniquetab.ini's collect_mi - admit the monster infrequents (see the header).
bool collectItems(const GameData& g, std::vector<CatalogueItem>& items,
                  std::vector<std::string>& warnings, std::string* error,
                  bool collectMi = false);

// The record-local MI test, shared by collectItems and the view probe: one of the 25 equipment
// classes (isEquipmentClass) and itemClassification Rare - faction-store and crafted rares too.
// key and fileDescription are unused (kept so a record-local rule needs no caller change).
// The path rule and the name tag are the caller's (the shipped filter / the mod rule); the junk
// folders of a game are [exclude] blocks of the built-in groups (data).
bool isMiCandidate(const std::string& key, const std::string& cls,
                   const std::string& classification, const std::string& fileDescription);
// One of the 25 equipment classes every built-in group knows: the 23 slot classes (head ..
// shield) and the two per-class groups' classes (WeaponMagical_Staff, WeaponHunting_Spear) - not
// relics, blueprints or components. The catalogue's isEquipment flag stays the 23 slot classes
// (catalogue.bin unchanged); the KIND mark (isMi) and the groups' `flag = equipment` of an MI use
// this set.
bool isEquipmentClass(const std::string& cls);

// A name without its colour codes ({^X} and ^X), for sorting; what is stored keeps them.
std::string stripColourCodes(const std::string& s);

// The class words of the view: for every item class the engine prints a type word for
// (utg::classTypeTags - Game.dll's class -> tag switches), lower-cased class -> the text of that
// tag in the view's text as GameData loaded it (the base Text_<lang>.arc, the mod's text_en*.arc
// over it; the generator always runs on EN). A tag with no text is left out. The groups derive
// their default labels and tags from it (utg::GroupSet::words); tools/build_uniq_db.py
// class_words() is the twin.
std::map<std::string, std::string> classWords(const GameData& g);

// pack_catalogue.py: items -> catalogue.bin bytes. False (with the reason) on bad input.
bool packCatalogue(std::vector<CatalogueItem>& items, std::vector<std::uint8_t>& out,
                   CatalogueStats* stats, std::string* error);

// Slot word -> the on-disk group index of model/catalogue.h (28 = Other).
int slotGroupOf(const std::string& slot);

} // namespace gen
