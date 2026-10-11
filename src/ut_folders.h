// ut_folders.h - where a custom game's files live, and the one-way move from the old layout.
//
// THE LAYOUT. The main campaign's files - the vanilla and the /basemods view - stay in the mod
// folder's root. A custom game has ONE folder,
// <mod folder>\mods\<leaf>\, named for the game's own <game>\mods\<leaf>\ folder:
//
//   mods\<leaf>\             the game's journal pair (uniq-items*.jsonl), its exports, prune copies
//                            and rescue-report.txt, its own uniquetab.ini and
//                            uniquetab-buttons.ini - shared by both variants -, and the generated
//                            set + uniquetab.state + uniquetab.log of the view custom-<leaf>
//                            (the game started WITHOUT /basemods)
//   mods\<leaf>\basemods\    the generated set + uniquetab.state + uniquetab.log of the view
//                            basemods+custom-<leaf> (the same game with /basemods) - nothing of
//                            the player's: delete it and the next entry rebuilds it
//   <journal_dir>\mods\<leaf>\   the journal set, when journal_dir is set
//
// The VIEW NAMES are unchanged (vanilla | basemods | custom-<leaf> | basemods+custom-<leaf>):
// the log lines, the [view:<key>] blocks and every key grammar still use them. Only the name ->
// folder mapping lives here, and every code point that builds a custom game's path calls it.
//
// THE MIGRATION. Older builds kept the same files
// under views\custom-<leaf>\ and views\basemods+custom-<leaf>\. utFoldersMigrate renames those
// folders once, on the worker,
// before the first view or journal folder is touched - a directory rename on the same volume,
// so nothing is copied, merged or overwritten. A target that already exists is NEVER merged into:
// the old folder stays where it is, with a WARN naming both, and the new one is used. views\ is
// removed only when it is empty. It is one-way: an older build does not look in mods\.
#pragma once

#include <windows.h>

#include <string.h>
#include <wchar.h>

#include <algorithm>
#include <string>
#include <vector>

#include "ut_log.h"

namespace ut {

// The custom games' folder under the mod folder and under journal_dir.
#define UT_CUSTOM_GAMES_DIR "mods"
#define UT_BASEMODS_SUBDIR "basemods"

// A leaf the mod would have named a folder: 1..63 of [A-Za-z0-9._+-], and not "." or "..".
// (ViewSpec::tag() makes [a-z0-9._-]; the settings' key grammar also allows '+'.)
inline bool utFolderLeafOk(const std::string& leaf) {
    if (leaf.empty() || leaf.size() >= 64 || leaf == "." || leaf == "..") return false;
    for (char c : leaf) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '.' || c == '_' || c == '+' || c == '-';
        if (!ok) return false;
    }
    return true;
}

// The custom game's leaf of a view name or a game key: "x" for "custom-x" and for
// "basemods+custom-x"; "" for vanilla, basemods, or anything that is not a legal leaf.
inline std::string utCustomLeaf(const char* viewOrKey) {
    if (!viewOrKey) return std::string();
    const char* v = viewOrKey;
    if (!_strnicmp(v, "basemods+", 9)) v += 9;
    if (_strnicmp(v, "custom-", 7) != 0) return std::string();
    const std::string leaf(v + 7);
    return utFolderLeafOk(leaf) ? leaf : std::string();
}

// True for basemods+custom-<leaf> (the /basemods variant of a custom game).
inline bool utViewIsBasemodsVariant(const char* viewName) {
    return viewName && !_strnicmp(viewName, "basemods+custom-", 16);
}

// "mods\<leaf>" for a bare leaf (the settings' key), "" for an illegal one.
inline std::string utCustomGameRelLeaf(const std::string& leaf) {
    return utFolderLeafOk(leaf) ? std::string(UT_CUSTOM_GAMES_DIR "\\") + leaf : std::string();
}

// "mods\<leaf>" - the custom game's own folder, relative to the mod folder (or journal_dir) -
// for custom-<leaf> and basemods+custom-<leaf> alike; "" for a start-up view.
inline std::string utCustomGameRel(const char* viewOrKey) {
    return utCustomGameRelLeaf(utCustomLeaf(viewOrKey));
}

