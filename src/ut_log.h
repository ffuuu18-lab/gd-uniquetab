// ut_log.h - line logger for the unique-tab .asi.
//
// Writes uniquetab.log into the mod folder (ut_paths.h), so the caller resolves the path and this
// file only writes lines. Opened with FILE_SHARE_READ|FILE_SHARE_WRITE so a PowerShell tail can
// read the file while the game runs.
//
// LEVELS. Every call names one: error (a feature is gone), warn (the player should act), info
// (what a player reads after a problem: the start-up banner and one line per event that changed
// something), debug (the numbers and the "why"), trace (per-tick, per-box, per-frame chatter).
// `log_level` in the ini sets the threshold; a line above it costs one comparison, because the
// wrappers below are inline and return before the arguments are formatted.
//
// Lines are BUFFERED. A detour on the render thread only formats
// into a fixed 64 KB buffer under a critical section; the worker thread flushes once a second
// (logFlush) and the buffer is flushed inline whenever it would overflow, so nothing is ever
// dropped. `log_flush_each_line=1` in the ini restores the old WriteFile+FlushFileBuffers per
// line for debugging a crash where the last lines matter.
#pragma once

#include <windows.h>

#include <stdarg.h>
#include <stddef.h>

namespace ut {

enum UtLogLevel {
    UT_LOG_ERROR = 0,
    UT_LOG_WARN = 1,
    UT_LOG_INFO = 2,
    UT_LOG_DEBUG = 3,
    UT_LOG_TRACE = 4,
};

// The threshold: a line whose level is above it is not written. Read directly (not through a
// call) so that dropping a trace line inside a detour costs one load and one branch.
extern volatile long g_logLevel;

inline bool logWants(int level) { return level <= g_logLevel; }

// The one real entry point; the wrappers below have already tested the threshold.
void logAtV(int level, const char* fmt, va_list ap);

inline void logAt(int level, const char* fmt, ...) {
    if (level > g_logLevel) return;
    va_list ap;
    va_start(ap, fmt);
    logAtV(level, fmt, ap);
    va_end(ap);
}

// A hook that did not install, a file the mod could not write, a feature that is now off.
inline void logE(const char* fmt, ...) {
    if (UT_LOG_ERROR > g_logLevel) return;
    va_list ap;
    va_start(ap, fmt);
    logAtV(UT_LOG_ERROR, fmt, ap);
    va_end(ap);
}

// Something the player should act on.
inline void logW(const char* fmt, ...) {
    if (UT_LOG_WARN > g_logLevel) return;
    va_list ap;
    va_start(ap, fmt);
    logAtV(UT_LOG_WARN, fmt, ap);
    va_end(ap);
}

// The banner, and one line per event that changed something. <= ~160 characters, one fact.
inline void logI(const char* fmt, ...) {
    if (UT_LOG_INFO > g_logLevel) return;
    va_list ap;
    va_start(ap, fmt);
    logAtV(UT_LOG_INFO, fmt, ap);
    va_end(ap);
}

// The numbers behind the line above it: addresses, counters, the reconcile detail.
inline void logD(const char* fmt, ...) {
    if (UT_LOG_DEBUG > g_logLevel) return;
    va_list ap;
    va_start(ap, fmt);
    logAtV(UT_LOG_DEBUG, fmt, ap);
    va_end(ap);
}

// Per tick, per box, per relayout, per frame, per registration, the heartbeat.
inline void logT(const char* fmt, ...) {
    if (UT_LOG_TRACE > g_logLevel) return;
    va_list ap;
    va_start(ap, fmt);
    logAtV(UT_LOG_TRACE, fmt, ap);
    va_end(ap);
}

// "error" | "warn" | "info" | "debug" | "trace"; anything else means info. Called on every config
// reload, so it must stay cheap and must not log at all when the level did not change.
void logSetLevel(const char* name);

// Creates the directory chain for `path`, archives the previous session's log, deletes all but the
// three newest archives and opens the current one. Safe to call twice.
bool logInit(const wchar_t* path);

// Writes whatever is buffered and flushes the handle. Called once a second by the worker and at
// shutdown. Safe to call from any thread, and a no-op when the buffer is empty.
// It is not the only drain: ut_log.cpp runs its own flusher thread from logInit, and
// a WARN or an ERROR is written before the call returns - a kill loses at most a second of
// debug/trace and never the banner or a warning, whatever the rest of the mod is doing.
void logFlush();

// 1 = the old behaviour (one WriteFile + FlushFileBuffers per line). Set from the ini.
void logSetFlushEachLine(int on);

void logShutdown();

}  // namespace ut
