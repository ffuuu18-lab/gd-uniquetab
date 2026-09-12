// ut_live.h - LIVE re-point of the reagent page's boxes (no character reload). This file owns
// the group table (uniq-groups.txt), the box capture, the relayout, the owned-only filter and the
// Ctrl+wheel / button input that selects a group.
//
// `ReagentWindow::Load` APPENDS its boxes, so re-running it would double every widget; the boxes
// that already exist are re-pointed instead, with the engine's own code:
//
//   UIReagentItem::Load(box, boxRecord)   exe rva 0x1F0660, vtable slot +0x18
//       reads itemBoxX/itemBoxY -> vtable[0xB8](pos), reads reagentName, creates the display
//       prototype through ObjectManager::CreateObjectFromFile, SetStackSize(0),
//       InitializeItem(), then vtable[0xA8](objectId, true).  Nothing is appended anywhere.
//   UIReagentItem::SetItem(u32,bool)      exe rva 0x1EE2E0, vtable slot +0xA8
//       id 0 (or a non-Item) takes the null branch, which CLEARS the box - that is how a
//       surplus box is hidden.  The previous texture is released by the engine itself.
//   UIReagentItem::SetLocalPosition       exe rva 0x1EE830, vtable slot +0xB8
//       two float stores into box+0x6C / box+0x70.  Nothing else.
//
// All three are exe-internal, so each is located at run time by a byte signature that must
// match EXACTLY ONCE in the loaded .text, and then cross-checked against a live box's own
// vtable (slot 0x18/0xA8/0xB8 must equal the three located addresses).  Any mismatch, any
// SEH fault, or a missing data file disables live paging for the session and logs why.
//
// The page the HUD builds is always `records/ui/caravan/uniq_frame.dbr`: the 24 REAL vanilla
// box records first (so a freshly loaded character sees the untouched Crafting Materials
// page) then 136 filler boxes parked at -4000,-4000.  Everything after that is re-pointing.
//
// Ownership: one display prototype per DISTINCT record ever shown, cached and re-used, so a
// scroll allocates nothing.
#pragma once

#include <windows.h>

