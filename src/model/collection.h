// collection.h - "which catalogue items does the player own right now".
//
// Fed by the ownership hooks:
// InventorySack::AddItem gives an (itemId, record path) pair, RemoveItem gives
// back the id alone.  The Collection turns that stream into per-item counts
// against a loaded Catalogue.
//
// Not internally synchronised: the hooks run on the game thread and drawing
// runs on the render thread.  The DLL owns the mutex (or the snapshot swap)
// around this object.

#ifndef GDUT_MODEL_COLLECTION_H
#define GDUT_MODEL_COLLECTION_H

#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "catalogue.h"

namespace gdut {

class Collection {
public:
    Collection() = default;

    // Binds to a catalogue (kept by pointer; it must outlive the Collection)
    // and drops all ownership state.
    void reset(const Catalogue* catalogue);

    const Catalogue* catalogue() const noexcept { return m_catalogue; }

    // --- events from the game thread -------------------------------------
    // An item entered a sack.  Records that are not in the catalogue (common:
    // rares, commons, components) are ignored but counted in
    // unknownRecordEvents().  A repeat of an id already known is not counted
    // twice; if the same id arrives with a different record the old one is
    // decremented first.
    void onItemAdded(std::uint32_t itemId, std::string_view recordPath);

    // An item left a sack.  Unknown ids are ignored.
    void onItemRemoved(std::uint32_t itemId);

    // Forget every item (stash closed / character swap).  Keeps the catalogue.
    void clear();

    // --- queries ----------------------------------------------------------
    int owned(int itemIndex) const noexcept;              // copies held, 0 if none
    bool isOwned(int itemIndex) const noexcept { return owned(itemIndex) > 0; }
    int ownedCount() const noexcept { return m_ownedDistinct; }   // distinct items
    int totalItems() const noexcept { return m_totalItems; }      // copies tracked
    std::size_t trackedIds() const noexcept { return m_idToIndex.size(); }

    // Bumped on every state change so a renderer can tell whether its snapshot
    // is stale without comparing the whole vector.
    std::uint64_t version() const noexcept { return m_version; }

    // Diagnostics, for the bring-up log and the tests.
    std::uint64_t unknownRecordEvents() const noexcept { return m_unknownEvents; }
    std::size_t distinctUnknownRecords() const noexcept { return m_unknownRecords.size(); }
    std::uint64_t duplicateIdEvents() const noexcept { return m_duplicateIds; }

    // --- test / bring-up helpers (no item ids involved) --------------------
    bool setOwnedByRecord(std::string_view recordPath, int count = 1);
    bool resetByRecord(std::string_view recordPath);

    // Copy of the per-item counts, for the snapshot-swap threading pattern.
    const std::vector<std::uint16_t>& counts() const noexcept { return m_counts; }

private:
    void addCount(int index, int delta);

    const Catalogue* m_catalogue = nullptr;
    std::vector<std::uint16_t> m_counts;                  // one per catalogue item
    std::unordered_map<std::uint32_t, int> m_idToIndex;   // live item id -> item index
    std::unordered_set<std::string> m_unknownRecords;
    int m_ownedDistinct = 0;
    int m_totalItems = 0;
    std::uint64_t m_version = 0;
    std::uint64_t m_unknownEvents = 0;
    std::uint64_t m_duplicateIds = 0;
};

}  // namespace gdut

#endif  // GDUT_MODEL_COLLECTION_H
