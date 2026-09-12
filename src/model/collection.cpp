#include "collection.h"

#include <string>

namespace gdut {

namespace {
constexpr int kMaxCount = 0xFFFF;
}

void Collection::reset(const Catalogue* catalogue) {
    m_catalogue = catalogue;
    m_counts.assign(catalogue ? catalogue->itemCount() : 0, 0);
    m_idToIndex.clear();
    m_unknownRecords.clear();
    m_ownedDistinct = 0;
    m_totalItems = 0;
    m_unknownEvents = 0;
    m_duplicateIds = 0;
    ++m_version;
}

void Collection::clear() {
    m_counts.assign(m_counts.size(), 0);
    m_idToIndex.clear();
    m_ownedDistinct = 0;
    m_totalItems = 0;
    ++m_version;
}

void Collection::addCount(int index, int delta) {
    if (index < 0 || static_cast<std::size_t>(index) >= m_counts.size()) return;
    const int before = m_counts[static_cast<std::size_t>(index)];
    int after = before + delta;
    if (after < 0) after = 0;
    if (after > kMaxCount) after = kMaxCount;
    if (after == before) return;
    m_counts[static_cast<std::size_t>(index)] = static_cast<std::uint16_t>(after);
    if (before == 0 && after > 0) ++m_ownedDistinct;
    if (before > 0 && after == 0) --m_ownedDistinct;
    m_totalItems += after - before;
    ++m_version;
}

void Collection::onItemAdded(std::uint32_t itemId, std::string_view recordPath) {
    if (!m_catalogue) return;
    const int index = m_catalogue->indexOfRecord(recordPath);
    if (index < 0) {
        // Not a collectible: ordinary loot, components, quest junk.  Counted so
        // bring-up can see the hook is firing, then dropped.
        ++m_unknownEvents;
        m_unknownRecords.emplace(recordPath);
        return;
    }
    const auto it = m_idToIndex.find(itemId);
    if (it != m_idToIndex.end()) {
        ++m_duplicateIds;
        if (it->second == index) return;   // same item announced twice
        addCount(it->second, -1);          // the id was recycled for another record
        it->second = index;
        addCount(index, +1);
        return;
    }
    m_idToIndex.emplace(itemId, index);
    addCount(index, +1);
}

void Collection::onItemRemoved(std::uint32_t itemId) {
    const auto it = m_idToIndex.find(itemId);
    if (it == m_idToIndex.end()) return;
    const int index = it->second;
    m_idToIndex.erase(it);
    addCount(index, -1);
}

int Collection::owned(int itemIndex) const noexcept {
    if (itemIndex < 0 || static_cast<std::size_t>(itemIndex) >= m_counts.size()) return 0;
    return m_counts[static_cast<std::size_t>(itemIndex)];
}

bool Collection::setOwnedByRecord(std::string_view recordPath, int count) {
    if (!m_catalogue) return false;
    const int index = m_catalogue->indexOfRecord(recordPath);
    if (index < 0) {
        ++m_unknownEvents;
        m_unknownRecords.emplace(recordPath);
        return false;
    }
    if (count < 0) count = 0;
    addCount(index, count - owned(index));
    return true;
}

bool Collection::resetByRecord(std::string_view recordPath) {
    return setOwnedByRecord(recordPath, 0);
}

}  // namespace gdut
