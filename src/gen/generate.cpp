// generate.cpp - see generate.h.
#include "gen/generate.h"
#include "gen/catalogue_gen.h"
#include "gen/heap_meter.h"
#include "gen/pages_gen.h"
#include "gen/plate_gen.h"
#include "gen/ut_groups.h"
#include "gen/ut_groups_default.h"
#include "ut_layout.h"
#include "model/catalogue.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <new>
#include <unordered_set>

namespace gen {

namespace {

const char* const kStampName = "catalogue.stamp";
// The view key the template's monster-infrequent blocks are filed under (ut_groups_default.h).
const char* const kCollectMiViewKey = "collect_mi";
const char* const kOutputs[4] = {"catalogue.bin", "uniq-records.txt", "uniq-pages.txt", "uniq-groups.txt"};

// The process's working set in MB, current and peak, 0 when the call is not there.
// K32GetProcessMemoryInfo lives in kernel32; resolving it at run time keeps the DLL from
// importing psapi.
//
// BOTH numbers are the WHOLE PROCESS. Inside the game that is mostly the engine: the generation
// runs on the start-up worker while the engine is loading its own database, so the raw peak
// there reads several hundred megabytes and says nothing about this code. What the log carries
// is therefore the RISE over the working set measured just before the generation started, which
// is the generator's own cost, plus the process peak for scale. The number matters because the
// generation is the heaviest thing the mod ever does and a Steam Deck shares 16 GB with its GPU.
bool workingSetMb(double* current, double* peak) {
    typedef BOOL(WINAPI * Fn)(HANDLE, PROCESS_MEMORY_COUNTERS*, DWORD);
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    FARPROC raw = k32 ? GetProcAddress(k32, "K32GetProcessMemoryInfo") : nullptr;
    Fn fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(raw));
    PROCESS_MEMORY_COUNTERS pmc;
    if (!fn || !fn(GetCurrentProcess(), &pmc, sizeof pmc)) return false;
    if (current) *current = double(pmc.WorkingSetSize) / (1024.0 * 1024.0);
    if (peak) *peak = double(pmc.PeakWorkingSetSize) / (1024.0 * 1024.0);
    return true;
}

bool fileExists(const std::string& p) {
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool readText(const std::string& p, std::string& out) {
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return false;
    char buf[4096];
    std::size_t n;
    out.clear();
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    std::fclose(f);
    return true;
}

bool writeBytes(const std::string& p, const void* data, std::size_t n) {
    FILE* f = std::fopen(p.c_str(), "wb");
    if (!f) return false;
    bool ok = n == 0 || std::fwrite(data, 1, n, f) == n;
    ok = (std::fclose(f) == 0) && ok;
    return ok;
}

std::string tmpName(const std::string& modDir, int i) {
    return modDir + "\\" + kOutputs[i] + ".tmp";
}

// The temporary names, whether this run or a crashed earlier one left them. The overlay is the
// fifth output and has a path of its own, because it is the file the tab loads.
void deleteTemporaries(const std::string& modDir, const std::string& pagesArz) {
    for (int i = 0; i < 4; ++i) DeleteFileA(tmpName(modDir, i).c_str());
    DeleteFileA((pagesArz + ".tmp").c_str());
}

// Deletes the temporaries on every exit that is not the rename into place - the plates'
// temporaries too, so a failed generation leaves the installed plate set untouched.
struct TmpGuard {
    const std::string& modDir;
    const std::string& pagesArz;
    PlateWriter* plates = nullptr;
    bool keep = false;
    TmpGuard(const std::string& d, const std::string& a) : modDir(d), pagesArz(a) {}
    ~TmpGuard() {
        if (keep) return;
        deleteTemporaries(modDir, pagesArz);
        if (plates)
            for (const std::pair<std::string, std::string>& r : plates->renames)
                DeleteFileA(r.first.c_str());
    }
};

// mkdir -p for the plate root: <game>\settings, then ui, then caravan.
void makeTree(const std::string& dir) {
    for (std::size_t i = 0; i <= dir.size(); ++i) {
        if (i != dir.size() && dir[i] != '\\' && dir[i] != '/') continue;
        if (i > 0) CreateDirectoryA(dir.substr(0, i).c_str(), nullptr);
    }
}

// ONE GENERATION AT A TIME. Re-entrant for its owner, so the guarded entries can take it OUTSIDE
// their __try (a fault skips the destructors under /EHsc and would leave it held) and the inner
// call's own hold only counts.
SRWLOCK g_genLock = SRWLOCK_INIT;
volatile DWORD g_genOwner = 0;
int g_genDepth = 0;   // touched by the owner only

void genLockEnter() {
    const DWORD me = GetCurrentThreadId();
    if (g_genOwner == me) { ++g_genDepth; return; }
    AcquireSRWLockExclusive(&g_genLock);
    g_genOwner = me;
    g_genDepth = 1;
}
void genLockLeave() {
    if (g_genOwner != GetCurrentThreadId()) return;
    if (--g_genDepth > 0) return;
    g_genOwner = 0;
    ReleaseSRWLockExclusive(&g_genLock);
}
// After a guarded call, faulted or not: whatever depth the fault left, the lock goes.
void genLockReset() {
    if (g_genOwner != GetCurrentThreadId()) return;
    g_genDepth = 0;
    g_genOwner = 0;
    ReleaseSRWLockExclusive(&g_genLock);
}

struct GenLockHold {
    GenLockHold() { genLockEnter(); }
    ~GenLockHold() { genLockLeave(); }
    GenLockHold(const GenLockHold&) = delete;
    GenLockHold& operator=(const GenLockHold&) = delete;
};

} // namespace

void generationLockAcquire() { genLockEnter(); }
void generationLockRelease() { genLockLeave(); }

bool basemodsOnCommandLine() {
    // The engine merges <game>\\mods\\database.arz into the main database only when /basemods is
    // on its command line, so the flag is part of WHICH database the geometry was derived from.
    const wchar_t* cl = GetCommandLineW();
    if (!cl) return false;
    for (const wchar_t* p = cl; *p; ++p) {
        if (*p != L'/' && *p != L'-') continue;
        const wchar_t* w = L"basemods";
        const wchar_t* q = p + 1;
        while (*w && *q && (*q | 0x20) == *w) { ++w; ++q; }
        if (!*w && (!*q || *q == L' ' || *q == L'\t')) return true;
    }
    return false;
}

namespace {

// Every .arz and .arc a custom game brings, sorted, so the fingerprint follows the mod's own
// files the way it follows the game's.
void customLayerFiles(const std::string& customDir, std::vector<std::string>& abs) {
    static const char* const kPat[2][2] = {{"\\database", "*.arz"}, {"\\resources", "*.arc"}};
    for (int i = 0; i < 2; ++i) {
        const std::string dir = customDir + kPat[i][0];
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((dir + "\\" + kPat[i][1]).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        std::vector<std::string> found;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                found.push_back(dir + "\\" + fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
        std::sort(found.begin(), found.end());
        for (const std::string& f : found) abs.push_back(f);
    }
}

// size + mtime of one file, or "missing".
std::string stampLine(const std::string& path, const std::string& label) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    char line[512];
    if (GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fa)) {
        unsigned long long size = (unsigned long long(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
        unsigned long long mt = (unsigned long long(fa.ftLastWriteTime.dwHighDateTime) << 32)
                              | fa.ftLastWriteTime.dwLowDateTime;
        std::snprintf(line, sizeof line, "%s %llu %llu\n", label.c_str(), size, mt);
    } else {
        std::snprintf(line, sizeof line, "%s missing\n", label.c_str());
    }
    return line;
}

// Why a stamp that is there does not match. Its first line is the output format: when that
// differs, a new build of the mod changed what it writes, and the game may be exactly as it was.
std::string stampMismatch(const std::string& old, const std::string& now) {
    const std::string was = old.substr(0, old.find('\n'));
    const std::string is = now.substr(0, now.find('\n'));
    if (was != is)
        return "catalogue.stamp does not match: it is \"" + was + "\" and this build writes \"" +
               is + "\" (the mod was updated)";
    return "catalogue.stamp does not match the game files (the game was updated)";
}

} // namespace

std::string stampHash(const std::string& bytes) {
    unsigned long long h = 1469598103934665603ULL;
    for (unsigned char c : bytes) { h ^= c; h *= 1099511628211ULL; }
    char buf[24];
    std::snprintf(buf, sizeof buf, "%016llx", h);
    return buf;
}

std::string inputFingerprint(const std::string& gameDir, const std::string& lang,
                             const std::string& plateGround, const ViewSpec* view,
                             const std::string& extra) {
    static const char* const kSub[4] = {"", "gdx1\\", "gdx2\\", "gdx3\\"};
    static const char* const kArz[4] = {"database\\database.arz", "gdx1\\database\\GDX1.arz",
                                        "gdx2\\database\\GDX2.arz", "gdx3\\database\\GDX3.arz"};
    std::vector<std::string> rel;
    for (const char* a : kArz) rel.push_back(a);
    for (const char* s : kSub) {
        rel.push_back(std::string(s) + "resources\\Items.arc");
        rel.push_back(std::string(s) + "resources\\UI.arc");
        rel.push_back(std::string(s) + "resources\\Level Art.arc");
        rel.push_back(std::string(s) + "resources\\Text_" + lang + ".arc");
    }
    // The basemod layer is an input like any other, and so is the flag that turns it on:
    // toggling /basemods changes the merged database and must regenerate.
    const ViewSpec startup = {basemodsOnCommandLine(), std::string()};
    const ViewSpec& v = view ? *view : startup;
    const bool basemods = v.basemods;
    if (basemods) {
        rel.push_back("mods\\database.arz");
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((gameDir + "\\mods\\*.arc").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            std::vector<std::string> arcs;
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    arcs.push_back(std::string("mods\\") + fd.cFileName);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
            std::sort(arcs.begin(), arcs.end());
            for (const std::string& a : arcs) rel.push_back(a);
        }
    }
    // The first line is the OUTPUT format: bump it whenever the generated files change for the
    // same inputs, so an installation that already has a stamp regenerates once with the new
    // build instead of keeping text the DLL no longer expects. 2 = the P (geometry) line;
    // 3 = uniq-pages.arz is generated beside them; 4 = the origin rule (stash grid);
    // 5 = the cover plates are painted from the derived ground; 6 = a modded plate is a flat
    // fill, and plate_ground picks its colour - so a changed colour regenerates the set;
    // 7 = the outputs are per view, and a custom game's archives are part of the fingerprint;
    // 8 = the catalogue is built from the view, the groups file is an input, the G line carries
    // the group's id and tag, and the frame always carries 160 boxes; 9 = the frame's box count
    // is the view's own (ut::utFrameBoxes: the cover's fit, 160..512), so every view regenerates
    // once with its frame and the grids fitted under it. The user's grid override is not an
    // input: it only moves the plates, which carry their own hash (ensureLayoutPlates).
    // 10 = the base game's records are judged by the junk list too, not the records/items/ path
    // rule (its real items outside that folder - the Lokarr set, the quest rewards - are
    // collected), and Level Art.arc is an icon archive (the Lokarr set's icons); 11 = the
    // endless dungeon's script entities are no longer a junk folder (the Totally Normal Shield's
    // three versions are collected).
    std::string fp = "GDUT-STAMP 13\nlang=" + lang + "\n";
    if (basemods) fp += "basemods=1\n";
    // A custom view's plate names always carry its tag; the line makes a view generated
    // before that rule regenerate once, and leaves the start-up view's stamp as it was.
    if (!v.customDir.empty()) fp += "view=" + v.name() + "\nplates=tagged\n";
    if (!plateGround.empty()) fp += "plate_ground=" + plateGround + "\n";
    fp += extra;
    for (const std::string& r : rel) fp += stampLine(gameDir + "\\" + r, r);
    if (!v.customDir.empty()) {
        std::vector<std::string> abs;
        customLayerFiles(v.customDir, abs);
        if (abs.empty()) fp += "custom missing\n";
        for (const std::string& a : abs) fp += stampLine(a, a);
    }
    return fp;
}

bool ensureOutputs(const std::string& gameDir, const std::string& modDir,
                   const std::string& pagesArz, const std::string& plateDir,
                   const std::string& lang, const std::string& plateGround, GenReport& report,
                   GenStartFn onStart, void* ctx, const ViewSpec* view,
                   const GenInputs* inputs) {
    GenLockHold hold;   // one generation at a time: start-up, a view switch, a layout repaint
    const ViewSpec startup = {basemodsOnCommandLine(), std::string()};
    const ViewSpec& v = view ? *view : startup;
    static const GenInputs kNoInputs;
    const GenInputs& in = inputs ? *inputs : kNoInputs;
    report = GenReport();
    report.view = v.name();
    // The groups: the built-in set (ut_groups_default.h, compiled in, never written) with the
    // user's optional uniquetab-groups.ini merged IN FRONT of it (utg::mergeGroupSets: its blocks
    // are tried first). A file that is an unedited copy of a template an earlier build wrote is
    // renamed .old and said so in the groups line; a file that cannot be read or parsed is one
    // warning naming it and its line, and the built-in set alone. The built-in text's hash and
    // the user's bytes are stamp inputs, so a new build's groups or an edit regenerate once.
    utg::GroupSet groupSet;
    utg::GroupsLoad groupsLoad;
    utg::loadGroupsFile(in.groupsPath, groupSet, &groupsLoad);
    if (groupsLoad.from == utg::GroupsFile::Refused)
        report.warnings.push_back("uniquetab-groups.ini is ignored - " + groupsLoad.why +
                                  "; the built-in groups are used");
    std::string extra = "groups=builtin:" + stampHash(utg::defaultGroupsIni());
    if (groupsLoad.from == utg::GroupsFile::Loaded) extra += "+user:" + stampHash(groupsLoad.bytes);
    extra += "\n";
    // collect_mi: a line only while it is on, so a flag-off stamp is byte for byte the one an
    // earlier build wrote (no regeneration for it) and a flip either way regenerates once.
    if (in.collectMi) extra += "collect_mi=1\n";
    {
        const utg::MergeStats& ms = groupsLoad.stats;
        if (groupsLoad.from == utg::GroupsFile::Loaded) {
            report.groups = "groups from " + in.groupsPath + ": " + std::to_string(ms.builtin) +
                            " built-in + " + std::to_string(ms.yours) + " of yours (" +
                            std::to_string(ms.extended) + " built-in group(s) given more rules)";
        } else {
            report.groups = "the built-in groups (" + std::to_string(ms.builtin) + ")";
            if (groupsLoad.from == utg::GroupsFile::Retired)
                report.groups += "; " + in.groupsPath + " was an unedited copy of an old built-in "
                                 "template and is renamed " + groupsLoad.renamedTo;
        }
    }
    std::string fp = inputFingerprint(gameDir, lang, plateGround, &v, extra);
    std::string stampPath = modDir + "\\" + kStampName;
    std::string old;
    for (const char* o : kOutputs) {
        if (!fileExists(modDir + "\\" + o)) { report.reason = std::string(o) + " is missing"; break; }
    }
    if (report.reason.empty() && !fileExists(pagesArz)) report.reason = "uniq-pages.arz is missing";
    if (report.reason.empty()) {
        if (!readText(stampPath, old)) report.reason = "no catalogue.stamp yet (first launch)";
        else if (old != fp) report.reason = stampMismatch(old, fp);
    }
    if (report.reason.empty()) {
        report.upToDate = true;
        return true;
    }
    if (onStart) onStart(report, ctx);

    // Before the first archive is opened, so the log can say what the generation itself
    // added rather than what the whole process happens to be holding.
    double wsBefore = 0.0, peakBefore = 0.0;
    workingSetMb(&wsBefore, &peakBefore);
    // What this generation holds itself, counted on this thread: the process numbers also carry
    // whatever the engine loads beside it.
    HeapMeter meter;

    auto t0 = std::chrono::steady_clock::now();
    deleteTemporaries(modDir, pagesArz);
    std::string err;
    // The view: the merged database the engine will have for THIS view - the basemod layer,
    // then a custom game's archives on top of it, which is the order the engine loads them in.
    // A failed layer is a warning and the view falls back to what was readable. The catalogue,
    // the names, the icons and the geometry are all read through it.
    ModView mods;
    if (!loadView(gameDir, v, mods, &err)) {
        report.warnings.push_back("the " + v.name() + " view's archives could not be read (" +
                                  err + "); the geometry is what was readable");
    }
    for (const std::string& s : mods.skipped())
        report.warnings.push_back("the " + v.name() + " view skips an unreadable archive (" + s +
                                  "); the rest of its archives are read");
    GameData g;
    if (!g.load(gameDir, lang, &err, mods.empty() ? nullptr : &mods)) {
        report.error = "reading the game archives: " + err;
        return false;
    }
    // What the view's journals own (read only: no writer has them open while a view
    // generates) - for the report alone: nothing generated depends on it. An owned record no
    // group takes stays in the journal and on no page; the report names those.
    std::unordered_set<std::string> owned;
    bool ownedKnown = true;
    {
        std::string why;
        if (!utg::ownedFromJournals(in.journals, owned, &why)) {
            ownedKnown = false;
            report.warnings.push_back("a journal of the view could not be read (" + why + "); the "
                                      "owned records no page shows are not counted this time");
        }
    }
    std::vector<CatalogueItem> items;
    if (!collectItems(g, items, report.warnings, &err, in.collectMi)) { report.error = "the collection rule: " + err; return false; }
    std::vector<std::uint8_t> bin;
    CatalogueStats st;
    if (!packCatalogue(items, bin, &st, &err)) { report.error = "packing catalogue.bin: " + err; return false; }
    // the view's own word for each item class: the derived group labels and tags
    groupSet.words = classWords(g);
    GroupsInput gin;
    gin.set = &groupSet;
    gin.viewKeys = utg::viewKeysFor(v.name());
    gin.plateTag = v.tag();
    // the groups' monster-infrequent blocks ([view:collect_mi], ut_groups_default.h) are on only
    // with the flag: off, no MI is in the catalogue and no block pages one (a stored one waits in
    // the journal, counted below and on the label)
    if (in.collectMi) gin.viewKeys.push_back(kCollectMiViewKey);
    PagesOutput po;
    if (!generatePages(g, items, po, &err, mods.empty() ? nullptr : &mods, &gin)) {
        report.error = "the page layout: " + err;
        return false;
    }
    // temporary names first; nothing in the folder changes until all five are good
    TmpGuard guard(modDir, pagesArz);
    // The plates, from the same geometry and the same cell grids the layout just derived. They
    // go to the engine's own texture override root, which is the only place a relative
    // "ui/caravan/..." name resolves from, and they are renamed with everything else.
    PlateWriter plates;
    PlateReport prep;
    if (!plateDir.empty() && !po.cells.empty()) {
        plates.dir = plateDir;
        guard.plates = &plates;
        makeTree(plateDir);
        if (!generatePlates(g, mods.empty() ? nullptr : &mods, po.geo, po.cells, plateGround,
                            v.tag(), &plateWriteSink, &plates, prep, &err)) {
            report.warnings.push_back("the cover plates could not be painted (" + err +
                                      "); the plates already installed are kept");
            for (const std::pair<std::string, std::string>& r : plates.renames)
                DeleteFileA(r.first.c_str());
            plates.renames.clear();
        }
    }
    if (!writeBytes(pagesArz + ".tmp", po.arz.data(), po.arz.size())) {
        report.error = "writing " + pagesArz + ".tmp";
        return false;
    }
    // the read-back: what was just written must hold every box the text names
    if (!checkPageCapacity(pagesArz + ".tmp", po, &err)) {
        report.error = "the generated uniq-pages.arz: " + err;
        return false;
    }
    const void* data[4] = {bin.data(), po.records.data(), po.pages.data(), po.groups.data()};
    std::size_t size[4] = {bin.size(), po.records.size(), po.pages.size(), po.groups.size()};
    for (int i = 0; i < 4; ++i) {
        if (!writeBytes(tmpName(modDir, i), data[i], size[i])) {
            report.error = "writing " + tmpName(modDir, i);
            return false;
        }
    }
    {
        gdut::Catalogue check;
        if (!check.loadFromFile(tmpName(modDir, 0), &err) || check.itemCount() != st.items) {
            report.error = "the generated catalogue.bin does not load: " + err;
            return false;
        }
    }
    for (int i = 0; i <= 4; ++i) {
        std::string src = i < 4 ? tmpName(modDir, i) : pagesArz + ".tmp";
        std::string dst = i < 4 ? modDir + "\\" + kOutputs[i] : pagesArz;
        if (!MoveFileExA(src.c_str(), dst.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            report.error = "renaming " + src + " into place (error " +
                           std::to_string(GetLastError()) + ")";
            return false;
        }
    }
    for (const std::pair<std::string, std::string>& r : plates.renames) {
        if (!MoveFileExA(r.first.c_str(), r.second.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileA(r.first.c_str());
            ++plates.failed;
            --plates.written;
        }
    }
    guard.keep = true;
    writeBytes(stampPath, fp.data(), fp.size());   // a lost stamp only costs one regeneration

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    char line[256];
    std::snprintf(line, sizeof line,
                  "%zu items, %zu sets, %zu collectible records on %zu pages in %zu groups, %lld ms",
                  st.items, st.sets, po.itemCount, po.pageCount, po.groupList.size(), (long long)ms);
    report.summary = line;
    if (in.collectMi) {
        std::snprintf(line, sizeof line,
                      " (collect_mi=1: %zu monster infrequent record(s) in the catalogue)", st.mi);
        report.summary += line;
    }
    std::snprintf(line, sizeof line, "overlay %zu records, %zu KB, frame at %d,%d with %d boxes",
                  po.arzRecords, (po.arz.size() + 1023) / 1024, po.frameX, po.frameY,
                  po.frameBoxes);
    report.overlay = line;
    report.geometry = po.geo.note;   // names the record the origin came from, for a modded view
    if (prep.painted) {
        std::snprintf(line, sizeof line,
                      "%zu cover plate(s) painted, %zu written to \"%s\", %zu already current; %s",
                      prep.painted, plates.written, plateDir.c_str(), plates.same,
                      prep.ground.c_str());
        report.plates = line;
        report.plateGround = prep.fill;   // empty on the vanilla view: nothing is filled there
    }
    for (const std::string& w : prep.warnings) report.warnings.push_back(w);
    if (plates.failed) {
        report.warnings.push_back("could not write " + std::to_string(plates.failed) +
                                  " cover plate(s) into \"" + plateDir +
                                  "\" - the plates already installed are used instead");
    }
    if (po.cappedGroups) {
        std::snprintf(line, sizeof line, " (%zu group(s) capped at the frame and paginated)",
                      po.cappedGroups);
        report.overlay += line;
    }
    double wsAfter = 0.0, peakMb = 0.0;
    if (workingSetMb(&wsAfter, &peakMb) && peakMb > 0.0) {
        const double rise = peakMb > wsBefore ? peakMb - wsBefore : 0.0;
        report.ownPeakMb = meter.active() ? meter.peakMb() : rise;
        std::snprintf(line, sizeof line, ", %.0f MB for this (process peak %.0f MB)",
                      report.ownPeakMb, peakMb);
        report.summary += line;
        char mem[384];
        std::snprintf(mem, sizeof mem,
                      "memory - %s %.1f MB of heap at its peak; the process: working set %.0f MB "
                      "before and %.0f MB after, peak %.0f MB before and %.0f MB after (its rise, "
                      "%.0f MB, also counts whatever else the process loaded meanwhile)",
                      meter.active() ? "the generation held" : "not metered, the process rise is",
                      report.ownPeakMb, wsBefore, wsAfter, peakBefore, peakMb, rise);
        report.memory = mem;
    }
    // The owned records no page of this view shows: the ones the catalogue has and no group
    // takes, and the ones it does not have at all (an MI with collect_mi=0, a record the mod
    // dropped). Each keeps its journal row and is back the moment a group takes it again.
    std::vector<std::string> notShown;
    if (!owned.empty()) {
        std::unordered_set<std::string> paged;
        std::size_t at = 0;
        while (at < po.records.size()) {
            std::size_t nl = po.records.find('\n', at);
            if (nl == std::string::npos) nl = po.records.size();
            std::string r = po.records.substr(at, nl - at);
            at = nl + 1;
            while (!r.empty() && (r.back() == '\r' || r.back() == ' ')) r.pop_back();
            if (!r.empty()) paged.insert(utg::normPath(r));
        }
        for (const std::string& k : owned)
            if (!paged.count(utg::normPath(k))) notShown.push_back(k);
        std::sort(notShown.begin(), notShown.end());
    }
    report.notShown = notShown.size();
    if (!notShown.empty()) {
        std::string names;
        for (std::size_t i = 0; i < notShown.size() && i < 5; ++i)
            names += (i ? ", " : "") + notShown[i];
        report.warnings.push_back(std::to_string(notShown.size()) + " owned record(s) no group of "
                                  "this game takes - kept in the collection, not shown: " + names +
                                  (notShown.size() > 5 ? ", ..." : ""));
    }
    report.groups += ", " + std::to_string(po.groupList.size()) + " group(s), " +
                     (ownedKnown ? std::to_string(notShown.size()) : std::string("?")) +
                     " owned not shown";
    report.generated = true;
    return true;
}

namespace {

struct GenArgs {
    const std::string* gameDir;
    const std::string* modDir;
    const std::string* pagesArz;
    const std::string* plateDir;
    const std::string* lang;
    const std::string* plateGround;
    GenStartFn onStart;
    void* ctx;
    const ViewSpec* view;
    const GenInputs* inputs;
};

bool runCatching(const GenArgs& a, GenReport& report) {
    try {
        return ensureOutputs(*a.gameDir, *a.modDir, *a.pagesArz, *a.plateDir, *a.lang,
                             *a.plateGround, report, a.onStart, a.ctx, a.view, a.inputs);
    } catch (const std::bad_alloc&) {
        // names memory, so the next report from a small machine diagnoses itself
        report.error = "out of memory while reading the archives";
    } catch (const std::exception& e) {
        report.error = std::string("an exception: ") + e.what();
    } catch (...) {
        report.error = "an exception";
    }
    return false;
}

void noteFault(const GenArgs& a, GenReport& report, unsigned long code) {
    char line[96];
    std::snprintf(line, sizeof line, "a fault (exception 0x%08lX) while reading the archives", code);
    report.error = line;
    report.generated = false;
    deleteTemporaries(*a.modDir, *a.pagesArz);
}

// No object with a destructor may live in the frame that holds __try (C2712).
bool runGuarded(const GenArgs& a, GenReport& report) {
    __try {
        return runCatching(a, report);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        noteFault(a, report, GetExceptionCode());
    }
    return false;
}

} // namespace

bool ensureOutputsGuarded(const std::string& gameDir, const std::string& modDir,
                          const std::string& pagesArz, const std::string& plateDir,
                          const std::string& lang, const std::string& plateGround,
                          GenReport& report, GenStartFn onStart, void* ctx,
                          const ViewSpec* view, const GenInputs* inputs) {
    GenArgs a = {&gameDir, &modDir, &pagesArz, &plateDir, &lang, &plateGround, onStart, ctx, view,
                 inputs};
    genLockEnter();
    const bool ok = runGuarded(a, report);
    genLockReset();
    return ok;
}

// ---- the layout plates ------------------------------------------------------------------------

std::string layoutPlateHash(const std::string& stampText, const std::string& gridText,
                            const std::vector<CellGrid>& cells, const std::string& plateGround) {
    std::string t = stampText;
    t += "\n";
    t += gridText;
    t += "\n";
    char buf[64];
    for (const CellGrid& c : cells) {
        std::snprintf(buf, sizeof buf, "%dx%d:%dx%d\n", c.cellW, c.cellH, c.cols, c.rows);
        t += buf;
    }
    t += "plate_ground=" + plateGround + "\n";
    // the painter's lattice rule: a set painted under an older one (the last column of a grid
    // flush with the plate's edge left bare) is superseded and painted again on the next entry
    std::snprintf(buf, sizeof buf, "lattice=%d\n", ut::kUtLatticeRule);
    t += buf;
    return stampHash(t).substr(8);   // the low 32 bits, eight hex digits
}

bool readGroupsGeometry(const std::string& groupsPath, UiGeometry& geo, int* maxCellW,
                        int* maxCellH, int* rowGap, std::vector<CellGrid>* cells,
                        int* frameBoxes) {
    std::string text;
    if (!readText(groupsPath, text)) return false;
    bool haveP = false;
    int mw = 0, mh = 0, gap = 2, nmax = 0;
    std::vector<CellGrid> found;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t nl = text.find('\n', at);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(at, nl - at);
        at = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> f;
        std::size_t b = 0;
        for (;;) {
            const std::size_t t = line.find('\t', b);
            f.push_back(line.substr(b, t == std::string::npos ? std::string::npos : t - b));
            if (t == std::string::npos) break;
            b = t + 1;
        }
        if (f[0] == "P" && f.size() >= 7) {
            geo.x0 = std::atoi(f[1].c_str());
            geo.y0 = std::atoi(f[2].c_str());
            geo.usableW = std::atoi(f[3].c_str());
            geo.usableH = std::atoi(f[4].c_str());
            geo.plateW = std::atoi(f[5].c_str());
            geo.plateH = std::atoi(f[6].c_str());
            geo.plate = f.size() >= 8 ? f[7] : std::string();
            // ui_geometry: usableW = plateW - (x0 - bitmapPositionX), unless the line carries
            // the cover's position itself (a grid that runs past its cover).
            geo.bmpX = f.size() >= 10 ? std::atoi(f[8].c_str()) : geo.x0 + geo.usableW - geo.plateW;
            geo.bmpY = f.size() >= 10 ? std::atoi(f[9].c_str()) : geo.y0 + geo.usableH - geo.plateH;
            haveP = geo.plateW > 0 && geo.plateH > 0;
        } else if (f[0] == "F" && f.size() >= 7) {
            nmax = std::atoi(f[2].c_str());
            gap = std::atoi(f[6].c_str());
        } else if (f[0] == "G" && f.size() >= 7) {
            CellGrid c;
            c.cols = std::atoi(f[3].c_str());
            c.rows = std::atoi(f[4].c_str());
            c.cellW = std::atoi(f[5].c_str());
            c.cellH = std::atoi(f[6].c_str());
            if (c.cellW < 1 || c.cellH < 1) continue;
            mw = (std::max)(mw, c.cellW);
            mh = (std::max)(mh, c.cellH);
            bool have = false;
            for (const CellGrid& o : found)
                if (o.cellW == c.cellW && o.cellH == c.cellH) { have = true; break; }
            if (!have) found.push_back(c);
        }
    }
    std::sort(found.begin(), found.end(), [](const CellGrid& a, const CellGrid& b) {
        return a.cellW != b.cellW ? a.cellW < b.cellW : a.cellH < b.cellH;
    });
    if (maxCellW) *maxCellW = mw;
    if (maxCellH) *maxCellH = mh;
    if (rowGap) *rowGap = gap;
    if (cells) *cells = found;
    // a file without an F line (or an unreadable count) is fitted at the floor, as before 9
    if (frameBoxes) *frameBoxes = nmax > 0 ? nmax : ut::kUtFrameBoxFloor;
    return haveP;
}

bool planLayoutPlates(const std::string& outDir, const LayoutGrid& grid,
                      const std::string& plateGround, const std::string& plateTag,
                      LayoutPlan& plan, std::string* error) {
    plan = LayoutPlan();
    std::string stamp;
    if (!readText(outDir + "\\" + kStampName, stamp)) {
        if (error) *error = "the view has no catalogue.stamp yet (its main generation has not run)";
        return false;
    }
    std::vector<CellGrid> sizes;
    int frameBoxes = ut::kUtFrameBoxFloor;
    if (!readGroupsGeometry(outDir + "\\uniq-groups.txt", plan.geo, nullptr, nullptr,
                            &plan.rowGap, &sizes, &frameBoxes) || sizes.empty()) {
        if (error) *error = "the view's uniq-groups.txt has no P line or no group";
        return false;
    }
    if (grid.w < 1 || grid.h < 1) {
        if (error) *error = "the grid rectangle is empty";
        return false;
    }
    const ut::UtRect r = {grid.x, grid.y, grid.w, grid.h};
    for (const CellGrid& s : sizes) {
        // At a zoom the cell the live page shows (ut::utLiveGroupGrid): magnified, clamped so one
        // fits the rectangle; two generated sizes may give one shown size (one plate). At 100
        // the generated cell, as before.
        const ut::UtZoomFit z = ut::utZoomFit(r, s.cellW, s.cellH, plan.rowGap, grid.zoom);
        CellGrid c = s;
        c.cellW = z.cellW;
        c.cellH = z.cellH;
        bool have = false;
        for (const CellGrid& o : plan.cells) have = have || (o.cellW == c.cellW && o.cellH == c.cellH);
        if (have) continue;
        // under the view's frame (the F line), exactly as the live page fits it (g_nmax)
        const ut::UtGrid fit = ut::utGridFit(r, c.cellW, c.cellH, plan.rowGap, frameBoxes);
        c.cols = fit.cols;
        c.rows = fit.rows;
        plan.cells.push_back(c);
    }
    std::sort(plan.cells.begin(), plan.cells.end(), [](const CellGrid& a, const CellGrid& b) {
        return a.cellW != b.cellW ? a.cellW < b.cellW : a.cellH < b.cellH;
    });
    plan.geo.x0 = grid.x;
    plan.geo.y0 = grid.y;
    plan.geo.usableW = grid.w;
    plan.geo.usableH = grid.h;
    plan.hash = layoutPlateHash(stamp, grid.text, plan.cells, plateGround);
    for (const CellGrid& c : plan.cells)
        plan.names.push_back(plateLeafName(c.cellW, c.cellH, plan.geo.plateW, plan.geo.plateH,
                                           plateTag, plan.hash));
    return true;
}

namespace {

// The hash of a file name that is one of OUR layout plates for this cover and tag
// ("uniq_plate_<cw>x<ch>_<cover>[_<tag>]_L<8 hex>.tex"), else empty.
std::string layoutHashOf(const std::string& leaf, const std::string& cover, const std::string& tag) {
    static const char kPre[] = "uniq_plate_";
    const std::size_t np = sizeof kPre - 1;
    if (leaf.size() < np + 4 || leaf.compare(0, np, kPre) != 0) return std::string();
    if (leaf.compare(leaf.size() - 4, 4, ".tex") != 0) return std::string();
    const std::string stem = leaf.substr(np, leaf.size() - np - 4);
    if (stem.size() < 11 || stem.compare(stem.size() - 10, 2, "_L") != 0) return std::string();
    const std::string h = stem.substr(stem.size() - 8);
    for (char c : h)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return std::string();
    const std::string mid = stem.substr(0, stem.size() - 10);   // <cw>x<ch>_<cover>[_<tag>]
    std::size_t i = 0;
    auto digits = [&]() {
        const std::size_t s0 = i;
        while (i < mid.size() && mid[i] >= '0' && mid[i] <= '9') ++i;
        return i > s0;
    };
    if (!digits() || i >= mid.size() || mid[i] != 'x') return std::string();
    ++i;
    if (!digits() || i >= mid.size() || mid[i] != '_') return std::string();
    ++i;
    const std::string want = tag.empty() ? cover : cover + "_" + tag;
    return mid.compare(i, std::string::npos, want) == 0 ? h : std::string();
}

} // namespace

bool ensureLayoutPlates(const std::string& gameDir, const std::string& outDir,
                        const std::string& plateDir, const std::string& lang,
                        const std::string& plateGround, const ViewSpec& view,
                        const LayoutGrid* grid, LayoutPlatesReport& report) {
    GenLockHold hold;
    report = LayoutPlatesReport();
    const auto t0 = std::chrono::steady_clock::now();
    const std::string tag = view.tag();
    UiGeometry pg;
    if (!readGroupsGeometry(outDir + "\\uniq-groups.txt", pg, nullptr, nullptr, nullptr, nullptr)) {
        report.error = "the view's uniq-groups.txt has no P line (its main generation has not run)";
        return false;
    }
    char cover[32];
    std::snprintf(cover, sizeof cover, "%dx%d", pg.plateW, pg.plateH);
    std::string err;
    if (grid) {
        LayoutPlan plan;
        if (!planLayoutPlates(outDir, *grid, plateGround, tag, plan, &err)) {
            report.error = err;
            return false;
        }
        report.hash = plan.hash;
        report.names = plan.names;
        bool all = true;
        for (const std::string& n : plan.names)
            if (!fileExists(plateDir + "\\" + n + ".tex")) { all = false; break; }
        if (all) {
            report.upToDate = true;
        } else {
            HeapMeter meter;   // what the plates-only pass holds on this thread
            {
                ModView mods;
                if (!loadView(gameDir, view, mods, &err))
                    report.warnings.push_back("the " + view.name() + " view's archives could not "
                                              "be read (" + err + "); the plates use what was readable");
                GameData g;
                if (!g.load(gameDir, lang, &err, mods.empty() ? nullptr : &mods)) {
                    report.error = "reading the game archives: " + err;
                    return false;
                }
                makeTree(plateDir);
                PlateWriter w;
                w.dir = plateDir;
                PlateReport prep;
                if (!generatePlates(g, mods.empty() ? nullptr : &mods, plan.geo, plan.cells,
                                    plateGround, tag, &plateWriteSink, &w, prep, &err, plan.hash,
                                    plan.rowGap)) {
                    for (const std::pair<std::string, std::string>& r : w.renames)
                        DeleteFileA(r.first.c_str());
                    report.error = "the layout plates could not be painted: " + err;
                    return false;
                }
                for (const std::pair<std::string, std::string>& r : w.renames) {
                    if (!MoveFileExA(r.first.c_str(), r.second.c_str(),
                                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                        DeleteFileA(r.first.c_str());
                        ++w.failed;
                        --w.written;
                    }
                }
                report.painted = true;
                report.written = w.written;
                report.same = w.same;
                report.failed = w.failed;
                report.ground = prep.ground;
                for (const std::string& s : prep.warnings) report.warnings.push_back(s);
                if (w.failed)
                    report.warnings.push_back("could not write " + std::to_string(w.failed) +
                                              " layout plate(s) into \"" + plateDir + "\"");
            }
            report.ownPeakMb = meter.active() ? meter.peakMb() : 0.0;
        }
    }
    // Only our own superseded names: uniq_plate_*_<cover>[_<tag>]_L<8 hex>.tex with another hash.
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((plateDir + "\\uniq_plate_*_L*.tex").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        std::vector<std::string> gone;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const std::string hh = layoutHashOf(fd.cFileName, cover, tag);
            if (!hh.empty() && hh != report.hash) gone.push_back(plateDir + "\\" + fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
        for (const std::string& f : gone) {
            if (DeleteFileA(f.c_str())) ++report.removed;
            else ++report.removeFailed;   // in use or read-only: the next call tries again
        }
    }
    report.ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0).count();
    return true;
}

namespace {

struct LayArgs {
    const std::string* gameDir;
    const std::string* outDir;
    const std::string* plateDir;
    const std::string* lang;
    const std::string* plateGround;
    const ViewSpec* view;
    const LayoutGrid* grid;
};

bool layCatching(const LayArgs& a, LayoutPlatesReport& report) {
    try {
        return ensureLayoutPlates(*a.gameDir, *a.outDir, *a.plateDir, *a.lang, *a.plateGround,
                                  *a.view, a.grid, report);
    } catch (const std::bad_alloc&) {
        report.error = "out of memory while painting the layout plates";
    } catch (const std::exception& e) {
        report.error = std::string("an exception: ") + e.what();
    } catch (...) {
        report.error = "an exception";
    }
    return false;
}

void noteLayFault(LayoutPlatesReport& report, unsigned long code) {
    char line[96];
    std::snprintf(line, sizeof line, "a fault (exception 0x%08lX) while painting the layout plates",
                  code);
    report.error = line;
    report.painted = false;
}

// No object with a destructor may live in the frame that holds __try (C2712).
bool layGuarded(const LayArgs& a, LayoutPlatesReport& report) {
    __try {
        return layCatching(a, report);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        noteLayFault(report, GetExceptionCode());
    }
    return false;
}

} // namespace

bool ensureLayoutPlatesGuarded(const std::string& gameDir, const std::string& outDir,
                               const std::string& plateDir, const std::string& lang,
                               const std::string& plateGround, const ViewSpec& view,
                               const LayoutGrid* grid, LayoutPlatesReport& report) {
    LayArgs a = {&gameDir, &outDir, &plateDir, &lang, &plateGround, &view, grid};
    genLockEnter();
    const bool ok = layGuarded(a, report);
    genLockReset();
    return ok;
}

} // namespace gen
