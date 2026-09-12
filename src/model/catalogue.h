// catalogue.h - read-only view over data\oracle\catalogue.bin (format "GDUT" v1).
//
// C++17, standard library only.  Nothing here touches the game; the DLL loads
// the file once at startup and keeps the Catalogue alive for the process.
// Every string is a std::string_view into the owned buffer, so the Catalogue
// must outlive every view handed out.  The loader never trusts an offset from
// the file: every section, every entry and every string reference is bounds
// checked before any view is created.
//
// Packer: tools/pack_catalogue.py; the generator that writes the shipped file is src/gen.

#ifndef GDUT_MODEL_CATALOGUE_H
#define GDUT_MODEL_CATALOGUE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gdut {

// Display order of the collection grid.  Must match GROUP_NAMES in
// tools/pack_catalogue.py.
enum class SlotGroup : std::uint8_t {
    Helm = 0,
    Shoulders,
    Chest,
    Gloves,
    Belt,
    Pants,
    Boots,
    Axe1H,
    Mace1H,
    Sword1H,
    Dagger,
    Scepter,
    Axe2H,
    Mace2H,
    Sword2H,
    Spear2H,
    Ranged1H,
    Ranged2H,
    Offhand,
    Shield,
    Ring,
    Amulet,
    Medal,
    Relic,
    Blueprint,
    Augment,
    Consumable,
    Quest,
    Other,
};

inline constexpr int kSlotGroupCount = 29;

// The first kDefaultSlotGroupCount groups (Helm .. Relic) are the default
// display set: exactly the items with ItemFlag::DefaultVisible.
inline constexpr int kDefaultSlotGroupCount = 24;

const char* slotGroupName(SlotGroup group) noexcept;

enum class Classification : std::uint8_t { Epic = 0, Legendary = 1 };

inline constexpr int kClassificationCount = 2;

const char* classificationName(Classification c) noexcept;

namespace ItemFlag {
enum : std::uint16_t {
    SetPiece       = 1u << 0,
    Blueprint      = 1u << 1,
    Augment        = 1u << 2,
    Relic          = 1u << 3,
    Faction        = 1u << 4,
    Equipment      = 1u << 5,
    DefaultVisible = 1u << 6,  // Equipment || Relic
    HasBitmap      = 1u << 7,
};
}  // namespace ItemFlag

// One catalogue item.  Views point into the Catalogue's buffer.
struct ItemView {
    std::string_view record;    // DBR path, e.g. "records/items/gearhead/d208_head.dbr"
    std::string_view name;      // localised display name
    std::string_view bitmap;    // resource path of the icon, empty for 39 blueprints
    std::int32_t     setIndex = -1;   // index into Catalogue::sets(), -1 if none
    Classification   classification = Classification::Epic;
    SlotGroup        group = SlotGroup::Other;
    std::uint16_t    levelRequirement = 0;
    std::uint16_t    itemLevel = 0;
    std::uint16_t    flags = 0;

    bool has(std::uint16_t f) const noexcept { return (flags & f) != 0; }
};

struct SetView {
    std::string_view    name;     // localised set name
    std::string_view    record;   // lootset DBR path
    const std::uint32_t* members = nullptr;  // catalogue item indices
    std::uint32_t        memberCount = 0;
};

struct GroupRange {
    std::uint32_t first = 0;
    std::uint32_t count = 0;
};

class Catalogue {
public:
    static constexpr std::uint32_t kFormatVersion = 1;

    Catalogue() = default;
    Catalogue(const Catalogue&) = delete;
    Catalogue& operator=(const Catalogue&) = delete;

    // Reads the whole file into one buffer and validates it.  On failure the
    // Catalogue is left empty and *error (if given) holds the reason.
    bool loadFromFile(const std::string& path, std::string* error = nullptr);

    // Same validation, buffer supplied by the caller (used by tests).
    bool loadFromMemory(std::vector<std::uint8_t> buffer, std::string* error = nullptr);

    bool loaded() const noexcept { return !m_items.empty(); }
    void clear() noexcept;

    const std::vector<ItemView>& items() const noexcept { return m_items; }
    std::size_t itemCount() const noexcept { return m_items.size(); }
    const ItemView& item(std::size_t i) const { return m_items[i]; }

    const std::vector<SetView>& sets() const noexcept { return m_sets; }
    std::size_t setCount() const noexcept { return m_sets.size(); }

    // Contiguous [first, first+count) range of the item array for one group.
    GroupRange groupRange(SlotGroup group) const noexcept;

    // -1 when the record is not in the catalogue.
    int indexOfRecord(std::string_view record) const noexcept;

    std::uint32_t equipmentCount() const noexcept { return m_equipmentCount; }
    std::uint32_t relicCount() const noexcept { return m_relicCount; }
    std::uint32_t defaultVisibleCount() const noexcept { return m_defaultVisibleCount; }
    std::uint32_t formatVersion() const noexcept { return m_formatVersion; }
    std::size_t byteSize() const noexcept { return m_buffer.size(); }

private:
    bool parse(std::string* error);
    void fail(std::string* error, const std::string& why);

    std::vector<std::uint8_t> m_buffer;
    std::vector<ItemView>     m_items;
    std::vector<SetView>      m_sets;
    std::vector<std::uint32_t> m_setMembers;
    GroupRange                m_groups[kSlotGroupCount];
    std::unordered_map<std::string_view, int> m_byRecord;
    std::uint32_t m_formatVersion = 0;
    std::uint32_t m_equipmentCount = 0;
    std::uint32_t m_relicCount = 0;
    std::uint32_t m_defaultVisibleCount = 0;
};

}  // namespace gdut

#endif  // GDUT_MODEL_CATALOGUE_H
