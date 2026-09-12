// ut_tooltip.cpp - the item rollover: the "already collected" tooltip line and the compare byte.
// See ut_tooltip.h for what the two halves are.
//
// EVERY address and every constant below was verified offline against the decrypted exe image.
// The short version:
//
//   * `box + 0x7E` is the "participates in equipment comparison" flag. The base item-box ctor
//     writes `mov word [rcx+0x7d],1` at exe 0x1EDA8A - i.e. +0x7D = 1 and +0x7E = 0 - the object
//     is 0xE0 bytes (`mov ecx,0xE0` at exe 0x1E9DA8, the allocation right before that ctor), and
//     the rollover at exe 0x1EE880 skips its whole compare block on `cmp byte [r12+0x7e],0`
//     (0x1EE9A4). The caravan copies the hovered box's byte into its own rollover mirror at
//     0x1330EB/0x1330EF. So 0x7E < 0xE0 is INSIDE the widget and one byte is the whole fix.
//
//   * `GameTextLine` is 0x40 bytes: +0x00 u32 GameTextClass, +0x08 basic_string<unsigned short>
//     (buf 0x10 / size at +0x18 / capacity at +0x20), +0x28 bool, +0x30 const GraphicsTexture*,
//     +0x38 float. Decoded from the exported ctor's own bytes (Game.dll 0x2FEC10): `mov [rcx],edx`,
//     `mov [rcx+0x20],7` + `mov [rcx+0x18],0` + `mov word [rcx],0` (a _Tidy_init of the string in
//     place - it never READS the destination, so a zeroed buffer is a legal `this`), the assign
//     call, `mov [rdi+0x28],bl`, `mov [rdi+0x30],rax`, `movss [rdi+0x38],xmm0`.
//
//   * `GameTextLineToString` (Game.dll 0x302A90) touches the vector object at `[rsi]` and
//     `[rsi+8]` ONLY (`mov rbx,[rsi]` 0x302ACA, `cmp rbx,[rsi+8]` 0x302ACD / 0x302E09,
//     `add rbx,0x40` 0x302D50) and writes nothing. That, and the fact that the exe deep-copies
//     every line before anything is destroyed, is what makes a BORROWED {begin,end,end} triple
//     over a mod-owned array safe. The mod never pushes into the engine's own vector: it may be
//     at capacity, and whatever is pushed the ENGINE destroys.
//
// RULE 9. Both detour bodies contain an engine call - their own trampoline - and neither is
// wrapped in a swallowing frame. All mod work happens OUTSIDE the trampoline call; the borrowed
// array is released in a `__finally`, which is cleanup, not a handler, so an engine C++ exception
// keeps unwinding exactly as it would have. The `__except(EXCEPTION_EXECUTE_HANDLER)` frames in
// this file protect MOD-SIDE READS of engine memory only (the replica probe, the vector shape,
// the byte write), and each of them raises `reagentProbeDepth` so dllmain's vectored handler
// stays silent for a fault the mod owns.
//
// RULE 7 (liveness) is satisfied structurally on the tooltip path: the detour runs inside the
// item's own method, so `this` is alive by construction - there is no stored object id to
// re-check and no window between a check and a use. The compare byte is written from `showBox`,
// immediately after that same box has survived the engine calls the relayout makes on it.
//
// MULTIPLAYER: nothing here leaves the machine. Both functions are pure local UI text, no packet
// type, no replication, no shared object mutated - so this is deliberately NOT behind mpBarred().

#include "ut_tooltip.h"

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "MinHook.h"
#include "gd_exports_reagent.h"
#include "ut_config.h"
#include "ut_live.h"
#include "ut_log.h"
#include "ut_plate.h"
#include "ut_reagent.h"
#include "ut_rescue.h"

