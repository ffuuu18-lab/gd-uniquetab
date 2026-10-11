#include "ut_log.h"

#include <windows.h>

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>

namespace ut {

volatile long g_logLevel = UT_LOG_INFO;

namespace {

HANDLE g_file = INVALID_HANDLE_VALUE;
CRITICAL_SECTION g_lock;
bool g_lockReady = false;

// The render thread must not do a WriteFile+FlushFileBuffers per line.
// A fixed buffer (no allocation, so no bad_alloc inside a detour) filled under the lock and
// drained by the worker thread once a second. Overflow flushes inline rather than dropping.
const size_t kBufCap = 64 * 1024;
char g_buf[kBufCap];
size_t g_used = 0;
volatile LONG g_flushEachLine = 0;

// THE FLUSH RULE. A worker loop that flushes once a second is not enough on its own: it is only
// reached after start-up has finished, so a mod that turns itself off during start-up and a game
// killed a minute later leave a 0-byte log - every line that said WHY the mod was off having
// lived in this buffer alone. The rule therefore depends on no other thread:
//   * an ERROR or a WARN line is flushed INLINE, before the call returns - the line that explains
//     a crash is on disk before the thing it explains can happen;
//   * this file's own flusher thread drains the buffer every kFlushEveryMs (1 s) whether or not
//     anything else in the mod is still running, so a kill or a crash loses at most one second of
//     debug/trace and never the banner or a warning;
//   * the banner's last line (READY, dllmain.cpp) calls logFlush() outright;
//   * log_flush_each_line=1 keeps meaning literally every line.
// The flusher is created by logInit and is never joined: logShutdown signals it and closes the
// file UNDER THE LOCK, and every write the thread can still be inside is under that same lock, so
// there is nothing to wait for - and waiting for a thread from DllMain's detach is the loader
// deadlock this mod must never risk.
const DWORD kFlushEveryMs = 1000;
HANDLE g_flushThread = nullptr;
HANDLE g_flushStop = nullptr;

// ROTATION. One session writes at most 4 MB; past that the file says so once and goes quiet, so a
// game that faults in a loop cannot fill the disk and the file a player attaches to a report stays
// sendable. Three archived sessions are kept beside it.
const unsigned long long kSessionCap = 4ull * 1024 * 1024;
const int kArchivesKept = 3;
unsigned long long g_written = 0;
bool g_capped = false;

const char kLevelTag[5] = {'E', 'W', 'I', 'D', 'T'};

// THE TEE (ut_log.h): the active view's own file. Its own buffer, session cap and handle, all
// under g_lock and flushed whenever the main buffer is.
HANDLE g_tee = INVALID_HANDLE_VALUE;
char g_teeBuf[kBufCap];
size_t g_teeUsed = 0;
unsigned long long g_teeWritten = 0;
bool g_teeCapped = false;
wchar_t g_teePath[MAX_PATH] = L"";
char g_teeView[64] = "";
// The tee files this process has started: a second entry into a view appends to its file.
const int kTeeSeenCap = 16;
wchar_t g_teeSeen[kTeeSeenCap][MAX_PATH];
int g_teeSeenN = 0;

// Create every missing directory in the path of a file.
void ensureDirs(const wchar_t* filePath) {
    wchar_t dir[MAX_PATH];
    lstrcpynW(dir, filePath, MAX_PATH);
    wchar_t* lastSlash = nullptr;
    for (wchar_t* p = dir; *p; ++p) {
        if (*p == L'\\' || *p == L'/') lastSlash = p;
    }
    if (!lastSlash) return;
    *lastSlash = 0;
    // Walk forward creating each level; the drive root is skipped by CreateDirectory.
    for (wchar_t* p = dir; *p; ++p) {
        if ((*p == L'\\' || *p == L'/') && p != dir) {
            wchar_t saved = *p;
            *p = 0;
            CreateDirectoryW(dir, nullptr);
            *p = saved;
        }
    }
    CreateDirectoryW(dir, nullptr);
}

// Caller holds g_lock.
void flushTeeLocked(bool hard) {
    if (g_tee == INVALID_HANDLE_VALUE) {
        g_teeUsed = 0;
        return;
    }
    if (g_teeUsed) {
        DWORD written = 0;
        WriteFile(g_tee, g_teeBuf, (DWORD)g_teeUsed, &written, nullptr);
        g_teeUsed = 0;
    }
    if (hard) FlushFileBuffers(g_tee);
}

// Caller holds g_lock. The tee goes first, so a line is never on the main file's disk without it.
void flushLocked(bool hard) {
    flushTeeLocked(hard);
    if (g_file == INVALID_HANDLE_VALUE) {
        g_used = 0;
        return;
    }
    if (g_used) {
        DWORD written = 0;
        WriteFile(g_file, g_buf, (DWORD)g_used, &written, nullptr);
        g_used = 0;
    }
    if (hard) FlushFileBuffers(g_file);
}

// The flusher thread. It wakes on its own stop event or every kFlushEveryMs, and writes only when
// there is something to write - an idle game does no I/O at all. g_file is read under the lock, so
// a shutdown that closes it mid-tick is seen, not raced.
DWORD WINAPI flushMain(LPVOID) {
    for (;;) {
        const DWORD w = WaitForSingleObject(g_flushStop, kFlushEveryMs);
        if (w == WAIT_OBJECT_0) break;
        EnterCriticalSection(&g_lock);
        if (g_used || g_teeUsed) flushLocked(true);
        LeaveCriticalSection(&g_lock);
    }
    return 0;
}

// Caller holds g_lock. Appends bytes that are already formatted, ignoring the session cap - only
// the cap line itself uses this.
void appendLocked(const char* text, size_t len) {
    if (g_used + len > kBufCap) flushLocked(false);
    if (len <= kBufCap) {
        memcpy(g_buf + g_used, text, len);
        g_used += len;
        g_written += len;
    }
}

// Caller holds g_lock. One formatted line into the tee, under the tee's own session cap.
void teeAppendLocked(const char* text, size_t len) {
    if (g_tee == INVALID_HANDLE_VALUE || g_teeCapped) return;
    if (g_teeUsed + len > kBufCap) flushTeeLocked(false);
    if (len > kBufCap) return;
    memcpy(g_teeBuf + g_teeUsed, text, len);
    g_teeUsed += len;
    g_teeWritten += len;
}

// "[hh:mm:ss.mmm tNNNNN L] " + text + CRLF into `out`; the length written.
size_t stampLine(char* out, size_t cap, int level, const char* text) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    const int n = _snprintf_s(out, cap, _TRUNCATE, "[%02u:%02u:%02u.%03u t%05lu %c] %s\r\n",
                              st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                              GetCurrentThreadId(), kLevelTag[level], text);
    return n < 0 ? 0 : (size_t)n;
}

// "<dir>\uniquetab" + "-20260911-193612.log": the archive names logInit makes. Anything that is
// not exactly that shape is somebody else's file and is never deleted.
bool isArchiveName(const wchar_t* stem, const wchar_t* name) {
    const int stemLen = lstrlenW(stem);
    if (_wcsnicmp(name, stem, (size_t)stemLen) != 0) return false;
    const wchar_t* tail = name + stemLen;              // "-20260911-193612.log"
    if (lstrlenW(tail) != 20) return false;
    if (tail[0] != L'-' || tail[9] != L'-') return false;
    for (int i = 1; i <= 8; ++i) {
        if (tail[i] < L'0' || tail[i] > L'9') return false;
    }
    for (int i = 10; i <= 15; ++i) {
        if (tail[i] < L'0' || tail[i] > L'9') return false;
    }
    return _wcsicmp(tail + 16, L".log") == 0;
}

// Keep the kArchivesKept newest archives of this log and delete the rest. The names sort
// chronologically (year first), so "newest" is a plain string comparison - no file times are read,
// which also means a copied folder rotates the same way.
void pruneArchives(const wchar_t* path) {
    wchar_t dir[MAX_PATH];
    lstrcpynW(dir, path, MAX_PATH);
    wchar_t* lastSlash = nullptr;
    for (wchar_t* p = dir; *p; ++p) {
        if (*p == L'\\' || *p == L'/') lastSlash = p;
    }
    if (!lastSlash) return;
    *lastSlash = 0;
    const wchar_t* fileName = lastSlash + 1;

    wchar_t stem[MAX_PATH];
    lstrcpynW(stem, fileName, MAX_PATH);
    int n = lstrlenW(stem);
    if (n > 4 && stem[n - 4] == L'.') stem[n - 4] = 0;  // "uniquetab"

    wchar_t pattern[MAX_PATH];
    _snwprintf_s(pattern, MAX_PATH, _TRUNCATE, L"%s\\%s-*.log", dir, stem);

    // Two passes over the directory: the first keeps the kArchivesKept newest names (ascending, so
    // keep[0] is the cut-off), the second deletes every archive below it. A fixed array, no
    // allocation, and a session makes at most one archive.
    wchar_t keep[kArchivesKept][MAX_PATH];
    int kept = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!isArchiveName(stem, fd.cFileName)) continue;
        if (kept < kArchivesKept) {
            int at = kept;
            while (at > 0 && _wcsicmp(keep[at - 1], fd.cFileName) > 0) {
                lstrcpynW(keep[at], keep[at - 1], MAX_PATH);
                --at;
            }
            lstrcpynW(keep[at], fd.cFileName, MAX_PATH);
            ++kept;
        } else if (_wcsicmp(fd.cFileName, keep[0]) > 0) {
            int at = 0;
            while (at + 1 < kArchivesKept && _wcsicmp(keep[at + 1], fd.cFileName) < 0) {
                lstrcpynW(keep[at], keep[at + 1], MAX_PATH);
                ++at;
            }
            lstrcpynW(keep[at], fd.cFileName, MAX_PATH);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (kept < kArchivesKept) return;  // nothing to drop yet

    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!isArchiveName(stem, fd.cFileName)) continue;
        if (_wcsicmp(fd.cFileName, keep[0]) >= 0) continue;  // one of the three kept
        wchar_t victim[MAX_PATH];
        _snwprintf_s(victim, MAX_PATH, _TRUNCATE, L"%s\\%s", dir, fd.cFileName);
        DeleteFileW(victim);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// Opens a session file the way the main log is opened: the previous session's file archived
// beside it (`archived` gets its name, MAX_PATH), the archives pruned, the file opened for
// appending. `how` says which; `carried` is what the file already holds when it is appended to.
HANDLE openRotated(const wchar_t* path, wchar_t* archived, const wchar_t** how,
                   unsigned long long* carried) {
    ensureDirs(path);

    // Never truncate. A previous session's log is ARCHIVED next to it as
    // uniquetab-<its last write time>.log; if that rename fails (another live process still
    // appending to it, or the archive name taken) we APPEND to it instead. FILE_SHARE_DELETE lets a
    // later process archive OUR file while we still hold it: our appends then follow the renamed
    // file and its fresh file starts empty - two processes never share or destroy a log.
    // (Opening with CREATE_ALWAYS instead would erase the previous session at the very moment a
    // freeze or a crash makes it the only evidence there is.)
    archived[0] = 0;
    *how = L"fresh file";
    *carried = 0;
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &fad) &&
        (fad.nFileSizeLow != 0 || fad.nFileSizeHigh != 0)) {  // an empty file (the test harness's placeholder) is just reused
        FILETIME lt;
        SYSTEMTIME st;
        if (FileTimeToLocalFileTime(&fad.ftLastWriteTime, &lt) && FileTimeToSystemTime(&lt, &st)) {
            wchar_t base[MAX_PATH];
            lstrcpynW(base, path, MAX_PATH);
            size_t n = lstrlenW(base);
            if (n > 4 && base[n - 4] == L'.') base[n - 4] = 0;  // strip ".log"
            _snwprintf_s(archived, MAX_PATH, _TRUNCATE, L"%s-%04u%02u%02u-%02u%02u%02u.log", base,
                         st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
            if (MoveFileExW(path, archived, 0)) {
                *how = L"previous log archived";
            } else {
                *how = L"previous log kept and APPENDED to (archive rename failed)";
                archived[0] = 0;
                // The cap counts what the file already holds: appending to a 4 MB file must not
                // start another 4 MB.
                *carried = ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
            }
        }
    }
    // The current log plus the three newest archives; older sessions go now, while the folder is
    // quiet and before this session writes its first line.
    pruneArchives(path);

    return CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

}  // namespace