namespace ut {

// Loads uniq-groups.txt from the mod folder and resolves the three signatures. Safe to call twice.
bool liveInit(HMODULE selfModule);

// Installs the UIReagentItem::Load detour (box capture). Adds its target count to `total`.
int liveInstall(int* total);

// True when live paging is armed: the ini switch is on, the data loaded and the code found.
bool liveActive();

// The record the page substitution must hand back while live paging is armed, or nullptr.
const char* liveFramePath();

// Called from the LoadTableFile detour the moment the frame record is substituted: the boxes
// the exe is about to build belong to the material ReagentWindow.
void liveBeginCapture();

// The counterpart: the substituted record was not in the active database and the detour handed
// the engine the VANILLA record instead, so the boxes about to be built are not ours. Closes the
// capture before the first of them and takes the capture epoch back - ut_plate.cpp names the
// material window by that epoch moving, and this window is not one of ours.
void liveCancelCapture();

// World teardown: re-arms a stop that was taken for the SHAPE of the world's HUD (a box vtable
// that did not match). A fault or a failed detour install is not re-armed - it stays off for the
// session. Writes no engine memory.
void liveOnWorldTeardown();

// Game-thread tick: applies a pending relayout and debounces the ini write.
void liveTick(bool gameThread);

// ---- repaint the group that is already on screen ------------------------------------------------
// The engine's own `ReagentWindow::Sync` skips a box that already shows the node's object id
// (exe 0x132663), so an IN-PLACE prototype refresh - same object id, new contents - repaints
// nothing; and a prototype SWAP only repaints because the id changed.  A relayout repaints both:
// `applyLayout` re-`SetItem`s every box to the mod's display prototype and
// `reagentAfterRelayout()` then lets Sync put the node's own prototype back.
//
// This call does NOT touch the engine - it sets a flag that the next GAME-THREAD `liveTick`
// turns into exactly one relayout of the group currently shown (the same path Ctrl+wheel takes,
// including `reagentAfterRelayout` and `plateApply`).  It is therefore safe to call from inside
// a detour body, from any thread, and any number of times before the tick coalesces them.
// Nothing happens when live paging is not armed, no boxes are captured, or no layout has run.
void liveRelayoutVisible();

// A thin re-export of ut_plate.cpp's page-visibility accessor, so ut_reagent.cpp can gate its
// own wheel/key handler on it without gaining an include. True only when the caravan is open AND
// the Crafting Materials page is the one on screen (caravan+0x1728 == 3).
bool liveMaterialsVisible();

// Input. Returns true when the event was consumed. `ticks` > 0 is wheel-up.
bool liveHandleWheel(int ticks, bool ctrl);
bool liveHandleKey(int vk, bool ctrl);

// The collection group the page is CURRENTLY SHOWING (the last relayout that actually ran), or
// -1 for the vanilla Crafting Materials layout / when live paging is not armed. This is what the
// deposit gate reads. `label` is optional and gets the group's name (never null when the call
// returns).
int liveShownGroup(const char** label);

// ---- UI scale -----------------------------------------------------------------------------------
// The measured UI scale for the CURRENT HUD (the 136 filler boxes are Loaded from records at
// exactly (-4000,-4000) and UIReagentItem::Load multiplies by GetUIScaleFactor, so the value is
// MEASURED, never guessed).  Independent of the shown group, so the category pad can draw on
// the vanilla page (group -1) too, where liveViewInfo() returns false.
// Fallback when no capture has completed: the captured window's own rect width / 438 (the record
// plate width).  Returns 0 when neither is available - and 0 means DRAW NOTHING.  It must NEVER
// fall back to 1.0: a pad drawn at 1.0 over a 0.7 page lands 43% too far right, on the boxes.
float liveUiScale();

// Asks GraphicsEngine::GetUIScaleFactor ONCE per HUD build and logs how it compares with the
// filler-box measurement. GAME THREAD ONLY (it makes engine calls); liveUiScale() itself never
// calls the engine, so the click path stays call-free.
void liveUiScaleRefresh();

// The number of catalogue entries in a group, or -1.
int liveGroupEntries(int group);

// The label of an arbitrary group (-1 = the vanilla Crafting Materials page). Never null.
const char* liveGroupLabel(int group);

// Absolute group selection - the Ctrl+wheel semantics with an explicit index.  -1 = the vanilla
// layout, 0..23 = a collection group; anything else is refused and nothing changes.  Writes the
// same two ints Ctrl+wheel writes and calls markDirty(); NO engine call, so it is safe from
// inside a detour body.  The next GAME-THREAD liveTick runs applyLayout exactly as it does for
// Ctrl+wheel.  Deliberately does NOT call inputAllowed(): the caller (the mouse detour) has
// already proven the Materials page is visible and that the click landed in a button rect.
bool liveSelectGroup(int group);

// The group liveSelectGroup last ASKED for (-1 = the vanilla page), i.e. what the page is about
// to show. liveShownGroup() only moves when applyLayout runs on the next GameEngine::Update, so
// the pad must advance from this one or two clicks inside one tick pick the same target.
int liveWantedGroup();

// ---- the owned-only filter ----------------------------------------------------------------------
// The page shows only the records the reagent map already holds; every other box is parked
// off-grid, which also hides it from the GAME'S own search (an empty box has no item id, so
// UpdateSearch puts its rect in neither the matched nor the not-matched list).
// The live value is a MOD-OWNED latch, not `g_cfg.ownedOnly`: configReload replaces g_cfg once a
// second, so a click that wrote only there would be undone within a second.  The latch is seeded
// from the ini once and written back to it through the same debounced path uniq_group/uniq_row
// use.  Nothing here touches the engine, so both are safe from inside a detour body.
bool liveOwnedOnly();

// Flips it, resets the first visible row to 0 IN THE SAME CALL (the rows change units when the
// filter flips), asks for one relayout of the group already on screen and marks the ini dirty.
// False when live paging is not armed - the caller must then NOT consume the click.
bool liveToggleOwnedOnly();

// Which collection group holds this ITEM record (the key the reagent map uses), or -1 when no
// group does / the group file never loaded. The lookup table is built once, on first use, from
// the loaded groups; the query is normalised (lowercased, backslashes turned into forward
// slashes) exactly the way a reagent-map key is, so a record copied straight out of a map node
// can be handed in unchanged. No engine call, no lock - just a hash lookup.
int liveGroupOfItem(const char* record);

// ---- the group table ----------------------------------------------------------------------------
// How many collection groups the loaded uniq-groups.txt has (0 before it loads, 24 today).
int liveGroupCount();

// Entry `index` of group `group`: its BOX record (the `uniq_b*` display record the mod's own
// display prototype is built from - this is the key `liveCachedProtoId` wants) and its ITEM
// record (the real `records/items/...` path - the key the reagent map and catalogue.bin use).
// False past the end; either string may be empty on an old uniq-groups.txt. The pointers are
// into data filled once at load and never mutated, so they live for the process.
bool liveGroupRecordAt(int group, int index, const char** boxRec, const char** itemRec);

// The display prototype's object id for a BOX record, or 0 when this record has not been shown in
// this world yet. GAME THREAD ONLY (the cache is filled by showBox there and cleared on a world
// teardown).
unsigned int liveCachedProtoId(const char* boxRecord);

const char* liveStatus();

// ---- the box capture ----------------------------------------------------------------------------
// Bumped once every time liveBeginCapture() opens a box capture, i.e. exactly once inside the
// MATERIAL ReagentWindow's own Load, and taken back by liveCancelCapture(). ut_plate.cpp brackets
// the original ReagentWindow::Load call with it to tell the two reagent windows apart without a
// string compare; a window built from the VANILLA record because the substitution was rolled back
// leaves the epoch where it was, so the plate never claims it.
long liveCaptureEpoch();

// Everything the on-screen label needs, as one snapshot taken on the render thread. `label`
// points at a string that lives for the process. Returns false when no collection group is on
// screen (the vanilla layout, live paging off, or the boxes not captured yet) - the label must
// then draw nothing at all.
struct LiveView {
    int group;          // >= 0 always when the call returns true
    const char* label;
    int firstRow;       // 1-based, inclusive
    int lastRow;        // 1-based, inclusive
    int totalRows;
    int entries;        // records in this group
    int shown;          // how many of them the last layout really showed
    int filtered;       // 1 = the owned-only filter was applied to that layout
    int owned;          // of them, present in the engine's reagent map (-1 = unknown)
    int ownedAll;       // records the map holds in total (-1 = unknown)
    int collection;     // records in the whole collection
    int cellW, cellH;
    float scale;        // the measured UI scale
};
bool liveViewInfo(LiveView* out);

// The DISTINCT (cellW, cellH) pairs the loaded groups use, in FIRST-APPEARANCE order (the loop
// keeps group order, it does not sort - for the shipped uniq-groups.txt that is 64x64, 64x96,
// 64x32, 32x32, 32x96, 64x128, 32x128). Writes at most `cap` pairs into w[]/h[] and returns how
// many exist (which can exceed `cap`). 0 = the group file has not been read. ut_plate.cpp uses
// it to probe every plate texture once, at the main menu, so the install can be verified without
// loading a character.
int liveCellSizes(int* w, int* h, int cap);

// The box widgets captured during the last material `ReagentWindow::Load`, in build order.
// Writes at most `cap` of them and returns how many were captured (which can exceed `cap`).
// ut_plate.cpp uses element 0 to prove that the window it captured really owns those boxes
// (they must be inside the window's own `std::vector` at window+0x380) before it writes
// anything into that window. Game thread only, valid until the next liveBeginCapture().
int liveCapturedBoxes(void** out, int cap);

}  // namespace ut
