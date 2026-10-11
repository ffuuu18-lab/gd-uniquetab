// plate_gen.h - paints the collection's cover plates at generation time, one per cell size.
//
// Port of tools/make_plates.py; that file's header is the authoritative description of the
// painting method (the lattice, the 9-slice alpha stamp, the ornament) and of the rule below.
// For the same game files the two produce the same bytes, which is what the vanilla gate in
// tools/test_catalogue.cpp asserts against the oracle set in data\oracle\plates\.
//
// THE GROUND RULE: the ground is what a NORMAL STASH TAB shows under the same screen rectangle.
// That is the stash cover - caravan_window -> TransferWindow -> TransferPlate
// (caravan_transfercoverimage.dbr, the record our own frame already uses as its plate) - read
// through the merged view, never the materials cover, whose art is Crate's 24 hand-placed frames
// on vanilla and a whole baked reagent cluster under X-Mod. Which of its pixels:
//   * the two covers cover the same rectangle (same width, same absolute left edge): the WHOLE
//     stash texture, its last row repeated (or its bottom cut) to the materials cover's height,
//     because the widget froze its destination rectangle from that size. Vanilla (438x626 ->
//     438x627) and X-Mod (975x968, both at absolute 0,62) take this branch;
//   * otherwise the pixels under the materials cover's rectangle, translated through the two
//     windows' WindowLocationX/Y and the two covers' bitmapPositionX/Y:
//         off = (matWindow + matBitmapPos) - (stashWindow + stashBitmapPos)
//     Baul takes this branch - a 346x625 cut of the 952x1007 whole-window art at 605,71;
//   * and when that rectangle is not inside the stash cover, or the stash cover is a surface
//     this reader does not handle, the materials cover itself, with the reason in the log.
// The lattice erasure and the ornament pass are MEASURED ON THE VANILLA TEXTURE and run only
// when the ground IS that texture.
//
// A MODDED GROUND IS A FLAT FILL. Whenever the rule above does not land on the vanilla stash
// texture, not one pixel of the mod's art is copied: the plate is filled with a single opaque
// colour and the frames are stamped on that. A mod's cover carries its own baked box lattice
// (X-Mod's is 32 px), which our frames are not aligned to and cannot be, so the two grids
// interleave; the mod's own window frame surrounds the page anyway. The colour is the vanilla
// ground's own tone, sampled at generation from ui/caravan/caravan_transfercoverimage.tex (the
// vanilla copy out of UI.arc, never a mod's) at 312,180 - the centre of a lattice cell, 16 px
// from every line, and the calmest such point on the texture: RGB 06 06 06, which is also the
// median of the whole non-gold interior. uniquetab.ini's `plate_ground` (a hex RRGGBB, empty =
// the sampled colour) replaces it; it changes only this fill and never a vanilla plate.
//
// The frames are always stamped from a 9-slice of one real vanilla frame out of UI.arc: a mod's
// cover has no frame of ours at those coordinates, so cutting the donor from the mod's own art
// yields an empty stamp and a plate with no grid on it.
#pragma once

#include "gen/ui_geometry.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gen {

struct GameData;

// A decoded surface, RGBA, row-major. The painter works on these and the test harness reads
// the plates and the covers back through the two calls below.
struct PlateSurface {
    int w = 0, h = 0;
    std::vector<std::uint8_t> px;
    void resize(int ww, int hh) {
        w = ww;
        h = hh;
        px.assign(std::size_t(ww) * hh * 4, 0);
    }
    std::uint8_t* at(int x, int y) { return &px[(std::size_t(y) * w + x) * 4]; }
    const std::uint8_t* at(int x, int y) const { return &px[(std::size_t(y) * w + x) * 4]; }
};

