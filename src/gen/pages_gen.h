// pages_gen.h - the three text files the tab reads beside uniq-pages.arz:
//   uniq-records.txt   every collectible item record, one per line, in box order
//   uniq-pages.txt     one line per page: index, record, "<label> k/m", box count
//   uniq-groups.txt    the group model: F line (frame), V lines (vanilla boxes), then per slot
//                      group a G line (index, label, cols, rows, cellW, cellH, count) and one
//                      E line per box (box record, item record)
//
// Port of tools/build_uniq_db.py's selection, ordering and per-group grid (the cell is the
// largest icon of the group, read from the .tex headers in Items.arc). uniq-pages.arz itself
// stays a shipped static file: checkPageCapacity() refuses a layout that names a box the
// archive does not have, so a database that outgrew the archive is an error, never a
// truncated file.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace gen {

struct GameData;
struct CatalogueItem;

struct PagesOutput {
    std::string records, pages, groups;           // the three files, ready to write
    std::size_t pageCount = 0, itemCount = 0, skipped = 0;
    struct Group { std::string label; std::size_t entries = 0, pages = 0; };
    std::vector<Group> groupList;
    std::vector<std::string> boxRecords;          // every E line's box record, in order
};

bool generatePages(const GameData& g, const std::vector<CatalogueItem>& items,
                   PagesOutput& out, std::string* error);

// True when every box record the layout names exists in the page archive; otherwise the error
// names the first group that does not fit and its box counts.
bool checkPageCapacity(const std::string& pagesArzPath, const PagesOutput& out, std::string* error);

} // namespace gen
