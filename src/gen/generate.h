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
// custom view's go to <mod folder>\views\<name>, so entering a custom game and leaving it again
// never rewrites the files the start-up view uses. The view is part of the stamp, together with
// the custom layer's own archives.
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
    std::vector<std::string> warnings;
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
                   const ViewSpec* view = nullptr);
bool ensureOutputsGuarded(const std::string& gameDir, const std::string& modDir,
                          const std::string& pagesArz, const std::string& plateDir,
                          const std::string& lang, const std::string& plateGround,
                          GenReport& report, GenStartFn onStart = nullptr, void* ctx = nullptr,
                          const ViewSpec* view = nullptr);

// The fingerprint text of the inputs under gameDir for a view (what catalogue.stamp holds).
std::string inputFingerprint(const std::string& gameDir, const std::string& lang,
                             const std::string& plateGround, const ViewSpec* view = nullptr);

// /basemods on the process's own command line: the start-up view's only variable.
bool basemodsOnCommandLine();

} // namespace gen
