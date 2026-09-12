// ut_plate.h - the page BACKGROUND (the painted cover plate) follows the collection group, and
// this file owns the captured material ReagentWindow: the plate swap, the page-visibility test,
// the window rect, the label draw route, the owned-record snapshot and the search marks.
//
// A third caravan tab is not possible, so the collection shares the Crafting Materials page.
// Its painted cover plate belongs to the vanilla 24-slot layout and is wrong behind a collection
// group, so the plate has to switch live with the group.
//
// WHERE THE PLATE POINTER LIVES  (all RVAs are `Grim Dawn.exe` 1.3.0.8, read out of the
// decrypted image; "window + 0x518" is the SEARCH BORDER, not the plate - +0x518 is
// `ui/character/itemsearchborder.tex`, loaded at 0x132332 and drawn once per box at 0x1336A8):
//
//   ReagentWindow ctor 0x131CC0 embeds a BitmapSingle widget INLINE at window + 0x480
//       (its vtable is stored there at 0x131D6B; a second base vtable at +0x488).
//   ReagentWindow::Load 0x132100 reads the record field `TransferPlate` (0x1321C1) and calls
//       [window+0x480]->vtable[+0x18]  = 0x121CD0 = BitmapSingle::Load     (0x132214)
//   BitmapSingle::Load reads `bitmapName` (0x121D2E) and calls
//       this->vtable[+0x88]           = 0x121820 = BitmapSingle::SetBitmap (0x121DB7)
//   BitmapSingle::SetBitmap:
//       0x121883  UnloadTexture(this[0x30])      ; releases the old one
//       0x1218B2  this[0x30] = LoadTexture(path) ; THE PLATE POINTER  == window + 0x4B0
//       0x1218FE  this[0x40] = GetWidth(tex)     ; the destination rect's w/h are FROZEN here
//       0x121910  this[0x44] = GetHeight(tex)
//   BitmapSingle::Draw 0x121440 re-reads `[this+0x30]` EVERY FRAME - twice, plus
//       GraphicsTexture::GetRect for the UVs and GetTexture for the RenderTexture - and caches
//       nothing.  So writing a different `GraphicsTexture*` into window + 0x4B0 changes the
//       painted plate on the very next frame, and writing the original back restores it.
//
// Because w/h were frozen from the vanilla plate, a replacement of a different size would be
// STRETCHED into the vanilla rectangle.  tools/make_plates.py therefore emits every plate at
// exactly 438 x 627, the size of ui/caravan/caravan_transfercomponent1_bg.tex.
//
// The window pointer is captured by detouring `ReagentWindow::Load` (a 45-byte prologue
// signature that must match EXACTLY ONCE in the decrypted .text, cross-checked against the
// captured object's own vtable slot +0x18) and noticing which of the two ReagentWindows
// triggered our frame-record substitution: `liveBeginCapture()` fires from inside the
// materials window's Load and nowhere else, so an epoch counter around the original call
// identifies it with no string compare and no window-list walk.
//
// The rules every engine touch in this file follows: one SEH-guarded helper per touch, the first
// fault disables the feature for the session, the textures are loaded once and never unloaded
// mid-world, and the vanilla pointer is restored on group -1 and on world teardown.
#pragma once

#include <windows.h>

struct GdTexture;   // GAME::GraphicsTexture (gd_runtime.h)

