// dllmain.cpp - entry point of the Unique Collection Tab mod (built as uniquetab.asi).
//
// The file is a plain DLL with the .asi extension Ultimate ASI Loader looks for; the loader
// LoadLibrary's it and nothing else in the process knows it exists. It exports nothing.
//
// DllMain stays minimal on purpose: open the log, install the fault watchdog, start one
// worker thread, return. Everything that can block or wait happens on that worker.
//
// THE LOADER DECIDES WHEN THIS RUNS. A winmm.dll loader is mapped through the exe's import
// table at process start; a dinput8.dll loader is mapped when the engine initialises input,
// i.e. after the engine has read its database. Everything below therefore has to work both
// ways: see reagentLateLoadTick (ut_reagent.h) for the overlay, and the from-the-game-thread
// retries in ut_live / ut_plate for the exe signatures.

#include <windows.h>

#include "gd_runtime.h"
#include "hooks.h"
#include "ut_config.h"
#include "ut_generate.h"
#include "ut_log.h"
#include "ut_version.h"
#include "ut_bindings.h"
#include "ut_panel.h"
#include "ut_paths.h"
#include "ut_plate.h"
#include "ut_reagent.h"
#include "ut_rescue.h"
#include "ut_tooltip.h"

namespace {

// Never hard-coded: ut::utModPathW (ut_paths.h) resolves the mod folder - the uniquetab folder
// beside this DLL - and creates it on first use. Filled in DllMain before anything else logs.
wchar_t kLogPath[MAX_PATH] = {0};
wchar_t kIniPath[MAX_PATH] = {0};

HANDLE g_worker = nullptr;
HANDLE g_stopEvent = nullptr;
HMODULE g_selfModule = nullptr;
PVOID g_veh = nullptr;

// ---- the fault watchdog -----------------------------------------------------------------------
// A first-chance access violation with this DLL on the stack is logged AT DEBUG, once per fault,
// with the module+rva, both parameters, the thread and the mod's identity state - and then handed
// straight on with EXCEPTION_CONTINUE_SEARCH. It NEVER handles anything: the engine's own SEH runs
// exactly as it would have, and a fault the mod itself catches is reported as a warning by the
// feature that caught it, not here.
//   * never for the mod's own guarded probes - they fault by design and count their own faults;
//   * rate-capped, because a game that faults in a loop must not fill the disk;
//   * never on the log thread's own faults (that would recurse through the log lock);
//   * C++ exceptions (0xE06D7363), DBG_PRINTEXCEPTION and breakpoints are ignored - the engine
//     throws C++ exceptions as a matter of course.
volatile LONG g_vehLogged = 0;
volatile LONG g_vehInside = 0;
DWORD g_vehThread = 0;

LONG CALLBACK utVectoredHandler(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != (DWORD)EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    // The mod's own DELIBERATE, SEH-handled probes fault by design (the replica slot scan
    // dereferences 8-aligned windows, the reagent map walk follows whatever a node holds), and the
    // probe counts each one. A vectored handler runs before the __except that owns them, so
    // without this test every one of them printed a fault line and buried the real ones. Tested
    // BEFORE the counter is touched, and again inside the fault logger.
    if (ut::reagentProbeDepth() > 0) return EXCEPTION_CONTINUE_SEARCH;
    // Nothing below is on a hot path, but a log level of warn or error means the reader asked for
    // less than this: the fault line and the repeat counter are both debug.
    if (!ut::logWants(ut::UT_LOG_DEBUG)) return EXCEPTION_CONTINUE_SEARCH;
    // The ENGINE raises and handles first-chance access violations of its own as a matter of
    // course (30 x Engine.dll+0x3A740 reading [0+0x2C] in 40 ms while a world loads). They are
    // not the mod's business and would drown the 32-line budget. Log a fault only when this DLL
    // is involved: the faulting address is inside it, or one of its frames is on the faulting
    // thread's stack (a Game.dll memcpy called from Item::CreateItem called from the mod's detour
    // is exactly that shape).
    {
        const unsigned char* base = (const unsigned char*)g_selfModule;
        size_t size = 0;
        if (base) {
            const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
            const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
            size = nt->OptionalHeader.SizeOfImage;
        }
        bool ours = false;
        const unsigned char* fault = (const unsigned char*)ep->ExceptionRecord->ExceptionAddress;
        if (base && fault >= base && fault < base + size) ours = true;
        if (!ours && base) {
            void* frames[48];
            const USHORT n = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
            for (USHORT i = 0; i < n && !ours; ++i) {
                const unsigned char* f = (const unsigned char*)frames[i];
                if (f >= base && f < base + size) ours = true;
            }
        }
        if (!ours) return EXCEPTION_CONTINUE_SEARCH;
    }
    const DWORD tid = GetCurrentThreadId();
    if (tid == g_vehThread) return EXCEPTION_CONTINUE_SEARCH;  // re-entered on this thread
    if (InterlockedCompareExchange(&g_vehLogged, 0, 0) >= 32) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedExchange(&g_vehInside, 1)) return EXCEPTION_CONTINUE_SEARCH;
    g_vehThread = tid;
    // One line per faulting ADDRESS, then a count. The engine's own handled faults repeat dozens
    // of times in a burst (32 x Engine.dll+0x3A740 while a world loads); the first line carries
    // everything a reader needs, the rest would only burn the budget.
    {
        static void* seenAddr[8] = {};
        static volatile LONG seenCount[8] = {};
        void* addr = ep->ExceptionRecord->ExceptionAddress;
        int slot = -1;
        for (int i = 0; i < 8; ++i) {
            if (seenAddr[i] == addr) { slot = i; break; }
            if (!seenAddr[i]) { seenAddr[i] = addr; slot = i; break; }
        }
        if (slot >= 0) {
            const LONG n = InterlockedIncrement(&seenCount[slot]);
            if (n != 1 && n != 10 && n != 100 && n != 1000) {
                g_vehThread = 0;
                InterlockedExchange(&g_vehInside, 0);
                return EXCEPTION_CONTINUE_SEARCH;  // counted, not logged
            }
            if (n != 1) ut::logD("watchdog: the fault at %p has now happened %ld times", addr, n);
        }
    }
    InterlockedIncrement(&g_vehLogged);
    ut::reagentLogFault("a first-chance access violation (vectored handler)",
                        ep->ExceptionRecord, tid);
    g_vehThread = 0;
    InterlockedExchange(&g_vehInside, 0);
    return EXCEPTION_CONTINUE_SEARCH;  // never swallowed
}

