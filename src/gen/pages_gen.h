// pages_gen.h - the three text files the tab reads beside uniq-pages.arz:
//   uniq-records.txt   every collectible item record, one per line, in box order
//   uniq-pages.txt     one line per page: index, record, "<label> k/m", box count
//   uniq-groups.txt    the group model: F line (frame), P line (the derived geometry),
//                      V lines (vanilla boxes), then per slot group a G line (index, label,
//                      cols, rows, cellW, cellH, count) and one E line per box (box record,
//                      item record)
//
// Port of tools/build_uniq_db.py's selection, ordering and per-group grid (the cell is the
// largest icon of the group, read from the .tex headers in Items.arc), and of the overlay
// archive it used to only check against: uniq-pages.arz is GENERATED here now, from the same
// geometry, so the frame's location and box count follow the database instead of a shipped
// file. checkPageCapacity() is the read-back: what was written must hold what the text names.
#pragma once

#include "gen/ui_geometry.h"

#include <cstddef>
#include <cstdint>
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
    std::vector<CellGrid> cells;                  // the distinct cell sizes, ascending
    UiGeometry geo;                               // what the P line carries
    std::vector<std::uint8_t> arz;                // uniq-pages.arz, ready to write
    std::size_t arzRecords = 0;                   // records in it: boxes, pages, fillers, frame
    int frameBoxes = 0;                           // boxes the frame carries (the F line's first count)
    int frameX = 0, frameY = 0;                   // the frame's WindowLocationX/Y, from the database
    std::size_t cappedGroups = 0;                 // groups whose rectangle wanted more than the cap
};

// mods, when given, is the override layer the engine will have merged (a /basemods install
// or a custom game); the geometry is then derived through it. Nothing else reads it - which
// items the collection holds is a separate decision from where the grid sits.
bool generatePages(const GameData& g, const std::vector<CatalogueItem>& items,
                   PagesOutput& out, std::string* error, const ModView* mods = nullptr);

// True when the archive at the path holds what the layout wrote: the record count, a frame with
// the right number of boxes, and every box record the text names. Otherwise the error names the
// first group that does not fit and its box counts.
bool checkPageCapacity(const std::string& pagesArzPath, const PagesOutput& out, std::string* error);

} // namespace gen
