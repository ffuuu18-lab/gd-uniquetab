// ut_startorder.h - two start-up decisions kept pure so the offline harness can prove them.
//
// 1. WHEN the plates may be loaded. GraphicsEngine::LoadTexture is an engine resource load; the
//    first frame the hooks see can fall in the engine's own start-up streaming, when its loader
//    thread is decoding archive parts (GAME::Archive, LZ4_decompress_fast). A fresh install
//    showed a fault on that thread right after the mod loaded seven plates and the cover image
//    there. The plates are therefore loaded only once a materials window has been captured in a
//    world: the HUD build, where the engine loads its own plate through the same call.
//
// 2. WHAT the stall check calls a frozen frame counter. The counter is the mod's own count of
//    PresentSurface calls since the hooks went in; at most one means no frame has been presented
//    after the first one, which is a slow first start as often as a stop, never proof of a crash.
//    Only the engine's CRASHREPORT event says crash.
#pragma once

namespace ut {

inline bool utPlateProbeAllowed(bool codeResolved, bool windowCaptured) {
    return codeResolved && windowCaptured;
}

enum UtStallVerdict {
    UT_STALL_CRASHED = 0,     // the crash reporter's event exists
    UT_STALL_NO_FRAME_YET,    // no frame presented after the first hooked one
    UT_STALL_CRASH_DLL,       // frames stopped and CrashReport.dll is in the process
    UT_STALL_UNKNOWN,         // frames stopped, no crash reporter: a stall or a long load
};

inline UtStallVerdict utStallVerdict(bool crashEvent, bool crashDll, unsigned long long frames) {
    if (crashEvent) return UT_STALL_CRASHED;
    if (frames <= 1) return UT_STALL_NO_FRAME_YET;
    return crashDll ? UT_STALL_CRASH_DLL : UT_STALL_UNKNOWN;
}

inline const char* utStallVerdictText(UtStallVerdict v) {
    switch (v) {
    case UT_STALL_CRASHED: return "the game has CRASHED, not deadlocked";
    case UT_STALL_NO_FRAME_YET:
        return "no frame yet - nothing has been presented since the first hooked frame "
               "(a slow first start, or the game stopped before it)";
    case UT_STALL_CRASH_DLL:
        return "the crash reporter is loaded but has not signalled: a crash, a stall or a long load";
    default: return "no crash reporter is up: a real stall or a long load";
    }
}

}  // namespace ut
