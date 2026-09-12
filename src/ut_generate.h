// ut_generate.h - the DLL side of src\gen: keeps catalogue.bin and the three uniq txt files in
// the mod folder current with the game's own archives before anything reads them.
//
// generateEnsure() is called once from the worker thread, before panelInit / reagentInit and
// before any hook is installed. It resolves the game folder and the mod folder through
// ut_paths.h, takes the text language from the ini, and runs gen::ensureOutputsGuarded. A stamp
// that matches costs one stat per input archive and one debug line; a first launch or a game
// update rebuilds the files (a few seconds, one info line each way); a failure is one ERROR
// line and the folder keeps the files it already had.
#pragma once

#include <windows.h>

namespace ut {

void generateEnsure(HMODULE selfModule);

}  // namespace ut
