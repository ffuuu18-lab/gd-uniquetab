// model_test.cpp - console test for the model (catalogue / collection /
// layout).  No engine, no DLL, no game process: it only reads
// data\oracle\catalogue.bin.
//
// Build and run: build_model.bat   (output kept in tests\model_test.out.txt)
// Optional argument: path to catalogue.bin (default data\oracle\catalogue.bin
// relative to the working directory used by build_model.bat).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../src/model/catalogue.h"
#include "../src/model/collection.h"
#include "../src/model/layout.h"

using namespace gdut;

namespace {

int g_checks = 0;

void failed(const char* expr, const char* msg, const char* file, int line) {
    std::printf("\n*** ASSERT FAILED ***\n  %s\n  condition: %s\n  at %s:%d\n",
                msg, expr, file, line);
    std::fflush(stdout);
    std::abort();
}

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        ++g_checks;                                                         \
        if (!(cond)) failed(#cond, (msg), __FILE__, __LINE__);              \
    } while (0)

void checkEq(long long got, long long want, const char* what,
             const char* file, int line) {
    ++g_checks;
    if (got != want) {
        char msg[256];
        std::snprintf(msg, sizeof msg, "%s: got %lld, expected %lld", what, got, want);
        failed("got == want", msg, file, line);
    }
}

#define CHECK_EQ(got, want, what) checkEq((long long)(got), (long long)(want), (what), __FILE__, __LINE__)

void rule(const char* title) {
    std::printf("\n== %s ==\n", title);
}

// Reads a whole file; returns an empty vector on failure.
std::vector<std::uint8_t> readFile(const char* path) {
    std::FILE* fh = nullptr;
#if defined(_MSC_VER)
    if (::fopen_s(&fh, path, "rb") != 0) fh = nullptr;
#else
    fh = std::fopen(path, "rb");
#endif
    std::vector<std::uint8_t> out;
    if (!fh) return out;
    std::fseek(fh, 0, SEEK_END);
    const long size = std::ftell(fh);
    std::rewind(fh);
    if (size > 0) {
        out.resize(static_cast<std::size_t>(size));
        if (std::fread(out.data(), 1, out.size(), fh) != out.size()) out.clear();
    }
    std::fclose(fh);
    return out;
}

std::string rectStr(const Rect& r) {
    char buf[80];
    std::snprintf(buf, sizeof buf, "(x=%d y=%d w=%d h=%d)", r.x, r.y, r.w, r.h);
    return std::string(buf);
}

}  // namespace

