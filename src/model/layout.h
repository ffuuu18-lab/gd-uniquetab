// layout.h - grid layout for the collection tab.  Pure arithmetic: it decides
// which catalogue items are shown, where each cell lands inside a rectangle,
// and which cell a mouse position is over.  No drawing, no engine types.
//
// The renderer calls relayout() once per filter/viewport/ownership
// change, then per frame walks visibleRange(), asks cellRect() for each slot
// and draws the icon; sections() gives the optional group header rows.

#ifndef GDUT_MODEL_LAYOUT_H
#define GDUT_MODEL_LAYOUT_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "catalogue.h"
#include "collection.h"

namespace gdut {

struct Rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    bool empty() const noexcept { return w <= 0 || h <= 0; }
    bool contains(int px, int py) const noexcept {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

using Viewport = Rect;   // the game's transfer grid is x=105 y=4 w=320 h=608

enum class OwnedFilter : std::uint8_t { Any = 0, OwnedOnly = 1, MissingOnly = 2 };

// Bit per SlotGroup.
constexpr std::uint64_t groupBit(SlotGroup g) noexcept {
    return std::uint64_t(1) << static_cast<int>(g);
}
constexpr std::uint64_t kAllGroupsMask = (std::uint64_t(1) << kSlotGroupCount) - 1;
// Helm .. Relic: equipment plus relics, the default display set.
constexpr std::uint64_t kDefaultGroupsMask = (std::uint64_t(1) << kDefaultSlotGroupCount) - 1;

// Bit per Classification.
constexpr std::uint32_t kAllClassificationsMask = (1u << kClassificationCount) - 1;

struct Filters {
    std::uint64_t groupMask = kDefaultGroupsMask;
    std::uint32_t classificationMask = kAllClassificationsMask;
    OwnedFilter   owned = OwnedFilter::Any;
    std::string   nameQuery;          // case-insensitive substring, empty = no filter
    bool          requireDefaultVisible = true;   // drop blueprints/augments/consumables/quest
};

struct Metrics {
    int cellSize = 40;      // square icon cell, pixels
    int gap = 2;            // pixels between cells and between rows
    int headerHeight = 0;   // >0 turns on one header row per non-empty group
};

// One group's block inside the laid-out content.  Content-space y (add the
// viewport origin and subtract the scroll offset for screen coordinates).
struct Section {
    SlotGroup group = SlotGroup::Other;
    int firstVisible = 0;   // index into the filtered list
    int count = 0;
    int headerTop = 0;      // content y of the header row (== gridTop if no headers)
    int gridTop = 0;        // content y of the first cell row
    int rows = 0;
};

struct VisibleRange {
    int first = 0;   // index into the filtered list
    int count = 0;
};

// ASCII case-insensitive substring test (item names are ASCII in 1.3.0.8).
bool containsNoCase(std::string_view haystack, std::string_view needle) noexcept;

class GridLayout {
public:
    GridLayout() = default;

    void setCatalogue(const Catalogue* catalogue);
    const Catalogue* catalogue() const noexcept { return m_catalogue; }

    void setMetrics(const Metrics& m);
    const Metrics& metrics() const noexcept { return m_metrics; }

    // With headerHeight > 0 every group starts on a fresh row under a header
    // row; with headerHeight == 0 the groups run together in one continuous
    // grid (sections() is then metadata only).
    bool sectioned() const noexcept { return m_metrics.headerHeight > 0; }

    // Rebuilds the filtered list and the geometry.  `owned` may be null when
    // no ownership filter is used; if it is null and a filter asks for owned or
    // missing items, everything is treated as not owned.  Keeps the scroll
    // offset where it can (clamped to the new content height).
    void relayout(const Viewport& viewport, const Filters& filters,
                  const Collection* owned = nullptr);

    // --- geometry ---------------------------------------------------------
    int columns() const noexcept { return m_columns; }
    int rows() const noexcept { return m_rows; }
    int contentHeight() const noexcept { return m_contentHeight; }
    int maxScroll() const noexcept;
    int scrollOffset() const noexcept { return m_scroll; }
    void scrollBy(int px);
    void scrollTo(int px);
    const Viewport& viewport() const noexcept { return m_viewport; }

    // --- filtered list ----------------------------------------------------
    int visibleItemCount() const noexcept { return static_cast<int>(m_visible.size()); }
    // slot -> catalogue item index (-1 when out of range)
    int itemAtSlot(int slot) const noexcept;
    // catalogue item index -> slot in the filtered list (-1 when filtered out)
    int slotOfItem(int itemIndex) const noexcept;
    const std::vector<int>& visibleItems() const noexcept { return m_visible; }
    const std::vector<Section>& sections() const noexcept { return m_sections; }

    // Slots whose cell intersects the viewport at the current scroll offset.
    VisibleRange visibleRange() const noexcept;

    // Screen rect of a catalogue item's cell.  Empty rect when the item is
    // filtered out; otherwise the rect even if it is scrolled off-screen.
    Rect cellRect(int itemIndex) const noexcept;
    // Same, addressed by slot in the filtered list.
    Rect cellRectBySlot(int slot) const noexcept;
    // Screen rect of a section's header row (empty when headers are off).
    Rect headerRect(int sectionIndex) const noexcept;

    // Catalogue item index under a screen pixel, or -1 (gaps, header rows,
    // padding and anything outside the viewport all give -1).
    int hitTest(int px, int py) const noexcept;

private:
    bool passes(int index, const ItemView& it, const Filters& f, const Collection* owned) const;
    void rebuildGeometry();

    const Catalogue* m_catalogue = nullptr;
    Metrics m_metrics;
    Viewport m_viewport;
    std::vector<int> m_visible;     // slot -> catalogue index
    std::vector<int> m_slotOf;      // catalogue index -> slot, -1
    std::vector<Section> m_sections;
    int m_columns = 1;
    int m_rows = 0;
    int m_contentHeight = 0;
    int m_scroll = 0;
};

}  // namespace gdut

#endif  // GDUT_MODEL_LAYOUT_H
