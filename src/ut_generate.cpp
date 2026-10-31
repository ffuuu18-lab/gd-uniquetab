// ut_generate.cpp - see ut_generate.h.
#include "ut_generate.h"

#include "gen/generate.h"
#include "ut_config.h"
#include "ut_folders.h"
#include "ut_layout.h"
#include "ut_log.h"
#include "ut_panel.h"
#include "ut_paths.h"
#include "ut_viewprobe.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

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
    char custom[MAX_PATH]; // the custom game's folder; empty on the start-up view
    char startup[64];    // the name the process started with
    char settled[MAX_PATH]; // the last custom folder already decided about
    ViewState() {
        std::strcpy(name, "vanilla");
        std::strcpy(startup, "vanilla");
        tag[0] = 0;
        dir[0] = 0;
        custom[0] = 0;
        settled[0] = 0;
    }
};
// Taken around a view switch's writes (game thread) and by the layout plates' snapshot (their
// own thread); the game thread's other readers keep reading the fixed storage directly.
SRWLOCK g_viewLock = SRWLOCK_INIT;

volatile LONG g_generating = 0;   // a generation is running on the game thread

ViewState& view() {
    static ViewState s;   // one initialisation, on the first call, before any hook exists
    return s;
}

void setText(char* dst, size_t cap, const char* src) {
    _snprintf_s(dst, cap, _TRUNCATE, "%s", src ? src : "");
}

// The generation's inputs besides the archives: uniquetab-groups.ini beside uniquetab.ini, and
// the view's journal pair - read only, to keep what they own. The journal folder follows
// ut_rescue's rule: journal_dir when it names an existing folder, else the mod folder; a custom
// game's set is under mods\<leaf>\ there (custom-x and basemods+custom-x share it, ut_folders.h).
// The grid override is not an input of the main generation: it moves only the plates, which the
// layout plates (below) paint under their own hashed names.
gen::GenInputs generationInputs(HMODULE selfModule, const gen::ViewSpec& spec) {
    gen::GenInputs in;
    char path[MAX_PATH] = {0};
    utModPathA(selfModule, "uniquetab-groups.ini", path, sizeof(path));
    in.groupsPath = path;
    char dir[MAX_PATH] = {0};
    utModDirA(selfModule, dir, sizeof(dir));
    std::string jd(g_cfg.journalDir);
    while (!jd.empty() && (jd.back() == '\\' || jd.back() == '/' || jd.back() == ' ' || jd.back() == '"'))
        jd.pop_back();
    if (!jd.empty() && jd[0] == '"') jd.erase(0, 1);
    const DWORD a = jd.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesA(jd.c_str());
    std::string base = (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) ? jd : std::string(dir);
    if (!spec.startup()) base += "\\" + utCustomGameRel(spec.name().c_str());
    in.journals.push_back(base + "\\uniq-items.jsonl");
    in.journals.push_back(base + "\\uniq-items-hc.jsonl");
    in.collectMi = g_cfg.collectMi != 0;
    return in;
}

// The per-view log (ut_log.h): <dir>\uniquetab.log for the view `name`, or no tee when `dir` is
// empty (the start-up view writes to the main log only).
void teeFollow(const char* dir, const char* name) {
    if (!dir || !dir[0]) {
        logTeeClose();
        return;
    }
    char pathA[MAX_PATH];
    _snprintf_s(pathA, sizeof(pathA), _TRUNCATE, "%s\\uniquetab.log", dir);
    wchar_t path[MAX_PATH];
    if (!MultiByteToWideChar(CP_ACP, 0, pathA, -1, path, MAX_PATH)) {
        logTeeClose();
        return;
    }
    if (logTeeOpen(path, name)) {
        logD("log: the %s view's lines also go to \"%s\"", name, pathA);
    } else {
        logW("log: \"%s\" could not be opened - the %s view's lines are in this log only", pathA,
             name);
    }
}