namespace ut {

// Resolves the ReagentWindow::Load signature and reads data/plates/PLATES.txt (optional; the
// built-in name pattern is used when it is absent). Safe to call twice.
bool plateInit(HMODULE selfModule);

// Installs the ReagentWindow::Load detour. Adds its target to *total when it really installs.
int plateInstall(int* total);

// Game-thread tick: the late signature scan (the exe .text is Steam-DRM encrypted at init).
void plateTick(bool gameThread);

// Called by ut_live.cpp right after a relayout succeeded. group < 0 restores the vanilla plate.
void plateApply(int group, const char* label, int cellW, int cellH);

// Forget the captured window (its HUD is gone) and release every texture the mod loaded, so the
// next world re-loads them instead of re-using a `GraphicsTexture*` the engine already unloaded.
void plateOnWorldTeardown();

// ---- PAGE VISIBILITY ----------------------------------------------------------------------------
// `caravan+0x1728` is the visible page index (0 transfer, 1 private stash, 2 Components,
// 3 Materials), and a page that is not `pages[+0x1728]` receives no input at all - so every
// wheel that reached the collection with the stash tab up came from the mod's own input hook.
// ReagentWindow's show/hide vtable slot is the folded `ret 0` stub (0x00005810), i.e. there is no
// per-window visible flag to read: the page index is the ONLY state, which is why this file
// resolves the CaravanWindow instead of detouring a slot.
//
// The CaravanWindow is embedded in the Hud (hud+0x4FD08) and holds its four pages as pointers at
// caravan+0x13C8[4]; the material ReagentWindow is [3] = caravan+0x13E0.  There is no accessor
// exported for it, so it is recovered inside our own `ReagentWindow::Load` detour by scanning the
// CALLER's stack frame (CaravanWindow::Load is the only caller) for a value P where
//     *(void**)(P+0x13E0) == the window we just captured   AND
//     *(void**)(P+0x13D8) is a second object whose vtable slot +0x18 is the same
//                          located ReagentWindow::Load     AND
//     *(void**)(P+0x13C8) and (P+0x13D0) are plausible pointers  AND
//     *(int*)(P+0x1728) is 0..3
// Four independent structural facts have to line up, so a false positive is not credible; when
// nothing matches, the state stays UNKNOWN and every caller fails safe (no wheel is consumed and
// no label is drawn).
int plateCaravanState();       // 0 = unknown, 1 = hidden, 2 = Materials is the visible page
bool plateMaterialsVisible();  // false when unknown or unarmed
// True only while the MOD's generated plate is the one installed in the window - false on the
// game's own caravan_transfercomponent1_bg.tex, whose central medallion at record x 248..279 the
// label has to stop short of.  Not the same question as "is a collection group shown":
// plate_swap=0 and un-installed loose plates both show a collection over the VANILLA plate.
bool plateModPlateShown();

// The captured window's live rect in SCREEN pixels: the box widgets' own parent origin
// ([box+0x80]/[box+0x84], the pair UIReagentItem's screen-position slot 0x1EE840 adds to every
// box's local position) plus the plate's frozen draw rect (window+0x4C0/+0x4C4).  Used for the
// wheel's cursor test and to anchor the group label; false = not captured / not readable, and
// then neither draws nor consumes anything.
bool plateWindowRect(float* x, float* y, float* w, float* h);

// True when (sx, sy) - CLIENT pixels of the game window, which is the same space the engine
// draws in - is inside that rect.
bool plateCursorInWindow(int sx, int sy);

// ---- the label under the tooltips ---------------------------------------------------------------
// A label emitted from `Engine::PresentSurface` is painted after the whole HUD including the
// tooltip layer, so it would sit ON TOP of item tooltips. That is a draw-ORDER fact, not a
// geometry bug, and there are two routes around it.
//
// ROUTE 1 (the proper fix, and what runs once the Draw hook is armed).  The ReagentWindow's
// primary vtable (decrypted-exe rva 0x3142B8, written by the ctor at 0x131CE1): +0x10 deleting
// dtor 0x131E30, **+0x18 Load 0x132100**, **+0x20 Draw 0x133440**, +0x38 mouse handler 0x1328D0,
// +0x120 Sync 0x1324A0 - each of those addresses appears exactly once as a qword in the image,
// so the slot mapping is unambiguous.  Draw's own bytes confirm the frame this code depends on:
//     00133454  movss  xmm0,[r8]              ; r8 = the caller's Vec2 offset
//     0013345C  addss  xmm0,[rcx+0x40]        ; + the window origin
//     00133461  movss  xmm1,[rcx+0x44]
//     00133469  addss  xmm1,[r8+4]
//     00133486  movss  [rbp+0x67],xmm0        ; the screen origin every child is drawn at
//     0013347B  add    rcx,0x480 ; call [rax+0x20]   ; the inline plate widget
//     001334C0..0013364A                      ; the 16-byte box vector
//     001337D7  call   [rip+0x1a2483]         ; = IAT 0x2D5C60
//                                             ;   GraphicsCanvas::RenderRect(Rect,Color)
//                                             ;   with rcx = r14 = Draw's SECOND argument
// so `Draw(ReagentWindow* this, GraphicsCanvas* canvas, Vec2 const* offset)` and the function
// ends in a plain `ret` at 0x133807 (void).  Anything emitted at the tail is painted before the
// tooltip layer and therefore UNDER it.
//
// The address is never hard-coded: it is read from the CAPTURED window's own vtable, accepted
// only when slot +0x18 equals the signature-located `ReagentWindow::Load` and slot +0x20 lies
// inside the exe `.text`, and the vtable qword itself is NEVER written (that would retarget the
// Components window too, and .rdata is page-protected).  The read happens inside our own
// `ReagentWindow::Load` detour, i.e. on the game thread after the Steam-DRM late scan has
// already decrypted `.text`; the MinHook install itself is deferred to the next game-thread
// tick so MinHook never runs inside an engine detour body.
//
// ROUTE 2 (the fallback, and what runs before a HUD exists or when the hook cannot arm): keep the
// PresentSurface draw but suppress the label whenever the cursor is inside any captured box's
// LIVE hit rect - box+0x6C/+0x70/+0x74/+0x78, the exact rectangle the engine's own hit loop
// tests at exe 0x1329D0..0x132A14.  A box tooltip can only appear when the cursor is in one of
// those rects, so this hides the label exactly when a tooltip could be showing.

// True when the ReagentWindow::Draw detour is armed, i.e. the label is already being drawn under
// the tooltips and the PresentSurface path must not draw it again.
bool plateLabelInDraw();

// Route 2's suppression test: (sx, sy) in CLIENT pixels is inside some captured box's live hit
// rect. False when nothing is captured or the read faults (fail-safe = draw the label).
bool plateCursorInAnyBox(int sx, int sy);

// ---- box hit test in window-local space ---------------------------------------------------------
// The same test in the engine's own WINDOW-LOCAL space: box+0x6C/+0x70 is the box's local
// position and +0x74/+0x78 its size, so the point the ReagentWindow's mouse handler computes
// (evt - offset - window+0x40/+0x44) is compared with them directly, without the parent origin.
// False when nothing is captured or the read faults (fail-safe = "not on a box" is NOT assumed:
// the caller treats false-from-a-fault as a reason to pass the event through).
bool plateBoxHitLocal(float lx, float ly, bool* faulted);

// Releases one texture the mod loaded, through the single GraphicsEngine::UnloadTexture
// resolution site this file owns (SEH-framed). false = unavailable, or the call faulted.
bool plateUnloadTexture(const GdTexture* tex);

// Logged once per world by whichever route actually drew the label, so one session log says
// which one ran. route 1 = ReagentWindow::Draw, route 2 = the PresentSurface fallback.
void plateNoteLabelRoute(int route);

// ---- owned-record snapshot ------------------------------------------------------------------
// One in-order walk of GameEngine's own reagent map (the same read-only walk ut_reagent.cpp
// uses for reconciliation), cached so the label can be drawn every frame without touching the
// engine. Game thread only. Returns the number of records found, or -1 if the map was
// unreadable. Throttled internally to `plate_count_ms`.
int plateOwnedRefresh(bool force);

// ---- the search marks ---------------------------------------------------------------------------
// Bit i = "collection group i holds a match for whatever is typed in the caravan's own search
// box". 0 = nothing is typed, the amortised sweep for the current needle has not finished yet,
// `search_buttons=0`, or the feature disabled itself. The answer comes from TWO sources, and both
// are the ENGINE'S OWN predicate, never a mod-side guess:
//   * the sweep: the exported `Item::SearchText` asked of the stored prototype of every OWNED
//     record, 32 records per game-thread tick, restarted whenever the needle changes;
//   * free, and instantaneous: the matched-rect vector `ReagentWindow::UpdateSearch` fills for
//     the page on screen, which marks the group the user is looking at.
// Pure interlocked reads: no engine call, no lock and no allocation, so the strip's draw path
// stays as call-free as it was.
unsigned int plateSearchMask();

// Drop the marks and restart the search sweep, because something other than the needle changed
// under it - today, the owned-only filter, which decides whether the sweep runs at all.
// Interlocked stores only; callable from the game thread's click path.
void plateSearchRearm();
bool plateOwns(const char* record);
int plateOwnedTotal();

// What the last walk saw, for the heartbeat: how many NODES the engine's reagent map holds and
// how many of them are EMPTIED (the node is there, but its stored prototype is gone or its stack
// is 0 - the state a take leaves behind, which must not count as owned). `plateOwnedTotal()` is
// nodes - emptied. Atomics only; any thread.
void plateOwnedCounts(int* nodes, int* emptied);

// The other half of the same snapshot: how many records the mod's OWN private table holds with a
// count >= 1, how many copies those rows add up to, and how many of those records the engine's
// map does NOT hold - i.e. how much of the collection has already moved out of `reagents.gst`.
// The owned set `plateOwns()` answers from is the UNION of the two halves; these three numbers
// are the SUM, which is a different question (a record in both places is legitimately two
// copies). 0/0/0 while the mod's own file holds nothing. Interlocked reads of mod-owned LONGs
// only: no lock, no engine memory, any thread - the heartbeat calls it.
void plateTableCounts(int* rows, int* copies, int* tableOnly);

// A monotonic counter that moves whenever the reagent map's node set, any stored prototype id,
// or any node's "the collection really holds this" bit changes - i.e. whenever `plateOwns()` may
// answer differently. `liveTick` watches it to re-clamp the first visible row while the
// owned-only filter is on: rows are in FILTERED units there, so a take that empties four records
// can leave `g_curRow` past the last row that still exists and every wheel event then clamps to
// the row it is already on. Interlocked read: any thread.
long plateOwnedGeneration();

const char* plateStatus();

// The search marks' own state for the heartbeat - `hook= off= needle= mask= valid= sweeps=`.
// Interlocked reads of mod-owned LONGs only, no engine memory, so the worker thread may call it
// (`plateStatus()` may not: it reads a live UI object).
const char* plateSearchStatus();

}  // namespace ut