void logSetLevel(const char* name) {
    long want = UT_LOG_INFO;
    if (name) {
        if (_stricmp(name, "error") == 0) want = UT_LOG_ERROR;
        else if (_stricmp(name, "warn") == 0) want = UT_LOG_WARN;
        else if (_stricmp(name, "info") == 0) want = UT_LOG_INFO;
        else if (_stricmp(name, "debug") == 0) want = UT_LOG_DEBUG;
        else if (_stricmp(name, "trace") == 0) want = UT_LOG_TRACE;
    }
    if (g_logLevel == want) return;
    InterlockedExchange((volatile LONG*)&g_logLevel, want);
}

bool logInit(const wchar_t* path) {
    if (!g_lockReady) {
        InitializeCriticalSection(&g_lock);
        g_lockReady = true;
    }
    if (g_file != INVALID_HANDLE_VALUE) return true;

    wchar_t archived[MAX_PATH] = L"";
    const wchar_t* how = L"fresh file";
    unsigned long long carried = 0;
    g_file = openRotated(path, archived, &how, &carried);
    if (g_file == INVALID_HANDLE_VALUE) return false;
    g_written = carried;
    g_capped = false;
    // The once-a-second drain, started before the first line is written so even a start-up that
    // never reaches the worker's own loop still leaves its reasons on disk.
    // No stop event, no thread: WaitForSingleObject(nullptr, ...) fails instantly and would spin
    // a core for the whole session. Without the thread the log still works - it just goes back to
    // being drained by the worker and by logFlush.
    if (!g_flushThread) {
        if (!g_flushStop) g_flushStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (g_flushStop) g_flushThread = CreateThread(nullptr, 0, flushMain, nullptr, 0, nullptr);
    }
    logAt(UT_LOG_INFO, "==== log opened by pid %lu (%ls%ls%ls), %d MB per session, %d archived "
                       "session(s) kept ====",
          GetCurrentProcessId(), how, archived[0] ? L": " : L"", archived[0] ? archived : L"",
          (int)(kSessionCap / (1024 * 1024)), kArchivesKept);
    return true;
}