void logGroups(const gen::GenReport& report) {
    if (!report.groups.empty()) logI("catalogue: %s", report.groups.c_str());
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

// ---- the layout plates ------------------------------------------------------------------------
// A user grid (grid_<W>x<H>) moves the boxes at once (ut_live); its frames come from a plate set of
// its own, painted here off the game thread under hashed names (gen::ensureLayoutPlates) and
// switched to by ut_plate when ready. Published state, read from any thread:
struct LayPublished {
    char view[64];
    int plateW, plateH;
    char hash[12];   // empty = no user grid in force: the derived plates
    bool ready;      // the set's files are on disk
};
SRWLOCK g_layLock = SRWLOCK_INIT;
LayPublished g_lay = {};
volatile LONG g_layGen = 0;
HMODULE g_layModule = nullptr;

void layPublish(const char* viewName_, int pw, int ph, const std::string& hash, bool ready) {
    AcquireSRWLockExclusive(&g_layLock);
    const bool same = !std::strcmp(g_lay.view, viewName_) && g_lay.plateW == pw &&
                      g_lay.plateH == ph && hash == g_lay.hash && g_lay.ready == ready;
    if (!same) {
        setText(g_lay.view, sizeof(g_lay.view), viewName_);
        g_lay.plateW = pw;
        g_lay.plateH = ph;
        setText(g_lay.hash, sizeof(g_lay.hash), hash.c_str());
        g_lay.ready = ready;
        InterlockedIncrement(&g_layGen);
    }
    ReleaseSRWLockExclusive(&g_layLock);
}

// The config values the grid in force depends on, copied twice until both copies agree (g_cfg is
// replaced by a struct copy on the worker thread). The two screen keys belong here as much as the
// [layout] families: a pad or label pinned to the screen takes no part in the in-window rules, so
// without them a grid the live page accepts (ut_live.cpp layoutCompute) would be refused here for
// meeting the pad's in-window rectangle - and the user grid would never get its plates.
struct LayCfg {
    UtCfgFamilyStore grid, pad, label;
    int padY, padH, padGap, labelSize, captionMax, plateSwap;
    int zoomPct;   // zoom_pct: a zoom above 100 is a layout of its own (the magnified cells)
    char ground[8];
    char padScreen[sizeof(g_cfg.padScreen)], labelScreen[sizeof(g_cfg.labelScreen)];
};
bool layCfgCopy(LayCfg* out) {
    for (int tries = 0; tries < 4; ++tries) {
        LayCfg a, b;
        memset(&a, 0, sizeof(a));
        memset(&b, 0, sizeof(b));
        for (LayCfg* c : {&a, &b}) {
            memcpy(&c->grid, &g_cfg.layGrid, sizeof(c->grid));
            memcpy(&c->pad, &g_cfg.layPad, sizeof(c->pad));
            memcpy(&c->label, &g_cfg.layLabel, sizeof(c->label));
            c->padY = g_cfg.padY;
            c->padH = g_cfg.padH;
            c->padGap = g_cfg.padGap;
            c->labelSize = panelLabelSizeInForce();
            c->captionMax = panelCaptionMaxInForce();
            c->plateSwap = g_cfg.plateSwap;
            c->zoomPct = g_cfg.zoomPct;
            memcpy(c->ground, g_cfg.plateGround, sizeof(c->ground));
            memcpy(c->padScreen, g_cfg.padScreen, sizeof(c->padScreen));
            memcpy(c->labelScreen, g_cfg.labelScreen, sizeof(c->labelScreen));
        }
        if (!memcmp(&a, &b, sizeof(a))) {
            *out = a;
            out->ground[sizeof(out->ground) - 1] = 0;
            out->padScreen[sizeof(out->padScreen) - 1] = 0;
            out->labelScreen[sizeof(out->labelScreen) - 1] = 0;
            return true;
        }
    }
    return false;
}

// What the active view wants: its output folder, its cover and the user grid in force there.
struct LayWant {
    std::string view, outDir;
    gen::ViewSpec spec;
    int plateW = 0, plateH = 0;
    bool user = false;
    gen::LayoutGrid grid;
    std::string ground;
    bool swap = true;
};
bool layWantNow(LayWant& w, std::string* why) {
    char name[64], dir[MAX_PATH], custom[MAX_PATH];
    AcquireSRWLockShared(&g_viewLock);
    setText(name, sizeof(name), view().name);
    setText(dir, sizeof(dir), view().dir);
    setText(custom, sizeof(custom), view().custom);
    ReleaseSRWLockShared(&g_viewLock);
    w.view = name;
    w.spec.basemods = gen::basemodsOnCommandLine();
    w.spec.customDir = custom;
    if (dir[0]) {
        w.outDir = dir;
    } else {
        char modDir[MAX_PATH] = {0};
        utModDirA(g_layModule, modDir, sizeof(modDir));
        w.outDir = modDir;
    }
    gen::UiGeometry geo;
    int mw = 0, mh = 0, gap = 2;
    if (!gen::readGroupsGeometry(w.outDir + "\\uniq-groups.txt", geo, &mw, &mh, &gap, nullptr)) {
        if (why) *why = "the " + w.view + " view's uniq-groups.txt has no P line";
        return false;
    }
    w.plateW = geo.plateW;
    w.plateH = geo.plateH;
    LayCfg c;
    if (!layCfgCopy(&c)) {
        if (why) *why = "the settings changed while they were read";
        return false;
    }
    w.ground = c.ground;
    w.swap = c.plateSwap != 0;
    char cover[32];
    _snprintf_s(cover, sizeof(cover), _TRUNCATE, "%dx%d", geo.plateW, geo.plateH);
    UtLayoutSource src = {};
    src.x0 = geo.x0;
    src.y0 = geo.y0;
    src.usableW = geo.usableW;
    src.usableH = geo.usableH;
    src.plateW = geo.plateW;
    src.plateH = geo.plateH;
    src.bmpX = geo.bmpX;
    src.bmpY = geo.bmpY;
    src.bmpKnown = 1;
    src.maxCellW = mw;
    src.maxCellH = mh;
    src.rowGap = gap;
    src.grid = configFamilyGet(c.grid, cover);
    src.pad = configFamilyGet(c.pad, cover);
    src.label = configFamilyGet(c.label, cover);
    src.padY = c.padY;
    src.padH = c.padH;
    src.padGap = c.padGap;
    src.labelSize = c.labelSize;
    src.captionMax = c.captionMax;
    // The screen keys as the live page passes them, with no screen known (utLayoutPlatesGrid): this
    // thread never sees one, and a screen key that cannot be placed yet PINS its part out of the
    // in-window rules (utLayoutResolve) - the pad the live page draws on the screen is not judged
    // against the grid in the window here either. A screen key the live page later refuses for a
    // reason of its own (over the grid, as large as the screen, no fit) can still leave the two
    // sides apart for that grid; ut_plate.cpp then shows the derived plates under the live page's
    // derived grid, never this set (utPlateSetChoice).
    src.padScreen = c.padScreen;
    src.labelScreen = c.labelScreen;
    // zoom_pct above 100: the grid in force (the derived one included) gets a set of its own for
    // the magnified cells, "grid_<cover>=x,y,w,h zoom=<pct>" (utLayoutPlatesGrid); `user` then
    // means "a layout set is in force", as on the live page (utLiveGridKind).
    src.zoom = c.zoomPct;
    UtRect r;
    char canon[80];
    w.user = utLayoutPlatesGrid(src, &r, canon, sizeof(canon)) != 0;
    if (w.user) {
        w.grid.x = r.x;
        w.grid.y = r.y;
        w.grid.w = r.w;
        w.grid.h = r.h;
        w.grid.text = std::string("grid_") + cover + "=" + canon;
        w.grid.zoom = utZoomPct(c.zoomPct);
    }
    return true;
}

volatile LONG g_layRunning = 0;

// One pass: the active view's layout plates made current (painted, or the superseded ones
// removed). Worker at start-up, then the layout plates' own thread. paint = false only decides and
// publishes which set is in force (and whether it is on disk) - a few file reads, so the page drops
// frames that no longer fit at once and the paint itself can wait for the settings to settle.
void layoutPlatesRun(const char* when, bool paint) {
    if (InterlockedExchange(&g_layRunning, 1)) return;
    LayWant w;
    std::string why;
    if (!layWantNow(w, &why)) {
        logD("layout plates: nothing to do %s - %s", when, why.c_str());
        InterlockedExchange(&g_layRunning, 0);
        return;
    }
    char gameDir[MAX_PATH] = {0};
    if (!utGameDirA(gameDir, sizeof(gameDir)) || !w.swap) {
        InterlockedExchange(&g_layRunning, 0);
        return;
    }
    const std::string plateDir = std::string(gameDir) + "\\settings\\ui\\caravan";
    const std::string lang = textLanguage();
    const std::string tag = w.spec.tag();
    if (!paint && !w.user) {
        layPublish(w.view.c_str(), w.plateW, w.plateH, std::string(), true);
        InterlockedExchange(&g_layRunning, 0);
        return;
    }
    if (w.user) {
        gen::LayoutPlan plan;
        std::string err;
        if (!gen::planLayoutPlates(w.outDir, w.grid, w.ground, tag, plan, &err)) {
            logW("layout plates: %s cannot be planned (%s) - the page shows the plain cover under "
                 "the moved grid", w.grid.text.c_str(), err.c_str());
            layPublish(w.view.c_str(), w.plateW, w.plateH, "unplanned", false);
            InterlockedExchange(&g_layRunning, 0);
            return;
        }
        bool all = true;
        for (const std::string& n : plan.names) {
            const DWORD a = GetFileAttributesA((plateDir + "\\" + n + ".tex").c_str());
            if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) { all = false; break; }
        }
        if (!paint) {
            layPublish(w.view.c_str(), w.plateW, w.plateH, plan.hash, all);
            InterlockedExchange(&g_layRunning, 0);
            return;
        }
        if (!all) {
            layPublish(w.view.c_str(), w.plateW, w.plateH, plan.hash, false);
            logI("layout plates: the grid in force is %s (%d,%d %dx%d) - painting %zu plate(s) "
                 "_L%s for the %s view on the worker; the page shows the plain cover meanwhile",
                 w.grid.text.c_str(), w.grid.x, w.grid.y, w.grid.w, w.grid.h, plan.names.size(),
                 plan.hash.c_str(), w.view.c_str());
            logFlush();
        }
    }
    gen::LayoutPlatesReport rep;
    const bool ok = gen::ensureLayoutPlatesGuarded(gameDir, w.outDir, plateDir, lang, w.ground,
                                                   w.spec, w.user ? &w.grid : nullptr, rep);
    for (const std::string& s : rep.warnings) logW("layout plates: %s", s.c_str());
    if (!ok) {
        logE("layout plates: painting %s FAILED - %s. The page shows the plain cover under the "
             "moved grid", w.user ? w.grid.text.c_str() : "nothing", rep.error.c_str());
        InterlockedExchange(&g_layRunning, 0);
        return;
    }
    layPublish(w.view.c_str(), w.plateW, w.plateH, w.user ? rep.hash : std::string(), true);
    if (rep.painted) {
        logI("layout plates: %zu plate(s) _L%s painted in %lld ms (%zu written, %zu already "
             "current, %.1f MB of heap at the peak) into \"%s\" - the page switches to them",
             rep.names.size(), rep.hash.c_str(), rep.ms, rep.written, rep.same, rep.ownPeakMb,
             plateDir.c_str());
        if (!rep.ground.empty()) logD("layout plates: %s", rep.ground.c_str());
    } else if (w.user) {
        logD("layout plates: the set _L%s for %s is already on disk (%s)", rep.hash.c_str(),
             w.grid.text.c_str(), when);
    }
    if (rep.removed || rep.removeFailed)
        logI("layout plates: %zu superseded plate(s) of the %dx%d cover removed%s", rep.removed,
             w.plateW, w.plateH, rep.removeFailed ? " (some are in use - the next pass retries)" : "");
    InterlockedExchange(&g_layRunning, 0);
}

