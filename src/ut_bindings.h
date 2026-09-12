// ut_bindings.h - EVERY fact this mod knows about the game's memory, in one place, with the way
// it is obtained and the way it is confirmed.
//
// WHY THIS FILE EXISTS. Nothing in the mod is allowed to assume a game version. The exe carries
// no usable version at all (its FileVersion resource reads 0.3.0.0 on 1.3.0.8), so "is this the
// build I was written for?" cannot be asked and must not be asked. What is asked instead is a
// CAPABILITY question, one binding at a time: can this address still be found, and does what was
// found still look like the thing it is supposed to be? A binding that answers yes works on any
// build that kept the shape; a binding that answers no turns the WHOLE mod off before a single
// hook is installed, because a half-bound mod is the one thing that could damage a save.
//
// THE FIVE CLASSES, from safest to most fragile:
//
//   EXPORT      GetProcAddress by decorated name in Game.dll / Engine.dll. Survives any patch
//               that keeps the name - which is every minor patch, because the names come out of
//               the compiler, not out of a table somebody maintains. gd_exports*.h holds ZERO
//               addresses; there is nothing in them to go stale.
//   SIGNATURE   a byte pattern scanned in a module's .text at run time. Carries no address of
//               its own, but it does carry the compiler's instruction selection, so it survives
//               a data patch and does not survive a recompile of that function. Every pattern
//               here is proved to match EXACTLY ONCE (twice for the take pair, which is two
//               sites in one handler) in the 1.3.0.8 image by tools/test_bindings.cpp, and the
//               run-time scan re-counts the matches and refuses anything but the expected count.
//   DECODED     an offset or a call target read out of an EXPORTED function's own instruction
//               bytes (`movzx eax,[rcx+disp32]; ret` and friends). The name anchors it, so it
//               survives everything an export survives plus anything that only moves the field.
//   STRUCTURAL  a layout the COMPILER guarantees, not the game: MSVC's std::map node, MSVC's
//               std::string. These are documented, never "converted" - there is nothing to scan
//               for and nothing that could drift while the game is still built with MSVC.
//   LITERAL     a number compiled in. Every one that is left is listed below WITH ITS REASON and
//               WITH THE RUN-TIME TEST that has to pass before it is used. None of them is a
//               code address any more; they are field offsets inside objects the mod already
//               proved it is holding (the window whose vtable slot +0x18 is the function the
//               signature scan found, and so on).
//
// ---------------------------------------------------------------------------------------------
// THE INVENTORY. One row per binding: what it is / how it is obtained / how it is confirmed.
// The 1.3.0.8 column is EVIDENCE, never a gate - it is what the offline test asserts, and the
// mod itself never compares against it.
// ---------------------------------------------------------------------------------------------
//
// == EXPORT (counted, not listed: the list is gd_exports.h / gd_exports_extra.h /
//            gd_exports_reagent.h, and resolveExports() logs one line per symbol) ==
//   every Game.dll / Engine.dll function and the static data exports.
//     obtained    GetProcAddress(module, "<decorated name>")
//     confirmed   a non-null address inside the module that was asked; a REQUIRED symbol that
//                 comes back null already turns the mod off (gd_runtime.cpp), and that rule is
//                 now folded into this gate so the count is reported with the others.
//
// == SIGNATURE (exe .text; the exe is Steam-DRM encrypted until its stub has run, so every one
//               of these is scanned from the GAME THREAD, retried, never from DllMain) ==
//   exe.ReagentWindow::Load            1.3.0.8 rva 0x132100   ut_plate.cpp
//     obtained    45-byte prologue, no RIP-relative bytes in it
//     confirmed   exactly one match in .text AND the live window's vtable slot +0x18 equals it
//   exe.UIReagentItem::Load            1.3.0.8 rva 0x1F0660   ut_live.cpp
//   exe.UIReagentItem::SetItem         1.3.0.8 rva 0x1EE2E0   ut_live.cpp
//   exe.UIReagentItem::SetPos          1.3.0.8 rva 0x1EE830   ut_live.cpp
//     obtained    prologue patterns (SetPos is the whole 12-byte leaf function)
//     confirmed   one match each AND each address occupies its own slot (+0x18 / +0xA8 / +0xB8)
//                 in a real box widget's vtable
//   exe.reagent-take replica pair      1.3.0.8 rva 0x132B4F and 0x132CC1   ut_reagent.cpp
//     obtained    the `mov rax,[rsi]; lea rdx,[rbp-0x30]; mov rcx,rsi; call [rax+0x590]` shape
//     confirmed   exactly TWO matches, less than 0x400 bytes apart (one handler)
//   exe.deposit site A                 1.3.0.8 rva 0x1EAB2A   ut_reagent.cpp   ** WAS LITERAL **
//     obtained    42 bytes with the four call displacements wildcarded; the accepted return
//                 window is sig+6 .. sig+0x1B, which is the byte range that used to be written
//                 as 0x1EAB30..45
//     confirmed   exactly one match in .text
//   exe.deposit site B                 1.3.0.8 rva 0x1EC64F   ut_reagent.cpp   ** WAS LITERAL **
//     obtained    the same shape with `mov edx,r14d` instead of `mov edx,[rbp+0x208]`, which is
//                 what makes the two distinguishable; window sig+1 .. sig+0x16 (= 0x1EC650..65)
//     confirmed   exactly one match in .text; label only - site B is the EQUIPMENT quick-move
//                 and the table gate refuses it whatever this says
//   gamedll.dragSite                   1.3.0.8 Game.dll rva 0x173960   ut_reagent.cpp  ** WAS
//                                                                                    LITERAL **
//     obtained    61 bytes with the three call displacements wildcarded, ending on the
//                 `mov [rdi+0x30],ebp` that clears the cursor slot - which is the instruction
//                 that makes the drag's removal unconditional and so belongs inside the proof.
//                 The accepted return window is sig+0x0D .. sig+0x20, the byte range that used
//                 to be written as 0x17396D..0x173980.
//     confirmed   exactly one match in Game.dll's .text. THIS ONE IS EARLY: Game.dll is plain on
//                 disk, so it is resolved before any hook is installed and the all-or-nothing
//                 gate covers it - no drag deposit is possible at all unless it was found.
//
//   With those three converted, NO EXE OR DLL CODE ADDRESS IS COMPILED INTO THE MOD ANY MORE.
//
// == DECODED (out of an exported function's own bytes, at export-resolution time) ==
//   Item + craftingMaterial   0xC64   from Item::IsReagentCompatible  `0F B6 81 <d32> C3`
//     confirmed   non-zero and < 0x2000; the gate refuses to arm without it
//   Item + soulbound          0xC00   from Item::IsSoulbound          same shape
//   Item + untradeable        0xC02   from Item::IsUntradeable        same shape
//     confirmed   both non-zero; second, independent confirmation: they must differ by 2 and sit
//                 below craftingMaterial, which is the order the exe's own drag test reads them
//   Item + ItemReplicaInfo    0x538   from Item::GetItemReplicaInfo   `48 8D 91 <d32>`
//   sizeof(ItemReplicaInfo)   0x190   from TWO sources (ut_replicasize.h): Item::Item's two
//                                     consecutive `lea rcx,[rdi+d32]; call` member constructions
//                                     (the member after the block pins its end) and the largest
//                                     destination store in ItemReplicaInfo::operator=, reached
//                                     through Item::GetItemReplicaInfo's own tail jmp
//     confirmed   both decoded, equal, 8-aligned, 0x100..0x200; the capture, the journal and the
//                 take all use this size, and a mismatch is one ERROR, never a fallback to 0x190
//   Item + seedRerolls 0x6B8 / affixRerolls 0x6B4 from their `8B 81 <d32> C3`
//     confirmed   each lands inside [replica, replica+size)
//   Item + prefixClass 0x880 / suffixClass 0x884 from the same shape        ADVISORY
//     confirmed   non-zero, below 0x2000, and exactly four bytes apart - these two are ITEM
//                 fields above the replica block; nothing in the mod reads either offset, so a
//                 failure is one WARN and cannot turn the mod off
//   Item vtable IncrementStack slot 0x5F8  from AddItemToReagents+0x1FF `FF 97 <d32>`   ADVISORY
//     confirmed   a plausible slot (8-aligned, < 0x2000); 0 when not decoded. Its only reader is
//                 a diagnostic field, so there is no fallback and no gate
//   Item + stack mirror               from Item::SetStackSize's own store
//     confirmed   lands inside [replica, replica+size)
//   Player + ctrlId           0x16C0  from CursorHandler::GetPlayerCtrl's `mov r32,[rax+<d32>]`
//     confirmed   non-zero; the bag proof refuses to arm without it
//   ObjectManager::ObjectFromId        from TakeItemFromReagents(id,int)+0x2C `E8 <rel32>`
//     confirmed   the decoded target lands inside Game.dll's own image
//
// == STRUCTURAL (the compiler's layout, not the game's - documented, never scanned) ==
//   MSVC std::_Tree_node: +0x20 the key std::string, +0x40 protoId, +0x44 count.
//   MSVC std::string: 16-byte SSO union, then size, then capacity (capacity < 16 => in place).
//   the box vector's element stride, 16 bytes {widget*, u32 id, bool} - measured from the exe's
//   own `add qword [rsi+8],0x10`, and a std::vector's stride cannot be anything else.
//
// == LITERAL - what is STILL compiled in, and why it may be ==
//   Every one of these is a FIELD OFFSET inside an object the mod has already proved it holds,
//   and every one is read under SEH with a plausibility test. None is a code address.
//   Hud + 0x4FD08 -> CaravanWindow (ut_plate.cpp)
//     why literal  there is no exported Hud accessor that returns it, so there is nothing whose
//                  bytes could be decoded. The mod does not actually NAVIGATE from the Hud: it
//                  finds the CaravanWindow by scanning the caller's own stack frame and then
//                  proves the candidate (pages[3] == the window it was called on, pages[0..2]
//                  non-null and 8-aligned, a readable vtable on pages[2]). The number is a
//                  documented landmark, not a route.
//     confirmed    the four-test candidate probe above, every time
//   CaravanWindow + 0x13C8 pages[4], + 0x1728 visible page index
//     why literal  a plain array in a window the mod holds; no export exposes it
//     confirmed    pages[3] must be the ReagentWindow the detour was called on; the index must
//                  be 0..3
//   ReagentWindow + 0x380 box vector, + 0x40 origin, + 0x480 plate widget, + 0x4B0 texture,
//                  + 0x4C0/+0x4C4 destination size, + 0x4E8/+0x4F0 the search match range
//   UIReagentItem + 0x30 prototype id, + 0x6C local position, + 0x80 parent origin
//     why literal  measured from the exe's own code in the DIGs; the only functions that touch
//                  them are exe-internal, so there is no export to decode
//     confirmed    the vector's (last-first) must be a multiple of the 16-byte stride and under
//                  the 8192 ceiling; the widget pointers must be readable and carry the vtable
//                  the located UIReagentItem::Load sits in
//   vtable SLOT NUMBERS +0x18 Load, +0x20 Draw, +0x38 mouse, +0x118 UpdateSearch,
//                       +0x128 SetPage, +0xA8 SetItem, +0xB8 SetPos
//     why literal  a slot index is not derivable from anything; it IS the binding
//     confirmed    CROSS-CHECKED, always, in the same SEH frame: slot +0x18 of the live object
//                  must equal the function the signature scan found, and only then are the other
//                  slots in that vtable believed. A mismatch turns that route off.
//   GameEngine + 0x375BA (the save-variant byte)                                    ADVISORY
//     why literal  log line only - it is printed beside GameInfo::GetHardcore so the two can be
//                  compared in a log, and nothing decides anything on it
//     confirmed    not gated at all; -1 with a reason when it cannot be read
//   sizeof(GameTextLine) 0x40 (ut_tooltip.cpp)
//     why literal  a data structure size every consumer in the engine divides by; the tooltip
//                  route is optional and turns itself off on any inconsistency
//   the text style class numbers 0x2C / 0x4A - DATA, not code: they are ids in the game's own
//     text-style table, exactly like a record path, and are left as they are by design.
//
// ---------------------------------------------------------------------------------------------
#pragma once

