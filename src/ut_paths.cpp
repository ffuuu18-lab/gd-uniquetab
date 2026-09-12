#include "ut_paths.h"

#include <stdio.h>
#include <string.h>

namespace ut {
namespace {

const wchar_t kModFolder[] = L"uniquetab";

wchar_t g_dir[MAX_PATH] = {0};      // the resolved mod folder, no trailing backslash
wchar_t g_beside[MAX_PATH] = {0};   // <the .asi's directory>\uniquetab, whether or not it won
char g_why[160] = "not resolved";
char g_warn[400] = {0};

bool dirExists(const wchar_t* dir) {
    const DWORD a = GetFileAttributesW(dir);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// Cuts the last component off `path` in place. False when there is nothing left to cut, so a
// caller walking up two levels can tell a path that was too short from one that worked.
bool stripLeaf(wchar_t* path) {
    wchar_t* slash = nullptr;
    for (wchar_t* p = path; *p; ++p) {
        if (*p == L'\\' || *p == L'/') slash = p;
    }
    if (!slash || slash == path) return false;
    *slash = 0;
    return true;
}

// Creates every missing level of `dir` and then PROVES the mod can write there. A game folder
// nobody relaxed the permissions on gets past CreateDirectory (the parent already exists) and
// fails here, which is exactly the case the Documents fallback exists for. The probe file opens
// with FILE_FLAG_DELETE_ON_CLOSE, so it is gone again before this function returns.
bool dirUsable(const wchar_t* dir) {
    wchar_t buf[MAX_PATH];
    lstrcpynW(buf, dir, MAX_PATH);
    for (wchar_t* p = buf; *p; ++p) {
        if ((*p == L'\\' || *p == L'/') && p != buf) {
            const wchar_t saved = *p;
            *p = 0;
            CreateDirectoryW(buf, nullptr);
            *p = saved;
        }
    }
    CreateDirectoryW(buf, nullptr);
    if (!dirExists(buf)) return false;

    wchar_t probe[MAX_PATH];
    _snwprintf_s(probe, MAX_PATH, _TRUNCATE, L"%s\\uniq-write-probe.tmp", buf);
    const HANDLE h = CreateFileW(probe, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

void take(const wchar_t* dir, const char* why) {
    lstrcpynW(g_dir, dir, MAX_PATH);
    _snprintf_s(g_why, sizeof(g_why), _TRUNCATE, "%s", why);
}

// "<the directory the module sits in>\uniquetab" into `out`. False when the module's own path
// is
// not available, which in practice never happens.
bool besideModule(HMODULE self, wchar_t* out, size_t cap) {
    wchar_t path[MAX_PATH] = {0};
    if (!GetModuleFileNameW(self, path, MAX_PATH)) return false;
    return utModDirBesideW(path, out, cap);
}

void toAnsi(const wchar_t* w, char* out, size_t cap) {
    if (!cap) return;
    out[0] = 0;
    WideCharToMultiByte(CP_ACP, 0, w, -1, out, (int)cap, nullptr, nullptr);
    out[cap - 1] = 0;
}

}  // namespace

bool utModDirBesideW(const wchar_t* modulePath, wchar_t* out, size_t cap) {
    if (!modulePath || !out || !cap) return false;
    wchar_t dir[MAX_PATH] = {0};
    lstrcpynW(dir, modulePath, MAX_PATH);
    if (!stripLeaf(dir)) return false;
    _snwprintf_s(out, cap, _TRUNCATE, L"%s\\%s", dir, kModFolder);
    return true;
}

bool utGameDirFromExeW(const wchar_t* hostExe, char* out, size_t cap) {
    if (!hostExe || !out || !cap) return false;
    wchar_t dir[MAX_PATH] = {0};
    lstrcpynW(dir, hostExe, MAX_PATH);
    if (!stripLeaf(dir)) return false;  // the file name; what is left is the x64 folder
    if (!stripLeaf(dir)) return false;  // the x64 folder; what is left is the game folder
    toAnsi(dir, out, cap);
    return true;
}

const wchar_t* utModDir(HMODULE self) {
    if (g_dir[0]) return g_dir;

    // The offline harnesses' override. Read first so a test can never reach an installation, and
    // resolved with one GetEnvironmentVariableW - no new dependency under the loader lock.
    wchar_t env[MAX_PATH] = {0};
    const DWORD envLen = GetEnvironmentVariableW(L"UNIQUETAB_OUT", env, MAX_PATH);
    if (envLen > 0 && envLen < MAX_PATH && dirUsable(env)) {
        take(env, "%UNIQUETAB_OUT%");
        besideModule(self, g_beside, MAX_PATH);
        return g_dir;
    }

    // THE MOD FOLDER: <the directory of the loaded DLL>\uniquetab.
    const bool haveBeside = besideModule(self, g_beside, MAX_PATH);
    if (haveBeside && dirUsable(g_beside)) {
        take(g_beside, "the uniquetab folder beside the .asi");
        return g_dir;
    }

    // The one fallback. A game installed under Program Files is writable only because Steam's
    // installer relaxes the permissions; where it is not, everything the mod writes goes here
    // instead and the warning below is logged once.
    wchar_t home[MAX_PATH] = {0};
    const DWORD homeLen = GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
    if (homeLen > 0 && homeLen < MAX_PATH) {
        static const wchar_t* const kDocs[2] = {L"Documents", L"OneDrive\\Documents"};
        static const char* const kWhy[2] = {"Documents\\My Games\\Grim Dawn\\uniquetab (fallback)",
                                            "OneDrive\\Documents\\My Games\\Grim Dawn\\uniquetab "
                                            "(fallback)"};
        for (int i = 0; i < 2; ++i) {
            wchar_t cand[MAX_PATH];
            _snwprintf_s(cand, MAX_PATH, _TRUNCATE, L"%s\\%s\\My Games\\Grim Dawn\\%s", home,
                         kDocs[i], kModFolder);
            if (!dirUsable(cand)) continue;
            take(cand, kWhy[i]);
            _snprintf_s(g_warn, sizeof(g_warn), _TRUNCATE,
                        "***** the mod folder beside the .asi (\"%S\") could not be created or "
                        "written to, so everything this mod writes is kept in \"%S\" instead. The "
                        "files it SHIPS are still read from the folder beside the .asi. *****",
                        haveBeside ? g_beside : L"<unknown>", cand);
            return g_dir;
        }
    }

    // Nothing was writable. Keep the mod folder's own name so every path is still well formed and
    // each failed write reports itself, instead of landing somewhere nobody would look for it.
    if (haveBeside) {
        take(g_beside, "the uniquetab folder beside the .asi (NOT WRITABLE)");
    } else {
        take(L".\\uniquetab", "the process's own folder (the .asi's path could not be read)");
    }
    _snprintf_s(g_warn, sizeof(g_warn), _TRUNCATE,
                "***** neither the mod folder beside the .asi nor the Documents fallback could be "
                "written to. The mod will keep running, but nothing it writes - the ini, the log, "
                "the collection - can be saved. Paths are reported against \"%S\". *****",
                g_dir);
    return g_dir;
}

void utModPathW(HMODULE self, const wchar_t* leaf, wchar_t* out, size_t cap) {
    _snwprintf_s(out, cap, _TRUNCATE, L"%s\\%s", utModDir(self), leaf);
}

void utModPathA(HMODULE self, const char* leaf, char* out, size_t cap) {
    char dirA[MAX_PATH] = {0};
    toAnsi(utModDir(self), dirA, sizeof(dirA));
    _snprintf_s(out, cap, _TRUNCATE, "%s\\%s", dirA, leaf);
}

void utModDirA(HMODULE self, char* out, size_t cap) {
    char dirA[MAX_PATH] = {0};
    toAnsi(utModDir(self), dirA, sizeof(dirA));
    _snprintf_s(out, cap, _TRUNCATE, "%s", dirA);
}

bool utGameDirA(char* out, size_t cap) {
    wchar_t exe[MAX_PATH] = {0};
    if (!cap || !GetModuleFileNameW(nullptr, exe, MAX_PATH)) return false;
    return utGameDirFromExeW(exe, out, cap);
}

const char* utModDirWhy() {
    return g_why;
}

const char* utModDirWarning() {
    return g_warn[0] ? g_warn : nullptr;
}

bool utModFile(HMODULE self, const char* leaf, char* out, size_t cap) {
    utModPathA(self, leaf, out, cap);
    DWORD a = GetFileAttributesA(out);
    if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) return true;

    // The mod folder is the Documents fallback (or %UNIQUETAB_OUT%) and the shipped file is still
    // beside the .asi, where deploy put it. Same resolver, second of its two candidates.
    if (!g_beside[0] || lstrcmpiW(g_beside, g_dir) == 0) return false;
    char besideA[MAX_PATH] = {0};
    toAnsi(g_beside, besideA, sizeof(besideA));
    char alt[MAX_PATH];
    _snprintf_s(alt, sizeof(alt), _TRUNCATE, "%s\\%s", besideA, leaf);
    a = GetFileAttributesA(alt);
    if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) return false;
    _snprintf_s(out, cap, _TRUNCATE, "%s", alt);
    return true;
}

}  // namespace ut