// The layout plates' own thread: a change of the settings or of the view publishes the set in
// force at once; after 1 s of quiet one pass paints it. It never touches the engine.
DWORD WINAPI layoutPlatesThread(LPVOID) {
    long seenCfg = configGeneration();
    char seenView[64];
    setText(seenView, sizeof(seenView), view().name);
    bool pending = false;
    DWORD changedAt = 0;
    for (;;) {
        Sleep(250);
        const long cg = configGeneration();
        char name[64];
        AcquireSRWLockShared(&g_viewLock);
        setText(name, sizeof(name), view().name);
        ReleaseSRWLockShared(&g_viewLock);
        if (cg != seenCfg || std::strcmp(name, seenView)) {
            seenCfg = cg;
            setText(seenView, sizeof(seenView), name);
            pending = true;
            changedAt = GetTickCount();
            layoutPlatesRun("on a change", false);   // the set in force, published at once
            continue;
        }
        if (pending && GetTickCount() - changedAt >= 1000) {
            pending = false;
            layoutPlatesRun("after a change", true);
        }
    }
}

}  // namespace

long layoutPlatesGen() { return InterlockedCompareExchange(&g_layGen, 0, 0); }

long layoutPlatesState(int plateW, int plateH, char* suffix, size_t cap, bool* ready) {
    if (suffix && cap) suffix[0] = 0;
    if (ready) *ready = true;
    AcquireSRWLockShared(&g_layLock);
    const long gen = InterlockedCompareExchange(&g_layGen, 0, 0);
    if (g_lay.hash[0] && g_lay.plateW == plateW && g_lay.plateH == plateH &&
        !std::strcmp(g_lay.view, view().name)) {
        if (suffix && cap) _snprintf_s(suffix, cap, _TRUNCATE, "_L%s", g_lay.hash);
        if (ready) *ready = g_lay.ready;
    }
    ReleaseSRWLockShared(&g_layLock);
    return gen;
}

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

    gen::ViewSpec spec;
    spec.basemods = gen::basemodsOnCommandLine();
    setText(view().name, sizeof(view().name), spec.name().c_str());
    setText(view().startup, sizeof(view().startup), spec.name().c_str());
    view().tag[0] = 0;
    view().dir[0] = 0;
    configSetView(view().name);   // the start-up view's state file, before any read below
    const std::string lang = textLanguage();
    // plate_ground: the flat colour a MODDED cover plate is filled with. It is part of the
    // stamp, so a change to it regenerates the set once; the vanilla plates never use it.
    const std::string plateGround(g_cfg.plateGround);

    gen::GenReport report;
    StartCtx ctx = {gameDir, modDir, pagesArz, &lang};
    const gen::GenInputs inputs = generationInputs(selfModule, spec);
    const bool ok = gen::ensureOutputsGuarded(gameDir, modDir, pagesArz, plateDir, lang,
                                              plateGround, report, &sayStart, &ctx, &spec, &inputs);
    if (ok && report.upToDate) {
        for (const std::string& w : report.warnings) logW("catalogue: %s", w.c_str());
        logD("catalogue: catalogue.stamp matches the game files (Text_%s) - catalogue.bin and "
             "the uniq-*.txt files in \"%s\" are current",
             lang.c_str(), modDir);
        layoutPlatesStart(selfModule);
        return;
    }
    for (const std::string& w : report.warnings) logW("catalogue: %s", w.c_str());
    if (ok) {
        logI("catalogue: built - %s", report.summary.c_str());
        logGroups(report);
        if (!report.overlay.empty()) logI("catalogue: %s", report.overlay.c_str());
        if (!report.geometry.empty()) logI("catalogue: %s", report.geometry.c_str());
        if (!report.plates.empty()) logI("catalogue: %s", report.plates.c_str());
        if (!report.plateGround.empty()) logI("catalogue: %s", report.plateGround.c_str());
        if (!report.memory.empty()) logD("catalogue: %s", report.memory.c_str());
    } else {
        logE("catalogue: generation FAILED - %s. The files already in \"%s\" are kept",
             report.error.c_str(), modDir);
    }
    layoutPlatesStart(selfModule);
}

