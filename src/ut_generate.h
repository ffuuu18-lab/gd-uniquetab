// ut_generate.h - the DLL side of src\gen: keeps catalogue.bin and the three uniq txt files in
// the mod folder current with the game's own archives before anything reads them.
//
// generateEnsure() is called once from the worker thread, before panelInit / reagentInit and
// before any hook is installed. It resolves the game folder and the mod folder through
// ut_paths.h, takes the text language from the ini, and runs gen::ensureOutputsGuarded. A stamp
// that matches costs one stat per input archive and one debug line; a first launch or a game
// update rebuilds the files (a few seconds, one info line each way); a failure is one ERROR
// line and the folder keeps the files it already had.
//
// THE VIEW. Which database the outputs describe is decided twice: once at start-up, from
// /basemods on the command line, and again every time the engine loads a database - a custom
// game is chosen from the main menu, long after start-up, and moves the caravan with its own
// records. The start-up view's outputs keep the mod folder itself, so an installed copy sees no
// change until it enters a custom game; a custom view's go to <mod folder>\mods\<leaf> (or
// mods\<leaf>\basemods for the /basemods variant - ut_folders.h), so
// entering one and leaving it again never rewrites what the start-up view reads. Everything that
// reads a generated file resolves it through utViewFile().
#pragma once

#include <windows.h>

#include <stddef.h>

namespace ut {

// The start-up view, into the mod folder. Worker thread, once.
void generateEnsure(HMODULE selfModule);

// The active view's name ("vanilla", "basemods", "custom-<name>", "basemods+custom-<name>") and
// the tag a non-vanilla plate file name carries (empty for the start-up view).
const char* viewName();
const char* viewTag();
// True while a custom game's view is the active one.
bool viewIsCustom();

// "<the active view's folder>\<leaf>", false when the file is not there. The start-up view
// answers exactly what utModFile() answers.
bool utViewFile(HMODULE selfModule, const char* leaf, char* out, size_t cap);

// The mod folder of a custom game's archive, from the path Engine::LoadDatabase was handed - the
// engine names it relative to its working directory with forward slashes
// ("mods/<name>/database/<name>.arz"). Resolved against the game folder, then against the current
// directory, and accepted only where that archive exists. False (and `out` empty) for every
// archive that is not a custom game's.
bool customGameFolderOf(const char* enginePath, char* out, size_t cap);

// A custom game's database is loading out of <game>\mods\<name>: generate that view's outputs
// under <mod folder>\mods\<leaf>\ and make it the active one. GAME THREAD, from inside the database
// detour and before the overlay is loaded, because the overlay a custom view needs is that
// view's own uniq-pages.arz. True when the active view CHANGED.
bool viewEnterCustom(HMODULE selfModule, const char* modFolder);

// True while a view is being generated on the game thread. The frame counter cannot advance
// during those few seconds, and the stall check must not read that as a crashed game.
bool generateBusy();

// ---- the layout plates -------------------------------------------------------------------------
// A user grid_<W>x<H> moves the boxes at once (ut_live); the frames under them come from a plate
// set of its own, uniq_plate_<cell>_<cover>[_<tag>]_L<hash8>.tex, painted off the game thread
// (gen::ensureLayoutPlates) - at start-up on the worker when the set is missing, and 1 s after a
// settings or view change on the layout plates' own thread - while the derived plates and the
// overlay stay as they are. Started by generateEnsure; never touches the engine.
void layoutPlatesStart(HMODULE selfModule);
// The set in force for cover plateW x plateH of the ACTIVE view: `suffix` gets "_L<hash8>" while a
// user grid is in force there (empty = the derived plates) and `ready` whether its files are on
// disk yet. Any thread. Returns layoutPlatesGen().
long layoutPlatesState(int plateW, int plateH, char* suffix, size_t cap, bool* ready);
// Bumps on every change of that state: ut_plate renames and re-probes its plates on an edge.
long layoutPlatesGen();

// Back to the view the process started in: Engine::LoadMainDatabase rebuilds the base database,
// so whatever custom layer was above it is gone. Its outputs are already there - nothing is
// generated. True when the active view changed.
bool viewEnterStartup();

}  // namespace ut