#include <windows.h>

#include <stddef.h>

namespace ut {

// ---- the pure scanner ------------------------------------------------------------------------
// Wildcards are a parallel mask array: mask[i] == 0 means "any byte here". Kept free of Windows
// and of the mod's own state on purpose, so tools/test_bindings.cpp runs EXACTLY this code over a
// decrypted image file offline.  Returns how many matches were found, capping the stored hits at
// `maxHits` but counting up to `maxHits + 1` so "more than expected" is always reportable.
struct UtBindPattern {
    const char* name;
    const unsigned char* bytes;
    const unsigned char* mask;  // 1 = must match, 0 = wildcard
    size_t len;
    int expectHits;             // how many matches the pattern MUST have
    size_t rva1308;             // the 1.3.0.8 rva of the first hit - evidence for the test only
};

inline int utBindScan(const unsigned char* lo, const unsigned char* hi, const UtBindPattern& p,
                      const unsigned char** hits, int maxHits) {
    if (!lo || !hi || hi <= lo || p.len == 0 || (size_t)(hi - lo) < p.len) return 0;
    const unsigned char first = p.bytes[0];
    const bool firstFixed = p.mask[0] != 0;
    int count = 0;
    for (const unsigned char* q = lo; q + p.len <= hi; ++q) {
        if (firstFixed && *q != first) continue;
        size_t j = 0;
        for (; j < p.len; ++j) {
            if (p.mask[j] && q[j] != p.bytes[j]) break;
        }
        if (j != p.len) continue;
        if (count < maxHits) hits[count] = q;
        if (++count > maxHits) break;
    }
    return count;
}

// The three deposit call sites, converted from RVA literals in this work package. Defined in
// ut_bindings.cpp so the test and the mod share one copy of the bytes.
extern const UtBindPattern kUtSigDepositSiteA;
extern const UtBindPattern kUtSigDepositSiteB;
extern const UtBindPattern kUtSigDragSite;  // Game.dll, not the exe: plain on disk, scanned early
// The accepted RETURN-ADDRESS window inside each, as an offset from the start of the match. The
// engine's call is inside the pattern, so these are fixed by the pattern, not by the build.
const size_t kUtSiteAWindowLo = 6;     // 0x1EAB2A + 6   == the old 0x1EAB30
const size_t kUtSiteAWindowHi = 0x1B;  // 0x1EAB2A + 0x1B == the old 0x1EAB45
const size_t kUtSiteBWindowLo = 1;     // 0x1EC64F + 1   == the old 0x1EC650
const size_t kUtSiteBWindowHi = 0x16;  // 0x1EC64F + 0x16 == the old 0x1EC665
const size_t kUtDragWindowLo = 0x0D;   // 0x173960 + 0x0D == the old 0x17396D (Game.dll)
const size_t kUtDragWindowHi = 0x20;   // 0x173960 + 0x20 == the old 0x173980

// ---- the registry ----------------------------------------------------------------------------
enum UtBindClass {
    UT_BIND_EXPORT = 0,
    UT_BIND_SIGNATURE = 1,
    UT_BIND_DECODED = 2,
    UT_BIND_STRUCTURAL = 3,
    UT_BIND_LITERAL = 4,
    UT_BIND_CLASS_COUNT = 5,
};

// EARLY bindings must all be resolved and confirmed before ANY hook is installed. LATE ones need
// the exe's .text, which the Steam stub only decrypts once the game is running, so they are
// resolved from the game thread; each one already turns its own route off on failure, and the
// summary line says which of them have reported.
enum UtBindPhase { UT_BIND_EARLY = 0, UT_BIND_LATE = 1 };

// CRITICAL rows have a consumer: some decision in the mod reads the value, so a failure turns the
// mod off. ADVISORY rows are decoded, confirmed and reported like the others, but nothing reads
// them, so a failure is one WARN naming the row and the gate still passes. Orthogonal to the
// class and to the phase; the offline test asserts the flag row by row.
enum UtBindGate { UT_BIND_CRITICAL = 0, UT_BIND_ADVISORY = 1 };

// Register one binding's outcome. `value` is the offset/slot/rva that was obtained (0 when the
// binding is not a number), `ok` is whether it RESOLVED AND CONFIRMED, and `why` is what the
// confirmation expected - it is what the ERROR line prints when ok is false. Names are compared
// by pointer-free strcmp against the compiled-in table, so a typo is reported, never silent.
void bindingsNote(const char* name, unsigned long long value, bool ok, const char* why);

// EARLY gate. Logs the module identity (size + PE timestamp of the exe, Game.dll and Engine.dll -
// for the record, NEVER as a gate), one compact INFO line with the counts per class, and the full
// table at DEBUG. Returns false when any EARLY binding failed, and the caller must then install
// nothing at all.
bool bindingsGate(int resolvedExports, int missingExports);

// The LATE half: called from the game thread whenever an exe-side binding reports. Emits the
// "all late bindings confirmed" INFO line once, or one ERROR naming the first failure.
void bindingsLateReport();

// For the status/heartbeat line: "61 by export, 7 by signature, 13 decoded (3 advisory),
// 3 structural, 15 literal (1 advisory) - all confirmed".
const char* bindingsSummary();

// The CRITICAL row the last bindingsGate refused on ("" when it passed).
const char* bindingsGateFailure();

// The registry itself, read only, so tools/test_bindings.cpp can assert the
// CLASSIFICATION and not just the outcomes. The gate treats an EARLY row that has not reported as
// a failure - which is right - so the set of EARLY rows must stay exactly the set of bindings that
// are obtainable WITHOUT the game thread. A row that quietly became lazily-decoded while staying
// EARLY would turn the mod off on a healthy game, and that is what the offline assertion catches.
// Returns false when `i` is out of range; any out pointer may be null.
int bindingsRowCount();
bool bindingsRowAt(int i, const char** name, int* cls, int* phase, int* gate = nullptr);

}  // namespace ut
