#include "catalogue.h"

#include <cstdio>
#include <cstring>

namespace gdut {
namespace {

constexpr std::uint32_t kHeaderSize = 80;
constexpr std::uint32_t kItemEntrySize = 24;
constexpr std::uint32_t kSetEntrySize = 16;
constexpr std::uint32_t kGroupEntrySize = 8;

// All multi-byte fields in the file are little-endian; these helpers read
// through memcpy so unaligned offsets and big-endian hosts stay correct.
std::uint32_t rd_u32(const std::uint8_t* p) noexcept {
    std::uint32_t v = 0;
    std::memcpy(&v, p, 4);
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    v = ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
        ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
#endif
    return v;
}

std::int32_t rd_i32(const std::uint8_t* p) noexcept {
    const std::uint32_t u = rd_u32(p);
    std::int32_t v = 0;
    std::memcpy(&v, &u, 4);
    return v;
}

std::uint16_t rd_u16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
}

const char* kGroupNames[kSlotGroupCount] = {
    "Helm", "Shoulders", "Chest", "Gloves", "Belt", "Pants", "Boots",
    "Axe1H", "Mace1H", "Sword1H", "Dagger", "Scepter",
    "Axe2H", "Mace2H", "Sword2H", "Spear2H",
    "Ranged1H", "Ranged2H", "Offhand", "Shield",
    "Ring", "Amulet", "Medal", "Relic",
    "Blueprint", "Augment", "Consumable", "Quest", "Other",
};

}  // namespace

const char* slotGroupName(SlotGroup group) noexcept {
    const int g = static_cast<int>(group);
    if (g < 0 || g >= kSlotGroupCount) return "?";
    return kGroupNames[g];
}

const char* classificationName(Classification c) noexcept {
    return c == Classification::Legendary ? "Legendary" : "Epic";
}

void Catalogue::clear() noexcept {
    m_byRecord.clear();
    m_items.clear();
    m_sets.clear();
    m_setMembers.clear();
    m_buffer.clear();
    for (int i = 0; i < kSlotGroupCount; ++i) m_groups[i] = GroupRange{};
    m_formatVersion = 0;
    m_equipmentCount = 0;
    m_relicCount = 0;
    m_defaultVisibleCount = 0;
}

void Catalogue::fail(std::string* error, const std::string& why) {
    clear();
    if (error) *error = why;
}

bool Catalogue::loadFromFile(const std::string& path, std::string* error) {
    clear();
#if defined(_MSC_VER)
    std::FILE* fh = nullptr;
    if (::fopen_s(&fh, path.c_str(), "rb") != 0) fh = nullptr;
#else
    std::FILE* fh = std::fopen(path.c_str(), "rb");
#endif
    if (!fh) {
        fail(error, "cannot open " + path);
        return false;
    }
    if (std::fseek(fh, 0, SEEK_END) != 0) {
        std::fclose(fh);
        fail(error, "cannot seek " + path);
        return false;
    }
    const long size = std::ftell(fh);
    if (size < 0) {
        std::fclose(fh);
        fail(error, "cannot size " + path);
        return false;
    }
    std::rewind(fh);
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(size));
    if (!buf.empty() && std::fread(buf.data(), 1, buf.size(), fh) != buf.size()) {
        std::fclose(fh);
        fail(error, "short read on " + path);
        return false;
    }
    std::fclose(fh);
    if (!loadFromMemory(std::move(buf), error)) {
        if (error) *error = path + ": " + *error;
        return false;
    }
    return true;
}

bool Catalogue::loadFromMemory(std::vector<std::uint8_t> buffer, std::string* error) {
    clear();
    m_buffer = std::move(buffer);
    if (!parse(error)) return false;
    return true;
}

