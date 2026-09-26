// ut_generate.cpp - see ut_generate.h.
#include "ut_generate.h"

#include "gen/generate.h"
#include "ut_config.h"
#include "ut_log.h"
#include "ut_paths.h"

#include <cctype>
#include <cstring>
#include <string>

namespace ut {

namespace {

// The ini value names a file suffix: letters and digits only, up to seven of them, else EN.
std::string textLanguage() {
    std::string lang;
    for (const char* p = g_cfg.textLanguage; *p && lang.size() < 7; ++p) {
        if (!std::isalnum((unsigned char)*p)) { lang.clear(); break; }
        lang += (char)std::toupper((unsigned char)*p);
    }
    if (lang.empty()) {
        if (g_cfg.textLanguage[0]) {
            logW("config: text_language=%s is not a Text_<lang>.arc suffix - using EN",
                 g_cfg.textLanguage);
        }
        lang = "EN";
    }
    return lang;
}

// THE ACTIVE VIEW. Written on the game thread from the database detour and read from the game
// thread (the file paths) and the worker thread (the log). Fixed storage, never a pointer that
// can be freed under a reader: the worst a race can do is print a half-updated name, and there
// is no moment at which a reader holds memory that is going away.
struct ViewState {
    char name[64];       // vanilla | basemods | custom-<name> | basemods+custom-<name>
    char tag[64];        // what a non-vanilla plate name carries; empty on the start-up view
    char dir[MAX_PATH];  // the view's output folder; empty = the mod folder itself
    char startup[64];    // the name the process started with
    char settled[MAX_PATH]; // the last custom folder already decided about
    ViewState() {
        std::strcpy(name, "vanilla");
        std::strcpy(startup, "vanilla");
        tag[0] = 0;
        dir[0] = 0;
        settled[0] = 0;
    }
};

volatile LONG g_generating = 0;   // a generation is running on the game thread

ViewState& view() {
    static ViewState s;   // one initialisation, on the first call, before any hook exists
    return s;
}

void setText(char* dst, size_t cap, const char* src) {
    _snprintf_s(dst, cap, _TRUNCATE, "%s", src ? src : "");
}

struct StartCtx {
    const char* gameDir;
    const char* modDir;
    const char* pagesArz;
    const std::string* lang;
};

// Printed before the archives are read, so the log says why the start-up pauses for a few seconds.
void sayStart(const gen::GenReport& report, void* ctx) {
    const StartCtx* c = (const StartCtx*)ctx;
    logI("catalogue: building from the game's database (%s, names from Text_%s.arc) ...",
         report.reason.empty() ? "no reason recorded" : report.reason.c_str(), c->lang->c_str());
    logD("catalogue: game folder \"%s\", output folder \"%s\", page archive \"%s\"", c->gameDir,
         c->modDir, c->pagesArz);
    logFlush();
}

}  // namespace

void generateEnsure(HMODULE selfModule) {
    char gameDir[MAX_PATH] = {0};
    char modDir[MAX_PATH] = {0};
    char pagesArz[MAX_PATH] = {0};
    if (!utGameDirA(gameDir, sizeof(gameDir))) {
        logE("catalogue: the game folder could not be resolved from the host exe's own path - "
             "the data files in the mod folder are used as they are");
        return;
    }
    utModDirA(selfModule, modDir, sizeof(modDir));
    utModFile(selfModule, "uniq-pages.arz", pagesArz, sizeof(pagesArz));
    // The engine's texture override root: the only place a relative "ui/caravan/..." name
    // resolves from, so it is where the painted plates have to land. Only files named
    // uniq_plate_*.tex are ever written there.
    const std::string plateDir = std::string(gameDir) + "\\settings\\ui\\caravan";
    const std::string lang = textLanguage();
    // plate_ground: the flat colour a MODDED cover plate is filled with. It is part of the
    // stamp, so a change to it regenerates the set once; the vanilla plates never use it.
    const std::string plateGround(g_cfg.plateGround);

    gen::ViewSpec spec;
    spec.basemods = gen::basemodsOnCommandLine();
    setText(view().name, sizeof(view().name), spec.name().c_str());
    setText(view().startup, sizeof(view().startup), spec.name().c_str());
    view().tag[0] = 0;
    view().dir[0] = 0;

    gen::GenReport report;
    StartCtx ctx = {gameDir, modDir, pagesArz, &lang};
    const bool ok = gen::ensureOutputsGuarded(gameDir, modDir, pagesArz, plateDir, lang,
                                              plateGround, report, &sayStart, &ctx, &spec);
    if (ok && report.upToDate) {
        logD("catalogue: catalogue.stamp matches the game files (Text_%s) - catalogue.bin and "
             "the uniq-*.txt files in \"%s\" are current",
             lang.c_str(), modDir);
        return;
    }
    for (const std::string& w : report.warnings) logW("catalogue: %s", w.c_str());
    if (ok) {
        logI("catalogue: built - %s", report.summary.c_str());
        if (!report.overlay.empty()) logI("catalogue: %s", report.overlay.c_str());
        if (!report.geometry.empty()) logI("catalogue: %s", report.geometry.c_str());
        if (!report.plates.empty()) logI("catalogue: %s", report.plates.c_str());
        if (!report.plateGround.empty()) logI("catalogue: %s", report.plateGround.c_str());
        if (!report.memory.empty()) logD("catalogue: %s", report.memory.c_str());
    } else {
        logE("catalogue: generation FAILED - %s. The files already in \"%s\" are kept",
             report.error.c_str(), modDir);
    }
}

bool generateBusy() { return InterlockedCompareExchange(&g_generating, 0, 0) != 0; }

const char* viewName() { return view().name; }
const char* viewTag() { return view().tag; }
bool viewIsCustom() { return view().dir[0] != 0; }

bool utViewFile(HMODULE selfModule, const char* leaf, char* out, size_t cap) {
    if (!view().dir[0]) return utModFile(selfModule, leaf, out, cap);
    _snprintf_s(out, cap, _TRUNCATE, "%s\\%s", view().dir, leaf);
    const DWORD a = GetFileAttributesA(out);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool customGameFolderOf(const char* enginePath, char* out, size_t cap) {
    if (!cap) return false;
    out[0] = 0;
    if (!enginePath || !enginePath[0]) return false;
    if (gen::customGameFolder(enginePath, std::string()).empty()) return false;
    char base[2][MAX_PATH] = {{0}, {0}};
    utGameDirA(base[0], sizeof(base[0]));
    GetCurrentDirectoryA(MAX_PATH, base[1]);
    const std::string folder = gen::findCustomGameFolder(enginePath, {base[0], base[1]});
    if (!folder.empty()) {
        setText(out, cap, folder.c_str());
        return true;
    }
    logW("catalogue: \"%s\" names a custom game's archive, but it is not under the game folder "
         "\"%s\" or the current directory \"%s\" - the view stays %s",
         enginePath, base[0], base[1], view().name);
    return false;
}

bool viewEnterStartup() {
    if (!view().dir[0]) return false;
    view().dir[0] = 0;
    view().tag[0] = 0;
    view().settled[0] = 0;   // a later entry into the same game tries again
    setText(view().name, sizeof(view().name), view().startup);
    logI("catalogue: the view is %s again - the main database is loading, so the custom game's "
         "records are gone with it",
         view().name);
    return true;
}

bool viewEnterCustom(HMODULE selfModule, const char* modFolder) {
    if (!modFolder || !modFolder[0]) return false;
    char gameDir[MAX_PATH] = {0};
    char modDir[MAX_PATH] = {0};
    if (!utGameDirA(gameDir, sizeof(gameDir))) return false;
    utModDirA(selfModule, modDir, sizeof(modDir));

    gen::ViewSpec spec;
    spec.basemods = gen::basemodsOnCommandLine();
    spec.customDir = modFolder;
    const std::string name = spec.name();
    if (!std::strcmp(name.c_str(), view().name)) return false;   // already the active view
    // A custom game loads several archives out of one folder (the Crucible loads four), so the
    // decision is remembered: one probe and at most one generation per entry, not one per
    // archive. viewEnterStartup clears it, so the next entry asks again.
    if (!_stricmp(modFolder, view().settled)) return false;

    // Most custom games are not stash-UI mods: the Crucible, a loot mod, a class mod all
    // leave records/ui/caravan alone, and their merged geometry is the start-up view's to
    // the pixel. Reading the mod's own archive index answers that in milliseconds, and a no
    // costs nothing further - no generation, no folder, and the start-up view goes on
    // serving the tab.
    {
        gen::ModView probe;
        std::string perr;
        if (probe.loadCustom(modFolder, &perr) && !probe.touchesCaravan()) {
            logI("catalogue: the custom game in \"%s\" does not override the caravan - the %s "
                 "view still describes it, so nothing is generated",
                 modFolder, view().name);
            setText(view().settled, sizeof(view().settled), modFolder);
            return false;
        }
    }

    // <mod folder>\views\<name>: the start-up view's files stay exactly where they are, so
    // leaving the custom game costs nothing and an installed copy that never enters one sees
    // no change at all.
    const std::string dir = std::string(modDir) + "\\views\\" + name;
    CreateDirectoryA((std::string(modDir) + "\\views").c_str(), nullptr);
    CreateDirectoryA(dir.c_str(), nullptr);
    const std::string pagesArz = dir + "\\uniq-pages.arz";
    const std::string plateDir = std::string(gameDir) + "\\settings\\ui\\caravan";
    const std::string lang = textLanguage();
    const std::string plateGround(g_cfg.plateGround);

    logI("catalogue: the view is %s - a custom game's database is loading from \"%s\"",
         name.c_str(), modFolder);
    gen::GenReport report;
    StartCtx ctx = {gameDir, dir.c_str(), pagesArz.c_str(), &lang};
    // The game thread is inside a database detour and no frame can advance until this returns;
    // the flag is what keeps the stall check from calling that a crash.
    InterlockedIncrement(&g_generating);
    const bool ok = gen::ensureOutputsGuarded(gameDir, dir, pagesArz, plateDir, lang, plateGround,
                                              report, &sayStart, &ctx, &spec);
    InterlockedDecrement(&g_generating);
    for (const std::string& w : report.warnings) logW("catalogue: %s", w.c_str());
    if (!ok) {
        logE("catalogue: the %s view could not be generated - %s. The tab keeps the %s view, "
             "which this world's caravan may not match",
             name.c_str(), report.error.c_str(), view().name);
        setText(view().settled, sizeof(view().settled), modFolder);
        return false;
    }
    if (report.upToDate) {
        logI("catalogue: the %s view is already current in \"%s\"", name.c_str(), dir.c_str());
    } else {
        logI("catalogue: built - %s", report.summary.c_str());
        if (!report.overlay.empty()) logI("catalogue: %s", report.overlay.c_str());
        if (!report.geometry.empty()) logI("catalogue: %s", report.geometry.c_str());
        if (!report.plates.empty()) logI("catalogue: %s", report.plates.c_str());
        if (!report.plateGround.empty()) logI("catalogue: %s", report.plateGround.c_str());
        if (!report.memory.empty()) logD("catalogue: %s", report.memory.c_str());
    }
    setText(view().dir, sizeof(view().dir), dir.c_str());
    setText(view().tag, sizeof(view().tag), spec.tag().c_str());
    setText(view().name, sizeof(view().name), name.c_str());
    return true;
}

}  // namespace ut