void logIdentity() {
    wchar_t self[MAX_PATH] = {0};
    wchar_t exe[MAX_PATH] = {0};
    GetModuleFileNameW(g_selfModule, self, MAX_PATH);
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    ut::logI("=== unique collection tab %s, built %s %s ===", UT_VERSION, __DATE__, __TIME__);
    ut::logI("pid=%lu  host exe = \"%S\"", GetCurrentProcessId(), exe);
    ut::logI("mod dll   = \"%S\"", self);
}

// THE LOAD-ORDER AUDIT. Everything below that could have happened before this DLL existed, and
// what covers it:
//   Engine::LoadMainDatabase may already have run, so its detour never fires. Covered by
//     reagentLateLoadTick (ut_reagent.cpp): the database checksum is non-zero once the load is
//     over, and the tick then calls loadOurArchive itself. It runs on the game thread from
//     hk_PresentSurface every frame and on this thread once a second, so the frame tick normally
//     wins; either way one interlocked guard lets exactly one of them load.
//   The overlay may therefore land after panelInit and reagentInit. Neither reads the database:
//     panelInit reads catalogue.bin, reagentInit resolves exports and reads uniq-*.txt. The page
//     substitution clears its own unavailable flag when the overlay goes in, and no world is up
//     while the loader runs, so the tab comes up either way.
//   The exe's signatures may already be decrypted rather than still behind the Steam stub.
//     Covered both ways: resolveCode runs at init and the game-thread late scans in ut_live and
//     ut_plate retry it, and each installs its own detour on whichever attempt succeeds.
//   Engine.dll and Game.dll are certainly loaded, so waitForGameModules returns at once.
//   Every other detour is on a function the engine calls per frame, per world or per UI action,
//     never once at start-up, so a later install only means a later first call.
// What a late load does cost: the fault watchdog is armed in DllMain, so a first-chance access
// violation before that is not logged.
DWORD WINAPI workerMain(LPVOID) {
    // The settings file is read FIRST, on this thread rather than under the loader lock, because
    // log_level has to be in force before the export and detour lines are written: a reader who
    // asks for debug is asking for exactly that start-up detail.
    ut::configReload(kIniPath);
    ut::logI("settings file = \"%S\" (re-read once a second)", kIniPath);
    ut::logD("worker thread started");

    if (!ut::waitForGameModules(60000)) {
        ut::logE("the mod is OFF: Engine.dll and Game.dll never loaded");
        return 1;
    }
    if (!ut::resolveExports()) {
        ut::logE("the mod is OFF: this build does not match the game - required exports missing");
        return 2;
    }

    // The generated data files. Runs here, on the worker, before every loader below reads them
    // and before any hook exists: a first launch or a game update costs a few seconds, once.
    ut::generateEnsure(g_selfModule);

    // The item catalogue. A failure here is not fatal - the hooks still install and the
    // mod tab says NO DATA.
    ut::panelInit(g_selfModule);

    // The engine's own reagent page - export resolution, the craftingMaterial field offset and
    // the record list our page carries.
    // Must run BEFORE hooksInstall, which installs its detours.
    ut::reagentInit(g_selfModule);

    // The item rollover: resolve the SIX Game.dll exports the item rollover needs (Item /
    // ItemEquipment / ItemArtifact / ItemRelic ::GetUIDisplayText, GameTextLineToString, and the
    // GameTextLine constructor - which is called, never hooked) and build the two prepared
    // GameTextLine records. Must run BEFORE hooksInstall, which installs the FIVE tooltip
    // detours. Every part of it is optional: a miss turns `tooltip_mark` off for the session and
    // is never fatal.
    ut::tooltipInit(g_selfModule);

    // THE BINDINGS GATE. Everything the mod knows about the game's memory that is not a name has now
    // reported whether it could still be found and whether it still looks like itself. One INFO
    // line says how many of each class; the whole table goes to the log at debug. If a single
    // EARLY binding failed, nothing at all is installed - no detour, no tab, no deposit - because
    // a mod that is half bound to a game it does not recognise is the one thing that could damage
    // a save. The exe's own signatures cannot be checked here (its .text is still encrypted until
    // the Steam stub has run), so they are confirmed from the game thread and reported separately.
    if (!ut::bindingsGate(ut::g_gd.resolvedByName, ut::g_gd.missingRequired)) return 4;

    if (!ut::hooksInstall()) {
        ut::logE("the mod is OFF: no detour could be installed");
        return 3;
    }
    ut::logI("READY: exports resolved, detours installed");
    ut::logFlush();  // the banner's last line is on disk before the first frame

    DWORD lastHeartbeat = GetTickCount();
    // The stall check: frames-not-advancing. A "freeze" is usually not a deadlock at all - the
    // game has CRASHED and its own CrashReport.dll is waiting on the named event CRASHREPORT for
    // a dialog nobody can see behind the fullscreen window. That is one OpenEvent away from being
    // said out loud in the log.
    unsigned long long lastFrames = ut::hookFrameCount();
    DWORD lastFrameMove = GetTickCount();
    bool stallReported = false;
    // The worker is what does the file I/O. It wakes on its own stop event, on the journal event
    // a detour signals, or once a second - and every wake flushes the buffered log.
    // The deposit journal: a detour only queues the entry; the file is written here,
    // atomically, so no engine frame ever waits on the disk.
    HANDLE waits[2] = {g_stopEvent, ut::journalEvent()};
    DWORD waitCount = 1;
    if (waits[1]) ++waitCount;
    for (;;) {
        const DWORD w = WaitForMultipleObjects(waitCount, waits, FALSE, 1000);
        if (w == WAIT_OBJECT_0) break;
        if (w == WAIT_OBJECT_0 + 1) {
            ut::journalService();
            ut::logFlush();
            continue;
        }
        ut::configReload(kIniPath);
        ut::reagentLateLoadTick(false);
        ut::logFlush();

        const DWORD now = GetTickCount();
        // ---- the stall check -------------------------------------------------------------
        {
            const unsigned long long frames = ut::hookFrameCount();
            if (frames != lastFrames) {
                lastFrames = frames;
                lastFrameMove = now;
                if (stallReported) {
                    ut::logI("stall over: frames are advancing again (frames=%llu)", frames);
                    stallReported = false;
                }
            } else if (!stallReported && now - lastFrameMove >= 5000) {
                stallReported = true;
                HANDLE ev = OpenEventW(SYNCHRONIZE, FALSE, L"CRASHREPORT");
                const bool crashDll = GetModuleHandleW(L"CrashReport.dll") != nullptr;
                char state[512];
                ut::reagentIdentityState(state, sizeof(state));
                ut::logW("stall: %lu s with no new frame - %s", (now - lastFrameMove) / 1000,
                         (ev || crashDll) ? "the game has CRASHED, not deadlocked"
                                          : "no crash reporter is up: a real stall or a long load");
                ut::logD("stall: frames=%llu updates=%llu, CRASHREPORT event %s, "
                         "CrashReport.dll %s",
                         frames, ut::hookUpdateCount(), ev ? "EXISTS" : "absent",
                         crashDll ? "loaded" : "not loaded");
                ut::logD("stall: %s", state);
                if (ev) CloseHandle(ev);
                ut::logFlush();
            }
        }
        // The heartbeat is trace, and the three status strings are built only when trace is on:
        // each walks mod-owned counters, which is work nobody asked for at info.
        if (now - lastHeartbeat >= 10000) {
            lastHeartbeat = now;
            if (ut::logWants(ut::UT_LOG_TRACE)) {
                ut::logT("heartbeat: frames=%llu updates=%llu transferSeen=%d | %s",
                         ut::hookFrameCount(), ut::hookUpdateCount(),
                         ut::hookSawTransferOpen() ? 1 : 0, ut::panelStatus());
                ut::logT("heartbeat: %s", ut::reagentStatus());
                // The category-button search marks' own six numbers. Interlocked reads of
                // mod-owned LONGs only - see plateSearchStatus() for why the whole plate line is
                // NOT emitted here.
                ut::logT("heartbeat: %s", ut::plateSearchStatus());
            }
        }
    }
    ut::logD("worker thread stopping");
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    (void)reserved;
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            g_selfModule = module;
            DisableThreadLibraryCalls(module);
            ut::utModPathW(module, L"uniquetab.log", kLogPath, MAX_PATH);
            ut::utModPathW(module, L"uniquetab.ini", kIniPath, MAX_PATH);
            ut::logInit(kLogPath);
            logIdentity();
            // The mod folder is resolved before the log file exists, so it cannot report itself.
            // This is the ONE line the Documents fallback is allowed to cost.
            if (const char* warn = ut::utModDirWarning()) ut::logW("%s", warn);

            // The fault watchdog: FIRST in the chain, so a first-chance access
            // violation is logged before anything else can consume it. It only ever logs.
            g_veh = AddVectoredExceptionHandler(1, utVectoredHandler);
            if (g_veh) {
                ut::logD("watchdog: a first-chance access violation with this mod on the stack is "
                         "logged (module+rva, parameters, thread) and handed on unchanged");
            } else {
                ut::logW("watchdog: the fault handler could NOT be installed - a crash will leave "
                         "nothing in this log");
            }

            g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            g_worker = CreateThread(nullptr, 0, workerMain, nullptr, 0, nullptr);
            if (!g_worker) {
                ut::logE("FATAL: CreateThread for the worker failed, err=%lu", GetLastError());
            }
            break;
        }
        case DLL_PROCESS_DETACH: {
            // NOTHING that touches another thread happens here.
            // MH_DisableHook/MH_Uninitialize suspend every thread in the process, which under
            // the loader lock is a textbook DllMain deadlock, and WaitForSingleObject on the
            // worker can time out and then unmap the image while a detour is still live. The
            // hooks are left in place until process exit; the worker is asked to stop but is
            // never waited on. (The loader never calls FreeLibrary on an .asi, so an unload
            // before process exit does not happen in practice.)
            ut::logI("shutting down: frames=%llu updates=%llu (processExit=%d)",
                     ut::hookFrameCount(), ut::hookUpdateCount(), reserved ? 1 : 0);
            ut::logD("the detours are deliberately left installed: disabling them here would "
                     "suspend every thread under the loader lock");
            if (!reserved && g_stopEvent) SetEvent(g_stopEvent);
            ut::logI("=== unique-tab unloaded ===");
            ut::logShutdown();
            break;
        }
        default:
            break;
    }
    return TRUE;
}
