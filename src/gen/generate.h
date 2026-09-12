// generate.h - keeps the generated data files in the mod folder current with the game.
//
// ensureOutputs() fingerprints every input archive (size + mtime of the four .arz and the
// Items / UI / Text_<lang> .arc of the base game and the three expansions), compares it with
// <mod>\catalogue.stamp and, when the stamp differs or an output is missing, regenerates
// catalogue.bin, uniq-records.txt, uniq-pages.txt and uniq-groups.txt: everything is written
// to temporary names, catalogue.bin is re-read through the model loader, and only then are all
// four renamed into place and the stamp written. On any failure the folder keeps the files it
// had and `error` names the input or rule that failed. uniq-pages.arz stays a shipped file;
// a layout that needs a box it does not have is refused.
//
// ensureOutputsGuarded() is the same call under SEH and a C++ catch: a corrupt archive turns
// into an error string, never into a crash of the process that loaded the DLL.
#pragma once

#include <string>
#include <vector>

namespace gen {

struct GenReport {
    bool generated = false;        // the four files were (re)written
    bool upToDate = false;         // the stamp matched and every output exists
    std::string reason;            // why a generation ran: no stamp, a missing output, a changed input
    std::string error;             // set when the generation failed
    std::string summary;           // counts and milliseconds, for the log
    std::vector<std::string> warnings;
};

// Called once, after the decision to (re)generate and before any archive is read, so the caller
// can say so before the seconds the generation takes. Never called when the stamp matches.
typedef void (*GenStartFn)(const GenReport& report, void* ctx);

// gameDir: the game folder (the parent of x64). modDir: where the outputs and the stamp go,
// no trailing slash. pagesArz: the shipped uniq-pages.arz the layout is checked against.
bool ensureOutputs(const std::string& gameDir, const std::string& modDir,
                   const std::string& pagesArz, const std::string& lang, GenReport& report,
                   GenStartFn onStart = nullptr, void* ctx = nullptr);
bool ensureOutputsGuarded(const std::string& gameDir, const std::string& modDir,
                          const std::string& pagesArz, const std::string& lang, GenReport& report,
                          GenStartFn onStart = nullptr, void* ctx = nullptr);

// The fingerprint text of the inputs under gameDir (what catalogue.stamp holds).
std::string inputFingerprint(const std::string& gameDir, const std::string& lang);

} // namespace gen
