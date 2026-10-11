// generate.h - keeps the generated data files in the mod folder current with the game.
//
// ensureOutputs() fingerprints every input archive (size + mtime of the four .arz and the
// Items / UI / Text_<lang> .arc of the base game and the three expansions), compares it with
// <mod>\catalogue.stamp and, when the stamp differs or an output is missing, regenerates
// catalogue.bin, uniq-records.txt, uniq-pages.txt and uniq-groups.txt: everything is written
// to temporary names, catalogue.bin is re-read through the model loader, and only then are all
// four renamed into place and the stamp written. uniq-pages.arz is the fifth output: the DLL
// writes the overlay itself from the same derived geometry, so the frame's location and box
// count follow the database, and it is read back before anything is renamed. On any failure the
// folder keeps the files it had - none on a fresh install - and `error` names the
// input or rule that failed.
//
// The cover plates are the sixth: painted from the same geometry into the engine's texture
// override root, written under temporary names and renamed with the rest, the stamp last. A
// plate whose bytes are already on disk is not rewritten and a folder that cannot be written
// is a warning - the plates already installed are then kept.
//
// ensureOutputsGuarded() is the same call under SEH and a C++ catch: a corrupt archive turns
// into an error string, never into a crash of the process that loaded the DLL.
//
// A VIEW is the named input set the outputs were derived from: vanilla, basemods, or a custom
// game's mod folder on top of either. The start-up view's outputs keep the mod folder itself; a
// custom view's go to the folder the DLL names (<mod folder>\mods\<name>\, its basemods\ for the
// /basemods variant), so entering a custom game and leaving it again never rewrites the files the
// start-up view uses. The view is part of the stamp, together with the custom layer's own
// archives.
//
// The catalogue is built from the view too (catalogue_gen.h): its records, names and icons. The
// groups come from uniquetab-groups.ini (written from the built-in template when it is absent,
// never rewritten; a file that does not parse is one warning and the built-in groups); its bytes
// are part of the stamp, so an edit regenerates once. A record a journal of the view owns that
// no group of the view takes (an MI with collect_mi=0, a record a group excludes, one the
// database lacks) stays in the journal and on no page: the report names them (one warning).
#pragma once

#include "gen/ui_geometry.h"

#include <string>
#include <vector>