void logAtV(int level, const char* fmt, va_list ap) {
    if (g_file == INVALID_HANDLE_VALUE || !g_lockReady) return;
    if (level < UT_LOG_ERROR || level > UT_LOG_TRACE) level = UT_LOG_INFO;

    char line[2048];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int n = _snprintf_s(line, sizeof(line), _TRUNCATE, "[%02u:%02u:%02u.%03u t%05lu %c] ",
                        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                        GetCurrentThreadId(), kLevelTag[level]);
    if (n < 0) n = 0;

    int m = _vsnprintf_s(line + n, sizeof(line) - (size_t)n, _TRUNCATE, fmt, ap);
    if (m < 0) m = (int)(sizeof(line) - (size_t)n - 1);

    size_t len = (size_t)n + (size_t)m;
    if (len > sizeof(line) - 3) len = sizeof(line) - 3;
    line[len++] = '\r';
    line[len++] = '\n';

    EnterCriticalSection(&g_lock);
    // The active view's own file gets the same line first (ut_log.h), under its own cap.
    if (g_tee != INVALID_HANDLE_VALUE && !g_teeCapped) {
        if (g_teeWritten + len > kSessionCap) {
            char note[200];
            const size_t k = stampLine(note, sizeof(note), UT_LOG_WARN,
                                       "log capped at 4 MB - this view's file writes nothing more "
                                       "this session; the main log goes on");
            teeAppendLocked(note, k);
            g_teeCapped = true;
            flushTeeLocked(true);
        } else {
            teeAppendLocked(line, len);
            // the main block below flushes both; only a capped main log leaves it to the tee
            if (g_capped &&
                (level <= UT_LOG_WARN || InterlockedCompareExchange(&g_flushEachLine, 0, 0))) {
                flushTeeLocked(true);
            }
        }
    }
    if (!g_capped) {
        if (g_written + len > kSessionCap) {
            g_capped = true;
            char note[160];
            const int k = _snprintf_s(note, sizeof(note), _TRUNCATE,
                                      "[%02u:%02u:%02u.%03u t%05lu W] log capped at %d MB - this "
                                      "session writes nothing more; restart the game for a fresh "
                                      "file\r\n",
                                      st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                                      GetCurrentThreadId(), (int)(kSessionCap / (1024 * 1024)));
            if (k > 0) appendLocked(note, (size_t)k);
            flushLocked(true);
        } else {
            appendLocked(line, len);
            // A WARN or an ERROR never waits for the next tick. Those are the lines a
            // reader needs after a kill, and they are rare enough that one WriteFile each costs
            // nothing on any frame the game actually draws.
            if (level <= UT_LOG_WARN || InterlockedCompareExchange(&g_flushEachLine, 0, 0)) {
                flushLocked(true);
            }
        }
    }
    LeaveCriticalSection(&g_lock);
}