// TEX v2 around an uncompressed 24/32-bit DDSR surface, which is what every caravan cover is.
// False with the reason for a compressed, mipmapped or truncated one.
bool plateDecodeTex(const std::vector<std::uint8_t>& blob, PlateSurface& out, std::string* why);
// A bitmapName out of the merged view: a mod .arc first (rooted at its own prefix), then UI.arc.
bool plateReadCover(const GameData& g, const ModView* mods, const std::string& bitmap,
                    PlateSurface& out, std::string* why);

struct PlateReport {
    std::string ground;                    // the one line the ground rule decided
    std::string fill;                      // the flat-ground line, empty on the vanilla view
    std::uint32_t fillRgb = 0;             // 0xRRGGBB, valid while `flat`
    bool flat = false;                     // the plate was filled, not painted on mod art
    std::vector<std::string> warnings;     // a refused surface, a failed write
    std::size_t painted = 0, frames = 0;
};

// Handed each finished plate: the resource leaf name ("uniq_plate_32x32" on the vanilla cover
// size, "uniq_plate_32x32_975x968" on any other) and the TEX bytes. One plate is alive at a
// time - a 975x968 surface is 3.8 MB - so the sink writes or compares and does not keep them.
// Returning false stops the walk and fails the call.
typedef bool (*PlateSink)(const std::string& name, const std::vector<std::uint8_t>& tex,
                          const CellGrid& cell, int frames, void* ctx);

// Paints one plate per cell. mods may be null (the vanilla view). groundHex is uniquetab.ini's
// plate_ground: six hex digits RRGGBB (a leading # or 0x is accepted) override the sampled flat
// colour on a modded view; anything else, empty included, samples it. False with a reason when
// the ground, the donor or a record could not be read; the caller then keeps the plates it has.
// plateTag: empty for the start-up view, so the shipped names are kept exactly; a custom game's
// view passes its own tag, which is appended ("uniq_plate_32x32_346x625_lootascension") because
// the cover size alone collides when two mods share one.
// layoutHash: empty for the plates of the DERIVED grid (the names above). A user grid's plates
// (ensureLayoutPlates) pass the eight hex digits of their hash and are always named with the
// cover and the tag: "uniq_plate_64x64_438x627_L1a2b3c4d", "uniq_plate_32x32_974x659_rot_L...".
// geo.x0/y0 is then the user grid's origin and `cells` its cols x rows.
// THE LATTICE: every one of a cell's cols x rows (utGridFit's, the numbers the page lays its
// boxes out in) is framed - ut_layout.h utLatticeFrame, which cuts a frame to the plate where its
// 2 px outset crosses the edge instead of dropping the cell. rowGap: the F line's (negative = the
// view's ut::kUtRowGap, which is what every generation writes).
bool generatePlates(const GameData& g, const ModView* mods, const UiGeometry& geo,
                    const std::vector<CellGrid>& cells, const std::string& groundHex,
                    const std::string& plateTag, PlateSink sink, void* ctx, PlateReport& rep,
                    std::string* error, const std::string& layoutHash = std::string(),
                    int rowGap = -1);

// The leaf name (no folder, no .tex) of one plate - the rule generatePlates and ut_plate.cpp share.
std::string plateLeafName(int cellW, int cellH, int plateW, int plateH, const std::string& plateTag,
                          const std::string& layoutHash);

// Writes the set into dir (the engine's texture override root, <game>\settings\ui\caravan).
// Only files named uniq_plate_*.tex are ever written there, a plate whose bytes are already on
// disk is not rewritten, and each is written as <name>.tex.tmp first: `renames` receives the
// tmp -> final pairs so they land with the rest of the generation's outputs. A folder that
// cannot be written is a warning, not a failure - the plates already installed are then kept.
struct PlateWriter {
    std::string dir;
    std::vector<std::pair<std::string, std::string> > renames;   // tmp, final
    std::size_t written = 0, same = 0, failed = 0;
    std::vector<std::string> names;
};
bool plateWriteSink(const std::string& name, const std::vector<std::uint8_t>& tex,
                    const CellGrid& cell, int frames, void* ctx);

} // namespace gen
