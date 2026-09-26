// generate.cpp - see generate.h.
#include "gen/generate.h"
#include "gen/catalogue_gen.h"
#include "gen/heap_meter.h"
#include "gen/pages_gen.h"
#include "gen/plate_gen.h"
#include "model/catalogue.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <new>

namespace gen {

namespace {

const char* const kStampName = "catalogue.stamp";
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

} // namespace

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

std::string inputFingerprint(const std::string& gameDir, const std::string& lang,
                             const std::string& plateGround, const ViewSpec* view) {
    static const char* const kSub[4] = {"", "gdx1\\", "gdx2\\", "gdx3\\"};
    static const char* const kArz[4] = {"database\\database.arz", "gdx1\\database\\GDX1.arz",
                                        "gdx2\\database\\GDX2.arz", "gdx3\\database\\GDX3.arz"};
    std::vector<std::string> rel;
    for (const char* a : kArz) rel.push_back(a);
    for (const char* s : kSub) {
        rel.push_back(std::string(s) + "resources\\Items.arc");
        rel.push_back(std::string(s) + "resources\\UI.arc");
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
    // 7 = the outputs are per view, and a custom game's archives are part of the fingerprint.
    std::string fp = "GDUT-STAMP 7\nlang=" + lang + "\n";
    if (basemods) fp += "basemods=1\n";
    if (!v.customDir.empty()) fp += "view=" + v.name() + "\n";
    if (!plateGround.empty()) fp += "plate_ground=" + plateGround + "\n";
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
                   GenStartFn onStart, void* ctx, const ViewSpec* view) {
    const ViewSpec startup = {basemodsOnCommandLine(), std::string()};
    const ViewSpec& v = view ? *view : startup;
    report = GenReport();
    report.view = v.name();
    std::string fp = inputFingerprint(gameDir, lang, plateGround, &v);
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
    GameData g;
    std::string err;
    if (!g.load(gameDir, lang, &err)) { report.error = "reading the game archives: " + err; return false; }
    std::vector<CatalogueItem> items;
    if (!collectItems(g, items, report.warnings, &err)) { report.error = "the collection rule: " + err; return false; }
    std::vector<std::uint8_t> bin;
    CatalogueStats st;
    if (!packCatalogue(items, bin, &st, &err)) { report.error = "packing catalogue.bin: " + err; return false; }
    // The geometry view: the merged database the engine will have for THIS view - the basemod
    // layer, then a custom game's archives on top of it, which is the order the engine loads
    // them in. A failed layer is a warning and the view falls back to what was readable.
    ModView mods;
    if (!loadView(gameDir, v, mods, &err)) {
        report.warnings.push_back("the " + v.name() + " view's archives could not be read (" +
                                  err + "); the geometry is what was readable");
    }
    PagesOutput po;
    if (!generatePages(g, items, po, &err, mods.empty() ? nullptr : &mods)) {
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
};

bool runCatching(const GenArgs& a, GenReport& report) {
    try {
        return ensureOutputs(*a.gameDir, *a.modDir, *a.pagesArz, *a.plateDir, *a.lang,
                             *a.plateGround, report, a.onStart, a.ctx, a.view);
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
                          const ViewSpec* view) {
    GenArgs a = {&gameDir, &modDir, &pagesArz, &plateDir, &lang, &plateGround, onStart, ctx, view};
    return runGuarded(a, report);
}

} // namespace gen