int main(int argc, char** argv) {
    const char* path = (argc > 1) ? argv[1] : "data/oracle/catalogue.bin";

    // ------------------------------------------------------------------ load
    rule("catalogue.bin");
    Catalogue cat;
    std::string err;
    if (!cat.loadFromFile(path, &err)) {
        std::printf("load failed: %s\n", err.c_str());
        return 2;
    }
    std::printf("loaded %s\n  format version %u, %zu bytes, %zu items, %zu sets\n",
                path, cat.formatVersion(), cat.byteSize(), cat.itemCount(), cat.setCount());
    CHECK_EQ(cat.formatVersion(), Catalogue::kFormatVersion, "format version");
    CHECK(cat.loaded(), "catalogue reports loaded");
    CHECK_EQ(cat.itemCount(), 4088, "item count");

    // -------------------------------------------------------- group breakdown
    rule("items per slot group");
    std::size_t groupTotal = 0;
    std::size_t equipmentGroups = 0;
    for (int g = 0; g < kSlotGroupCount; ++g) {
        const SlotGroup group = static_cast<SlotGroup>(g);
        const GroupRange r = cat.groupRange(group);
        groupTotal += r.count;
        if (g < static_cast<int>(SlotGroup::Relic)) equipmentGroups += r.count;
        if (r.count == 0) continue;
        std::size_t epic = 0;
        for (std::uint32_t i = 0; i < r.count; ++i) {
            if (cat.item(r.first + i).classification == Classification::Epic) ++epic;
        }
        std::printf("  %2d %-11s %5u  first=%-5u epic=%-4zu legendary=%zu\n",
                    g, slotGroupName(group), r.count, r.first, epic, r.count - epic);
    }
    CHECK_EQ(groupTotal, cat.itemCount(), "sum of group ranges");
    CHECK_EQ(equipmentGroups, 3197, "equipment items (groups Helm..Medal)");
    CHECK_EQ(cat.groupRange(SlotGroup::Relic).count, 91, "relic items");
    CHECK_EQ(cat.equipmentCount(), 3197, "header equipment count");
    CHECK_EQ(cat.relicCount(), 91, "header relic count");
    CHECK_EQ(cat.defaultVisibleCount(), 3288, "header default-visible count");

    std::size_t defaultVisible = 0, legendaryDefault = 0, setPieces = 0, withBitmap = 0;
    for (const ItemView& it : cat.items()) {
        if (it.has(ItemFlag::DefaultVisible)) {
            ++defaultVisible;
            if (it.classification == Classification::Legendary) ++legendaryDefault;
        }
        if (it.has(ItemFlag::SetPiece)) ++setPieces;
        if (it.has(ItemFlag::HasBitmap)) ++withBitmap;
    }
    std::printf("  default visible %zu (legendary %zu), set pieces %zu, with bitmap %zu\n",
                defaultVisible, legendaryDefault, setPieces, withBitmap);
    CHECK_EQ(defaultVisible, 3288, "items flagged DefaultVisible");
    CHECK_EQ(setPieces, 757, "items flagged SetPiece");

    // --------------------------------------------------------- record lookup
    rule("record -> index map");
    const char* kKnown = "records/items/gearhead/d208_head.dbr";
    const int known = cat.indexOfRecord(kKnown);
    CHECK(known >= 0, "known record resolves");
    std::printf("  %s -> #%d \"%.*s\" (%s, %s, level %u, set %d)\n",
                kKnown, known,
                static_cast<int>(cat.item(known).name.size()), cat.item(known).name.data(),
                slotGroupName(cat.item(known).group),
                classificationName(cat.item(known).classification),
                cat.item(known).levelRequirement, cat.item(known).setIndex);
    CHECK(cat.item(known).name == "Horns of Korvaak", "known record display name");
    CHECK_EQ(cat.indexOfRecord("records/items/does_not_exist.dbr"), -1, "unknown record lookup");
    CHECK_EQ(cat.indexOfRecord(""), -1, "empty record lookup");
    for (std::size_t i = 0; i < cat.itemCount(); i += 617) {
        CHECK_EQ(cat.indexOfRecord(cat.item(i).record), static_cast<int>(i), "round-trip lookup");
    }

    // ------------------------------------------------------------ item sets
    rule("item sets");
    std::size_t memberLinks = 0;
    for (std::size_t s = 0; s < cat.setCount(); ++s) {
        const SetView& set = cat.sets()[s];
        memberLinks += set.memberCount;
        for (std::uint32_t m = 0; m < set.memberCount; ++m) {
            const std::uint32_t idx = set.members[m];
            CHECK(idx < cat.itemCount(), "set member index in range");
            CHECK_EQ(cat.item(idx).setIndex, static_cast<int>(s), "member points back at its set");
        }
    }
    std::printf("  %zu sets, %zu member links; first set \"%.*s\" (%u pieces)\n",
                cat.setCount(), memberLinks,
                static_cast<int>(cat.sets()[0].name.size()), cat.sets()[0].name.data(),
                cat.sets()[0].memberCount);
    CHECK_EQ(cat.setCount(), 201, "set count");
    CHECK_EQ(memberLinks, setPieces, "set member links == set pieces");

    // -------------------------------------------------- rejecting bad buffers
    rule("loader rejects damaged files");
    {
        std::vector<std::uint8_t> tiny(16, 0);
        Catalogue bad;
        std::string why;
        CHECK(!bad.loadFromMemory(tiny, &why), "truncated file rejected");
        std::printf("  truncated: %s\n", why.c_str());

        std::vector<std::uint8_t> junk(200, 0);
        std::memcpy(junk.data(), "XXXX", 4);
        CHECK(!bad.loadFromMemory(junk, &why), "bad magic rejected");
        std::printf("  bad magic: %s\n", why.c_str());

        // valid file with the version bumped
        const std::vector<std::uint8_t> raw = readFile(path);
        CHECK_EQ(raw.size(), cat.byteSize(), "reread size");
        std::vector<std::uint8_t> wrongVersion = raw;
        wrongVersion[4] = 99;
        CHECK(!bad.loadFromMemory(wrongVersion, &why), "wrong version rejected");
        std::printf("  wrong version: %s\n", why.c_str());

        std::vector<std::uint8_t> wildOffset = raw;
        wildOffset[24] = 0xFF; wildOffset[25] = 0xFF; wildOffset[26] = 0xFF; wildOffset[27] = 0x7F;
        CHECK(!bad.loadFromMemory(wildOffset, &why), "wild item-table offset rejected");
        std::printf("  wild offset: %s\n", why.c_str());

        // a string reference past the end of the string table
        std::vector<std::uint8_t> wildString = raw;
        const std::uint32_t itemOff = 80 + 29 * 8;   // header + group table
        wildString[itemOff + 0] = 0xFF; wildString[itemOff + 1] = 0xFF;
        wildString[itemOff + 2] = 0xFF; wildString[itemOff + 3] = 0x7F;
        CHECK(!bad.loadFromMemory(wildString, &why), "wild string reference rejected");
        std::printf("  wild string ref: %s\n", why.c_str());

        // every 32-bit header field, forced to 0 and to 0xFFFFFFFF: the loader
        // must either refuse the file or come back with a fully consistent one,
        // and must never read outside the buffer.
        int refused = 0, accepted = 0;
        for (int field = 1; field < 20; ++field) {
            const std::uint32_t poison[2] = {0u, 0xFFFFFFFFu};
            for (int p = 0; p < 2; ++p) {
                std::vector<std::uint8_t> mutated = raw;
                std::memcpy(mutated.data() + field * 4, &poison[p], 4);
                Catalogue probe;
                std::string ignored;
                if (!probe.loadFromMemory(mutated, &ignored)) {
                    ++refused;
                    CHECK(!probe.loaded(), "refused file leaves an empty catalogue");
                    continue;
                }
                ++accepted;
                for (std::size_t i = 0; i < probe.itemCount(); ++i) {
                    const ItemView& v = probe.item(i);
                    CHECK(!v.record.empty(), "accepted file has no empty record");
                    CHECK(v.setIndex < static_cast<int>(probe.setCount()), "set index in range");
                    CHECK(probe.indexOfRecord(v.record) == static_cast<int>(i), "lookup consistent");
                }
                for (const SetView& s : probe.sets()) {
                    for (std::uint32_t m = 0; m < s.memberCount; ++m) {
                        CHECK(s.members[m] < probe.itemCount(), "set member in range");
                    }
                }
            }
        }
        std::printf("  header field sweep: %d variants refused, %d accepted and self-consistent\n",
                    refused, accepted);
        CHECK_EQ(refused + accepted, 19 * 2, "every header field probed");
        CHECK(refused > 0, "the sweep refuses something");
    }

    // ------------------------------------------------- layout: transfer grid
    rule("layout: game transfer grid (x=105 y=4 w=320 h=608)");
    GridLayout grid;
    grid.setCatalogue(&cat);
    Metrics metrics;                     // 40 px cells, 2 px gap, no headers
    grid.setMetrics(metrics);
    Filters filters;                     // default: equipment + relics, both tiers
    const Viewport transferGrid{105, 4, 320, 608};
    grid.relayout(transferGrid, filters);
    std::printf("  visible items %d, columns %d, rows %d, contentHeight %d, maxScroll %d\n",
                grid.visibleItemCount(), grid.columns(), grid.rows(),
                grid.contentHeight(), grid.maxScroll());
    CHECK_EQ(grid.visibleItemCount(), 3288, "default filter shows equipment + relics");
    CHECK_EQ(grid.columns(), 7, "columns in a 320 px wide grid");
    CHECK_EQ(grid.rows(), (3288 + 6) / 7, "rows");
    CHECK_EQ(grid.contentHeight(), grid.rows() * 42 - 2, "content height");
    CHECK_EQ(grid.maxScroll(), grid.contentHeight() - 608, "max scroll");
    CHECK_EQ(grid.scrollOffset(), 0, "initial scroll");

    VisibleRange vr = grid.visibleRange();
    std::printf("  visibleRange: first=%d count=%d (rows on screen %d)\n",
                vr.first, vr.count, (vr.count + grid.columns() - 1) / grid.columns());
    CHECK_EQ(vr.first, 0, "first visible slot at scroll 0");
    CHECK_EQ(vr.count, 15 * 7, "visible cells in 608 px");

    // hit tests: centre of the first visible cell, centre of the last, a gap
    {
        const int firstItem = grid.itemAtSlot(vr.first);
        const Rect r0 = grid.cellRectBySlot(vr.first);
        std::printf("  first visible cell slot %d item #%d %s \"%.*s\"\n",
                    vr.first, firstItem, rectStr(r0).c_str(),
                    static_cast<int>(cat.item(firstItem).name.size()),
                    cat.item(firstItem).name.data());
        CHECK_EQ(r0.x, 105, "first cell x");
        CHECK_EQ(r0.y, 4, "first cell y");
        CHECK_EQ(r0.w, 40, "cell width");
        CHECK_EQ(grid.hitTest(r0.x + r0.w / 2, r0.y + r0.h / 2), firstItem, "hit centre of first cell");
        CHECK(grid.cellRect(firstItem).contains(r0.x + 1, r0.y + 1), "cellRect(itemIndex) agrees");

        // 608 px / 42 px is 14.5 rows, so the last visible row is clipped by the
        // viewport: visibleRange() includes it (it has to be drawn) but only its
        // top strip is clickable.
        const int lastSlot = vr.first + vr.count - 1;
        const int lastItem = grid.itemAtSlot(lastSlot);
        const Rect rl = grid.cellRectBySlot(lastSlot);
        std::printf("  last visible cell  slot %d item #%d %s \"%.*s\" (clipped by %d px)\n",
                    lastSlot, lastItem, rectStr(rl).c_str(),
                    static_cast<int>(cat.item(lastItem).name.size()),
                    cat.item(lastItem).name.data(),
                    (rl.y + rl.h) - (transferGrid.y + transferGrid.h));
        CHECK(rl.y < transferGrid.y + transferGrid.h, "last visible cell starts inside the viewport");
        CHECK_EQ(grid.hitTest(rl.x + rl.w / 2, rl.y + 2), lastItem, "hit the visible strip of the last cell");
        CHECK_EQ(grid.hitTest(rl.x + rl.w / 2, rl.y + rl.h / 2), -1, "clipped part of the last cell is outside");

        // last cell that is fully inside the viewport
        int lastFull = lastSlot;
        while (lastFull > 0 &&
               grid.cellRectBySlot(lastFull).y + metrics.cellSize > transferGrid.y + transferGrid.h) {
            --lastFull;
        }
        const Rect rf = grid.cellRectBySlot(lastFull);
        const int fullItem = grid.itemAtSlot(lastFull);
        std::printf("  last fully visible slot %d item #%d %s \"%.*s\"\n",
                    lastFull, fullItem, rectStr(rf).c_str(),
                    static_cast<int>(cat.item(fullItem).name.size()),
                    cat.item(fullItem).name.data());
        CHECK_EQ(grid.hitTest(rf.x + rf.w / 2, rf.y + rf.h / 2), fullItem, "hit centre of the last full cell");
        CHECK(rf.y + rf.h <= transferGrid.y + transferGrid.h, "last full cell fits the viewport");

        const int gapX = r0.x + metrics.cellSize;          // between column 0 and 1
        const int gapY = r0.y + metrics.cellSize;          // between row 0 and 1
        std::printf("  hitTest gaps: column gap at x=%d -> %d, row gap at y=%d -> %d\n",
                    gapX, grid.hitTest(gapX, r0.y + 5), gapY, grid.hitTest(r0.x + 5, gapY));
        CHECK_EQ(grid.hitTest(gapX, r0.y + 5), -1, "column gap is not a cell");
        CHECK_EQ(grid.hitTest(r0.x + 5, gapY), -1, "row gap is not a cell");
        CHECK_EQ(grid.hitTest(transferGrid.x - 1, r0.y + 5), -1, "left of the viewport");
        CHECK_EQ(grid.hitTest(transferGrid.x + transferGrid.w, r0.y + 5), -1, "right of the viewport");
        CHECK_EQ(grid.hitTest(r0.x + 5, transferGrid.y + transferGrid.h), -1, "below the viewport");
        // 7 columns x 42 = 294, so x 399..424 is right-hand padding
        CHECK_EQ(grid.hitTest(transferGrid.x + 7 * 42, r0.y + 5), -1, "padding right of the last column");
    }

    // ------------------------------------------------------------- scrolling
    rule("scrolling");
    grid.scrollBy(1000000);
    std::printf("  scrolled to end: offset %d (max %d)\n", grid.scrollOffset(), grid.maxScroll());
    CHECK_EQ(grid.scrollOffset(), grid.maxScroll(), "scroll clamps to the end");
    vr = grid.visibleRange();
    std::printf("  visibleRange at end: first=%d count=%d, last slot %d of %d\n",
                vr.first, vr.count, vr.first + vr.count - 1, grid.visibleItemCount() - 1);
    CHECK_EQ(vr.first + vr.count, grid.visibleItemCount(), "last item visible at the end");
    {
        const int lastSlot = grid.visibleItemCount() - 1;
        const Rect rl = grid.cellRectBySlot(lastSlot);
        CHECK_EQ(grid.hitTest(rl.x + 5, rl.y + 5), grid.itemAtSlot(lastSlot), "hit the last item at the end");
        CHECK(rl.y + rl.h <= transferGrid.y + transferGrid.h, "last cell inside the viewport at max scroll");
    }
    grid.scrollBy(-1000000);
    std::printf("  scrolled back: offset %d\n", grid.scrollOffset());
    CHECK_EQ(grid.scrollOffset(), 0, "scroll clamps to the start");
    vr = grid.visibleRange();
    CHECK_EQ(vr.first, 0, "back at the top");
    grid.scrollTo(420);
    CHECK_EQ(grid.scrollOffset(), 420, "scrollTo inside the range");
    CHECK_EQ(grid.visibleRange().first, 10 * 7, "first visible row after scrolling 10 rows");
    grid.scrollTo(0);

    // -------------------------------------------- layout: 900x600 with headers
    rule("layout: 900x600 window with group headers");
    GridLayout wide;
    wide.setCatalogue(&cat);
    Metrics wideMetrics;
    wideMetrics.headerHeight = 18;
    wide.setMetrics(wideMetrics);
    const Viewport window{0, 0, 900, 600};
    wide.relayout(window, filters);
    std::printf("  visible items %d, columns %d, rows %d, sections %zu, contentHeight %d\n",
                wide.visibleItemCount(), wide.columns(), wide.rows(),
                wide.sections().size(), wide.contentHeight());
    CHECK_EQ(wide.columns(), (900 + 2) / 42, "columns in a 900 px wide grid");
    CHECK_EQ(wide.sections().size(), 24, "one section per non-empty default group");
    {
        int expectedRows = 0, expectedHeight = 0;
        for (const Section& s : wide.sections()) {
            const int rows = (s.count + wide.columns() - 1) / wide.columns();
            expectedRows += rows;
            expectedHeight += 18 + 2 + rows * 42;
        }
        CHECK_EQ(wide.rows(), expectedRows, "rows over all sections");
        CHECK_EQ(wide.contentHeight(), expectedHeight - 2, "content height with headers");
    }
    for (std::size_t i = 0; i < wide.sections().size(); ++i) {
        const Section& s = wide.sections()[i];
        std::printf("    %-11s %5d items, %3d rows, headerTop %6d, gridTop %6d\n",
                    slotGroupName(s.group), s.count, s.rows, s.headerTop, s.gridTop);
        CHECK_EQ(s.gridTop, s.headerTop + 20, "header occupies 18 px + 2 px gap");
        CHECK_EQ(wide.hitTest(10, s.headerTop - wide.scrollOffset() + 5), -1,
                 "header row is not a cell");
    }
    {
        const Rect h0 = wide.headerRect(0);
        std::printf("  headerRect(0) %s\n", rectStr(h0).c_str());
        CHECK_EQ(h0.h, 18, "header rect height");
        CHECK_EQ(h0.w, 900, "header rect width");
        const VisibleRange wr = wide.visibleRange();
        std::printf("  visibleRange: first=%d count=%d\n", wr.first, wr.count);
        CHECK_EQ(wr.first, 0, "first slot visible");
        CHECK(wr.count > 0 && wr.count <= wide.visibleItemCount(), "sane visible count");
        const Rect r0 = wide.cellRectBySlot(0);
        CHECK_EQ(r0.y, 20, "first cell sits under the first header");
        CHECK_EQ(wide.hitTest(r0.x + 5, r0.y + 5), wide.itemAtSlot(0), "hit the first cell");
    }

    // ------------------------------------------------------------ collection
    rule("collection: ownership events");
    Collection owned;
    owned.reset(&cat);
    CHECK_EQ(owned.ownedCount(), 0, "empty collection");

    const int idxA = known;                                   // Horns of Korvaak
    const std::string recA(cat.item(idxA).record);
    const int idxB = cat.indexOfRecord(cat.item(0).record);
    const std::string recB(cat.item(0).record);

    owned.onItemAdded(1001, recA);
    CHECK_EQ(owned.owned(idxA), 1, "one copy after the first add");
    CHECK_EQ(owned.ownedCount(), 1, "one distinct item owned");
    CHECK(owned.isOwned(idxA), "isOwned");

    owned.onItemAdded(1001, recA);                            // duplicate id
    CHECK_EQ(owned.owned(idxA), 1, "duplicate id does not double count");
    CHECK_EQ(owned.duplicateIdEvents(), 1, "duplicate id counted");

    owned.onItemAdded(1002, recA);                            // second copy
    CHECK_EQ(owned.owned(idxA), 2, "two copies");
    CHECK_EQ(owned.ownedCount(), 1, "still one distinct item");
    CHECK_EQ(owned.totalItems(), 2, "two copies tracked");

    owned.onItemAdded(2001, "records/items/gearhead/not_a_real_record.dbr");
    owned.onItemAdded(2002, "");
    std::printf("  unknown record events %llu (%zu distinct), duplicate ids %llu\n",
                static_cast<unsigned long long>(owned.unknownRecordEvents()),
                owned.distinctUnknownRecords(),
                static_cast<unsigned long long>(owned.duplicateIdEvents()));
    CHECK_EQ(owned.unknownRecordEvents(), 2, "unknown records counted");
    CHECK_EQ(owned.distinctUnknownRecords(), 2, "distinct unknown records");
    CHECK_EQ(owned.ownedCount(), 1, "unknown records do not change ownership");

    owned.onItemRemoved(1002);
    CHECK_EQ(owned.owned(idxA), 1, "removal drops one copy");
    owned.onItemRemoved(999999);                              // never seen
    CHECK_EQ(owned.owned(idxA), 1, "unknown id removal is a no-op");
    owned.onItemRemoved(2001);                                // unknown record's id
    CHECK_EQ(owned.ownedCount(), 1, "unknown id removal keeps the count");
    owned.onItemRemoved(1001);
    CHECK_EQ(owned.owned(idxA), 0, "last copy removed");
    CHECK_EQ(owned.ownedCount(), 0, "nothing owned");
    CHECK_EQ(owned.trackedIds(), 0, "no live ids");

    // an id recycled onto a different record
    owned.onItemAdded(3000, recA);
    owned.onItemAdded(3000, recB);
    CHECK_EQ(owned.owned(idxA), 0, "recycled id released the old record");
    CHECK_EQ(owned.owned(idxB), 1, "recycled id owns the new record");
    owned.clear();
    CHECK_EQ(owned.ownedCount(), 0, "clear()");
    CHECK_EQ(owned.trackedIds(), 0, "clear() drops ids");

    // ------------------------------------------------------- owned filtering
    rule("filters");
    // four marked through the real event path, one through the test helper
    const int marked[5] = {0, 100, 1000, 3000, 3265};
    for (int i = 0; i < 4; ++i) {
        owned.onItemAdded(static_cast<std::uint32_t>(4000 + i), cat.item(marked[i]).record);
        CHECK_EQ(owned.owned(marked[i]), 1, "onItemAdded marks the record owned");
    }
    CHECK(owned.setOwnedByRecord(cat.item(marked[4]).record, 1), "setOwnedByRecord on a catalogue record");
    CHECK_EQ(owned.owned(marked[4]), 1, "setOwnedByRecord marks the record owned");
    CHECK_EQ(owned.ownedCount(), 5, "five items owned");
    CHECK(!owned.setOwnedByRecord("records/items/nope.dbr", 1), "setOwnedByRecord rejects unknown");

    Filters ownedOnly = filters;
    ownedOnly.owned = OwnedFilter::OwnedOnly;
    grid.relayout(transferGrid, ownedOnly, &owned);
    std::printf("  owned-only: %d items, rows %d, contentHeight %d\n",
                grid.visibleItemCount(), grid.rows(), grid.contentHeight());
    CHECK_EQ(grid.visibleItemCount(), 5, "owned-only filter");
    for (int i = 0; i < grid.visibleItemCount(); ++i) {
        CHECK(owned.owned(grid.itemAtSlot(i)) > 0, "every shown item is owned");
    }
    CHECK_EQ(grid.contentHeight(), 40, "one row of owned items");
    CHECK_EQ(grid.maxScroll(), 0, "no scrolling needed");

    Filters missingOnly = filters;
    missingOnly.owned = OwnedFilter::MissingOnly;
    grid.relayout(transferGrid, missingOnly, &owned);
    std::printf("  missing-only: %d items\n", grid.visibleItemCount());
    CHECK_EQ(grid.visibleItemCount(), 3288 - 5, "missing-only filter");

    Filters legendaryOnly = filters;
    legendaryOnly.classificationMask = 1u << static_cast<int>(Classification::Legendary);
    grid.relayout(transferGrid, legendaryOnly);
    std::printf("  legendary-only: %d items\n", grid.visibleItemCount());
    CHECK_EQ(grid.visibleItemCount(), static_cast<int>(legendaryDefault), "classification mask");

    Filters helmsOnly = filters;
    helmsOnly.groupMask = groupBit(SlotGroup::Helm);
    grid.relayout(transferGrid, helmsOnly);
    std::printf("  helms only: %d items\n", grid.visibleItemCount());
    CHECK_EQ(grid.visibleItemCount(), cat.groupRange(SlotGroup::Helm).count, "slot-group mask");

    Filters named = filters;
    named.nameQuery = "KoRvAaK";
    grid.relayout(transferGrid, named);
    std::printf("  name contains \"korvaak\" (case-insensitive): %d items, e.g. \"%.*s\"\n",
                grid.visibleItemCount(),
                static_cast<int>(cat.item(grid.itemAtSlot(0)).name.size()),
                cat.item(grid.itemAtSlot(0)).name.data());
    CHECK(grid.visibleItemCount() > 0, "name filter finds something");
    for (int i = 0; i < grid.visibleItemCount(); ++i) {
        CHECK(containsNoCase(cat.item(grid.itemAtSlot(i)).name, "korvaak"), "name filter is exact");
    }
    named.nameQuery = "zzzz-no-such-item";
    grid.relayout(transferGrid, named);
    CHECK_EQ(grid.visibleItemCount(), 0, "name filter can empty the grid");
    CHECK_EQ(grid.contentHeight(), 0, "empty grid has no content");
    CHECK_EQ(grid.hitTest(200, 200), -1, "empty grid hit test");
    CHECK_EQ(grid.visibleRange().count, 0, "empty grid visible range");

    Filters everything = filters;
    everything.groupMask = kAllGroupsMask;
    everything.requireDefaultVisible = false;
    grid.relayout(transferGrid, everything);
    std::printf("  no filters at all: %d items\n", grid.visibleItemCount());
    CHECK_EQ(grid.visibleItemCount(), cat.itemCount(), "unfiltered layout shows everything");

    std::printf("\n%d checks passed\n", g_checks);
    std::printf("MODEL TESTS PASSED\n");
    return 0;
}