void layoutPlatesStart(HMODULE selfModule) {
    static volatile LONG started = 0;
    if (InterlockedExchange(&started, 1)) return;
    g_layModule = selfModule;
    layoutPlatesRun("at start-up", true);   // the worker, before any loader reads the plates
    HANDLE t = CreateThread(nullptr, 0, &layoutPlatesThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    else logW("layout plates: their thread could not be started - a grid change repaints at the next start");
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
    AcquireSRWLockExclusive(&g_viewLock);
    view().dir[0] = 0;
    view().tag[0] = 0;
    view().custom[0] = 0;
    view().settled[0] = 0;   // a later entry into the same game tries again
    setText(view().name, sizeof(view().name), view().startup);
    ReleaseSRWLockExclusive(&g_viewLock);
    logI("catalogue: the view is %s again - the main database is loading, so the custom game's "
         "records are gone with it",
         view().name);
    teeFollow(nullptr, view().name);   // the custom view's file ends with the line above
    configSetView(view().name);
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

    // Most custom games need no view: the Crucible, a map or a class mod leave the caravan and
    // the item records alone, so the start-up view's geometry AND catalogue describe them. A game
    // that touches the caravan, or adds or overrides a record the catalogue could collect (a loot
    // mod that leaves the caravan alone included), gets one (ut_viewprobe.h). The game's own
    // archives answer that in milliseconds, and a no costs nothing further - no generation, no
    // folder, and the start-up view goes on serving the tab.
    {
        gen::ModView probe;
        std::string perr;
        // A big game without items is walked to its end (the Crucible's 3,147 records: ~30 ms);
        // the stall check must not read a long walk as a crash.
        InterlockedIncrement(&g_generating);
        const ULONGLONG t0 = GetTickCount64();
        const bool loaded = probe.loadCustom(modFolder, &perr);
        UtViewProbe p;
        // collect_mi as THIS game will have it (its own file, or the main file's value it is
        // seeded from) - g_cfg is still the previous view's here
        if (loaded) utProbeView(probe, p, false, configViewIntFor(name.c_str(), "collect_mi") != 0);
        const unsigned ms = (unsigned)(GetTickCount64() - t0);
        InterlockedDecrement(&g_generating);
        if (loaded) {
            if (!p.needsView()) {
                logI("catalogue: the custom game in \"%s\" overrides neither the caravan nor an item "
                     "record - the %s view still describes it, so nothing is generated",
                     modFolder, view().name);
                logD("catalogue: %zu record(s), %zu read for the item test in %u ms", p.records,
                     p.decoded, ms);
                setText(view().settled, sizeof(view().settled), modFolder);
                return false;
            }
            if (p.caravan) {
                logD("catalogue: the custom game in \"%s\" overrides the caravan", modFolder);
            } else {
                logD("catalogue: the custom game in \"%s\" leaves the caravan alone but adds or "
                     "overrides item records (\"%s\" first; %zu of %zu record(s) read in %u ms)",
                     modFolder, p.sample.c_str(), p.decoded, p.records, ms);
            }
        }
    }

    // <mod folder>\mods\<leaf> for custom-<leaf>, <mod folder>\mods\<leaf>\basemods for
    // basemods+custom-<leaf> (ut_folders.h): the start-up view's files stay exactly where they
    // are, so leaving the custom game costs nothing and an installed copy that never enters one
    // sees no change at all.
    const std::string rel = utCustomViewRel(name.c_str());
    if (rel.empty()) {
        logW("catalogue: the view %s has no folder of its own - the tab keeps the %s view",
             name.c_str(), view().name);
        setText(view().settled, sizeof(view().settled), modFolder);
        return false;
    }
    const std::string dir = std::string(modDir) + "\\" + rel;
    utMakeRelDirs(modDir, rel);
    // From here the view's lines go to its own log as well, its generation's included, and the
    // game's own mods\<leaf>\uniquetab.ini (written populated at its first entry) and the view
    // folder's uniquetab.state are in force before the generation reads a setting.
    teeFollow(dir.c_str(), name.c_str());
    configSetView(name.c_str());
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
    const gen::GenInputs inputs = generationInputs(selfModule, spec);
    const bool ok = gen::ensureOutputsGuarded(gameDir, dir, pagesArz, plateDir, lang, plateGround,
                                              report, &sayStart, &ctx, &spec, &inputs);
    InterlockedDecrement(&g_generating);
    for (const std::string& w : report.warnings) logW("catalogue: %s", w.c_str());
    if (!ok) {
        logE("catalogue: the %s view could not be generated - %s. The tab keeps the %s view, "
             "which this world's caravan may not match",
             name.c_str(), report.error.c_str(), view().name);
        setText(view().settled, sizeof(view().settled), modFolder);
        // the view in force is still the previous one: its log and its settings
        teeFollow(view().dir, view().name);
        configSetView(view().name);
        return false;
    }
    if (report.upToDate) {
        logI("catalogue: the %s view is already current in \"%s\"", name.c_str(), dir.c_str());
    } else {
        logI("catalogue: built - %s", report.summary.c_str());
        logGroups(report);
        if (!report.overlay.empty()) logI("catalogue: %s", report.overlay.c_str());
        if (!report.geometry.empty()) logI("catalogue: %s", report.geometry.c_str());
        if (!report.plates.empty()) logI("catalogue: %s", report.plates.c_str());
        if (!report.plateGround.empty()) logI("catalogue: %s", report.plateGround.c_str());
        if (!report.memory.empty()) logD("catalogue: %s", report.memory.c_str());
    }
    AcquireSRWLockExclusive(&g_viewLock);
    setText(view().custom, sizeof(view().custom), modFolder);
    setText(view().dir, sizeof(view().dir), dir.c_str());
    setText(view().tag, sizeof(view().tag), spec.tag().c_str());
    setText(view().name, sizeof(view().name), name.c_str());
    ReleaseSRWLockExclusive(&g_viewLock);
    return true;
}

}  // namespace ut