namespace ut {
namespace {

// ---- the two GameTextClass values, with their evidence ---------------------------------------
// GameEngine::LoadFromDatabase registers every class with its UI style name; each registration is
// `lea rdx,[rip -> "<StyleName>"]` followed by `mov dword [rbp+..],<class>`.
//
//   0x12 = "ItemBaseStats"  (registered at Game.dll 0x2FFF09). It is the class
//          `Item::GetUIDisplayText` uses for its OWN annotation lines - the Soulbound line
//          (push_back at Game.dll 0x311A6F) and the Untradeable line (0x311B5F) - and that
//          `ItemEquipment::GetUIDisplayText` and `..._DPS` use as well. A mod line in this class
//          therefore reads exactly like an engine annotation on the same tooltip.
//
//   0x50 = "GrayStats1"     (registered at Game.dll 0x3024AE). `GameTextLineToString` does
//          `mov eax,[rbx]; sub eax,0x50; cmp eax,3; ja` at 0x302B1F-0x302B25 and, on the
//          not-taken branch, assigns the literal "desaturate" (0x302B6C) into the GameTextString
//          it builds. 0x50..0x53 are GrayStats1..4 - the greyed band - and 0x50 is the first.
//
// 0x14 ("ItemRequirements") and 0x23 ("ItemDirections") are the two classes the exe's own filter
// pass drops when `widget+0xD8` is set, so neither was eligible even though the dig proved +0xD8
// is always 0.
// ---- the class table, and why the defaults are what they are ----------------------------------
// The line should be coloured slightly, never brightly.  Choosing a colour here means choosing a CLASS - `GameTextLineToString` looks the
// class's style name up through `GameEngine::GetGameTextStyleName` (Game.dll 0x2E1600) and reads
// the colour off the style map node at +0x48, so the mod still never draws and never invents an
// RGB of its own.
//
// The table below is MEASURED, not typed.  `tools\census_textclass.py` walks Game.dll
// 0x2FD000..0x304000 for the registration pattern (`lea rdx,[rip -> "<StyleName>"]` then
// `mov dword [rbp+..],<class>`) and finds 84 classes, 0x01..0x54 with no gaps; then
// `tools\find_textstyles.py` resolves each style name as a FIELD of `records/game/gameengine.dbr`
// and reads `fontColor0.R/G/B` off the style record that field names (`colours` mode).  Both
// scripts are in `tools\` and both re-run offline against the shipped game; the census needs its
// two range arguments (`python census_textclass.py 0x2FD000 0x304000`) and each script's own
// docstring gives its exact usage.
//
// The two defaults, with their evidence:
//   0x2C ItemEnchantmentStats -> records/ui/styles/text/style_nooutline_textolive_sizet.dbr
//        (0.57, 0.797, 0.00) = RGB (145,203,0), font size 15.  COLLECTED.  A mild olive green -
//        the colour the game already uses for component text, so the line still reads as an
//        engine annotation the way class 0x12 did, only in green.
//   0x4A EffectHeading        -> records/ui/styles/text/style_skilleffecttitle.dbr
//        (0.88, 0.60, 0.24) = RGB (224,153,61), font size 20.  NOT COLLECTED, and THE LEAST-BAD
//        OPTION rather than a good one.  Of all 84 classes exactly four are red or orange, and
//        the other three are the item-NAME band: 0x09/0x0F ItemNamePotion (255,66,0) and
//        0x0A/0x1D ItemRelicName (242,163,77) at font size 21-23, which is TITLE size, and 0x34
//        SkillRequirementsNotMet, a pure (255,0,0) bold red - exactly the bright red to avoid.  THIS ENGINE HAS NO MILD RED AT BODY SIZE.  0x4A's one wart is its size: 20
//        against the 17 of the stat lines it follows.
// Both are outside 0x50..0x53, so `GameTextLineToString`'s "desaturate" branch (the test at
// 0x302B1F, the literal at 0x302B6C) does NOT fire - desaturation is off, as asked.
//
// The plain pair stays reachable from the ini: `tooltip_class_yes=18` (0x12 ItemBaseStats,
// white - the class Item::GetUIDisplayText uses for its own Soulbound line, push_back at
// 0x311A6F) and `tooltip_class_no=80` (0x50 GrayStats1, grey AND desaturated).
//
// 0x14 ("ItemRequirements") and 0x23 ("ItemDirections") remain INELIGIBLE: they are the two the
// exe's own filter pass drops when `widget+0xD8` is set (even though the dig proved +0xD8 is
// always 0).
struct TextClassInfo {
    unsigned int cls;
    const char* style;
    int r, g, b;    // fontColor0 of the style record, 0..255; -1 = that record carried no colour
};

const TextClassInfo kTextClasses[] = {
    {0x01, "DefaultShort", 255, 255, 255},
    {0x02, "ItemBanner", 255, 255, 255},
    {0x03, "ItemNameCommon", 255, 255, 255},
    {0x04, "ItemNameMagical", 242, 229, 26},
    {0x05, "ItemNameRare", 64, 242, 77},
    {0x06, "ItemNameEpic", 51, 140, 206},
    {0x07, "ItemNameLegendary", 166, 56, 255},
    {0x08, "ItemNameBroken", 178, 178, 178},
    {0x09, "ItemNamePotion", 255, 66, 0},
    {0x0A, "ItemNameRelic", 242, 163, 77},
    {0x0B, "ItemNameEnchantment", 145, 203, 0},
    {0x0C, "ItemNameQuest", 178, 153, 204},
    {0x0D, "ItemNameArtifactFormula", 242, 163, 77},
    {0x0E, "ItemNameArtifact", 0, 255, 255},
    {0x0F, "ItemNameScroll", 255, 66, 0},
    {0x10, "ItemNameLore", 194, 176, 198},
    {0x11, "ItemDescription", 191, 176, 140},
    {0x12, "ItemBaseStats", 255, 255, 255},
    {0x13, "ItemBonuses", 212, 204, 172},
    {0x14, "ItemRequirements", 153, 153, 153},
    {0x15, "ItemSetName", 79, 189, 191},
    {0x16, "ItemSetDescription", 191, 176, 140},
    {0x17, "ItemSetComponentNotAvailable", 153, 153, 153},
    {0x18, "ItemSetComponentAvailable", 227, 186, 107},
    {0x19, "ItemSetComponentTitle", 227, 186, 107},
    {0x1A, "ItemSetBonuses", 212, 204, 172},
    {0x1B, "ItemSetBonusesSkill", 212, 204, 172},
    {0x1C, "ItemSetBonusesNotAvailable", 153, 153, 153},
    {0x1D, "ItemRelicName", 242, 163, 77},
    {0x1E, "ItemRelicNameDisabled", 153, 153, 153},
    {0x1F, "ItemRelicNumber", 227, 186, 107},
    {0x20, "ItemRelicDescription", 191, 176, 140},
    {0x21, "ItemRelicBonus", 212, 204, 172},
    {0x22, "ItemRelicCompleteTitle", 64, 242, 77},
    {0x23, "ItemDirections", 153, 153, 153},
    {0x24, "ItemSkillHeading", 227, 186, 107},
    {0x25, "ItemSkillName", 102, 153, 255},
    {0x26, "ItemSkillNameSet", 102, 153, 255},
    {0x27, "ItemSkillDescription", 191, 176, 140},
    {0x28, "ItemSkillDescriptionSet", 191, 176, 140},
    {0x29, "ItemSkillStats", 212, 204, 172},
    {0x2A, "ItemPetSkillStats", 212, 204, 172},
    {0x2B, "ItemEnchantmentName", 145, 203, 0},
    {0x2C, "ItemEnchantmentStats", 145, 203, 0},
    {0x2D, "ItemEnchantmentStatsNotAvailable", 153, 153, 153},
    {0x2E, "SkillName", 102, 153, 255},
    {0x2F, "SkillDescription", 212, 204, 172},
    {0x30, "SkillLevelTitles", 255, 229, 166},
    {0x31, "SkillStatsCurrent", 212, 204, 172},
    {0x32, "SkillStatsNext", 153, 153, 153},
    {0x33, "SkillRequirements", 204, 204, 204},
    {0x34, "SkillRequirementsNotMet", 255, 0, 0},
    {0x35, "PetSkillNameCurrent", 102, 153, 255},
    {0x36, "PetSkillNameNext", 153, 153, 153},
    {0x37, "PetSkillStatsCurrent", 212, 204, 172},
    {0x38, "PetSkillStatsNext", 153, 153, 153},
    {0x39, "ArtifactFormulaTitle", 255, 255, 255},
    {0x3A, "ArtifactFormulaDescription", 191, 176, 140},
    {0x3B, "ArtifactFormulaReagents", 227, 186, 107},
    {0x3C, "ArtifactFormulaReagentAvailable", 151, 129, 95},
    {0x3D, "ArtifactFormulaReagentNotAvailable", 255, 255, 255},
    {0x3E, "ArtifactFormulaCostAvailable", 255, 255, 255},
    {0x3F, "ArtifactFormulaCostNotAvailable", 255, 255, 255},
    {0x40, "ArtifactTitle", 0, 255, 255},
    {0x41, "ArtifactDescription", 191, 176, 140},
    {0x42, "ArtifactClass", 227, 186, 107},
    {0x43, "ArtifactBonusTitle", 227, 186, 107},
    {0x44, "ArtifactBonus", 212, 204, 172},
    {0x45, "ArtifactCraft", 227, 186, 107},
    {0x46, "PetBonusTitle", 227, 186, 107},
    {0x47, "PetBonus", 212, 204, 172},
    {0x48, "PetBonusNextTitle", 153, 153, 153},
    {0x49, "PetBonusNext", 153, 153, 153},
    {0x4A, "EffectHeading", 224, 153, 61},
    {0x4B, "EffectName", 102, 153, 255},
    {0x4C, "EffectDescription", 153, 143, 128},
    {0x4D, "EffectStatsCurrent", 212, 204, 172},
    {0x4E, "EffectStatsNext", 153, 153, 153},
    {0x4F, "Debug", 102, 102, 102},
    {0x50, "GrayStats1", 153, 153, 153},
    {0x51, "GrayStats2", 153, 153, 153},
    {0x52, "GrayStats3", 153, 153, 153},
    {0x53, "GrayStats4", 153, 153, 153},
    {0x54, "ItemUpgrade", 153, 153, 153},
};
const int kTextClassCount = (int)(sizeof(kTextClasses) / sizeof(kTextClasses[0]));

const unsigned int kClassCollectedDefault = 0x2C;     // ItemEnchantmentStats, olive green
const unsigned int kClassNotCollectedDefault = 0x4A;  // EffectHeading, warm orange

// The classes actually used, latched at start-up - the prepared lines become engine-owned
// strings, so these can no more follow a live ini edit than the two texts can.
unsigned int g_classYes = kClassCollectedDefault;
unsigned int g_classNo = kClassNotCollectedDefault;

const TextClassInfo* classInfo(unsigned int cls) {
    for (int i = 0; i < kTextClassCount; ++i) {
        if (kTextClasses[i].cls == cls) return &kTextClasses[i];
    }
    return nullptr;
}

// An UNREGISTERED class is neither a crash nor a refusal by the engine.
// `GameEngine::GetGameTextStyleName`'s map miss falls through to Game.dll 0x2E167B, which builds
// a ZERO-LENGTH std::string (`_Mysize = 0` at +0x10, `_Myres = 0xF` at +0x18, first byte 0) and
// returns it - so the line would simply lose its style and be painted with whatever the no-style
// path uses.  The mod refuses the value itself rather than shipping a colourless line, and says
// so once.
unsigned int pickClass(int fromIni, unsigned int fallback, const char* which) {
    if (fromIni == (int)fallback) return fallback;
    // 0x14 ItemRequirements and 0x23 ItemDirections are REGISTERED
    // classes, so `classInfo` finds them - but they are the two the exe's own filter pass drops
    // when widget+0xD8 is set (the comment at the head of this file), i.e. the mod would build a
    // line the game then throws away and the user would see nothing with no explanation.
    if (fromIni == 0x14 || fromIni == 0x23) {
        logW("tooltip: %s=%d is a class the game drops from an item rollover - the default "
             "0x%02X is kept", which, fromIni, fallback);
        logD("0x14 ItemRequirements and 0x23 ItemDirections are dropped by the exe's own filter "
             "pass when widget+0xD8 is set: the line would be built and thrown away");
        return fallback;
    }
    if (classInfo((unsigned int)fromIni)) return (unsigned int)fromIni;
    logW("tooltip: %s=%d is not a text style this game registers (1..%d) - the default 0x%02X "
         "is kept",
         which, fromIni, (int)kTextClasses[kTextClassCount - 1].cls, fallback);
    logD("an unregistered class has an empty style name, so the line would lose its colour "
         "(%d classes are registered)", kTextClassCount);
    return fallback;
}

const char* const kDefaultTextYes = "In your collection";
const char* const kDefaultTextNo = "Not in your collection";

const size_t kLineSize = 0x40;   // sizeof(GAME::GameTextLine); every consumer divides by 0x40
const size_t kMaxLines = 512;    // a real tooltip is 20-40 lines; anything else is not one
const unsigned int kCompareOff = 0x7E;
const DWORD kLatchMaxAgeMs = 1000;

// ---- MSVC std::string / std::wstring, built or read by hand -----------------------------------
// Both sides use the VC14x runtime (the mod is /MD for exactly this reason). 16-byte SSO buffer,
// then size, then capacity; the pointer form is used once capacity >= 16 / sizeof(element).
struct MsvcString {
    union {
        char buf[16];
        const char* ptr;
    } bx;
    size_t size;
    size_t res;
};

struct MsvcWString {
    union {
        unsigned short buf[8];
        const unsigned short* ptr;
    } bx;
    size_t size;
    size_t res;
};

// ---- the resolved exports ---------------------------------------------------------------------
typedef void(__cdecl* PfnItem_GetUIDisplayText)(void* item, const void* character, void* lines,
                                                bool detailed);
typedef void(__cdecl* PfnGameTextLineToString)(const void* lines, void* out);
// (GameTextLine* this, GameTextClass, const wstring&, bool, const GraphicsTexture*, float).
// Arguments 5 and 6 are on the stack - the ctor reads them from [rsp+0x50] and [rsp+0x58].
typedef void(__cdecl* PfnGameTextLine_Ctor)(void* self, unsigned int cls, const void* wstr,
                                            bool flag, const void* tex, float scale);

// The raw addresses GetProcAddress handed back, kept as void* so the alias check and MinHook see
// exactly what the export table holds and no function-pointer cast is ever needed on that path.
void* r_ItemText = nullptr;
void* r_EquipText = nullptr;
void* r_ArtifactText = nullptr;
void* r_RelicText = nullptr;
void* r_ToString = nullptr;
void* r_LineCtor = nullptr;

PfnGameTextLine_Ctor p_LineCtor = nullptr;

PfnItem_GetUIDisplayText o_ItemText = nullptr;
PfnItem_GetUIDisplayText o_EquipText = nullptr;
PfnItem_GetUIDisplayText o_ArtifactText = nullptr;
PfnItem_GetUIDisplayText o_RelicText = nullptr;
PfnGameTextLineToString o_ToString = nullptr;

// ---- session state ----------------------------------------------------------------------------
// THE fault latch. 1 = the "already collected" mark is off for the rest of the process. Set by a
// missing export, a refused (folded) export, a failed install, a line that did not verify, or the
// first fault in the swap. Mod-owned volatile LONG, never a g_cfg field - configReload replaces
// g_cfg once a second.
volatile LONG g_tooltipOff = 0;
// The compare byte is INDEPENDENT of it: it needs no export and no detour, so it has its own.
volatile LONG g_compareOff = 0;

volatile LONG g_hooksOn = 0;
volatile LONG g_linesReady = 0;
volatile LONG g_swaps = 0;        // tooltips that got the extra line
volatile LONG g_latches = 0;      // collectible items whose tooltip was latched
volatile LONG g_allocBalance = 0; // borrowed arrays alive right now; 0 at rest, always
volatile LONG g_faults = 0;
volatile LONG g_refusedShape = 0; // vectors the swap refused because their shape made no sense
char g_offWhy[192] = "";

// The two prepared lines. Built ONCE per process by the engine's own exported constructor, never
// destroyed and never freed - one bounded allocation of a few dozen bytes per state, which is not
// a leak that grows.
unsigned char g_lineYes[kLineSize];
unsigned char g_lineNo[kLineSize];

char g_textYes[96] = "";
char g_textNo[96] = "";

// ---- the compare-byte counters (game thread only) ---------------------------------------------
// "game thread only" is CHECKED, not asserted - `cmpThreadOk()` claims the first thread that touches them and ignores any other,
// saying so once. That is why they can stay plain LONGs.
LONG g_cmpEpoch = -1;      // liveCaptureEpoch() the counters below belong to
LONG g_cmpPassSet = 0;     // this relayout pass
LONG g_cmpPassSeen = 0;
LONG g_cmpLastIndex = -1;  // the box index the last tooltipCompareBox saw, for the pass boundary
LONG g_cmpDoneSet = 0;     // the pass that has just finished, waiting to be reported
LONG g_cmpDoneSeen = 0;
LONG g_cmpLogged = 0;      // 1 = this world has printed its one line
volatile LONG g_cmpTotal = 0;
volatile LONG g_cmpThread = 0;
volatile LONG g_cmpThreadSaid = 0;

char g_status[320] = "tooltip: not initialised";

// ---- little helpers ---------------------------------------------------------------------------
void* proc(HMODULE m, const char* name, const char* pretty) {
    void* p = m ? (void*)GetProcAddress(m, name) : nullptr;
    logT("  tooltip export %-34s %s", pretty, p ? "ok" : "MISSING");
    return p;
}

// MSVC's identical-COMDAT folding makes hundreds of exports
// share one address, and detouring such an address changes every one of them. Exactly one entry
// of the module's own export address table may point at a target we are about to hook. (None of
// these five is folded on 1.3.0.8; the rule is unconditional, so it is asked anyway.)
int exportAliasCount(HMODULE mod, const void* addr) {
    if (!mod || !addr) return -1;
    __try {
        const unsigned char* base = (const unsigned char*)mod;
        const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return -1;
        const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return -1;
        const IMAGE_DATA_DIRECTORY& d =
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (!d.VirtualAddress || !d.Size) return -1;
        const IMAGE_EXPORT_DIRECTORY* ex = (const IMAGE_EXPORT_DIRECTORY*)(base + d.VirtualAddress);
        const DWORD* fn = (const DWORD*)(base + ex->AddressOfFunctions);
        const DWORD want = (DWORD)((const unsigned char*)addr - base);
        int n = 0;
        for (DWORD i = 0; i < ex->NumberOfFunctions; ++i) {
            if (fn[i] == want) ++n;
        }
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// THE capitalised line the menu test asserts is ABSENT. Every "the mark cannot run" path ends
// here, exactly once, and the feature never re-arms in this process.
void disable(const char* why) {
    if (InterlockedExchange(&g_tooltipOff, 1)) return;
    _snprintf_s(g_offWhy, sizeof(g_offWhy), _TRUNCATE, "%s", why ? why : "?");
    logE("tooltip: OFF ***** the \"already collected\" line is off for this session ***** - %s",
         g_offWhy);
    logD("every item rollover is the vanilla one again; nothing persistent was touched and the "
         "compare byte is unaffected");
}

void faulted(const char* where) {
    InterlockedIncrement(&g_faults);
    logW("tooltip: FAULT in %s - the mark disables itself now", where ? where : "?");
    disable("a fault inside the mod's own tooltip code");
}

// ---- reading engine memory (MOD-SIDE reads: a swallowing frame is the right shape here) --------
bool readRecordSeh(const void* item, unsigned int replicaOff, char* out, size_t cap) {
    __try {
        const MsvcString* s =
            (const MsvcString*)((const unsigned char*)item + replicaOff + 0x08);
        if (s->size == 0 || s->size > 250 || s->size > s->res) return false;
        const char* data = (s->res < 16) ? s->bx.buf : s->bx.ptr;
        if (!data) return false;
        size_t n = s->size;
        if (n >= cap) n = cap - 1;
        memcpy(out, data, n);
        out[n] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = 0;
        return false;
    }
}

// ItemReplicaInfo lives INSIDE the Item at Item + reagentReplicaOffset() (0x538 on 1.3.0.8,
// decoded at run time from Item::GetItemReplicaInfo's own `lea rdx,[rcx+disp32]`), with the
// base-record std::string at +0x08. A pure memory read: no engine call, nothing allocated.
bool readRecord(const void* item, char* out, size_t cap) {
    if (out && cap) out[0] = 0;
    const unsigned int off = reagentReplicaOffset();
    if (!item || !off || !out || cap < 8) return false;
    reagentProbeEnter();
    const bool ok = readRecordSeh(item, off, out, cap);
    reagentProbeLeave();
    return ok;
}

// mem::vector<GameTextLine> is {begin, end, capacityEnd}; GameTextLineToString reads the first
// two words and nothing else.
bool readVecSeh(const void* vec, const unsigned char** begin, const unsigned char** end) {
    __try {
        const unsigned char* const* p = (const unsigned char* const*)vec;
        *begin = p[0];
        *end = p[1];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *begin = nullptr;
        *end = nullptr;
        return false;
    }
}

bool readClassSeh(const unsigned char* line, unsigned int* out) {
    __try {
        *out = *(const unsigned int*)line;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *out = 0;
        return false;
    }
}

// How many lines the vector holds, where they start, and the class of its first and last one.
// That is the fingerprint the swap matches the latch against (see pendingLine()).
bool readVecShape(const void* vec, const unsigned char** first, unsigned int* count,
                  unsigned int* firstCls, unsigned int* lastCls) {
    *first = nullptr;
    *count = 0;
    *firstCls = 0;
    *lastCls = 0;
    const unsigned char* begin = nullptr;
    const unsigned char* end = nullptr;
    reagentProbeEnter();
    bool ok = readVecSeh(vec, &begin, &end);
    if (ok) {
        if (!begin || end < begin || ((size_t)(end - begin) % kLineSize) != 0) {
            ok = false;
        } else {
            const size_t n = (size_t)(end - begin) / kLineSize;
            if (n == 0 || n > kMaxLines) {
                ok = false;
            } else {
                ok = readClassSeh(begin, firstCls) &&
                     readClassSeh(end - kLineSize, lastCls);
                if (ok) {
                    *count = (unsigned int)n;
                    *first = begin;
                }
            }
        }
    }
    reagentProbeLeave();
    return ok;
}

// The class of one line of an already-validated array. Used for the PREFIX test below.
bool classAt(const unsigned char* first, unsigned int index, unsigned int* out) {
    reagentProbeEnter();
    const bool ok = readClassSeh(first + (size_t)index * kLineSize, out);
    reagentProbeLeave();
    return ok;
}

// ---- THE LATCH ---------------------------------------------------------------------------------
// Matching the swap to the latch on `&arg == latch.vector` would be WRONG on the one path that
// matters, and that is the load-bearing design decision in this file: on the shared item-box rollover (exe 0x1EE880)
// `GetUIDisplayText` fills the vector at [rsp+0x78] (0x1EEE9E/0x1EEEA5) and the exe's filter pass
// then COPIES it, line by line, into a SECOND vector at [rsp+0x40] - and it is that second vector
// which `lea rcx,[rsp+0x40]` / `call [rip+0xec0e4]` at 0x1EF0D1/0x1EF0D6 hands to
// GameTextLineToString. The two pointers are never equal there.
//
// So the match is: SAME THREAD (the latch is thread-local), FRESH (< 1 s), NOT YET CONSUMED, and
// one of two things:
//   * the same {line count, first class, last class} fingerprint - the item-box path, where the
//     filter copies every line unless `widget+0xD8` is set, which the dig proved is always 0, so
//     the copy is exact, and equally the rollover paths that hand the SAME vector straight
//     through (an unchanged vector trivially fingerprints as itself);
//   * a PREFIX match: more lines than the latch counted, the same first class, and the latched
//     last class still sitting at index count-1. That is the shape a derived GetUIDisplayText
//     leaves when it calls the base and then appends its own lines - which is why
//     `ItemEquipment::GetUIDisplayText` is hooked as well (see the note on the job table): with
//     that hook the equipment collection takes the EXACT tier, and this one is only the safety
//     net for the chat-link, blueprint and character-window rollovers.
// The one call that could otherwise be decorated by mistake is the vendor buy/sell text at exe
// 0x1EED3F, which runs between the compare block and GetUIDisplayText: it is one or two lines of
// GameTextClass 0x2, so neither its count nor its first class can match an item tooltip.
//
// Stale-safety, two rules, both deliberate:
//   (a) the latch was matched on a BARE POINTER first, with no corroboration. Stack addresses
//       repeat, so a vector belonging to some later, unrelated call can land on the same address.
//       So the fingerprint is always asked. It costs nothing, because on every
//       path where the pointer really was the same object the exact tier fires anyway.
//   (b) the latch is CONSUMED BY THE FIRST GameTextLineToString after it, match or no match.
//       A latch that survived a non-match would go stale, and latches are routinely left
//       unclaimed: `Item::GetUIDisplayText` has non-tooltip callers inside Game.dll
//       (`ItemEquipment::GetSearchText` at 0x32A0AD and ten more) that never reach
//       GameTextLineToString, and 16 of the GetUIDisplayText overrides are NOT hooked at all
//       (ItemNote, QuestItem, the OneShot_* potions/scrolls/dyes, ItemTransmuter(Set),
//       ItemArtifactFormula, ItemAttributeReset, ItemDevotionReset, ItemDifficultyUnlock,
//       ItemFactionBooster/Warrant, ItemSet), so those neither refresh nor clear it. A stale
//       latch could therefore be claimed by the tooltip of a component hovered a moment after a
//       unique. Consuming it is safe on the path that matters: on the item-box rollover the only
//       GameTextLineToString that can run near a latch is the vendor buy/sell text at exe
//       0x1EED3F, and that one runs BEFORE GetUIDisplayText (0x1EEEA5), never after it.
struct Latch {
    unsigned int count;
    unsigned int firstClass;
    unsigned int lastClass;
    int state;  // 0 = none / consumed, 1 = COLLECTED, 2 = NOT COLLECTED
    DWORD tick;
};
__declspec(thread) Latch t_latch;

void clearLatch() {
    t_latch.state = 0;
}

const unsigned char* pendingLine(const void* vec) {
    if (!vec) return nullptr;
    if (InterlockedCompareExchange(&g_tooltipOff, 0, 0)) return nullptr;
    if (!InterlockedCompareExchange(&g_linesReady, 0, 0)) return nullptr;
    if (!g_cfg.tooltipMark) return nullptr;
    if (t_latch.state != 1 && t_latch.state != 2) return nullptr;
    const Latch cur = t_latch;   // POD copy
    clearLatch();                // ONE-SHOT: consumed here whether it matches below or not
    if (GetTickCount() - cur.tick > kLatchMaxAgeMs) return nullptr;
    if (cur.count == 0) return nullptr;
    const unsigned char* first = nullptr;
    unsigned int count = 0, firstCls = 0, lastCls = 0;
    if (!readVecShape(vec, &first, &count, &firstCls, &lastCls)) return nullptr;
    if (firstCls != cur.firstClass) return nullptr;
    bool match = (count == cur.count && lastCls == cur.lastClass);   // exact
    if (!match && count > cur.count) {
        unsigned int atPrefixEnd = 0;
        match = classAt(first, cur.count - 1, &atPrefixEnd) &&
                atPrefixEnd == cur.lastClass;                        // prefix
    }
    if (!match) return nullptr;
    return cur.state == 1 ? g_lineYes : g_lineNo;
}

// ---- THE SWAP ----------------------------------------------------------------------------------
bool copyLinesSeh(unsigned char* dst, const unsigned char* src, size_t bytes) {
    __try {
        memcpy(dst, src, bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

// Factored out of the detour so the offline harness (tools/test_tooltip.cpp) drives exactly the
// code the game drives. Builds an array of N+1 GameTextLine records: the engine's N copied byte
// for byte, then `extraLine`. Returns false - and allocates nothing - for any vector whose shape
// is not a plausible tooltip.
bool tooltipSwapBuild(const void* vec, const unsigned char* extraLine, unsigned char** ownedOut,
                      UtTooltipSwap* out) {
    if (ownedOut) *ownedOut = nullptr;
    if (!vec || !extraLine || !ownedOut || !out) return false;
    const unsigned char* begin = nullptr;
    const unsigned char* end = nullptr;
    reagentProbeEnter();
    const bool got = readVecSeh(vec, &begin, &end);
    reagentProbeLeave();
    if (!got || !begin || end < begin) {
        InterlockedIncrement(&g_refusedShape);
        return false;
    }
    const size_t bytes = (size_t)(end - begin);
    if ((bytes % kLineSize) != 0) {
        InterlockedIncrement(&g_refusedShape);
        return false;
    }
    const size_t n = bytes / kLineSize;
    if (n == 0 || n > kMaxLines) {
        InterlockedIncrement(&g_refusedShape);
        return false;
    }
    unsigned char* buf = (unsigned char*)malloc((n + 1) * kLineSize);
    if (!buf) return false;
    reagentProbeEnter();
    const bool copied = copyLinesSeh(buf, begin, bytes);
    reagentProbeLeave();
    if (!copied) {
        free(buf);
        return false;
    }
    memcpy(buf + bytes, extraLine, kLineSize);
    out->begin = buf;
    out->end = buf + bytes + kLineSize;
    out->cap = out->end;
    *ownedOut = buf;
    InterlockedIncrement(&g_allocBalance);
    return true;
}

void tooltipSwapFree(unsigned char* owned) {
    if (!owned) return;
    free(owned);
    InterlockedDecrement(&g_allocBalance);
}

long tooltipSwapAllocBalance() {
    return InterlockedCompareExchange(&g_allocBalance, 0, 0);
}

namespace {

// ---- "is this record already collected?", MEMOISED ----------------------------------------------
// `journalHas` is NOT a cheap hash probe. It is a case-insensitive LINEAR scan of
// up to 3,288 entries (`findEntry`, ut_rescue.cpp) taken under the journal's own critical section -
// the SAME section `journalService` holds across the whole atomic file write (build the text in
// memory, write the .tmp, FlushFileBuffers, rename), which the worker runs the moment a deposit
// signals the journal event, i.e. exactly while the user is standing at the caravan hovering boxes.
// `plateOwns` really is a hash probe, but it takes the same kind of trip.
//
// The rollover is rebuilt EVERY FRAME while the cursor rests on a box, so the same record would
// be asked 60-144 times a second. It is asked at most four times a second per record. A quarter of a second of staleness is invisible: the only thing that can change the
// answer is the user's own deposit or take, and both take far longer than that to complete.
struct Collected {
    char record[256];
    int state;   // 0 = nothing cached, 1 = COLLECTED, 2 = NOT COLLECTED
    DWORD tick;
};
__declspec(thread) Collected t_collected;

const DWORD kProbeMaxAgeMs = 250;

// THE ONLY TWO LOCKS ON THE TOOLTIP PATH. Called from latchBody OUTSIDE every SEH frame - see the
// comment there for why that placement is not negotiable.
int collectedState(const char* record) {
    const DWORD now = GetTickCount();
    if (t_collected.state && (now - t_collected.tick) <= kProbeMaxAgeMs &&
        _stricmp(t_collected.record, record) == 0) {
        return t_collected.state;
    }
    // plateOwns is the one rule the painted boxes and the owned counters use: the table holds
    // at least one copy (storeCount), or the engine's reagent map holds a live one. A journal
    // entry alone is NOT ownership - a take leaves the entry in the file with count 0 so a later
    // deposit can restore the item's identity, and asking journalHas here painted every item
    // ever deposited green for good.
    const int state = plateOwns(record) ? 1 : 2;
    _snprintf_s(t_collected.record, sizeof(t_collected.record), _TRUNCATE, "%s", record);
    t_collected.state = state;
    t_collected.tick = now;
    return state;
}

// ---- the detour bodies -------------------------------------------------------------------------
struct LatchPrep {
    char record[256];
    unsigned int count;
    unsigned int firstClass;
    unsigned int lastClass;
};

// MOD-SIDE READS OF ENGINE MEMORY, and NOTHING ELSE: no lock, no allocation, no logging. That is
// what lets the caller wrap this - and only this - in a swallowing frame.
bool latchRead(void* item, void* lines, LatchPrep* p) {
    if (!readRecord(item, p->record, sizeof(p->record)) || !p->record[0]) return false;
    // NOT COLLECTIBLE: the record is not one of the collection's own. Nothing is added - a
    // crafting material, a component, a common item and every quest item keep the vanilla
    // tooltip exactly. (`isOurRecordSafe` is a hash probe over a set built at DB load, with its
    // own try/catch and no lock.)
    if (!reagentIsCollectionRecord(p->record)) return false;
    const unsigned char* first = nullptr;
    return readVecShape(lines, &first, &p->count, &p->firstClass, &p->lastClass);
}

// -1 = faulted, 0 = nothing to latch, 1 = `p` is filled.
int latchReadGuarded(void* item, void* lines, LatchPrep* p) {
    __try {
        return latchRead(item, lines, p) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;   // faulted() is called by the CALLER, outside this frame - it takes the log lock
    }
}

// The mod half of the latch. Runs AFTER the trampoline, so the vector is already filled.
//
// The shape of this function is deliberate. The whole body must NOT sit inside one
// `__except(EXCEPTION_EXECUTE_HANDLER)`, because the body takes TWO critical sections: the
// journal's (`journalHas`) and the log's (`logf`). A fault swallowed with either of them held
// would never leave it - a Windows CRITICAL_SECTION is not released by unwinding - and the
// journal's lock is what `journalService` (the worker) and `journalUpsert` (a deposit, on this
// same game thread) both wait on, so the game would have hung on the next deposit. Worse, the
// handler itself called `logf`, which re-enters the log lock recursively and leaves it owned at
// count 1: every logf in the process would have blocked for good. The project's own precedent for
// the opposite shape is ut_reagent.cpp's `__try/__finally` "so a fault still clears it".
//
// So: the SEH frame now covers ONLY the pure reads of engine memory (latchReadGuarded), which
// take no lock at all, and everything that locks happens after it has been left.
void latchBody(void* item, void* lines) {
    if (InterlockedCompareExchange(&g_tooltipOff, 0, 0)) return;
    if (!g_cfg.tooltipMark) {
        clearLatch();
        return;
    }
    LatchPrep prep;
    prep.record[0] = 0;
    prep.count = 0;
    prep.firstClass = 0;
    prep.lastClass = 0;
    const int got = latchReadGuarded(item, lines, &prep);
    clearLatch();
    if (got <= 0) {
        if (got < 0) faulted("the GetUIDisplayText latch");   // OUTSIDE the frame: it logs
        return;
    }
    // ---- from here on nothing is inside an SEH frame and every read is mod-owned ---------------
    try {
        Latch fresh;
        fresh.count = prep.count;
        fresh.firstClass = prep.firstClass;
        fresh.lastClass = prep.lastClass;
        fresh.state = collectedState(prep.record);   // takes the journal lock, at most 4 Hz/record
        fresh.tick = GetTickCount();
        t_latch = fresh;
        InterlockedIncrement(&g_latches);
        // Second flush point for the compare-flag line: a relayout can only have finished by the
        // time a rollover is being built, and this costs two reads of process-local longs.
        tooltipCompareFlushPending();
        // The only telemetry this feature has. It lives HERE rather than in the worker's heartbeat
        // because here it is more useful: it prints exactly while someone is hovering
        // collectible items, at most once a minute, and never at the menu.
        // (Constant-initialised POD, so no thread-safe-static guard is emitted.)
        static DWORD lastStatus = 0;
        if (fresh.tick - lastStatus > 60000) {
            lastStatus = fresh.tick;
            logD("%s", tooltipStatus());
        }
    } catch (...) {
        // Every C++ body reachable from a detour has one. Nothing above allocates
        // and neither `journalHas` nor `plateOwns` can throw past its own lock (plateOwns has its
        // own try/catch; journalHas only does _stricmp over std::strings that already exist), so
        // this is belt and braces rather than the lock guard - the lock guard is the fact that
        // there is no SEH frame around either of them.
        clearLatch();
    }
}

void __cdecl hk_ItemGetUIDisplayText(void* item, const void* character, void* lines,
                                     bool detailed) {
    if (o_ItemText) o_ItemText(item, character, lines, detailed);  // ENGINE CALL - unguarded
    latchBody(item, lines);
}

// ItemEquipment CALLS the base at Game.dll 0x3299AF and then appends its own lines, so the latch
// the base detour takes sees a PARTIAL vector. This detour runs on the way out and overwrites it
// with the complete one - which is what makes the exact fingerprint tier above cover the whole
// equipment collection instead of falling through to the prefix tier.
void __cdecl hk_EquipGetUIDisplayText(void* item, const void* character, void* lines,
                                      bool detailed) {
    if (o_EquipText) o_EquipText(item, character, lines, detailed);
    latchBody(item, lines);
}

void __cdecl hk_ArtifactGetUIDisplayText(void* item, const void* character, void* lines,
                                         bool detailed) {
    if (o_ArtifactText) o_ArtifactText(item, character, lines, detailed);
    latchBody(item, lines);
}

void __cdecl hk_RelicGetUIDisplayText(void* item, const void* character, void* lines,
                                      bool detailed) {
    if (o_RelicText) o_RelicText(item, character, lines, detailed);
    latchBody(item, lines);
}

// No C++ object and no swallowing frame lives in this function: the `__finally` is CLEANUP, so an
// engine C++ exception out of the trampoline keeps unwinding to the engine's own handler.
void __cdecl hk_GameTextLineToString(const void* lines, void* out) {
    const unsigned char* extra = nullptr;
    __try {
        extra = pendingLine(lines);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        extra = nullptr;
        faulted("the GameTextLineToString match");
    }
    if (!extra) {
        if (o_ToString) o_ToString(lines, out);  // ENGINE CALL - unguarded
        return;
    }
    unsigned char* owned = nullptr;
    UtTooltipSwap borrowed;
    borrowed.begin = nullptr;
    borrowed.end = nullptr;
    borrowed.cap = nullptr;
    if (!tooltipSwapBuild(lines, extra, &owned, &borrowed)) {
        if (o_ToString) o_ToString(lines, out);
        return;
    }
    InterlockedIncrement(&g_swaps);
    __try {
        if (o_ToString) o_ToString(&borrowed, out);  // ENGINE CALL - unguarded
    } __finally {
        tooltipSwapFree(owned);
    }
}

// ---- building the two static lines --------------------------------------------------------------
void makeWide(MsvcWString* s, const char* ascii, unsigned short* storage, size_t storageChars) {
    memset(s, 0, sizeof(*s));
    size_t n = strlen(ascii);
    if (n > storageChars - 1) n = storageChars - 1;
    for (size_t i = 0; i < n; ++i) storage[i] = (unsigned short)(unsigned char)ascii[i];
    storage[n] = 0;
    s->size = n;
    if (n < 8) {
        memcpy(s->bx.buf, storage, (n + 1) * sizeof(unsigned short));
        s->bx.buf[n] = 0;
        s->res = 7;
    } else {
        s->bx.ptr = storage;
        s->res = n;
    }
}

// Reads back what the ctor wrote. A mod-side read of a mod-owned buffer, so the swallowing frame
// is the right shape; a mismatch means the export is not the constructor we think it is and the
// feature turns itself off before a single tooltip has been touched.
bool verifyLineSeh(const unsigned char* line, unsigned int cls, size_t wantLen) {
    __try {
        if (*(const unsigned int*)line != cls) return false;
        const MsvcWString* s = (const MsvcWString*)(line + 0x08);
        if (s->size != wantLen) return false;
        if (s->res < s->size) return false;
        const unsigned short* text = (s->res < 8) ? s->bx.buf : s->bx.ptr;
        if (!text && wantLen) return false;
        if (line[0x28] != 0) return false;
        if (*(const void* const*)(line + 0x30) != nullptr) return false;
        if (*(const float*)(line + 0x38) != 1.0f) return false;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool buildLine(unsigned char* line, unsigned int cls, const char* ascii) {
    memset(line, 0, kLineSize);
    unsigned short storage[128];
    MsvcWString in;
    makeWide(&in, ascii, storage, 128);
    // ENGINE CALL, deliberately UNGUARDED (rule 9). dllmain's vectored handler already logs any
    // access violation with one of this DLL's frames on the stack, and the ctor never reads its
    // destination - it _Tidy_init's the string in place before assigning.
    p_LineCtor(line, cls, &in, false, nullptr, 1.0f);
    // The verification re-reads the +0x00 GameTextClass word among the rest, checked against the
    // ini-selected class, so
    // this line is also the proof that the colour the user asked for is the one in the record.
    const bool ok = verifyLineSeh(line, cls, in.size);
    const TextClassInfo* ci = classInfo(cls);
    logD("tooltip: line class 0x%02X = style \"%s\" RGB (%d,%d,%d) - \"%s\" (%zu chars) "
         "built by the engine's own ctor -> %s",
         cls, ci ? ci->style : "(unregistered)", ci ? ci->r : -1,
         ci ? ci->g : -1, ci ? ci->b : -1, ascii, in.size,
         ok ? "verified (class, string size, the +0x28 bool, the null texture and the 1.0f scale "
              "all read back)"
            : "***** DID NOT VERIFY *****");
    return ok;
}

const char* textFor(const char* fromIni, const char* fallback) {
    if (fromIni && fromIni[0]) return fromIni;
    return fallback;
}

// ---- the compare byte ---------------------------------------------------------------------------
bool writeCompareSeh(void* box, unsigned char value) {
    __try {
        *((unsigned char*)box + kCompareOff) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The counters above are plain LONGs read-modify-written from two call sites (`applyLayout`'s
// relayout and the rollover build). Both are the game thread, which is now ASSERTED rather than
// assumed: the first thread to arrive owns them and any other is ignored after one log line.
bool cmpThreadOk() {
    const LONG me = (LONG)GetCurrentThreadId();
    const LONG owner = InterlockedCompareExchange(&g_cmpThread, me, 0);
    if (owner == 0 || owner == me) return true;
    if (!InterlockedExchange(&g_cmpThreadSaid, 1)) {
        logD("tooltip: the compare counters were reached from thread %ld as well as their owner "
             "%ld - the second thread is ignored (they are game-thread only by design; only the "
             "one `compare flag set` log line can be affected, never the byte itself)", me, owner);
    }
    return false;
}

void reportPass() {
    if (g_cmpLogged || g_cmpDoneSeen <= 0) return;
    g_cmpLogged = 1;
    logD("tooltip: compare flag set on %ld of %ld boxes (compare_popup=%d) - hovering a stored "
         "unique now reaches the engine's own \"Currently Equipped\" comparison",
         g_cmpDoneSet, g_cmpDoneSeen, g_cfg.comparePopup);
}

}  // namespace

// Called from the latch detour and from the start of every relayout pass. Prints the one
// per-world line as soon as a pass has finished, and resets everything when a new HUD is built.
void tooltipCompareFlushPending() {
    if (!cmpThreadOk()) return;
    const LONG epoch = (LONG)liveCaptureEpoch();
    if (epoch != g_cmpEpoch) {
        g_cmpEpoch = epoch;
        g_cmpLogged = 0;
        g_cmpDoneSet = 0;
        g_cmpDoneSeen = 0;
        g_cmpPassSet = 0;
        g_cmpPassSeen = 0;
        g_cmpLastIndex = -1;
        return;
    }
    // A rollover is being built, so any relayout has already finished - `applyLayout` runs whole,
    // on this same thread, and never has a tooltip inside it. Retire the pass here too, or a world
    // where the user opens exactly one group and never leaves it would never print the line.
    if (g_cmpPassSeen > 0) {
        g_cmpDoneSet = g_cmpPassSet;
        g_cmpDoneSeen = g_cmpPassSeen;
    }
    reportPass();
}

void tooltipCompareBox(void* box, size_t index, bool collection) {
    if (!box) return;
    if (!cmpThreadOk()) return;
    // A NEW HUD retires everything. This is asked on every box, not only on index 0: the epoch
    // is one interlocked read, and inferring "a pass started" from one particular index is wrong
    // (see below).
    const LONG epoch = (LONG)liveCaptureEpoch();
    if (epoch != g_cmpEpoch) {
        g_cmpEpoch = epoch;
        g_cmpLogged = 0;
        g_cmpDoneSet = 0;
        g_cmpDoneSeen = 0;
        g_cmpPassSet = 0;
        g_cmpPassSeen = 0;
        g_cmpLastIndex = -1;
    }
    // The pass boundary is NOT `index == 0`: that index is not reached when a pass shows no box
    // at all, and then the previous pass's counters would survive into the next one and the
    // single `compare flag set on N of M boxes` line could report a sum of two passes.
    // `applyLayout` walks the boxes with a STRICTLY INCREASING index, so "the index did not
    // advance" is the pass boundary itself: index 0 in practice, and still right if a pass ever
    // begins somewhere else. (Only that log line was ever at risk - the byte is written per box
    // and does not depend on any of this.)
    if ((LONG)index <= g_cmpLastIndex) {
        if (g_cmpPassSeen > 0) {
            g_cmpDoneSet = g_cmpPassSet;
            g_cmpDoneSeen = g_cmpPassSeen;
            reportPass();
        }
        g_cmpPassSet = 0;
        g_cmpPassSeen = 0;
    }
    g_cmpLastIndex = (LONG)index;
    if (InterlockedCompareExchange(&g_compareOff, 0, 0)) return;
    // 0 is written as deliberately as 1: the vanilla crafting-materials page must never carry the
    // flag (base Item's vtable +0x550 - the equip-slot type the partner test compares - is a
    // folded stub, so an Aether Crystal could otherwise acquire a nonsense compare partner), and
    // writing it makes `compare_popup=0` take effect on the very next relayout.
    const unsigned char want = (collection && g_cfg.comparePopup) ? (unsigned char)1 : (unsigned char)0;
    ++g_cmpPassSeen;
    reagentProbeEnter();
    const bool ok = writeCompareSeh(box, want);
    reagentProbeLeave();
    if (!ok) {
        if (!InterlockedExchange(&g_compareOff, 1)) {
            logW("tooltip: FAULT writing the compare byte on box %p +0x%02X ***** it is off for "
                 "this session ***** - nothing else of the mod is affected.", box, kCompareOff);
        }
        return;
    }
    if (want) {
        ++g_cmpPassSet;
        InterlockedIncrement(&g_cmpTotal);
    }
}

bool tooltipInit(HMODULE selfModule) {
    (void)selfModule;
    static bool done = false;
    if (done) return InterlockedCompareExchange(&g_tooltipOff, 0, 0) == 0;
    done = true;

    _snprintf_s(g_textYes, sizeof(g_textYes), _TRUNCATE, "%s",
                textFor(g_cfg.tooltipTextYes, kDefaultTextYes));
    _snprintf_s(g_textNo, sizeof(g_textNo), _TRUNCATE, "%s",
                textFor(g_cfg.tooltipTextNo, kDefaultTextNo));

    // The policy line - printed whatever the keys say, so the menu test can always see it.
    logD("tooltip: compare_popup=%d tooltip_mark=%d - the compare byte is box+0x%02X and is only "
         "ever set on COLLECTION boxes (never the vanilla materials page); the mark is five "
         "exported Game.dll detours that append one GameTextLine and never draw a pixel. "
         "collected=\"%s\" notCollected=\"%s\"",
         g_cfg.comparePopup, g_cfg.tooltipMark, kCompareOff, g_textYes, g_textNo);

    HMODULE game = GetModuleHandleA("Game.dll");
    if (!game) {
        disable("Game.dll is not loaded");
        return false;
    }
    r_ItemText = proc(game, GD_ITEM_GETUIDISPLAYTEXT, "Item::GetUIDisplayText");
    r_EquipText = proc(game, GD_ITEMEQUIPMENT_GETUIDISPLAYTEXT, "ItemEquipment::GetUIDisplayText");
    r_ArtifactText = proc(game, GD_ITEMARTIFACT_GETUIDISPLAYTEXT, "ItemArtifact::GetUIDisplayText");
    r_RelicText = proc(game, GD_ITEMRELIC_GETUIDISPLAYTEXT, "ItemRelic::GetUIDisplayText");
    r_ToString = proc(game, GD_GAMETEXTLINETOSTRING, "GameTextLineToString");
    r_LineCtor = proc(game, GD_GAMETEXTLINE_CTOR_W, "GameTextLine::GameTextLine");

    if (!r_ItemText || !r_EquipText || !r_ArtifactText || !r_RelicText || !r_ToString ||
        !r_LineCtor) {
        disable("at least one tooltip export is MISSING");
        return false;
    }
    const int a1 = exportAliasCount(game, r_ItemText);
    const int a2 = exportAliasCount(game, r_EquipText);
    const int a3 = exportAliasCount(game, r_ArtifactText);
    const int a4 = exportAliasCount(game, r_RelicText);
    const int a5 = exportAliasCount(game, r_ToString);
    const int a6 = exportAliasCount(game, r_LineCtor);
    logD("tooltip: export-alias check - Item %p x%d, ItemEquipment %p x%d, ItemArtifact %p x%d, "
         "ItemRelic %p x%d, GameTextLineToString %p x%d, GameTextLine ctor %p x%d (1 = not folded)",
         r_ItemText, a1, r_EquipText, a2, r_ArtifactText, a3, r_RelicText, a4, r_ToString, a5,
         r_LineCtor, a6);
    if (a1 != 1 || a2 != 1 || a3 != 1 || a4 != 1 || a5 != 1 || a6 != 1) {
        disable("a tooltip export is FOLDED across several names - detouring it would change "
                "every one of them");
        return false;
    }
    p_LineCtor = (PfnGameTextLine_Ctor)r_LineCtor;

    // The two classes are ini-selectable, and both are validated against the
    // measured registration table before a single GameTextLine is built.
    g_classYes = pickClass(g_cfg.tooltipClassYes, kClassCollectedDefault, "tooltip_class_yes");
    g_classNo = pickClass(g_cfg.tooltipClassNo, kClassNotCollectedDefault, "tooltip_class_no");
    const bool okYes = buildLine(g_lineYes, g_classYes, g_textYes);
    const bool okNo = buildLine(g_lineNo, g_classNo, g_textNo);
    if (!okYes || !okNo) {
        disable("a prepared GameTextLine did not verify after the engine's ctor ran");
        return false;
    }
    InterlockedExchange(&g_linesReady, 1);
    const TextClassInfo* iy = classInfo(g_classYes);
    const TextClassInfo* in = classInfo(g_classNo);
    logD("tooltip: both lines ready - COLLECTED is class 0x%02X \"%s\" RGB (%d,%d,%d) and NOT "
         "COLLECTED is class 0x%02X \"%s\" RGB (%d,%d,%d); the colours come from the style "
         "records records/game/gameengine.dbr names, read back through the engine's own "
         "GameEngine::GetGameTextStyleName - the mod draws nothing. desaturate=%s (the "
         "GameTextLineToString branch fires only for 0x50..0x53)",
         g_classYes, iy ? iy->style : "?", iy ? iy->r : -1, iy ? iy->g : -1, iy ? iy->b : -1,
         g_classNo, in ? in->style : "?", in ? in->r : -1, in ? in->g : -1, in ? in->b : -1,
         ((g_classYes - 0x50u) <= 3u || (g_classNo - 0x50u) <= 3u) ? "ON for one of them" : "off");
    return true;
}

const int kJobs = 5;

int tooltipInstall(int* total) {
    if (total) *total = kJobs;
    if (InterlockedCompareExchange(&g_tooltipOff, 0, 0)) {
        logE("tooltip: no detour is installed - %s", g_offWhy);
        return 0;
    }
    // ALL-OR-NOTHING. Every hook is CREATED first; only when all five exist are they enabled.
    // Why FIVE and not the dig's two: `ItemEquipment` calls the base and then appends, so the
    // base hook alone latches a partial vector for the bulk of the collection; `ItemArtifact` and
    // `ItemRelic` do not call the base at all, so they need their own.
    struct {
        const char* pretty;
        void* target;
        void* detour;
        void** original;
    } jobs[kJobs] = {
        {"Item::GetUIDisplayText", r_ItemText, (void*)&hk_ItemGetUIDisplayText,
         (void**)&o_ItemText},
        {"ItemEquipment::GetUIDisplayText", r_EquipText, (void*)&hk_EquipGetUIDisplayText,
         (void**)&o_EquipText},
        {"ItemArtifact::GetUIDisplayText", r_ArtifactText, (void*)&hk_ArtifactGetUIDisplayText,
         (void**)&o_ArtifactText},
        {"ItemRelic::GetUIDisplayText", r_RelicText, (void*)&hk_RelicGetUIDisplayText,
         (void**)&o_RelicText},
        {"GameTextLineToString", r_ToString, (void*)&hk_GameTextLineToString,
         (void**)&o_ToString},
    };
    int created = 0;
    for (int i = 0; i < kJobs; ++i) {
        const MH_STATUS s = MH_CreateHook(jobs[i].target, jobs[i].detour, jobs[i].original);
        if (s != MH_OK) {
            const char* t = MH_StatusToString(s);
            logE("  tooltip hook %-32s MH_CreateHook FAILED: %s", jobs[i].pretty, t ? t : "?");
            break;
        }
        ++created;
    }
    if (created != kJobs) {
        for (int i = 0; i < created; ++i) MH_RemoveHook(jobs[i].target);
        disable("a tooltip detour could not be created - the group installs ALL or NOTHING");
        return 0;
    }
    int enabled = 0;
    for (int i = 0; i < kJobs; ++i) {
        const MH_STATUS s = MH_EnableHook(jobs[i].target);
        if (s != MH_OK) {
            const char* t = MH_StatusToString(s);
            logE("  tooltip hook %-32s MH_EnableHook FAILED: %s", jobs[i].pretty, t ? t : "?");
            break;
        }
        logD("  tooltip hook %-32s installed at %p (trampoline %p)", jobs[i].pretty,
             jobs[i].target, *jobs[i].original);
        ++enabled;
    }
    if (enabled != kJobs) {
        for (int i = 0; i < kJobs; ++i) MH_DisableHook(jobs[i].target);
        for (int i = 0; i < kJobs; ++i) MH_RemoveHook(jobs[i].target);
        disable("a tooltip detour could not be enabled - the group installs ALL or NOTHING");
        return 0;
    }
    InterlockedExchange(&g_hooksOn, 1);
    logI("tooltip: the collection mark is on (5 of 5 detours installed)");
    logD("the five are NOT counted in `detours installed: N of N` - a missing tooltip export "
         "turns one feature off and must never fail the whole mod");
    return kJobs;
}

const char* tooltipStatus() {
    _snprintf_s(g_status, sizeof(g_status), _TRUNCATE,
                "tooltip: %s hooks=%ld lines=%ld latches=%ld swaps=%ld refusedShape=%ld "
                "faults=%ld alloc=%ld | compare: %s set=%ld epoch=%ld",
                InterlockedCompareExchange(&g_tooltipOff, 0, 0) ? "OFF" : "armed",
                InterlockedCompareExchange(&g_hooksOn, 0, 0),
                InterlockedCompareExchange(&g_linesReady, 0, 0),
                InterlockedCompareExchange(&g_latches, 0, 0),
                InterlockedCompareExchange(&g_swaps, 0, 0),
                InterlockedCompareExchange(&g_refusedShape, 0, 0),
                InterlockedCompareExchange(&g_faults, 0, 0),
                InterlockedCompareExchange(&g_allocBalance, 0, 0),
                InterlockedCompareExchange(&g_compareOff, 0, 0) ? "OFF" : "armed",
                InterlockedCompareExchange(&g_cmpTotal, 0, 0), g_cmpEpoch);
    return g_status;
}

}  // namespace ut
