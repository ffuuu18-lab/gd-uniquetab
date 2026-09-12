#include "layout.h"

#include <algorithm>

namespace gdut {

namespace {

char lowerAscii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

}  // namespace

bool containsNoCase(std::string_view haystack, std::string_view needle) noexcept {
    if (needle.empty()) return true;
    if (needle.size() > haystack.size()) return false;
    const std::size_t last = haystack.size() - needle.size();
    for (std::size_t i = 0; i <= last; ++i) {
        std::size_t j = 0;
        while (j < needle.size() && lowerAscii(haystack[i + j]) == lowerAscii(needle[j])) ++j;
        if (j == needle.size()) return true;
    }
    return false;
}

void GridLayout::setCatalogue(const Catalogue* catalogue) {
    m_catalogue = catalogue;
    m_visible.clear();
    m_slotOf.assign(catalogue ? catalogue->itemCount() : 0, -1);
    m_sections.clear();
    m_columns = 1;
    m_rows = 0;
    m_contentHeight = 0;
    m_scroll = 0;
}

void GridLayout::setMetrics(const Metrics& m) {
    m_metrics = m;
    if (m_metrics.cellSize < 1) m_metrics.cellSize = 1;
    if (m_metrics.gap < 0) m_metrics.gap = 0;
    if (m_metrics.headerHeight < 0) m_metrics.headerHeight = 0;
    rebuildGeometry();
}

bool GridLayout::passes(int index, const ItemView& it, const Filters& f,
                        const Collection* owned) const {
    if (f.requireDefaultVisible && !it.has(ItemFlag::DefaultVisible)) return false;
    if ((f.groupMask & groupBit(it.group)) == 0) return false;
    const std::uint32_t clsBit = 1u << static_cast<int>(it.classification);
    if ((f.classificationMask & clsBit) == 0) return false;
    if (f.owned != OwnedFilter::Any) {
        const bool isOwned = owned ? owned->owned(index) > 0 : false;
        if (f.owned == OwnedFilter::OwnedOnly && !isOwned) return false;
        if (f.owned == OwnedFilter::MissingOnly && isOwned) return false;
    }
    if (!f.nameQuery.empty() && !containsNoCase(it.name, f.nameQuery)) return false;
    return true;
}

void GridLayout::relayout(const Viewport& viewport, const Filters& filters,
                          const Collection* owned) {
    m_viewport = viewport;
    if (m_viewport.w < 0) m_viewport.w = 0;
    if (m_viewport.h < 0) m_viewport.h = 0;

    m_visible.clear();
    m_sections.clear();
    if (!m_catalogue) {
        m_slotOf.clear();
        rebuildGeometry();
        return;
    }
    const std::size_t n = m_catalogue->itemCount();
    m_slotOf.assign(n, -1);
    m_visible.reserve(n);

    // Items are stored sorted by (group, level, name, record), so walking the
    // group ranges in enum order keeps the filtered list in display order and
    // makes every section contiguous.
    for (int g = 0; g < kSlotGroupCount; ++g) {
        const SlotGroup group = static_cast<SlotGroup>(g);
        if ((filters.groupMask & groupBit(group)) == 0) continue;
        const GroupRange range = m_catalogue->groupRange(group);
        if (range.count == 0) continue;
        Section section;
        section.group = group;
        section.firstVisible = static_cast<int>(m_visible.size());
        for (std::uint32_t i = 0; i < range.count; ++i) {
            const int index = static_cast<int>(range.first + i);
            const ItemView& it = m_catalogue->item(static_cast<std::size_t>(index));
            if (!passes(index, it, filters, owned)) continue;
            m_slotOf[static_cast<std::size_t>(index)] = static_cast<int>(m_visible.size());
            m_visible.push_back(index);
        }
        section.count = static_cast<int>(m_visible.size()) - section.firstVisible;
        if (section.count > 0) m_sections.push_back(section);
    }

    rebuildGeometry();
}

void GridLayout::rebuildGeometry() {
    const int step = m_metrics.cellSize + m_metrics.gap;
    m_columns = (m_viewport.w + m_metrics.gap) / step;
    if (m_columns < 1) m_columns = 1;
    m_rows = 0;

    if (sectioned()) {
        // Each group starts on a fresh row under its own header.
        int y = 0;
        for (Section& s : m_sections) {
            s.headerTop = y;
            y += m_metrics.headerHeight + m_metrics.gap;
            s.gridTop = y;
            s.rows = (s.count + m_columns - 1) / m_columns;
            m_rows += s.rows;
            y += s.rows * step;
        }
        m_contentHeight = y > 0 ? y - m_metrics.gap : 0;
    } else {
        // One continuous grid; sections stay as metadata for the renderer.
        const int n = static_cast<int>(m_visible.size());
        m_rows = (n + m_columns - 1) / m_columns;
        m_contentHeight = m_rows > 0 ? m_rows * step - m_metrics.gap : 0;
        for (Section& s : m_sections) {
            const int firstRow = s.firstVisible / m_columns;
            const int lastRow = (s.firstVisible + s.count - 1) / m_columns;
            s.headerTop = firstRow * step;
            s.gridTop = s.headerTop;
            s.rows = lastRow - firstRow + 1;
        }
    }
    scrollTo(m_scroll);
}

int GridLayout::maxScroll() const noexcept {
    const int over = m_contentHeight - m_viewport.h;
    return over > 0 ? over : 0;
}

void GridLayout::scrollTo(int px) {
    const int limit = maxScroll();
    if (px < 0) px = 0;
    if (px > limit) px = limit;
    m_scroll = px;
}

void GridLayout::scrollBy(int px) { scrollTo(m_scroll + px); }

int GridLayout::itemAtSlot(int slot) const noexcept {
    if (slot < 0 || slot >= static_cast<int>(m_visible.size())) return -1;
    return m_visible[static_cast<std::size_t>(slot)];
}

int GridLayout::slotOfItem(int itemIndex) const noexcept {
    if (itemIndex < 0 || static_cast<std::size_t>(itemIndex) >= m_slotOf.size()) return -1;
    return m_slotOf[static_cast<std::size_t>(itemIndex)];
}

VisibleRange GridLayout::visibleRange() const noexcept {
    VisibleRange out;
    if (m_visible.empty() || m_viewport.h <= 0) return out;
    const int step = m_metrics.cellSize + m_metrics.gap;
    const int top = m_scroll;
    const int bottom = m_scroll + m_viewport.h;

    if (!sectioned()) {
        int firstRow = top / step;
        if (firstRow < 0) firstRow = 0;
        if (firstRow >= m_rows) return out;
        int lastRow = (bottom - 1) / step;
        if (lastRow >= m_rows) lastRow = m_rows - 1;
        if (lastRow < firstRow) return out;
        const int n = static_cast<int>(m_visible.size());
        out.first = firstRow * m_columns;
        int end = (lastRow + 1) * m_columns;
        if (end > n) end = n;
        out.count = end - out.first;
        if (out.count < 0) out.count = 0;
        return out;
    }

    int first = -1;
    int end = 0;
    for (const Section& s : m_sections) {
        const int sectionBottom = s.gridTop + s.rows * step - m_metrics.gap;
        if (sectionBottom <= top) continue;          // fully above
        if (s.gridTop >= bottom) break;              // fully below
        int firstRow = (top - s.gridTop) / step;
        if (top <= s.gridTop) firstRow = 0;
        if (firstRow < 0) firstRow = 0;
        if (firstRow >= s.rows) continue;
        int lastRow = (bottom - 1 - s.gridTop) / step;
        if (lastRow >= s.rows) lastRow = s.rows - 1;
        if (lastRow < firstRow) continue;
        const int firstSlot = s.firstVisible + firstRow * m_columns;
        int endSlot = s.firstVisible + (lastRow + 1) * m_columns;
        if (endSlot > s.firstVisible + s.count) endSlot = s.firstVisible + s.count;
        if (first < 0) first = firstSlot;
        end = endSlot;
    }
    if (first < 0) return out;
    out.first = first;
    out.count = end - first;
    if (out.count < 0) out.count = 0;
    return out;
}

Rect GridLayout::cellRectBySlot(int slot) const noexcept {
    Rect r;
    if (slot < 0 || slot >= static_cast<int>(m_visible.size())) return r;
    const int stepSize = m_metrics.cellSize + m_metrics.gap;
    if (!sectioned()) {
        r.x = m_viewport.x + (slot % m_columns) * stepSize;
        r.y = m_viewport.y + (slot / m_columns) * stepSize - m_scroll;
        r.w = m_metrics.cellSize;
        r.h = m_metrics.cellSize;
        return r;
    }
    const Section* found = nullptr;
    for (const Section& s : m_sections) {
        if (slot >= s.firstVisible && slot < s.firstVisible + s.count) {
            found = &s;
            break;
        }
    }
    if (!found) return r;
    const int step = m_metrics.cellSize + m_metrics.gap;
    const int local = slot - found->firstVisible;
    const int row = local / m_columns;
    const int col = local % m_columns;
    r.x = m_viewport.x + col * step;
    r.y = m_viewport.y + found->gridTop + row * step - m_scroll;
    r.w = m_metrics.cellSize;
    r.h = m_metrics.cellSize;
    return r;
}

Rect GridLayout::cellRect(int itemIndex) const noexcept {
    return cellRectBySlot(slotOfItem(itemIndex));
}

Rect GridLayout::headerRect(int sectionIndex) const noexcept {
    Rect r;
    if (m_metrics.headerHeight <= 0) return r;
    if (sectionIndex < 0 || sectionIndex >= static_cast<int>(m_sections.size())) return r;
    const Section& s = m_sections[static_cast<std::size_t>(sectionIndex)];
    r.x = m_viewport.x;
    r.y = m_viewport.y + s.headerTop - m_scroll;
    r.w = m_viewport.w;
    r.h = m_metrics.headerHeight;
    return r;
}

int GridLayout::hitTest(int px, int py) const noexcept {
    if (m_visible.empty()) return -1;
    if (px < m_viewport.x || px >= m_viewport.x + m_viewport.w) return -1;
    if (py < m_viewport.y || py >= m_viewport.y + m_viewport.h) return -1;

    const int step = m_metrics.cellSize + m_metrics.gap;
    const int localX = px - m_viewport.x;
    const int col = localX / step;
    if (col >= m_columns) return -1;                       // right-hand padding
    if (localX - col * step >= m_metrics.cellSize) return -1;   // vertical gap

    const int contentY = py - m_viewport.y + m_scroll;

    if (!sectioned()) {
        if (contentY % step >= m_metrics.cellSize) return -1;   // horizontal gap
        const int row = contentY / step;
        if (row < 0 || row >= m_rows) return -1;
        const int slot = row * m_columns + col;
        if (slot < 0 || slot >= static_cast<int>(m_visible.size())) return -1;
        return m_visible[static_cast<std::size_t>(slot)];
    }

    for (const Section& s : m_sections) {
        if (contentY < s.gridTop) return -1;               // header row or its gap
        const int localY = contentY - s.gridTop;
        const int sectionHeight = s.rows * step;
        if (localY >= sectionHeight) continue;             // next section
        if (localY % step >= m_metrics.cellSize) return -1;      // horizontal gap
        const int row = localY / step;
        const int slot = s.firstVisible + row * m_columns + col;
        if (slot >= s.firstVisible + s.count) return -1;   // ragged last row
        return m_visible[static_cast<std::size_t>(slot)];
    }
    return -1;
}

}  // namespace gdut
