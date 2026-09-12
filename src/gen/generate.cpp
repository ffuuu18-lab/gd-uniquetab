// generate.cpp - see generate.h.
#include "gen/generate.h"
#include "gen/catalogue_gen.h"
#include "gen/pages_gen.h"
#include "model/catalogue.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <exception>

namespace gen {

namespace {

const char* const kStampName = "catalogue.stamp";
const char* const kOutputs[4] = {"catalogue.bin", "uniq-records.txt", "uniq-pages.txt", "uniq-groups.txt"};

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

// The four temporary names, whether this run or a crashed earlier one left them.
void deleteTemporaries(const std::string& modDir) {
    for (int i = 0; i < 4; ++i) DeleteFileA(tmpName(modDir, i).c_str());
}

// Deletes the temporaries on every exit that is not the rename into place.
struct TmpGuard {
    const std::string& modDir;
    bool keep = false;
    explicit TmpGuard(const std::string& d) : modDir(d) {}
    ~TmpGuard() { if (!keep) deleteTemporaries(modDir); }
};

} // namespace

std::string inputFingerprint(const std::string& gameDir, const std::string& lang) {
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
    std::string fp = "GDUT-STAMP 1\nlang=" + lang + "\n";
    for (const std::string& r : rel) {
        WIN32_FILE_ATTRIBUTE_DATA fa;
        char line[512];
        if (GetFileAttributesExA((gameDir + "\\" + r).c_str(), GetFileExInfoStandard, &fa)) {
            unsigned long long size = (unsigned long long(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
            unsigned long long mt = (unsigned long long(fa.ftLastWriteTime.dwHighDateTime) << 32)
                                  | fa.ftLastWriteTime.dwLowDateTime;
            std::snprintf(line, sizeof line, "%s %llu %llu\n", r.c_str(), size, mt);
        } else {
            std::snprintf(line, sizeof line, "%s missing\n", r.c_str());
        }
        fp += line;
    }
    return fp;
}

bool ensureOutputs(const std::string& gameDir, const std::string& modDir,
                   const std::string& pagesArz, const std::string& lang, GenReport& report,
                   GenStartFn onStart, void* ctx) {
    report = GenReport();
    std::string fp = inputFingerprint(gameDir, lang);
    std::string stampPath = modDir + "\\" + kStampName;
    std::string old;
    for (const char* o : kOutputs) {
        if (!fileExists(modDir + "\\" + o)) { report.reason = std::string(o) + " is missing"; break; }
    }
    if (report.reason.empty()) {
        if (!readText(stampPath, old)) report.reason = "no catalogue.stamp yet (first launch)";
        else if (old != fp) report.reason = "catalogue.stamp does not match the game files (the game was updated)";
    }
    if (report.reason.empty()) {
        report.upToDate = true;
        return true;
    }
    if (onStart) onStart(report, ctx);

    auto t0 = std::chrono::steady_clock::now();
    deleteTemporaries(modDir);
    GameData g;
    std::string err;
    if (!g.load(gameDir, lang, &err)) { report.error = "reading the game archives: " + err; return false; }
    std::vector<CatalogueItem> items;
    if (!collectItems(g, items, report.warnings, &err)) { report.error = "the collection rule: " + err; return false; }
    std::vector<std::uint8_t> bin;
    CatalogueStats st;
    if (!packCatalogue(items, bin, &st, &err)) { report.error = "packing catalogue.bin: " + err; return false; }
    PagesOutput po;
    if (!generatePages(g, items, po, &err)) { report.error = "the page layout: " + err; return false; }
    if (!checkPageCapacity(pagesArz, po, &err)) {
        report.error = "uniq-pages.arz capacity: " + err;
        return false;
    }

    // temporary names first; nothing in the folder changes until all four are good
    TmpGuard guard(modDir);
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
    for (int i = 0; i < 4; ++i) {
        std::string dst = modDir + "\\" + kOutputs[i];
        if (!MoveFileExA(tmpName(modDir, i).c_str(), dst.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            report.error = "renaming " + tmpName(modDir, i) + " into place (error " +
                           std::to_string(GetLastError()) + ")";
            return false;
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
    report.generated = true;
    return true;
}

namespace {

struct GenArgs {
    const std::string* gameDir;
    const std::string* modDir;
    const std::string* pagesArz;
    const std::string* lang;
    GenStartFn onStart;
    void* ctx;
};

bool runCatching(const GenArgs& a, GenReport& report) {
    try {
        return ensureOutputs(*a.gameDir, *a.modDir, *a.pagesArz, *a.lang, report, a.onStart,
                             a.ctx);
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
    deleteTemporaries(*a.modDir);
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
                          const std::string& pagesArz, const std::string& lang, GenReport& report,
                          GenStartFn onStart, void* ctx) {
    GenArgs a = {&gameDir, &modDir, &pagesArz, &lang, onStart, ctx};
    return runGuarded(a, report);
}

} // namespace gen