// The VIEW's folder, relative: "mods\<leaf>" for custom-<leaf>, "mods\<leaf>\basemods" for
// basemods+custom-<leaf>, "" for a start-up view (its files are in the root).
inline std::string utCustomViewRel(const char* viewName) {
    const std::string game = utCustomGameRel(viewName);
    if (game.empty()) return game;
    return utViewIsBasemodsVariant(viewName) ? game + "\\" UT_BASEMODS_SUBDIR : game;
}

// Creates `base`\`rel` level by level (`base` itself must exist). True when the folder is there.
inline bool utMakeRelDirs(const std::string& base, const std::string& rel) {
    std::string path = base;
    size_t from = 0;
    while (from <= rel.size()) {
        size_t to = rel.find('\\', from);
        if (to == std::string::npos) to = rel.size();
        if (to > from) {
            path += "\\" + rel.substr(from, to - from);
            CreateDirectoryA(path.c_str(), nullptr);
        }
        from = to + 1;
    }
    const DWORD a = GetFileAttributesA(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// journal_dir as journalInit reads it: trailing slashes, spaces and quotes cut, a leading quote
// cut; "" unless it names an existing folder.
inline std::string utJournalDirResolved(const char* raw) {
    std::string jd(raw ? raw : "");
    while (!jd.empty() && (jd.back() == '\\' || jd.back() == '/' || jd.back() == ' ' || jd.back() == '"'))
        jd.pop_back();
    if (!jd.empty() && jd[0] == '"') jd.erase(0, 1);
    if (jd.empty()) return jd;
    const DWORD a = GetFileAttributesA(jd.c_str());
    return (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) ? jd : std::string();
}

// ---- the one-way move ------------------------------------------------------------------------

struct UtFolderMigration {
    bool hadViews = false;               // <base>\views was there
    bool viewsRemoved = false;           // ... and is gone now (it was empty after the moves)
    std::vector<std::string> moved;      // "custom-x -> mods\x", "basemods+custom-x -> mods\x\basemods"
    std::vector<std::string> refused;    // one line per folder left in place, naming both paths
    std::vector<std::string> left;       // what views\ still holds when it could not be removed
};

namespace folders_detail {

inline bool dirThere(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}
inline bool anyThere(const std::wstring& p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}
inline std::string narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
inline std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
// The entries of `dir` (no "." / ".."), sorted, folders only when `dirsOnly`.
inline std::vector<std::wstring> entries(const std::wstring& dir, bool dirsOnly) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    const HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        const std::wstring n(fd.cFileName);
        if (n == L"." || n == L"..") continue;
        if (dirsOnly && !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        out.push_back(n);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace folders_detail

// Moves <base>\views\custom-<leaf>\ to <base>\mods\<leaf>\ and then
// <base>\views\basemods+custom-<leaf>\ to <base>\mods\<leaf>\basemods\ (mods\<leaf>\ is created
// for it when absent), and removes views\ when that left it empty. MoveFileExW without
// MOVEFILE_REPLACE_EXISTING: a target that exists refuses, so nothing is merged or overwritten.
// The plain folders go first so the /basemods pass can never take mods\<leaf>\ from them, and a
// /basemods folder whose plain folder is still in views\ (not moved) waits for it. A
// folder whose name is not one the mod made stays where it is (reported in `left`). Touches
// nothing when <base>\views is absent. Pure file system; the caller logs.
inline UtFolderMigration utFoldersMigrateIn(const std::wstring& base) {
    namespace fd = folders_detail;
    UtFolderMigration r;
    const std::wstring views = base + L"\\views";
    if (base.empty() || !fd::dirThere(views)) return r;
    r.hadViews = true;
    const std::wstring mods = base + L"\\" UT_CUSTOM_GAMES_DIR;
    static const wchar_t* const kPrefix[2] = {L"custom-", L"basemods+custom-"};
    for (int pass = 0; pass < 2; ++pass) {
        const size_t plen = wcslen(kPrefix[pass]);
        for (const std::wstring& name : fd::entries(views, true)) {
            if (name.size() <= plen || _wcsnicmp(name.c_str(), kPrefix[pass], plen) != 0) continue;
            const std::wstring leafW = name.substr(plen);
            const std::string leaf = fd::narrow(leafW);
            if (!utFolderLeafOk(leaf) || fd::widen(leaf) != leafW) continue;
            const std::wstring src = views + L"\\" + name;
            const std::wstring game = mods + L"\\" + leafW;
            const std::wstring dst = pass ? game + L"\\" UT_BASEMODS_SUBDIR : game;
            const std::string what = fd::narrow(name);
            const std::string to = std::string(UT_CUSTOM_GAMES_DIR "\\") + leaf +
                                   (pass ? "\\" UT_BASEMODS_SUBDIR : "");
            if (pass && fd::dirThere(views + L"\\" + kPrefix[0] + leafW)) {
                // Its plain folder did not move (taken, or a file in it was open). Moving this one
                // would create mods\<leaf>\ and so block that folder's move for good; both wait.
                r.refused.push_back("views\\" + what + " was NOT moved: it waits for views\\custom-" +
                                    leaf + ", which is still there");
                continue;
            }
            CreateDirectoryW(mods.c_str(), nullptr);
            if (pass) CreateDirectoryW(game.c_str(), nullptr);
            if (fd::anyThere(dst)) {
                r.refused.push_back("views\\" + what + " was NOT moved: " + to +
                                    " already exists (\"" + fd::narrow(src) + "\" and \"" + fd::narrow(dst) +
                                    "\" are both kept; the mod uses " + to + ")");
                continue;
            }
            if (MoveFileExW(src.c_str(), dst.c_str(), 0)) {
                r.moved.push_back(what + " -> " + to);
            } else {
                r.refused.push_back("views\\" + what + " could not be moved to " + to + " (error " +
                                    std::to_string((unsigned long)GetLastError()) +
                                    ") - it stays where it is and the mod uses " + to +
                                    ". A program holding a file in it open (GD Stash?) stops the "
                                    "move: close it and restart the game BEFORE you enter that "
                                    "custom game, and the move is tried again");
            }
        }
    }
    if (RemoveDirectoryW(views.c_str())) {
        r.viewsRemoved = true;
    } else {
        for (const std::wstring& n : fd::entries(views, false)) r.left.push_back(fd::narrow(n));
    }
    return r;
}

// The start-up step: the mod folder, then journal_dir when it is set and is another folder.
// One INFO line per folder something moved in, one WARN per folder left in place, one WARN when
// views\ stays because it still holds something. Silent when there is no views\ at all - every
// start after the first. WORKER THREAD, before the first view or journal folder is touched.
inline void utFoldersMigrate(const wchar_t* modDir, const char* journalDirRaw) {
    namespace fd = folders_detail;
    std::vector<std::wstring> bases;
    if (modDir && modDir[0]) bases.push_back(modDir);
    const std::string jd = utJournalDirResolved(journalDirRaw);
    if (!jd.empty()) {
        const std::wstring jw = fd::widen(jd);
        if (bases.empty() || _wcsicmp(jw.c_str(), bases[0].c_str()) != 0) bases.push_back(jw);
    }
    for (size_t b = 0; b < bases.size(); ++b) {
        const UtFolderMigration r = utFoldersMigrateIn(bases[b]);
        if (!r.hadViews) continue;
        const std::string where = b == 0 ? std::string() : " (journal_dir \"" + fd::narrow(bases[b]) + "\")";
        if (!r.moved.empty()) {
            std::string list;
            for (const std::string& m : r.moved) list += (list.empty() ? "" : ", ") + m;
            if (list.size() > 700) list = list.substr(0, 700) + " ...";
            logI("files: the custom games' folders moved from views\\ to " UT_CUSTOM_GAMES_DIR
                 "\\%s - %zu folder(s): %s",
                 where.c_str(), r.moved.size(), list.c_str());
        }
        for (const std::string& w : r.refused) logW("files: %s%s", w.c_str(), where.c_str());
        if (r.viewsRemoved) {
            if (r.moved.empty()) logD("files: an empty views\\ folder was removed%s", where.c_str());
        } else {
            std::string list;
            for (const std::string& n : r.left) list += (list.empty() ? "" : ", ") + n;
            if (list.size() > 500) list = list.substr(0, 500) + " ...";
            logW("files: views\\ in \"%s\" is kept - it still holds %zu item(s): %s. The mod no longer "
                 "reads it; move or delete them by hand",
                 fd::narrow(bases[b]).c_str(), r.left.size(), list.empty() ? "?" : list.c_str());
        }
    }
}

}  // namespace ut