bool Catalogue::parse(std::string* error) {
    const std::size_t fileSize = m_buffer.size();
    if (fileSize < kHeaderSize) {
        fail(error, "file smaller than the header");
        return false;
    }
    const std::uint8_t* base = m_buffer.data();

    if (std::memcmp(base, "GDUT", 4) != 0) {
        fail(error, "bad magic (expected GDUT)");
        return false;
    }
    const std::uint32_t version    = rd_u32(base + 4);
    const std::uint32_t headerSize = rd_u32(base + 8);
    // base + 12: reserved
    const std::uint32_t itemCount  = rd_u32(base + 16);
    const std::uint32_t itemEntry  = rd_u32(base + 20);
    const std::uint32_t itemOff    = rd_u32(base + 24);
    const std::uint32_t setCount   = rd_u32(base + 28);
    const std::uint32_t setEntry   = rd_u32(base + 32);
    const std::uint32_t setOff     = rd_u32(base + 36);
    const std::uint32_t memberCnt  = rd_u32(base + 40);
    const std::uint32_t memberOff  = rd_u32(base + 44);
    const std::uint32_t groupCount = rd_u32(base + 48);
    const std::uint32_t groupOff   = rd_u32(base + 52);
    const std::uint32_t stringSize = rd_u32(base + 56);
    const std::uint32_t stringOff  = rd_u32(base + 60);
    const std::uint32_t totalSize  = rd_u32(base + 64);
    const std::uint32_t defVisible = rd_u32(base + 68);
    const std::uint32_t equipCount = rd_u32(base + 72);
    const std::uint32_t relicCount = rd_u32(base + 76);

    if (version != kFormatVersion) {
        char msg[96];
        std::snprintf(msg, sizeof msg, "format version %u, expected %u", version, kFormatVersion);
        fail(error, msg);
        return false;
    }
    if (headerSize != kHeaderSize) {
        fail(error, "unexpected header size");
        return false;
    }
    if (itemEntry != kItemEntrySize || setEntry != kSetEntrySize) {
        fail(error, "unexpected entry size");
        return false;
    }
    if (groupCount != static_cast<std::uint32_t>(kSlotGroupCount)) {
        fail(error, "unexpected slot-group count");
        return false;
    }
    if (totalSize != fileSize) {
        fail(error, "header total size does not match the file size");
        return false;
    }

    // Section bounds.  Every product below is done in 64 bits so a hostile
    // count cannot wrap.
    auto sectionOk = [&](std::uint64_t off, std::uint64_t count, std::uint64_t entry) {
        const std::uint64_t bytes = count * entry;
        return off <= fileSize && bytes <= fileSize && off + bytes <= fileSize;
    };
    if (!sectionOk(groupOff, groupCount, kGroupEntrySize)) {
        fail(error, "group table out of bounds");
        return false;
    }
    if (!sectionOk(itemOff, itemCount, itemEntry)) {
        fail(error, "item table out of bounds");
        return false;
    }
    if (!sectionOk(setOff, setCount, setEntry)) {
        fail(error, "set table out of bounds");
        return false;
    }
    if (!sectionOk(memberOff, memberCnt, 4)) {
        fail(error, "set member table out of bounds");
        return false;
    }
    if (!sectionOk(stringOff, stringSize, 1)) {
        fail(error, "string table out of bounds");
        return false;
    }
    if (stringSize == 0 || base[static_cast<std::size_t>(stringOff) + stringSize - 1] != 0) {
        fail(error, "string table is not NUL terminated");
        return false;
    }

    const char* strings = reinterpret_cast<const char*>(base) + stringOff;
    // Safe because the table's last byte is 0: strlen cannot run past it.
    auto str = [&](std::uint32_t ref, bool* ok) -> std::string_view {
        if (ref >= stringSize) {
            *ok = false;
            return std::string_view();
        }
        return std::string_view(strings + ref, std::strlen(strings + ref));
    };

    // ---- items
    bool ok = true;
    m_items.resize(itemCount);
    for (std::uint32_t i = 0; i < itemCount; ++i) {
        const std::uint8_t* p = base + itemOff + static_cast<std::size_t>(i) * itemEntry;
        ItemView& it = m_items[i];
        it.record = str(rd_u32(p + 0), &ok);
        it.name   = str(rd_u32(p + 4), &ok);
        it.bitmap = str(rd_u32(p + 8), &ok);
        if (!ok) {
            fail(error, "string reference out of bounds in the item table");
            return false;
        }
        const std::int32_t setIndex = rd_i32(p + 12);
        if (setIndex < -1 || setIndex >= static_cast<std::int32_t>(setCount)) {
            fail(error, "item set index out of bounds");
            return false;
        }
        it.setIndex = setIndex;
        const std::uint8_t cls = p[16];
        if (cls >= kClassificationCount) {
            fail(error, "item classification out of range");
            return false;
        }
        it.classification = static_cast<Classification>(cls);
        const std::uint8_t grp = p[17];
        if (grp >= kSlotGroupCount) {
            fail(error, "item slot group out of range");
            return false;
        }
        it.group = static_cast<SlotGroup>(grp);
        it.levelRequirement = rd_u16(p + 18);
        it.itemLevel        = rd_u16(p + 20);
        it.flags            = rd_u16(p + 22);
    }

    // ---- set members (copied out so the exposed pointer is aligned)
    m_setMembers.resize(memberCnt);
    for (std::uint32_t i = 0; i < memberCnt; ++i) {
        const std::uint32_t m = rd_u32(base + memberOff + static_cast<std::size_t>(i) * 4);
        if (m >= itemCount) {
            fail(error, "set member index out of bounds");
            return false;
        }
        m_setMembers[i] = m;
    }

    // ---- sets
    m_sets.resize(setCount);
    for (std::uint32_t i = 0; i < setCount; ++i) {
        const std::uint8_t* p = base + setOff + static_cast<std::size_t>(i) * setEntry;
        SetView& s = m_sets[i];
        s.name   = str(rd_u32(p + 0), &ok);
        s.record = str(rd_u32(p + 4), &ok);
        if (!ok) {
            fail(error, "string reference out of bounds in the set table");
            return false;
        }
        const std::uint32_t first = rd_u32(p + 8);
        const std::uint32_t count = rd_u32(p + 12);
        if (static_cast<std::uint64_t>(first) + count > memberCnt) {
            fail(error, "set member range out of bounds");
            return false;
        }
        s.members = count ? (m_setMembers.data() + first) : nullptr;
        s.memberCount = count;
    }

    // ---- group ranges
    for (std::uint32_t g = 0; g < groupCount; ++g) {
        const std::uint8_t* p = base + groupOff + static_cast<std::size_t>(g) * kGroupEntrySize;
        const std::uint32_t first = rd_u32(p + 0);
        const std::uint32_t count = rd_u32(p + 4);
        if (static_cast<std::uint64_t>(first) + count > itemCount) {
            fail(error, "group range out of bounds");
            return false;
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            if (m_items[first + i].group != static_cast<SlotGroup>(g)) {
                fail(error, "group range does not match the item table");
                return false;
            }
        }
        m_groups[g] = GroupRange{first, count};
    }

    // ---- record -> index
    m_byRecord.reserve(itemCount * 2);
    for (std::uint32_t i = 0; i < itemCount; ++i) {
        if (m_items[i].record.empty()) {
            fail(error, "item with an empty record path");
            return false;
        }
        if (!m_byRecord.emplace(m_items[i].record, static_cast<int>(i)).second) {
            fail(error, "duplicate record path in the item table");
            return false;
        }
    }

    // ---- the header's summary counts must match the item flags
    std::uint32_t nEquip = 0, nRelic = 0, nDefault = 0;
    for (const ItemView& it : m_items) {
        if (it.has(ItemFlag::Equipment)) ++nEquip;
        if (it.has(ItemFlag::Relic)) ++nRelic;
        if (it.has(ItemFlag::DefaultVisible)) ++nDefault;
    }
    if (nEquip != equipCount || nRelic != relicCount || nDefault != defVisible) {
        fail(error, "header counts disagree with the item flags");
        return false;
    }

    m_formatVersion = version;
    m_defaultVisibleCount = defVisible;
    m_equipmentCount = equipCount;
    m_relicCount = relicCount;
    return true;
}

GroupRange Catalogue::groupRange(SlotGroup group) const noexcept {
    const int g = static_cast<int>(group);
    if (g < 0 || g >= kSlotGroupCount) return GroupRange{};
    return m_groups[g];
}

int Catalogue::indexOfRecord(std::string_view record) const noexcept {
    const auto it = m_byRecord.find(record);
    return it == m_byRecord.end() ? -1 : it->second;
}

}  // namespace gdut
