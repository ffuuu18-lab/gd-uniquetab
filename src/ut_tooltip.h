// ut_tooltip.h - the item rollover.
//
// Two independent user-visible things, one file, because they share the same widget fields and
// the same in-game test:
//
//  1. THE COMPARE POPUP - ONE BYTE, no detour, no engine call.  The shared item-box rollover
//     (exe 0x1EE880) skips the whole "Currently Equipped" comparison block unless the hovered
//     widget's `+0x7E` is 1 (`cmp byte [r12+0x7e],0` at exe 0x1EE9A4), the base item-box
//     constructor leaves that byte 0 (`mov word [rcx+0x7d],1` at 0x1EDA8A writes +0x7D and
//     +0x7E together, 1 and 0), and the two containers that DO set it (0x1E9E17, 0x21F8E8)
//     never see a `UIReagentItem`.  The caravan's mouse handler copies the hovered box's +0x7E
//     verbatim into its rollover mirror (`movzx ecx,[rax+0x7e]` / `mov [r14+0x41e],cl` at
//     0x1330EB/0x1330EF, and 0x41E - 0x3A0 = 0x7E), so writing the byte on the mod's own boxes
//     is the whole fix.  `tooltipCompareBox` does that, from `ut_live.cpp`'s `showBox`, on the
//     game thread, behind the same per-probe SEH the neighbouring writes use.
//     NEVER on the vanilla crafting-materials page: base `Item`'s vtable +0x550 (the equip-slot
//     type the partner test compares) is a folded stub, so an Aether Crystal could otherwise
//     acquire a nonsense compare partner.
//
//  2. THE "ALREADY COLLECTED" LINE - FIVE exported Game.dll detours over SIX resolved exports,
//     all-or-nothing.  `Item::GetUIDisplayText` (+ the `ItemEquipment` override, which calls the
//     base at Game.dll 0x3299AF and then APPENDS - so the base's latch would otherwise be of a
//     partial vector - and the `ItemArtifact` / `ItemRelic` overrides, which do not call the base
//     at all) LATCHES {which state, when} after the original has filled the line vector, and
//     `GameTextLineToString` SWAPS in a borrowed array of N+1 `GameTextLine` records - the
//     engine's N, byte for byte, plus one mod-owned static line - for the duration of that one
//     call.  The mod never pushes into the engine's vector (it may be at capacity, and the
//     engine destroys what it holds) and never draws a pixel: the engine lays the extra line
//     out exactly as it lays out its own.
//
// Both are behind their own ini key (`compare_popup`, `tooltip_mark`, both default 1) and the
// tooltip half is behind one more session latch: any failure to resolve an export or install a
// detour turns the whole mark OFF for the session with one capitalised log line.  The compare
// byte is INDEPENDENT of that latch - it needs no export and no detour.
#pragma once

#include <windows.h>

#include <stddef.h>

namespace ut {

// Worker thread, once, before hooksInstall(). Resolves the SIX Game.dll exports - the four
// `GetUIDisplayText` overrides that get a detour (Item, ItemEquipment, ItemArtifact, ItemRelic),
// `GameTextLineToString` (the fifth detour) and the `GameTextLine` constructor, which is CALLED
// and never hooked - all OPTIONAL (a miss can never fail start-up, it only turns the mark off),
// checks none of them is a folded stub, and builds the two static GameTextLine records. Safe to
// call twice.
bool tooltipInit(HMODULE selfModule);

// Worker thread, from hooksInstall(). Installs the tooltip detours ALL-OR-NOTHING: every hook is
// created first, and only if every create succeeded are they enabled; otherwise the created ones
// are removed again and the feature is off for the session. Returns how many were installed and
// writes how many were attempted into `total`. DELIBERATELY NOT counted in the mod's
// `detours installed: N of N` line - a missing tooltip export must not fail the menu test.
int tooltipInstall(int* total);

// GAME THREAD, from ut_live.cpp's `showBox` only, once per box per relayout.
//   box        - the captured UIReagentItem widget the caller has just SetItem/SetLocalPosition'd
//   index      - the box's index in this relayout pass. It must INCREASE within a pass: an index
//                that does not advance is what marks the start of the next one.
//   collection - true when the box is showing a COLLECTION group; false on the vanilla
//                crafting-materials page, where the byte must stay 0
// Writes `box[+0x7E] = collection && compare_popup` behind its own per-probe SEH. No engine call,
// no allocation, no lock. A fault disables the compare byte for the session and says so once.
void tooltipCompareBox(void* box, size_t index, bool collection);

// Prints the one per-world `tooltip: compare flag set on N of M boxes` line as soon as a relayout
// pass has finished, and resets the counters when a new HUD is built. Called from the start of a
// pass (see `index` above) and from the tooltip latch - a rollover can only be built after a
// relayout has ended, so the line appears even in a world with exactly one relayout. Two
// process-local reads; no engine memory, no lock. GAME THREAD, and that is now enforced: the
// first thread to reach the counters owns them and any other is ignored after one log line.
void tooltipCompareFlushPending();

// One line for the log / the menu test. Never touches engine memory.
const char* tooltipStatus();

// ---- the swap, exposed so the offline harness drives the SAME code the game drives -------------
// The borrowed vector handed to GameTextLineToString's trampoline: mem::vector's
// {begin, end, capacityEnd}, over an array this mod owns for the duration of one call.
struct UtTooltipSwap {
    const unsigned char* begin;
    const unsigned char* end;
    const unsigned char* cap;
};

// Builds an array of N+1 0x40-byte GameTextLine records - the engine's N copied byte for byte,
// then `extraLine` - and fills `out` with a triple over it. `*ownedOut` is the array, to be given
// back to tooltipSwapFree. Returns false, having allocated nothing, for any vector whose shape is
// not a plausible tooltip (unreadable, not a multiple of 0x40, empty, or over 512 lines).
bool tooltipSwapBuild(const void* vec, const unsigned char* extraLine, unsigned char** ownedOut,
                      UtTooltipSwap* out);
void tooltipSwapFree(unsigned char* owned);

// Borrowed arrays alive right now. Zero at rest - the harness asserts exactly that.
long tooltipSwapAllocBalance();

}  // namespace ut