void logFlush() {
    if (!g_lockReady) return;
    EnterCriticalSection(&g_lock);
    flushLocked(true);
    LeaveCriticalSection(&g_lock);
}

void logSetFlushEachLine(int on) {
    InterlockedExchange(&g_flushEachLine, on ? 1 : 0);
}

void logShutdown() {
    // Tell the flusher to stop, but never wait for it: this runs from DLL_PROCESS_DETACH, where
    // waiting for a thread to exit waits for the loader lock this call already holds.
    if (g_flushStop) SetEvent(g_flushStop);
    bool held = false;
    if (g_lockReady) {
        // AND NEVER BLOCK ON THE LOCK EITHER. Process exit terminates every other thread BEFORE
        // this runs, so the flusher can have been killed inside its once-a-second WriteFile with
        // the lock still taken - and a plain EnterCriticalSection would then hang the game on the
        // way out forever. Two hundred milliseconds of trying, and then the file is simply closed:
        // losing the last second of a CLEAN exit is nothing, a game that will not close is
        // everything. (Nothing else is running by then, so the close races no one.)
        for (int i = 0; i < 20 && !held; ++i) {
            held = TryEnterCriticalSection(&g_lock) != 0;
            if (!held) Sleep(10);
        }
        if (held) flushLocked(true);
    }
    if (g_tee != INVALID_HANDLE_VALUE) {
        CloseHandle(g_tee);
        g_tee = INVALID_HANDLE_VALUE;
    }
    if (g_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;  // the flusher sees this under the lock and does nothing
    }
    if (held) LeaveCriticalSection(&g_lock);
}

