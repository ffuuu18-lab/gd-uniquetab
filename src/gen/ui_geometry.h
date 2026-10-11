// ui_geometry.h - the collection grid's rectangle and its plate, derived from the database the
// engine merges instead of held as constants, so a stash-UI mod that moves or enlarges the
// caravan is followed without a per-mod profile.
//
// Port of tools/ui_geometry.py; that file's header is the authoritative description of WHICH
// records are read and WHY all of them have to be. The short version: the two stash-UI families
// move the same page by opposite means - one moves the 24 reagent boxes and leaves the
// sub-window, the other moves the sub-window and leaves the boxes - so both signals are read,
// and the plate's size comes from the texture's own header, never from the window extent.
//
// The origin is where a NORMAL TAB'S item grid starts: caravan_window -> StashWindow ->
// InventoryDBR, translated into the materials page's space by the two WindowLocations, taken
// when that anchor falls inside the page's own plate and the page's reagent-box bounding box
// otherwise. The boxes say where the materials are, not where the page is free. On top of that
// the grid clears the mod's own header band (the label and the 26-button pad, y 8..68 of the
// page), which is why the first box row is at y 76 on every view.
//
// The convention, because one pixel hangs on it: the usable rectangle is measured from the
// PLATE's top-left, i.e. usableW = plateW - (x0 - bitmapPositionX). Vanilla's cover sits at
// 0,1, so the derived height is 552 where the old constant was 551. No cell height in play
// (32, 64, 96, 128 on a 2 px row gap) divides the two differently, so the shipped layout is
// unchanged; the pad is kept because it is the arithmetic that reproduces the modded families.
#pragma once

#include "gen/arc_reader.h"
#include "gen/arz_reader.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gen {

struct GameData;

// The layers a mod puts on top of the base game, in the engine's own order (the last that has
// a record wins). Two shapes, because the engine has two:
//   /basemods       <game>\mods\database.arz and every <game>\mods\*.arc
//   a custom game   <dir>\database\*.arz and <dir>\resources\*.arc
// An empty view is the vanilla install, which is what every caller gets by default.
class ModView {
public:
    bool loadBasemods(const std::string& gameDir, std::string* error);
    bool loadCustom(const std::string& modDir, std::string* error);
    bool empty() const { return m_arz.empty() && m_arc.empty(); }
    // The .arc files that could not be read and were skipped ("path: reason"), in load order.
    const std::vector<std::string>& skipped() const { return m_skipped; }
    std::size_t arcCount() const { return m_arc.size(); }
    const std::string& label() const { return m_label; }
    // The layers themselves, in load order (the last .arz that has a record wins): the
    // catalogue walks the .arz records and reads the mod's text archives through these.
    const std::vector<ArzArchive>& arzLayers() const { return m_arz; }
    const std::vector<ArcArchive>& arcLayers() const { return m_arc; }
    // The winning override of a record, or false when no layer has it.
    bool get(const std::string& name, ArzRecord& out) const;
    // Does any layer touch the caravan at all - a records/ui/caravan/* override, or a texture
    // whose name names the caravan? False means the derived geometry is what the layer below
    // would have produced, so a custom game that answers false needs no view of its own. It is
    // deliberately generous: a false positive costs one generation, a false negative would put
    // the grid where no record says.
    bool touchesCaravan() const;
    // A mod .arc is rooted at its own prefix, so a bitmapName is tried whole, without its
    // first path element, and by leaf name.
    bool texSize(const std::string& bitmap, int& w, int& h) const;
    // The whole texture, looked up the same way. False when no layer has it.
    bool texRead(const std::string& bitmap, std::vector<std::uint8_t>& out) const;

private:
    std::vector<ArzArchive> m_arz;
    std::vector<ArcArchive> m_arc;
    std::vector<std::string> m_skipped;
    std::string m_label;
    void addArc(const std::string& path);
};

// WHICH database the outputs were derived from. The engine builds one in exactly this order:
// Engine::LoadMainDatabase merges the base game, the expansions and - when /basemods is on the
// command line - <game>\mods\database.arz; a custom game then loads <game>\mods\<name>\database
// \*.arz on top of THAT through Engine::LoadDatabase, after LoadMainDatabase has returned. So a
// custom game does not replace the basemod layer, it sits above it, and a view carries both.
struct ViewSpec {
    bool basemods = false;
    std::string customDir;    // <game>\mods\<name>; empty = the start-up view

    // vanilla | basemods | custom-<name> | basemods+custom-<name>. A folder name: the custom
    // part is the mod folder's leaf, lowercased, with anything but [a-z0-9._-] folded to '-'.
    std::string name() const;
    // What a plate file name carries so two mods with the same cover size cannot collide:
    // empty for the start-up view (the shipped names are kept exactly), the custom leaf
    // otherwise.
    std::string tag() const;
    bool startup() const { return customDir.empty(); }
};

// The mod folder of a custom game's archive, from the path the engine hands
// Engine::LoadDatabase: "[<dir>/]mods/<name>/database/<file>.arz" in any case, with '/' or '\',
// relative to the game folder (the engine's own form, "mods/<name>/database/<name>.arz") or
// absolute. Returns "<gameDir>\...\mods\<name>" (the path's own prefix when it is absolute),
// with backslashes and no "." segments, or empty when the path is not a custom game's archive:
// the base game's and the expansions' own, the Crucible's DLC parts at the game root, and
// <game>\mods\database.arz, which is the /basemods layer.
std::string customGameFolder(const std::string& enginePath, const std::string& gameDir);

// customGameFolder against each base in turn, kept only where the archive itself is on disk:
// the engine opened it relative to its working directory, which has been the game folder in
// every launch seen so far. Empty when no base holds it or the path is not a custom game's.
std::string findCustomGameFolder(const std::string& enginePath,
                                 const std::vector<std::string>& bases);

// The override layers of a view, in the engine's order (the last that has a record wins):
// the basemod layer first, then the custom game's. False with a reason when a layer cannot be
// read; `out` is then whatever was loaded before the failure.
bool loadView(const std::string& gameDir, const ViewSpec& view, ModView& out, std::string* error);

struct UiGeometry {
    int x0 = 105, y0 = 76;             // the grid origin, window units
    int usableW = 333, usableH = 552;  // the rectangle the grid is laid out in
    int plateW = 438, plateH = 627;    // the cover texture's real pixel size
    int bmpX = 0, bmpY = 1;            // the cover's bitmapPositionX/Y (plate_gen translates by it)
    std::string plate;                 // its bitmapName
    std::string tabStrip;              // "left" (vanilla) or "top" (both modded families)
    std::string note;                  // one line for the log

    // The P line of uniq-groups.txt, without its line ending.
    std::string line() const;
    bool vanillaPlate() const { return plateW == 438 && plateH == 627; }
};

// One cell size the layout uses, with the grid the derived rectangle gives it. The page layout
// computes these; the plates are painted from them, one plate per cell size.
struct CellGrid {
    int cellW = 0, cellH = 0, cols = 0, rows = 0;
};

// Reads the merged view out of g and fills out. False with a reason when a record or the
// cover texture cannot be read; the caller then keeps the vanilla defaults above.
bool deriveGeometry(const GameData& g, UiGeometry& out, std::string* error);
// The materials page's reagent box records through the merged view, as (itemBoxX, itemBoxY) in
// record px. The pad's materials-page rule is checked against these offline.
bool materialBoxPositions(const GameData& g, const ModView& mods,
                          std::vector<std::pair<int, int> >& out, std::string* error);
bool deriveGeometry(const GameData& g, const ModView& mods, UiGeometry& out,
                    std::string* error);

} // namespace gen