namespace gen {

struct GenReport {
    bool generated = false;        // the four files were (re)written
    bool upToDate = false;         // the stamp matched and every output exists
    std::string reason;            // why a generation ran: no stamp, a missing output, a changed input
    std::string error;             // set when the generation failed
    std::string summary;           // counts and milliseconds, for the log
    std::string overlay;           // the overlay that was written: records, size, frame
    std::string geometry;          // the grid rectangle, and which record the origin came from
    std::string plates;            // the cover plates that were painted, and where they went
    std::string plateGround;       // the flat modded ground: the colour and where it came from
    std::string view;              // the view these outputs are for
    std::string memory;            // the process working set around the generation
    double ownPeakMb = 0.0;        // the heap the generation itself held at its peak
    std::string groups;            // which rule set grouped the records, and the not-shown count
    std::size_t notShown = 0;      // records a journal of the view owns that no page shows
    std::vector<std::string> warnings;
};

// The inputs besides the archives; every member may stay empty. The user's grid override is
// NOT one of them: it moves only the plates, which ensureLayoutPlates paints under their own
// hashed names, so a layout change never regenerates the catalogue or the overlay.
struct GenInputs {
    std::string groupsPath;               // uniquetab-groups.ini; empty = the built-in groups
    std::vector<std::string> journals;    // the view's journals (softcore, -hc), read only, for
                                          // the report alone (the owned records no page shows)
    // uniquetab.ini's collect_mi: the monster infrequents are admitted (catalogue_gen.h) and every
    // view answers to [view:collect_mi] in the groups (ut_groups_default.h's MI blocks). A stamp
    // input ("collect_mi=1" only while it is on, so an OFF stamp is the one earlier builds wrote):
    // a flip regenerates every view once. Default ON, as uniquetab.ini's default (collect_mi=1); the
    // DLL always sets it from the ini (ut_generate.cpp generationInputs).
    bool collectMi = true;
};

// Called once, after the decision to (re)generate and before any archive is read, so the caller
// can say so before the seconds the generation takes. Never called when the stamp matches.
typedef void (*GenStartFn)(const GenReport& report, void* ctx);

// gameDir: the game folder (the parent of x64). modDir: where the outputs and the stamp go,
// no trailing slash. pagesArz: where the overlay is written - the path the tab loads it from.
// plateDir: the engine's texture override root <game>\settings\ui\caravan, the only place a
// relative "ui/caravan/..." name resolves from; the cover plates are painted into it, and only
// files named uniq_plate_*.tex are ever written there. Empty: no plate is painted (the harness
// points it at a scratch folder, and nothing but the DLL's start-up at a game folder).
// plateGround: uniquetab.ini's plate_ground, which picks the flat colour a MODDED plate is
// filled with. It is part of the stamp, so changing it regenerates the set once.
// view: which database this generation is for. The default is the start-up view read from the
// command line, which is what every caller had before views existed.
bool ensureOutputs(const std::string& gameDir, const std::string& modDir,
                   const std::string& pagesArz, const std::string& plateDir,
                   const std::string& lang, const std::string& plateGround, GenReport& report,
                   GenStartFn onStart = nullptr, void* ctx = nullptr,
                   const ViewSpec* view = nullptr, const GenInputs* inputs = nullptr);
bool ensureOutputsGuarded(const std::string& gameDir, const std::string& modDir,
                          const std::string& pagesArz, const std::string& plateDir,
                          const std::string& lang, const std::string& plateGround,
                          GenReport& report, GenStartFn onStart = nullptr, void* ctx = nullptr,
                          const ViewSpec* view = nullptr, const GenInputs* inputs = nullptr);

// The fingerprint text of the inputs under gameDir for a view (what catalogue.stamp holds).
// extra: further stamp lines (the groups file's and the layout override's hashes).
std::string inputFingerprint(const std::string& gameDir, const std::string& lang,
                             const std::string& plateGround, const ViewSpec* view = nullptr,
                             const std::string& extra = std::string());

// FNV-1a 64 of a text, 16 hex digits: how a stamp names a file it does not fingerprint by size
// and time.
std::string stampHash(const std::string& bytes);

// /basemods on the process's own command line: the start-up view's only variable.
bool basemodsOnCommandLine();

// ---- the layout plates ------------------------------------------------------------------------
// ONE GENERATION AT A TIME. ensureOutputs and ensureLayoutPlates hold this lock for their whole
// run (a start-up generation, a custom view's switch and a layout repaint never overlap: they
// share the plate folder and the view's outputs). Not re-entrant.
void generationLockAcquire();
void generationLockRelease();

// The user's grid in force for one view's cover, record px, window-local (ut::utLayoutGridRect).
struct LayoutGrid {
    int x = 0, y = 0, w = 0, h = 0;
    std::string text;   // the canonical override, "grid_438x627=105,120,333,460" (the hash input)
};

// What a layout plate set is, before anything is painted: read from the view's own outputs
// (<outDir>\catalogue.stamp and uniq-groups.txt), so it costs two small file reads.
struct LayoutPlan {
    UiGeometry geo;                 // the P line; x0/y0 moved to the user grid's origin
    int rowGap = 2;                 // the F line's
    std::vector<CellGrid> cells;    // every distinct cell size, fitted to the user grid (cap: the F line's frame count)
    std::string hash;               // eight hex digits: the low half of FNV-1a 64 over the inputs
    std::vector<std::string> names; // the leaf names, one per cell, ascending like `cells`
};
// hash = FNV-1a 64 of: the main stamp text, the grid override text, every cell as
// "<cw>x<ch>:<cols>x<rows>", "plate_ground=<value>" and "lattice=<ut::kUtLatticeRule>", newline
// separated; the low 32 bits. A game update (the stamp), a new override, a new cell size, another
// ground or a new lattice rule of the painter: a new hash.
std::string layoutPlateHash(const std::string& stampText, const std::string& gridText,
                            const std::vector<CellGrid>& cells, const std::string& plateGround);
bool planLayoutPlates(const std::string& outDir, const LayoutGrid& grid,
                      const std::string& plateGround, const std::string& plateTag,
                      LayoutPlan& plan, std::string* error);

// The P line of a uniq-groups.txt as a UiGeometry (bmpX/Y out of the usable rectangle), the
// largest cell of its G lines, its F line's row gap and its F line's frame box count (the cap a
// grid is fitted under; ut::kUtFrameBoxFloor when there is no F line). False when there is no
// P line.
bool readGroupsGeometry(const std::string& groupsPath, UiGeometry& geo, int* maxCellW,
                        int* maxCellH, int* rowGap, std::vector<CellGrid>* cells,
                        int* frameBoxes = nullptr);

struct LayoutPlatesReport {
    bool upToDate = false;          // every plate of the set was already on disk
    bool painted = false;           // the set was (re)painted
    std::string hash;               // the set in force, empty = none (the derived plates)
    std::vector<std::string> names; // its leaf names
    std::size_t written = 0, same = 0, failed = 0;
    std::size_t removed = 0, removeFailed = 0;   // superseded _L plates of this cover and tag
    long long ms = 0;               // the plates-only pass, from the lock to the last rename
    double ownPeakMb = 0.0;         // the heap it held at its peak (HeapMeter)
    std::string ground;             // the ground rule's line
    std::string error;
    std::vector<std::string> warnings;
};

// The plates-only generation: no catalogue, no overlay, no stamp. grid == null: no user grid is
// in force for this view - nothing is painted and every _L plate of its cover and tag goes.
// Otherwise the set for `grid` is painted into plateDir (temporary names, renamed together) when
// any of its files is missing, and the _L plates of the same cover and tag with another hash are
// deleted (a failed delete waits for the next call). Only files named uniq_plate_*_L<8 hex>.tex
// are ever deleted. outDir: the view's output folder (where catalogue.stamp and uniq-groups.txt
// are); the main generation must have run there.
bool ensureLayoutPlates(const std::string& gameDir, const std::string& outDir,
                        const std::string& plateDir, const std::string& lang,
                        const std::string& plateGround, const ViewSpec& view,
                        const LayoutGrid* grid, LayoutPlatesReport& report);
bool ensureLayoutPlatesGuarded(const std::string& gameDir, const std::string& outDir,
                               const std::string& plateDir, const std::string& lang,
                               const std::string& plateGround, const ViewSpec& view,
                               const LayoutGrid* grid, LayoutPlatesReport& report);

} // namespace gen