namespace {

// Caller holds g_lock. The tee's own last line, flushed and closed.
void teeCloseLocked() {
    if (g_tee == INVALID_HANDLE_VALUE) return;
    char text[160];
    _snprintf_s(text, sizeof(text), _TRUNCATE, "==== the %s view is left - the main log goes on ====",
                g_teeView);
    char line[256];
    const size_t k = stampLine(line, sizeof(line), UT_LOG_INFO, text);
    g_teeCapped = false;   // the closing line is written even past the cap
    teeAppendLocked(line, k);
    flushTeeLocked(true);
    CloseHandle(g_tee);
    g_tee = INVALID_HANDLE_VALUE;
    g_teePath[0] = 0;
    g_teeView[0] = 0;
    g_teeUsed = 0;
}

}  // namespace

bool logTeeOpen(const wchar_t* path, const char* view) {
    if (!g_lockReady || !path || !path[0]) return false;
    bool seen = false;
    EnterCriticalSection(&g_lock);
    const bool same = g_tee != INVALID_HANDLE_VALUE && !_wcsicmp(g_teePath, path);
    for (int i = 0; i < g_teeSeenN && !seen; ++i) seen = !_wcsicmp(g_teeSeen[i], path);
    LeaveCriticalSection(&g_lock);
    if (same) return true;

    // The file work outside the lock: no other thread sees this handle before it is installed.
    wchar_t archived[MAX_PATH] = L"";
    const wchar_t* how = L"fresh file";
    unsigned long long carried = 0;
    HANDLE h = INVALID_HANDLE_VALUE;
    if (seen) {
        ensureDirs(path);
        h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        LARGE_INTEGER sz;
        if (h != INVALID_HANDLE_VALUE && GetFileSizeEx(h, &sz) && sz.QuadPart > 0)
            carried = (unsigned long long)sz.QuadPart;
        how = L"appended to: the view was entered before in this session";
    } else {
        h = openRotated(path, archived, &how, &carried);
    }

    EnterCriticalSection(&g_lock);
    teeCloseLocked();   // the previous view's file, if one was open
    if (h == INVALID_HANDLE_VALUE) {
        LeaveCriticalSection(&g_lock);
        return false;
    }
    g_tee = h;
    lstrcpynW(g_teePath, path, MAX_PATH);
    _snprintf_s(g_teeView, sizeof(g_teeView), _TRUNCATE, "%s", view ? view : "");
    g_teeUsed = 0;
    g_teeWritten = carried;
    g_teeCapped = false;
    if (!seen && g_teeSeenN < kTeeSeenCap) lstrcpynW(g_teeSeen[g_teeSeenN++], path, MAX_PATH);
    char text[MAX_PATH * 2 + 240];
    _snprintf_s(text, sizeof(text), _TRUNCATE,
                "==== the %s view's log, opened by pid %lu (%ls%ls%ls), %d MB per session, %d "
                "archived session(s) kept; every line is in the main log as well ====",
                g_teeView, GetCurrentProcessId(), how, archived[0] ? L": " : L"",
                archived[0] ? archived : L"", (int)(kSessionCap / (1024 * 1024)), kArchivesKept);
    char line[sizeof(text) + 64];
    const size_t k = stampLine(line, sizeof(line), UT_LOG_INFO, text);
    teeAppendLocked(line, k);
    flushTeeLocked(true);
    LeaveCriticalSection(&g_lock);
    return true;
}

void logTeeClose() {
    if (!g_lockReady) return;
    EnterCriticalSection(&g_lock);
    teeCloseLocked();
    LeaveCriticalSection(&g_lock);
}

void logTeePath(wchar_t* out, size_t cap) {
    if (!out || !cap) return;
    out[0] = 0;
    if (!g_lockReady) return;
    EnterCriticalSection(&g_lock);
    lstrcpynW(out, g_teePath, (int)cap);
    LeaveCriticalSection(&g_lock);
}

}  // namespace ut
