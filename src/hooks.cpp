// hooks.cpp - the detours.
//
// Rules every detour here obeys:
//   * it always calls the original, with the arguments it was given, and returns the original's
//     result unchanged - the sack detours OBSERVE, they never alter an add or a remove;
//   * its own body is wrapped in __try/__except so a mistake cannot take the game down;
//   * it never creates, moves, deletes or saves anything, and reads no keyboard state.
//     The only input it looks at is the mouse messages the game's own window already receives,
//     and only while the stash is open and the cursor is over the mod's own rectangles.
//
// Seven detours: Engine::PresentSurface, GameEngine::Update, GameEngine::SetTransferOpen,
// InventorySack::AddItem x2, InventorySack::RemoveItem, WinWindow::WindowProc.
// GraphicsCanvas::EndFrame is NOT hooked: it is never called on 1.3.0.8.

#include "hooks.h"

#include <windows.h>

#include <stdio.h>

#include "MinHook.h"
#include "gd_runtime.h"
#include "ut_config.h"
#include "ut_log.h"
#include "ut_panel.h"
#include "ut_reagent.h"
#include "ut_tooltip.h"

namespace ut {
namespace {

PfnEngine_PresentSurface o_PresentSurface = nullptr;
PfnGameEngine_Update o_GameUpdate = nullptr;
PfnGameEngine_SetTransferOpen o_SetTransferOpen = nullptr;
PfnSack_AddItem o_SackAddItem = nullptr;
PfnSack_AddItemVec o_SackAddItemVec = nullptr;
PfnSack_RemoveItem o_SackRemoveItem = nullptr;
PfnWindow_WindowProc o_WindowProc = nullptr;


volatile LONG64 g_frames = 0;
volatile LONG64 g_updates = 0;
volatile LONG g_sawTransferOpen = 0;

DWORD g_lastFrameLogTick = 0;
LONG64 g_lastFrameLogValue = 0;
bool g_loggedFirstPresent = false;
bool g_loggedFirstUpdate = false;


// The GameEngine `this` pointer captured from Update/SetTransferOpen. Read-only use.
GdGameEngine* volatile g_gameEngineThis = nullptr;

// ---------------------------------------------------------------------------------------------
// Engine::PresentSurface -- frame counter and the only draw site
// ---------------------------------------------------------------------------------------------
// The body of every detour is wrapped in a C++ try/catch as well as the SEH
// __try in the detour itself, so a std::bad_alloc raised deep inside the mod's own containers is
// swallowed here, where the stack is still ours, instead of unwinding into the engine's frame.
void presentBody(GdEngine* self) try {
    const LONG64 frames = InterlockedIncrement64(&g_frames);

    if (!g_loggedFirstPresent) {
        g_loggedFirstPresent = true;
        unsigned int engineFrame = 0;
        if (g_gd.EngineGetFrameCount) engineFrame = g_gd.EngineGetFrameCount(self);
        logD("HOOK Engine::PresentSurface first call: this=%p Engine::GetFrameCount()=%u",
             (void*)self, engineFrame);
        g_lastFrameLogTick = GetTickCount();
        g_lastFrameLogValue = frames;
    }

    if (g_gd.EngineGetGraphicsEngine && g_gd.GfxGetCanvas) {
        GdGraphicsEngine* gfx = g_gd.EngineGetGraphicsEngine(self);
        if (gfx) {
            GdCanvas* canvas = g_gd.GfxGetCanvas(gfx);
            if (canvas) {
                // The collection view. It checks GameEngine::IsTransferOpen() itself, so
                // nothing of it can appear at the main menu.
                panelDraw(canvas, g_gameEngineThis);
            }
        }
    }

    const DWORD now = GetTickCount();
    if (now - g_lastFrameLogTick >= 5000) {
        const LONG64 delta = frames - g_lastFrameLogValue;
        const double secs = (double)(now - g_lastFrameLogTick) / 1000.0;
        unsigned int engineFrame = 0;
        if (g_gd.EngineGetFrameCount) engineFrame = g_gd.EngineGetFrameCount(self);
        logT("frames: total=%lld  +%lld in %.1fs (%.1f fps)  Engine::GetFrameCount()=%u",
             frames, delta, secs, secs > 0.0 ? (double)delta / secs : 0.0, engineFrame);
        g_lastFrameLogTick = now;
        g_lastFrameLogValue = frames;
    }
} catch (...) {
}

void __cdecl hk_PresentSurface(GdEngine* self) {
    __try {
        // The game thread is the safe place to slot our extra .arz in if the
        // LoadMainDatabase detour was installed too late to catch it, and the safe place to
        // read one record back out of the live database once. Both are one interlocked read
        // per frame after the first success.
        // It reaches file I/O paths, so it must stay INSIDE the __try.
        reagentLateLoadTick(true);
        presentBody(self);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // no logging inside the filter path: keep the crash path as small as possible
    }
    if (o_PresentSurface) o_PresentSurface(self);
}

// ---------------------------------------------------------------------------------------------
// GameEngine::Update -- game-thread tick, and where the GameEngine `this` is captured
// ---------------------------------------------------------------------------------------------
// The observable world-teardown signal. Once a session has had a main player,
// the first Update that reports none means the world is gone and every Item* the mod remembers
// belongs to it. Polled every 30 updates, so this costs one exported call twice a second.
void watchForTeardown(GdGameEngine* self, LONG64 n) {
    static bool sawPlayer = false;
    if (!g_gd.GameGetMainPlayer || (n % 30) != 0) return;
    const bool have = g_gd.GameGetMainPlayer(self) != nullptr;
    if (have) {
        // `self` IS the GameEngine, and reagentOnMainPlayer captures it. This detour is the
        // earliest place in a live world that has the pointer in its hand.
        //
        // Called on EVERY POLL, not just the first one: the hardcore/softcore switch inside
        // reagentOnMainPlayer must get more than one chance per world - if the mode cannot be
        // read on the first tick that has a main player, it would otherwise stay unknown for the
        // whole session. A mode that becomes readable later (or changes under a running world)
        // moves the collection on the next poll. Everything in reagentOnMainPlayer that must
        // happen once is latched inside it, so repeating the call costs two interlocked reads and
        // one call to GameInfo::GetHardcore, twice a second.
        reagentOnMainPlayer(self);
        sawPlayer = true;
    } else if (sawPlayer) {
        sawPlayer = false;
        reagentOnWorldTeardown("the first GameEngine::Update after a session with no main player");
    }
}

void updateBody(GdGameEngine* self, int deltaMs) try {
    g_gameEngineThis = self;
    const LONG64 n = InterlockedIncrement64(&g_updates);
    if (!g_loggedFirstUpdate) {
        g_loggedFirstUpdate = true;
        logD("HOOK GameEngine::Update first call: this=%p delta=%d", (void*)self, deltaMs);
    } else if ((n % 600) == 0) {
        bool open = false;
        if (g_gd.GameIsTransferOpen) open = g_gd.GameIsTransferOpen(self);
        logT("GameEngine::Update tick %lld (delta=%d, IsTransferOpen=%d)", n, deltaMs,
             open ? 1 : 0);
    }
    watchForTeardown(self, n);
} catch (...) {
}

void __cdecl hk_GameUpdate(GdGameEngine* self, int deltaMs) {
    __try {
        updateBody(self, deltaMs);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (o_GameUpdate) o_GameUpdate(self, deltaMs);
}

// ---------------------------------------------------------------------------------------------
// GameEngine::SetTransferOpen -- the stash open/close signal
// ---------------------------------------------------------------------------------------------
void transferBody(GdGameEngine* self, bool open) try {
    g_gameEngineThis = self;
    InterlockedExchange(&g_sawTransferOpen, 1);
    // SetTransferOpen(true) is called EVERY FRAME while the caravan is up, so this line is
    // edge-triggered too - a per-call line would be thousands of lines per session.
    static int lastState = -1;
    const int state = open ? 1 : 0;
    if (lastState != state) {
        lastState = state;
        logD("HOOK GameEngine::SetTransferOpen(this=%p, open=%d)", (void*)self, state);
    }
    panelOnTransferOpen(self, open);
} catch (...) {
}

void __cdecl hk_SetTransferOpen(GdGameEngine* self, bool open) {
    __try {
        transferBody(self, open);
        // The caravan's open/close edge - the reconcile and the registry sweep hang off it.
        reagentOnTransferOpen(self, open);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (o_SetTransferOpen) o_SetTransferOpen(self, open);
}


// ---------------------------------------------------------------------------------------------
// InventorySack::AddItem / RemoveItem -- ownership, OBSERVED ONLY
//
// The original runs first with the caller's own arguments; only then do we look at the Item*.
// Neither detour changes an argument, and both return exactly what the original returned.
// ---------------------------------------------------------------------------------------------
void observeAdd(GdItem* item, bool accepted) try {
    if (!accepted) return;
    panelOnItemAdded(item);
    // If this item's record is one of the ones our reagent page carries, arm its
    // craftingMaterial byte so the engine will accept it when the user drops it on a box.
    reagentNoteItem(item);
} catch (...) {
}

bool __cdecl hk_SackAddItem(GdSack* self, GdItem* item, bool a, bool b) {
    const bool r = o_SackAddItem ? o_SackAddItem(self, item, a, b) : false;
    __try {
        observeAdd(item, r);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return r;
}

bool __cdecl hk_SackAddItemVec(GdSack* self, const void* pos, GdItem* item, bool a) {
    const bool r = o_SackAddItemVec ? o_SackAddItemVec(self, pos, item, a) : false;
    __try {
        observeAdd(item, r);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return r;
}

bool __cdecl hk_SackRemoveItem(GdSack* self, unsigned int itemId) {
    const bool r = o_SackRemoveItem ? o_SackRemoveItem(self, itemId) : false;
    __try {
        if (r) {
            panelOnItemRemoved(itemId);
            // The item left a sack the mod watches - drop it from the id map so the
            // choke point's "in a sack the mod can see" test means that and not "seen once".
            reagentForgetSackItem(itemId);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return r;
}

// ---------------------------------------------------------------------------------------------
// WinWindow::WindowProc -- mouse wheel and left clicks over the mod's own panel
//
// Everything the panel does not claim is passed straight to the original window procedure, so
// the game's own input path is untouched. Nothing about input is logged beyond two counters.
// ---------------------------------------------------------------------------------------------
// A __try and a C++ try may not share a function, so the C++ half lives in its own.
bool handleMessageCpp(UINT msg, WPARAM wParam, LPARAM lParam, LRESULT* result) try {
    // The page switch. It only ever changes one integer and rewrites uniquetab.ini.
    return reagentHandleMessage(msg, wParam, lParam, result);
} catch (...) {
    return false;
}

bool windowProcBody(UINT msg, WPARAM wParam, LPARAM lParam, LRESULT* result) {
    bool handled = false;
    __try {
        handled = handleMessageCpp(msg, wParam, lParam, result);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        handled = false;
    }
    return handled;
}

LRESULT __cdecl hk_WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    LRESULT result = 0;
    if (windowProcBody(msg, wParam, lParam, &result)) return result;
    return o_WindowProc ? o_WindowProc(hwnd, msg, wParam, lParam)
                        : DefWindowProcW(hwnd, msg, wParam, lParam);
}

const char* mhText(MH_STATUS s) {
    const char* t = MH_StatusToString(s);
    return t ? t : "?";
}

bool createOne(const char* pretty, void* target, void* detour, void** original) {
    if (!target) {
        logE("  hook %-32s SKIPPED (target not resolved)", pretty);
        return false;
    }
    MH_STATUS s = MH_CreateHook(target, detour, original);
    if (s != MH_OK) {
        logE("  hook %-32s MH_CreateHook FAILED: %s", pretty, mhText(s));
        return false;
    }
    s = MH_EnableHook(target);
    if (s != MH_OK) {
        logE("  hook %-32s MH_EnableHook FAILED: %s", pretty, mhText(s));
        return false;
    }
    logD("  hook %-32s installed at %p (trampoline %p)", pretty, target, *original);
    return true;
}

bool g_initialised = false;

}  // namespace

bool hooksInstall() {
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
        logE("MH_Initialize FAILED: %s", mhText(s));
        return false;
    }
    g_initialised = true;
    logD("installing detours (MinHook):");

    int ok = 0;
    ok += createOne("Engine::PresentSurface", (void*)g_gd.EnginePresentSurface,
                    (void*)&hk_PresentSurface, (void**)&o_PresentSurface) ? 1 : 0;
    ok += createOne("GameEngine::Update", (void*)g_gd.GameUpdate, (void*)&hk_GameUpdate,
                    (void**)&o_GameUpdate) ? 1 : 0;
    ok += createOne("GameEngine::SetTransferOpen", (void*)g_gd.GameSetTransferOpen,
                    (void*)&hk_SetTransferOpen, (void**)&o_SetTransferOpen) ? 1 : 0;
    ok += createOne("InventorySack::AddItem(Item*)", (void*)g_gd.SackAddItem,
                    (void*)&hk_SackAddItem, (void**)&o_SackAddItem) ? 1 : 0;
    ok += createOne("InventorySack::AddItem(Vec2)", (void*)g_gd.SackAddItemVec,
                    (void*)&hk_SackAddItemVec, (void**)&o_SackAddItemVec) ? 1 : 0;
    ok += createOne("InventorySack::RemoveItem", (void*)g_gd.SackRemoveItem,
                    (void*)&hk_SackRemoveItem, (void**)&o_SackRemoveItem) ? 1 : 0;
    ok += createOne("WinWindow::WindowProc", (void*)g_gd.WindowProc, (void*)&hk_WindowProc,
                    (void**)&o_WindowProc) ? 1 : 0;

    // The reagent page - the database overlay and the craftingMaterial gate.
    int reagentTotal = 0;
    const int reagentOk = reagentInstall(&reagentTotal);

    const int installed = ok + reagentOk;
    const int wanted = 7 + reagentTotal;
    if (installed == wanted) {
        logI("detours installed: %d of %d", installed, wanted);
    } else {
        logW("detours installed: %d of %d - the missing ones are features this run does not have",
             installed, wanted);
    }

    // The item rollover (the "already collected" line). DELIBERATELY OUTSIDE the
    // tally above and logged separately - the five Game.dll detours (Item / ItemEquipment /
    // ItemArtifact / ItemRelic ::GetUIDisplayText and GameTextLineToString) are OPTIONAL, all-or-nothing
    // among themselves, and a missing tooltip export must turn one feature off, never count as
    // a missing detour for the whole mod.
    tooltipInstall(nullptr);
    return ok > 0;
}

// This is NOT called from DllMain. MH_DisableHook/MH_Uninitialize
// suspend every thread, which under the loader lock is a textbook DllMain deadlock, and after an
// unmap a still-live detour jumps into unmapped code. The hooks stay in place until process exit.
// Kept for a future explicit, non-DllMain shutdown path.
void hooksRemove() {
    if (!g_initialised) return;
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    g_initialised = false;
    logD("detours removed");
}

unsigned long long hookFrameCount() {
    return (unsigned long long)InterlockedCompareExchange64(&g_frames, 0, 0);
}
unsigned long long hookUpdateCount() {
    return (unsigned long long)InterlockedCompareExchange64(&g_updates, 0, 0);
}
bool hookSawTransferOpen() {
    return InterlockedCompareExchange(&g_sawTransferOpen, 0, 0) != 0;
}

}  // namespace ut
