// ut_reagent.cpp - the collection itself: everything that decides what the reagent map holds.
//
// This file owns
//   * the REAGENT GATE - the Item+craftingMaterial byte that makes an item acceptable to the
//     engine's reagent map at all, armed only inside the narrow window in which it is needed;
//   * the DEPOSIT PATHS - the one choke point (GameEngine::AddItemToReagents) through which all
//     four engine routes into the map pass, plus the auto-deposit and sack-transfer detours;
//   * the BAG PROOF - the evidence that the item really came out of a bag the mod can see, which
//     is what separates a legitimate deposit from an unsupported route;
//   * the TAKE and the IDENTITY - re-creating the stored item and restoring its replica so that
//     what comes back out is the item that went in, not a stock copy of the record;
//   * the CENSUS - the tally of what the collection holds, reconciled against the engine's map;
//   * the BINDINGS DECODE - resolving the engine offsets and private call targets this file
//     depends on out of exported code bytes, never from hard-coded constants.
//
// Invariants, in order of importance:
//   * NEVER LOSE OR DUPLICATE AN ITEM. A deposit that cannot be completed must be refused before
//     the engine removes the item from the bag; a take that cannot restore identity must still
//     hand back the item.
//   * The game's own save files are never written. The mod writes only its own files.
//   * The gate byte is only ever written on an item the LOCAL player owns, and is always written
//     back to 0 when the window closes - an item left armed is locked out of every stash tab.
//   * Offsets and private call targets are decoded from exported code bytes at run time. A decode
//     that does not match its expected shape turns the feature off; nothing falls back to a
//     literal.
//
// See ut_reagent.h for the interface.

#include "ut_reagent.h"

#include <intrin.h>
#include <psapi.h>
#include <stdio.h>
#include <string.h>

#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "MinHook.h"
#include "gd_exports_reagent.h"
#include "ut_config.h"
#include "ut_depositgate.h"  // the table-or-refusal decision, as pure logic
#include "ut_live.h"
#include "ut_pagegate.h"  // substitute-or-vanilla, as pure logic
#include "ut_plate.h"  // plateCaravanState() / plateOnWorldTeardown()
#include "ut_rescue.h"
#include "ut_store.h"
#include "ut_bindings.h"
#include "ut_log.h"
#include "ut_paths.h"
#include "ut_replicasize.h"

namespace ut {

// ---- the relayout hook -----------------------------------------------------------------------
// `liveRelayoutVisible()` is defined in ut_live.cpp: it forces a relayout of the VISIBLE page on
// the next game-thread tick. The prototype swap needs it because ReagentWindow::Sync only
// re-points box+0x30, while the box widget rebuilds its component sub-icons on its own SetItem -
// which is what a relayout runs. Without it a freshly swapped prototype shows no sub-icons until
// the user scrolls to another page and back.
//
// It is DECLARED here, at ut:: scope and outside the anonymous namespace, rather than by
// including ut_live.h. The /alternatename directive points the symbol at a local stub so that a
// build without ut_live.cpp still links (and says so once); in a complete build the real
// definition wins and the stub is dead code.
void liveRelayoutVisible();
void utLiveRelayoutVisibleFallback();
#pragma comment( \
    linker,      \
    "/alternatename:?liveRelayoutVisible@ut@@YAXXZ=?utLiveRelayoutVisibleFallback@ut@@YAXXZ")

namespace {

// ---- engine prototypes ----------------------------------------------------------------------
typedef bool(__cdecl* PfnItem_IsReagentCompatible)(const GdItem*);
typedef void(__cdecl* PfnItem_Load)(GdItem*, const void* loadTable);
typedef bool(__cdecl* PfnGE_AddItemToReagents)(GdGameEngine*, unsigned int);
typedef int(__cdecl* PfnGE_TakeItemFromReagents)(GdGameEngine*, const void* stdString, int);
typedef void(__cdecl* PfnGE_Void)(GdGameEngine*);
typedef void(__cdecl* PfnCtrl_Void)(void*);
typedef bool(__cdecl* PfnSack_DepositIntoReagents)(GdSack*, void* controllerPlayer);
typedef void(__cdecl* PfnOM_DestroyObjectEx)(void*, void*, const char*, int);
typedef bool(__cdecl* PfnCursor_Bool)(void*);
typedef void(__cdecl* PfnGE_Void)(GdGameEngine*);  // SyncCaravanReagents
typedef void(__cdecl* PfnEngine_LoadMainDatabase)(GdEngine*);
typedef bool(__cdecl* PfnEngine_LoadDatabase)(GdEngine*, const void* stdString);
typedef unsigned int(__cdecl* PfnEngine_GetDbChecksum)(GdEngine*);
typedef bool(__cdecl* PfnEngine_HasLoadedCustomDatabase)(const GdEngine*);
typedef int(__cdecl* PfnGE_GetReagentItemCount)(const GdGameEngine*, const void* stdString,
                                                void* memVectorUInt);
typedef int(__cdecl* PfnGE_TakeItemFromReagentsId)(GdGameEngine*, unsigned int, int);
typedef unsigned int(__cdecl* PfnItem_GetU32)(const GdItem*);
typedef void*(__cdecl* PfnOM_ObjectFromId)(void* objectManager, unsigned int id);
typedef bool(__cdecl* PfnOM_IsObjectOnDeletedList)(void* objectManager, void* object);
typedef bool(__cdecl* PfnOM_IsObjectIdOnDeletedList)(void* objectManager, unsigned int id);
typedef void*(__cdecl* PfnEngine_GetGameInfo)(GdEngine*);
typedef bool(__cdecl* PfnGameInfo_GetIsMultiPlayer)(const void*);
// TELEMETRY ONLY, never a gate. It is unproven whether GetNumOfPlayers (GameInfo+0x1B8) is the
// live roster or the lobby's configured size, and gating on a peer count would flip the mod off
// mid-deposit the moment a friend joins - so these three are read, logged, never branched on.
typedef bool(__cdecl* PfnGameInfo_GetBool)(const void*);
typedef unsigned int(__cdecl* PfnGameInfo_GetUInt)(const void*);
typedef void(__cdecl* PfnGE_ExitPlayingMode)(GdGameEngine*);

PfnItem_IsReagentCompatible p_IsReagentCompatible = nullptr;
PfnEngine_LoadDatabase p_LoadDatabase = nullptr;
PfnEngine_GetDbChecksum p_GetDbChecksum = nullptr;
PfnEngine_HasLoadedCustomDatabase p_HasCustomDb = nullptr;
PfnGE_Void p_SyncCaravanReagents = nullptr;  // the box-badge repaint
typedef void*(__cdecl* PfnObjectManagerGet)(void);
typedef const void*(__cdecl* PfnOM_LoadTableFile)(void*, const void* stdString);
typedef const void*(__cdecl* PfnOM_GetLoadTable)(const void*, const void* stdString);
typedef unsigned int(__cdecl* PfnLTB_GetNumElementsForField)(const void*, const char*);

PfnObjectManagerGet p_ObjectManagerGet = nullptr;
PfnOM_LoadTableFile p_LoadTableFile = nullptr;
PfnLTB_GetNumElementsForField p_GetNumElementsForField = nullptr;
const void* p_LoadTableBinaryVft = nullptr;
volatile LONG g_verifyDone = 0;
int g_verifyBoxes = -1;

void* t_ItemLoad = nullptr;
void* t_AddItemToReagents = nullptr;
void* t_TakeFromReagents = nullptr;
void* t_DepositReagents = nullptr;
void* t_DepositSack = nullptr;
void* t_DepositTransfer = nullptr;
void* t_DestroyObjectEx = nullptr;
void* t_LoadMainDatabase = nullptr;
void* t_LoadDatabase = nullptr;

PfnItem_Load o_ItemLoad = nullptr;
PfnGE_AddItemToReagents o_AddItemToReagents = nullptr;
PfnGE_TakeItemFromReagents o_TakeFromReagents = nullptr;
PfnCtrl_Void o_DepositReagents = nullptr;
PfnSack_DepositIntoReagents o_DepositSack = nullptr;
PfnGE_Void o_DepositTransfer = nullptr;
PfnOM_DestroyObjectEx o_DestroyObjectEx = nullptr;
PfnEngine_LoadMainDatabase o_LoadMainDatabase = nullptr;
PfnEngine_LoadDatabase o_LoadDatabase = nullptr;

// ---- state ----------------------------------------------------------------------------------
CRITICAL_SECTION g_cs;
bool g_csReady = false;

// The offset of the `craftingMaterial` bool inside GAME::Item, decoded from
// Item::IsReagentCompatible's own code bytes (`0F B6 81 <disp32> C3`). 0 = unknown -> gate off.
unsigned int g_flagOffset = 0;

std::unordered_set<std::string>* g_pageRecords = nullptr;  // lower-case record paths on our page

// The registry stores the object id NEXT TO the pointer, and every write into an Item re-checks
// both (ObjectManager::IsObjectOnDeletedList plus Object::GetObjectId == the id we stored) -
// the same identity test the read path (probeItem) uses. The engine reuses both ids and Item
// addresses, so neither alone identifies an object.
struct RegEntry {
    GdItem* item;
    unsigned int id;
};
std::vector<RegEntry>* g_registry = nullptr;  // live items whose record is ours
std::unordered_set<GdItem*>* g_registrySet = nullptr;

char g_arzPath[MAX_PATH] = {0};

// ---- the page switch -------------------------------------------------------------------------
// The exe builds a reagent sub-window in ReagentWindow::Load, which asks
// ObjectManager::LoadTableFile / GetLoadTable - two Engine.dll exports - for the record named by
// caravan_window.dbr's MaterialWindow field. Both detours below answer a request for THAT record
// with one of our own page records while uniq_page >= 0. Nothing else in the game is affected:
// the comparison is a size check plus one _stricmp, and the vanilla record is never overridden,
// so uniq_page = -1 gives the user their real Crafting Materials page back with no other change.
struct UniqPage {
    std::string record;  // records/ui/caravan/uniq_pNN.dbr
    std::string label;   // "Helms 1/8"
    int boxes;
};
std::vector<UniqPage>* g_pages = nullptr;
static const char kMaterialRecord[] = "records/ui/caravan/caravan_materialwindow.dbr";
static const size_t kMaterialLen = sizeof(kMaterialRecord) - 1;
volatile LONG g_pageSubs = 0;      // how often the path was substituted
volatile LONG g_pageSubLogged = 0;
// Per WORLD: this world's database has no page records of ours, so nothing is substituted in it.
volatile LONG g_frameUnavail = 0;
volatile LONG g_frameUnavailSaid = 0;  // the one info line the user gets for it
int g_verifyPageBoxes = -1;        // reagentBoxes on our page record, read back from the database
char g_pageLabel[96] = "vanilla materials";
volatile LONG g_depositDepth = 0;   // > 0 while an auto-deposit is on the stack
volatile LONG g_dbLoaded = 0;       // 1 once uniq.arz went in
volatile LONG g_dbTried = 0;
volatile LONG g_dbMainLoads = 0;    // how often Engine::LoadMainDatabase has run this process
volatile LONG g_dbReloads = 0;      // how often the overlay had to go in again, for the log
volatile LONG g_ourLoadDepth = 0;   // > 0 while OUR Engine::LoadDatabase call is on the stack
// > 0 while the engine is inside a database load. The late-load fallback runs on the WORKER
// thread and loads whenever it sees no overlay, so without this it could call
// Engine::LoadDatabase in parallel with the LoadMainDatabase that is busy replacing the database.
volatile LONG g_dbBusy = 0;
volatile LONG g_gateArmed = 0;      // MONOTONIC registration EVENTS since process start
volatile LONG g_gateLogged = 0;
volatile LONG g_regLogUsed = 0;     // registration LOG lines used this world/menu
volatile LONG g_worldActive = 0;    // 1 once a main player has been seen
volatile LONG g_depositCalls = 0;
volatile LONG g_addCalls = 0;
volatile LONG g_takeCalls = 0;
const char* g_dbRoute = "not attempted";
// The heartbeat line. It is assembled with _TRUNCATE, and liveStatus() - the only telemetry the
// live-paging code has - is appended LAST, so a buffer that is merely big enough silently eats
// that tail as the line grows. The observed payload is ~900 bytes; 1400 leaves real headroom.
char g_status[1400] = "reagent: idle";

// The registration log budget. The transfer stash deserialises at the MAIN MENU on the loader
// thread and registers hundreds of items there, so a single process-global budget is spent
// before the first character even loads and no `registered #N` line from a world is ever seen.
// The menu therefore has its own small budget, and the in-game budget is reset per world in
// teardownTables().
const int kGateLogMax = 60;
const int kGateLogMenuMax = 12;  // before the first world: just enough to prove it happens
// The choke point logs EVERY entry whose record is one of ours, and the first kEntryLogMax
// entries whose record is not (a vanilla reagent, or an id that resolves to nothing). This cap
// is per SESSION, not per world: an auto-deposit button can enter the choke point dozens of
// times per click and those entries are refused elsewhere anyway.
const LONG kEntryLogMax = 200;

// ---- the load-time keep-alive, and the map-owned prototype set --------------------------------
// `GameEngine::ReadPlayerReagents` (Game.dll rva 0x2CDF70, resolved BY NAME) re-creates every stored prototype from the replica saved in reagents.gst and then tests
// `cmp byte [proto+0xC64],0` at +0x441 (0x2CE3B1). The failing branch does NOT delete: at
// 0x2CE44C it calls Player::GiveItemToCharacter(proto, true, false) - a REFUND as a stock copy,
// because the saved replica carries only the record string and the count - and sets r12b, which
// makes the SAME CALL run `SaveReagents` at 0x2CE535. One load therefore refunds the collection
// AND erases it from reagents.gst. Identity is what is lost, not the items.
//
// `Item::CreateItem` builds through ObjectManager::CreateObjectFromFile(record, replica, doLoad=1)
// (doLoad hardcoded at 0x11961F), so `Item::Load` runs for every stored prototype INSIDE that
// bracket and nothing between Item::Load returning and the test at 0x2CE3B1 touches +0xC64. The
// mod's existing Item::Load detour is on the path and can set the byte before the engine reads
// it - that is the whole fix.
// Thread-local: the bracket is per call stack, so a load on any thread is handled and no other
// thread can be confused by it.
__declspec(thread) long g_addReagentsDepth = 0;  // > 0 while o_AddItemToReagents is on THIS stack
// The two drop bytes the soulbound bracket has just ZEROED, as UT_JF_* bits, for the duration of
// that bracket on this thread. hk_AddItemToReagents runs INSIDE the bracket on a drag
// (PrimaryReagentActivate calls AddItemToReagents at 0x17396D), so itemFlagBits() would read the
// zeroed bytes and journal flags=0 - the item taken back out would then not be soulbound and
// could be moved into a normal stash tab.
__declspec(thread) unsigned long g_bracketFlagBits = 0;
// ---- the DELIBERATE-PROBE depth ---------------------------------------------------------------
// The mod faults ON PURPOSE in a few places and handles the fault itself: the replica slot probe
// walks 8-aligned windows and dereferences whatever pointer they hold ("N faulted candidates" is
// an ordinary line in every session log), and copyProtoReplica reads a prototype that may have
// died. dllmain.cpp's vectored handler is registered FIRST and therefore sees those first-chance
// access violations BEFORE the __except that owns them - so without this counter it prints
// "***** EXCEPTION ... the mod does NOT handle it" for a fault the mod does handle, and burns one
// of its 32 lines on it. Raised around each deliberate probe; the handler returns immediately
// while it is non-zero, which also makes the 32-line cap mean UNEXPECTED faults.
__declspec(thread) long g_probeDepth = 0;
struct ProbeScope {  // no __try may live in the same function as this (C2712) - wrap the callee
    ProbeScope() { ++g_probeDepth; }
    ~ProbeScope() { --g_probeDepth; }
};
volatile LONG g_soulboundRestoreFailed = 0;  // brackets whose restore did NOT go in
volatile LONG g_mapOwnedKept = 0;     // map-owned prototypes the last sweep left armed
volatile LONG g_addEntries = 0;       // hk_AddItemToReagents ENTRIES
// Every object id the mod knows the engine's reagent map owns: created inside ReadPlayerReagents
// or inside AddItemToReagents, or read back out of a live map node. The byte on THESE stays 1.
// That is safe because +0xC64 has nine readers, all in Game.dll, and only ReadPlayerReagents+0x441
// ever sees a map-owned prototype. It must never be permanent on an item the player can pick up -
// Item::CanBePlacedInTransferStash+0x9 would then bar it from every stash tab.
std::unordered_set<unsigned int>* g_mapProtoIds = nullptr;
volatile LONG g_mapOwnedSize = 0;   // its size, readable without the lock (see forgetMapOwnedId)

// ---- the id map and the drop trace ------------------------------------------------------------
// The id map and the drop trace. Every item the engine puts into a sack this session is
// remembered by object id (the stash is deserialised at the main menu, so a plain menu launch
// already fills the map), and the drop trace explains a refusal:
// PrimaryReagentActivate / QuickDropInReagents require item[soulbound]==0 &&
// item[untradeable]==0 (Item::Load fills both from the record fields `soulbound` and
// `untradeable`); AddItemToReagents then requires item[craftingMaterial]!=0. All three offsets
// are decoded from the getters' own code bytes, never hard-coded.

std::unordered_map<unsigned int, GdItem*>* g_byId = nullptr;  // object id -> live Item*
std::unordered_map<GdItem*, unsigned int>* g_idOf = nullptr;  // and back, to erase on destroy
volatile LONG g_dropLogs = 0;
unsigned int g_soulboundOffset = 0;
unsigned int g_untradeableOffset = 0;

PfnItem_IsReagentCompatible p_IsSoulbound = nullptr;    // read only - never called
PfnItem_IsReagentCompatible p_IsUntradeable = nullptr;  // read only - never called
void* t_PrimaryReagentActivate = nullptr;
void* t_QuickDropInReagents = nullptr;
PfnCursor_Bool o_PrimaryReagentActivate = nullptr;
PfnCursor_Bool o_QuickDropInReagents = nullptr;

// ---- the collection policy's choke point ------------------------------------------------------
// The collection semantics live in ONE place: hk_AddItemToReagents. Every route into the reagent
// map goes through GameEngine::AddItemToReagents, which has exactly four callers:
// CursorHandlerItemMove::{PrimaryReagentActivate, QuickDropInReagents, Cancel} and
// InventorySack::DepositSackIntoReagents. Refusing there (return false WITHOUT calling the
// original) is strictly better than refusing in PrimaryReagentActivate, because the engine's own
// failure branch then runs: it pops the "tagTransferStashError" message and, crucially, does NOT
// reach SendRemoveItemFromInventory - so the item stays on the cursor and the user is told why.
PfnGE_GetReagentItemCount p_GetReagentItemCount = nullptr;
PfnItem_GetU32 p_GetSeedRerolls = nullptr;    // read only - its bytes carry the offset
PfnItem_GetU32 p_GetAffixRerolls = nullptr;   // read only
PfnItem_GetU32 p_GetPrefixClass = nullptr;    // read only
PfnItem_GetU32 p_GetSuffixClass = nullptr;    // read only
PfnOM_ObjectFromId p_ObjectFromId = nullptr;  // decoded out of TakeItemFromReagents(id,int)
void* t_TakeFromReagentsId = nullptr;
void* t_Cancel = nullptr;
PfnGE_TakeItemFromReagentsId o_TakeFromReagentsId = nullptr;
PfnCursor_Bool o_Cancel = nullptr;

unsigned int g_replicaOffset = 0;      // Item + off == ItemReplicaInfo (0x538 on 1.3.0.8)
// sizeof(ItemReplicaInfo), decoded from two agreeing sources (ut_replicasize.h; 0x190 on
// 1.3.0.8). 0 = not confirmed, and then the gate turns the mod off: nothing falls back to 0x190.
unsigned int g_replicaSize = 0;
unsigned int g_replicaSizeCtor = 0;    // what Item::Item gave
unsigned int g_replicaSizeAssign = 0;  // what ItemReplicaInfo::operator= gave
// The offset INSIDE the ItemReplicaInfo of the stack-size mirror. Decoded at
// run time from Item::SetStackSize's own two stores (`mov [rcx+0x88C],edx; mov [rcx+0x6B0],edx;
// ret`): the second one is the replica mirror, so replicaStackOff = 0x6B0 - g_replicaOffset.
// 0 = not decoded, and then the journal's own stack value is kept on a restore.
unsigned int g_replicaStackOff = 0;
unsigned int g_stackMirrorInItem = 0;
unsigned int g_seedRerollsOffset = 0;
unsigned int g_affixRerollsOffset = 0;
unsigned int g_prefixClassOffset = 0;
unsigned int g_suffixClassOffset = 0;
GdGameEngine* g_gameEngine = nullptr;  // captured, never owned
volatile LONG g_refusals = 0;
volatile LONG g_accepts = 0;
volatile LONG g_takeIdCalls = 0;
volatile LONG g_cancelCalls = 0;
volatile LONG g_reconcileFailed = 0;
volatile LONG g_creationLogs = 0;
volatile LONG g_transferOpenNow = 0;  // the take-probe only logs while the caravan is up
std::map<std::string, int>* g_counts = nullptr;  // our tally, reconciled with the engine's map

// Observation only - the three item-creation routes the reagent-box take could use.
typedef GdItem*(__cdecl* PfnItem_CreateItem)(const void* replica);
typedef GdItem*(__cdecl* PfnCC_CreateItemInInventory)(void* self, const void* stdString);
typedef void(__cdecl* PfnGE_CreateItemForCharacter)(GdGameEngine*, unsigned int, const void*,
                                                    void*, void*);
void* t_ItemCreateItem = nullptr;
void* t_CreateItemInInventory = nullptr;
void* t_CreateItemForCharacter = nullptr;
PfnItem_CreateItem o_ItemCreateItem = nullptr;
PfnCC_CreateItemInInventory o_CreateItemInInventory = nullptr;
PfnGE_CreateItemForCharacter o_CreateItemForCharacter = nullptr;

// ---- the stacking fix -------------------------------------------------------------------------
// GameEngine::AddItemToReagents rva 0x2CEC10, merge branch at +0x1C5:
//     edi = node[0x40] (the stored PROTOTYPE item id) -> resolve -> rsi
//     if (!rsi) return true;                       <- the item is consumed and nothing happens
//     edx = incoming->vtable[0x618]()              == Item::GetStackSize
//     rsi->vtable[0x5F8](edx, &out)                == IncrementStack
//     node[0x44] = rsi->vtable[0x618]()            == the new box count
// ItemEquipment::IncrementStack is the folded `xor al,al; ret` stub at rva 0x1A6E0 (525 exports
// share it), so for our uniques the merge is a NO-OP: the count never rises and the item is gone.
// The fix writes the prototype's stack size ourselves through the exported, never-overridden
// Item::SetStackSize BEFORE calling the original, so the engine's own `node[0x44] = GetStackSize()`
// stores the right number. Nothing is patched, nothing is detoured, and the write only happens
// when the prototype's vtable slot really is the no-op stub.
typedef const void*(__cdecl* PfnGE_GetPlayerReagents)(const GdGameEngine*);
typedef void(__cdecl* PfnItem_SetU32)(GdItem*, unsigned int);
PfnGE_GetPlayerReagents p_GetPlayerReagents = nullptr;
PfnItem_GetU32 p_GetStackSize = nullptr;
PfnItem_SetU32 p_SetStackSize = nullptr;
volatile LONG g_refreshes = 0;         // FRESH-PROTOTYPE swaps performed
volatile LONG g_refreshFails = 0;      // ...and refused or faulted
const void* p_EquipIncrementStub = nullptr;  // ItemEquipment::IncrementStack == the no-op stub
volatile LONG g_stackBumps = 0;              // how many deposits the fix rescued
volatile LONG g_deadProtoRefusals = 0;       // deposits refused because the prototype is gone
volatile LONG g_stackRollbacks = 0;

// MSVC _Tree_node: {_Left, _Parent, _Right, _Color, _Isnil, pad, _Myval}. `_Myval` is
// pair<const std::string, ReagentData> - the key at +0x20 (32-byte MSVC string) and
// ReagentData{u32 itemId; u32 count;} at +0x40. Both offsets come straight out of the engine:
// GetReagentItemCount does `lea rdx,[rax+0x20]` for the key and `mov ebx,[rbx+0x40]` for the id,
// AddItemToReagents does `mov [rbx+0x44],eax` for the count.
struct ReagentNode {
    ReagentNode* left;
    ReagentNode* parent;
    ReagentNode* right;
    unsigned char color;
    unsigned char isnil;
};
const unsigned int kNodeKeyOff = 0x20;
const unsigned int kNodeIdOff = 0x40;
const unsigned int kNodeCountOff = 0x44;

PfnOM_IsObjectOnDeletedList p_IsObjectOnDeletedList = nullptr;
PfnOM_IsObjectIdOnDeletedList p_IsObjectIdOnDeletedList = nullptr;
PfnEngine_GetGameInfo p_GetGameInfo = nullptr;
PfnGameInfo_GetIsMultiPlayer p_GetIsMultiPlayer = nullptr;
// OPTIONAL. A miss costs nothing - the heartbeat prints -1 for that field.
PfnGameInfo_GetBool p_GetIsServer = nullptr;
PfnGameInfo_GetUInt p_GetNumOfPlayers = nullptr;
PfnGameInfo_GetUInt p_GetGameMode = nullptr;
// THE COLLECTION MODE. Not telemetry and not optional in spirit - without it the mod has
// no mode and the collection stays hidden - but a miss must not take the rest of the session
// down, so it is resolved like the others and answered with -1 plus a reason where it is read.
PfnGameInfo_GetBool p_GetHardcore = nullptr;
// ENGINE state, not lobby state - these three say precisely whether an ActorConfigCommand
// round-trips in this session. OPTIONAL, and never a gate.
PfnGameInfo_GetBool p_IsNetEnabled = nullptr;
PfnGameInfo_GetBool p_IsNetServer = nullptr;
PfnGameInfo_GetBool p_IsNetClient = nullptr;
void* t_ExitPlayingMode = nullptr;
PfnGE_ExitPlayingMode o_ExitPlayingMode = nullptr;

volatile LONG g_mpActive = 0;       // last observed GameInfo::GetIsMultiPlayer (heartbeat mp=)
volatile LONG g_mpKnown = 0;        // 1 once the pair of exports resolved and was polled
volatile LONG g_mpTick = 0;         // GetTickCount of the last poll (throttle)
// The tri-state session mode and the telemetry that rides with it.
volatile LONG g_mpMode = 0;          // 0 single, 1 multiplayer, 2 UNKNOWN
volatile LONG g_mpHost = -1;         // GameInfo::GetIsServer, -1 = unavailable
volatile LONG g_mpPlayers = -1;      // GameInfo::GetNumOfPlayers, -1 = unavailable
volatile LONG g_mpGameMode = -1;     // GameInfo::GetMode, -1 = unavailable
volatile LONG g_mpCollectSeen = -1;  // the mp_collect the last edge line printed
volatile LONG g_mpPolicyLogged = 0;  // the one-per-session policy line
volatile LONG g_mpUnknownWarned = 0; // the one-per-session UNKNOWN + mp_collect=0 warning
volatile LONG g_mpNoteLogged = 0;    // the one-per-session "what left this machine" line
volatile LONG g_mpPendingCount = 0;  // deposits still awaiting their 5 s MP-removal measurement
                                     // (also the lock-free fast path in hk_DestroyObjectEx)
volatile LONG g_mpInfoSeen = 0;      // 1 once a non-null GameInfo was read this process
volatile LONG g_sbBarredId = 0;      // the cursor id the soulbound-MP refusal line last named
volatile LONG g_deadWrites = 0;     // writes refused because the Item was not live
volatile LONG g_regIdRefresh = 0;   // registry entries whose Item address the engine reused
volatile LONG g_mpRefusals = 0;
volatile LONG g_depositRefusals = 0;
volatile LONG g_clears = 0;
volatile LONG g_gateForcedOff = 0;  // set when a deposit guard hook did not install

// An RAII guard, so a std::bad_alloc thrown inside a container operation can never unwind past a
// manual unlock() and leave g_cs held forever - which would hang the game the next time the
// render thread took it.
struct Guard {
    Guard() {
        if (g_csReady) EnterCriticalSection(&g_cs);
    }
    ~Guard() {
        if (g_csReady) LeaveCriticalSection(&g_cs);
    }

   private:
    Guard(const Guard&);
    Guard& operator=(const Guard&);
};

// ---- an MSVC std::string the engine can read -------------------------------------------------
// MSVC layout, confirmed by Engine::LoadDatabase itself (`cmp [str+0x18], 0x10; jb inline;
// mov rbx,[str]`): { union { char buf[16]; char* ptr; }; size_t size; size_t capacity; }.
// Built by hand so no assumption about our own <string> ABI is involved. The callee only reads.
struct MsvcString {
    union {
        char buf[16];
        char* ptr;
    } u;
    size_t size;
    size_t capacity;
};

std::string* g_stringHeap = nullptr;

void makeString(MsvcString* s, const char* text) {
    memset(s, 0, sizeof(*s));
    const size_t n = strlen(text);
    s->size = n;
    if (n < 16) {
        memcpy(s->u.buf, text, n + 1);
        s->capacity = 15;
    } else {
        if (!g_stringHeap) g_stringHeap = new std::string();
        *g_stringHeap = text;
        s->u.ptr = const_cast<char*>(g_stringHeap->c_str());
        s->capacity = n;
    }
}

// ---- helpers ---------------------------------------------------------------------------------
void toLower(std::string* s) {
    for (size_t i = 0; i < s->size(); ++i) {
        char c = (*s)[i];
        if (c >= 'A' && c <= 'Z') (*s)[i] = (char)(c - 'A' + 'a');
        if (c == '\\') (*s)[i] = '/';
    }
}

bool isOurRecord(const char* name) {
    if (!name || !*name || !g_pageRecords) return false;
    std::string key(name);
    toLower(&key);
    return g_pageRecords->find(key) != g_pageRecords->end();
}

// isOurRecord builds a std::string and hits an unordered_set, so it can throw. Every C++ body
// reachable from a detour must swallow its own exceptions: hk_PrimaryReagentActivate has no
// try/catch of its own, and a throw there would unwind a C++ exception into the engine's vtable
// dispatch at exe 0x13298D. Same shape the choke point uses.
bool isOurRecordSafe(const char* name) {
    try {
        return isOurRecord(name);
    } catch (...) {
        return false;
    }
}

unsigned int safeObjectId(GdItem* item);

// ---- is this Item* still the object we think it is? -------------------------------------------
// Engine.dll exports ObjectManager::IsObjectOnDeletedList and IsObjectIdOnDeletedList. Both are
// asked, and then the stored object id is re-checked against Object::GetObjectId - the same test
// probeItem already used on the READ path. Only when all of that passes may a byte be written.
// Unknown (an export missing) is treated as NOT live: the gate simply stays disarmed.
bool itemIsLive(GdItem* item, unsigned int wantId) {
    if (!item) return false;
    if (!p_ObjectManagerGet || !p_IsObjectOnDeletedList) return false;
    __try {
        void* om = p_ObjectManagerGet();
        if (!om) return false;
        if (p_IsObjectOnDeletedList(om, item)) return false;
        if (wantId && p_IsObjectIdOnDeletedList && p_IsObjectIdOnDeletedList(om, wantId)) {
            return false;
        }
        if (wantId && g_gd.ObjectGetObjectId) {
            if (g_gd.ObjectGetObjectId(item) != wantId) return false;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- THE MULTIPLAYER POLICY -------------------------------------------------------------------
// The collection works for the LOCAL player in any session mode. Other players need nothing
// installed and are never affected. The evidence for that, all measured against 1.3.0.8:
//   * The reagent map, reagents.gst and the Crafting Materials page are 100% local and
//     file-backed. A full .pdata-bounded call-target scan of AddItemToReagents,
//     TakeItemFromReagents (both overloads), Read/WritePlayerReagents, Save/LoadPlayerReagents,
//     ClearReagents, GetReagentItemCount and SyncCaravanReagents finds ZERO network-shaped calls
//     and no host/client branch. Each machine has its own reagents.gst (GetSharedSavePath ->
//     SaveManager::DirectRead/DirectWrite).
//   * Item+0xC64 - the gate byte - has nine disp32 sites, ALL in Game.dll, 0 in Engine.dll and 0
//     in the decrypted exe image, and not one is a serialiser, a replicator or a packet builder.
//     The byte cannot reach a peer because no code that could put it on the wire ever reads it.
//   * The identity substitution is deep-copied into engine storage by SetItemReplicaInfo and the
//     object id is overwritten by the engine, so once Item::CreateItem returns there is no
//     pointer into mod memory anywhere in the engine's Item, and nothing mod-specific exists to
//     replicate.
//   * What DOES leave this machine on a deposit is the engine's own
//     ControllerCharacter::SendRemoveItemFromInventory command - packet type 0xB6, carrying
//     {characterId, itemId} and nothing else - the identical message vanilla sends for an Aether
//     Crystal. The mod neither builds it nor changes it.
//   * The TAKE is the other direction and it is NOT bare. AddInventoryItemConfigCmd ships packet
//     type 0xB7 (0x310 B) with a full ItemReplicaInfo at +0x180, filled by
//     Item::GetItemReplicaInfo inside GetNetPacket (Game.dll 0xD4C56) - i.e. strictly AFTER
//     Item::CreateItem returned, and so after the mod's substitution, which happens inside that
//     very call. The peer therefore sees the RESTORED item, not a stock copy; a failed restore is
//     what would send a stock one, and the "identity: NOT restored" line says so when the session
//     is not single player.
// `mp_collect=0` bars the collection outside single player. The ONE thing that stays barred in a
// multiplayer session whatever mp_collect says is the soulbound/untradeable DRAG bracket: +0xC00
// and +0xC02 are gameplay rules living in a shared object's own state. A SHIFT-CLICK gets the
// same item in anyway, because the exe's quick-move site has no soulbound test at all.
//
// The mode is a TRI-STATE. A boolean would have to fail OPEN - an unresolved export reading as
// "single player" silently defeats an explicit mp_collect=0. UNKNOWN means "the mod could not
// ask"; with mp_collect=0 it is treated as MULTIPLAYER, so the explicit setting wins over a
// missing export.
const int UT_MP_SINGLE = 0;
const int UT_MP_MULTI = 1;
const int UT_MP_UNKNOWN = 2;

// SEH only - no C++ object may live in a function that carries __try (C2712).
int readSessionMode() {
    if (!p_GetGameInfo || !p_GetIsMultiPlayer || !g_gd.ppEngine || !*g_gd.ppEngine)
        return UT_MP_UNKNOWN;
    __try {
        void* info = p_GetGameInfo(*g_gd.ppEngine);
        // No GameInfo object at all = no session = not multiplayer, and that is what the main
        // menu looks like. But once a session object HAS been seen, a later null means "the mod
        // could not ask", not "single player": otherwise a transient null inside a live co-op
        // session would un-bar mp_collect=0 and open the soulbound bracket for up to the 500 ms
        // cache window. UNKNOWN is the fail-CLOSED answer.
        if (!info)
            return InterlockedCompareExchange(&g_mpInfoSeen, 0, 0) ? UT_MP_UNKNOWN : UT_MP_SINGLE;
        InterlockedExchange(&g_mpInfoSeen, 1);
        return p_GetIsMultiPlayer(info) ? UT_MP_MULTI : UT_MP_SINGLE;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return UT_MP_UNKNOWN;
    }
}

// ---- THE COLLECTION MODE, ASKED OF THE CHARACTER HIMSELF --------------------------------------
// GameInfo::GetHardcore is the loaded character's own hardcore flag, and it is what decides which
// of the two shared crafting stashes - and so which of the two collections - is his. One journal
// per mode, and the mod must never show one mode's collection to the other.
//
// GameEngine+0x375BA is NOT that flag, however plausible it looks: it reads 1 on a hardcore
// character and 1 again on a softcore one, which puts a softcore player in front of an empty
// hardcore collection. Do not reintroduce it.
//
// Shape: a const member returning bool, so the GameInfo pointer is the only argument (see
// readSessionMode above for the same shape on GetIsMultiPlayer). SEH only - no C++ object may
// live in a function that carries __try (C2712). Answers 0 softcore, 1 hardcore, or -1 with a
// reason the caller prints; the three ways it can fail read differently on purpose.
int readCollectionMode(const char** why) {
    const char* sink = "";
    if (!why) why = &sink;
    if (!p_GetHardcore) {
        *why = "GameInfo::GetHardcore is not exported by this Engine.dll";
        return -1;
    }
    if (!p_GetGameInfo || !g_gd.ppEngine || !*g_gd.ppEngine) {
        *why = "the Engine singleton has not been captured yet";
        return -1;
    }
    __try {
        void* info = p_GetGameInfo(*g_gd.ppEngine);
        if (!info) {
            *why = "Engine::GetGameInfo answered null - no session object yet";
            return -1;
        }
        const int v = p_GetHardcore(info) ? 1 : 0;
        *why = "";
        return v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *why = "the call to GameInfo::GetHardcore faulted";
        return -1;
    }
}

// Telemetry, never a gate. -1 in a field means "unavailable".
void readMpTelemetry(int* host, int* players, int* mode) {
    *host = -1;
    *players = -1;
    *mode = -1;
    if (!p_GetGameInfo || !g_gd.ppEngine || !*g_gd.ppEngine) return;
    __try {
        void* info = p_GetGameInfo(*g_gd.ppEngine);
        if (!info) return;
        if (p_GetIsServer) *host = p_GetIsServer(info) ? 1 : 0;
        if (p_GetNumOfPlayers) *players = (int)p_GetNumOfPlayers(info);
        if (p_GetGameMode) *mode = (int)p_GetGameMode(info);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *host = -1;
        *players = -1;
        *mode = -1;
    }
}

// The SESSION SHAPE, from Engine state rather than lobby state.
// "off" = Engine::IsNetworkEnabled is 0, i.e. the StubConnectionManager, i.e. Actor::Enqueue takes
// its LOCAL branch and every inventory command executes synchronously. "server" and "client" both
// SEND: a host's ServerConnectionManager is connected to itself through ConnectToLoopback, so a
// host round-trips exactly like a client, one network pump later. Telemetry only, never a gate.
const char* readNetShape() {
    if (!p_IsNetEnabled || !g_gd.ppEngine || !*g_gd.ppEngine) return "?";
    __try {
        if (!p_IsNetEnabled(*g_gd.ppEngine)) return "off";
        if (p_IsNetServer && p_IsNetServer(*g_gd.ppEngine)) return "server";
        if (p_IsNetClient && p_IsNetClient(*g_gd.ppEngine)) return "client";
        return "on";
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "?";
    }
}

// The three-state query. Polled at most every 500 ms, exactly as the old boolean was; every
// state change - and every change of mp_collect - prints one line.
int mpSessionMode() {
    if (!InterlockedExchange(&g_mpPolicyLogged, 1)) {
        logD("reagent gate: multiplayer policy - mp_collect=%d (1 = the collection works in "
             "every session mode for the local player; other players need nothing)",
             g_cfg.mpCollect);
    }
    const LONG now = (LONG)GetTickCount();
    const LONG last = InterlockedCompareExchange(&g_mpTick, 0, 0);
    if (InterlockedCompareExchange(&g_mpKnown, 0, 0) && (DWORD)(now - last) < 500)
        return (int)InterlockedCompareExchange(&g_mpMode, 0, 0);
    InterlockedExchange(&g_mpTick, now);
    const LONG v = (LONG)readSessionMode();
    int host = -1, players = -1, mode = -1;
    readMpTelemetry(&host, &players, &mode);
    InterlockedExchange(&g_mpHost, host);
    InterlockedExchange(&g_mpPlayers, players);
    InterlockedExchange(&g_mpGameMode, mode);
    InterlockedExchange(&g_mpActive, v == UT_MP_MULTI ? 1 : 0);
    const LONG collect = g_cfg.mpCollect ? 1 : 0;
    const LONG was = InterlockedExchange(&g_mpMode, v);
    const LONG wasCollect = InterlockedExchange(&g_mpCollectSeen, collect);
    if (!InterlockedExchange(&g_mpKnown, 1) || was != v || wasCollect != collect) {
        const char* verdict = (!collect && v != UT_MP_SINGLE) ? "DISABLED" : "ENABLED";
        // `net=` is the Engine's own shape and it is the field that says whether commands
        // round-trip; host=/players=/mode= are lobby facts, printed on EVERY variant so that a
        // null-GameInfo read is visible as -1/-1/-1.
        const char* net = readNetShape();
        if (v == UT_MP_SINGLE) {
            logD("reagent gate: GameInfo::GetIsMultiPlayer = %d - single player - collection "
                 "ENABLED (mp_collect=%d, net=%s, host=%d, players=%d, mode=%d)",
                 0, (int)collect, net, host, players, mode);
        } else if (v == UT_MP_MULTI) {
            logD("reagent gate: GameInfo::GetIsMultiPlayer = 1 - multiplayer session - "
                 "collection %s (mp_collect=%d, net=%s, host=%d, players=%d, mode=%d)",
                 verdict, (int)collect, net, host, players, mode);
        } else {
            logD("reagent gate: GameInfo::GetIsMultiPlayer could not be read - the session mode "
                 "is UNKNOWN - collection %s (mp_collect=%d, net=%s, host=%d, players=%d, "
                 "mode=%d)",
                 verdict, (int)collect, net, host, players, mode);
        }
    }
    if (v == UT_MP_UNKNOWN && !collect && !InterlockedExchange(&g_mpUnknownWarned, 1)) {
        logW("reagent gate: WARNING - GameInfo::GetIsMultiPlayer is unavailable; with "
             "mp_collect=0 the mod treats the session as multiplayer");
    }
    return (int)v;
}

// "This is a multiplayer session." The ONLY gate that uses it is the soulbound bracket;
// everything else asks mpBarred().
bool multiplayerActive() { return mpSessionMode() == UT_MP_MULTI; }

// THE multiplayer conjunct of the gate: the collection is barred only when the user asked for it
// to be (mp_collect=0) AND this is not provably single player.
bool mpBarred() {
    const int m = mpSessionMode();  // always poll: the edge line is telemetry, not a gate
    if (g_cfg.mpCollect) return false;
    return m != UT_MP_SINGLE;  // multiplayer, or UNKNOWN - the explicit mp_collect=0 wins
}

// ---- the MP REMOVAL MEASUREMENT ---------------------------------------------------------------
// It performs NO engine write, undoes nothing, refuses nothing and warns about nothing. It prints
// ONE informational line per deposit, 5 s later, saying whether the source Item object was still
// alive at that moment. It must stay a measurement: a surviving object is not an error, so
// nothing here may be turned into an alarm or a rollback.
//
// The bag slot is emptied LOCALLY and SYNCHRONOUSLY on every machine, in the same click, before
// anything reaches the network: exe site A runs AddItemToReagents (0x1EAB38) ->
// PlayerInventoryCtrl::RemoveItem(ic,id,true) (0x1EAB4E - it erases the id from the ctrl's own bag
// grid map) -> SendRemoveItemFromInventory (0x1EAB5D), and the drag route clears the cursor itself
// at Game.dll 0x17399A. So the item is out of the bag on this machine whatever the network does,
// and it cannot be deposited twice.
//
// What the round trip decides is only when the Item OBJECT is destroyed. The host's
// Execute@RemoveInventoryItemConfigCmd -> Character::TakeItemFromCharacter -> DestroyObjectEx runs
// on the machine that receives the command, and there is no echo back to a client and no
// destruction replication on this path: a full .pdata call-target scan of DestroyObjectEx finds
// no packet, no connection manager and no shim, and Actor::LocalEnqueue has exactly two callers,
// so a received command is executed, not relayed. Therefore on a CLIENT hk_DestroyObjectEx may
// never fire for the deposited source item, and a surviving object is EXPECTED, not a fault. On a
// host it does fire, one loopback pump later - a host SENDS too, through ConnectToLoopback; only
// single player is synchronous.
//
// Consequences enforced here: hk_DestroyObjectEx is NOT the confirmation of a completed deposit,
// so it only MARKS the entry; the line is printed either way, and it is a measurement, not a
// verdict.
struct MpPending {
    unsigned int id;
    DWORD tick;
    int stage;      // 0 = nothing said yet, 1 = the line is out (the entry is then dropped)
    int destroyed;  // 1 once hk_DestroyObjectEx has seen this id on this machine
    char record[80];
};
const int kMpPendingMax = 64;
CRITICAL_SECTION g_mpCs;
bool g_mpCsReady = false;
MpPending g_mpPending[kMpPendingMax];
int g_mpPendingUsed = 0;

void mpPendingNote(unsigned int id, const char* record) {
    if (!g_mpCsReady || !id) return;
    EnterCriticalSection(&g_mpCs);
    int at = -1;
    for (int i = 0; i < g_mpPendingUsed; ++i) {
        if (g_mpPending[i].id == id) {
            at = i;
            break;
        }
    }
    if (at < 0 && g_mpPendingUsed < kMpPendingMax) at = g_mpPendingUsed++;
    if (at >= 0) {
        g_mpPending[at].id = id;
        g_mpPending[at].tick = GetTickCount();
        g_mpPending[at].stage = 0;
        g_mpPending[at].destroyed = 0;
        _snprintf_s(g_mpPending[at].record, sizeof(g_mpPending[at].record), _TRUNCATE, "%s",
                    record ? record : "?");
    }
    InterlockedExchange(&g_mpPendingCount, (LONG)g_mpPendingUsed);
    LeaveCriticalSection(&g_mpCs);
}

void mpPendingForgetAt(int i) {  // the caller holds g_mpCs
    g_mpPending[i] = g_mpPending[g_mpPendingUsed - 1];
    --g_mpPendingUsed;
    InterlockedExchange(&g_mpPendingCount, (LONG)g_mpPendingUsed);
}

// Called from hk_DestroyObjectEx (game thread). It only MARKS: this detour is the engine
// destroying the object, which happens in single player and on the machine that executes the
// removal command, and on a client may never happen at all - so it is NOT the confirmation of a
// completed deposit and nothing may be made to depend on it.
void mpPendingClear(unsigned int id) {
    if (!g_mpCsReady || !id) return;
    if (!InterlockedCompareExchange(&g_mpPendingCount, 0, 0)) return;
    EnterCriticalSection(&g_mpCs);
    for (int i = 0; i < g_mpPendingUsed; ++i) {
        if (g_mpPending[i].id == id) {
            g_mpPending[i].destroyed = 1;
            break;
        }
    }
    LeaveCriticalSection(&g_mpCs);
}

// Worker thread, once a second (never the game thread: this only reads mod-owned memory - the
// liveness answer comes from hk_DestroyObjectEx's own mark, not from an engine call here).
void mpPendingTick() {
    if (!g_mpCsReady) return;
    if (!InterlockedCompareExchange(&g_mpPendingCount, 0, 0)) return;
    const DWORD now = GetTickCount();
    EnterCriticalSection(&g_mpCs);
    for (int i = 0; i < g_mpPendingUsed;) {
        const unsigned int age = (unsigned int)(now - g_mpPending[i].tick);
        if (age >= 5000 && g_mpPending[i].stage == 0) {
            logD("reagent collect: MP removal - source item id=%u %s is %s %u ms after the "
                 "deposit (the bag slot was emptied locally by the exe at deposit time; the "
                 "object itself is destroyed only when the host's command is applied back on "
                 "this machine, which on a client may never happen - this line is a measurement, "
                 "not a fault)",
                 g_mpPending[i].id, g_mpPending[i].record,
                 g_mpPending[i].destroyed ? "destroyed" : "alive", age);
            mpPendingForgetAt(i);  // said once; the mod has nothing further to add
            continue;
        }
        ++i;
    }
    LeaveCriticalSection(&g_mpCs);
}

int wantedPage();  // defined with the page switch below

// Which collection page is on screen right now, or -1 for "the user's own vanilla materials page".
//
// With live paging on, the live GROUP is the page selection - the live re-point never touches
// `uniq_page`, so reading that field here would keep the gate shut for the whole session and
// every deposit would be refused by the engine.
//   live_pages=1 : the shown group, -1 while the vanilla materials layout is up (and -1 when live
//                  paging failed to arm - the page is then the vanilla one, so the gate must stay
//                  shut exactly as it does with no collection page selected);
//   live_pages=0 : the `uniq_page` ini value, which needs a character reload to take effect.
int shownCollection() {
    if (g_cfg.livePages) return liveActive() ? liveShownGroup(nullptr) : -1;
    return wantedPage();
}

// The one place that decides whether the mod may touch an item at all.
//
// The page test matters: with no collection page shown, the Crafting Materials page is the user's
// own vanilla page, and a unique dropped on it would be swallowed into the reagent map where no
// box shows it. The gate therefore stays shut and the game refuses the drop itself, exactly as it
// does without the mod. The page-RECORD set stays the loaded record count in full: a deposit of a
// record whose group is not the one on screen is legitimate - it is stored by record and shows up
// when its own group is scrolled to - so only "no collection at all" closes the gate.
//
// THE SCOPE IS A RULE, not an accident. The window (caravan open + the Materials page + a group
// shown) bounds it, and the byte is only ever written on an item the LOCAL player owns, because
// `registerItem` is fed exclusively by `hk_ItemLoad` and by `observeAdd` on
// `InventorySack::AddItem` - i.e. only the local player's own sacks. Nothing must ever arm an
// item the mod learned about any other way.
bool gateAllowed() {
    return g_flagOffset && shownCollection() >= 0 &&
           !InterlockedCompareExchange(&g_gateForcedOff, 0, 0) && !mpBarred();
}

// ---- GATE SCOPING -----------------------------------------------------------------------------
// The craftingMaterial byte is what makes Item::CanBePlacedInTransferStash (vtable +0x408) return
// false, and that ONE byte locks the item out of every stash tab, out of the exe's quick-move
// (0x1EAB7E) and out of Cancel's fallback. Leaving it set for as long as a group is selected
// would span the whole caravan session and beyond - the group survives a close - so a marked
// unique could be put nowhere else while the mod was on.
//
// The byte therefore lives only in the narrowest window in which it is actually needed: the
// caravan is open, the Crafting Materials page is the page on screen, and a collection group is
// shown. The moment any of that stops being true the byte is written back to 0 on every item the
// mod armed, so CanBePlacedInTransferStash answers true again and the item behaves exactly as it
// does without the mod.
//
// Accepted consequence: while a unique group IS on screen, a shift-click on a
// collection record deposits into the collection (the exe tries AddItemToReagents first at
// 0x1EAB38 and the mod cannot pre-empt that site). A refusal is safe: 0x1EAB3E `test al,al ;
// je 0x1EAB78` falls through to CanBePlacedInTransferStash and then to tagTransferStashError,
// and PlayerInventoryCtrl::RemoveItem / SendRemoveItemFromInventory are only ever reached on the
// SUCCESS branch (0x1EAB4E / 0x1EAB5D), so a refused deposit leaves the item exactly where it
// was. Verified in out/gd-exe-image.bin, 0x1EAB05..0x1EABB6.
//
// `g_transferOpenNow` must be part of the CONDITION, not just of the disarm reason. The
// caravan-close edge sets that flag to 0 and then calls gateApply(force), but hk_SetTransferOpen
// runs the mod BEFORE the engine's own SetTransferOpen, so the engine's IsTransferOpen byte - and
// therefore ut_plate's g_stashOpen snapshot, taken a frame earlier - is still 1 at that instant.
// Without the flag here, gateShouldArm() re-evaluated on the CLOSE edge answers "arm", and the
// edge that exists to clear the byte sets it instead.
bool gateShouldArm() {
    return gateAllowed() && InterlockedCompareExchange(&g_transferOpenNow, 0, 0) != 0 &&
           liveMaterialsVisible();
}

// Why the gate is NOT armed right now, for the one edge line the log gets.
const char* gateDisarmReason() {
    if (!g_flagOffset) return "the craftingMaterial offset is unknown";
    if (InterlockedCompareExchange(&g_gateForcedOff, 0, 0)) return "the gate is FORCED OFF";
    // Reachable only with mp_collect=0.
    if (mpBarred()) {
        return mpSessionMode() == UT_MP_UNKNOWN
                   ? "the session mode is UNKNOWN and mp_collect=0"
                   : "a multiplayer session is active and mp_collect=0";
    }
    if (shownCollection() < 0) return "no collection group is shown";
    if (!InterlockedCompareExchange(&g_transferOpenNow, 0, 0)) return "the caravan is closed";
    const int st = plateCaravanState();
    if (st == 1) return "the Materials page is not the visible caravan page";
    if (st == 0) return "the caravan page could not be read";
    // Everything this function can test says "arm", so the only refuser left is
    // plateMaterialsVisible()'s OWN caravan-open snapshot (ut_plate g_stashOpen, refreshed inside
    // plateAssertPlate from GameIsTransferOpen). Say so instead of printing "?".
    return "plateMaterialsVisible() refuses: ut_plate's caravan-open snapshot is 0 "
           "(plateAssertPlate has not run this frame)";
}

// ---- the map-owned prototype set --------------------------------------------------------------
// Membership means "the engine created this object inside ReadPlayerReagents or inside
// AddItemToReagents, or it is the object a live map node points at". Those objects are never on
// the cursor, never in a sack and never in a stash tab, so a permanent +0xC64 = 1 on them has
// exactly one effect: ReadPlayerReagents+0x441 keeps the entry instead of refunding it.
// `Item::IsReagentCompatible` (Game.dll 0x314AE0) has ZERO callers on 1.3.0.8 and is not imported
// by the exe, so nothing iterates the map asking that question either.
bool isMapOwnedId(unsigned int id) {
    if (!id) return false;
    try {
        Guard g;
        if (!g_mapProtoIds) return false;
        return g_mapProtoIds->find(id) != g_mapProtoIds->end();
    } catch (...) {
        return false;
    }
}

void noteMapOwnedId(unsigned int id) {
    if (!id) return;
    try {
        Guard g;
        if (g_mapProtoIds) {
            g_mapProtoIds->insert(id);
            InterlockedExchange(&g_mapOwnedSize, (LONG)g_mapProtoIds->size());
        }
    } catch (...) {
    }
}

// The engine reuses object ids. An id whose object has been destroyed must leave the set at once,
// or a later item that inherits it could never be disarmed.
// `ObjectManager::DestroyObjectEx` is called for EVERY object the engine destroys, so the empty
// case must not take a lock or an engine call: g_mapOwnedSize is the unlocked fast path.
void forgetMapOwnedId(unsigned int id) {
    if (!id || !InterlockedCompareExchange(&g_mapOwnedSize, 0, 0)) return;
    try {
        Guard g;
        if (g_mapProtoIds) {
            g_mapProtoIds->erase(id);
            InterlockedExchange(&g_mapOwnedSize, (LONG)g_mapProtoIds->size());
        }
    } catch (...) {
    }
}

// The LIVE registry size, for the heartbeat. `g_gateArmed` next to it is a MONOTONIC count of
// registration EVENTS since process start, not a size - it runs into five figures in a long
// session and is not a leak.
size_t registrySize() {
    try {
        Guard g;
        return g_registry ? g_registry->size() : 0;
    } catch (...) {
        return 0;
    }
}

size_t mapOwnedCount() {
    try {
        Guard g;
        return g_mapProtoIds ? g_mapProtoIds->size() : 0;
    } catch (...) {
        return 0;
    }
}

// The gate-byte write itself, for a caller that has JUST run itemIsLive() on this exact pointer
// and id. The liveness check is the real protection: writing into recycled heap does NOT fault,
// so the __try only covers an unmapped page. It is deliberately NOT repeated here - itemIsLive is
// up to three engine calls and setWholeRegistry sweeps a registry that can hold hundreds of
// entries. The scoped-gate test is not dropped.
//
// `id` is required, because a DISARM (value 0) must skip a prototype the engine's own reagent map
// owns: clearing that byte is exactly what makes the next character load refund the collection
// and then erase it (ReadPlayerReagents+0x441 -> SaveReagents 0x2CE535).
bool writeFlagChecked(GdItem* item, unsigned int id, unsigned char value) {
    if (!item || !g_flagOffset) return false;
    if (value && !gateShouldArm()) return false;
    if (!value && isMapOwnedId(id)) {
        InterlockedIncrement(&g_mapOwnedKept);
        return false;
    }
    __try {
        *((unsigned char*)item + g_flagOffset) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool writeFlag(GdItem* item, unsigned int id, unsigned char value) {
    if (!item || !g_flagOffset) return false;
    // SETTING the byte needs the full scoped condition (gateAllowed(), which carries the
    // multiplayer refusal, PLUS the Materials page actually being on screen). CLEARING it needs
    // nothing - a disarm must always be able to run.
    if (value && !gateShouldArm()) return false;
    if (!itemIsLive(item, id)) {
        InterlockedIncrement(&g_deadWrites);
        return false;
    }
    return writeFlagChecked(item, id, value);
}

// The engine's IsObjectOnDeletedList is called with our critical section NOT held: a snapshot is
// taken under the guard and the writes happen after it is released.
//
// Returns how many entries the write actually reached, and prunes the entries that no longer pass
// the liveness test (IsObjectOnDeletedList plus the stored object-id match). The registry survives
// a caravan close, so it has to clean up after itself rather than being emptied wholesale.
void forgetItem(void* obj);  // defined just below

int setWholeRegistry(unsigned char value) {
    std::vector<RegEntry> snapshot;
    try {
        Guard g;
        if (g_registry) snapshot = *g_registry;
    } catch (...) {
        return 0;
    }
    int done = 0;
    int kept = 0;
    std::vector<GdItem*> dead;
    for (size_t i = 0; i < snapshot.size(); ++i) {
        if (!itemIsLive(snapshot[i].item, snapshot[i].id)) {
            try {
                dead.push_back(snapshot[i].item);
            } catch (...) {
            }
            continue;
        }
        // A map-owned prototype is never disarmed (see writeFlagChecked).
        if (!value && isMapOwnedId(snapshot[i].id)) {
            ++kept;
            continue;
        }
        // liveness just checked, above
        if (writeFlagChecked(snapshot[i].item, snapshot[i].id, value)) ++done;
    }
    for (size_t i = 0; i < dead.size(); ++i) forgetItem(dead[i]);
    if (!value && kept) logT("reagent gate: %d map-owned prototypes kept armed", kept);
    return done;
}

void registerItem(GdItem* item, const char* record) {
    const unsigned int id = safeObjectId(item);
    try {
        Guard g;
        if (g_registrySet && g_registrySet->find(item) != g_registrySet->end()) {
            // The set keys on the raw Item* while the entry stores the object id, and the engine
            // REUSES Item addresses. Dropping a recycled address here would leave the NEW
            // object's id unrecorded: every later sweep would fail itemIsLive() on the stale id
            // and prune the entry, leaving the item armed, untracked and impossible to disarm for
            // the rest of the world session. Refresh the id instead.
            if (g_registry) {
                for (size_t i = 0; i < g_registry->size(); ++i) {
                    if ((*g_registry)[i].item == item) {
                        if ((*g_registry)[i].id != id) {
                            const LONG n = InterlockedIncrement(&g_regIdRefresh);
                            if (n <= 8) {
                                logT("reagent gate: registry entry %p id %u -> %u (the engine "
                                     "reused the address) %s",
                                     (void*)item, (*g_registry)[i].id, id, record ? record : "?");
                            }
                            (*g_registry)[i].id = id;
                        }
                        break;
                    }
                }
            }
        } else if (g_registrySet) {
            g_registrySet->insert(item);
            RegEntry e;
            e.item = item;
            e.id = id;
            g_registry->push_back(e);
            const LONG n = InterlockedIncrement(&g_gateArmed);
            // "registered", not "armed": registration is bookkeeping, and the byte is written
            // (or not) by the writeFlag call below, under gateShouldArm(). The LOG budget is per
            // world, and the main menu - where the transfer stash deserialises on the loader
            // thread - gets its own smaller one, so a character load's own registration lines can
            // still be seen.
            const LONG used = InterlockedIncrement(&g_regLogUsed);
            const LONG budget =
                InterlockedCompareExchange(&g_worldActive, 0, 0) ? kGateLogMax : kGateLogMenuMax;
            if (used <= budget) {
                logT("reagent gate: registered #%ld (%ld of %ld this %s) %p id=%u %s", n, used,
                     budget,
                     InterlockedCompareExchange(&g_worldActive, 0, 0) ? "world" : "menu",
                     (void*)item, id, record ? record : "?");
            } else if (used == budget + 1) {
                logT("reagent gate: (further registration lines suppressed until the next world)");
            }
        }
    } catch (...) {
        return;
    }
    // Only arm the byte when no auto-deposit is running.
    writeFlag(item, id, InterlockedCompareExchange(&g_depositDepth, 0, 0) > 0 ? 0 : 1);
}

void forgetItem(void* obj) {
    try {
        Guard g;
        if (g_idOf) {
            std::unordered_map<GdItem*, unsigned int>::iterator f = g_idOf->find((GdItem*)obj);
            if (f != g_idOf->end()) {
                if (g_byId) g_byId->erase(f->second);
                g_idOf->erase(f);
            }
        }
        if (g_registrySet) {
            std::unordered_set<GdItem*>::iterator it = g_registrySet->find((GdItem*)obj);
            if (it != g_registrySet->end()) {
                g_registrySet->erase(it);
                for (size_t i = 0; i < g_registry->size(); ++i) {
                    if ((*g_registry)[i].item == (GdItem*)obj) {
                        (*g_registry)[i] = g_registry->back();
                        g_registry->pop_back();
                        break;
                    }
                }
            }
        }
    } catch (...) {
    }
}

// No Item* may outlive the WORLD it came from. This is reached only on a world teardown
// (ExitPlayingMode, or the first Update after a session with no main player), and on both of those
// paths gateDisarmAll() has already written the byte back to 0 on every live entry, so nothing is
// left armed behind a cleared registry. Safety does not depend on the bytes anyway:
// hk_AddItemToReagents refuses a deposit of one of our records whenever an auto-deposit is on the
// stack, registry or no registry.
//
// The id map goes with it, and only here. That map is the choke point's "is this item in a sack
// the mod can see" evidence, and a sack the mod watched does not stop existing because a window
// closed - clearing it anywhere but a world teardown turns every later deposit into the
// UNSUPPORTED path. At a world teardown every Item* in it is about to die.
void registryClear(const char* why) {
    size_t n = 0;
    try {
        Guard g;
        if (g_registry) {
            n = g_registry->size();
            g_registry->clear();
        }
        if (g_registrySet) g_registrySet->clear();
        if (g_byId) g_byId->clear();
        if (g_idOf) g_idOf->clear();
    } catch (...) {
    }
    // Count EVERY clear, not only the ones that happen to log, or the heartbeat undercounts.
    const LONG c = InterlockedIncrement(&g_clears);
    if (n || c <= 8) {
        logD("reagent gate: registry cleared (%zu items forgotten) - %s", n, why);
    }
}

// The live group IS the page selection, so the gate follows it: Ctrl+wheel to a unique group arms
// every item the registry already knows, Ctrl+wheel back to the vanilla materials layout disarms
// them again.
//
// ONE edge-triggered decision, polled from the game-thread tick. It watches the live group AND the
// caravan page (plateCaravanState(), read through liveMaterialsVisible()), and acts only when the
// decision itself changes - never per frame. Every path that can change the answer runs through
// here: a group change, the caravan opening or closing, the user switching caravan tab, a HUD
// rebuild (which drops the captured window, so the page reads "unknown" and the gate disarms), the
// multiplayer flag, and ExitPlayingMode.
volatile LONG g_gateArmedNow = 0;    // 1 = the byte is currently set on the registry
volatile LONG g_gateArmedCount = 0;  // how many items the last arm/disarm reached
volatile LONG g_gateEdges = 0;

// The last decision lives at file scope so gateDisarmAll() can reset it.
int g_gateLastGroup = -2;
int g_gateLastArm = -1;
volatile LONG g_gatePageState = 0;  // plateCaravanState(), cached BY THE GAME THREAD

// Re-reads every live map node's prototype id into g_mapProtoIds. Defined with the map walk
// further down; called on every gate EDGE (never per frame - gateApply early-returns unless the
// decision changed).
void refreshMapOwnedFromMap();

void gateApply(const char* why, bool force) {
    const int group = shownCollection();
    const bool arm = gateShouldArm();
    const int armI = arm ? 1 : 0;
    // The heartbeat runs on the WORKER thread and must never call plateCaravanState() itself:
    // that dereferences the captured CaravanWindow (a raw read of caravan+0x1728) on a pointer the
    // game thread nulls at teardown. gateApply runs from the game-thread tick every frame, so the
    // value is cached here and the worker reads the atomic.
    InterlockedExchange(&g_gatePageState, (LONG)plateCaravanState());
    if (!force && group == g_gateLastGroup && armI == g_gateLastArm) return;
    // Never touch the flags underneath a running auto-deposit; retry on the next tick instead
    // (the statics are deliberately NOT advanced here).
    if (InterlockedCompareExchange(&g_depositDepth, 0, 0) > 0) return;
    g_gateLastGroup = group;
    g_gateLastArm = armI;
    refreshMapOwnedFromMap();  // know what the map owns BEFORE sweeping the registry
    const int n = setWholeRegistry(arm ? 1 : 0);
    InterlockedExchange(&g_gateArmedNow, armI);
    InterlockedExchange(&g_gateArmedCount, (LONG)n);
    InterlockedIncrement(&g_gateEdges);
    if (arm) {
        logT("reagent gate: ARMED %d items (page visible, group %d)%s%s", n, group,
             why ? " - " : "", why ? why : "");
    } else {
        logT("reagent gate: DISARMED %d items (%s, group %d)%s%s", n, gateDisarmReason(), group,
             why ? " - " : "", why ? why : "");
    }
}

void gateFollowLiveGroup() {
    gateApply(nullptr, false);
}

// A TEARDOWN is not a decision, so gateApply(force=true) is the wrong tool for it: that
// re-evaluates gateShouldArm() (and so could ARM at teardown), and its deposit-depth early-return
// is not conditioned on `force` (so the write could be skipped entirely, after which registryClear
// removes the only handle on those items and their bytes can never be cleared again). This writes
// 0 on every live entry, unconditionally: writeFlag/writeFlagChecked always allow the value 0, and
// clearing a byte underneath a running auto-deposit is safe - the choke point refuses a collection
// deposit while g_depositDepth > 0 regardless of any byte.
void gateDisarmAll(const char* why) {
    refreshMapOwnedFromMap();  // never disarm what the engine's map owns
    const int n = setWholeRegistry(0);
    g_gateLastGroup = -2;  // force the next polled edge to re-decide from scratch
    g_gateLastArm = 0;
    InterlockedExchange(&g_gateArmedNow, 0);
    InterlockedExchange(&g_gateArmedCount, (LONG)n);
    InterlockedIncrement(&g_gateEdges);
    logD("reagent gate: DISARMED %d items (world teardown, group %d) - %s", n, shownCollection(),
         why ? why : "?");
}

// The gate state, for the heartbeat and for every ACCEPTED / REFUSED line.
const char* gateStateText() {
    // Thread-local: the game thread writes it from the choke point while the worker writes it
    // into the heartbeat.
    static __declspec(thread) char buf[160];
    // Read the game thread's cached snapshot - never touch the engine from here.
    const int st = (int)InterlockedCompareExchange(&g_gatePageState, 0, 0);
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "gate %s: page=%s group=%d armed=%ld edges=%ld",
                InterlockedCompareExchange(&g_gateArmedNow, 0, 0) ? "ARMED" : "disarmed",
                st == 2 ? "materials" : (st == 1 ? "other" : "unknown"), shownCollection(),
                InterlockedCompareExchange(&g_gateArmedCount, 0, 0),
                InterlockedCompareExchange(&g_gateEdges, 0, 0));
    return buf;
}

// Every raw read of a possibly-stale Item* lives in its own SEH-only function (a __try block
// cannot hold objects that need unwinding).
struct ItemProbe {
    unsigned int id;
    const char* record;
    int gate;
    int soulbound;
    int untradeable;
    bool ok;
};

void probeItem(GdItem* item, unsigned int wantId, ItemProbe* out) {
    out->id = wantId;
    out->record = "<no live Item* for that id>";
    out->gate = out->soulbound = out->untradeable = -1;
    out->ok = false;
    if (!item) return;
    __try {
        const unsigned int got = g_gd.ObjectGetObjectId ? g_gd.ObjectGetObjectId(item) : wantId;
        if (got != wantId) {
            out->record = "<pointer no longer carries that id>";
            return;
        }
        const char* n = g_gd.ObjectGetObjectName ? g_gd.ObjectGetObjectName(item) : nullptr;
        out->record = n ? n : "<unnamed>";
        if (g_flagOffset) out->gate = *((const unsigned char*)item + g_flagOffset);
        if (g_soulboundOffset) out->soulbound = *((const unsigned char*)item + g_soulboundOffset);
        if (g_untradeableOffset) {
            out->untradeable = *((const unsigned char*)item + g_untradeableOffset);
        }
        out->ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out->record = "<read faulted>";
    }
}

unsigned int cursorItemId(void* cursorHandler) {
    // CursorHandlerItemMove + 0x30 is the id of the item on the cursor
    // (docs/RESEARCH-REAGENT.md a): PrimaryReagentActivate reads it there).
    if (!cursorHandler) return 0;
    __try {
        return *(const unsigned int*)((const char*)cursorHandler + 0x30);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

GdItem* itemById(unsigned int id) {
    GdItem* found = nullptr;
    try {
        Guard g;
        if (g_byId && id) {
            std::unordered_map<unsigned int, GdItem*>::const_iterator it = g_byId->find(id);
            if (it != g_byId->end()) found = it->second;
        }
    } catch (...) {
        return nullptr;
    }
    return found;
}

// The engine's own id -> Object* helper. It is private, so its address is decoded from the bytes
// of an EXPORTED function that calls it: TakeItemFromReagents(unsigned int, int) is
//   push rdi / sub rsp,0x50 / ... / call [rip+..]  (Singleton<ObjectManager>::Get)
//                                  / mov edx,ebx / mov rcx,rax / call rel32  <- the helper
// so byte 0x2C must be 0xE8 and the target is fn + 0x31 + rel32. Nothing is hard-coded: if the
// shape does not match we fall back to the id -> Item* map the AddItem detours build.
void* decodeCallTarget(const void* fn, unsigned int at) {
    if (!fn) return nullptr;
    const unsigned char* p = (const unsigned char*)fn;
    __try {
        if (p[at] != 0xE8) return nullptr;
        int rel = 0;
        memcpy(&rel, p + at + 1, 4);
        return (void*)(p + at + 5 + rel);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// The same resolution, but it SAYS which route answered.
//
// An EQUIPPED item is in no sack, so `observeAdd` never registered it and the id->Item->record
// resolution is the only thing that can find it. The ObjectManager route (ObjectFromId +
// Object::GetObjectName) must therefore be tried FIRST and the registry map only as a fallback;
// with the order reversed, a shift-click on an equipped collection item reaches the choke point,
// is classified as a non-collection item, and is refused by the engine's own +0xC64 test with no
// mod line at all. When neither route answers, the log has to say so instead of falling silently
// through to the engine.
GdItem* findItemByIdWhere(unsigned int id, const char** via) {
    *via = "unresolved";
    if (!id) return nullptr;
    if (p_ObjectFromId && p_ObjectManagerGet) {
        __try {
            void* om = p_ObjectManagerGet();
            void* obj = om ? p_ObjectFromId(om, id) : nullptr;
            if (obj) {
                const unsigned int got =
                    g_gd.ObjectGetObjectId ? g_gd.ObjectGetObjectId((GdItem*)obj) : id;
                if (got == id) {
                    *via = "ObjectFromId";
                    return (GdItem*)obj;
                }
                *via = "ObjectFromId returned an object with a different id";
            } else {
                *via = "ObjectFromId found nothing";
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            *via = "ObjectFromId faulted";
        }
    } else {
        *via = "no id->Object helper was decoded";
    }
    GdItem* r = itemById(id);
    if (r) {
        *via = "the AddItem id map (the ObjectManager route did not answer)";
        return r;
    }
    return nullptr;
}

GdItem* findItemById(unsigned int id) {
    const char* via = nullptr;
    return findItemByIdWhere(id, &via);
}

// `mem::vector<unsigned int>` is {begin, end, cap}, proven on LoadAdditionalDatabases. One static
// vector is reused for every query and its `end` is reset to
// `begin` before each call, so the engine allocates its storage at most once for the session.
struct MemVecU32 {
    unsigned int* begin;
    unsigned int* end;
    unsigned int* cap;
};
MemVecU32 g_countVec = {nullptr, nullptr, nullptr};

// -1 = unavailable (never treat that as "empty"); >= 0 = the count the engine really holds.
int engineCount(const char* record) {
    if (!p_GetReagentItemCount || !g_gameEngine || !record) return -1;
    MsvcString s;
    makeString(&s, record);
    __try {
        g_countVec.end = g_countVec.begin;
        return p_GetReagentItemCount(g_gameEngine, &s, &g_countVec);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        p_GetReagentItemCount = nullptr;  // never try again
        InterlockedIncrement(&g_reconcileFailed);
        return -1;
    }
}

int trackedCount(const std::string& key) {
    int n = 0;
    try {
        Guard g;
        if (g_counts) {
            std::map<std::string, int>::const_iterator it = g_counts->find(key);
            if (it != g_counts->end()) n = it->second;
        }
    } catch (...) {
        return 0;
    }
    return n;
}

void setTrackedCount(const std::string& key, int n) {
    try {
        Guard g;
        if (g_counts) (*g_counts)[key] = n < 0 ? 0 : n;
    } catch (...) {
    }
}

// ---- "is this item pristine?" -----------------------------------------------------------------
// ItemReplicaInfo IS Item + g_replicaOffset (0x538 on 1.3.0.8), 0x190 bytes.
// Item::GetItemReplicaInfo is `mov rax,rdx; lea rdx,[rcx+disp32]; mov rcx,rax; jmp <assign>`, so
// the offset is decoded from the exported getter's own bytes. Item::CreateItem proves +0x00 is a
// u32 object id and +0x08 is the base record std::string (`cmp [rdx+0x18],0x10` = the MSVC
// 32-byte string test).
//
// The rest of the layout (prefix, suffix, modifier, transmute, materia/component, augment,
// relicCompletionBonus, ascendant, ascendant2H) is NOT hard-coded. The blob is scanned for MSVC
// std::string-shaped slots instead, which is layout-agnostic and self-validating: the first slot
// must be at +0x08 and must equal Object::GetObjectName(item), otherwise the check reports
// "unknown" and the item is allowed through. Any OTHER non-empty string means the item carries an
// affix, a component, an augment, a transmuter or an ascendant bonus - i.e. it is not pristine.

// Set by collectionAccepts for the deposit the choke point is deciding on. Game thread only, read
// exactly once, immediately after.
bool g_lastNotPristine = false;
char g_lastPristineWhy[192] = {0};
// The two numbers every ACCEPTED / REFUSED line carries.
int g_lastHeld = -1;        // what collectionAccepts decided the collection already holds
int g_lastProtoStack = -1;  // the stored prototype's live stack size, -1 = no node / not live
// The TABLE half of the last `held`, so the refusal line can name which authority is holding the
// copy. -1 = not computed for this deposit.
int g_lastTableHeld = -1;

struct PristineResult {
    bool known;  // the layout was confirmed on this item
    bool ok;     // pristine
    unsigned int badOffset;
    char why[192];
};

// `allowEmptyHeap` accepts a size == 0 / capacity > 15 window WITHOUT dereferencing its pointer.
// Only the journal capture passes it, and only for a window that sits exactly 0x20 after a slot it
// has already confirmed - i.e. where the next std::string member of a consecutive run would be.
// Every other caller demands a readable pointer.
//
// The damage is bounded: a window wrongly accepted this way is rebuilt by writeMsvcString, which
// MEMSETS the whole 0x20 and stores an SSO empty string, so it can never leave a dangling pointer
// behind. The worst case is one string member of the identity coming back empty.
bool looksLikeStringEx(const unsigned char* base, unsigned int off, size_t* sizeOut,
                       const char** textOut, bool allowEmptyHeap) {
    const unsigned char* p = base + off;
    const size_t size = *(const size_t*)(p + 0x10);
    const size_t cap = *(const size_t*)(p + 0x18);
    if (cap < 15 || cap > 0x4000 || size > cap) return false;
    const char* text = nullptr;
    if (cap == 15) {
        text = (const char*)p;
        size_t n = 0;
        while (n < 16 && text[n]) ++n;
        if (n != size) return false;
    } else {
        text = *(const char* const*)p;
        if (!text) return false;
        if (size == 0 && allowEmptyHeap) {
            *sizeOut = 0;
            *textOut = "";
            return true;
        }
        size_t n = 0;
        while (n <= size && text[n]) ++n;
        if (n != size) return false;
    }
    *sizeOut = size;
    *textOut = text;
    return true;
}

bool looksLikeString(const unsigned char* base, unsigned int off, size_t* sizeOut,
                     const char** textOut) {
    return looksLikeStringEx(base, off, sizeOut, textOut, false);
}

// Defined further down (the SEH-guarded probe over engine memory that utScanReplicaSlots drives).
void slotProbeEngine(void* ctx, unsigned int off, bool allowEmptyHeap, UtSlotProbeResult* out);

void checkPristineRaw(const GdItem* item, const char* record, PristineResult* out) {
    out->known = false;
    out->ok = true;
    out->badOffset = 0;
    out->why[0] = 0;
    if (!item || !g_replicaOffset) {
        _snprintf_s(out->why, sizeof(out->why), _TRUNCATE,
                    "the ItemReplicaInfo offset is unknown");
        return;
    }
    // The SEH must be PER PROBE, which is what utScanReplicaSlots + slotProbeEngine give. A
    // single __try around a whole walk of the replica is not enough: a u32 that happens to sit
    // where a string could be - a relic's at +0x68, for instance - is read as a pointer, faults,
    // and takes the entire test with it, so a perfectly ordinary item reports "the pristine test
    // is unavailable" instead of reading the real slot a few bytes further on.
    const unsigned char* blob = (const unsigned char*)item + g_replicaOffset;
    UtSlotProbeResult base;
    memset(&base, 0, sizeof(base));
    slotProbeEngine((void*)blob, 0x08, false, &base);
    // The base record must sit at +0x08 and must match the object's own name.
    if (!base.ok || base.faulted || !record || _stricmp(base.text, record) != 0) {
        _snprintf_s(out->why, sizeof(out->why), _TRUNCATE,
                    "the replica at +0x8 is not this item's record");
        return;
    }
    out->known = true;
    unsigned int slotOff[24];
    char slotText[24][160];
    UtSlotShape shapes[24];
    memset(slotOff, 0, sizeof(slotOff));
    memset(shapes, 0, sizeof(shapes));
    int faults = 0;
    const int n = utScanReplicaSlots((void*)blob, g_replicaSize, &slotProbeEngine, slotOff,
                                     slotText, shapes, 24, &faults);
    for (int i = 0; i < n; ++i) {
        if (slotOff[i] == 0x08) continue;  // the base record itself
        if (slotText[i][0] != 0) {
            out->ok = false;
            out->badOffset = slotOff[i];
            _snprintf_s(out->why, sizeof(out->why), _TRUNCATE,
                        "ItemReplicaInfo+0x%X = \"%.90s\" (affix / component / augment / "
                        "transmuter / ascendant bonus)",
                        slotOff[i], slotText[i]);
            return;
        }
    }
    __try {
        if (g_seedRerollsOffset) {
            const unsigned int v =
                *(const unsigned int*)((const unsigned char*)item + g_seedRerollsOffset);
            if (v) {
                out->ok = false;
                out->badOffset = g_seedRerollsOffset;
                _snprintf_s(out->why, sizeof(out->why), _TRUNCATE, "seedRerolls = %u", v);
                return;
            }
        }
        if (g_affixRerollsOffset) {
            const unsigned int v =
                *(const unsigned int*)((const unsigned char*)item + g_affixRerollsOffset);
            if (v) {
                out->ok = false;
                out->badOffset = g_affixRerollsOffset;
                _snprintf_s(out->why, sizeof(out->why), _TRUNCATE, "affixRerolls = %u", v);
                return;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out->known = false;
        out->ok = true;
        _snprintf_s(out->why, sizeof(out->why), _TRUNCATE, "the replica read faulted");
    }
}

// One-shot diagnostic: log every std::string slot the scan finds in the first item we look at, so
// the session log names the real ItemReplicaInfo layout even if the pristine test misfires.
struct SlotDump {
    unsigned int off[16];
    char text[16][64];
    int n;
    int faults;  // candidate offsets whose read faulted; the scan now continues past them
};

// The SEH is PER CANDIDATE. `looksLikeString` dereferences a pointer it read out of the blob, so
// one 16-byte pair that passes the size/capacity test while holding garbage would otherwise fault
// and take the whole scan with it - and the log would then say "0 found", which reads like "the
// ItemReplicaInfo layout is wrong" and is not.
bool slotProbe(const unsigned char* blob, unsigned int off, size_t* size, const char** text,
               bool* faulted) {
    *faulted = false;
    __try {
        return looksLikeString(blob, off, size, text);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *faulted = true;
        return false;
    }
}

void slotCopyText(char* dst, const char* text, size_t size) {
    __try {
        _snprintf_s(dst, 64, _TRUNCATE, "%.60s", size ? text : "");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        dst[0] = 0;
    }
}

void dumpReplicaSlots(const GdItem* item, SlotDump* out) {
    out->n = 0;
    out->faults = 0;
    if (!item || !g_replicaOffset) return;
    const unsigned char* blob = (const unsigned char*)item + g_replicaOffset;
    size_t size = 0;
    const char* text = nullptr;
    for (unsigned int off = 0; off + 0x20 <= g_replicaSize && out->n < 16; off += 8) {
        bool faulted = false;
        if (!slotProbe(blob, off, &size, &text, &faulted)) {
            if (faulted) ++out->faults;
            continue;
        }
        out->off[out->n] = off;
        slotCopyText(out->text[out->n], text, size);
        ++out->n;
        off += 0x18;
    }
}

// ---- forward declarations (the blocks themselves are further down this namespace) ------------
std::unordered_set<std::string>* g_rescueEmptied = nullptr;  // emptied by the rescue command
bool rescueEmptied(const std::string& key);
bool captureReplicaRaw(const GdItem* item, UtReplicaCapture* cap);
void journalOnDeposit(const UtReplicaCapture& cap, const char* record);
void reagentSyncCaravan(const char* why);  // defined with the sync block
// The take-side restore; the block sits after the journal, below.
const void* identitySubstitute(const void* replica, const void* ret);
void identityAfterCreate(const GdItem* created);
bool identityAvailable();  // false = a non-pristine deposit must be REFUSED, never stripped
extern volatile LONG g_idArms;  // observed arms - a DIAGNOSTIC counter only
void journalReconcile();
void rescueTick();
// The crash diagnostics. `identityStateText` is also what the worker's stall line prints, so it
// must never touch engine memory.
const char* identityStateText(char* out, size_t cap);
void logEngineFault(const char* where, const EXCEPTION_RECORD* er, unsigned long tid);
void identityClear();
extern volatile LONG g_idBusy;
extern volatile LONG g_idFaults;
// The overlay record test the swap path uses; the block itself sits further down this namespace.
bool overlayRecordIs(const UtIdentityOverlay* ov, const std::string& key);

// ---- the prototype behind one record -----------------------------------------------------------
// Everything the stacking fix needs about a record's node in GameEngine's reagent map. `mapOk`
// says the map itself was readable - without it no deposit into a record that already has a node
// may be allowed, because the engine's merge branch would swallow the item silently.
struct ProtoInfo {
    const void* node;
    GdItem* proto;
    unsigned int protoId;
    unsigned int stack;
    bool mapOk;
    bool live;
    bool stubIncrement;
};

// Decoded from AddItemToReagents' own `call [rdi+disp32]`; 0 = not decoded. Its only reader is a
// diagnostic field, so there is no fallback and the binding is advisory.
unsigned int g_incrementSlot = 0;
bool g_incrementSlotDecoded = false;

bool nodeKeyEquals(const void* node, const std::string& key) {
    size_t size = 0;
    const char* text = nullptr;
    if (!looksLikeString((const unsigned char*)node, kNodeKeyOff, &size, &text)) return false;
    if (!text || size != key.size()) return false;
    for (size_t i = 0; i < size; ++i) {
        char c = text[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c == '\\') c = '/';
        if (c != key[i]) return false;
    }
    return true;
}

// In-order walk of the engine's own std::map. Read only, bounded in both depth and node count,
// and it never touches the tree if the head node does not have the MSVC `_Isnil == 1` shape.
const void* findReagentNode(const std::string& key, bool* mapOk) {
    *mapOk = false;
    if (!p_GetPlayerReagents || !g_gameEngine || key.empty()) return nullptr;
    __try {
        const ReagentNode* const* mapObj =
            (const ReagentNode* const*)p_GetPlayerReagents(g_gameEngine);
        if (!mapObj) return nullptr;
        const ReagentNode* head = *mapObj;
        if (!head || head->isnil != 1) return nullptr;
        *mapOk = true;
        const ReagentNode* stack[64];
        int sp = 0;
        int visited = 0;
        const ReagentNode* cur = head->parent;
        while ((cur && !cur->isnil) || sp > 0) {
            if (++visited > 8192) return nullptr;
            while (cur && !cur->isnil) {
                if (sp >= 64) return nullptr;
                stack[sp++] = cur;
                cur = cur->left;
            }
            const ReagentNode* n = stack[--sp];
            if (nodeKeyEquals(n, key)) return n;
            cur = n->right;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    return nullptr;
}

void readProtoFields(const void* node, ProtoInfo* out) {
    __try {
        out->protoId = *(const unsigned int*)((const unsigned char*)node + kNodeIdOff);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out->protoId = 0;
    }
}

// ReagentData::count, the field AddItemToReagents itself writes (`mov [rbx+0x44],eax`).
// -1 = unreadable.
int nodeCountOf(const void* node) {
    if (!node) return -1;
    __try {
        const unsigned int n = *(const unsigned int*)((const unsigned char*)node + kNodeCountOff);
        return n > 0x40000000u ? -1 : (int)n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// ONE in-order walk reports every page record the engine's map holds (reagentWalkPageRecords,
// below). The per-record engineCount() query is O(log n) inside the engine but it also resolves
// the prototype object and calls two virtuals, so reconciling a few thousand records one at a time
// is both slow and wrong.
//
// The key of one node, lowercased and slash-normalised, into a caller buffer. SEH only.
bool nodeKeyCopy(const void* node, char* buf, size_t cap) {
    size_t size = 0;
    const char* text = nullptr;
    __try {
        if (!looksLikeString((const unsigned char*)node, kNodeKeyOff, &size, &text)) return false;
        if (!text || !size || size >= cap) return false;
        memcpy(buf, text, size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    buf[size] = 0;
    for (size_t i = 0; i < size; ++i) {
        if (buf[i] >= 'A' && buf[i] <= 'Z') buf[i] = (char)(buf[i] - 'A' + 'a');
        if (buf[i] == '\\') buf[i] = '/';
    }
    return true;
}

// KEPT FOR THE LOG LINE ONLY. `LoadPlayerReagents+0x43` calls
// `GetSharedSavePath(SharedSave = 5, ..., byte [GameEngine+0x375BA])`, so this byte is what the
// engine itself hands that call.
//
// IT IS NOT THE CURRENT CHARACTER'S MODE. It reads 1 for a hardcore character and then 1 again
// for the softcore character loaded after him; whatever it is - a high-water mark, a stale
// argument slot - it does not go back down. GameInfo::GetHardcore is the mode
// (readCollectionMode above), and the ONE caller of this reader is `sayVariant`, which prints the
// byte beside the mode in the once-per-world info line so that the two can be compared in a log.
// Read-only, one byte, SEH-guarded, no engine call and no C++ object in the frame. -1 = could not
// be read.
const size_t kSaveVariantOff = 0x375BA;
// Every -1 carries a REASON, and a null engine pointer is answered without touching memory:
// "not captured yet" and "the byte faulted" are two different failures and must never read the
// same in the log. The returned value is the RAW byte, so the caller can name it; only 0 and 1
// mean anything, and every caller checks that itself.
int reagentSaveVariantEx(const char** why) {
    const char* sink = "";
    if (!why) why = &sink;
    if (!g_gameEngine) {
        *why = "the GameEngine pointer has not been captured yet";
        return -1;
    }
    __try {
        const int v =
            (int)*(const volatile unsigned char*)((const char*)g_gameEngine + kSaveVariantOff);
        *why = "";
        return v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *why = "the read at GameEngine+0x375BA faulted";
        return -1;
    }
}

// (There is deliberately no reason-less wrapper next to it: a second entry point that throws the
// reason away is how an unexplained -1 gets into a log. `sayVariant` is the only caller there is -
// if that half of the info line ever goes, this reader and kSaveVariantOff go with it, because
// nothing else in the mod reads the byte.)

// SEH-only walk: collects the node pointers, no C++ objects in this frame.
int reagentCollectNodes(const void** nodes, int cap) {
    int found = 0;
    __try {
        const ReagentNode* const* mapObj =
            (const ReagentNode* const*)p_GetPlayerReagents(g_gameEngine);
        if (!mapObj) return -1;
        const ReagentNode* head = *mapObj;
        if (!head || head->isnil != 1) return -1;
        const ReagentNode* stack[64];
        int sp = 0;
        int visited = 0;
        const ReagentNode* cur = head->parent;
        while ((cur && !cur->isnil) || sp > 0) {
            if (++visited > 8192) return -1;
            while (cur && !cur->isnil) {
                if (sp >= 64) return -1;
                stack[sp++] = cur;
                cur = cur->left;
            }
            const ReagentNode* n = stack[--sp];
            if (found < cap) nodes[found++] = n;
            cur = n->right;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return found;
}

// Every prototype id the engine's reagent map points at, folded into g_mapProtoIds. One in-order
// walk, no engine call per node beyond the raw u32 read the walk already does, and it runs only on
// a gate EDGE. The set only ever grows within a world; it is emptied by teardownTables(), because
// ids belong to the world that created them.
void refreshMapOwnedFromMap() {
    if (!p_GetPlayerReagents || !g_gameEngine) return;
    static const int kMaxNodes = 4096;
    std::vector<const void*> nodes;
    try {
        nodes.resize(kMaxNodes);
    } catch (...) {
        return;
    }
    const int n = reagentCollectNodes(&nodes[0], kMaxNodes);
    if (n <= 0) return;
    for (int i = 0; i < n; ++i) {
        ProtoInfo pi;
        memset(&pi, 0, sizeof(pi));
        readProtoFields(nodes[i], &pi);
        if (pi.protoId) noteMapOwnedId(pi.protoId);
    }
}

// One walk, then the string work outside any SEH frame (MSVC forbids mixing the two).
bool reagentWalkPageRecords(std::map<std::string, int>* out) {
    if (!p_GetPlayerReagents || !g_gameEngine || !g_pageRecords) return false;
    static const int kMaxNodes = 4096;
    std::vector<const void*> nodes;
    try {
        nodes.resize(kMaxNodes);
    } catch (...) {
        return false;
    }
    const int n = reagentCollectNodes(&nodes[0], kMaxNodes);
    if (n < 0) return false;
    for (int i = 0; i < n; ++i) {
        char raw[400];
        if (!nodeKeyCopy(nodes[i], raw, sizeof(raw))) continue;
        try {
            std::string key(raw);
            if (g_pageRecords->find(key) != g_pageRecords->end()) {
                const int c = nodeCountOf(nodes[i]);
                (*out)[key] = c > 0 ? c : 1;  // a present node is at least one copy
            }
        } catch (...) {
        }
    }
    return true;
}

void readProtoStack(ProtoInfo* out) {
    // `held` is the prototype's live stack size ALONE, so a missing Item::GetStackSize must not
    // read as 0 = "the collection does not hold this record": a second copy would then be accepted
    // straight into the engine's merge branch, which for a unique is the folded `xor al,al; ret`
    // stub and eats the item. A missing accessor is therefore a LIVENESS failure and the record
    // falls into the `held = 1` / dead-prototype refusal instead. That is what makes the "a record
    // that already has a node will be refused instead, so no item can be lost" promise in
    // reagentInstall's `stack fix: DISABLED` line true.
    if (!p_GetStackSize) {
        out->live = false;
        return;
    }
    __try {
        out->stack = p_GetStackSize(out->proto);
        const unsigned char* vt = *(const unsigned char* const*)out->proto;
        const void* inc = (vt && g_incrementSlot) ? *(const void* const*)(vt + g_incrementSlot)
                                                  : nullptr;
        out->stubIncrement = (inc != nullptr && inc == p_EquipIncrementStub);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out->live = false;
    }
}

void reagentProtoInfo(const std::string& key, ProtoInfo* out) {
    memset(out, 0, sizeof(*out));
    out->node = findReagentNode(key, &out->mapOk);
    if (!out->node) return;
    readProtoFields(out->node, out);
    if (!out->protoId) return;
    out->proto = findItemById(out->protoId);
    if (!out->proto) return;
    out->live = itemIsLive(out->proto, out->protoId);
    if (!out->live) return;
    readProtoStack(out);
}

// Item::GetStackSize on any live item, -1 when it cannot be read. SEH only.
int safeStackOf(const GdItem* item) {
    if (!p_GetStackSize || !item) return -1;
    __try {
        const unsigned int s = p_GetStackSize(item);
        return s > 0x10000u ? -1 : (int)s;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// The two drop-condition bytes of a live item, as journal flag bits.
unsigned int itemFlagBits(const GdItem* item) {
    unsigned int bits = 0;
    if (!item) return 0;
    __try {
        if (g_soulboundOffset && *((const unsigned char*)item + g_soulboundOffset)) {
            bits |= UT_JF_SOULBOUND;
        }
        if (g_untradeableOffset && *((const unsigned char*)item + g_untradeableOffset)) {
            bits |= UT_JF_UNTRADEABLE;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return bits;
}

// The ONE engine write the stacking fix performs, and it is the engine's own exported setter
// (`mov [rcx+0x88C],edx; mov [rcx+0x6B0],edx; ret`), behind the liveness check.
bool setStackSafe(GdItem* item, unsigned int id, unsigned int n) {
    if (!p_SetStackSize || !item) return false;
    if (!itemIsLive(item, id)) return false;
    __try {
        p_SetStackSize(item, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

volatile LONG g_slotDumpDone = 0;

// ---- THE TRANSITION RULE, in ONE function -----------------------------------------------------
// Two authorities can hold a record: the mod's own TABLE (it has the record with count >= 1) and
// the engine's reagent MAP (it holds the record with a LIVE stored prototype of stack >= 1). This
// arithmetic may exist exactly once, so `mapHeldFrom` is the ONLY place that turns a ProtoInfo
// into a number and `heldSumFrom` is the only place that combines it with the table. Everything
// else - collectionAccepts, the exported helper the model asks, the rescue - goes through these.
//
// The map half's rule: the ONE number the engine itself acts on is the stored prototype's live
// stack size (Item+0x88C), read behind the liveness check. Node presence and node+0x44 never
// count on their own; the engine's own count may RAISE it and never lower it; the mod's tally is
// trusted only when the map itself could not be read.
int mapHeldFrom(const ProtoInfo& proto, int engine, int tracked) {
    int held;
    if (!proto.mapOk) {
        held = tracked > 0 ? tracked : 0;      // the map is unreadable: the tally is all there is
        if (engine > held) held = engine;
    } else if (!proto.node) {
        held = 0;                              // no node: the collection does not hold it at all
    } else if (!proto.proto || !proto.live) {
        held = 1;                              // a node whose prototype is gone - refused below
    } else {
        // FAIL CLOSED on an unreadable stack. `proto.stack` is unsigned: a garbage read above
        // INT_MAX casts negative, and "held < 0 -> 0" would read as "the collection is empty" =
        // ACCEPT, i.e. straight into the merge branch. An absurd stack counts as one held copy
        // and the deposit is refused instead.
        held = proto.stack <= 0x10000u ? (int)proto.stack : 1;
        if (engine > held) held = engine;      // the engine may raise it, never lower it
    }
    return held;
}

// The table half: the count the mod's own file holds for this record, and 0 while the table
// cannot own anything (a read-only journal, or no path to it).
int tableHeldOf(const char* record) {
    if (!record || !*record) return 0;
    if (!storeTableOwns()) return 0;
    const unsigned int n = storeCount(record);
    return n > 0x10000u ? 1 : (int)n;  // the same fail-closed clamp the map half applies
}

// The one arithmetic site for "how many does the collection hold", with exactly two callers:
//
//   * `collectionAccepts` (the deposit choke point), which passes the ProtoInfo, the engine count
//     and the tally it has ALREADY read, and
//   * `reagentHeldTotal` (the export the model asks), which reads those three off the engine
//     first because its caller has none of them.
//
// `collectionAccepts` deliberately does NOT go through `reagentHeldTotal`: that re-reads
// `reagentProtoInfo` + `engineCount` (GetReagentItemCount, an engine call behind the liveness
// triple), and paying for it a second time inside the click path, for numbers the caller is
// already holding, adds an engine call to the thinnest path in the mod. Neither caller adds,
// subtracts or clamps anything of its own, so the two cannot drift.
//
// THE TABLE WINS. The engine's `reagents.gst` is foreign territory: nothing of ours may enter it,
// and nothing of ours that is ALREADY in it may be counted as a second copy of something the
// table holds. So the table's count IS the holding and the map's row is not added to it. A row
// the engine still holds stays alive and painted, and nothing is ever lost.
//
// Why this cannot cost an item: the two halves are still reported separately through `fromTable`
// / `fromMap` (the refusal line prints `table=N map=M`, and the takeover census names the rows),
// and the RESCUE does not use this number at all - `runRescue` walks the map and the table
// independently and hands back one copy per authority. This function only decides what "the
// collection holds" means for `max_per_record`, the owned filter and the label.
//
// `tableHeldOf` returns 0 unless `storeTableOwns()`, so a session whose table cannot own anything
// reports 0 held rather than borrowing the engine's number.
int heldSumFrom(const ProtoInfo& proto, int engine, int tracked, const std::string& key,
                int* fromTable, int* fromMap) {
    int map = mapHeldFrom(proto, engine, tracked);
    if (rescueEmptied(key) && proto.node && proto.live && proto.stack == 0 && engine <= 0) {
        map = 0;
    }
    const int table = tableHeldOf(key.c_str());
    if (fromMap) *fromMap = map;
    if (fromTable) *fromTable = table;
    return table;
}

// ---- WHAT A REFUSAL ACTUALLY PROMISES ---------------------------------------------------------
// In one string, so the three places that print it cannot drift. "The item stays where it was" is
// true of exactly one of the three refusal sites. Disassembled from the loaded exe image:
//
//   * exe quick-move site A: 001EAB38 call (the choke point) / 001EAB3E test al,al / 001EAB40
//     je 0x1EAB78, and 0x1EAB78 loads r15's vtable and calls [rax+0x408]
//     (CanBePlacedInTransferStash) at 001EAB7E before continuing into the stash move at 001EAB88.
//     A FALSE therefore runs the ORDINARY TRANSFER-STASH MOVE - the item moves, just not into our
//     collection.
//   * exe site B (001EC660 test / 001EC662 je 0x1EC694 / 001EC697 [rax+0x408]): the same shape.
//   * the Game.dll DRAG (0x1739C5, tagTransferStashError): a FALSE leaves the item on the cursor.
//
// What the MOD promises is the part the mod controls, and it is the part that matters for the
// "never lose, never duplicate" rule: nothing was added to the collection and nothing was taken
// out of the player's hands by us.
//
// ---- and what the GATE BYTE has to do with it -------------------------------------------------
// `Item::CanBePlacedInTransferStash` (Game.dll 0x310F60) is five instructions - `cmp byte
// [rcx+0xC00],0 ; jne false ; cmp byte [rcx+0xC64],0 ; jne false ; mov al,1`. So while the mod's
// gate byte is armed on a collection item - which it is for every one of our page records in the
// registry, for exactly as long as the caravan is open on a collection group - site A's FALSE
// branch cannot do the stash move either: 0x1EAB7E returns false, 0x1EAB86 takes the
// `je 0x1EABC4` and the exe raises `tagTransferStashError`. The item stays in the bag.
//
// That is WHY the byte is still written even though nothing reads it on the deposit path: it is
// what makes a refusal a true no-op for the shift-click, and a refusal is the answer to every
// deposit the table cannot take.
const char* refusalPromiseText() {
    return "the mod added nothing and removed nothing - and while a collection group is on screen "
           "the gate byte makes Item::CanBePlacedInTransferStash false (Game.dll 0x310F60), so "
           "even the game's own fallback moves nothing: a refused bag shift-click leaves the item "
           "in the bag with the usual 'cannot be placed here', and a refused drag leaves it on "
           "the cursor";
}

// The one decision function. `reason` is filled whenever the answer is false.
bool collectionAccepts(GdItem* item, const std::string& key, const char* record, ProtoInfo* proto,
                       char* reason, size_t reasonCap) {
    if (!InterlockedExchange(&g_slotDumpDone, 1)) {
        try {
            SlotDump d;
            dumpReplicaSlots(item, &d);
            std::string line;
            for (int i = 0; i < d.n; ++i) {
                char one[128];
                _snprintf_s(one, sizeof(one), _TRUNCATE, "%s+0x%X=\"%s\"", i ? " " : "", d.off[i],
                            d.text[i]);
                line += one;
            }
            logD("reagent collect: ItemReplicaInfo string slots of the first candidate (%s): "
                 "%d found (base +0x%X, %d bytes, %d faulted candidates) -> %s",
                 record ? record : "?", d.n, g_replicaOffset, g_replicaSize, d.faults,
                 line.c_str());
        } catch (...) {
        }
    }
    // The count must be read BEFORE the policy test, and it must be read from the map, not from
    // GetReagentItemCount.
    //
    // `GameEngine::GetReagentItemCount` (Game.dll 0x2CEF40) does NOT return ReagentData::count:
    // it resolves the stored prototype id and returns `prototype->vtable[0x618]()`
    // (Item::GetStackSize), returning 0 early when that is 0. A freshly created unique EQUIPMENT
    // prototype has stack size 0, so the FIRST stored copy of a unique reports "0 held" - and
    // `held(0) >= max_per_record(1)` is false, so a SECOND copy is accepted, runs into the
    // engine's merge branch (`IncrementStack` is the folded no-op stub for ItemEquipment) and is
    // consumed without raising anything. That is a lost item.
    //
    // Presence in the map is therefore the truth: a node for this record means the collection
    // already holds at least one copy, whatever any count field says.
    reagentProtoInfo(key, proto);
    const int engine = engineCount(record);
    const int nodeN = proto->node ? nodeCountOf(proto->node) : -1;
    const int tracked = trackedCount(key);
    // ---- what "held" means --------------------------------------------------------------------
    // NOT `max(node ? 1 : 0, node+0x44, engine, tracked)`. The engine NEVER removes a map node on
    // a take: the exe take (0x132BFA..0x132C16) and GameEngine::TakeItemFromReagents both only
    // write the stored PROTOTYPE's stack size to 0, and neither touches ReagentData::count at
    // node+0x44. So `node ? 1 : 0` would pin held at >= 1 for the rest of the session and every
    // re-deposit of a record the player has emptied would be refused - the engine saying empty
    // while the mod says full.
    //
    // The ONE number the engine itself acts on is the stored prototype's live stack size
    // (Item::GetStackSize, Item+0x88C): GetReagentItemCount returns it, the exe take bails when it
    // is 0, and TakeItemFromReagents clamps against it. The arithmetic itself lives in
    // mapHeldFrom(), and the table half is combined with it in heldSumFrom() - the only two sites
    // that may compute this, so the deposit path and the exported total cannot drift apart. The
    // rescue-emptied override is applied there too, after the map half and before the tally is
    // written.
    int mapHeld = 0, tableHeld = 0;
    int held = heldSumFrom(*proto, engine, tracked, key, &tableHeld, &mapHeld);
    // setTrackedCount must never re-inflate a record the engine reports as empty. It is the MAP
    // tally and stays the map half only: the table's count is persisted in the journal and is
    // never a fallback for an unreadable map.
    setTrackedCount(key, mapHeld);
    g_lastTableHeld = tableHeld;
    g_lastHeld = held;
    g_lastProtoStack = (proto->node && proto->live) ? (int)proto->stack : -1;

    // ---- THE PAGE TEST --------------------------------------------------------------------
    // "The caravan is not open / no group is shown" is carried by the gate byte's scoping
    // (gateShouldArm -> gateAllowed) only while the ENGINE decides: with the byte 0 the engine's
    // own test at 0x2CEC94 refuses and the choke point never has to. When the private table owns
    // the record the mod answers BEFORE that test is reached and no byte decides anything, so the
    // window has to be tested here - otherwise a shift-click with the caravan shut is accepted
    // into the table with no page to show it on. Exactly the two conjuncts gateAllowed() applies,
    // no more: a deposit whose group is not the one on screen stays legitimate, since it is
    // stored by record.
    if (storeTableOwns() &&
        (shownCollection() < 0 || !InterlockedCompareExchange(&g_transferOpenNow, 0, 0))) {
        _snprintf_s(reason, reasonCap, _TRUNCATE,
                    "the collection page is not up right now (caravanOpen=%d shownGroup=%d) - "
                    "open the caravan on the Crafting Materials page first",
                    InterlockedCompareExchange(&g_transferOpenNow, 0, 0) ? 1 : 0,
                    shownCollection());
        return false;
    }

    // Everything below needs to know whether the record already has a node in the engine's map,
    // because the merge branch of AddItemToReagents eats the item without raising the count
    // whenever the stored prototype cannot stack (uniques) or is gone.
    if (!proto->mapOk) {
        if (held >= 1 || tracked >= 1) {
            _snprintf_s(reason, reasonCap, _TRUNCATE,
                        "the engine's reagent map could not be read, and this record already has "
                        "%d - a second copy would be swallowed by the engine's merge",
                        held);
            return false;
        }
    } else if (proto->node && !proto->live) {
        InterlockedIncrement(&g_deadProtoRefusals);
        _snprintf_s(reason, reasonCap, _TRUNCATE,
                    "the page's stored prototype item (id=%u) for this record is no longer live - "
                    "the engine's merge would swallow the item",
                    proto->protoId);
        return false;
    }

    if (g_cfg.maxPerRecord > 0 && held >= g_cfg.maxPerRecord) {
        // The line names which authority holds what. `table=N map=M` with BOTH non-zero on a
        // record deposited exactly once means the same copy is being counted twice: a legacy row
        // must upgrade to count 0 while its copy is still MAP-OWNED, or it reads as 2.
        _snprintf_s(reason, reasonCap, _TRUNCATE,
                    "the box already holds %d of a maximum of %d (table=%d map=%d protoStack=%d "
                    "node=%d count=%d engine=%d tally=%d) - max_per_record",
                    held, g_cfg.maxPerRecord, tableHeld, mapHeld, g_lastProtoStack,
                    proto->node ? 1 : 0, nodeN, engine, tracked);
        return false;
    }
    // The pristine test RUNS even with collect_pristine_only=0 - it is what tells the choke point
    // whether this deposit carries an identity that must be journalled before the engine is
    // allowed to swallow the item (see hk_AddItemToReagents).
    g_lastNotPristine = false;
    g_lastPristineWhy[0] = 0;
    {
        PristineResult pr;
        checkPristineRaw(item, record, &pr);
        if (pr.known && !pr.ok) {
            g_lastNotPristine = true;
            _snprintf_s(g_lastPristineWhy, sizeof(g_lastPristineWhy), _TRUNCATE, "%s", pr.why);
            if (g_cfg.collectPristineOnly) {
                _snprintf_s(reason, reasonCap, _TRUNCATE, "the item is not pristine: %s", pr.why);
                return false;
            }
        } else if (!pr.known) {
            // The layout could not be confirmed on this item. Treat it as carrying an identity:
            // a wrong "pristine" answer here would let the engine strip a componented item.
            g_lastNotPristine = true;
            _snprintf_s(g_lastPristineWhy, sizeof(g_lastPristineWhy), _TRUNCATE,
                        "the pristine test is unavailable (%s)", pr.why);
            if (g_cfg.collectPristineOnly) {
                logD("reagent collect: pristine test UNAVAILABLE for %s (%s) - allowing the drop",
                     record, pr.why);
            }
        }
    }
    return true;
}

void logDropProbeLine(const char* where, unsigned int id, const ItemProbe& pr) {
    const char* verdict = "";
    if (pr.ok) {
        if (pr.soulbound > 0) verdict = "  -> REFUSED: the item is SOULBOUND";
        else if (pr.untradeable > 0) verdict = "  -> REFUSED: the item is UNTRADEABLE";
        else if (pr.gate == 0) verdict = "  -> REFUSED: no box on the page carries this record";
        else if (pr.gate > 0) verdict = "  -> should be ACCEPTED";
    }
    logD("reagent drop: %s id=%u record=%s | craftingMaterial(+0x%X)=%d soulbound(+0x%X)=%d "
         "untradeable(+0x%X)=%d%s",
         where, id, pr.record, g_flagOffset, pr.gate, g_soulboundOffset, pr.soulbound,
         g_untradeableOffset, pr.untradeable, verdict);
}

void logDropCandidate(const char* where, void* cursorHandler) {
    const unsigned int id = cursorItemId(cursorHandler);
    ItemProbe pr;
    probeItem(itemById(id), id, &pr);
    logDropProbeLine(where, id, pr);
}

// Remembers every record the engine puts into a sack, plus an id -> Item* map the drop trace
// needs (there is no exported ObjectManager id->Object accessor).
const char* safeObjectName(GdItem* item) {
    __try {
        return g_gd.ObjectGetObjectName ? g_gd.ObjectGetObjectName(item) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

unsigned int safeObjectId(GdItem* item) {
    __try {
        return g_gd.ObjectGetObjectId ? g_gd.ObjectGetObjectId(item) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// The id map is NOT a diagnostic and no switch may turn it off. The choke point reads it to
// answer "is this item in a sack the mod can see"; an empty map makes every collection deposit
// compute equipped=true, so the arm never runs and the engine refuses all of them on byte 0 -
// i.e. the whole collect feature, killed by a diagnostic switch.
void noteOwnedRecord(GdItem* item) {
    if (!item) return;
    const char* name = safeObjectName(item);
    if (!name || !*name) return;
    const unsigned int id = safeObjectId(item);
    try {
        std::string key(name);
        toLower(&key);
        if (key.compare(0, 8, "records/") != 0) return;
        {
            Guard g;
            if (id && g_byId && g_idOf) {
                (*g_byId)[id] = item;
                (*g_idOf)[item] = id;
            }
        }
    } catch (...) {
        return;
    }
}

// ---- detours ---------------------------------------------------------------------------------
void __cdecl hk_ItemLoad(GdItem* self, const void* loadTable) {
    if (o_ItemLoad) o_ItemLoad(self, loadTable);
    // Registration is BOOKKEEPING and writes nothing into the engine, so it is deliberately NOT
    // behind the gate. Registering only while a collection group is already on screen would leave
    // every item that entered a sack while the vanilla page was up unknown, and
    // `setWholeRegistry(1)` would then have nothing to arm when the group appears - the drag is
    // refused with `craftingMaterial(+0xC64)=-1`. The BYTE itself stays behind gateShouldArm(),
    // inside writeFlag.
    if (!g_flagOffset) return;
    __try {
        const char* name = g_gd.ObjectGetObjectName ? g_gd.ObjectGetObjectName(self) : nullptr;
        if (!isOurRecord(name)) return;
        registerItem(self, name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// The original, bracketed by a thread-local depth counter in its own frame -
// hk_AddItemToReagents holds C++ objects, so it cannot carry a __try/__finally itself.
bool callAddOriginal(GdGameEngine* self, unsigned int itemId) {
    ++g_addReagentsDepth;
    bool r = false;
    __try {
        r = o_AddItemToReagents ? o_AddItemToReagents(self, itemId) : false;
    } __finally {
        --g_addReagentsDepth;
    }
    return r;
}

// ---- THE BAG PROOF ----------------------------------------------------------------------------
// A `true` from the choke point that never calls the original is the caller's licence to destroy
// the source, so the mod may only give it when the caller's own removal is GUARANTEED to find the
// item. At exe quick-move site A that removal is `PlayerInventoryCtrl::RemoveItem(ic, id, true)`
// (0x1EAB4E, IAT 0x2DB518) followed by `ControllerCharacter::SendRemoveItemFromInventory`
// (0x1EAB5D). RemoveItem searches the `mem::vector<InventorySack*>` at `ic+0x20..0x28` - THE
// CHARACTER'S OWN BAGS - and nothing else (0x3DB8A9 `mov rbx,[rcx+0x20]` / 0x3DB8AD
// `cmp rbx,[rcx+0x28]`, per sack 0x3DB8C3 `mov rax,[rcx+0x20]` and 0x3DB8D8 `cmp esi,[rax+0x1c]`).
// Not the cursor, not an equipment slot, not the private stash, not the transfer stash.
//
// The mod's own id map is NOT an acceptable approximation of that, and must never be used as one.
// `noteOwnedRecord` fills it from the `InventorySack::AddItem` detour only, so it answers a
// different question - "did the mod WATCH this item enter a sack this session" - and it answers
// it wrong for the majority of real quick-move deposits: items already in the bag when the mod
// loaded are absent from it, get classified as the UNSUPPORTED equipped quick-move, and fall
// through to the ENGINE's reagent map instead of the table. That is how `reagents.gst` refills.
//
// So the question is put to the ENGINE, on the object the caller will actually search, with three
// read-only accessors and no open-coded offset:
//   PlayerInventoryCtrl::GetNumberOfSacks  0x3DAA10  `([rcx+0x28]-[rcx+0x20])>>3 ; ret`
//   PlayerInventoryCtrl::GetSack(int)      0x3DA770  `[[rcx+0x20]+rdx*8] ; ret`  (NO bounds check)
//   InventorySack::ContainsItem(u32)       0x30BBD0  - the same `sack+0x20` map and the same
//                                                     `node+0x1C` key RemoveItem walks
// The `this` chain: Player + g_ctrlIdOffset (0x16C0, decoded from CursorHandler::GetPlayerCtrl's
// own bytes) is the ControllerPlayer's object id, and `ControllerPlayer::GetInventoryCtrl`
// (`lea rax,[rcx+0x470]`) is the PlayerInventoryCtrl. One assert rides along: `ic+0x10` must be
// the ControllerPlayer (DepositReagents 0x3E0670 and RemoveItem 0x3DBA0C both read it there).
//
// THE ANSWER IS TRI-STATE AND THE MOD FAILS CLOSED: 1 = the id is in one of the bags RemoveItem
// searches, so its F2 branch cannot be taken; 0 = it is in none of them; -1 = the question could
// not be put (an export missing, the chain did not validate, a fault). Anything but 1 is a
// REFUSAL on the quick-move paths - never a fall-through to the engine.
//
// THE EXACT STRENGTH OF THE CLAIM. A 1 proves the removal cannot miss its SEARCH, and that is
// what this proof is for. `RemoveItem` can still bail AFTER the match, and the bytes say exactly
// how (from the shipped Game.dll):
//     003DB8D8  cmp esi,[rax+0x1c]        ; the node key - the match
//     003DB8DB  je  0x3DB938
//     003DB938  call [rip+0x21775a]       ; -> 0x5F3098, the ObjectManager singleton
//     003DB943  call 0x19D20              ; ObjectFromId - THE MOD'S OWN ROUTE
//     003DB94B  test rax,rax
//     003DB94E  je  0x3DB9CB              ; and 0x3DB964 / 0x3DB975, the RTTI type tests
//     003DB9CB  add rbx,8                 ; <- NOT a return: the NEXT-SACK advance
// so a matched id whose object does not resolve (or is not an Item) does not stop the walk - it
// carries on to the next sack and, if nothing else matches, falls out at 0x3DBAEB having removed
// nothing. The mod validated the object with the liveness triple a few instructions earlier and
// runs on the same thread with no reentrancy in between, so there is no window in practice; it is
// written down because "cannot miss" on its own is one clause too strong. Note which way the
// residue falls: that tail leaves the item IN the bag while the mod has already journalled it,
// i.e. a DUPLICATE - the same failure the bag refusal's own advice names ("holding the item AND a
// row in the collection") - never a loss. If it ever became reachable the answer would be to
// re-check the bag after the caller's removal, not to widen this proof.
//
// COST: one deposit CLICK on one of our records with the table on. Never for a record that is not
// ours, and never on a tick.
typedef void*(__cdecl* PfnCtrlPlayer_GetInvCtrl)(void*);
typedef unsigned int(__cdecl* PfnInvCtrl_NumSacks)(const void*);
typedef void*(__cdecl* PfnInvCtrl_GetSackAt)(void*, int);
typedef bool(__cdecl* PfnSack_ContainsId)(const void*, unsigned int);

PfnCtrlPlayer_GetInvCtrl p_GetInventoryCtrl = nullptr;
PfnInvCtrl_NumSacks p_IcNumberOfSacks = nullptr;
PfnInvCtrl_GetSackAt p_IcGetSack = nullptr;
PfnSack_ContainsId p_SackContainsId = nullptr;
unsigned int g_ctrlIdOffset = 0;  // Player + this = the ControllerPlayer's object id (0x16C0)
volatile LONG g_bagProofSaid = 0;

GdPlayer* safeMainPlayer();  // defined with the rescue helpers, further down

// SEH only - no C++ object may live in a frame that uses __try. Returns 1 / 0 / -1 as above.
int bagsHoldItemInner(unsigned int id, void** icOut) {
    *icOut = nullptr;
    GdPlayer* player = safeMainPlayer();
    if (!player) return -1;
    unsigned int ctrlId = 0;
    __try {
        ctrlId = *(const unsigned int*)((const unsigned char*)player + g_ctrlIdOffset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    if (!ctrlId) return -1;
    void* om = nullptr;
    void* cp = nullptr;
    __try {
        om = p_ObjectManagerGet ? p_ObjectManagerGet() : nullptr;
        cp = (om && p_ObjectFromId) ? p_ObjectFromId(om, ctrlId) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    // The same liveness triple every other engine read in this file sits behind.
    if (!cp || !itemIsLive((GdItem*)cp, ctrlId)) return -1;
    void* ic = nullptr;
    __try {
        ic = p_GetInventoryCtrl(cp);
        // One comparison validates the whole `this` chain.
        if (ic && *(void* const*)((const unsigned char*)ic + 0x10) != cp) ic = nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    if (!ic) return -1;
    *icOut = ic;
    __try {
        const unsigned int n = p_IcNumberOfSacks(ic);
        // PlayerInventoryCtrl::AddSack caps the vector at 6 (0x3DA7AD `cmp edi,6`). Anything
        // larger means the pointer is not what we think it is: say "could not ask", never "no".
        if (n > 8) return -1;
        for (unsigned int i = 0; i < n; ++i) {
            void* sack = p_IcGetSack(ic, (int)i);
            if (!sack) continue;
            if (p_SackContainsId(sack, id)) return 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return 0;
}

// 1 = in one of the bags the quick-move's own removal searches; 0 = in none of them;
// -1 = the mod could not ask. `why` always gets a short phrase for the log line.
int bagsHoldItem(unsigned int id, const char** why) {
    *why = "?";
    if (!id) {
        *why = "no item id";
        return -1;
    }
    if (!p_GetInventoryCtrl || !p_IcNumberOfSacks || !p_IcGetSack || !p_SackContainsId ||
        !g_ctrlIdOffset || !p_ObjectFromId || !p_ObjectManagerGet) {
        *why = "the bag-proof exports did not resolve on this build";
        return -1;
    }
    void* ic = nullptr;
    const int r = bagsHoldItemInner(id, &ic);
    if (r < 0) {
        *why = ic ? "the bag walk faulted" : "the PlayerInventoryCtrl chain did not validate";
        return -1;
    }
    *why = r ? "the id is in one of the character's bags"
             : "the id is in NONE of the character's bags";
    if (!InterlockedExchange(&g_bagProofSaid, 1)) {
        logD("reagent collect: the bag proof is ARMED - a quick-move deposit into the private "
             "table is only accepted while PlayerInventoryCtrl::ContainsItem says the item is in "
             "one of the character's own bags, which is the exact container the caller's own "
             "PlayerInventoryCtrl::RemoveItem (exe 0x1EAB4E) searches (first answer for id=%u: "
             "%s)",
             id, *why);
    }
    return r;
}

// ---- EQUIPPED SHIFT-CLICK IS UNSUPPORTED ------------------------------------------------------
// Shift-clicking an EQUIPPED item into the collection is a deliberately unsupported feature: the
// removal cannot be proved (see the bag proof above) and making it work is not worth the risk to
// the never-lose rule. The mod does NOT try to make that path work - what it does is SAY so, once
// per item, rather than leaving the user with a shift-click that does nothing and no line
// explaining it.
//
// What the mod can honestly test is ONE thing, plus a corroborating detail:
//   * the item is in no sack the mod can see - `itemById` answers out of the map that
//     `InventorySack::AddItem` fills and `InventorySack::RemoveItem` empties again (observeAdd ->
//     reagentNoteItem -> noteOwnedRecord / reagentForgetSackItem). Only judged on the exe
//     quick-move sites, where the item is still in its container when the choke point is reached,
//     so a miss there means it came from a container the mod does not watch. An item on the
//     CURSOR is out of every sack legitimately and is never judged.
//   * exe site B (return address inside 0x1EC650..0x1EC665), which has the shape of the EQUIPMENT
//     quick-move handler. It has never been observed in a real session - site A carries every
//     quick-move seen, equipped ones included - so it is kept only as a second, conservative
//     reason NOT to arm.
// The engine's own behaviour is kept exactly: the original still runs, its +0xC64 test at
// 0x2CEC94 refuses on a byte the mod deliberately did not arm, and the exe falls through to the
// normal move (`CanBePlacedInTransferStash` at 0x1EAB7E) just as it does today. Nothing is
// refused BY THE MOD here and no item can be lost: the item is on the character the whole time.
// One id is logged once - a shift-click that fell through can be repeated all day.
bool noteUnsupportedEquipped(unsigned int itemId) {
    static unsigned int seen[64] = {0};
    static int at = 0;
    try {
        Guard g;
        for (int i = 0; i < 64; ++i) {
            if (seen[i] == itemId) return false;
        }
        seen[at] = itemId;
        at = (at + 1) & 63;
        return true;
    } catch (...) {
        return false;
    }
}

// THE choke point. Every route into the reagent map calls this, so the collection policy is
// applied here exactly once and a refusal costs the user nothing: PrimaryReagentActivate's own
// `AddItemToReagents == false` branch shows "tagTransferStashError" and never reaches
// SendRemoveItemFromInventory, so the item stays on the cursor.
const char* depositPathLabel(const void* ra, char* buf, size_t cap);  // defined below
// WHICH proved caller, because the drag and the bag shift-click need different proofs that the
// source is removed exactly once. Returns a `UtDepositCallerKind` from `ut_depositgate.h`;
// defined below. A bool cannot express this: "site A, but only while the item is in a bag" is a
// distinct answer from both "proved" and "unknown".
int depositCallerKind(const void* ra);

bool __cdecl hk_AddItemToReagents(GdGameEngine* self, unsigned int itemId) {
    const void* callerRa = _ReturnAddress();
    g_gameEngine = self;
    // ENTRIES, counted before any decision. g_addCalls is incremented only where the original is
    // actually reached, so without this counter a MOD refusal leaves both unchanged and
    // hk_PrimaryReagentActivate reports "AddItemToReagents was NEVER called" over our own
    // refusal.
    InterlockedIncrement(&g_addEntries);
    char reason[400] = {0};  // room for the longest refusal text (the identity one)
    char path[128];
    depositPathLabel(callerRa, path, sizeof(path));
    // These two are only written by collectionAccepts, which the multiplayer and auto-deposit
    // refusals below never reach - without the reset the REFUSED line prints the PREVIOUS item's
    // numbers. -1 = "not computed for this deposit".
    g_lastHeld = -1;
    g_lastProtoStack = -1;
    bool refuse = false;
    std::string key;
    const char* record = nullptr;
    ProtoInfo proto;
    memset(&proto, 0, sizeof(proto));
    // The journal copy of the ItemReplicaInfo has to be taken from the LIVE item BEFORE the
    // original runs: AddItemToReagents builds its own copy with the object id zeroed and then
    // throws the incoming item away, so this is the last moment the identity exists anywhere.
    static UtReplicaCapture cap;  // ~4 KB: too big for a detour's own stack frame
    bool haveCap = false;
    // Hoisted out of the decision block: the in-place refresh below needs exactly the same "the
    // capture can be restored" test the identity refusal computes.
    bool capUsable = false;
    // Was this record's box emptied by a take? The refresh only ever runs on a RE-deposit into a
    // node the engine kept, never on a first copy.
    bool wasEmptied = false;
    int depositedStack = -1;  // GetStackSize of the item being deposited
    // ONE line per choke-point entry, resolution first and the decision after. A deposit that
    // does nothing is diagnosed from an ABSENCE, and an absence can only be read when every entry
    // says what it resolved to and what the gate looked like at that instant.
    const char* via = "?";
    GdItem* entryItem = findItemByIdWhere(itemId, &via);
    const char* entryRecord = entryItem ? safeObjectName(entryItem) : nullptr;
    const bool entryOurs = isOurRecordSafe(entryRecord);
    if (entryOurs || InterlockedCompareExchange(&g_addEntries, 0, 0) <= kEntryLogMax) {
        logD("reagent collect: entry id=%u %s path=%s gate=%s -> %s [resolved via %s]", itemId,
             entryRecord ? entryRecord : "<no record>", path, gateStateText(),
             entryOurs ? "a collection record: deciding"
                       : (entryItem ? "not a collection record - straight to the engine"
                                    : "the id resolves to NO live object - straight to the "
                                      "engine, which will refuse it if its own gate byte is 0"),
             via);
    }
    // What the mod can test without the bag proof is "the id is not in a sack the mod can see, on
    // a path where the item should still BE in one". The exe's two quick-move sites reach the
    // choke point with the item still in its container - site A's id comes from
    // PlayerInventoryCtrl::GetItemUnderPoint and nothing is picked up or detached before the call
    // - so a miss THERE means the item came out of something the mod does not watch, an equipment
    // slot being the usual case. A cursor DRAG through PrimaryReagentActivate is legitimately out
    // of every sack while it is on the cursor, and its removal is the cursor clear plus
    // SendRemoveItemFromInventory, so it is not judged here at all. `equipped` only suppresses the
    // mod's own arm below; it never refuses and never accepts.
    const bool quickMoveSite = strstr(path, "quick-move site") != nullptr;
    const bool inSack = itemById(itemId) != nullptr;
    bool equipped =
        entryOurs && (strstr(path, "site B") != nullptr || (quickMoveSite && !inSack));
    // ---- under the private table the ENGINE answers this, not the mod's map --------------------
    // `inSack` reads `g_byId`, which `noteOwnedRecord` fills from the InventorySack::AddItem
    // detour only - "did the mod WATCH this item enter a sack this session", which is NOT the
    // question and gets it wrong for the majority of real deposits (an item already in the bag
    // when the mod loaded is absent from the map, is called `equipped`, and goes to the engine's
    // map). `bagsHoldItem` asks the engine about the exact container the caller's own removal
    // searches. -2 = never asked (every non-collection item, and every path that is not a
    // quick-move).
    int bagProof = kUtBagNotAsked;
    const char* bagWhy = "not asked";
    bool decideFaulted = false;  // the classification below threw - see the catch
    if (entryOurs && quickMoveSite && storeTableOwns()) {
        bagProof = bagsHoldItem(itemId, &bagWhy);
        if (bagProof >= 0) equipped = (bagProof != kUtBagYes);
    }
    try {
        GdItem* item = entryItem;
        if (item) {
            record = entryRecord;
            if (record && *record) {
                key.assign(record);
                toLower(&key);
                if (g_pageRecords && g_pageRecords->find(key) != g_pageRecords->end()) {
                    // "The deposit is net-synced to the peers" is FALSE: there is not one
                    // network-shaped call on any reagent path, and SyncCaravanReagents is a UI
                    // vtable tail-jump. With mp_collect=1 a deposit in a co-op session runs the
                    // identical code path it runs in single player. The two checks that DO matter
                    // are: this must be the game thread (the caller is the exported
                    // AddItemToReagents on all three routes) and g_depositDepth must be 0, i.e.
                    // never in bulk.
                    if (mpBarred()) {
                        InterlockedIncrement(&g_mpRefusals);
                        _snprintf_s(reason, sizeof(reason), _TRUNCATE,
                                    "%s and mp_collect=0 - the collection is barred in this "
                                    "session by your own setting",
                                    mpSessionMode() == UT_MP_UNKNOWN
                                        ? "the session mode is UNKNOWN"
                                        : "a multiplayer session is active");
                        refuse = true;
                    } else if (InterlockedCompareExchange(&g_depositDepth, 0, 0) > 0) {
                        // The auto-deposit buttons are refused HERE, not only by the gate byte,
                        // so clearing the registry can never re-open that door.
                        InterlockedIncrement(&g_depositRefusals);
                        _snprintf_s(reason, sizeof(reason), _TRUNCATE,
                                    "an auto-deposit button is running - a collection item is "
                                    "never deposited in bulk");
                        refuse = true;
                    } else {
                        refuse = !collectionAccepts(item, key, record, &proto, reason,
                                                    sizeof(reason));
                        // The deposited item's OWN stack size, read before the original runs.
                        // The engine's new-node branch (0x2CEE62) initialises ReagentData::count
                        // from exactly this number, so it is what decides whether a first copy is
                        // born invisible.
                        depositedStack = safeStackOf(item);
                        if (!refuse && g_cfg.journal) {
                            memset(&cap, 0, sizeof(cap));
                            _snprintf_s(cap.record, sizeof(cap.record), _TRUNCATE, "%s",
                                        key.c_str());
                            cap.stack = 1;
                            // `| g_bracketFlagBits`: on a DRAG this runs inside the soulbound
                            // bracket, which has already written 0/0, so the live bytes read 0 and
                            // the journal would otherwise record flags=0 and the item would come
                            // back not soulbound.
                            cap.flags = itemFlagBits(item) | (unsigned int)g_bracketFlagBits;
                            haveCap = captureReplicaRaw(item, &cap);
                        }
                        // The safety net that makes collect_pristine_only=0 safe:
                        // `AddItemToReagents` re-creates the item from a replica whose
                        // object id is zeroed and then DESTROYS the original, so this is the
                        // last instant the identity exists anywhere. If it could not be
                        // journalled - journal off, identity off, the observer disabled itself,
                        // or the capture failed - an item with an affix, component, augment or
                        // Ascended bonus would come back stripped. Refuse instead: the item
                        // stays on the cursor and the engine shows its own message. The mod
                        // never strips silently.
                        // The capture is only USABLE for a restore if it found the base-record
                        // slot at +0x08: identityBuild refuses without it, because a blob whose
                        // string members were not re-pointed carries the deposited item's freed
                        // heap pointers and Item::CreateItem reads them.
                        for (int si = 0; haveCap && si < cap.slotCount; ++si) {
                            if (cap.slotOff[si] == 0x08) capUsable = true;
                        }
                        wasEmptied = rescueEmptied(key);
                        // ---- STRICTER UNDER THE PRIVATE TABLE -----------------------------
                        // While the ENGINE holds the copy, a failed capture costs only the
                        // identity - a stripped stock copy is still in the engine's map behind
                        // the journal - so `g_lastNotPristine` is what makes it a refusal. Under
                        // the table the capture is not a backup: IT IS THE ITEM, because the
                        // accept path returns true without calling the original and the caller
                        // has already destroyed the source. The pristine conjunct therefore
                        // drops out and ANY deposit the mod cannot record is refused.
                        if (!refuse && (g_lastNotPristine || storeTableOwns()) &&
                            (!g_cfg.journal || !capUsable || !identityAvailable())) {
                            // The three numbers are the only reasons this can happen, and each
                            // is a broken CAPABILITY the user cannot fix by retrying: enabled=0
                            // means the mod is switched off (the only thing that clears
                            // g_cfg.journal, which is what this tests), captured=0 means this
                            // item's replica had no base-record slot at +0x08, identity=0 means
                            // the machinery is disabled or never resolved (idOff= / idSites= on
                            // the heartbeat say which). The first names the SETTING, so the
                            // reader can go and find it.
                            _snprintf_s(reason, sizeof(reason), _TRUNCATE,
                                        "this item %s and it cannot be preserved right now "
                                        "(enabled=%d captured=%d identity=%d) - depositing it "
                                        "would %s, so the mod refuses instead",
                                        g_lastNotPristine ? "carries an identity" : "would be the "
                                            "private table's ONLY copy",
                                        g_cfg.enabled, capUsable ? 1 : 0,
                                        identityAvailable() ? 1 : 0,
                                        g_lastNotPristine ? "strip it" : "LOSE it");
                            if (g_lastNotPristine && g_lastPristineWhy[0]) {
                                const size_t at = strlen(reason);
                                _snprintf_s(reason + at, sizeof(reason) - at, _TRUNCATE, " [%s]",
                                            g_lastPristineWhy);
                            }
                            refuse = true;
                        }
                    }
                } else {
                    key.clear();  // a vanilla reagent: never our business
                }
            }
        }
    } catch (...) {
        // Never let a C++ exception reach the engine's frame: fall through and behave exactly
        // as the game would on its own.
        // ...EXCEPT for an item we already know is one of ours. `key.clear()` means "the mod does
        // not know what this is", and letting THAT fall through to `callAddOriginal` is a second
        // way for one of our records to reach `reagents.gst`. `entryOurs` was decided before the
        // try block, off the engine's own record string, so when it is true the answer is a
        // REFUSAL: the item stays in the bag and nothing goes into the engine's file.
        decideFaulted = true;
        refuse = false;
        key.clear();
    }
    if (refuse) {
        InterlockedIncrement(&g_refusals);
        logI("deposit REFUSED: %s - %s", record ? record : "?", reason);
        logD("id=%u held=%d protoStack=%d, %s; path: %s; %s", itemId, g_lastHeld,
             g_lastProtoStack, gateStateText(), path, refusalPromiseText());
        return false;  // deliberately WITHOUT calling the original
    }
    // ---- THE PRIVATE TABLE DEPOSIT ------------------------------------------------------------
    // The whole safety property in one sentence: the row is JOURNALLED AND ON DISK before this
    // function returns true, because a `true` is the caller's signal to destroy the source. The
    // callers, verified against the decrypted image and Game.dll, are what make that exact:
    //   * the DRAG, CursorHandlerItemMove::PrimaryReagentActivate 0x173972 `test al,al` /
    //     0x173974 `je 0x1739BC` -> 0x173995 SendRemoveItemFromInventory(edx=[rdi+0x30]) and
    //     0x17399A `mov [rdi+0x30], ebp` (the cursor is cleared);
    //   * exe quick-move site A 0x1EAB3E/0x1EAB40 -> 0x1EAB4E PlayerInventoryCtrl::RemoveItem
    //     (r8b=1) and 0x1EAB5D SendRemoveItemFromInventory;
    //   * exe quick-move site B 0x1EC660/0x1EC662 -> 0x1EC66D / 0x1EC679, the same shape - NOT
    //     accepted, because its removal `this` is not provable (see depositCallerKind);
    //   * InventorySack::DepositSackIntoReagents 0x30EC09 -> the id goes into a local vector and
    //     the removal loop runs after the walk (0x30EE0B SendRemoveItemFromInventory, else the
    //     local sack erase at 0x30EE57). That caller also tests +0xC64 itself at 0x30EBE5, and
    //     the mod refuses every bulk deposit above anyway (g_depositDepth > 0).
    // The source's disposal is entirely CALLER-side and nothing inside AddItemToReagents writes
    // to, moves or destroys the incoming item (it reads the replica at 0x2CECB5 and builds its
    // OWN prototype at 0x2CECC4), so a `true` that never calls the original removes the source
    // EXACTLY ONCE and a `false` leaves it exactly where it was.
    //
    // TWO CALLERS AND NO OTHERS. A `true` without the original is only ever returned to a caller
    // whose TRUE branch is disassembled above AND whose removal cannot miss, which is what
    // `depositCallerKind` + the bag proof decide. Everything else is REFUSED - never a
    // fall-through to the engine, which is how `reagents.gst` fills back up. The refusals, in the
    // order they are tested (the full list and the reasoning are in `ut_depositgate.h`):
    //   * the table cannot OWN anything (a READ-ONLY journal, or no path), and a fault while
    //     classifying.
    //   * a caller the mod has not read - exe site B, `DepositSackIntoReagents`, anything new.
    //   * exe site A with the item NOT in one of the character's bags (the equipment quick-move
    //     is the case the user meets): `PlayerInventoryCtrl::RemoveItem` searches those bags and
    //     nothing else, so accepting it could leave the item where it was AND put a row in the
    //     table - a DUPLICATE.
    //   * A RECORD THE ENGINE'S MAP STILL HAS A NODE FOR, even an EMPTY one. `ReagentWindow::Sync`
    //     skips only a NODE-LESS box (exe 0x132655 `cmp rbx,rdi ; je 0x132676`) - a box whose
    //     record still HAS a node is re-pointed at the map's prototype at 0x13266B, whatever the
    //     mod painted. And the engine never removes a node on a take - the exe take and
    //     TakeItemFromReagents only write the prototype's stack - so a record the user has ever
    //     stored keeps a count-0 node for ever. A table row for such a record would be painted
    //     over and the take would die at 0x132AE0. Clearing those rows out of `reagents.gst` is
    //     an OFFLINE job, with the game closed.
    // ---- THE TABLE OR A REFUSAL. NEVER THE ENGINE'S MAP. ------------------------------------
    // NOTHING of ours may enter `reagents.gst`, by any click path. There is deliberately no
    // "MAP-OWNED, nothing is lost" fall-through for a deposit the table cannot take: that branch
    // is what refills the engine's save file, a few hundred rows per session. For one of OUR page
    // records there are exactly two outcomes:
    //
    //   * the table takes it - the journal row is on disk BEFORE this function returns true, and
    //     the caller's own removal of the source is PROVED (see depositCallerKind), or
    //   * it is REFUSED - `return false` without calling the original, so the mod adds nothing,
    //     removes nothing, and the item is exactly where it was.
    //
    // A refusal is not a loss and it is not a dead end: the log line names the reason and what
    // the user can do instead (drag it, unequip it, run the offline `reagents.gst` clean-up).
    // Records the engine's map ALREADY holds keep every read path - they stay alive, painted
    // and takeable from the map, and `rescue=1` still empties both authorities.
    //
    // THE ENTRY CONDITION IS "IT IS ONE OF OURS", NOT `storeTableOwns()`. Gating this whole
    // block on `storeTableOwns()` would send our records straight into `callAddOriginal` whenever
    // the journal is READ-ONLY (a newer file format) or has no path. The record alone decides
    // that the engine's map is out of bounds; whether the TABLE can take the item is then one of
    // the facts below, and when it cannot the answer is a refusal, never the engine.
    bool tableDeposit = false;
    if (entryOurs) {
        // THE DECISION ITSELF IS NOT HERE. It is `utDepositDecide` in `src\ut_depositgate.h` -
        // pure logic, no engine, no globals - so `tools\test_store.cpp` proves the policy the
        // shipped build runs rather than a copy of it, including the property everything here
        // rests on: every verdict but kUtDepTable is reached WITHOUT THE JOURNAL BEING TOUCHED.
        UtDepositFacts facts;
        facts.caller = depositCallerKind(callerRa);
        facts.bagProof = bagProof;
        facts.tableOwns = storeTableOwns();
        // `key`/`record` empty here means the classification above did not finish; the table
        // branch needs both, so an empty one is the same fact as a fault.
        facts.faulted = decideFaulted || key.empty() || record == nullptr;
        facts.mapOk = proto.mapOk;
        facts.mapNode = proto.node != nullptr;
        facts.journal = g_cfg.journal != 0;  // a fixed 1 that only enabled=0 clears
        facts.haveCap = haveCap;
        facts.capUsable = capUsable;
        const int verdict = utDepositDecide(facts);
        char deny[420];
        deny[0] = 0;
        const char* advice = nullptr;
        static volatile LONG saidCaller = 0, saidBag = 0, saidNode = 0,
                             saidMap = 0, saidTableOff = 0, saidFaulted = 0;
        volatile LONG* said = nullptr;
        switch (verdict) {
            case kUtDepRefuseTableOff:
                // The table cannot own anything: a READ-ONLY journal (a newer build wrote it) or
                // no journal path at all. An item the mod cannot write down is an item it must
                // not take - and the engine's map is out of bounds, so the only answer left is
                // "no".
                // `storeTableOwns()` folds a THIRD case in: the mod does not yet know whether
                // this character is hardcore or softcore, and there is one collection per mode.
                // That is a different thing to tell the user, so the line names it.
                _snprintf_s(deny, sizeof(deny), _TRUNCATE,
                            journalModeKnown()
                                ? "THE MOD'S OWN FILE CANNOT OWN ANYTHING THIS SESSION (it is "
                                  "read-only - written by a newer build - or it has no path), and "
                                  "nothing of ours may go into the game's reagents.gst either"
                                : "THE MOD DOES NOT KNOW YET WHETHER THIS CHARACTER IS HARDCORE "
                                  "OR SOFTCORE, and it keeps one collection per mode - so it "
                                  "cannot tell which of the two this item would go into");
                advice = journalModeKnown()
                             ? "run the build that wrote uniq-items.jsonl. Nothing is lost: this "
                               "deposit simply did not happen"
                             : "nothing is lost and nothing is painted while this lasts. It "
                               "clears itself once a character is in the world; if it does not, "
                               "the log's \"collection: mode =\" line says why";
                said = &saidTableOff;
                break;
            case kUtDepRefuseFaulted:
                _snprintf_s(deny, sizeof(deny), _TRUNCATE,
                            "THE MOD COULD NOT CLASSIFY THIS DEPOSIT (an internal read faulted). "
                            "It knows the record is one of the collection's, and an unclassified "
                            "collection item is never handed to the engine's map");
                advice = "this one is worth reporting with the log - nothing is lost, the item "
                         "is where you left it, but the mod should not be faulting here";
                said = &saidFaulted;
                break;
            case kUtDepRefuseCaller:
                _snprintf_s(deny, sizeof(deny), _TRUNCATE,
                            "THE PRIVATE TABLE DOES NOT RECOGNISE THIS CLICK - the mod only "
                            "answers \"taken\" to a caller whose own source removal it has "
                            "disassembled");
                advice = "the private table takes the cursor DRAG onto a box and the bag "
                         "SHIFT-CLICK (exe quick-move site A). exe site B and the \"deposit all\" "
                         "button are not on that list: site B re-loads its removal `this` at "
                         "least six times between 0x1EBDA2 and 0x1EC66A, so which "
                         "PlayerInventoryCtrl its 0x1EC66D RemoveItem gets is not provable from "
                         "the listing, and the bulk deposit is refused a few hundred lines above "
                         "(g_depositDepth > 0)";
                said = &saidCaller;
                break;
            case kUtDepRefuseBag:
                _snprintf_s(deny, sizeof(deny), _TRUNCATE,
                            "THE MOD CANNOT PROVE THIS ITEM IS IN ONE OF YOUR BAGS (%s) - the "
                            "shift-click's own removal (PlayerInventoryCtrl::RemoveItem, exe "
                            "0x1EAB4E) searches the character's bags and nothing else",
                            bagWhy);
                advice = "unequip the item into a bag first, or DRAG it onto the box - the cursor "
                         "drag removes the source from the cursor slot itself and always works. "
                         "Accepting this click would risk leaving you holding the item AND a row "
                         "in the collection, which is the one thing this mod never does";
                said = &saidBag;
                break;
            case kUtDepRefuseMapUnreadable:
                _snprintf_s(deny, sizeof(deny), _TRUNCATE,
                            "THE ENGINE'S REAGENT MAP COULD NOT BE READ, so the mod cannot tell "
                            "whether this record already has a row in reagents.gst - the "
                            "collection fails closed");
                advice = "try again with the caravan open on the Crafting Materials page";
                said = &saidMap;
                break;
            case kUtDepRefuseMapNode:
                _snprintf_s(deny, sizeof(deny), _TRUNCATE,
                            "THE GAME'S OWN reagents.gst STILL HAS A ROW FOR THIS RECORD "
                            "(stack=%u) and the collection never mixes the two files",
                            proto.stack);
                advice = "ReagentWindow::Sync re-points a box whose record HAS a node (exe "
                         "0x13266B) and the engine never deletes a node on a take, so a table row "
                         "for such a record would be painted over and the take would die at "
                         "0x132AE0. Take the old row out of the game's own list first - open the "
                         "caravan, move that item out of the Crafting Materials page and back "
                         "into a bag. After that this record deposits into the table normally. "
                         "Until then what is already in the game's file stays there, stays "
                         "visible and can still be taken out or rescued - nothing is lost";
                said = &saidNode;
                break;
            case kUtDepRefuseCapture:
                // A belt: the identity refusal above already makes these unconditional under the
                // table, so reaching this with an unusable capture would be a bug in that block,
                // not a policy decision. Refuse rather than trust it.
                _snprintf_s(deny, sizeof(deny), _TRUNCATE,
                            "THE PRIVATE TABLE COULD NOT RECORD THIS ITEM (enabled=%d captured=%d "
                            "usable=%d) and the collection keeps no engine copy behind the row",
                            g_cfg.enabled, haveCap ? 1 : 0, capUsable ? 1 : 0);
                break;
            default:
                break;  // kUtDepTable - the journal write below is the only thing left to do
        }
        if (!utDepositIsRefusal(verdict)) {
            // `cap.stack` is the DEPOSITED ITEM's own stack (1 for equipment, set at the
            // capture), not the collection's count: the count is the table's own field and
            // storeOnDeposit is what raises it. Conflating the two makes a first copy invisible.
            cap.stack = 1;
            bool recorded = false;
            try {
                // MUST be durable, not merely in memory: storeOnDeposit does the upsert AND
                // journalFlushNow() on THIS thread, and rolls the row back if either fails. A
                // false here is the one answer that cannot be argued with.
                recorded = storeOnDeposit(cap, record);
            } catch (...) {
                recorded = false;
            }
            if (recorded) {
                tableDeposit = true;
            } else {
                _snprintf_s(deny, sizeof(deny), _TRUNCATE,
                            "THE JOURNAL FILE COULD NOT BE WRITTEN (the file IS the collection, "
                            "so an item that is not on disk is not deposited)");
            }
        }
        if (!tableDeposit) {
            InterlockedIncrement(&g_refusals);
            // `record` is null on the faulted path, so the entry-line record stands in for it.
            logI("deposit REFUSED: %s - %s",
                 record ? record : (entryRecord ? entryRecord : "?"), deny);
            logD("id=%u, %s [path: %s]", itemId, refusalPromiseText(), path);
            // The long "what to do instead" goes out ONCE per reason. The refusal line itself is
            // printed for every click - a user who clicks a hundred times needs a hundred
            // answers.
            if (advice && said && !InterlockedExchange(said, 1)) {
                logI("how to get this one in: %s", advice);
            }
            return false;  // deliberately WITHOUT calling the original
        }
    }
    // ---- arm the gate byte AT the choke point ------------------------------------------------
    // Registry coverage, not policy, is why an arm is needed here at all: `registerItem` is
    // reached from `hk_ItemLoad` and `observeAdd` (InventorySack::AddItem) only, and an item
    // EQUIPPED on the character is in no sack, so the gate-edge sweep (setWholeRegistry, which
    // walks g_registry and nothing else) never sees it and the item reaches the choke point with
    // Item+0xC64 still 0. Arming the item we are about to let through is more reliable than
    // relying on the sweep. The condition is exactly what writeFlag already enforces -
    // gateShouldArm(), which carries gateAllowed() and therefore the multiplayer refusal - plus
    // the liveness triple, so multiplayer neutrality is unchanged. Every way the arm can NOT
    // happen says so, or an absence of lines is the only symptom.
    //
    // The arm is for SACK items only. The equipment quick-move is unsupported, so no attempt is
    // made for it and the engine's own +0xC64 test decides, exactly as it does with the mod
    // absent.
    if (equipped) {
        // All three lines print HERE, once per item id, and only after the policy has run.
        // Printed above the refusal check they would tell an item the policy then REFUSED that
        // "the normal stash move runs" from a call that returns WITHOUT running it, and the gate
        // line would repeat on every shift-click.
        if (noteUnsupportedEquipped(itemId)) {
            logW("deposit REFUSED: %s is equipped - unequip it into a bag first, a shift-click "
                 "from an equipment slot is not handled",
                 record ? record : (entryRecord ? entryRecord : "?"));
            logD("id=%u is in no sack the mod can see, on a path where it should still be in one "
                 "(%s)", itemId, path);
            logD("reagent collect: the engine decides this one on its own - the mod does not arm "
                 "the byte, the item stays on the character and the normal stash move runs");
            logD("reagent gate: NOT armed at the choke point (id=%u %s) - UNSUPPORTED path (see "
                 "the two lines above)",
                 itemId, record ? record : (entryRecord ? entryRecord : "?"));
        }
    } else if (tableDeposit) {
        // The gate byte is NOT written when the private table took the item. The byte exists to
        // pass the engine's own test at 0x2CEC94, inside the original this path never calls, so
        // it decides nothing here - and the caller destroys the source the moment we return true
        // (exe 001EAB4E RemoveItem r8b=1 / 001EAB5D SendRemoveItemFromInventory). Writing it
        // would be an engine write on an object about to be destroyed, on the one path that is
        // deliberately free of them.
        static volatile LONG saidNoArm = 0;
        if (!InterlockedExchange(&saidNoArm, 1)) {
            logD("reagent gate: not armed - the private table took this one, the engine's map was "
                 "never asked (no Item+0x%X write, no CreateItem, no node)",
                 g_flagOffset);
        }
    } else if (!key.empty() && record) {
        // With mp_collect=1 this branch is unreachable - the arm is behind gateShouldArm(),
        // which asks mpBarred() like everything else, and a barred deposit was already REFUSED
        // above. It is kept so that a user who set mp_collect=0 and then wonders why nothing arms
        // gets a named reason.
        if (mpBarred()) {
            logW("deposit REFUSED: %s - %s; set mp_collect=1 to collect in every session mode",
                 record, gateDisarmReason());
            logD("the gate was not armed at the choke point for id=%u", itemId);
        } else if (!gateShouldArm()) {
            logD("reagent gate: NOT armed at the choke point (id=%u %s) - %s", itemId, record,
                 gateDisarmReason());
        } else {
            GdItem* armItem = findItemById(itemId);
            if (!armItem || !itemIsLive(armItem, itemId)) {
                logD("reagent gate: NOT armed at the choke point (id=%u %s) - the id no longer "
                     "resolves to a live object",
                     itemId, record);
            } else {
                registerItem(armItem, record);
                if (writeFlag(armItem, itemId, 1)) {
                    logD("reagent gate: armed at the choke point (id=%u, path: %s)", itemId, path);
                } else {
                    logW("deposit REFUSED: %s - the mod could not arm the item for this deposit",
                         record);
                    logD("the byte write at Item+0x%X was refused for id=%u", g_flagOffset,
                         itemId);
                }
            }
        }
    }

    // A deposit of one of OUR records does NOT call the original: no node is ever inserted, so
    // there is nothing of ours in reagents.gst. The original still runs for every REAL crafting
    // material, unchanged.
    const bool r = tableDeposit ? true : callAddOriginal(self, itemId);
    const LONG n = InterlockedIncrement(&g_addCalls);
    if (r && !key.empty()) {
        InterlockedIncrement(&g_accepts);
        // The table's own count, read back from the row that is already on disk. The journal
        // write is storeOnDeposit's job and it has already happened - on this thread, to the
        // file - so nothing is written again here.
        const int cnt = tableHeldOf(record);
        const int after = cnt > 0 ? cnt : 1;
        try {
            if (g_rescueEmptied) g_rescueEmptied->erase(key);
        } catch (...) {
        }
        // The exe repaints the badge itself after PrimaryReagentActivate succeeds (0x1739A0:
        // GameEngine+0x19B0 vtable +0x88), but Game.dll's own deposit routes and the shift-click
        // site do not, so it is done here for every accepted deposit. Harmless when the engine
        // already did it: Sync only touches a box whose item id actually differs.
        reagentSyncCaravan("an accepted deposit");
        {
            // "written to disk before the deposit was accepted" and NOT "queued": the row really
            // is durable at this point, and a queue would be a promise the code cannot keep.
            logD("reagent collect: ACCEPTED id=%u %s -> THE PRIVATE TABLE now holds %d (journal: "
                 "written to disk before the deposit was accepted; no engine node was inserted, "
                 "so nothing of this item is in reagents.gst) (depositedStack=%d) [%s] "
                 "[path: %s]",
                 itemId, record, after, depositedStack, gateStateText(), path);
            // `journalOnDeposit`'s "items stored on this page are lost if the mod is removed"
            // warning is not reached for a table deposit (that function is skipped), and it would
            // be FALSE if it were: this copy never entered the engine's map, so no unmodded load
            // can refund it, prune it or see it. It is in the mod's own file and nowhere else,
            // which is what the user has to act on.
            static volatile LONG tableWarned = 0;
            if (!InterlockedExchange(&tableWarned, 1)) {
                logI("*** your collection lives in %s and NOWHERE ELSE - BACK THAT FILE UP ***",
                     journalPath());
                logD("a deposited item never enters reagents.gst, so removing the mod neither "
                     "refunds nor deletes it - and an unmodded game cannot show it to you");
            }
        }
        // With owned_only=1 the deposited record's box is parked off-grid until the mod's next
        // reagent-map walk, so without these two lines the item that just went in is invisible
        // for up to plate_count_ms (1 s by default) - and a user who has just dropped one and
        // sees nothing may drop a SECOND copy. The deposit itself can never be lost this way: the
        // deposit path never touches a box at all (PrimaryReagentActivate takes an item id and
        // the box hit-test loop is not on that branch). This only removes the window in which
        // nothing appears to have happened.
        //   force=true : the walk's own plate_count_ms throttle is exactly what has to be
        //                bypassed here; the walk is the same one plateTick makes on this thread.
        //   liveRelayoutVisible(): ONE interlocked store, consumed by the next game-thread tick.
        plateOwnedRefresh(true);
        liveRelayoutVisible();
        // Say once per session exactly what left this machine, and start the removal measurement
        // for the sessions where the source item's removal may be a round trip.
        if (mpSessionMode() != UT_MP_SINGLE) {
            if (!InterlockedExchange(&g_mpNoteLogged, 1)) {
                logD("reagent collect: multiplayer note - what left this machine is the engine's "
                     "own SendRemoveItemFromInventory command (packet type 0xB6, "
                     "{characterId,itemId}) - the same message vanilla sends for any crafting "
                     "material; the reagent map, reagents.gst and Item+0xC64 are local and were "
                     "not sent. A take sends the engine's own SendAddItemToInventory command "
                     "(packet type 0xB7) carrying the item's full ItemReplicaInfo, read from the "
                     "item AFTER the mod's restore - the same message vanilla sends for any item");
            }
            mpPendingNote(itemId, record);
        }
    } else if (n <= kGateLogMax) {
        logD("reagent: AddItemToReagents(id=%u) -> %d  (autoDepositDepth=%ld, path: %s)", itemId,
             r ? 1 : 0, InterlockedCompareExchange(&g_depositDepth, 0, 0), path);
    }
    return r;
}

// The two drop-condition bytes, written together. SEH only.
bool writeDropBytes(GdItem* item, unsigned char soulbound, unsigned char untradeable) {
    if (!item || !g_soulboundOffset || !g_untradeableOffset) return false;
    __try {
        *((unsigned char*)item + g_soulboundOffset) = soulbound;
        *((unsigned char*)item + g_untradeableOffset) = untradeable;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The original, with the restore in a __finally so a fault inside the engine cannot leave the
// user's item with its soulbound flag laundered.
//
// The restore must NOT be conditional on the original returning false. "On true the engine has
// already destroyed the Item" is not what the code does: PrimaryReagentActivate's success path
// (0x173976 PlayDropSound, 0x17398A GetPlayerCtrl, 0x173995 SendRemoveItemFromInventory,
// 0x17399A cursor+0x30 = 0, 0x1739B1 Sync) destroys nothing, and AddItemToReagents' only
// DestroyObjectEx (0x2CEE36, IAT 0x5F3118) takes the TEMPORARY merge prototype, never the
// deposited item. A successful bracketed drag would otherwise leave a LIVE item permanently
// laundered (+0xC00/+0xC02 = 0), which lets it into an ordinary stash tab. What makes the write
// safe is the IsObjectOnDeletedList + IsObjectIdOnDeletedList + stored-id triple, never the
// return value.
//
// `outcome` carries EVIDENCE: 0 the Item was gone (nothing to restore), 1 the two bytes went back
// in, 2 the item was live and the store failed. The one place the mod deliberately clears a
// player item's flags must not report "restored" from a return value alone.
bool callPrimaryOriginal(void* self, GdItem* item, unsigned int id, bool bracketed,
                         unsigned char s, unsigned char u, int* outcome) {
    bool r = false;
    __try {
        r = o_PrimaryReagentActivate ? o_PrimaryReagentActivate(self) : false;
    } __finally {
        if (bracketed) {
            if (!itemIsLive(item, id)) {
                *outcome = 0;
            } else {
                *outcome = writeDropBytes(item, s, u) ? 1 : 2;
            }
            g_bracketFlagBits = 0;
        }
    }
    return r;
}

bool __cdecl hk_PrimaryReagentActivate(void* self) {
    // CursorHandlerItemMove::PrimaryReagentActivate (Game.dll 0x1738E0) has NO hit test of its
    // own; it resolves the cursor item [this+0x30], refuses on soulbound (+0xC00) or untradeable
    // (+0xC02) at 0x173947/0x17394E WITHOUT calling anything, and otherwise calls
    // GameEngine::AddItemToReagents at 0x17396D.
    //
    // The probe is taken as LATE as possible - immediately before the original - because an id
    // read earlier often does not resolve to a live Item yet, including ids the very next
    // AddItemToReagents accepts. If it still cannot resolve, the line is held back and printed
    // only when the deposit did NOT succeed.
    const unsigned int cursorId = cursorItemId(self);
    GdItem* item = findItemById(cursorId);
    ItemProbe pr;
    probeItem(item, cursorId, &pr);
    const bool trace = InterlockedCompareExchange(&g_dropLogs, 0, 0) < kGateLogMax;
    if (trace && pr.ok) {
        InterlockedIncrement(&g_dropLogs);
        logDropProbeLine("PrimaryReagentActivate (drop on a box)", cursorId, pr);
    }

    // ---- the SOULBOUND bracket ---------------------------------------------------------------
    // A soulbound unique is refused by the engine's own drop test before AddItemToReagents is
    // ever reached, while the exe's quick-move site (0x1EAB38) has no such test at all - so the
    // same item goes in on shift-click and never on a drag. Clearing the two bytes for the
    // duration of THIS call closes that asymmetry. The window is a handful of instructions on
    // the game thread and the only engine code that can observe it is this function's own test:
    // AddItemToReagents never reads +0xC00/+0xC02 (its only pre-test is +0xC64 at 0x2CEC94).
    //
    // THIS ONE STAYS BARRED IN A MULTIPLAYER SESSION, and it is the only gate in the mod that
    // asks multiplayerActive() rather than mpBarred(). +0xC00 /
    // +0xC02 are gameplay RULES living in the item's own state, and the bracket deliberately
    // makes the engine believe for a handful of instructions that a soulbound item is tradeable.
    // In single player that is between the user and their own save; with peers present it is a
    // rule change on a shared object, and the benefit is small because a SHIFT-CLICK gets the
    // same item in anyway (the exe's quick-move site 0x1EAB38 has no soulbound test at all).
    //
    // The multiplayer test comes BEFORE itemIsLive: itemIsLive is an SEH frame plus
    // Object::GetObjectId plus two ObjectManager deleted-list calls, and in a barred session it
    // need not run at all. The refusal line is latched per cursor id, because this detour runs on
    // EVERY drop attempt and one stubborn drag would otherwise fill the log.
    unsigned char savedS = 0, savedU = 0;
    bool bracketed = false;
    const bool sbCandidate = g_cfg.soulboundCollect && item && pr.ok &&
                             (pr.soulbound > 0 || pr.untradeable > 0) &&
                             isOurRecordSafe(pr.record) && gateShouldArm();
    const bool sbBarred = sbCandidate && multiplayerActive();
    if (sbBarred && InterlockedExchange(&g_sbBarredId, (LONG)cursorId) != (LONG)cursorId) {
        logW("deposit REFUSED: a soulbound item cannot be dropped in in multiplayer - "
             "SHIFT-CLICK it instead");
        logD("the soulbound bracket is single-player only; the quick-move path has no soulbound "
             "test");
    }
    const bool sbWanted = sbCandidate && !sbBarred && itemIsLive(item, cursorId);
    if (sbWanted) {
        savedS = (unsigned char)(pr.soulbound > 0 ? pr.soulbound : 0);
        savedU = (unsigned char)(pr.untradeable > 0 ? pr.untradeable : 0);
        if (writeDropBytes(item, 0, 0)) {
            bracketed = true;
            // Hand the flags to the choke point, which runs INSIDE this window.
            g_bracketFlagBits = (savedS ? UT_JF_SOULBOUND : 0u) | (savedU ? UT_JF_UNTRADEABLE : 0u);
            logD("reagent drop: soulbound bracket OPEN for %s id=%u on thread %lu (saved "
                 "+0x%X=%u, +0x%X=%u; restored unless the deposit succeeds)",
                 pr.record, cursorId, GetCurrentThreadId(), g_soulboundOffset,
                 (unsigned)savedS, g_untradeableOffset, (unsigned)savedU);
        }
    }

    const LONG entriesBefore = InterlockedCompareExchange(&g_addEntries, 0, 0);
    const LONG addsBefore = InterlockedCompareExchange(&g_addCalls, 0, 0);
    const LONG refusalsBefore = InterlockedCompareExchange(&g_refusals, 0, 0);
    const LONG acceptsBefore = InterlockedCompareExchange(&g_accepts, 0, 0);
    int restoreOutcome = 0;
    const bool r =
        callPrimaryOriginal(self, item, cursorId, bracketed, savedS, savedU, &restoreOutcome);
    const LONG entriesAfter = InterlockedCompareExchange(&g_addEntries, 0, 0);
    const LONG addsAfter = InterlockedCompareExchange(&g_addCalls, 0, 0);
    const LONG refusalsAfter = InterlockedCompareExchange(&g_refusals, 0, 0);
    const LONG acceptsAfter = InterlockedCompareExchange(&g_accepts, 0, 0);
    if (bracketed) {
        if (restoreOutcome == 2) InterlockedIncrement(&g_soulboundRestoreFailed);
        logD("reagent drop: soulbound bracket CLOSED for id=%u - the original returned %d and the "
             "two bytes were %s",
             cursorId, r ? 1 : 0,
             restoreOutcome == 1
                 ? "RESTORED (+0xC00/+0xC02 written back and the store returned ok)"
                 : (restoreOutcome == 0
                        ? "not restorable - the Item failed the liveness triple check, i.e. it "
                          "is already gone; the journal entry carries the flags and identity "
                          "re-applies them on the take back"
                        : "***** RESTORE FAILED ***** - the Item is LIVE but the store faulted, "
                          "so it is still laundered (+0xC00/+0xC02 = 0) and could be placed in "
                          "an ordinary stash tab"));
    }
    // The held-back probe line, now that the outcome is known.
    if (trace && !pr.ok && acceptsAfter == acceptsBefore) {
        ItemProbe again;
        probeItem(findItemById(cursorId), cursorId, &again);
        InterlockedIncrement(&g_dropLogs);
        logDropProbeLine("PrimaryReagentActivate (drop on a box, re-resolved after the call)",
                         cursorId, again);
    }
    // THREE-way. `g_addEntries` counts entries into the choke point; `g_addCalls` counts the
    // calls that actually reached the original. A mod refusal leaves the second unchanged, so
    // reading only that one reports "AddItemToReagents was NEVER called" over our own REFUSED
    // line.
    if (!r) {
        const char* why =
            entriesAfter == entriesBefore
                ? "AddItemToReagents was NEVER called, so the engine bailed at its own "
                  "soulbound/untradeable test (Item+0xC00 / Item+0xC02) - see the drop line above "
                  "for the two bytes"
                : (refusalsAfter != refusalsBefore
                       ? "AddItemToReagents WAS called and the MOD refused it - see the REFUSED "
                         "line above for the reason"
                       : (addsAfter != addsBefore
                              ? "AddItemToReagents WAS called, the mod allowed it and the ENGINE "
                                "refused: the craftingMaterial byte was not set on that Item at "
                                "that moment"
                              : "AddItemToReagents was entered but the original was never "
                                "reached - see the line above"));
        logD("reagent drop: PrimaryReagentActivate returned 0 - %s", why);
    } else if (InterlockedCompareExchange(&g_dropLogs, 0, 0) <= kGateLogMax) {
        logD("reagent drop: PrimaryReagentActivate returned 1");
    }
    return r;
}

// Shift-click. `false` is what the base CursorHandler::QuickDropInReagents returns (its whole body
// is `xor al,al; ret`), i.e. "not handled" - so returning it WITHOUT calling the original lets the
// game's normal quick-move to the transfer stash run. The collection is a display case, not a
// vacuum: it must never swallow an item the user shift-clicked.
bool __cdecl hk_QuickDropInReagents(void* self) {
    if (g_cfg.collectQuickPass) {
        bool ours = false;
        const char* record = nullptr;
        try {
            const unsigned int id = cursorItemId(self);
            GdItem* item = findItemById(id);
            record = item ? safeObjectName(item) : nullptr;
            ours = record && isOurRecord(record);
        } catch (...) {
            ours = false;
        }
        if (ours) {
            if (InterlockedIncrement(&g_dropLogs) <= kGateLogMax) {
                logD("reagent collect: quick-drop of %s NOT HANDLED (returning false without "
                     "calling the original, so the normal quick-move to the stash tab runs)",
                     record);
            }
            return false;
        }
    }
    if (InterlockedIncrement(&g_dropLogs) <= kGateLogMax) {
        logDropCandidate("QuickDropInReagents (shift-click)", self);
    }
    const bool r = o_QuickDropInReagents ? o_QuickDropInReagents(self) : false;
    if (InterlockedCompareExchange(&g_dropLogs, 0, 0) <= kGateLogMax) {
        logD("reagent drop: QuickDropInReagents returned %d", r ? 1 : 0);
    }
    return r;
}

// ---- which call does the reagent-box TAKE use? ------------------------------------------------
// The UI take is in Grim Dawn.exe, which exports nothing, and it is NOT TakeItemFromReagents -
// both overloads' callers are crafting consumers. The three item-creation detours below log only,
// and only for records our page carries, so the log names the take path exactly - and with it the
// substitution point identity preservation needs.
//
// Which side of the page a creation is on: the deposit's own prototype is built by
// GameEngine::AddItemToReagents (Game.dll rva 0x2CECC4); the UI take is built by the reagent-page
// widget, which lives in Grim Dawn.exe. Classifying by the return address' module is therefore
// exact, and it is the hook point identity preservation substitutes into.
const unsigned char* g_exeBase = nullptr;
size_t g_exeSize = 0;
const unsigned char* g_gameBase = nullptr;
size_t g_gameSize = 0;

void moduleRange(HMODULE m, const unsigned char** base, size_t* size) {
    *base = nullptr;
    *size = 0;
    if (!m) return;
    MODULEINFO mi;
    memset(&mi, 0, sizeof(mi));
    if (GetModuleInformation(GetCurrentProcess(), m, &mi, sizeof(mi))) {
        *base = (const unsigned char*)mi.lpBaseOfDll;
        *size = mi.SizeOfImage;
    }
}

const char* creationSide(const void* ret) {
    const unsigned char* p = (const unsigned char*)ret;
    if (g_exeBase && p >= g_exeBase && p < g_exeBase + g_exeSize) return "TAKE (Grim Dawn.exe)";
    if (g_gameBase && p >= g_gameBase && p < g_gameBase + g_gameSize) return "DEPOSIT (Game.dll)";
    return "unclassified module";
}

// Which route reached the choke point. `Grim Dawn.exe` imports `GameEngine::AddItemToReagents`
// itself (IAT rva 0x2DC320) and calls it from exactly two sites, 0x001EAB38 and 0x001EC65A - the
// drop-on-a-box handler and the shift-click quick-move handler. Both do `test al,al;
// je <next destination>`, so returning false leaves the item exactly where it was. The exe does
// NOT import CursorHandlerItemMove::QuickDropInReagents, which is why a shift-click never appears
// in the drop trace: it bypasses that trace, not the detour.
//
// ---- the TWO callers whose TRUE branch is DISASSEMBLED AND REACHABLE -------------------------
// A `true` from the choke point that never calls the original is the caller's licence to destroy
// the source, so the mod may only give it to a caller it has read:
//   exe site A, return address 0x1EAB3E:  0x1EAB4E PlayerInventoryCtrl::RemoveItem(ctrl,id,true)
//                                         0x1EAB5D SendRemoveItemFromInventory      <- ACCEPTED
//   the DRAG,   return address 0x173972:  0x173995 SendRemoveItemFromInventory(edx=[rdi+0x30])
//                                         0x17399A the cursor slot cleared          <- ACCEPTED
//   exe site B, return address 0x1EC660:  0x1EC66D / 0x1EC679, the same two         <- NOT here
// Site B's TRUE branch really does remove the source (001EC660 test al,al / 001EC662 je 0x1EC694
// / 001EC697 [rax+0x408], the same shape as site A), but it is the EQUIPMENT quick-move: the
// choke point classifies every site-B deposit as `equipped` unconditionally and the table gate
// requires `!equipped`, so listing it here would be unreachable weight advertising support the
// mod does not offer. The site-A bounds are the ones `depositPathLabel` uses, so the label and
// the decision cannot disagree. `DepositSackIntoReagents` is deliberately absent too: the mod
// refuses every bulk deposit at the choke point (g_depositDepth > 0) and never reaches it.
//
// ---- WHICH of the two, because they need DIFFERENT proofs -----------------------------------
//   UT_CALLER_DRAG  - the source is the CURSOR SLOT. 0x173995 SendRemoveItemFromInventory takes
//                     `[rdi+0x30]`, the id the handler itself read at 0x1738FF, and 0x17399A
//                     clears that slot. No container is searched, so the removal cannot MISS:
//                     the proof is unconditional.
//   UT_CALLER_EXE_A - the source is a bag. 0x1EAB4E PlayerInventoryCtrl::RemoveItem searches
//                     `ic+0x20..0x28` only, so the removal is guaranteed exactly once ONLY while
//                     the id is in one of those bags - which is what `bagsHoldItem` asks the
//                     engine, on the same container, with the same key offset.
//   anything else   - not proved, therefore never given a `true` without the original, and
//                     REFUSED rather than handed to the engine: the game's own save files are
//                     foreign territory.
// The two exe call sites are LOCATED BY SIGNATURE, not written down as addresses. Until the scan
// has succeeded both are null and `depositCallerKind` answers UNKNOWN, which is the fail-closed
// answer: the deposit is REFUSED rather than taken on an unproven path. The scan is retried from
// the game thread because the exe's .text is Steam-DRM encrypted until its stub has run (see
// `depositSiteTick` below, next to the take-site scan, which works the same way).
const unsigned char* g_siteA = nullptr;
const unsigned char* g_siteB = nullptr;
// The cursor DRAG site lives in Game.dll, which is NOT encrypted, so it is located once at
// export-resolution time and the all-or-nothing gate covers it: no drag deposit is possible at
// all unless this scan found exactly one match before the first hook was installed.
const unsigned char* g_siteDrag = nullptr;

// One module's .text, for a scan (defined further down, next to its first user). Game.dll and
// Engine.dll are plain; the exe has its own reader (identityTextRange) because its .text is only
// readable once the Steam stub has decrypted it.
bool moduleTextRange(HMODULE m, const unsigned char** lo, const unsigned char** hi);

// Called once from reagentInit, before any hook exists. Reports to the bindings table either way.
void resolveDragSite() {
    const unsigned char *lo = nullptr, *hi = nullptr;
    if (!moduleTextRange(GetModuleHandleA("Game.dll"), &lo, &hi)) {
        bindingsNote(kUtSigDragSite.name, 0, false, "Game.dll's .text section to be readable");
        return;
    }
    const unsigned char* hit = nullptr;
    const int n = utBindScan(lo, hi, kUtSigDragSite, &hit, 1);
    if (n == 1 && hit) {
        g_siteDrag = hit;
        bindingsNote(kUtSigDragSite.name, (unsigned long long)(size_t)(hit - (const unsigned char*)
                                                                                GetModuleHandleA(
                                                                                    "Game.dll")),
                     true, "exactly one match in Game.dll's .text");
        logD("deposit: the cursor-drag site located by signature in Game.dll at +0x%llX (accepted "
             "return window +0x%llX..+0x%llX)",
             (unsigned long long)(size_t)(hit - (const unsigned char*)GetModuleHandleA("Game.dll")),
             (unsigned long long)(size_t)(hit + kUtDragWindowLo -
                                          (const unsigned char*)GetModuleHandleA("Game.dll")),
             (unsigned long long)(size_t)(hit + kUtDragWindowHi -
                                          (const unsigned char*)GetModuleHandleA("Game.dll")));
        return;
    }
    char why[160];
    _snprintf_s(why, sizeof(why), _TRUNCATE,
                "exactly one match in Game.dll's .text (this build matched %d time(s))", n);
    bindingsNote(kUtSigDragSite.name, 0, false, why);
}

int depositCallerKind(const void* ra) {
    const unsigned char* p = (const unsigned char*)ra;
    if (g_exeBase && p >= g_exeBase && p < g_exeBase + g_exeSize) {
        if (g_siteA && p > g_siteA + kUtSiteAWindowLo && p < g_siteA + kUtSiteAWindowHi) {
            return kUtDepCallerExeA;
        }
        return kUtDepCallerUnknown;
    }
    if (g_siteDrag && p > g_siteDrag + kUtDragWindowLo && p < g_siteDrag + kUtDragWindowHi) {
        return kUtDepCallerDrag;
    }
    return kUtDepCallerUnknown;
}


const char* depositPathLabel(const void* ra, char* buf, size_t cap) {
    const unsigned char* p = (const unsigned char*)ra;
    if (g_exeBase && p >= g_exeBase && p < g_exeBase + g_exeSize) {
        const size_t rva = (size_t)(p - g_exeBase);
        const bool inA = g_siteA && p > g_siteA + kUtSiteAWindowLo && p < g_siteA + kUtSiteAWindowHi;
        const bool inB = g_siteB && p > g_siteB + kUtSiteBWindowLo && p < g_siteB + kUtSiteBWindowHi;
        const char* which = inA   ? "exe drop/quick-move site A"
                            : inB ? "exe drop/quick-move site B"
                                  : "exe";
        _snprintf_s(buf, cap, _TRUNCATE, "%s +0x%zX", which, rva);
    } else if (g_gameBase && p >= g_gameBase && p < g_gameBase + g_gameSize) {
        _snprintf_s(buf, cap, _TRUNCATE, "Game.dll +0x%zX", (size_t)(p - g_gameBase));
    } else {
        _snprintf_s(buf, cap, _TRUNCATE, "unclassified %p", ra);
    }
    const LONG d = InterlockedCompareExchange(&g_depositDepth, 0, 0);
    if (d > 0) {
        const size_t n = strlen(buf);
        if (n + 24 < cap) _snprintf_s(buf + n, cap - n, _TRUNCATE, " [autoDepositDepth=%ld]", d);
    }
    return buf;
}

void logCreation(const char* where, const char* record, const void* result, const void* ret) {
    // Item::CreateItem is THE creation call for every item in every sack, so at the main menu it
    // fires hundreds of times while the stashes deserialise. Only the caravan window matters here.
    if (!InterlockedCompareExchange(&g_transferOpenNow, 0, 0)) return;
    if (!record || !*record || !isOurRecord(record)) return;
    if (InterlockedIncrement(&g_creationLogs) > 60) return;
    logD("reagent take-probe: %s(\"%s\") -> %p  <- %s (return address %p)", where, record, result,
         creationSide(ret), ret);
}

const char* replicaRecord(const void* replica) {
    if (!replica) return nullptr;
    __try {
        size_t size = 0;
        const char* text = nullptr;
        if (!looksLikeString((const unsigned char*)replica, 0x08, &size, &text)) return nullptr;
        return size ? text : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// The SAVED COUNT of one reagent row, read out of the replica the engine is about to hand to
// `Item::CreateItem`. `ReadPlayerReagents` puts it there at 0x2CE395 (`mov [rbp+0x198], r15d` -
// the replica's stack mirror, replica + 0x178 on this build, decoded from `Item::SetStackSize`
// into `g_replicaStackOff`) one instruction before the call at 0x2CE3A0, so this is the only
// place in the whole load where the row's count and the row's record are both in hand at once.
// A MOD-SIDE READ of engine memory, so swallowing the fault is correct here: a fault simply
// means "not proved".
bool replicaStackMirror(const void* replica, unsigned int* out) {
    if (out) *out = 0;
    if (!replica || !out || !g_replicaStackOff || !g_replicaSize) return false;
    if (g_replicaStackOff + 4 > g_replicaSize) return false;
    __try {
        *out = *(const unsigned int*)((const unsigned char*)replica + g_replicaStackOff);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *out = 0;
        return false;
    }
}

const char* msvcStringText(const void* s) {
    if (!s) return nullptr;
    __try {
        size_t size = 0;
        const char* text = nullptr;
        if (!looksLikeString((const unsigned char*)s, 0, &size, &text)) return nullptr;
        return size ? text : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// THE substitution point. `identitySubstitute` returns non-null only when the mod's own state
// machine says this exact call is the reagent page handing back a record the journal holds the
// deposited item's ItemReplicaInfo for (see the identity block below); the return address is
// never the classifier, only ever an extra positive check that gets logged. Every other creation
// - crafting, enchanting, the market, loot - runs completely unchanged.
//
// Two rules here, and NEITHER of them swallows anything:
//  (a) the call to the original sits in an SEH frame whose FILTER logs the fault (code, address
//      as module+rva, both parameters, the thread, the armed record and the overlay's slots) and
//      flushes synchronously, then returns EXCEPTION_CONTINUE_SEARCH so the engine's own handler
//      runs exactly as it would have. Swallowing an access violation in the middle of
//      ItemReplicaInfo::operator= would hand the player a half-assigned Item. An engine exception
//      is never swallowed.
//  (b) g_idBusy is released on EVERY exit path, including an unwind. Leaving it at 1 makes every
//      later substitution return nullptr for the rest of the session.
GdItem* __cdecl hk_ItemCreateItem(const void* replica) {
    const void* ret = _ReturnAddress();
    const void* sub = nullptr;
    GdItem* r = nullptr;
    bool handed = false;
    __try {
        // BOTH identitySubstitute calls must be inside this frame, so the `__finally` below
        // really does run on every exit path the header promises - an SEH unwind out of
        // `identitySubstituteAtLoad` included. That function's own wrapper is a C++
        // `try/catch(...)`, which under /EHsc does NOT catch an access violation, and
        // first-chance AVs during ReadPlayerReagents are routine and handled by the engine, so a
        // load really can continue past one.
        sub = identitySubstitute(replica, ret);
        const void* use = sub ? sub : replica;
        __try {
            r = o_ItemCreateItem ? o_ItemCreateItem(use) : nullptr;
        } __except (logEngineFault("Item::CreateItem", GetExceptionInformation()->ExceptionRecord,
                                   GetCurrentThreadId()),
                    EXCEPTION_CONTINUE_SEARCH) {
            r = nullptr;  // never reached: the filter always continues the search
        }
        if (sub) {
            identityAfterCreate(r);
            handed = true;
        }
        logCreation("Item::CreateItem", replicaRecord(replica), r, ret);
    } __finally {
        if (sub && !handed) {
            // The engine faulted (or unwound) inside its own creation. identityAfterCreate never
            // ran, so release the identity state here or nothing else can ever substitute again.
            InterlockedIncrement(&g_idFaults);
            identityClear();
            InterlockedExchange(&g_idBusy, 0);
        }
    }
    return r;
}

GdItem* __cdecl hk_CreateItemInInventory(void* self, const void* record) {
    GdItem* r = o_CreateItemInInventory ? o_CreateItemInInventory(self, record) : nullptr;
    logCreation("ControllerCharacter::CreateItemInInventory", msvcStringText(record), r,
                _ReturnAddress());
    return r;
}

void __cdecl hk_CreateItemForCharacter(GdGameEngine* self, unsigned int charId,
                                       const void* worldCoords, void* replica, void* name) {
    if (o_CreateItemForCharacter) {
        o_CreateItemForCharacter(self, charId, worldCoords, replica, name);
    }
    logCreation("GameEngine::CreateItemForCharacter", replicaRecord(replica), nullptr,
                _ReturnAddress());
}

// Observed only: the 4th AddItemToReagents caller (a drop the UI could not place anywhere).
bool __cdecl hk_Cancel(void* self) {
    const LONG n = InterlockedIncrement(&g_cancelCalls);
    const bool r = o_Cancel ? o_Cancel(self) : false;
    if (n <= 12) logD("reagent: CursorHandlerItemMove::Cancel -> %d", r ? 1 : 0);
    return r;
}

int __cdecl hk_TakeFromReagents(GdGameEngine* self, const void* name, int count) {
    g_gameEngine = self;
    const int r = o_TakeFromReagents ? o_TakeFromReagents(self, name, count) : 0;
    const LONG n = InterlockedIncrement(&g_takeCalls);
    if (n <= kGateLogMax) logD("reagent: TakeItemFromReagents(string, count=%d) -> %d", count, r);
    return r;
}

int __cdecl hk_TakeFromReagentsId(GdGameEngine* self, unsigned int id, int count) {
    g_gameEngine = self;
    GdItem* item = findItemById(id);
    const char* record = item ? safeObjectName(item) : nullptr;
    const int r = o_TakeFromReagentsId ? o_TakeFromReagentsId(self, id, count) : 0;
    const LONG n = InterlockedIncrement(&g_takeIdCalls);
    if (record && *record) {
        try {
            std::string key(record);
            toLower(&key);
            if (g_pageRecords && g_pageRecords->find(key) != g_pageRecords->end()) {
                bool mapOk = false;
                const void* node = findReagentNode(key, &mapOk);
                int after = trackedCount(key) - count;
                if (mapOk) {
                    const int nodeN = nodeCountOf(node);
                    after = !node ? 0 : (nodeN > 0 ? nodeN : 1);
                }
                if (after < 0) after = 0;
                setTrackedCount(key, after);
                logD("reagent collect: TAKE id=%u %s count=%d -> %d, the page now holds %d", id,
                     record, count, r, after);
                return r;
            }
        } catch (...) {
            return r;
        }
    }
    if (n <= kGateLogMax) {
        logD("reagent: TakeItemFromReagents(id=%u, count=%d) -> %d", id, count, r);
    }
    return r;
}

// The three auto-deposit entry points. While any of them runs the whole registry's gate byte is
// 0, so the engine's own compatibility test refuses our uniques exactly as it does today.
void depositEnter(const char* which) {
    if (InterlockedIncrement(&g_depositDepth) == 1) {
        setWholeRegistry(0);
    }
    const LONG n = InterlockedIncrement(&g_depositCalls);
    if (n <= kGateLogMax) logD("reagent: auto-deposit ENTER %s (gate disarmed)", which);
}
void depositLeave(const char* which) {
    const LONG d = InterlockedDecrement(&g_depositDepth);
    if (d == 0) setWholeRegistry(1);
    if (InterlockedCompareExchange(&g_depositCalls, 0, 0) <= kGateLogMax) {
        logD("reagent: auto-deposit LEAVE %s (gate re-armed)", which);
    }
}

void __cdecl hk_DepositReagents(void* self) {
    depositEnter("PlayerInventoryCtrl::DepositReagents");
    if (o_DepositReagents) o_DepositReagents(self);
    depositLeave("PlayerInventoryCtrl::DepositReagents");
}

bool __cdecl hk_DepositSack(GdSack* self, void* controllerPlayer) {
    depositEnter("InventorySack::DepositSackIntoReagents");
    const bool r = o_DepositSack ? o_DepositSack(self, controllerPlayer) : false;
    depositLeave("InventorySack::DepositSackIntoReagents");
    return r;
}

void __cdecl hk_DepositTransfer(GdGameEngine* self) {
    depositEnter("GameEngine::DepositTransferReagents");
    if (o_DepositTransfer) o_DepositTransfer(self);
    depositLeave("GameEngine::DepositTransferReagents");
}

void __cdecl hk_DestroyObjectEx(void* self, void* obj, const char* file, int line) {
    // The engine REUSES object ids, so a destroyed prototype's id must leave g_mapProtoIds -
    // otherwise a later player item that inherits that id could never be disarmed, and
    // Item::CanBePlacedInTransferStash+0x9 would bar it from every stash tab. The merge branch of
    // AddItemToReagents creates a temporary prototype and destroys it at 0x2CEE36, so this really
    // happens on an ordinary second deposit.
    //
    // The unlocked g_mapOwnedSize fast path has to come BEFORE safeObjectId: that helper is an
    // SEH frame plus an Object::GetObjectId call, and this detour fires for every object the
    // engine destroys - thousands at a world teardown, with the set usually empty.
    //
    // The same object id also MARKS an MP-removal measurement entry as destroyed. This detour is
    // the engine DESTROYING THE OBJECT, which happens in single player and on the machine that
    // executes the removal command, and on a client may never happen at all - there is no echo
    // and no destruction replication on this path. It is therefore NOT the confirmation that a
    // deposit completed and nothing in the mod may treat it as one: the bag slot was already
    // emptied locally by the exe (PlayerInventoryCtrl::RemoveItem, 0x1EAB4E), the registry prune
    // and the id-recycling guard below are id-keyed and idempotent, and the measurement line
    // prints with or without this mark.
    if (InterlockedCompareExchange(&g_mapOwnedSize, 0, 0) ||
        InterlockedCompareExchange(&g_mpPendingCount, 0, 0)) {
        const unsigned int oid = safeObjectId((GdItem*)obj);
        if (InterlockedCompareExchange(&g_mapOwnedSize, 0, 0)) forgetMapOwnedId(oid);
        mpPendingClear(oid);
    }
    __try {
        forgetItem(obj);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (o_DestroyObjectEx) o_DestroyObjectEx(self, obj, file, line);
}

// World teardown. Every Item the mod remembers belongs to the session that is ending here, so the
// registry is emptied BEFORE the engine destroys them. This is the ONE moment at which the HUD,
// the caravan window and every Item the mod remembers are all still alive and are all about to be
// destroyed. In order:
//   1. disarm - write 0 into every byte the mod set, while the Items still exist;
//   2. drop every table that keys on a pointer or an object id from the dying world;
//   3. tell ut_plate the window is going (plateOnWorldTeardown performs NO engine write - it only
//      forgets the captured window and releases the textures the MOD loaded);
//   4. only then let the engine tear the world down.
void teardownTables(const char* why);  // defined with the take watch, below
void pageOnWorldTeardown();            // defined with the record substitution, below

void exitDisarmSeh() {
    __try {
        gateDisarmAll("GameEngine::ExitPlayingMode");
        registryClear("GameEngine::ExitPlayingMode");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void exitPlateSeh() {
    __try {
        plateOnWorldTeardown();
        liveOnWorldTeardown();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void __cdecl hk_ExitPlayingMode(GdGameEngine* self) {
    pageOnWorldTeardown();  // the next world gets its own database, and its own answer
    exitDisarmSeh();
    try {
        teardownTables("GameEngine::ExitPlayingMode");
    } catch (...) {
    }
    exitPlateSeh();
    if (o_ExitPlayingMode) o_ExitPlayingMode(self);
}

// ---- the database checksum probe (multiplayer-critical) --------------------------------------
// "Other players need nothing installed" rests on the game's database checksum staying the
// vanilla one, and the checksum differs between a menu-only run and a run that has loaded a
// world. A single log line cannot tell menu from world, nor before the mod's own database load
// from after it, so this probe is called at five NAMED moments and the log answers the question
// by itself.
volatile LONG g_dbProbes = 0;
unsigned int g_dbSumFirst = 0;
char g_dbSumFirstWhen[48] = {0};

unsigned int dbChecksumProbe(const char* when) {
    GdEngine* engine = (g_gd.ppEngine && *g_gd.ppEngine) ? *g_gd.ppEngine : nullptr;
    if (!engine || !p_GetDbChecksum) {
        logD("reagent db: CHECKSUM PROBE [%s]: the Engine is not constructed yet (engine=%p "
             "getter=%p)",
             when, (void*)engine, (void*)p_GetDbChecksum);
        return 0;
    }
    unsigned int sum = 0;
    int custom = -1;
    __try {
        sum = p_GetDbChecksum(engine);
        if (p_HasCustomDb) custom = p_HasCustomDb(engine) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logD("reagent db: CHECKSUM PROBE [%s]: the read faulted", when);
        return 0;
    }
    const char* delta = "";
    if (!g_dbSumFirst && sum) {
        g_dbSumFirst = sum;
        _snprintf_s(g_dbSumFirstWhen, sizeof(g_dbSumFirstWhen), _TRUNCATE, "%s", when);
        delta = " (first non-zero reading of this session)";
    } else if (sum && sum != g_dbSumFirst) {
        delta = " *** CHANGED since the first reading ***";
    }
    logD("reagent db: CHECKSUM PROBE [%s]: 0x%08X HasLoadedCustomDatabase=%d db_load=%d "
         "ourArchiveLoaded=%ld (first was 0x%08X at [%s])%s",
         when, sum, custom, g_cfg.dbLoad, InterlockedCompareExchange(&g_dbLoaded, 0, 0),
         g_dbSumFirst, g_dbSumFirstWhen, delta);
    InterlockedIncrement(&g_dbProbes);
    return sum;
}

// ---- the database load ------------------------------------------------------------------------
// The overlay is loaded ONCE PER DATABASE, not once per process. A custom game - the Crucible
// (<game>\mods\survivalmode\database\*.arz) and every other <game>\mods world - runs
// Engine::LoadMainDatabase a second time on its way in, and whatever that leaves of the archives
// loaded before it, the overlay has to be in the database the world actually reads. This clears
// the once-guards so the next loadOurArchive() really loads.
void dbForgetLoad(const char* why) {
    if (!InterlockedCompareExchange(&g_dbLoaded, 0, 0) &&
        !InterlockedCompareExchange(&g_dbTried, 0, 0)) {
        return;
    }
    InterlockedExchange(&g_dbLoaded, 0);
    InterlockedExchange(&g_dbTried, 0);
    InterlockedExchange(&g_verifyDone, 0);  // the new database gets its own VERIFY lines
    g_dbRoute = "reloading";
    logD("reagent db: the overlay must go in again, reload #%ld (%s)",
         InterlockedIncrement(&g_dbReloads), why);
}

bool loadOurArchive(GdEngine* engine, const char* route) {
    if (InterlockedCompareExchange(&g_dbLoaded, 0, 0)) return true;
    if (InterlockedExchange(&g_dbTried, 1)) return false;
    if (!engine || !p_LoadDatabase || !g_arzPath[0]) {
        logE("reagent db: cannot load (engine=%p LoadDatabase=%p path=\"%s\")", (void*)engine,
             (void*)p_LoadDatabase, g_arzPath);
        g_dbRoute = "unavailable";
        return false;
    }
    MsvcString s;
    makeString(&s, g_arzPath);
    bool ok = false;
    // Through the TRAMPOLINE while Engine::LoadDatabase is detoured: the mod's own load must not
    // re-enter its own detour. The depth counter says the same thing to anything the engine
    // calls from inside this load.
    InterlockedIncrement(&g_ourLoadDepth);
    __try {
        ok = o_LoadDatabase ? o_LoadDatabase(engine, &s) : p_LoadDatabase(engine, &s);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logE("reagent db: EXCEPTION inside Engine::LoadDatabase");
        ok = false;
    }
    InterlockedDecrement(&g_ourLoadDepth);
    logD("reagent db: Engine::LoadDatabase(\"%s\") via %s -> %s", g_arzPath, route,
         ok ? "OK" : "FAILED");
    if (ok) {
        InterlockedExchange(&g_dbLoaded, 1);
        // The page records are in the database again, so a world that had fallen back to the
        // vanilla page may substitute once more.
        InterlockedExchange(&g_frameUnavail, 0);
        g_dbRoute = route;
        unsigned int sum = p_GetDbChecksum ? p_GetDbChecksum(engine) : 0;
        bool custom = p_HasCustomDb ? p_HasCustomDb(engine) : false;
        logD("reagent db: database checksum now 0x%08X, HasLoadedCustomDatabase=%d "
             "(both are taken BEFORE our load, so neither should have moved)",
             sum, custom ? 1 : 0);
    } else {
        InterlockedExchange(&g_dbTried, 0);  // allow the fallback route to try again
    }
    return ok;
}

// ---- "can the active database serve this page record" ------------------------------------------
// The one test behind both the substitution fallback and the reload after a foreign database
// load. LoadTableBinary::GetNumElementsForField is a virtual, so it is only called when the
// object's vptr really is the exported LoadTableBinary vftable; -1 = the count could not be read,
// which ut::pageTableServable answers with YES.
int tableBoxCount(const void* table) {
    if (!table || !p_GetNumElementsForField) return -1;
    int n = -1;
    __try {
        const void* vft = *(const void* const*)table;
        if (!p_LoadTableBinaryVft || vft == p_LoadTableBinaryVft) {
            n = (int)p_GetNumElementsForField(table, "reagentBoxes");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        n = -1;
    }
    return n;
}

bool tableServesPage(const void* table) {
    return ut::pageTableServable(table != nullptr, tableBoxCount(table));
}

// ---- the record substitution ------------------------------------------------------------------
void* t_LoadTableFileHook = nullptr;
PfnOM_LoadTableFile o_LoadTableFile = nullptr;
void* t_GetLoadTable = nullptr;
PfnOM_GetLoadTable o_GetLoadTable = nullptr;

// An MsvcString pointing at caller-owned storage: no shared heap, so the detours are reentrant
// and thread-safe (the engine loads records from more than one place).
void makeStringInto(MsvcString* s, char* storage) {
    memset(s, 0, sizeof(*s));
    const size_t n = strlen(storage);
    s->size = n;
    if (n < 16) {
        memcpy(s->u.buf, storage, n + 1);
        s->capacity = 15;
    } else {
        s->u.ptr = storage;
        s->capacity = n;
    }
}

// -1 = show the vanilla page. Otherwise a valid index into g_pages.
int wantedPage() {
    const int p = g_cfg.uniqPage;
    if (p < 0 || !g_pages || (size_t)p >= g_pages->size()) return -1;
    return p;
}

// True (and fills `out`/`storage`) when this record request is the Crafting Materials window
// AND a collection page is selected. The size test runs first, so every other record in the
// game costs one compare.
bool substitutePage(const void* in, MsvcString* out, char* storage, size_t cap,
                    bool isLoadTableFile) {
    if (!in) return false;
    // While live paging is armed the page is ALWAYS the frame record: it carries the 24 real
    // vanilla boxes first, so a freshly built HUD looks exactly like the untouched Crafting
    // Materials page, and ut_live.cpp re-points the boxes from there.
    size_t len = 0;
    __try {
        len = ((const MsvcString*)in)->size;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (len != kMaterialLen) return false;
    const char* text = msvcStringText(in);
    if (!text || _stricmp(text, kMaterialRecord) != 0) return false;
    const char* frame = liveFramePath();
    ut::UtPageFacts facts;
    facts.frameArmed = frame != nullptr;
    facts.wantedPage = wantedPage();
    facts.unavailable = InterlockedCompareExchange(&g_frameUnavail, 0, 0) != 0;
    facts.isMaterial = true;  // proved by the two tests above; every other record has left
    const ut::UtPageDecision decision = ut::pageAction(facts);
    if (decision.action != ut::kUtPageSubstitute) return false;
    const int page = decision.frame ? 0 : facts.wantedPage;
    _snprintf_s(storage, cap, _TRUNCATE, "%s",
                frame ? frame : (*g_pages)[page].record.c_str());
    makeStringInto(out, storage);
    const LONG n = InterlockedIncrement(&g_pageSubs);
    if (InterlockedIncrement(&g_pageSubLogged) <= 6) {
        logD("reagent page: substituted \"%s\" -> \"%s\" (%s) [%ld]", kMaterialRecord, storage,
             frame ? "the live frame record" : (*g_pages)[page].label.c_str(), n);
    }
    // The box loop runs inside ReagentWindow::Load, straight after this LoadTableFile call, so
    // this is the moment the box capture must open.
    if (frame && isLoadTableFile) liveBeginCapture();
    return true;
}

// The flag and the single info line behind it. Called from the LoadTableFile detour, i.e. from
// inside ReagentWindow::Load and BEFORE the engine builds the first box, which is the last moment
// at which the capture can still be cancelled.
void pageUnavailable() {
    liveCancelCapture();
    InterlockedExchange(&g_frameUnavail, 1);
    if (InterlockedExchange(&g_frameUnavailSaid, 1)) return;
    logI("reagent page: the collection tab is off in this world - its page records are not in the "
         "active database (a custom game loads its own). The vanilla Crafting Materials page is "
         "shown instead; the collection file is untouched and nothing can be deposited here.");
}

void pageOnWorldTeardown() {
    InterlockedExchange(&g_frameUnavail, 0);
    InterlockedExchange(&g_frameUnavailSaid, 0);
}

const void* __cdecl hk_LoadTableFile(void* self, const void* path) {
    MsvcString sub;
    char buf[MAX_PATH];
    if (substitutePage(path, &sub, buf, sizeof(buf), true)) {
        const void* table = o_LoadTableFile ? o_LoadTableFile(self, &sub) : nullptr;
        if (!ut::pageFallsBack(true, tableServesPage(table))) return table;
        // The active database cannot serve the page. Give the engine back the record it asked
        // for, so the world keeps the vanilla Crafting Materials window instead of an empty one.
        pageUnavailable();
        return o_LoadTableFile ? o_LoadTableFile(self, path) : nullptr;
    }
    return o_LoadTableFile ? o_LoadTableFile(self, path) : nullptr;
}

const void* __cdecl hk_GetLoadTable(const void* self, const void* path) {
    MsvcString sub;
    char buf[MAX_PATH];
    if (substitutePage(path, &sub, buf, sizeof(buf), false)) {
        return o_GetLoadTable ? o_GetLoadTable(self, &sub) : nullptr;
    }
    return o_GetLoadTable ? o_GetLoadTable(self, path) : nullptr;
}

// Reads records/ui/caravan/caravan_materialwindow.dbr back out of the LIVE database and counts
// its reagentBoxes entries: 40 = our override won, 24 = the vanilla page is still in place.
// LoadTableBinary::GetNumElementsForField is a virtual, so it is only called when the object's
// vptr really is the exported LoadTableBinary vftable.
void verifyOverride() {
    if (InterlockedExchange(&g_verifyDone, 1)) return;
    if (!p_ObjectManagerGet || !p_LoadTableFile || !p_GetNumElementsForField) {
        logD("reagent db: cannot verify the pages (verification exports missing)");
        return;
    }
    // Read the VANILLA record and (if one is selected) OUR page record straight out of the live
    // database. The vanilla one must still have its own 24 boxes - the substitution never
    // overrides it. p_LoadTableFile is the raw export, the substitution lives in the detour, and
    // this call goes through that detour, so ask for our page by its real name and the vanilla
    // one by its.
    struct Probe {
        const char* what;
        const char* record;
    };
    char pageRec[MAX_PATH] = {0};
    const int page = wantedPage();
    if (page >= 0) _snprintf_s(pageRec, _TRUNCATE, "%s", (*g_pages)[page].record.c_str());
    const Probe probes[2] = {{"vanilla materials", kMaterialRecord},
                             {"selected page", pageRec[0] ? pageRec : nullptr}};
    for (int i = 0; i < 2; ++i) {
        if (!probes[i].record) continue;
        MsvcString path;
        char storage[MAX_PATH];
        _snprintf_s(storage, _TRUNCATE, "%s", probes[i].record);
        makeStringInto(&path, storage);
        __try {
            void* om = p_ObjectManagerGet();
            // o_LoadTableFile when the detour is in, so the vanilla probe is never substituted.
            const void* table = nullptr;
            if (om) {
                table = o_LoadTableFile ? o_LoadTableFile(om, &path) : p_LoadTableFile(om, &path);
            }
            if (!table) {
                logD("reagent db: VERIFY %s (%s) - LoadTableFile returned null", probes[i].what,
                     probes[i].record);
                continue;
            }
            const void* vft = *(const void* const*)table;
            if (p_LoadTableBinaryVft && vft != p_LoadTableBinaryVft) {
                logD("reagent db: VERIFY %s - not a LoadTableBinary (vft=%p), skipping",
                     probes[i].what, vft);
                continue;
            }
            const unsigned int n = p_GetNumElementsForField(table, "reagentBoxes");
            if (i == 0) {
                g_verifyBoxes = (int)n;
                logD("reagent db: VERIFY vanilla %s has %u reagentBoxes (%s)", kMaterialRecord, n,
                     n == 24 ? "UNTOUCHED - the user's Crafting Materials page is intact"
                             : "UNEXPECTED - it should be 24");
            } else {
                g_verifyPageBoxes = (int)n;
                logD("reagent db: VERIFY page %d %s (%s) has %u reagentBoxes", page,
                     probes[i].record, g_pageLabel, n);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            logD("reagent db: VERIFY %s - EXCEPTION while reading the record back",
                 probes[i].what);
        }
    }
}

// The record the overlay archive is asked for when the question is "is our archive in the
// database the game is reading right now". The frame page while live paging is armed, the first
// collection page otherwise; nullptr when there are no pages at all.
const char* overlayProbeRecord() {
    const char* frame = liveFramePath();
    if (frame && frame[0]) return frame;
    if (g_pages && !g_pages->empty()) return (*g_pages)[0].record.c_str();
    return nullptr;
}

// Reads that record straight out of the LIVE database through the trampoline, so the
// substitution in the detour never sees it, and counts its boxes: a record the active database
// cannot serve still comes back as a table, only an EMPTY one. "Cannot tell" (no exports, no
// pages) answers YES, because the answer only ever triggers another load and a load nobody needs
// is the worse of the two mistakes.
bool overlayRecordReadable() {
    const char* rec = overlayProbeRecord();
    if (!rec || !p_ObjectManagerGet || !o_LoadTableFile) return true;
    bool ok = true;
    __try {
        MsvcString path;
        char storage[MAX_PATH];
        _snprintf_s(storage, _TRUNCATE, "%s", rec);
        makeStringInto(&path, storage);
        void* om = p_ObjectManagerGet();
        ok = om ? tableServesPage(o_LoadTableFile(om, &path)) : true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = true;
    }
    return ok;
}

// Engine::LoadDatabase - the call the game makes for every archive of a custom game
// (<game>\mods\<mod>\database\*.arz, plus survivalmode1..3 for the DLC parts of the Crucible)
// after LoadMainDatabase has run. The overlay goes in through this same loader, so a custom load
// that replaces the loaded archives takes it out again; the only reliable test is to ask the
// database for one of our records, and it is asked after every load that is not ours.
struct DbBusy {
    DbBusy() { InterlockedIncrement(&g_dbBusy); }
    ~DbBusy() { InterlockedDecrement(&g_dbBusy); }
};

// The archive path out of the caller's std::string, copied where a fault cannot reach the log
// call. Its own function because hk_LoadDatabase holds an object with a destructor.
void dbPathText(const void* str, char* out, size_t cap) {
    out[0] = 0;
    __try {
        const char* t = msvcStringText(str);
        _snprintf_s(out, cap, _TRUNCATE, "%s", t ? t : "?");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        _snprintf_s(out, cap, _TRUNCATE, "%s", "?");
    }
}

bool __cdecl hk_LoadDatabase(GdEngine* self, const void* str) {
    DbBusy busy;
    const bool ok = o_LoadDatabase ? o_LoadDatabase(self, str) : false;
    if (InterlockedCompareExchange(&g_ourLoadDepth, 0, 0)) return ok;  // our own load
    if (!g_cfg.dbLoad) return ok;
    // One line per foreign archive, with the verdict, so a log says when a custom game's
    // <game>\mods\<mod>\database\*.arz goes in relative to LoadMainDatabase and the HUD build,
    // and whether it took the overlay with it.
    const bool had = InterlockedCompareExchange(&g_dbLoaded, 0, 0) != 0;
    const bool readable = had ? overlayRecordReadable() : false;
    char path[MAX_PATH];
    dbPathText(str, path, sizeof(path));
    logD("reagent db: Engine::LoadDatabase(\"%s\") by the game -> %s; our pages %s", path,
         ok ? "OK" : "FAILED",
         !had ? "are not in the database yet"
              : (readable ? "still read out of the database" : "no longer read out of it"));
    if (!had || readable) return ok;
    dbForgetLoad("a custom database load replaced the loaded archives");
    loadOurArchive(self, "LoadDatabase detour (custom database)");
    return ok;
}

void __cdecl hk_LoadMainDatabase(GdEngine* self) {
    DbBusy busy;
    const LONG n = InterlockedIncrement(&g_dbMainLoads);
    // The question is "is the overlay in the database this call is about to replace", never "is
    // this the detour's second call": under a late loader the process's first LoadMainDatabase
    // runs before the mod exists and the overlay goes in through the game-thread fallback, which
    // makes a custom game's load the detour's call #1. Cleared BEFORE the original, so exactly
    // one reload happens per LoadMainDatabase and never two.
    const bool again = InterlockedCompareExchange(&g_dbLoaded, 0, 0) != 0;
    if (again) {
        dbForgetLoad("Engine::LoadMainDatabase is loading a database over the overlay");
    }
    if (o_LoadMainDatabase) o_LoadMainDatabase(self);
    logD("reagent db: Engine::LoadMainDatabase returned (engine=%p, load #%ld)", (void*)self, n);
    // Probe (b) is the VANILLA value: LoadMainDatabase has written Engine+0xBA8 at its +0x1F7 and
    // our Engine::LoadDatabase has NOT run yet, so whatever this prints is the game's own checksum
    // on this installation.
    dbChecksumProbe("b: LoadMainDatabase returned, BEFORE our load");
    loadOurArchive(self, again ? "LoadMainDatabase detour (reload)" : "LoadMainDatabase detour");
    dbChecksumProbe("c: after our Engine::LoadDatabase");
}

// ---- init helpers ------------------------------------------------------------------------------
void* proc(HMODULE m, const char* name, const char* pretty) {
    void* p = m ? (void*)GetProcAddress(m, name) : nullptr;
    if (p) ++g_gd.resolvedByName;   // counted in the bindings gate's "by export"
    logT("  export %-38s %s", pretty, p ? "ok" : "MISSING");
    return p;
}

// `FF 97 <disp32>` = call qword ptr [rdi + disp32]: a virtual call through a vtable slot.
unsigned int decodeVcallSlot(const void* fn, unsigned int at) {
    if (!fn) return 0;
    const unsigned char* p = (const unsigned char*)fn;
    unsigned int off = 0;
    __try {
        if (p[at] == 0xFF && p[at + 1] == 0x97) memcpy(&off, p + at + 2, 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return off > 0x1000 ? 0 : off;
}

// `0F B6 81 <disp32> C3` = movzx eax, byte ptr [rcx + disp32] ; ret
unsigned int decodeFlagOffset(const void* fn) {
    if (!fn) return 0;
    const unsigned char* p = (const unsigned char*)fn;
    unsigned int off = 0;
    __try {
        if (p[0] == 0x0F && p[1] == 0xB6 && p[2] == 0x81 && p[7] == 0xC3) {
            memcpy(&off, p + 3, 4);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return off;
}

// `CursorHandler::GetPlayerCtrl`'s own bytes carry Player+0x16C0, the ControllerPlayer's object
// id: `48 8B 41 18` (mov rax,[rcx+0x18] - the CursorHandler's Player) and then the first
// `mov r32,[rax+disp32]`. ut_selftest.cpp makes the same decode over the same export; the two are
// duplicated rather than shared because the files hold no common header for it and neither may
// include the other's statics.
unsigned int decodeCtrlIdOffsetBytes(const void* fn) {
    if (!fn) return 0;
    const unsigned char* p = (const unsigned char*)fn;
    unsigned int off = 0;
    __try {
        int at = -1;
        for (int i = 0; i + 4 <= 32; ++i) {
            if (p[i] == 0x48 && p[i + 1] == 0x8B && p[i + 2] == 0x41 && p[i + 3] == 0x18) {
                at = i + 4;
                break;
            }
        }
        if (at < 0) return 0;
        for (int j = at; j + 6 <= 40; ++j) {
            // mov r32,[rax+disp32] - any destination register (modrm 0x80 | reg<<3, rm = rax)
            if (p[j] == 0x8B && (p[j + 1] & 0xC7) == 0x80) {
                memcpy(&off, p + j + 2, 4);
                break;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return off > 0x20000 ? 0 : off;
}

// `8B 81 <disp32> C3` = mov eax, dword ptr [rcx + disp32] ; ret  (the u32/enum getters)
unsigned int decodeDwordOffset(const void* fn) {
    if (!fn) return 0;
    const unsigned char* p = (const unsigned char*)fn;
    unsigned int off = 0;
    __try {
        if (p[0] == 0x8B && p[1] == 0x81 && p[6] == 0xC3) memcpy(&off, p + 2, 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return off;
}

// Item::GetItemReplicaInfo is
//   48 8B C2          mov rax, rdx
//   48 8D 91 <disp32> lea rdx, [rcx + disp32]     <- ItemReplicaInfo lives INSIDE the Item
//   48 8B C8          mov rcx, rax
//   E9 <rel32>        jmp <ItemReplicaInfo::operator=>
unsigned int decodeReplicaOffset(const void* fn) {
    if (!fn) return 0;
    const unsigned char* p = (const unsigned char*)fn;
    unsigned int off = 0;
    __try {
        if (p[0] == 0x48 && p[1] == 0x8B && p[2] == 0xC2 && p[3] == 0x48 && p[4] == 0x8D &&
            p[5] == 0x91) {
            memcpy(&off, p + 6, 4);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return off;
}

// sizeof(ItemReplicaInfo) from Item::Item and from ItemReplicaInfo::operator= (reached through
// Item::GetItemReplicaInfo's own tail jmp, which must land inside Game.dll). Both are read under
// SEH; the rule in ut_replicasize.h decides, and a miss is 0 - never 0x190.
void decodeReplicaSize(const void* ctor, const void* getReplica, const void* gameLo,
                       size_t gameLen) {
    g_replicaSizeCtor = 0;
    g_replicaSizeAssign = 0;
    __try {
        if (ctor && g_replicaOffset) {
            g_replicaSizeCtor =
                utReplicaSizeFromItemCtor((const unsigned char*)ctor, 0x120, g_replicaOffset);
        }
        int rel = 0;
        const size_t after = utReplicaAssignJump((const unsigned char*)getReplica, 0x12, &rel);
        if (after && gameLo && gameLen > 0x400) {
            const unsigned char* target = (const unsigned char*)getReplica + after + rel;
            const unsigned char* lo = (const unsigned char*)gameLo;
            if (target > lo && target + 0x400 < lo + gameLen) {
                g_replicaSizeAssign = utReplicaSizeFromAssign(target, 0x400);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_replicaSizeCtor = 0;
        g_replicaSizeAssign = 0;
    }
    g_replicaSize = utReplicaSizeConfirmed(g_replicaSizeCtor, g_replicaSizeAssign)
                        ? g_replicaSizeCtor
                        : 0;
}

bool loadRecordList(const char* path) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::string text;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got) text.append(buf, got);
    CloseHandle(h);
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        while (!line.empty() && (line[line.size() - 1] == '\r' || line[line.size() - 1] == ' ')) {
            line.erase(line.size() - 1);
        }
        if (!line.empty()) {
            toLower(&line);
            g_pageRecords->insert(line);
        }
        if (end == text.size()) break;
        start = end + 1;
    }
    return !g_pageRecords->empty();
}

// uniq-pages.txt : "<index>\t<record>\t<label>\t<boxes>" per page.
bool loadPageList(const char* path) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::string text;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got) text.append(buf, got);
    CloseHandle(h);
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        while (!line.empty() && (line[line.size() - 1] == '\r' || line[line.size() - 1] == ' ')) {
            line.erase(line.size() - 1);
        }
        if (!line.empty()) {
            std::string col[4];
            int c = 0;
            for (size_t i = 0; i < line.size() && c < 4; ++i) {
                if (line[i] == '\t') { ++c; continue; }
                col[c] += line[i];
            }
            if (!col[1].empty()) {
                UniqPage pg;
                pg.record = col[1];
                toLower(&pg.record);
                pg.label = col[2];
                pg.boxes = atoi(col[3].c_str());
                g_pages->push_back(pg);
            }
        }
        if (end == text.size()) break;
        start = end + 1;
    }
    return !g_pages->empty();
}

bool g_inited = false;

// =============================================================================================
// THE RESCUE KIT
//
// `ReadPlayerReagents` drops every reagent-map entry whose record has `craftingMaterial == 0`,
// and `LoadPlayerReagents` can trigger a `SaveReagents` on its own through
// `DepositTransferReagents`. Mod removed / gate broken / archive missing => the stored uniques
// are PRUNED at the next character load, and the pruned reagents.gst goes to Steam Cloud. This
// block is the way back out: a journal of everything deposited and a command that hands every
// stored item back through the engine's own take path.
//
// ---- the take-path mirror, and why it is the engine's own code ------------------------------
// `Grim Dawn.exe` 0x132B10..0x132C1D (out/gd-exe-image.bin, tools/xref_exe.py) IS the reagent
// take. Read literally, one call at a time:
//
//     player = GameEngine::GetMainPlayer(gGameEngine)                       IAT 0x2D7190
//     if (!player->vt[0x938](proto)) { Player::PlayInventoryFullSound(player); return; }
//     ItemReplicaInfo replica;                                              ctor exe 0x1D400
//     proto->vt[0x590](&replica)                                            GetItemReplicaInfo
//     want = min(want, GameEngine::GetItemMaxStackSize(gGameEngine))        IAT 0x2DB4D8
//     replica.objectId = 0
//     newItem = Item::CreateItem(&replica)                                  IAT 0x2D7B50
//     if (newItem) {
//         ctrl = <om id->object>(Singleton<ObjectManager>::Get(), Character::GetControllerId(player))
//         if (ctrl) ControllerCharacter::SendAddItemToInventory(ctrl, GetObjectId(newItem))
//         player->vt[0x578](newItem, false, false)                          GiveItemToCharacter
//         newItem->vt[0x410]()                                              PlayDropSound
//         proto->vt[0x610](max(0, proto->vt[0x618]() - want))               Set/GetStackSize
//     }
//
// Every one of those steps is an EXPORT, and the last line is character for character the body
// of the exported `GameEngine::TakeItemFromReagents(std::string const&, int)` (Game.dll
// 0x2CF220, disassembled: lower_bound the map, resolve node[0x40], RTTI-check it,
// `stack = proto->vt[0x618]()`, clamp, `proto->vt[0x610](stack - count)`, return count). That
// overload is therefore the DECREMENT HALF ONLY - it creates nothing and it never removes the
// node - so the mirror is "exported creation + placement, then the exported take".
//
// The four Item vtable slots the exe uses (0x410 PlayDropSound, 0x590 GetItemReplicaInfo,
// 0x610 SetStackSize, 0x618 GetStackSize) are NOT overridden by any of the 19
// `??_7Item*@GAME@@6BObject@1@@` vftables Game.dll exports, and Player is the concrete class
// GetMainPlayer returns, so calling the exports directly is identical to the exe's virtual
// dispatch. `verifyTakeMirror` proves that on the live binary before the command may run: it
// reads the two exported vftables and requires every slot to equal the export we resolved.
//
// TRAP (found before the first run, see restoreOne): `Item::GetItemReplicaInfo` is an
// ASSIGNMENT into an already-constructed ItemReplicaInfo - both the exe and AddItemToReagents run
// the (unexported) constructor on their stack local first. The mirror therefore copies the
// 0x190-byte blob out of `Item + g_replicaOffset` by hand and zeroes the object id, which is what
// the pristine test and the journal capture already do; `p_GetReplicaCall` is kept only for the
// vtable cross-check.
//
// The `AddItemToTransfer` fallback in the task brief is NOT used: the mirror is complete with
// exports, and guessing the argument order of a function that MOVES items would be a worse risk
// than stopping. When the inventory is full the run stops and says so, exactly like the engine.
// =============================================================================================
typedef bool(__cdecl* PfnPlayer_IsInvSpace)(const void* player, const GdItem* item);
typedef void(__cdecl* PfnPlayer_Void)(void* player);
typedef unsigned int(__cdecl* PfnGE_GetU32)(const GdGameEngine*);
typedef unsigned int(__cdecl* PfnChar_GetU32)(const void* character);
typedef void(__cdecl* PfnCC_SendAddItem)(void* controller, unsigned int itemId);
typedef void(__cdecl* PfnPlayer_GiveItem)(void* player, GdItem* item, bool a, bool b);
typedef void(__cdecl* PfnItem_Void)(GdItem*);

PfnItem_GetItemReplicaInfo p_GetReplicaCall = nullptr;
PfnPlayer_IsInvSpace p_IsInventorySpaceAvailable = nullptr;
PfnPlayer_Void p_PlayInventoryFullSound = nullptr;
PfnGE_GetU32 p_GetItemMaxStackSize = nullptr;
PfnChar_GetU32 p_GetControllerId = nullptr;
PfnCC_SendAddItem p_SendAddItemToInventory = nullptr;
PfnPlayer_GiveItem p_GiveItemToCharacter = nullptr;
PfnItem_Void p_PlayDropSound = nullptr;
const void* p_ItemVftObject = nullptr;    // ??_7Item@GAME@@6BObject@1@@   - read only
const void* p_PlayerVftObject = nullptr;  // ??_7Player@GAME@@6BObject@1@@ - read only
void* p_AddItemToTransfer = nullptr;      // resolved and logged; DELIBERATELY never called

// The two observer detours. Both targets are the p_ pointers above, so no new export name is
// introduced; MinHook fills these trampolines in reagentInstall.
PfnPlayer_IsInvSpace o_IsInvSpace = nullptr;
PfnGE_GetU32 o_GetItemMaxStack = nullptr;
// The real arm site (see the block by hk_ItemGetItemReplicaInfo).
typedef void(__cdecl* PfnItem_GetReplica)(const GdItem*, void*);
PfnItem_GetReplica o_ItemGetReplica = nullptr;

bool g_takeMirrorOk = false;
char g_takeMirrorWhy[256] = "not checked yet";

volatile LONG g_rescueRuns = 0;
volatile LONG g_rescueRestored = 0;
volatile LONG g_rescueFailed = 0;
volatile LONG g_journalAdds = 0;
volatile LONG g_journalDrops = 0;
volatile LONG g_journalSynth = 0;

// EVERY journal removal goes through here, so journalDrops is the truth. Counting anywhere else
// misses the removals a restore performs.
bool journalDrop(const char* record) {
    const bool r = journalRemove(record);
    if (r) InterlockedIncrement(&g_journalDrops);
    return r;
}
volatile LONG g_depositWarned = 0;  // the first-accepted-deposit warning fired
int g_rescueSeen = 0;               // the ini's rescue= as we last saw it (edge detection)

// The one SEH-guarded call both the rescue tick and restoreOne need. Its own function, because
// MSVC allows only ONE form of exception handling per function and both callers use try/catch.
GdPlayer* safeMainPlayer() {
    if (!g_gameEngine || !g_gd.GameGetMainPlayer) return nullptr;
    __try {
        return g_gd.GameGetMainPlayer(g_gameEngine);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool rescueEmptied(const std::string& key) {
    if (!g_rescueEmptied) return false;
    try {
        return g_rescueEmptied->find(key) != g_rescueEmptied->end();
    } catch (...) {
        return false;
    }
}

// One vtable slot must equal the export we resolved. SEH only; no C++ objects in this frame.
bool vslotIs(const void* vft, unsigned int off, const void* want) {
    if (!vft || !want) return false;
    const void* got = nullptr;
    __try {
        got = *(const void* const*)((const unsigned char*)vft + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return got == want;
}

void verifyTakeMirror() {
    g_takeMirrorOk = false;
    struct Need {
        const void* p;
        const char* name;
    };
    const Need needs[] = {
        {(const void*)g_gd.GameGetMainPlayer, "GameEngine::GetMainPlayer"},
        {(const void*)p_IsInventorySpaceAvailable, "Player::IsInventorySpaceAvailable"},
        {(const void*)p_GetReplicaCall, "Item::GetItemReplicaInfo"},
        {(const void*)p_GetItemMaxStackSize, "GameEngine::GetItemMaxStackSize"},
        {(const void*)p_GetControllerId, "Character::GetControllerId"},
        {(const void*)p_SendAddItemToInventory, "ControllerCharacter::SendAddItemToInventory"},
        {(const void*)p_GiveItemToCharacter, "Player::GiveItemToCharacter"},
        {(const void*)p_PlayDropSound, "Item::PlayDropSound"},
        {(const void*)p_GetStackSize, "Item::GetStackSize"},
        {(const void*)p_SetStackSize, "Item::SetStackSize"},
        {(const void*)p_ObjectManagerGet, "Singleton<ObjectManager>::Get"},
        {(const void*)p_ObjectFromId, "ObjectManager id->Object"},
        {(const void*)t_ItemCreateItem, "Item::CreateItem"},
        {(const void*)t_TakeFromReagents, "GameEngine::TakeItemFromReagents(string,int)"},
        {(const void*)g_gd.ObjectGetObjectId, "Object::GetObjectId"},
    };
    for (size_t i = 0; i < sizeof(needs) / sizeof(needs[0]); ++i) {
        if (!needs[i].p) {
            _snprintf_s(g_takeMirrorWhy, sizeof(g_takeMirrorWhy), _TRUNCATE,
                        "%s did not resolve", needs[i].name);
            return;
        }
    }
    struct SlotCheck {
        const void* vft;
        unsigned int off;
        const void* want;
        const char* name;
    };
    const SlotCheck slots[] = {
        {p_ItemVftObject, 0x410, (const void*)p_PlayDropSound, "Item+0x410 PlayDropSound"},
        {p_ItemVftObject, 0x590, (const void*)p_GetReplicaCall, "Item+0x590 GetItemReplicaInfo"},
        {p_ItemVftObject, 0x610, (const void*)p_SetStackSize, "Item+0x610 SetStackSize"},
        {p_ItemVftObject, 0x618, (const void*)p_GetStackSize, "Item+0x618 GetStackSize"},
        {p_PlayerVftObject, 0x578, (const void*)p_GiveItemToCharacter,
         "Player+0x578 GiveItemToCharacter"},
        {p_PlayerVftObject, 0x938, (const void*)p_IsInventorySpaceAvailable,
         "Player+0x938 IsInventorySpaceAvailable"},
    };
    for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); ++i) {
        if (!vslotIs(slots[i].vft, slots[i].off, slots[i].want)) {
            _snprintf_s(g_takeMirrorWhy, sizeof(g_takeMirrorWhy), _TRUNCATE,
                        "vtable slot %s does not hold the exported function - the exe's take "
                        "sequence cannot be mirrored on this build",
                        slots[i].name);
            return;
        }
    }
    g_takeMirrorOk = true;
    _snprintf_s(g_takeMirrorWhy, sizeof(g_takeMirrorWhy), _TRUNCATE,
                "15 exports resolved, 6 vtable slots cross-checked against them");
}

// ---- the box-badge repaint --------------------------------------------------------------------
// GameEngine::SyncCaravanReagents (Game.dll 0x2C3460) is three instructions:
//   mov rcx,[rcx+0x19B0]   ; GameUIInterface*
//   mov rax,[rcx]          ; its vtable
//   jmp [rax+0x88]         ; ReagentWindow::Sync, exe 0x1324A0..0x13270D
// so it dereferences GameEngine+0x19B0 unconditionally: it must never be called before the UI
// exists. Both callers below are on the game thread with the caravan window open and the live
// box vector already captured, and the call is SEH-guarded and self-disabling on a fault.
volatile LONG g_syncCalls = 0;
volatile LONG g_syncFaults = 0;
volatile LONG g_syncOff = 0;

void reagentSyncCaravan(const char* why) {
    if (!p_SyncCaravanReagents || !g_gameEngine) return;
    if (InterlockedCompareExchange(&g_syncOff, 0, 0)) return;
    if (!InterlockedCompareExchange(&g_transferOpenNow, 0, 0)) return;
    __try {
        p_SyncCaravanReagents(g_gameEngine);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_syncOff, 1);
        InterlockedIncrement(&g_syncFaults);
        logW("reagent sync: GameEngine::SyncCaravanReagents FAULTED (%s) - the badge repaint is "
             "disabled for this session; nothing else changes",
             why ? why : "?");
        return;
    }
    const LONG n = InterlockedIncrement(&g_syncCalls);
    if (n <= 8 || (n % 50) == 0) {
        logT("reagent sync: SyncCaravanReagents #%ld (%s)", n, why ? why : "?");
    }
}

volatile LONG g_uiTakes = 0;
DWORD g_takeWatchAt = 0;

// ---- THE TAKE WATCH, table branch -------------------------------------------------------------
// The exe's take is box-object driven and, under the private table, the box's item is OUR
// prototype: 0x132BFA reads its stack, 0x132C00 subtracts the taken count and 0x132C16
// `[rsi vt+0x610] Item::SetStackSize` writes it back (the cursor pick-up does the same at
// 0x132D91..0x132D9E). The mod does NOT decrement the prototype - it OBSERVES, exactly as it
// observes the map's prototypes today.
//
// Two rules make this safe against the OTHER decrement - identityAfterCreate's, which runs
// earlier in the same take:
//   1. it only acts on a DROP in the observed stack of the SAME prototype object, so a count
//      raised by a deposit whose prototype has not been re-stacked yet can never be walked back;
//   2. it SETS the count to the stack it observed (never subtracts blindly), so a take whose
//      identity restore already decremented finds count == stack and does nothing.
struct TableWatchRow {
    unsigned int protoId;
    int stack;
};
std::map<std::string, TableWatchRow>* g_tableStackPrev = nullptr;
const int kTableWatchCap = 256;

void tableTakeWatchClear() {
    try {
        if (g_tableStackPrev) g_tableStackPrev->clear();
    } catch (...) {
    }
}

void tableTakeWatch(bool* anyTaken) {
    // The gate is WHAT THE TABLE HAS BUILT. A prototype the table built in this world is a box
    // the player can take from for as long as the world lives, and that take has to be recorded -
    // otherwise the item ends up in the bag with the row still claiming it, i.e. a duplicate.
    // `storeCollectBuilt` is a pure read that NEVER builds and returns 0 rows when the table has
    // built nothing, so a world that stored nothing costs exactly one call per tick.
    //
    // Game thread only (takeWatchTick is called from the game-thread tick), so plain statics.
    static char recs[kTableWatchCap][256];
    static unsigned int ids[kTableWatchCap];
    static unsigned int counts[kTableWatchCap];
    const int n = storeCollectBuilt(recs, ids, counts, kTableWatchCap);
    if (n <= 0) {
        tableTakeWatchClear();
        return;
    }
    if (!g_tableStackPrev) g_tableStackPrev = new std::map<std::string, TableWatchRow>();
    for (int i = 0; i < n && i < kTableWatchCap; ++i) {
        const unsigned int id = ids[i];
        if (!id || !recs[i][0]) continue;
        GdItem* proto = findItemById(id);
        if (!proto || !itemIsLive(proto, id)) continue;
        const int stack = safeStackOf(proto);
        if (stack < 0 || stack > 0x10000) continue;  // unreadable / absurd: never act on it
        std::string key(recs[i]);
        toLower(&key);
        std::map<std::string, TableWatchRow>::iterator it = g_tableStackPrev->find(key);
        if (it == g_tableStackPrev->end() || it->second.protoId != id) {
            TableWatchRow row;
            row.protoId = id;
            row.stack = stack;
            (*g_tableStackPrev)[key] = row;  // first sighting of THIS object: nothing to compare
            continue;
        }
        const int prev = it->second.stack;
        it->second.stack = stack;
        if (stack >= prev) continue;
        const unsigned int cnt = storeCount(key.c_str());
        unsigned int left = cnt;
        if ((int)cnt > stack) left = storeOnTake(key.c_str(), cnt - (unsigned int)stack);
        *anyTaken = true;
        logD("reagent take: %s went from %d to %d on the PRIVATE TABLE's own prototype (id=%u, "
             "the exe's inline take at 0x132C16 wrote the stack) - the table now holds %u; the "
             "journal entry is KEPT as history%s",
             key.c_str(), prev, stack, id, left,
             left ? "" : " and the row is marked not stored");
    }
}

void takeWatchTick() {
    if (!g_cfg.takeWatch || !g_pageRecords) return;
    // The TABLE's baseline is deliberately NOT cleared when the caravan closes: our prototypes
    // live as long as the world, so a stack the engine decremented at 0x132C16 in the last
    // 400 ms is still worth comparing against when the window comes back. Clearing would
    // re-baseline to the post-take stack and the count would stay one too high for ever. The
    // world teardown is the only thing that may clear it (object ids die with the world).
    if (!InterlockedCompareExchange(&g_transferOpenNow, 0, 0)) return;
    const DWORD now = GetTickCount();
    const int period = g_cfg.takeWatchMs > 50 ? g_cfg.takeWatchMs : 50;
    if (g_takeWatchAt && (now - g_takeWatchAt) < (DWORD)period) return;
    g_takeWatchAt = now;
    try {
        // The two refresh calls are what keep the page in step: a take must refresh it exactly
        // like an accepted deposit does, within one take_watch_ms tick, or the row the player
        // just emptied stays on screen until the next scroll.
        bool tableTaken = false;
        tableTakeWatch(&tableTaken);
        if (tableTaken) {
            plateOwnedRefresh(true);
            liveRelayoutVisible();
        }
    } catch (...) {
    }
}

// Drops everything that keys on an object, a pointer or a count from the world that is ending.
// Called from hk_ExitPlayingMode BEFORE the engine destroys the HUD, and from the observed
// teardown signal. Performs NO engine write.
void identityClear();  // defined with the identity block, below

void teardownTables(const char* why) {
    size_t counts = 0, emptied = 0, held = 0;
    try {
        Guard g;
        if (g_counts) {
            counts = g_counts->size();
            g_counts->clear();
        }
    } catch (...) {
    }
    try {
        if (g_rescueEmptied) {
            emptied = g_rescueEmptied->size();
            g_rescueEmptied->clear();
        }
    } catch (...) {
    }
    // The table watch's baseline is object ids, so it dies with the world. ut_store.cpp's own
    // teardown clears the prototypes themselves.
    tableTakeWatchClear();
    // Object ids belong to the world that created them, so the map-owned set dies with it. The
    // next load re-fills it from hk_ItemLoad inside ReadPlayerReagents.
    size_t owned = 0;
    try {
        Guard g;
        if (g_mapProtoIds) {
            owned = g_mapProtoIds->size();
            g_mapProtoIds->clear();
            InterlockedExchange(&g_mapOwnedSize, 0);
        }
    } catch (...) {
    }
    g_takeWatchAt = 0;
    identityClear();
    // The registration LOG budget is per world.
    InterlockedExchange(&g_regLogUsed, 0);
    InterlockedExchange(&g_worldActive, 0);
    logD("reagent: world tables cleared (%zu tallies, %zu emptied marks, %zu take-watch "
         "baselines, %zu map-owned prototype ids) - %s",
         counts, emptied, held, owned, why ? why : "?");
}

// ---- the journal ---------------------------------------------------------------------------
// The 0x190-byte ItemReplicaInfo plus every MSVC std::string slot inside it. Allocation-free and
// SEH-guarded: it runs inside the engine's own AddItemToReagents frame.
// Probe AND deep-copy one candidate slot under ONE per-candidate __try, so the pointer
// `looksLikeString` reads out of the blob is never dereferenced outside SEH. This is the
// SEH-guarded probe utScanReplicaSlots drives; `ctx` is the raw replica blob inside the live
// Item. It reports the window's SHAPE as well as its text, so the capture can warn about a slot
// it accepted without ever dereferencing the pointer.
void slotProbeEngineSeh(void* ctx, unsigned int off, bool allowEmptyHeap,
                        UtSlotProbeResult* out) {
    const unsigned char* blob = (const unsigned char*)ctx;
    memset(out, 0, sizeof(*out));
    __try {
        const unsigned char* p = blob + off;
        memcpy(&out->ptr, p, sizeof(out->ptr));
        memcpy(&out->cap, p + 0x18, sizeof(out->cap));
        size_t size = 0;
        const char* text = nullptr;
        if (!looksLikeStringEx(blob, off, &size, &text, allowEmptyHeap)) return;
        // "Unverified" means exactly one thing: looksLikeStringEx took the early exit at
        // `size == 0 && allowEmptyHeap` and never dereferenced the pointer.
        out->emptyHeap = allowEmptyHeap && size == 0 && out->cap > 15;
        const size_t room = sizeof(out->text) - 5;
        const int n = (int)(size > room ? room : size);
        _snprintf_s(out->text, sizeof(out->text), _TRUNCATE, "%.*s", n, size ? text : "");
        out->ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out->ok = false;
        out->faulted = true;
        out->text[0] = 0;
    }
}

// Every fault raised by the probe above is DELIBERATE and handled there, so the vectored handler
// must not report it. ProbeScope cannot live in a function that contains a __try (C2712), hence
// the split into two functions.
void slotProbeEngine(void* ctx, unsigned int off, bool allowEmptyHeap, UtSlotProbeResult* out) {
    ProbeScope scope;
    slotProbeEngineSeh(ctx, off, allowEmptyHeap, out);
}


// THE SEH MUST NOT SPAN THE BLOB COPY AND THE STRING SCAN.
// Wrapping both in ONE __try loses every capture: `looksLikeString` dereferences a pointer it
// reads out of the blob, so a single 16-byte pair that passes the size/capacity test while
// holding garbage raises an access violation, the __except unwinds out of the *entire* function,
// and the caller is told "the ItemReplicaInfo could not be copied" even though the memcpy that
// matters had already succeeded - leaving the journal permanently empty.
// So: the blob copy has its own __try (a fault there really is fatal - the Item is gone), and
// the string scan is per candidate and merely counts faults. A capture with zero readable string
// slots is still a valid backup: the 0x190 blob IS the identity; the slot table only re-points
// the heap strings on restore.
bool captureReplicaRaw(const GdItem* item, UtReplicaCapture* cap) {
    if (!item || !g_replicaOffset || !g_replicaSize) return false;
    if (g_replicaSize > sizeof(cap->replica)) return false;
    const unsigned char* blob = (const unsigned char*)item + g_replicaOffset;
    __try {
        memcpy(cap->replica, blob, g_replicaSize);
        cap->replicaLen = g_replicaSize;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        cap->replicaLen = 0;
        return false;
    }
    // The stored copy is a template, never a live object. ItemReplicaInfo + 0x00 is the u32 object
    // id (proven by Item::CreateItem); AddItemToReagents zeroes it in its own copy, so zero it in
    // ours - a stale id must never be handed to Item::CreateItem on a restore.
    if (g_replicaSize >= 4) memset(cap->replica, 0, 4);
    // The scan itself lives in ut_rescue.cpp as pure logic (utScanReplicaSlots), so
    // tools/test_journal.cpp exercises the SHIPPED rule with no game running. All that is left
    // here is the SEH-guarded probe over engine memory.
    UtSlotShape shapes[24];
    memset(shapes, 0, sizeof(shapes));
    int faults = 0;
    cap->slotCount = utScanReplicaSlots((void*)blob, g_replicaSize, &slotProbeEngine,
                                        cap->slotOff, cap->slotText, shapes, 24, &faults);
    cap->slotFaults = faults;
    // A shape guess is worth a line of its own, not just a data field: an accepted-but-unverified
    // slot is rare and is the shape a hang in Item::CreateItem traces back to.
    for (int i = 0; i < cap->slotCount; ++i) {
        if (!shapes[i].emptyHeap) continue;
        logD("capture: slot +0x%03X is an EMPTY HEAP string (ptr=%p cap=%zu) - shape-guess, "
             "unverified (%s)",
             cap->slotOff[i], (void*)(size_t)shapes[i].ptr, (size_t)shapes[i].cap, cap->record);
    }
    return true;
}

// The engine's real holding for one record: the stored PROTOTYPE's stack size. That is the one
// number the engine itself acts on - `GetReagentItemCount` returns it, the exe take bails when it
// is 0 and `TakeItemFromReagents` clamps against it - while `ReagentData::count` (node+0x44) is
// written only by `AddItemToReagents` and goes STALE the moment an item is withdrawn, because
// neither take path ever touches it. -1 = the node exists but the prototype could not be read.
int heldOf(const void* node) {
    // -1, not 0. `reagentStoreCensus` classifies 0 as "an empty row a probe may borrow", and its
    // contract is that an unreadable row can never be mistaken for an empty one; a null node is
    // the definition of unreadable. No caller depends on a 0 here - `reagentWalkHeld` maps h < 0
    // to 1 ("an unreadable prototype reports 1, never 0") and the census counts it as
    // `unreadable`. `reagentCollectNodes` never stores a nil node, so the guard is unreachable
    // today; it is written this way so the contract holds even if that changes.
    if (!node) return -1;
    ProtoInfo pi;
    memset(&pi, 0, sizeof(pi));
    pi.node = node;
    readProtoFields(node, &pi);
    if (!pi.protoId) return -1;
    pi.proto = findItemById(pi.protoId);
    if (!pi.proto) return -1;
    pi.live = itemIsLive(pi.proto, pi.protoId);
    if (!pi.live) return -1;
    readProtoStack(&pi);
    return (int)pi.stack;
}

// Called from hk_AddItemToReagents once the engine has accepted the deposit.
void journalOnDeposit(const UtReplicaCapture& cap, const char* record) {
    if (!g_cfg.journal) return;
    if (journalUpsert(cap)) {
        InterlockedIncrement(&g_journalAdds);
        // Print the captured slot OFFSETS, not only how many there were: "was the +0xD8 slot
        // actually captured?" is the question a journal diagnosis starts from, and a bare count
        // cannot answer it.
        char offs[240];
        offs[0] = 0;
        for (int i = 0; i < cap.slotCount && i < 24; ++i) {
            const size_t at = strlen(offs);
            if (at + 10 >= sizeof(offs)) break;
            _snprintf_s(offs + at, sizeof(offs) - at, _TRUNCATE, "%s+0x%X", i ? " " : "",
                        cap.slotOff[i]);
        }
        logD("rescue journal: BACKED UP %s (replica %u bytes, %d string slot%s at [%s], %d "
             "faulted candidate%s, count %u, flags 0x%X) -> %s",
             record, cap.replicaLen, cap.slotCount, cap.slotCount == 1 ? "" : "s", offs,
             cap.slotFaults, cap.slotFaults == 1 ? "" : "s", cap.stack, cap.flags, journalPath());
    } else {
        logE("rescue journal: could not record %s - THE BACKUP IS INCOMPLETE", record);
    }
    if (!InterlockedExchange(&g_depositWarned, 1)) {
        logI("*** ITEMS STORED ON THIS PAGE ARE LOST IF THE MOD IS REMOVED ***");
        logI("*** before uninstalling, set rescue=1 with the caravan open and let the mod hand "
             "everything back ***");
        logI("*** back up %s ***", journalPath());
        logD("the engine drops every reagent entry whose record has craftingMaterial == 0 at the "
             "next character load (ReadPlayerReagents +0x441), and the pruned reagents.gst goes "
             "to Steam Cloud");
    }
}

// ---- the take-side restore --------------------------------------------------------------------
//
// THE ONE THING THIS MUST NOT DO: `Grim Dawn.exe` calls `Item::CreateItem` from TEN sites
// (0x50C45, 0x50DAB, 0xF2B9C, 0xF3450, 0x132B89, 0x132CE4, 0x166066, 0x1AB5B4, 0x1ABA62,
// 0x1DCC65) and only the two inside 0x1328D0..0x1331C3 are the reagent take. The rest are
// crafting, enchanting and market paths, and substituting a stored replica into one of those
// would silently FORGE an item. So the classifier is STATE, and the call-site RVA is at most an
// extra positive check (identity_require_callsite, off by default).
//
// The state the exe's own take produces, in order (out/gd-exe-image.bin, 0x132B10..0x132C1D):
//
//     0x132B1A  GameEngine::GetMainPlayer                       (export, far too common to use)
//     0x132B2C  player->vt[0x938](proto)  == Player::IsInventorySpaceAvailable   <- ARM here
//     0x132B49  ItemReplicaInfo ctor on a stack local
//     0x132B59  proto->vt[0x590](&replica) == Item::GetItemReplicaInfo
//     0x132B69  GameEngine::GetItemMaxStackSize                                  <- STEP 2 here
//     0x132B82  replica.objectId = 0
//     0x132B89  Item::CreateItem(&replica)                                       <- SUBSTITUTE
//
// The arm carries the ITEM POINTER the room check was asked about. That is the stored prototype
// of one specific record, so the final test - "the record this creation is for has a map node
// whose live prototype IS the object the room check just asked about" - cannot be produced by any
// crafting or market path, which never touch the reagent map's prototypes at all.
//
// ALL of these must hold, every time, or the engine's own replica is used unchanged and the log
// says which one failed:
//   1. identity=1 and the caravan window is open (g_transferOpenNow, the edge-triggered flag);
//   2. the observer reached step 2 within identity_window_ms (the exe does all of it in a frame);
//   3. the incoming replica's base record (+0x08) is one of OUR page records;
//   4. that record has a journal entry (the deposited item's whole 0x190 replica + its strings);
//   5. that record's node is in the engine's own map, its prototype passes the liveness triple
//      and its stack size is >= 1, i.e. the page really still holds the item;
//   6. that prototype is the very object the room check was asked about;
//   7. identityBuild() accepts: the journal replica is exactly as long as the live one.
// The substitute keeps the incoming object id and stack-size mirror and takes everything else -
// seed, prefix, suffix, modifier, transmute, materia, relicCompletionBonus, enchantment,
// ascendant, ascendant2H, relicSeed, enchantmentSeed, materiaCombines, seedRerolls, affixRerolls
// - from the journal: the string-shaped ones through the deep-copied slot table (re-pointed at
// storage that outlives the call), the numeric ones as the blob bytes they are.
volatile LONG g_idStage = 0;    // 0 idle | 1 the room check armed | 2 GetItemMaxStackSize seen
volatile LONG g_idArmAt = 0;    // GetTickCount at the arm
volatile LONG g_idBusy = 0;     // held across the substituted Item::CreateItem call
volatile LONG g_idArms = 0;
volatile LONG g_idRestores = 0;
volatile LONG g_idMisses = 0;
volatile LONG g_idFaults = 0;
volatile LONG g_idRescueRestores = 0;
const GdItem* g_idProto = nullptr;  // the object Player::IsInventorySpaceAvailable was asked about
// WHICH authority this arm is about. A record can be TABLE-OWNED and MAP-OWNED at the same time
// while an old engine row survives, and the box the user actually clicked decides which copy
// leaves - so the count that comes down afterwards must be the one that armed, never "the
// table's, if it has one". False = the engine's map armed this take.
bool g_idFromTable = false;
char g_idRecord[256] = {0};         // the record the arm belongs to (lower-cased)
char g_idPending[256] = {0};        // the record the in-flight substitution belongs to
char g_idWhy[256] = "no reagent take observed yet";
char g_idSite[192] = "?";
UtReplicaCapture* g_idEntry = nullptr;     // the journal entry - 4 KB, never on a detour stack
UtIdentityOverlay* g_idOverlay = nullptr;  // the substitute; MUST outlive the CreateItem call
unsigned char* g_idIncoming = nullptr;     // the replica the engine was about to use

volatile LONG g_idOff = 0;      // 1 = the feature disabled itself; never re-armed in-session
// 1 = GameEngine::GetItemMaxStackSize could not be observed (folded across exports, or the hook
// failed), so step 2 of the state machine is unavailable and the arm alone has to carry it. The
// arm is still the load-bearing test - it fires only for an object that IS a stored prototype of
// one of our records with a journal entry - so this degrades the classifier without ever weakening
// it into "the return address is in the exe", which is the one rule this block may not break.
volatile LONG g_idOneStep = 0;

// MSVC's identical-COMDAT folding makes hundreds of exports share one address
// (ItemEquipment::IncrementStack and 524 others are the same `xor al,al; ret`). Detouring such an
// address would change the behaviour of every one of them.
// So before either observer is hooked, count how many entries of the module's OWN export address
// table point at it. Exactly one = the function is not folded and may be hooked; anything else
// (or an unreadable header) = the observer is refused and identity disables itself.
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
        const IMAGE_EXPORT_DIRECTORY* ex =
            (const IMAGE_EXPORT_DIRECTORY*)(base + d.VirtualAddress);
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

// 1 once the two reagent-take arm sites have been resolved by signature (see the block next to
// hk_ItemGetItemReplicaInfo).
volatile LONG g_idSitesOk = 0;

// ---- NOTHING GATES IDENTITY ON HAVING OBSERVED AN ARM -----------------------------------------
// Identity is "available" as soon as the machinery is present (see identityAvailable below); the
// mod does not additionally wait to see a detour fire before it lets a non-pristine item be
// deposited. A take that then cannot restore an identity is loud rather than refused: it logs
// `identity: NOT restored ... the taken copy is STOCK - <why>` in capitals, counts idMisses, and
// changes nothing else. The safety that matters does not depend on that observation:
//   * identityBuild's NULL-shape refusal, and the same refusal applied when the journal file is
//     read, so a bad capture is never fed to Item::CreateItem;
//   * the self-DISABLE (g_idOff) on a signature or detour failure - if the machinery is not
//     there, identityAvailable() is false and the choke point REFUSES rather than strips;
//   * the multiplayer bar on every identity operation;
//   * `collect_pristine_only`, an ini switch (default 0) that restricts collection to pristine
//     items.
volatile LONG g_gameTid = 0;        // the game thread, recorded from PresentSurface's own tick
unsigned int g_selfBuildStamp = 0;  // our own IMAGE_NT_HEADERS.FileHeader.TimeDateStamp

unsigned int moduleTimeDateStamp(HMODULE m) {
    if (!m) return 0;
    __try {
        const unsigned char* base = (const unsigned char*)m;
        const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
        const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
        return (unsigned int)nt->FileHeader.TimeDateStamp;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// Start-up, once, from reagentInit. The stamp decides nothing; it is diagnostics, and it is what
// matches a crash dump to the binary that produced it.
void modBuildStampInit(HMODULE selfModule) {
    g_selfBuildStamp = moduleTimeDateStamp(selfModule);
    logD("mod: this build stamp 0x%08X (this module's PE TimeDateStamp; the .pdb beside it and "
         "the .map the build kept belong to exactly this binary)",
         g_selfBuildStamp);
}

// identitySubstituteInner returns nullptr for two very different reasons and only ONE of them
// says the restore machinery is broken:
//   * "this creation is not the take we armed for", or "there is no journal entry for this
//     record" (deposited before the journal existed, a capture that was never made, an entry a
//     rescue restored without one) - the mod had nothing to restore and nothing failed;
//   * the entry EXISTED and could not be turned into a replica (the NULL-shape flag, an
//     identityBuild refusal, a replica that could not be read, missing buffers) - that is a real
//     failure of the machinery.
// This flag gates nothing. It decides how loud the `NOT restored` line is, and it is what the
// load path's own fault gate reads next to g_idFaults.
// Set false at the top of every identitySubstitute call, true only on the second kind.
bool g_idRestoreFailed = false;

// "Is the machinery THERE", and nothing else. Every conjunct below is a capability:
//   identity=1                the user has not turned the feature off,
//   !g_idOff                  it has not self-disabled after a signature or detour failure,
//   g_idSitesOk               the exactly-twice take-site signature resolved,
//   the three buffers         the identity working set was allocated,
//   g_replicaOffset           the ItemReplicaInfo offset was decoded.
bool identityAvailable() {
    return g_cfg.identity != 0 && !InterlockedCompareExchange(&g_idOff, 0, 0) &&
           InterlockedCompareExchange(&g_idSitesOk, 0, 0) != 0 && g_idEntry && g_idOverlay &&
           g_idIncoming && g_replicaOffset != 0;
}

void identityClear() {
    InterlockedExchange(&g_idStage, 0);
    g_idProto = nullptr;
    g_idRecord[0] = 0;
    g_idFromTable = false;  // which authority armed dies with the arm
}

void identityNote(const char* why) {
    _snprintf_s(g_idWhy, sizeof(g_idWhy), _TRUNCATE, "%s", why ? why : "?");
}

// ---- the crash diagnostics --------------------------------------------------------------------
// A crash inside the engine while it copies an ItemReplicaInfo - an access violation reading
// address 0 out of a memcpy with a NULL source is the shape that occurs in practice - is fully
// diagnosable from the log these two helpers write, with no minidump at all. Neither touches
// engine memory: everything they print is mod state or the exception record itself.

// `addr` -> "Game.dll+0x388BE". VirtualQuery + GetModuleFileName only, so it is safe on any
// address, including one in no module at all.
void faultModuleText(const void* addr, char* out, size_t cap) {
    MEMORY_BASIC_INFORMATION mbi;
    memset(&mbi, 0, sizeof(mbi));
    if (VirtualQuery(addr, &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.AllocationBase) {
        char path[MAX_PATH] = {0};
        const HMODULE m = (HMODULE)mbi.AllocationBase;
        if (GetModuleFileNameA(m, path, MAX_PATH)) {
            const char* leaf = strrchr(path, '\\');
            _snprintf_s(out, cap, _TRUNCATE, "%s+0x%zX", leaf ? leaf + 1 : path,
                        (size_t)((const unsigned char*)addr - (const unsigned char*)m));
            return;
        }
    }
    _snprintf_s(out, cap, _TRUNCATE, "%p (no module)", addr);
}

// The mod's identity state in one line. Called from the fault filter, from the vectored handler
// and from the worker's stall check, so it reads atomics and fixed buffers only.
const char* identityStateText(char* out, size_t cap) {
    char slots[192];
    slots[0] = 0;
    size_t at = 0;
    if (g_idOverlay) {
        const int n = g_idOverlay->slotCount;
        for (int i = 0; i < n && i < 24 && at + 10 < sizeof(slots); ++i) {
            const int w = _snprintf_s(slots + at, sizeof(slots) - at, _TRUNCATE, "%s+0x%03X",
                                      i ? " " : "", g_idOverlay->slotOff[i]);
            if (w <= 0) break;
            at += (size_t)w;
        }
    }
    _snprintf_s(out, cap, _TRUNCATE,
                "game thread t%ld | idBusy=%ld idStage=%ld idOff=%ld armed=\"%s\" "
                "pending=\"%s\" arms=%ld restores=%ld misses=%ld faults=%ld overlay slots=[%s]",
                InterlockedCompareExchange(&g_gameTid, 0, 0),
                InterlockedCompareExchange(&g_idBusy, 0, 0),
                InterlockedCompareExchange(&g_idStage, 0, 0),
                InterlockedCompareExchange(&g_idOff, 0, 0), g_idRecord[0] ? g_idRecord : "-",
                g_idPending[0] ? g_idPending : "-", InterlockedCompareExchange(&g_idArms, 0, 0),
                InterlockedCompareExchange(&g_idRestores, 0, 0),
                InterlockedCompareExchange(&g_idMisses, 0, 0),
                InterlockedCompareExchange(&g_idFaults, 0, 0), slots);
    return out;
}

// ONE line per fault, flushed synchronously. NEVER a handler: every caller returns
// EXCEPTION_CONTINUE_SEARCH after this.
void logEngineFault(const char* where, const EXCEPTION_RECORD* er, unsigned long tid) {
    __try {
        if (!er) return;
        // A fault inside one of the mod's own guarded probes IS the probe's answer - the scanners
        // dereference 8-aligned windows and the map walk follows whatever a node holds. The probe
        // counts them ("N faulted candidates"); logging them as well buried the real ones.
        if (g_probeDepth > 0) return;
        // The two SEH filters that call this (hk_ItemCreateItem and refreshApply) are NOT
        // restricted by exception code the way the vectored handler is - they see every
        // exception raised inside the engine call they wrap. The engine throws C++ exceptions
        // (0xE06D7363) as a matter of course and handles them in an OUTER frame, and
        // Item::CreateItem runs at the MAIN MENU, so logging one would print a
        // "***** EXCEPTION 0x" line for perfectly normal engine behaviour.
        // Log only ERROR-severity codes (0xCxxxxxxx: access violation, stack
        // overflow, illegal instruction, in-page error) - the same class the vectored handler is
        // restricted to; 0xE... (C++), 0x8... (breakpoint) and 0x4... (DBG_PRINTEXCEPTION) are
        // not crashes. Every caller still returns EXCEPTION_CONTINUE_SEARCH either way, so this
        // changes what is LOGGED and never what is handled.
        if ((er->ExceptionCode & 0xF0000000ul) != 0xC0000000ul) return;
        char at[160];
        faultModuleText(er->ExceptionAddress, at, sizeof(at));
        char state[512];
        identityStateText(state, sizeof(state));
        unsigned long long p0 = 0, p1 = 0;
        if (er->NumberParameters > 0) p0 = er->ExceptionInformation[0];
        if (er->NumberParameters > 1) p1 = er->ExceptionInformation[1];
        const char* kind = "";
        if (er->ExceptionCode == (DWORD)EXCEPTION_ACCESS_VIOLATION) {
            kind = p0 == 0 ? " (read)" : (p0 == 1 ? " (write)" : " (execute)");
        }
        logD("***** EXCEPTION 0x%08lX%s at %s inside %s, thread %lu, params=[0x%llX 0x%llX] | %s "
             "| the mod does NOT handle it - EXCEPTION_CONTINUE_SEARCH",
             er->ExceptionCode, kind, at, where ? where : "?", tid, p0, p1, state);
        logFlush();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// The one place that turns identity off for the rest of the session. Never re-armed.
// Everything downstream fails safe: the journal keeps every entry, the rescue command still hands
// items back, and hk_AddItemToReagents REFUSES a non-pristine deposit instead of stripping it.
void identityDisable(const char* why) {
    if (InterlockedExchange(&g_idOff, 1)) return;
    identityNote(why);
    identityClear();
    logE("identity: ***** DISABLED ***** - %s", why ? why : "?");
    logD("no replica is substituted this session; the collection keeps every entry, rescue=1 "
         "still hands items back and a non-pristine deposit is refused rather than stripped");
}

// `Item::SetStackSize` is `mov [rcx+0x88C],edx ; mov [rcx+0x6B0],edx ; ret`, which is why a
// stack count survives a save/load. The SECOND store is the ItemReplicaInfo mirror - the field
// `ReadPlayerReagents` writes the saved count into at replica+0x178 - so its displacement minus
// g_replicaOffset is where the stack lives inside a replica. Decoded, never typed.
unsigned int decodeStackMirrorOffset(const void* fn) {
    if (!fn) return 0;
    const unsigned char* p = (const unsigned char*)fn;
    unsigned int a = 0, b = 0;
    __try {
        if (p[0] != 0x89 || p[1] != 0x91) return 0;
        memcpy(&a, p + 2, 4);
        if (p[6] != 0x89 || p[7] != 0x91) return 0;
        memcpy(&b, p + 8, 4);
        if (p[12] != 0xC3) return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return (a && b && b < a) ? b : 0;
}

bool identityCopyIn(const void* src, unsigned char* dst, unsigned int n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// =============================================================================================
// THE ARM SITE.
//
// The obvious place to arm - `Player::IsInventorySpaceAvailable`, the room check - is the WRONG
// one. The ReagentWindow mouse handler has TWO take blocks, only the first of them asks about
// room, and real UI takes go through the second. Disassembly of the exe image:
//
//   block A  0x132B2C  call [r8+0x938]   Player::IsInventorySpaceAvailable
//            0x132B45  lea rcx,[rbp-0x30]; call 0x1D400        ItemReplicaInfo ctor
//            0x132B59  call [rax+0x590]  Item::GetItemReplicaInfo   -> RETURNS TO 0x132B5F
//            0x132B69  GameEngine::GetItemMaxStackSize
//            0x132B82  replica.objectId = 0
//            0x132B89  call [rip+...]    Item::CreateItem            -> returns to 0x132B8F
//
//   block B  0x132CB7  lea rcx,[rbp-0x30]; call 0x1D400        ItemReplicaInfo ctor
//            0x132CC1  mov rax,[rsi]; lea rdx,[rbp-0x30]; mov rcx,rsi
//            0x132CCB  call [rax+0x590]  Item::GetItemReplicaInfo   -> RETURNS TO 0x132CD1
//            0x132CD1  mov [rbp+0x148],1
//            0x132CDD  replica.objectId = 0
//            0x132CE4  call [rip+0x1A4E66] Item::CreateItem          -> returns to 0x132CEA
//
// Block B - the one a real take goes through - contains NO room check at all, so an observer on
// the room check never arms. The arm is therefore the call the two blocks DO share:
// `Item::GetItemReplicaInfo` on the stored prototype, one Game.dll export (rva 0x3100B0,
// `mov rax,rdx; lea rdx,[rcx+0x538]; mov rcx,rax; jmp <ItemReplicaInfo::operator=>`; the only
// `GetItemReplicaInfo` in Game.dll's 25,100-name export table, so no derived class hides it).
//
// The exe RVA is NOT hard-coded. The 16 bytes
//     48 8B 06 48 8D 55 D0 48 8B CE FF 90 90 05 00 00
// (`mov rax,[rsi]; lea rdx,[rbp-0x30]; mov rcx,rsi; call [rax+0x590]`) carry no relocated byte
// and match EXACTLY TWICE in the whole 4.7 MB image - at 0x132B4F and 0x132CC1, i.e. the two
// reagent-take sites and nothing else. The scan therefore demands exactly two hits, both inside
// the exe's own .text, within 0x400 bytes of each other, and the accepted return addresses are
// `hit + 16`. Anything else disables identity for the session (fail-safe: the journal keeps every
// entry, the rescue command still hands items back, and a non-pristine deposit is REFUSED rather
// than stripped).
//
// The return address is only an EXTRA positive check, never the classifier on its own.
// The load-bearing classifier is state - `this` must be the live stored prototype of one of our
// records, with a journal entry - and `identitySubstituteInner` re-runs every one of those tests
// on the CreateItem side before a single byte is substituted.
const unsigned char kSigTakeReplica[] = {0x48, 0x8B, 0x06, 0x48, 0x8D, 0x55, 0xD0,
                                         0x48, 0x8B, 0xCE, 0xFF, 0x90, 0x90, 0x05,
                                         0x00, 0x00};
const void* g_idTakeRa[2] = {nullptr, nullptr};
volatile LONG g_idSiteTries = 0;

bool identityTextRange(const unsigned char** lo, const unsigned char** hi) {
    HMODULE exe = GetModuleHandleW(nullptr);
    if (!exe) return false;
    const unsigned char* base = (const unsigned char*)exe;
    __try {
        const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
        const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            if (memcmp(sec[i].Name, ".text", 5) == 0) {
                *lo = base + sec[i].VirtualAddress;
                *hi = *lo + sec[i].Misc.VirtualSize;
                return true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return false;
}

// Returns the number of hits found (capped at 3, which is already a failure).
int identityScanTakeSites(const unsigned char** hits) {
    const unsigned char *lo = nullptr, *hi = nullptr;
    if (!identityTextRange(&lo, &hi)) return -1;
    const size_t n = sizeof(kSigTakeReplica);
    int count = 0;
    __try {
        for (const unsigned char* p = lo; p + n <= hi; ++p) {
            if (p[0] != kSigTakeReplica[0]) continue;
            if (memcmp(p, kSigTakeReplica, n) != 0) continue;
            if (count < 2) hits[count] = p;
            if (++count > 2) break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return count;
}

void identityNote(const char* why);
void identityDisable(const char* why);

// Game thread, 2 Hz. The exe's .text is Steam-DRM encrypted until the stub has run, so the scan
// has to be retried exactly the way ut_live's and ut_plate's are (40 attempts, then give up).
void identityTakeSiteTick() {
    if (!g_cfg.identity) return;
    if (InterlockedCompareExchange(&g_idOff, 0, 0)) return;
    if (InterlockedCompareExchange(&g_idSitesOk, 0, 0)) return;
    static DWORD lastTry = 0;
    const DWORD now = GetTickCount();
    if (lastTry && now - lastTry < 500) return;
    lastTry = now;
    const LONG try_ = InterlockedIncrement(&g_idSiteTries);
    const unsigned char* hits[2] = {nullptr, nullptr};
    const int n = identityScanTakeSites(hits);
    if (n == 2 && hits[0] && hits[1]) {
        const size_t gap = (size_t)(hits[1] - hits[0]);
        if (gap == 0 || gap > 0x400) {
            identityDisable("the two reagent-take signatures are not in the same handler");
            return;
        }
        g_idTakeRa[0] = hits[0] + sizeof(kSigTakeReplica);
        g_idTakeRa[1] = hits[1] + sizeof(kSigTakeReplica);
        InterlockedExchange(&g_idSitesOk, 1);
        bindingsNote("exe.takeReplicaPair", (unsigned long long)(ULONG_PTR)hits[0], true,
                     "exactly two matches in the exe .text, inside one handler");
        logD("identity: take-site signature -> 2 sites, exe rva 0x%llX and 0x%llX (arm return "
             "addresses 0x%llX / 0x%llX), %ld bytes apart, attempt %ld",
             (unsigned long long)(hits[0] - g_exeBase), (unsigned long long)(hits[1] - g_exeBase),
             (unsigned long long)((const unsigned char*)g_idTakeRa[0] - g_exeBase),
             (unsigned long long)((const unsigned char*)g_idTakeRa[1] - g_exeBase), (long)gap,
             try_);
        return;
    }
    if (try_ == 40) {
        char why[192];
        _snprintf_s(why, sizeof(why), _TRUNCATE,
                    "the reagent-take signature matched %d time(s) in the exe .text after 40 "
                    "attempts (need exactly 2)",
                    n);
        bindingsNote("exe.takeReplicaPair", 0, false,
                     "exactly two matches in the exe .text after 40 attempts");
        identityDisable(why);
    }
}

// ---- the two deposit call sites, by signature -------------------------------------------------
// No EXE code address is compiled into the mod. Both patterns wildcard every
// `call qword ptr [rip+disp32]` displacement, so they carry no address at all; tools/
// test_bindings.cpp proves each one matches exactly once in the 1.3.0.8 image, and the run-time
// scan counts the matches again and accepts nothing but one.
//
// WHAT CHANGES IF THE SCAN NEVER SUCCEEDS: `depositCallerKind` answers UNKNOWN, so a bag
// shift-click is REFUSED instead of being taken into the private table - the item stays in the
// bag. That is the same fail-closed direction the rest of the gate takes, and it is why the scan
// is allowed to be late: nothing is ever taken on an unproven path while it is still running.
volatile LONG g_siteTries = 0;

void depositSiteTick() {
    if (g_siteA && g_siteB) return;
    if (InterlockedCompareExchange(&g_siteTries, 0, 0) > 40) return;
    static DWORD lastTry = 0;
    const DWORD now = GetTickCount();
    if (lastTry && now - lastTry < 500) return;
    lastTry = now;
    const LONG try_ = InterlockedIncrement(&g_siteTries);

    const unsigned char *lo = nullptr, *hi = nullptr;
    if (!identityTextRange(&lo, &hi)) return;
    const UtBindPattern* pats[2] = {&kUtSigDepositSiteA, &kUtSigDepositSiteB};
    const unsigned char** slots[2] = {&g_siteA, &g_siteB};
    for (int i = 0; i < 2; ++i) {
        if (*slots[i]) continue;
        const unsigned char* hit = nullptr;
        int count = 0;
        __try {
            count = utBindScan(lo, hi, *pats[i], &hit, 1);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return;  // the .text is not readable yet; try again in half a second
        }
        if (count == 1 && hit) {
            *slots[i] = hit;
            bindingsNote(pats[i]->name, (unsigned long long)(size_t)(hit - g_exeBase), true,
                         "exactly one match in the exe .text");
            logD("deposit: %s located by signature at exe rva 0x%llX on attempt %ld (accepted "
                 "return window 0x%llX..0x%llX)",
                 pats[i]->name, (unsigned long long)(size_t)(hit - g_exeBase), try_,
                 (unsigned long long)(size_t)(hit - g_exeBase +
                                              (i == 0 ? kUtSiteAWindowLo : kUtSiteBWindowLo)),
                 (unsigned long long)(size_t)(hit - g_exeBase +
                                              (i == 0 ? kUtSiteAWindowHi : kUtSiteBWindowHi)));
        } else if (count > 1) {
            // More than one match is NOT a late-decryption artefact: it means the pattern stopped
            // being unique, and an ambiguous call site must never be used to prove a removal.
            InterlockedExchange(&g_siteTries, 41);
            bindingsNote(pats[i]->name, 0, false, "exactly one match in the exe .text");
            logE("deposit: the %s signature matched more than once - that path is OFF for this "
                 "session and every deposit through it is REFUSED",
                 pats[i]->name);
        } else if (try_ == 40) {
            bindingsNote(pats[i]->name, 0, false,
                         "exactly one match in the exe .text after 40 attempts");
            logE("deposit: the %s signature was not found in the exe .text after 40 attempts - "
                 "that path is OFF for this session and every deposit through it is REFUSED",
                 pats[i]->name);
        }
    }
}

// An arm that is never consumed must not sit there. The exe runs
// GetItemReplicaInfo -> CreateItem inside one frame, so anything older than identity_window_ms is
// a miss: drop it, count it and say so once. Game thread, next tick.
void identityArmTick() {
    if (InterlockedCompareExchange(&g_idBusy, 0, 0)) return;  // a substitution is on the stack
    if (InterlockedCompareExchange(&g_idStage, 0, 0) == 0) return;
    const DWORD armed = (DWORD)InterlockedCompareExchange(&g_idArmAt, 0, 0);
    const int windowMs = g_cfg.identityWindowMs > 0 ? g_cfg.identityWindowMs : 250;
    if (GetTickCount() - armed <= (DWORD)windowMs) return;
    const LONG n = InterlockedIncrement(&g_idMisses);
    if (n <= 16) {
        logD("identity: the arm for %s expired without an Item::CreateItem (%d ms) - the engine's "
             "own copy was handed back and the journal entry is KEPT",
             g_idRecord[0] ? g_idRecord : "?", windowMs);
    }
    identityClear();
}

// Step 1 (the REAL one): Item::GetItemReplicaInfo was called on `item` from one of the two
// reagent-take sites. Everything expensive is behind the return-address test, which is two
// pointer compares, so this detour costs nothing on the thousands of other calls the engine
// makes.
void identityNoteTakeReplica(const GdItem* item, const void* ra) {
    if (!item) return;
    const char* name = safeObjectName(const_cast<GdItem*>(item));
    if (!name || !*name || !isOurRecord(name)) return;
    try {
        std::string key(name);
        toLower(&key);
        if (!journalHas(key.c_str())) {
            if (InterlockedCompareExchange(&g_idArms, 0, 0) < 8) {
                logD("identity: the take of %s has no journal entry - the engine's own copy is "
                     "handed back unchanged",
                     key.c_str());
            }
            return;
        }
        // ---- the private table's own arm ---------------------------------------------------
        // When the private table owns the record, the engine's reagent map does not hold it, so
        // the MAP condition three lines below could never fire and the substitution would never
        // even be asked. This is the table's equivalent pair test, and it is strictly STRONGER
        // than the map test: the record is in the table with count >= 1, and the object
        // `Item::GetItemReplicaInfo` was called on IS the prototype the MOD built for that
        // record this world. No crafting, enchanting or market path can produce a mod-created
        // object, which is exactly the property the classifier needs.
        // `storeBuiltProtoId` never BUILDS one (that is why it exists): this runs inside the
        // engine's own take frame, where creating an Item would be both a leak and a re-entry.
        // The object-id read is behind the table test, so a record the table does not own costs
        // exactly one interlocked read here.
        const unsigned int tableProtoId = storeTableOwns() ? storeBuiltProtoId(key.c_str()) : 0;
        const unsigned int itemObjId = tableProtoId ? reagentSafeObjectId(item) : 0;
        if (tableProtoId && itemObjId && tableProtoId == itemObjId &&
            storeCount(key.c_str()) >= 1) {
            _snprintf_s(g_idRecord, sizeof(g_idRecord), _TRUNCATE, "%s", key.c_str());
            g_idProto = item;
            g_idFromTable = true;
            InterlockedExchange(&g_idArmAt, (LONG)GetTickCount());
            InterlockedExchange(&g_idStage, 2);
            const LONG nn = InterlockedIncrement(&g_idArms);
            if (nn <= 24) {
                logD("identity: ARMED on Item::GetItemReplicaInfo for %s (THE PRIVATE TABLE's own "
                     "prototype %p id=%u count=%u, exe rva 0x%llX) - the next Item::CreateItem on "
                     "this thread gets the deposited identity",
                     key.c_str(), (const void*)item, tableProtoId, storeCount(key.c_str()),
                     (unsigned long long)((const unsigned char*)ra - g_exeBase));
            }
            return;
        }
        // The map's equivalent test: `this` must BE the live stored prototype of that record in
        // the engine's own reagent map. No crafting, enchanting or market path ever touches
        // those objects.
        ProtoInfo pi;
        reagentProtoInfo(key, &pi);
        if (!pi.mapOk || !pi.node || !pi.proto || !pi.live || pi.proto != item) {
            // Capped like every neighbouring identity line: should `pi.proto == item` ever be
            // false in ordinary play, this fires on every take and floods the 64 KB log buffer.
            static volatile LONG notArmed = 0;
            if (InterlockedIncrement(&notArmed) > 8) return;
            logD("identity: NOT armed for %s - the object GetItemReplicaInfo was called on is not "
                 "this record's live stored prototype (mapOk=%d node=%d live=%d same=%d)",
                 key.c_str(), pi.mapOk ? 1 : 0, pi.node ? 1 : 0, pi.live ? 1 : 0,
                 pi.proto == item ? 1 : 0);
            return;
        }
        _snprintf_s(g_idRecord, sizeof(g_idRecord), _TRUNCATE, "%s", key.c_str());
        g_idProto = item;
        g_idFromTable = false;  // the MAP armed this one
        InterlockedExchange(&g_idArmAt, (LONG)GetTickCount());
        InterlockedExchange(&g_idStage, 2);  // the arm alone completes the classifier
        const LONG n = InterlockedIncrement(&g_idArms);
        if (n <= 24) {
            logD("identity: ARMED on Item::GetItemReplicaInfo for %s (prototype %p id=%u stack=%u,"
                 " exe rva 0x%llX) - the next Item::CreateItem on this thread gets the deposited "
                 "identity",
                 key.c_str(), (const void*)item, pi.protoId, pi.stack,
                 (unsigned long long)((const unsigned char*)ra - g_exeBase));
        }
    } catch (...) {
        identityClear();
    }
}

void __cdecl hk_ItemGetItemReplicaInfo(const GdItem* self, void* out) {
    if (o_ItemGetReplica) o_ItemGetReplica(self, out);
    // The cheapest possible filter first: this export is HOT (every replication and every save
    // calls it), so the common path is one plain int read and two pointer compares. `g_idTakeRa`
    // is null until the signature scan succeeds, and no return address is ever null.
    if (!g_cfg.identity || !self) return;
    const void* ra = _ReturnAddress();
    if (ra != g_idTakeRa[0] && ra != g_idTakeRa[1]) return;
    if (InterlockedCompareExchange(&g_idOff, 0, 0)) return;
    if (!InterlockedCompareExchange(&g_idSitesOk, 0, 0)) return;
    if (!InterlockedCompareExchange(&g_transferOpenNow, 0, 0)) return;
    // The multiplayer bar here is the ini-controlled one, not an absolute ban, and it is safe to
    // let mp_collect=1 through: this arm is a MOD-SIDE READ of the stored prototype's replica -
    // no engine write at all - and the substitution it leads to is deep-copied into engine-owned
    // storage by SetItemReplicaInfo, with the object id overwritten by the engine, so after
    // Item::CreateItem returns the Item is byte-for-byte the shape the engine would have built
    // from a legitimate replica. Nothing mod-specific can reach a peer, and a take in co-op hands
    // back the real item instead of a stock copy.
    // It must stay in step with the same bar in identitySubstitute or identity is silently dead
    // here.
    if (mpBarred()) return;
    try {
        identityNoteTakeReplica(self, ra);
    } catch (...) {
    }
}

// Step 1: Player::IsInventorySpaceAvailable was asked about `item`. Cheap: everything below the
// transfer-open test costs nothing while the caravan is shut, which is every frame of normal play.
// This observer does NOT arm. The take block a real take runs through (0x132CB7..0x132CEA) never
// calls Player::IsInventorySpaceAvailable at all, so an arm placed here would fire on the wrong
// events - and, since identityArmTick() expires an unconsumed arm, it would count a miss for
// every unrelated room check on one of our records. The hook stays installed (a pure
// straight through) and logs, at most a few times, that it saw a room check for a journalled record;
// the arm itself lives in hk_ItemGetItemReplicaInfo.
void identityNoteRoomCheck(const GdItem* item) {
    if (!g_cfg.identity || !item) return;
    if (InterlockedCompareExchange(&g_idOff, 0, 0)) return;
    if (!InterlockedCompareExchange(&g_transferOpenNow, 0, 0)) return;
    static volatile LONG logged = 0;
    if (InterlockedCompareExchange(&logged, 0, 0) >= 4) return;
    const char* name = safeObjectName(const_cast<GdItem*>(item));
    if (!name || !*name || !isOurRecord(name)) return;
    try {
        std::string key(name);
        toLower(&key);
        if (!journalHas(key.c_str())) return;
        if (InterlockedIncrement(&logged) <= 4) {
            logD("identity: Player::IsInventorySpaceAvailable saw %s (diagnostic only - the arm "
                 "is on Item::GetItemReplicaInfo)",
                 key.c_str());
        }
    } catch (...) {
    }
}

bool __cdecl hk_IsInventorySpaceAvailable(const void* player, const GdItem* item) {
    const bool r = o_IsInvSpace ? o_IsInvSpace(player, item) : true;
    try {
        identityNoteRoomCheck(item);
    } catch (...) {
    }
    return r;
}

unsigned int __cdecl hk_GetItemMaxStackSize(const GdGameEngine* self) {
    const unsigned int r = o_GetItemMaxStack ? o_GetItemMaxStack(self) : 1;
    if (InterlockedCompareExchange(&g_idStage, 0, 0) == 1) InterlockedExchange(&g_idStage, 2);
    return r;
}

// The exe's two reagent-take call sites. Logged always; only ENFORCED when the ini says so,
// because an exe RVA must never be the thing a feature depends on.
bool identityCallSite(const void* ret, char* out, size_t cap) {
    const unsigned char* q = (const unsigned char*)ret;
    if (!g_exeBase || q < g_exeBase || q >= g_exeBase + g_exeSize) {
        _snprintf_s(out, cap, _TRUNCATE, "%s", creationSide(ret));
        return false;
    }
    const size_t rva = (size_t)(q - g_exeBase);
    // The two windows are not written down as rvas: they are derived from the take sites the
    // signature scan already found. The CreateItem call the observer wants to see is
    // the next call in the same handler, within 0x40 bytes of the replica call's return address.
    // A window that is a little wider than the two hand-measured ones costs nothing here - this
    // is an EXTRA positive check, never the thing a substitution depends on (the load-bearing
    // classifier is state, re-tested in full on the CreateItem side).
    bool known = false;
    for (int i = 0; i < 2 && !known; ++i) {
        const unsigned char* ra = (const unsigned char*)g_idTakeRa[i];
        if (ra && q > ra && q <= ra + 0x40) known = true;
    }
    _snprintf_s(out, cap, _TRUNCATE, "Grim Dawn.exe +0x%zX%s", rva,
                known ? " (a known reagent-take site)" : " (NOT a known reagent-take site)");
    return known;
}

const void* identitySubstituteInner(const void* replica, const void* ret, char* site,
                                    size_t siteCap) {
    const DWORD armed = (DWORD)InterlockedCompareExchange(&g_idArmAt, 0, 0);
    const int windowMs = g_cfg.identityWindowMs > 0 ? g_cfg.identityWindowMs : 250;
    if (GetTickCount() - armed > (DWORD)windowMs) {
        identityNote("the take observer's arm went stale before Item::CreateItem");
        return nullptr;
    }
    if (!InterlockedCompareExchange(&g_transferOpenNow, 0, 0)) {
        identityNote("the caravan window is not open");
        return nullptr;
    }
    if (!g_replicaOffset || !g_replicaSize || g_replicaSize > 0x200) {
        g_idRestoreFailed = true;  // the machinery itself is not there
        identityNote("the ItemReplicaInfo offset/size is unknown");
        return nullptr;
    }
    if (!g_idEntry || !g_idOverlay || !g_idIncoming) {
        g_idRestoreFailed = true;  // the machinery itself is not there
        identityNote("the identity buffers were never allocated");
        return nullptr;
    }
    const char* rec = replicaRecord(replica);  // SEH-guarded reader
    if (!rec || !*rec) {
        identityNote("the incoming replica has no base record at +0x08");
        return nullptr;
    }
    std::string key(rec);
    toLower(&key);
    if (_stricmp(key.c_str(), g_idRecord) != 0) {
        char why[256];
        _snprintf_s(why, sizeof(why), _TRUNCATE,
                    "the creation is for %.80s but the observer armed on %.80s", key.c_str(),
                    g_idRecord);
        identityNote(why);
        return nullptr;
    }
    // ---- the last two conditions, private-table variant --------------------------------------
    // Under the private table the map conditions in the `else` branch are false by construction -
    // there is no node, and the room check asked about a MOD-built object - so every take would
    // come back "STOCK". This pair replaces them: the record is in the table with count >= 1, and
    // the object the room check was asked about IS the table's prototype for that record.
    const unsigned int tableProtoId = storeTableOwns() ? storeBuiltProtoId(key.c_str()) : 0;
    const unsigned int tableCount = tableProtoId ? storeCount(key.c_str()) : 0;
    if (tableProtoId && tableCount >= 1) {
        const unsigned int askedId = g_idProto ? reagentSafeObjectId(g_idProto) : 0;
        if (!askedId || askedId != tableProtoId) {
            char why[256];
            _snprintf_s(why, sizeof(why), _TRUNCATE,
                        "the object the room check asked about (id=%u) is not the private table's "
                        "prototype for %.60s (id=%u) - this creation is NOT the reagent take",
                        askedId, key.c_str(), tableProtoId);
            identityNote(why);
            return nullptr;
        }
    } else {
        ProtoInfo pi;
        reagentProtoInfo(key, &pi);
        if (!pi.mapOk || !pi.node || !pi.proto || !pi.live || pi.stack < 1) {
            char why[256];
            _snprintf_s(why, sizeof(why), _TRUNCATE,
                        "the reagent map does not hold %.60s right now (mapOk=%d node=%d live=%d "
                        "stack=%u)%s",
                        key.c_str(), pi.mapOk ? 1 : 0, pi.node ? 1 : 0, pi.live ? 1 : 0, pi.stack,
                        storeTableOwns() ? " and the private table has no built prototype with a "
                                           "count for it either"
                                         : "");
            identityNote(why);
            return nullptr;
        }
        if (pi.proto != g_idProto) {
            identityNote(
                "the object the room check asked about is not this record's stored prototype - "
                "this creation is NOT the reagent take");
            return nullptr;
        }
    }
    if (!journalGet(key.c_str(), g_idEntry)) {
        identityNote(
            "no journal entry for this record (deposited before the journal existed, or already "
            "handed back)");
        return nullptr;
    }
    // An entry that was flagged as malformed while the journal file was read is never
    // substituted. It is not deleted, so a re-deposit of the same item overwrites it with a
    // capture that is sound.
    const unsigned int flagged = journalEntryFlagged(key.c_str());
    if (flagged) {
        g_idRestoreFailed = true;  // the entry EXISTS and cannot be used
        char why[256];
        _snprintf_s(why, sizeof(why), _TRUNCATE,
                    "the journal entry has a NULL string pointer at +0x%X (it was captured by a "
                    "build with the +0x68 slot bug) - deposit this item again to re-capture it",
                    flagged - 1);
        identityNote(why);
        return nullptr;
    }
    if (!identityCopyIn(replica, g_idIncoming, g_replicaSize)) {
        g_idRestoreFailed = true;
        identityNote("the incoming replica could not be read");
        return nullptr;
    }
    if (!identityBuild(*g_idEntry, g_idIncoming, g_replicaSize, g_replicaStackOff, g_idOverlay)) {
        g_idRestoreFailed = true;
        identityNote(g_idOverlay->why);
        return nullptr;
    }
    // One line per rebuilt slot, plus the NULL-string test printed for the 8 bytes after each
    // rebuilt window even when it did not fire. Together they name a string window that was
    // rebuilt at the wrong offset the moment it happens, from the log alone.
    for (int si = 0; si < g_idOverlay->slotCount && si < 24; ++si) {
        const unsigned int off = g_idOverlay->slotOff[si];
        char text[176];
        text[0] = 0;
        utReadMsvcString(g_idOverlay->replica, off, g_replicaSize, text, sizeof(text));
        unsigned long long nptr = 0;
        size_t nsize = 0;
        bool after = false;
        if (off + 8 + 0x20 <= g_replicaSize) {
            memcpy(&nptr, g_idOverlay->replica + off + 8, sizeof(nptr));
            memcpy(&nsize, g_idOverlay->replica + off + 8 + 0x10, sizeof(nsize));
            after = (nptr == 0 && nsize > 0);
        }
        logD("identity: rebuilt +0x%03X = \"%.120s\"%s", off, text,
             after ? "  [the 8 bytes after this window read _Ptr==0 with _Mysize>0]" : "");
    }
    const bool knownSite = identityCallSite(ret, site, siteCap);
    if (g_cfg.identityRequireCallsite && !knownSite) {
        char why[256];
        _snprintf_s(why, sizeof(why), _TRUNCATE,
                    "identity_require_callsite=1 and the return address is %.150s", site);
        identityNote(why);
        return nullptr;
    }
    _snprintf_s(g_idPending, sizeof(g_idPending), _TRUNCATE, "%s", key.c_str());
    return g_idOverlay->replica;
}

const void* identitySubstitute(const void* replica, const void* ret) {
    if (!g_cfg.identity || !replica) return nullptr;
    if (InterlockedCompareExchange(&g_idOff, 0, 0)) return nullptr;
    const LONG need = InterlockedCompareExchange(&g_idOneStep, 0, 0) ? 1 : 2;
    if (InterlockedCompareExchange(&g_idStage, 0, 0) < need) return nullptr;
    // The same ini-controlled multiplayer bar the arm applies; the two must stay in step.
    if (mpBarred()) return nullptr;  // belt and braces, see the arm
    if (InterlockedCompareExchange(&g_idBusy, 1, 0)) return nullptr;  // never re-enter the buffers
    const void* sub = nullptr;
    identityNote("?");
    _snprintf_s(g_idSite, sizeof(g_idSite), _TRUNCATE, "?");
    g_idRestoreFailed = false;  // see the flag's own comment
    try {
        sub = identitySubstituteInner(replica, ret, g_idSite, sizeof(g_idSite));
    } catch (...) {
        sub = nullptr;
        g_idRestoreFailed = true;
        InterlockedIncrement(&g_idFaults);
        identityNote("the substitution threw - the engine's own replica is used unchanged");
    }
    if (!sub) {
        InterlockedIncrement(&g_idMisses);
        // A miss refuses nothing and costs nothing except this line - which is why the line has
        // to be worth reading: it is the only thing that says a copy came back stock.
        // `g_idRestoreFailed` separates "there was nothing here to restore" from "the machinery
        // asked for an identity and could not build it".
        // In a multiplayer session the stake is higher and this is the one place that can say
        // so: the add command 0xB7 serialises the item's whole ItemReplicaInfo AFTER the
        // substitution point, so whatever comes back here is what the host is told -
        // permanently.
        logD("identity: NOT restored - the taken copy is STOCK - %s [site %s]%s%s", g_idWhy,
             g_idSite,
             g_idRestoreFailed
                 ? " ***** the identity machinery FAILED on an entry that WAS in the journal: "
                   "deposit this item again to re-capture it, and read the lines above this one"
                 : " (nothing was lost: the mod held no identity for this creation)",
             mpSessionMode() != UT_MP_SINGLE
                 ? " - in a multiplayer session the host receives this stock copy (the add "
                   "command 0xB7 serialises the item AFTER the restore)"
                 : "");
        identityClear();
        InterlockedExchange(&g_idBusy, 0);
    }
    return sub;
}

// The two drop-condition bytes are NOT part of an ItemReplicaInfo: +0xC00 has 96 displacement
// sites in Game.dll and none of them is in Item::GetItemReplicaInfo, SetItemReplicaInfo or
// Item::CreateItem, so a copy the take path creates from a replica is never soulbound and never
// untradeable, whatever the deposited item was. The journal entry carries what the deposited item
// had; write it back onto the created item, behind the same liveness rule as every other write.
// Only reached from identityAfterCreate, i.e. inside identitySubstitute's own multiplayer bar.
void identityReapplyFlags(const GdItem* created) {
    if (!created || !g_idEntry) return;
    const unsigned int f = g_idEntry->flags;
    if (!(f & (UT_JF_SOULBOUND | UT_JF_UNTRADEABLE))) return;
    if (!g_soulboundOffset || !g_untradeableOffset) return;
    GdItem* it = const_cast<GdItem*>(created);
    const unsigned int id = safeObjectId(it);
    if (!itemIsLive(it, id)) {
        InterlockedIncrement(&g_deadWrites);
        return;
    }
    const unsigned char s = (f & UT_JF_SOULBOUND) ? 1 : 0;
    const unsigned char u = (f & UT_JF_UNTRADEABLE) ? 1 : 0;
    if (writeDropBytes(it, s, u)) {
        logD("identity: soulbound/untradeable re-applied to %s (Item+0x%X=%u, Item+0x%X=%u) - the "
             "engine's own replica does not carry either byte",
             g_idPending, g_soulboundOffset, (unsigned)s, g_untradeableOffset, (unsigned)u);
    }
}

void identityAfterCreate(const GdItem* created) {
    try {
        if (created && g_idOverlay) {
            InterlockedIncrement(&g_idRestores);
            // ---- a take DECREMENTS the row, it does not drop it -----------------------------
            // Under the private table the journal IS the collection: dropping the entry would
            // throw away the identity of an item the user may put straight back, and the row the
            // page paints from with it. The count goes down instead and the entry survives as
            // history (`journal_prune` stays the only thing in the mod that removes a row).
            // The decrement happens HERE, inside the take's own Item::CreateItem frame, i.e.
            // BEFORE the engine writes the decremented stack onto our prototype at exe 0x132C16
            // - which is exactly why takeWatchTick SETS the count to the stack it observes
            // instead of subtracting again.
            // g_idFromTable, not "the table has a row": a record can be in BOTH places during the
            // transition and the box the user clicked is what decides which copy left. Taking the
            // map's copy must not decrement the table's count.
            // THE COUNT is the guard, and no mode byte is: a table-painted item must not be
            // taken with neither branch recording it - the item in the bag AND the row still
            // claiming it is exactly the duplicate this code exists to prevent. A row the table
            // does not hold has `storeCount` 0 and control reaches `journalDrop`.
            if (g_idFromTable && g_idPending[0] && storeCount(g_idPending) >= 1) {
                const unsigned int left = storeOnTake(g_idPending, 1);
                logD("identity: the private table's count for %s is now %u (the journal entry is "
                     "KEPT as history%s)",
                     g_idPending, left, left ? "" : " and the row is marked not stored");
            } else if (g_idPending[0] && storeCount(g_idPending) >= 1) {
                // The MAP's copy of a record the table ALSO holds. Dropping the entry here would
                // take the table's row - its count and its identity - with it.
                logD("identity: %s left the engine's map, but the private table still holds %u - "
                     "the journal entry is KEPT (it is the table's row)",
                     g_idPending, storeCount(g_idPending));
            } else {
                journalDrop(g_idPending);
            }
            // The byte count is printed with its denominator on purpose: it is NOT a
            // completeness score, because the ten 0x20 string windows are excluded from the
            // comparison.
            logD("identity: restored %s (%d string slot%s re-pointed, %d skipped, %d of %d "
                 "comparable non-string bytes came from the journal, object id %s, stack %s, "
                 "site %s)",
                 g_idPending, g_idOverlay->slotCount, g_idOverlay->slotCount == 1 ? "" : "s",
                 g_idOverlay->slotsSkipped, g_idOverlay->bytesFromJournal,
                 g_idOverlay->bytesComparable,
                 g_idOverlay->keptObjectId ? "kept from the engine" : "from the journal",
                 g_idOverlay->keptStack ? "kept from the engine" : "from the journal", g_idSite);
            identityReapplyFlags(created);
        } else {
            logD("identity: Item::CreateItem returned nothing for %s - the journal entry is KEPT",
                 g_idPending);
        }
    } catch (...) {
    }
    g_idPending[0] = 0;
    identityClear();
    InterlockedExchange(&g_idBusy, 0);
}

// SEH-guarded read of a live prototype's ItemReplicaInfo into a plain buffer, object id zeroed -
// the same in-place read the pristine test and the journal capture do. It reads the embedded
// replica directly because `Item::GetItemReplicaInfo` is an ASSIGNMENT into a live
// ItemReplicaInfo, not a copy-out into a raw buffer.
bool copyProtoReplicaSeh(const GdItem* proto, unsigned char* dst, unsigned int n) {
    if (!proto || !dst || !n || !g_replicaOffset) return false;
    __try {
        memcpy(dst, (const unsigned char*)proto + g_replicaOffset, n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (n >= 4) memset(dst, 0, 4);
    return true;
}

// A deliberate, handled probe over an object that may already be gone -
// see ProbeScope. Split because a __try may not share a function with an unwindable object.
bool copyProtoReplica(const GdItem* proto, unsigned char* dst, unsigned int n) {
    ProbeScope scope;
    return copyProtoReplicaSeh(proto, dst, n);
}

// ---- the FRESH-PROTOTYPE SWAP ----------------------------------------------------------------
// A re-deposit must NOT be applied in place. Writing the new replica onto the node's existing
// prototype and then running that object's own vtable slot +0x3F0 InitializeItem looks like the
// cheap way and is wrong: **that call is not idempotent**. `ItemRelic::InitializeItem` (0x333E40)
// re-parses the completion-bonus record and ADDS its skill grant again, so a relic reads "+1 to
// <skill>" after the first deposit, "+2" after the second and "+3" after the third.
// `ItemEquipment::InitializeItem` (0x327E20) is not proven idempotent either - it re-reads four
// records through LoadTableFile and rebuilds derived stats over whatever is already there.
//
// So the mod does exactly what the ENGINE does for a brand-new node (AddItemToReagents
// 0x2CECC4): build a replica, hand it to `Item::CreateItem`, `SetMaxStackSize(-1)` (vt+0x620,
// 0x2CEE4A), `SetStackSize(1)`, and point the node at the new object. Nothing is initialised
// twice because nothing existed before.
//
// The OLD prototype is deliberately LEFT ALIVE - never DestroyObjectEx'd - and that is what makes
// the swap safe. Destroying it is the dangerous half: a box could then hold a dead id, and the
// window destructor could double-free a build-time id. Neither risk exists without a destroy.
// The price is two live Items per record - ONE leaked Item per re-deposit - which the log names
// by id so the count is visible. A dangling std::string inside the capture is a separate risk,
// and is exactly what the capUsable + identityBuild gate below is for; it is the same gate the
// take path uses.
//
// The replica handed over is NEVER the raw capture: the deposited item is destroyed by the time
// this runs, so its heap std::strings are freed. It is utBuildSwapOverlay's output (identityBuild
// plus a zeroed object id and stack mirror 1), whose every slot points at storage inside the
// overlay and which carries the NULL-shape refusal.
unsigned char g_refreshIncoming[0x200];    // game thread only
UtIdentityOverlay* g_refreshOverlay = nullptr;  // 4 KB, allocated once in reagentInit

// The module a pointer belongs to must be Game.dll's .text before we call through it.
bool moduleTextRange(HMODULE m, const unsigned char** lo, const unsigned char** hi) {
    if (!m) return false;
    const unsigned char* base = (const unsigned char*)m;
    __try {
        const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
        const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            if (memcmp(sec[i].Name, ".text", 5) == 0) {
                *lo = base + sec[i].VirtualAddress;
                *hi = *lo + sec[i].Misc.VirtualSize;
                return true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return false;
}

// A vtable slot read out of a LIVE object and verified to lie inside Game.dll's .text - never
// assumed, never hard-coded. The slot contents differ by class: ItemRelic overrides +0x008 /
// +0x3F0 / +0x5F8 where ItemEquipment does not, so nothing may be assumed about any slot on any
// class.
const void* itemVtableSlot(const GdItem* obj, unsigned int byteOff) {
    if (!obj) return nullptr;
    const void* fn = nullptr;
    __try {
        const void* const* vft = *(const void* const* const*)obj;
        if (!vft) return nullptr;
        fn = vft[byteOff / 8];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    if (!fn) return nullptr;
    const unsigned char *lo = nullptr, *hi = nullptr;
    if (!moduleTextRange(GetModuleHandleA("Game.dll"), &lo, &hi)) return nullptr;
    const unsigned char* p = (const unsigned char*)fn;
    return (p >= lo && p < hi) ? fn : nullptr;
}

// The engine factory call, in its own SEH frame. Never swallowed: a fault is LOGGED and the
// filter then returns EXCEPTION_CONTINUE_SEARCH, so the engine's own handler runs exactly as it
// would have. o_ItemCreateItem is the TRAMPOLINE deliberately -
// the mod's own hk_ItemCreateItem has nothing to add to a creation the mod itself is making, and
// routing through it would put the take-time identity state machine on this frame for no reason.
GdItem* swapCreateItem(const void* replica) {
    GdItem* fresh = nullptr;
    __try {
        fresh = o_ItemCreateItem ? o_ItemCreateItem(replica) : nullptr;
    } __except (logEngineFault("the reagent prototype swap (Item::CreateItem)",
                               GetExceptionInformation()->ExceptionRecord, GetCurrentThreadId()),
                EXCEPTION_CONTINUE_SEARCH) {
        fresh = nullptr;  // never reached: the filter always continues the search
    }
    return fresh;
}

// Item::SetMaxStackSize(-1) through the NEW object's own vtable slot +0x620 - the same call the
// engine makes on every stored prototype (ReadPlayerReagents+0x24E, AddItemToReagents 0x2CEE4A).
// Same rule: log, then continue the search.
bool swapSetMaxStack(GdItem* fresh, const void* fn) {
    typedef void(__cdecl * PfnSetMaxStack)(GdItem*, int);
    bool ok = false;
    __try {
        ((PfnSetMaxStack)fn)(fresh, -1);
        ok = true;
    } __except (logEngineFault("the reagent prototype swap (Item::SetMaxStackSize)",
                               GetExceptionInformation()->ExceptionRecord, GetCurrentThreadId()),
                EXCEPTION_CONTINUE_SEARCH) {
        ok = false;  // never reached
    }
    return ok;
}

// Reads the base record out of an overlay THIS PROCESS built (never engine memory) and compares
// it with the node key, case-folded. Load-bearing: CreateObjectFromFile builds the object from
// replica+0x08, so a mismatch would create a DIFFERENT item whose Object::GetObjectName no longer
// matches the map key the node is filed under.
bool overlayRecordIs(const UtIdentityOverlay* ov, const std::string& key) {
    char text[256];
    text[0] = 0;
    if (!utReadMsvcString(ov->replica, 0x08, ov->replicaLen, text, sizeof(text))) return false;
    std::string got(text);
    toLower(&got);
    return got == key;
}

// The rescue command's half: build the same substitute over the STORED PROTOTYPE's own replica,
// in place. Returns true when `replica` now carries the deposited item's identity.
bool identityForRescue(const std::string& key, unsigned char* replica, unsigned int len,
                       const char** used) {
    *used = "stock copy";
    if (!g_cfg.identity || !g_idEntry || !g_idOverlay) return false;
    if (InterlockedCompareExchange(&g_idOff, 0, 0)) return false;
    if (!journalGet(key.c_str(), g_idEntry)) return false;
    if (!identityBuild(*g_idEntry, replica, len, g_replicaStackOff, g_idOverlay)) {
        logD("identity: %s could not be restored from the journal (%s) - the stored prototype's "
             "own replica is used instead",
             key.c_str(), g_idOverlay->why);
        return false;
    }
    memcpy(replica, g_idOverlay->replica, len);
    *used = "identity";
    InterlockedIncrement(&g_idRescueRestores);
    return true;
}

// ---- the rescue command ----------------------------------------------------------------------
struct RescueLine {
    std::string record;
    int restored;
    const char* where;
};

// ---- the rescue, ONE copy of one record -------------------------------------------------------
// The same sequence the exe's own take runs, with the two ends changed: the item is built from
// the JOURNAL (there is no engine prototype to copy a replica off) and the row is decremented
// instead of `TakeItemFromReagents`.
//
// The ORDER is the point: the room check comes first and the decrement is LAST, so a full
// inventory costs the collection nothing. Returns 1 = restored, 0 = the inventory is full (the
// engine's own answer - stop and say so), -1 = a hard failure for this record.
int restoreOneFromTable(const std::string& key, char* why, size_t whyCap) {
    GdPlayer* player = safeMainPlayer();
    if (!player) {
        _snprintf_s(why, whyCap, _TRUNCATE, "no main player");
        return -1;
    }
    const char* bwhy = "?";
    const unsigned int freshId = reagentBuildIdentityProto(key.c_str(), 1, &bwhy);
    GdItem* fresh = freshId ? findItemById(freshId) : nullptr;
    if (!fresh || !itemIsLive(fresh, freshId)) {
        _snprintf_s(why, whyCap, _TRUNCATE,
                    "the private table could not build this item out of the journal (%s) - the "
                    "row is UNTOUCHED and the identity is still in uniq-items.jsonl",
                    bwhy ? bwhy : "?");
        return -1;
    }
    bool room = false;
    __try {
        room = p_IsInventorySpaceAvailable(player, fresh);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // A faulted room check is a hard failure with its own name, never "the inventory is
        // full". Nothing was decremented either way.
        room = false;
        _snprintf_s(why, whyCap, _TRUNCATE, "the room check faulted");
        return -1;
    }
    if (!room) {
        __try {
            p_PlayInventoryFullSound(player);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        // The built item is left ALIVE and un-given, exactly like the old prototype after a
        // re-deposit swap: the world owns it and the world's teardown takes it. Nothing was
        // decremented, so the collection still holds the copy.
        _snprintf_s(why, whyCap, _TRUNCATE,
                    "the inventory is FULL (nothing was taken out of the private table)");
        return 0;
    }
    bool placed = false;
    __try {
        const unsigned int ctrlId = p_GetControllerId(player);
        void* om = p_ObjectManagerGet();
        void* ctrl = (om && ctrlId) ? p_ObjectFromId(om, ctrlId) : nullptr;
        if (ctrl) {
            p_SendAddItemToInventory(ctrl, freshId);
            placed = true;
        }
        p_GiveItemToCharacter(player, fresh, false, false);
        p_PlayDropSound(fresh);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        _snprintf_s(why, whyCap, _TRUNCATE, "the placement call faulted");
        return -1;
    }
    if (!placed) {
        _snprintf_s(why, whyCap, _TRUNCATE,
                    "no ControllerCharacter for the main player - the item was created but not "
                    "sent to the inventory (the private table was NOT decremented)");
        return -1;
    }
    // LAST, and only now: the player has the item, so the collection may lose it.
    const unsigned int before = storeCount(key.c_str());
    const unsigned int left = storeOnTake(key.c_str(), 1);
    // The gate above is the row's own count, so this branch runs with a READ-ONLY journal too -
    // which is the point, because those rows exist nowhere else. But a decrement that did not
    // take is a DUPLICATE waiting for the next rescue: the item is in the player's bags AND the
    // row still
    // says 1. It cannot be undone here (the item has been handed over; taking it back is not
    // something this mod does), so it is measured and said out loud, once per record.
    const unsigned int after = storeCount(key.c_str());
    if (after >= before && before > 0) {
        logW("rescue: %s is in your bags but its count did not go down (%u -> %u) - do NOT "
             "rescue again until the file is writable", key.c_str(), before, after);
        logD("the file is probably read-only, so the row cannot be marked taken on disk; the item "
             "is safe, but a second rescue in a new session would hand you a second copy");
        _snprintf_s(why, whyCap, _TRUNCATE,
                    "the character's inventory - restored (identity, from the private table) BUT "
                    "the row could not be decremented (%u -> %u): see the log, do not run rescue "
                    "twice",
                    before, after);
        return 1;
    }
    _snprintf_s(why, whyCap, _TRUNCATE,
                "the character's inventory - restored (identity, from the private table; %u "
                "left in the table, the journal entry is kept as history)",
                left);
    return 1;
}

// ONE copy of `key` back into the character's inventory. The mod's own file is the only place
// a copy of one of our records can be, so there is exactly one route: `restoreOneFromTable`.
// A row at count 0 is history, not an item, and is refused.
int restoreOne(const std::string& key, char* why, size_t whyCap) {
    if (storeCount(key.c_str()) >= 1) return restoreOneFromTable(key, why, whyCap);
    _snprintf_s(why, whyCap, _TRUNCATE,
                "the mod's own file holds no copy of this record (count 0), and nothing of the "
                "mod's is ever in the game's own reagent map");
    return -1;
}

void runRescue() {
    InterlockedIncrement(&g_rescueRuns);
    logI("=== RESCUE: returning every stored item to the character's inventory ===");
    logD("rescue: take-path mirror = %s (%s)", g_takeMirrorOk ? "ARMED" : "DISABLED",
         g_takeMirrorWhy);
    std::string report;
    int restored = 0, failed = 0, full = 0, records = 0;
    char header[512];
    SYSTEMTIME st;
    GetLocalTime(&st);
    _snprintf_s(header, sizeof(header), _TRUNCATE,
                "uniq rescue report  %04u-%02u-%02u %02u:%02u:%02u\r\n"
                "take path: the exe's own sequence, mirrored with exports only\r\n"
                "  GetMainPlayer / IsInventorySpaceAvailable / GetItemReplicaInfo /\r\n"
                "  Item::CreateItem / GetControllerId / SendAddItemToInventory /\r\n"
                "  GiveItemToCharacter / PlayDropSound / TakeItemFromReagents(string,int)\r\n"
                "mirror check: %s\r\n\r\n"
                "record                                                       n  where\r\n"
                "-------------------------------------------------------------------------\r\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, g_takeMirrorWhy);
    try {
        report.assign(header);
    } catch (...) {
    }
    if (!g_takeMirrorOk) {
        logW("rescue: REFUSED - %s. Nothing was touched.", g_takeMirrorWhy);
        try {
            report += "REFUSED: the take-path mirror is disabled. Nothing was touched.\r\n";
            rescueReportWrite(report.c_str(), report.size());
        } catch (...) {
        }
        return;
    }
    // The LAST door the paint gate does not cover. A rescue hands the collection's items back to
    // the character in the world, so running it while the mod does not know which of the two
    // collections is his would hand him the OTHER mode's items - a duplicate made deliberately,
    // a whole collection at a time. The table's rows are read through
    // `storeTableOwns()` below, which is false in that state, so the run would silently restore
    // nothing anyway; this says so instead.
    if (!journalModeKnown()) {
        logW("rescue: REFUSED - the mod does not know yet whether this character is HARDCORE or "
             "SOFTCORE, and it keeps one collection per mode. Nothing was touched.");
        logD("rescue: the log's \"collection: mode =\" line says why the mode is not "
             "known; the collection is not painted in that state either");
        try {
            report +=
                "REFUSED: the hardcore/softcore mode is not known, so the mod cannot tell which\r\n"
                "collection belongs to this character. Nothing was touched.\r\n";
            rescueReportWrite(report.c_str(), report.size());
        } catch (...) {
        }
        return;
    }
    std::map<std::string, int> held;
    // ---- the collection's rows ARE the rescue -------------------------------------------------
    // Without this the report reads "restored 0 of 0" over a full collection and looks like
    // success. The walk is UNCONDITIONAL, never gated on the file being writable: a rescue run
    // with a READ-ONLY journal would otherwise skip every row the FILE is the only copy of and
    // tell the user nothing of ours is held. `journalCollectStored` returns only rows at
    // count >= 1, and the log line and the report line below print only when there IS something
    // to say.
    int tableRows = 0, tableCopies = 0;
    {
        static char recs[512][256];
        static unsigned int counts[512];
        const int n = journalCollectStored(recs, counts, 512);
        for (int i = 0; i < n && i < 512; ++i) {
            if (!recs[i][0] || !counts[i]) continue;
            try {
                std::string key(recs[i]);
                toLower(&key);
                held[key] += (int)(counts[i] > 64u ? 64u : counts[i]);
                ++tableRows;
                tableCopies += (int)counts[i];
            } catch (...) {
            }
        }
    }
    if (tableRows > 0) {
        logI("rescue: the private table holds %d record%s (%d cop%s)", tableRows,
             tableRows == 1 ? "" : "s", tableCopies, tableCopies == 1 ? "y" : "ies");
        try {
            char line[320];
            _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "private table: %d records, %d copies\r\n\r\n", tableRows, tableCopies);
            report += line;
        } catch (...) {
        }
    }
    for (std::map<std::string, int>::const_iterator it = held.begin(); it != held.end(); ++it) {
        if (it->second <= 0) continue;
        ++records;
        int gotBack = 0;
        char why[256] = {0};
        for (int i = 0; i < it->second && i < 64; ++i) {
            const int r = restoreOne(it->first, why, sizeof(why));
            if (r == 1) {
                ++gotBack;
                ++restored;
                continue;
            }
            if (r == 0) ++full;
            else ++failed;
            break;
        }
        if (gotBack > 0) {
            InterlockedAdd(&g_rescueRestored, gotBack);
            // The journal IS the collection, so the row is never dropped: `restoreOneFromTable`
            // has already decremented it, once per copy it handed back, and the entry stays as
            // history. Dropping it here would throw away the identity of an item the user has
            // just been handed and may put straight back. The row's own COUNT decides, never a
            // mode byte: if the decrement did not take (a READ-ONLY journal -
            // `restoreOneFromTable` logs that in capitals), the row still claims a copy, and
            // dropping it would delete the identity of an item the table still believes it holds.
            setTrackedCount(it->first, 0);
            try {
                if (g_rescueEmptied) g_rescueEmptied->insert(it->first);
            } catch (...) {
            }
        } else {
            InterlockedIncrement(&g_rescueFailed);
        }
        logD("rescue: %s -> %d restored (%s)", it->first.c_str(), gotBack, why);
        try {
            char line[512];
            _snprintf_s(line, sizeof(line), _TRUNCATE, "%-60s %2d  %s\r\n", it->first.c_str(),
                        gotBack, why);
            report += line;
        } catch (...) {
        }
        if (full) break;  // the engine says there is no room; stop rather than churn
    }
    // Verify: nothing of ours may still be held. The mod's own file is the only authority, so
    // the check is one read of it.
    int leftRecords = 0, leftItems = 0;
    {
        static char recs2[512][256];
        static unsigned int counts2[512];
        const int n2 = journalCollectStored(recs2, counts2, 512);
        for (int i = 0; i < n2 && i < 512; ++i) {
            if (!recs2[i][0] || !counts2[i]) continue;
            ++leftRecords;
            leftItems += (int)counts2[i];
        }
    }
    char tail[640];
    _snprintf_s(tail, sizeof(tail), _TRUNCATE,
                "\r\n%d record%s walked, %d item%s restored, %d failed.\r\n"
                "%s\r\n"
                "verification: %s\r\n"
                "journal after the run: %zu entr%s in %s\r\n",
                records, records == 1 ? "" : "s", restored, restored == 1 ? "" : "s", failed,
                full ? "STOPPED: the character's inventory is FULL. Make room and set rescue=1 "
                       "again - the run continues where it stopped."
                     : "The run completed.",
                leftRecords ? "ITEMS STILL STORED - re-run rescue=1"
                            : "the private table holds none of our records",
                journalCount(), journalCount() == 1 ? "y" : "ies", journalPath());
    try {
        report += tail;
        rescueReportWrite(report.c_str(), report.size());
    } catch (...) {
    }
    logI("rescue: %d record(s) walked, %d item(s) restored, %d failed%s", records, restored,
         failed, full ? " - STOPPED, THE INVENTORY IS FULL" : "");
    logI("rescue: verification %s (%d record(s) / %d item(s) still stored)",
         leftRecords ? "INCOMPLETE" : "clean", leftRecords, leftItems);
    logI("rescue: report written to %s", rescueReportPath());
    logI("=== RESCUE END ===");
}

// Game thread, once a frame. `rescue=1` in the ini is a one-shot command: it runs when the
// caravan window is open and a character is loaded, then writes rescue=0 back.
void rescueTick() {
    if (!g_cfg.rescue) {
        g_rescueSeen = 0;
        return;
    }
    if (g_rescueSeen) return;  // already handled this arming
    if (!InterlockedCompareExchange(&g_transferOpenNow, 0, 0)) {
        static DWORD lastNag = 0;
        const DWORD now = GetTickCount();
        if (now - lastNag > 5000) {
            lastNag = now;
            logI("rescue=1 is set: OPEN THE CARAVAN (transfer stash) window and the mod will hand "
                 "every stored item back.");
        }
        return;
    }
    if (!g_gameEngine || !g_gd.GameGetMainPlayer) return;
    if (!safeMainPlayer()) return;
    g_rescueSeen = 1;
    try {
        runRescue();
    } catch (...) {
        logE("rescue: aborted (allocation failure)");
    }
    configPersistInt("rescue", 0);
    g_cfg.rescue = 0;
}

// `journal_prune=1` is a ONE-SHOT command, exactly like `rescue=1`, and it is
// the only path in the mod that deliberately removes journal entries. It drops ONLY entries
// marked "stored":false - never an UNKNOWN one - the whole file is copied to
// <journal>.pruned-<stamp> before the first entry goes, every dropped record is written to the
// log, and the key is set back to 0 so it can never run twice by accident.
//
// The two arguments are what makes the safety argument true.
// `markedFalse` counts the entries THIS call marked, not the file-wide "notStored" count -
// that one also counts marks read off DISK for entries this very reconciliation declined to look
// at, i.e. exactly the entries the failure ladder exists to protect. `inconclusive` is how many
// entries the ladder left alone (not one of our page records, a partial walk, a variant
// mismatch); a prune is REFUSED while it is non-zero, because a file the mod could not fully
// assess this session is not a file to delete from.
void journalPruneTick(size_t markedFalse, size_t inconclusive) {
    if (!g_cfg.journalPrune) return;
    configPersistInt("journal_prune", 0);
    g_cfg.journalPrune = 0;
    if (inconclusive > 0) {
        logW("***** journal_prune REFUSED - %zu entr%s could not be checked against the engine's "
             "map; nothing was dropped and the key is back to 0 *****",
             inconclusive, inconclusive == 1 ? "y" : "ies");
        logD("a prune is only ever allowed on a reconciliation that reached a verdict on EVERY "
             "entry: an unchecked one is not one of our page records, an incomplete map walk, or "
             "a different shared stash");
        return;
    }
    if (markedFalse == 0) {
        logI("journal_prune: nothing to drop - the key is back to 0");
        return;
    }
    // ---- THE PRUNE MAY NOT RUN WHILE THE COLLECTION HOLDS ANYTHING ---------------------------
    // `journalPruneNotStored` drops an entry on `stored == UT_STORED_NO` alone - it never looks
    // at the row's count (ut_rescue.cpp's `struct Entry` carries no count member). A row that was
    // wrongly marked "stored":false by anything at all would therefore be DELETED, and for a row
    // the private table owns the file is the only copy of the item.
    //
    // So this caller refuses the whole prune while any row still has a count, and names the file.
    // It costs nothing on a collection that holds nothing: `journalCollectStored` returns 0 rows
    // there and the prune runs normally.
    {
        static char liveRecs[512][256];
        static unsigned int liveCounts[512];
        const int liveRows = journalCollectStored(liveRecs, liveCounts, 512);
        if (liveRows > 0) {
            logW("***** journal_prune REFUSED - the collection still holds %d record(s); take "
                 "them out with rescue=1 first *****", liveRows);
            logD("nothing was dropped and the key is back to 0: the prune cannot yet tell a row "
                 "the file is the only copy of (first: %s x%u) from an entry the engine's map "
                 "lost", liveRecs[0], liveCounts[0]);
            return;
        }
    }
    const int cap = 4096;
    std::vector<char> names;
    try {
        names.resize((size_t)cap * 256);
    } catch (...) {
        logW("rescue journal: journal_prune=1 but the record list could not be allocated - "
             "NOTHING was dropped");
        return;
    }
    char (*out)[256] = (char(*)[256])&names[0];
    char why[MAX_PATH + 128] = {0};
    const int n = journalPruneNotStored(out, cap, why, sizeof(why));
    if (n < 0) {
        logW("***** rescue journal: journal_prune REFUSED - %s *****", why);
        return;
    }
    for (int i = 0; i < n && i < cap; ++i) {
        logD("rescue journal: PRUNED %s - it was in the journal, it is not in the collection, and "
             "the copy taken before this prune still has it",
             out[i]);
    }
    logI("journal_prune: PRUNED %d entr%s the collection no longer holds - the key is back to 0",
         n, n == 1 ? "y" : "ies");
    logD("%s. This is the only thing in the mod that removes an entry, and it ran because "
         "journal_prune=1 was set", why);
}

// Journal reconciliation, on every caravan open. `stored` is a restatement of `count >= 1`:
// the mod's own file is the only place one of our records can be, so a row with a count is
// stored and a row at 0 is history. Marking loses no data - the entry, the replica and every
// slot stay in the file - but the mark is what `journal_guard.ps1` reads, so `journal=0` returns
// before anything is read or marked.
void journalReconcile() {
    if (!g_cfg.journal) return;
    const int cap = 4096;
    std::vector<char> scratch;
    try {
        scratch.resize((size_t)cap * 256);
    } catch (...) {
        return;
    }
    char (*records)[256] = (char(*)[256])&scratch[0];
    const int n = journalRecords(records, cap);
    int markedFalse = 0;
    int tableOwnedRows = 0;
    // A ROW THE TABLE OWNS IS THE ONLY KIND THERE IS. `stored` and `count >= 1` say the same
    // thing now: the mod's own file is the only place a copy of one of our records can be, so
    // the reconcile is one pass over the rows that writes the mark the count implies.
    for (int i = 0; i < n; ++i) {
        if (storeCount(records[i]) >= 1) {
            journalMarkStored(records[i], true);
            ++tableOwnedRows;
        } else {
            journalMarkStored(records[i], false);
            ++markedFalse;
        }
    }
    size_t total = 0, stored = 0, notStored = 0, unknown = 0;
    journalCounts(&total, &stored, &notStored, &unknown);
    logD("rescue journal: reconciled - %d row%s the table owns (count >= 1), %d entr%s marked "
         "\"stored\":false because the count is 0; the file now says %zu entries / %zu stored / "
         "%zu not stored / %zu not yet reconciled - %s",
         tableOwnedRows, tableOwnedRows == 1 ? "" : "s", markedFalse,
         markedFalse == 1 ? "y" : "ies", total, stored, notStored, unknown, journalPath());
    if (notStored > 0) {
        logD("rescue journal: %zu entr%s in the file %s NOT in the collection any more. They are "
             "kept on purpose - each one is the only record of what that item was - and they are "
             "written at the END of the file, after the %zu that are stored. Set journal_prune=1 "
             "in uniquetab.ini and re-open the caravan to drop them (the whole file is copied to "
             "<journal>.pruned-<stamp> first).",
             notStored, notStored == 1 ? "y is" : "ies are", notStored == 1 ? "is" : "are",
             stored);
    }
    // The prune sees only what THIS call decided - never the file-wide notStored, which includes
    // marks read off disk for entries this reconciliation skipped.
    journalPruneTick((size_t)markedFalse, 0);
}

}  // namespace

bool reagentInit(HMODULE selfModule) {
    if (g_inited) return true;
    g_inited = true;
    InitializeCriticalSection(&g_cs);
    g_csReady = true;
    InitializeCriticalSection(&g_mpCs);  // the bag watchdog's own lock
    g_mpCsReady = true;
    g_pageRecords = new std::unordered_set<std::string>();
    g_pages = new std::vector<UniqPage>();
    g_registry = new std::vector<RegEntry>();
    g_registrySet = new std::unordered_set<GdItem*>();
    g_byId = new std::unordered_map<unsigned int, GdItem*>();
    g_idOf = new std::unordered_map<GdItem*, unsigned int>();
    g_counts = new std::map<std::string, int>();
    g_rescueEmptied = new std::unordered_set<std::string>();
    // The ini's journal_dir is handed over HERE and nowhere else - journalInit reads it once and
    // never again (configReload replaces g_cfg once a second).
    journalInit(selfModule, g_cfg.journalDir);
    modBuildStampInit(selfModule);

    logD("reagent page: resolving exports");
    HMODULE game = g_gd.gameDll;
    HMODULE eng = g_gd.engineDll;

    p_IsReagentCompatible =
        (PfnItem_IsReagentCompatible)proc(game, GD_ITEM_ISREAGENTCOMPATIBLE,
                                          "Item::IsReagentCompatible");
    t_ItemLoad = proc(game, GD_ITEM_LOAD, "Item::Load");
    t_AddItemToReagents = proc(game, GD_GAMEENGINE_ADDITEMTOREAGENTS,
                               "GameEngine::AddItemToReagents");
    t_TakeFromReagents = proc(game, GD_GAMEENGINE_TAKEITEMFROMREAGENTS_NAME,
                              "GameEngine::TakeItemFromReagents");
    t_DepositReagents = proc(game, GD_PLAYERINVCTRL_DEPOSITREAGENTS,
                             "PlayerInvCtrl::DepositReagents");
    t_DepositSack = proc(game, GD_SACK_DEPOSITSACKINTOREAGENTS, "Sack::DepositSackIntoReagents");
    t_DepositTransfer = proc(game, GD_GAMEENGINE_DEPOSITTRANSFERREAGENTS,
                             "GameEngine::DepositTransferReagents");
    t_DestroyObjectEx = proc(eng, GD_OBJECTMANAGER_DESTROYOBJECTEX,
                             "ObjectManager::DestroyObjectEx");
    t_LoadMainDatabase = proc(eng, GD_ENGINE_LOADMAINDATABASE, "Engine::LoadMainDatabase");
    p_LoadDatabase = (PfnEngine_LoadDatabase)proc(eng, GD_ENGINE_LOADDATABASE,
                                                  "Engine::LoadDatabase");
    t_LoadDatabase = (void*)p_LoadDatabase;  // detoured too: see hk_LoadDatabase
    p_GetDbChecksum = (PfnEngine_GetDbChecksum)proc(eng, GD_ENGINE_GETDATABASEARCHIVECHECKSUM,
                                                    "Engine::GetDatabaseArchiveChecksum");
    p_HasCustomDb = (PfnEngine_HasLoadedCustomDatabase)proc(eng, GD_ENGINE_HASLOADEDCUSTOMDATABASE,
                                                            "Engine::HasLoadedCustomDatabase");
    p_SyncCaravanReagents = (PfnGE_Void)proc(game, GD_GAMEENGINE_SYNCCARAVANREAGENTS,
                                             "GameEngine::SyncCaravanReagents");
    p_ObjectManagerGet = (PfnObjectManagerGet)proc(eng, GD_SINGLETON_OBJECTMANAGER_GET,
                                                   "Singleton<ObjectManager>::Get");
    p_LoadTableFile = (PfnOM_LoadTableFile)proc(eng, GD_OBJECTMANAGER_LOADTABLEFILE,
                                                "ObjectManager::LoadTableFile");
    t_LoadTableFileHook = (void*)p_LoadTableFile;
    t_GetLoadTable = proc(eng, GD_OBJECTMANAGER_GETLOADTABLE, "ObjectManager::GetLoadTable");
    p_GetNumElementsForField =
        (PfnLTB_GetNumElementsForField)proc(eng, GD_LOADTABLEBINARY_GETNUMELEMENTSFORFIELD,
                                            "LoadTableBinary::GetNumElementsForField");
    p_LoadTableBinaryVft = proc(eng, GD_LOADTABLEBINARY_VFTABLE, "LoadTableBinary vftable");
    p_IsSoulbound = (PfnItem_IsReagentCompatible)proc(game, GD_ITEM_ISSOULBOUND,
                                                      "Item::IsSoulbound");
    p_IsUntradeable = (PfnItem_IsReagentCompatible)proc(game, GD_ITEM_ISUNTRADEABLE,
                                                        "Item::IsUntradeable");
    t_PrimaryReagentActivate = proc(game, GD_CURSORITEMMOVE_PRIMARYREAGENTACTIVATE,
                                    "CursorHandlerItemMove::PrimaryReagentActivate");
    t_QuickDropInReagents = proc(game, GD_CURSORITEMMOVE_QUICKDROPINREAGENTS,
                                 "CursorHandlerItemMove::QuickDropInReagents");

    // ---- the take path ---------------------------------------------------------------------
    t_TakeFromReagentsId = proc(game, GD_GAMEENGINE_TAKEITEMFROMREAGENTS_ID,
                                "GameEngine::TakeItemFromReagents(id)");
    t_Cancel = proc(game, GD_CURSORITEMMOVE_CANCEL, "CursorHandlerItemMove::Cancel");
    t_ItemCreateItem = proc(game, GD_ITEM_CREATEITEM, "Item::CreateItem");
    t_CreateItemInInventory = proc(game, GD_CONTROLLERCHAR_CREATEITEMININVENTORY,
                                   "ControllerChar::CreateItemInInventory");
    t_CreateItemForCharacter = proc(game, GD_GAMEENGINE_CREATEITEMFORCHARACTER,
                                    "GameEngine::CreateItemForCharacter");
    p_GetReagentItemCount = (PfnGE_GetReagentItemCount)proc(game, GD_GAMEENGINE_GETREAGENTITEMCOUNT,
                                                            "GameEngine::GetReagentItemCount");
    void* p_GetReplica = proc(game, GD_ITEM_GETITEMREPLICAINFO, "Item::GetItemReplicaInfo");
    p_GetSeedRerolls = (PfnItem_GetU32)proc(game, GD_ITEM_GETSEEDREROLLS, "Item::GetSeedRerolls");
    p_GetAffixRerolls = (PfnItem_GetU32)proc(game, GD_ITEM_GETAFFIXREROLLS,
                                             "Item::GetAffixRerolls");
    p_GetPrefixClass = (PfnItem_GetU32)proc(game, GD_ITEM_GETPREFIXCLASSIFICATION,
                                            "Item::GetPrefixClassification");
    p_GetSuffixClass = (PfnItem_GetU32)proc(game, GD_ITEM_GETSUFFIXCLASSIFICATION,
                                            "Item::GetSuffixClassification");

    // ---- the liveness check, the session mode and the collection mode ----------------------
    p_IsObjectOnDeletedList =
        (PfnOM_IsObjectOnDeletedList)proc(eng, GD_OBJECTMANAGER_ISOBJECTONDELETEDLIST,
                                          "ObjectManager::IsObjectOnDeletedList");
    p_IsObjectIdOnDeletedList =
        (PfnOM_IsObjectIdOnDeletedList)proc(eng, GD_OBJECTMANAGER_ISOBJECTIDONDELETEDLIST,
                                            "ObjectManager::IsObjectIdOnDeletedList");
    p_GetGameInfo = (PfnEngine_GetGameInfo)proc(eng, GD_ENGINE_GETGAMEINFO, "Engine::GetGameInfo");
    p_GetIsMultiPlayer =
        (PfnGameInfo_GetIsMultiPlayer)proc(eng, GD_GAMEINFO_GETISMULTIPLAYER,
                                           "GameInfo::GetIsMultiPlayer");
    // OPTIONAL telemetry. A miss disables nothing - the heartbeat and the edge line print -1 for
    // that field. Never a gate: gating on a peer count would flip the mod off mid-deposit the
    // moment a friend joined.
    p_GetIsServer = (PfnGameInfo_GetBool)proc(eng, GD_GAMEINFO_GETISSERVER,
                                              "GameInfo::GetIsServer (optional)");
    p_GetNumOfPlayers = (PfnGameInfo_GetUInt)proc(eng, GD_GAMEINFO_GETNUMOFPLAYERS,
                                                  "GameInfo::GetNumOfPlayers (optional)");
    p_GetGameMode = (PfnGameInfo_GetUInt)proc(eng, GD_GAMEINFO_GETMODE,
                                              "GameInfo::GetMode (optional)");
    // THE collection mode - which of the two shared stashes this character's is. Read
    // through the same GameInfo pointer as the multiplayer flag above, same const-bool shape.
    p_GetHardcore = (PfnGameInfo_GetBool)proc(eng, GD_GAMEINFO_GETHARDCORE,
                                              "GameInfo::GetHardcore");
    // The three ENGINE-state getters. They are what actually says whether an ActorConfigCommand
    // round-trips in this session; GetIsMultiPlayer and GetNumOfPlayers are lobby facts.
    // OPTIONAL and never a gate - a miss prints net=?.
    p_IsNetEnabled = (PfnGameInfo_GetBool)proc(eng, GD_ENGINE_ISNETWORKENABLED,
                                               "Engine::IsNetworkEnabled (optional)");
    p_IsNetServer = (PfnGameInfo_GetBool)proc(eng, GD_ENGINE_ISNETWORKSERVER,
                                              "Engine::IsNetworkServer (optional)");
    p_IsNetClient = (PfnGameInfo_GetBool)proc(eng, GD_ENGINE_ISNETWORKCLIENT,
                                              "Engine::IsNetworkClient (optional)");
    t_ExitPlayingMode = proc(game, GD_GAMEENGINE_EXITPLAYINGMODE, "GameEngine::ExitPlayingMode");
    try {
        if (!g_mapProtoIds) g_mapProtoIds = new std::unordered_set<unsigned int>();
    } catch (...) {
        g_mapProtoIds = nullptr;
    }

    // ---- the stacking fix --------------------------------------------------------------------
    p_GetPlayerReagents = (PfnGE_GetPlayerReagents)proc(game, GD_GAMEENGINE_GETPLAYERREAGENTS,
                                                        "GameEngine::GetPlayerReagents");
    p_GetStackSize = (PfnItem_GetU32)proc(game, GD_ITEM_GETSTACKSIZE_R, "Item::GetStackSize");
    p_SetStackSize = (PfnItem_SetU32)proc(game, GD_ITEM_SETSTACKSIZE, "Item::SetStackSize");
    p_EquipIncrementStub = proc(game, GD_ITEMEQUIPMENT_INCREMENTSTACK,
                                "ItemEquipment::IncrementStack (read only: the no-op stub)");
    proc(game, GD_ITEM_INCREMENTSTACK, "Item::IncrementStack (read only: the real one)");
    moduleRange(GetModuleHandleW(nullptr), &g_exeBase, &g_exeSize);
    moduleRange(game, &g_gameBase, &g_gameSize);
    if (!p_IsObjectOnDeletedList || !p_ObjectManagerGet) {
        InterlockedExchange(&g_gateForcedOff, 1);
        logE("collection: FORCED OFF - this game is missing an export the liveness check needs");
        logD("both ObjectManager::IsObjectOnDeletedList and Singleton<ObjectManager>::Get are "
             "required; no craftingMaterial byte is written this session");
    }
    if (!p_GetGameInfo || !p_GetIsMultiPlayer) {
        // The session mode is UNKNOWN for the whole session. With mp_collect=1 that costs
        // nothing (the collection is enabled in every mode anyway); with mp_collect=0 the mod
        // treats every session as multiplayer, which is the safe half of that switch.
        logW("collection: the session mode cannot be read (mp_collect=%d) - the collection is %s",
             g_cfg.mpCollect,
             g_cfg.mpCollect ? "enabled anyway, it works in every mode"
                             : "OFF for this whole session; set mp_collect=1");
        logD("GameInfo::GetIsMultiPlayer is unavailable, and mp_collect=0 treats an unreadable "
             "session mode as co-op");
    }
    if (!p_GetGameInfo || !p_GetHardcore) {
        // Said at resolution time, not at the first world, because this one is fatal to
        // the collection however the session goes: with no hardcore flag the mod cannot tell the
        // hardcore collection from the softcore one, and guessing is the single mistake that
        // writes one character's items into the other's file.
        logW("collection: GameInfo::GetHardcore cannot be read, so the mod cannot tell which of "
             "the two collections is this character's - the collection stays HIDDEN this session "
             "(no box of ours is painted and deposits are refused)");
    }

    g_flagOffset = decodeFlagOffset((const void*)p_IsReagentCompatible);
    if (g_flagOffset) {
        logD("reagent gate: craftingMaterial flag is Item + 0x%X, decoded from "
             "Item::IsReagentCompatible's own bytes (movzx eax,[rcx+disp32]; ret)",
             g_flagOffset);
    } else {
        logE("reagent gate: DISABLED - Item::IsReagentCompatible does not have the expected "
             "shape, so the field offset is unknown. The page will still render.");
    }

    g_soulboundOffset = decodeFlagOffset((const void*)p_IsSoulbound);
    g_untradeableOffset = decodeFlagOffset((const void*)p_IsUntradeable);
    logD("reagent drop conditions: soulbound = Item + 0x%X, untradeable = Item + 0x%X "
         "(both decoded from their getters; PrimaryReagentActivate/QuickDropInReagents refuse "
         "the drop unless BOTH are 0)",
         g_soulboundOffset, g_untradeableOffset);

    g_replicaOffset = decodeReplicaOffset(p_GetReplica);
    decodeReplicaSize(proc(game, GD_ITEM_CTOR, "Item::Item (read only: pins the replica size)"),
                      p_GetReplica, g_gameBase, g_gameSize);
    g_seedRerollsOffset = decodeDwordOffset((const void*)p_GetSeedRerolls);
    g_affixRerollsOffset = decodeDwordOffset((const void*)p_GetAffixRerolls);
    g_prefixClassOffset = decodeDwordOffset((const void*)p_GetPrefixClass);
    g_suffixClassOffset = decodeDwordOffset((const void*)p_GetSuffixClass);
    p_ObjectFromId = (PfnOM_ObjectFromId)decodeCallTarget(t_TakeFromReagentsId, 0x2C);
    // `FF 97 <disp32>` = call qword ptr [rdi + disp32] at AddItemToReagents + 0x1FF: the merge
    // branch's IncrementStack call. Decoded, never typed, exactly like every other offset here.
    {
        const unsigned int slot = decodeVcallSlot(t_AddItemToReagents, 0x1FF);
        g_incrementSlot = slot;  // 0 when not decoded; nothing decides on it either way
        g_incrementSlotDecoded = slot != 0;
        logD("reagent stack fix: Item vtable IncrementStack slot = +0x%X (%s); "
             "ItemEquipment::IncrementStack stub = %p; Item::SetStackSize = %p",
             g_incrementSlot, slot ? "decoded from AddItemToReagents+0x1FF" : "NOT decoded",
             p_EquipIncrementStub, (void*)p_SetStackSize);
        if (!p_GetPlayerReagents || !p_GetStackSize || !p_SetStackSize || !p_EquipIncrementStub) {
            logW("collection: a second copy of a record cannot be stacked in this build - such a "
                 "deposit is refused instead, so no item can be lost");
            logD("one of GetPlayerReagents / GetStackSize / SetStackSize / "
                 "ItemEquipment::IncrementStack is missing");
        }
    }
    logD("reagent collect: ItemReplicaInfo = Item + 0x%X (%u bytes), seedRerolls = Item + 0x%X, "
         "affixRerolls = Item + 0x%X, prefixClass = Item + 0x%X, suffixClass = Item + 0x%X",
         g_replicaOffset, g_replicaSize, g_seedRerollsOffset, g_affixRerollsOffset,
         g_prefixClassOffset, g_suffixClassOffset);
    logD("reagent collect: id->Object helper %s (decoded from TakeItemFromReagents(id,int)+0x2C); "
         "policy: maxPerRecord=%d pristineOnly=%d quickPass=%d reconcile=%d",
         p_ObjectFromId ? "resolved" : "NOT resolved - falling back to the AddItem id map",
         g_cfg.maxPerRecord, g_cfg.collectPristineOnly, g_cfg.collectQuickPass);

    // ---- the identity working set ------------------------------------------------------------
    g_stackMirrorInItem = decodeStackMirrorOffset((const void*)p_SetStackSize);
    if (g_stackMirrorInItem && g_replicaOffset && g_stackMirrorInItem > g_replicaOffset &&
        g_stackMirrorInItem + 4 <= g_replicaOffset + g_replicaSize) {
        g_replicaStackOff = g_stackMirrorInItem - g_replicaOffset;
    } else {
        g_replicaStackOff = 0;
    }
    try {
        if (!g_idEntry) g_idEntry = new UtReplicaCapture();
        if (!g_idOverlay) g_idOverlay = new UtIdentityOverlay();
        if (!g_idIncoming) g_idIncoming = new unsigned char[0x200];
        // The prototype swap has its OWN overlay: it must never share a buffer with the
        // take-time machinery, because a deposit can run while nothing is armed and the two must
        // not be able to corrupt each other.
        if (!g_refreshOverlay) g_refreshOverlay = new UtIdentityOverlay();
    } catch (...) {
        g_idEntry = nullptr;
        g_idOverlay = nullptr;
        g_idIncoming = nullptr;
        g_refreshOverlay = nullptr;
    }
    if (!g_idEntry || !g_idOverlay || !g_idIncoming || !g_replicaOffset) {
        InterlockedExchange(&g_idOff, 1);
        logE("identity: ***** DISABLED ***** - the buffers could not be allocated, or the "
             "replica offset is unknown");
        logD("a take hands back the engine's own copy of the item instead of the one deposited");
    } else {
        logD("identity: armed - the stored ItemReplicaInfo is handed back on a take. Stack "
             "mirror = Item + 0x%X -> replica + 0x%X (%s), replica %u bytes, window %d ms, "
             "call-site check %s",
             g_stackMirrorInItem, g_replicaStackOff,
             g_replicaStackOff ? "decoded from Item::SetStackSize"
                               : "NOT decoded - the journal's own count is kept",
             g_replicaSize, g_cfg.identityWindowMs,
             g_cfg.identityRequireCallsite ? "ENFORCED" : "logged only");
    }

    // Probe (a): before anything of ours has loaded. Normally the Engine object does not exist
    // yet at DLL init and the line says so - which is itself the answer to "was the checksum
    // already different before we touched anything?".
    dbChecksumProbe("a: DLL init, before our database load");

    // ---- the take-path mirror ----------------------------------------------------------------
    p_GetReplicaCall = (PfnItem_GetItemReplicaInfo)p_GetReplica;
    p_IsInventorySpaceAvailable = (PfnPlayer_IsInvSpace)proc(
        game, GD_PLAYER_ISINVENTORYSPACEAVAILABLE, "Player::IsInventorySpaceAvailable");
    p_PlayInventoryFullSound = (PfnPlayer_Void)proc(game, GD_PLAYER_PLAYINVENTORYFULLSOUND,
                                                    "Player::PlayInventoryFullSound");
    p_GetItemMaxStackSize = (PfnGE_GetU32)proc(game, GD_GAMEENGINE_GETITEMMAXSTACKSIZE,
                                               "GameEngine::GetItemMaxStackSize");
    p_GetControllerId = (PfnChar_GetU32)proc(game, GD_CHARACTER_GETCONTROLLERID,
                                             "Character::GetControllerId");
    p_SendAddItemToInventory = (PfnCC_SendAddItem)proc(
        game, GD_CONTROLLERCHAR_SENDADDITEMTOINVENTORY,
        "ControllerCharacter::SendAddItemToInventory");
    p_GiveItemToCharacter = (PfnPlayer_GiveItem)proc(game, GD_PLAYER_GIVEITEMTOCHARACTER,
                                                     "Player::GiveItemToCharacter");
    p_PlayDropSound = (PfnItem_Void)proc(game, GD_ITEM_PLAYDROPSOUND, "Item::PlayDropSound");
    p_ItemVftObject = proc(game, GD_ITEM_VFTABLE_OBJECT, "Item vftable (Object base)");
    p_PlayerVftObject = proc(game, GD_PLAYER_VFTABLE_OBJECT, "Player vftable (Object base)");
    p_AddItemToTransfer = proc(game, GD_GAMEENGINE_ADDITEMTOTRANSFER,
                               "GameEngine::AddItemToTransfer");
    // ---- the BAG PROOF's four read-only accessors, plus the decoded offset --------------------
    // All four are pure readers (see the block by `bagsHoldItem` for the disassembly of each) and
    // none is hooked. When any of them is missing the proof answers -1 and every quick-move
    // deposit into the private table is REFUSED - which is the safe direction: the item stays
    // where it is.
    p_GetInventoryCtrl = (PfnCtrlPlayer_GetInvCtrl)proc(
        game, GD_CONTROLLERPLAYER_GETINVENTORYCTRL, "ControllerPlayer::GetInventoryCtrl");
    p_IcNumberOfSacks = (PfnInvCtrl_NumSacks)proc(game, GD_PLAYERINVCTRL_GETNUMBEROFSACKS,
                                                  "PlayerInventoryCtrl::GetNumberOfSacks");
    p_IcGetSack = (PfnInvCtrl_GetSackAt)proc(game, GD_PLAYERINVCTRL_GETSACK,
                                             "PlayerInventoryCtrl::GetSack");
    p_SackContainsId =
        (PfnSack_ContainsId)proc(game, GD_SACK_CONTAINSITEM_ID, "InventorySack::ContainsItem(id)");
    g_ctrlIdOffset = decodeCtrlIdOffsetBytes(
        proc(game, GD_CURSORHANDLER_GETPLAYERCTRL, "CursorHandler::GetPlayerCtrl (read only)"));
    logD("reagent collect: the bag proof %s - Player+0x%X = the ControllerPlayer object id "
         "(decoded from the export's own bytes), and the three PlayerInventoryCtrl /"
         " InventorySack readers are %s. A bag shift-click is only taken into the private table "
         "while this says the item is in one of the character's own bags",
         (p_GetInventoryCtrl && p_IcNumberOfSacks && p_IcGetSack && p_SackContainsId &&
          g_ctrlIdOffset)
             ? "is ARMABLE"
             : "CANNOT ARM (quick-move deposits will be REFUSED)",
         g_ctrlIdOffset,
         (p_IcNumberOfSacks && p_IcGetSack && p_SackContainsId) ? "resolved" : "INCOMPLETE");
    // ---- every decoded offset reports its outcome to the bindings gate -------------------------
    // These are the facts that are neither a name nor a compiler guarantee, so each one says what
    // it got and whether its own confirmation held. The gate runs before hooksInstall and turns
    // the WHOLE mod off if a single one of them is missing: a decoder that has quietly stopped
    // matching costs the session rather than one feature, which is the safe direction.
    {
        resolveDragSite();
        const bool replicaOk = g_replicaOffset != 0 && g_replicaOffset < 0x2000;
        const unsigned int rlo = g_replicaOffset;
        const unsigned int rhi = g_replicaOffset + g_replicaSize;
        const bool inReplica = replicaOk && g_replicaSize != 0;
        bindingsNote("item.craftingMaterial", g_flagOffset, g_flagOffset != 0 && g_flagOffset < 0x2000,
                     "a non-zero offset below 0x2000 out of Item::IsReagentCompatible");
        bindingsNote("item.soulbound", g_soulboundOffset,
                     g_soulboundOffset != 0 && g_untradeableOffset == g_soulboundOffset + 2 &&
                         g_soulboundOffset < g_flagOffset,
                     "non-zero, exactly two bytes below untradeable, both below craftingMaterial");
        bindingsNote("item.untradeable", g_untradeableOffset,
                     g_untradeableOffset != 0 && g_untradeableOffset == g_soulboundOffset + 2,
                     "non-zero, exactly two bytes above soulbound");
        bindingsNote("item.replicaInfo", g_replicaOffset, replicaOk,
                     "a non-zero offset below 0x2000 out of Item::GetItemReplicaInfo");
        {
            char sizeWhy[192];
            _snprintf_s(sizeWhy, sizeof(sizeWhy), _TRUNCATE,
                        "two agreeing sources, 8-aligned, 0x100..0x200 (Item::Item gave 0x%X, "
                        "ItemReplicaInfo::operator= gave 0x%X)",
                        g_replicaSizeCtor, g_replicaSizeAssign);
            bindingsNote("item.replicaSize", g_replicaSize, g_replicaSize != 0, sizeWhy);
        }
        bindingsNote("item.seedRerolls", g_seedRerollsOffset,
                     inReplica && g_seedRerollsOffset >= rlo && g_seedRerollsOffset < rhi,
                     "an offset inside the replica block");
        bindingsNote("item.affixRerolls", g_affixRerollsOffset,
                     inReplica && g_affixRerollsOffset >= rlo && g_affixRerollsOffset < rhi,
                     "an offset inside the replica block");
        // THESE TWO ARE NOT REPLICA FIELDS. Item::GetPrefixClassification and
        // Item::GetSuffixClassification decode (1.3.0.8, Game.dll rva 0x310040 / 0x310050, both
        // the plain `8B 81 <d32> C3`) to Item + 0x880 and Item + 0x884, which is ABOVE
        // [replicaInfo 0x538, +0x190). seedRerolls 0x6B8 and affixRerolls 0x6B4 really do land
        // inside the block; these never do, so demanding "inside the replica block" of them
        // would turn the whole mod off on a healthy 1.3.0.8. What is asked of them instead is
        // the shape their two getters share - the same kind of pair test soulbound/untradeable
        // get - and neither offset is ever used to read the replica.
        const bool classPair = g_prefixClassOffset != 0 && g_prefixClassOffset < 0x2000 &&
                               g_suffixClassOffset == g_prefixClassOffset + 4;
        bindingsNote("item.prefixClass", g_prefixClassOffset, classPair,
                     "a non-zero Item offset below 0x2000, four bytes below suffixClass");
        bindingsNote("item.suffixClass", g_suffixClassOffset, classPair,
                     "a non-zero Item offset below 0x2000, four bytes above prefixClass");
        bindingsNote("item.incrementStackSlot", g_incrementSlot,
                     g_incrementSlotDecoded && (g_incrementSlot & 7) == 0 &&
                         g_incrementSlot < 0x2000,
                     "an 8-aligned vtable slot below 0x2000 (advisory: nothing decides on it)");
        bindingsNote("item.stackMirror", g_stackMirrorInItem,
                     g_stackMirrorInItem != 0 && inReplica && g_stackMirrorInItem >= rlo &&
                         g_stackMirrorInItem + 4 <= rhi,
                     "a dword offset inside the replica block");
        bindingsNote("player.ctrlId", g_ctrlIdOffset,
                     g_ctrlIdOffset != 0 && g_ctrlIdOffset < 0x20000,
                     "a non-zero offset below 0x20000 out of CursorHandler::GetPlayerCtrl");
        bindingsNote("objectManager.objectFromId", (unsigned long long)(ULONG_PTR)p_ObjectFromId,
                     p_ObjectFromId != nullptr, "a decoded call target inside Game.dll");
    }

    // Entries captured by a game whose ItemReplicaInfo had another size. Said once, here; the
    // take path refuses each such entry on its own when it is asked for.
    if (g_replicaSize) {
        unsigned int otherLen = 0;
        const int other = journalReplicaLengthCensus(g_replicaSize, &otherLen);
        if (other > 0) {
            logW("journal: %d of %zu entries carry a replica of %u bytes but this game's "
                 "ItemReplicaInfo is %u bytes. A take of such an entry is REFUSED - the private "
                 "table cannot build the item from it, the row and the file stay untouched and "
                 "the box keeps the ordinary display prototype - until the item is deposited "
                 "again, which re-captures it at %u bytes. Nothing is rewritten.",
                 other, journalCount(), otherLen, g_replicaSize, g_replicaSize);
        }
    }

    verifyTakeMirror();
    logD("rescue take-mirror: %s - %s. The exe's own reagent take (0x132B10..0x132C1D) is "
         "mirrored with EXPORTS only; the transfer-stash fallback (%p) is resolved but "
         "deliberately never called.",
         g_takeMirrorOk ? "ARMED" : "DISABLED", g_takeMirrorWhy, p_AddItemToTransfer);

    char recPath[MAX_PATH] = {0};
    if (utModFile(selfModule, "uniq-records.txt", recPath, sizeof(recPath)) &&
        loadRecordList(recPath)) {
        logD("reagent: %zu page records read from \"%s\"", g_pageRecords->size(), recPath);
    } else {
        logE("reagent: NO page record list found (\"%s\") - the gate will never arm", recPath);
    }

    char pagesPath[MAX_PATH] = {0};
    if (utModFile(selfModule, "uniq-pages.txt", pagesPath, sizeof(pagesPath)) &&
        loadPageList(pagesPath)) {
        logD("reagent page: %zu pages read from \"%s\"", g_pages->size(), pagesPath);
    } else {
        logD("reagent page: NO page list (\"%s\") - only the vanilla materials page is available",
             pagesPath);
    }
    liveInit(selfModule);
    const int page = wantedPage();
    if (page >= 0) {
        _snprintf_s(g_pageLabel, _TRUNCATE, "%s", (*g_pages)[page].label.c_str());
        logD("reagent page: uniq_page=%d -> \"%s\" (%s, %d boxes). The exe builds the page once "
             "when the HUD is created, so this applies from the next character load on.",
             page, (*g_pages)[page].record.c_str(), g_pageLabel, (*g_pages)[page].boxes);
    } else {
        _snprintf_s(g_pageLabel, _TRUNCATE, "vanilla materials");
        logD("reagent page: uniq_page=%d -> the VANILLA Crafting Materials page (untouched)",
             g_cfg.uniqPage);
    }

    if (utModFile(selfModule, "uniq-pages.arz", g_arzPath, sizeof(g_arzPath))) {
        logD("reagent db: page archive = \"%s\"", g_arzPath);
    } else {
        logE("reagent db: uniq-pages.arz NOT FOUND (\"%s\") - no collection page can be shown",
             g_arzPath);
        g_arzPath[0] = 0;
    }
    storeInit();  // the mod's own store of the collection - see ut_store.h
    return true;
}

int reagentInstall(int* total) {
    struct Target {
        const char* pretty;
        void* target;
        void* detour;
        void** original;
    };
    // Refuse an observer whose address is shared by more than one export. Identical function
    // bodies are folded together by the linker (ItemEquipment::IncrementStack is one
    // `xor al,al; ret` shared by 525 exports), and detouring a folded address would change
    // hundreds of unrelated virtuals. Verified on the LOADED module, every session.
    void* t_IdRoomCheck = (void*)p_IsInventorySpaceAvailable;
    void* t_IdMaxStack = (void*)p_GetItemMaxStackSize;
    // The PRIMARY arm site. Same folding rule.
    void* t_IdTakeReplica = (void*)g_gd.ItemGetItemReplicaInfo;
    {
        HMODULE game = g_gd.gameDll ? g_gd.gameDll : GetModuleHandleW(L"Game.dll");
        const int aRoom = exportAliasCount(game, t_IdRoomCheck);
        const int aMax = exportAliasCount(game, t_IdMaxStack);
        const int aRep = exportAliasCount(game, t_IdTakeReplica);
        logD("identity: export-alias check - Item::GetItemReplicaInfo %p x%d, "
             "Player::IsInventorySpaceAvailable %p x%d, GameEngine::GetItemMaxStackSize %p x%d "
             "(1 = not folded, safe to detour)",
             t_IdTakeReplica, aRep, t_IdRoomCheck, aRoom, t_IdMaxStack, aMax);
        if (aRep != 1) {
            t_IdTakeReplica = nullptr;
            identityDisable(
                "Item::GetItemReplicaInfo is shared by more than one export (folded), or the "
                "export table could not be read - the reagent take cannot be observed");
        }
        if (aRoom != 1) {
            // The ARM is the load-bearing observer. Without it there is no state at all, and
            // classifying a creation by its return address alone is exactly what
            // FEASIBILITY-V2 section 4 forbids. So: nothing is detoured, nothing substituted.
            t_IdRoomCheck = nullptr;
            t_IdMaxStack = nullptr;
            InterlockedExchange(&g_idOff, 1);
            logE("identity: ***** DISABLED ***** - this build folds an export the take observer "
                 "needs (%d aliases)", aRoom);
            logD("Player::IsInventorySpaceAvailable is shared, or the export table could not be "
                 "read; nothing is detoured, a take hands back the engine's own copy, the "
                 "collection keeps every entry and a non-pristine deposit is refused");
        } else if (aMax != 1) {
            t_IdMaxStack = nullptr;
            InterlockedExchange(&g_idOneStep, 1);
            logW("identity: ONE-STEP mode - GameEngine::GetItemMaxStackSize is folded into %d "
                 "exports and is not detoured", aMax);
            logD("the arm on Player::IsInventorySpaceAvailable plus the five state tests still "
                 "classify the take; only the ordering step is gone");
        }
    }
    const Target targets[] = {
        {"Engine::LoadMainDatabase", t_LoadMainDatabase, (void*)&hk_LoadMainDatabase,
         (void**)&o_LoadMainDatabase},
        {"Engine::LoadDatabase", t_LoadDatabase, (void*)&hk_LoadDatabase,
         (void**)&o_LoadDatabase},
        {"Item::Load", t_ItemLoad, (void*)&hk_ItemLoad, (void**)&o_ItemLoad},
        {"GameEngine::AddItemToReagents", t_AddItemToReagents, (void*)&hk_AddItemToReagents,
         (void**)&o_AddItemToReagents},
        {"GameEngine::TakeItemFromReagents", t_TakeFromReagents, (void*)&hk_TakeFromReagents,
         (void**)&o_TakeFromReagents},
        {"PlayerInvCtrl::DepositReagents", t_DepositReagents, (void*)&hk_DepositReagents,
         (void**)&o_DepositReagents},
        {"Sack::DepositSackIntoReagents", t_DepositSack, (void*)&hk_DepositSack,
         (void**)&o_DepositSack},
        {"GameEngine::DepositTransferReagents", t_DepositTransfer, (void*)&hk_DepositTransfer,
         (void**)&o_DepositTransfer},
        {"ObjectManager::DestroyObjectEx", t_DestroyObjectEx, (void*)&hk_DestroyObjectEx,
         (void**)&o_DestroyObjectEx},
        {"CursorItemMove::PrimaryReagentActivate", t_PrimaryReagentActivate,
         (void*)&hk_PrimaryReagentActivate, (void**)&o_PrimaryReagentActivate},
        {"CursorItemMove::QuickDropInReagents", t_QuickDropInReagents,
         (void*)&hk_QuickDropInReagents, (void**)&o_QuickDropInReagents},
        {"GameEngine::TakeItemFromReagents(id)", t_TakeFromReagentsId,
         (void*)&hk_TakeFromReagentsId, (void**)&o_TakeFromReagentsId},
        {"CursorItemMove::Cancel", t_Cancel, (void*)&hk_Cancel, (void**)&o_Cancel},
        {"Item::CreateItem", t_ItemCreateItem, (void*)&hk_ItemCreateItem,
         (void**)&o_ItemCreateItem},
        {"ControllerChar::CreateItemInInventory", t_CreateItemInInventory,
         (void*)&hk_CreateItemInInventory, (void**)&o_CreateItemInInventory},
        {"GameEngine::CreateItemForCharacter", t_CreateItemForCharacter,
         (void*)&hk_CreateItemForCharacter, (void**)&o_CreateItemForCharacter},
        {"GameEngine::ExitPlayingMode", t_ExitPlayingMode, (void*)&hk_ExitPlayingMode,
         (void**)&o_ExitPlayingMode},
        {"ObjectManager::LoadTableFile", t_LoadTableFileHook, (void*)&hk_LoadTableFile,
         (void**)&o_LoadTableFile},
        {"ObjectManager::GetLoadTable", t_GetLoadTable, (void*)&hk_GetLoadTable,
         (void**)&o_GetLoadTable},
        // The two observers of the exe's own take sequence. Both are pure
        // straight through - they call the original first and only ever set a per-frame flag -
        // and
        // both are resolved by EXPORT NAME, so nothing here depends on an exe RVA.
        {"Player::IsInventorySpaceAvailable", t_IdRoomCheck,
         (void*)&hk_IsInventorySpaceAvailable, (void**)&o_IsInvSpace},
        {"GameEngine::GetItemMaxStackSize", t_IdMaxStack, (void*)&hk_GetItemMaxStackSize,
         (void**)&o_GetItemMaxStack},
        // The real take observer. It passes straight through - it calls the original
        // first, then does two pointer compares against the two signature-located return
        // addresses and returns; only a genuine reagent take gets past them.
        {"Item::GetItemReplicaInfo", t_IdTakeReplica, (void*)&hk_ItemGetItemReplicaInfo,
         (void**)&o_ItemGetReplica},
    };
    const int n = (int)(sizeof(targets) / sizeof(targets[0]));
    if (total) *total += n;
    int ok = liveInstall(total);
    // The gate is all-or-nothing: every name in kRequiredForGate below must be live or the gate
    // is forced off - the choke point, the three auto-deposit entry points, Item::Load and
    // ObjectManager::DestroyObjectEx.
    // WHY the load path: without Item::Load (or without a decoded flag offset) the next character
    // load REFUNDS every stored unique as a stock copy and `SaveReagents` at 0x2CE535 erases
    // reagents.gst in the same call - so deposits must not be possible in a build that would do
    // that.
    // WHY DestroyObjectEx: it is the ONLY thing that takes a recycled id back out of
    // g_mapProtoIds, and AddItemToReagents' merge branch really does destroy a temporary
    // prototype through it on every second and later deposit (0x2CEE36 -> IAT 0x5F3118 =
    // ?DestroyObjectEx@ObjectManager@GAME@@). Without it a stale id stays in the set for the rest
    // a later PLAYER item that inherits that id can never be disarmed - permanently barred from
    // every stash tab by Item::CanBePlacedInTransferStash+0x9. Forcing the gate off is the
    // fail-safe: with no arm on a player item the hazard cannot arise at all, while the
    // keep-alive below stays available so the stored collection still survives a load.
    const char* kRequiredForGate[] = {"GameEngine::AddItemToReagents",
                                      "PlayerInvCtrl::DepositReagents",
                                      "Sack::DepositSackIntoReagents",
                                      "GameEngine::DepositTransferReagents", "Item::Load",
                                      "ObjectManager::DestroyObjectEx"};
    const int kRequiredN = (int)(sizeof(kRequiredForGate) / sizeof(kRequiredForGate[0]));
    bool required[6] = {false, false, false, false, false, false};
    for (int i = 0; i < n; ++i) {
        const Target& t = targets[i];
        if (!t.target) {
            logE("  hook %-36s SKIPPED (target not resolved)", t.pretty);
            continue;
        }
        MH_STATUS s = MH_CreateHook(t.target, t.detour, t.original);
        if (s == MH_OK) s = MH_EnableHook(t.target);
        if (s != MH_OK) {
            logE("  hook %-36s FAILED (%d)", t.pretty, (int)s);
            continue;
        }
        logD("  hook %-36s installed at %p", t.pretty, t.target);
        ++ok;
        for (int k = 0; k < kRequiredN; ++k) {
            if (!strcmp(t.pretty, kRequiredForGate[k])) required[k] = true;
        }
    }
    for (int k = 0; k < kRequiredN; ++k) {
        if (required[k]) continue;
        InterlockedExchange(&g_gateForcedOff, 1);
        logE("collection: ***** FORCED OFF ***** - \"%s\" could not be hooked, so nothing is "
             "collected this session", kRequiredForGate[k]);
        logD("the gate is all-or-nothing: without the choke point, the three auto-deposit entry "
             "points, Item::Load and the DestroyObjectEx recycling guard, an auto-deposit could "
             "convert a unique into a reagent count, or a recycled object id could leave an item "
             "barred from every stash tab");
    }
    if (!g_flagOffset || !g_mapProtoIds) {
        InterlockedExchange(&g_gateForcedOff, 1);
        logE("collection: ***** FORCED OFF ***** - this build could not decode what a deposit "
             "needs, so nothing is collected this session");
        logD("craftingMaterial offset=0x%X, map-owned id set=%d; without both, a deposit could "
             "arm a byte the mod cannot take back off",
             g_flagOffset, g_mapProtoIds ? 1 : 0);
    }
    if (!InterlockedCompareExchange(&g_gateForcedOff, 0, 0)) {
        logD("reagent gate: all six gate-critical detours are live (choke point + 3 deposit "
             "entry points + Item::Load + DestroyObjectEx), so the gate may arm");
    }
    // The identity observers are all-or-nothing too: without BOTH of them there is no
    // state-based classifier, and a return address alone must never be allowed to classify a
    // creation. Half of this must never run.
    if (!InterlockedCompareExchange(&g_idOff, 0, 0) && !o_ItemGetReplica) {
        identityDisable(
            "Item::GetItemReplicaInfo is not hooked, so the reagent take cannot be observed at "
            "the one call both take blocks make");
    } else if (!InterlockedCompareExchange(&g_idOff, 0, 0) && !o_IsInvSpace) {
        identityDisable(
            "Player::IsInventorySpaceAvailable is not hooked, so the secondary observer is "
            "gone");
    } else if (!InterlockedCompareExchange(&g_idOff, 0, 0)) {
        if (!o_GetItemMaxStack) InterlockedExchange(&g_idOneStep, 1);
        logD("identity: take observer live (Item::GetItemReplicaInfo, the arm; "
             "Player::IsInventorySpaceAvailable%s, the secondary) - a taken item comes "
             "back as the item deposited once the take-site signature resolves",
             InterlockedCompareExchange(&g_idOneStep, 0, 0)
                 ? ", ONE-STEP: GameEngine::GetItemMaxStackSize is not hooked"
                 : " + GameEngine::GetItemMaxStackSize");
    }
    return ok;
}

// The map has to LOSE an id when the engine takes the item back out of the sack, or "in a sack
// the mod can see" decays into
// "the mod saw this id in a sack once this session" - and an item that sat in a bag and was then
// EQUIPPED still answered yes. `InventorySack::RemoveItem` is the only removal the mod observes
// (hooks.cpp); the ctrl's own `PlayerInventoryCtrl::RemoveItem` erases the grid map INLINE
// and is not hooked, so this closes the sack routes and no others.
// Losing an id costs nothing: the next `InventorySack::AddItem`
// puts it back, and until then the worst case is one shift-click taking the UNSUPPORTED path,
// where the item never leaves the character.
void reagentForgetSackItem(unsigned int id) {
    try {
        Guard g;
        if (!g_byId || !id) return;
        std::unordered_map<unsigned int, GdItem*>::iterator it = g_byId->find(id);
        if (it == g_byId->end()) return;
        if (g_idOf) g_idOf->erase(it->second);
        g_byId->erase(it);
    } catch (...) {
    }
}

void reagentNoteItem(GdItem* item) {
    noteOwnedRecord(item);
    // Unconditional registration, see hk_ItemLoad.
    if (!item || !g_flagOffset) return;
    __try {
        const char* name = g_gd.ObjectGetObjectName ? g_gd.ObjectGetObjectName(item) : nullptr;
        if (isOurRecord(name)) registerItem(item, name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Defined with THE SWITCH, far below; the caravan's open edge asks it too.
static void followSaveVariant();

// GameEngine::SetTransferOpen(true) is called EVERY FRAME while the caravan is up, so the work
// below is edge-triggered: one reconcile per actual open.
void reagentOnTransferOpen(GdGameEngine* engine, bool open) {
    static int lastState = -1;
    if (engine) g_gameEngine = engine;
    const int state = open ? 1 : 0;
    if (state == lastState) return;
    lastState = state;
    // THE SWITCH MUST HAPPEN BEFORE THE FIRST RELAYOUT OF A WORLD, and the caravan's open
    // edge is the last moment that is still true - the page is laid out from the frames that
    // follow it. The Update poll normally settles the mode long before this (twice a second from
    // the first live player). Asking here as well means a world that reaches the caravan before
    // the poll has succeeded still paints the right collection instead of none. On the close edge
    // there is nothing to follow. (The GameEngine pointer the line above captures is not on the
    // mode's path at all - GameInfo::GetHardcore is read through the Engine singleton - but it is
    // what lets the info line print the +0x375BA byte beside it.)
    if (open) followSaveVariant();
    InterlockedExchange(&g_transferOpenNow, state);
    if (!open) {
        // The close edge is a DISARM edge - every byte the mod set goes back to 0 before
        // anything else, so a unique can be stashed normally the instant the caravan closes.
        // The registry itself SURVIVES the close: dropping it would throw the knowledge away a
        // dozen times a session for nothing, and every entry is re-validated anyway
        // (IsObjectOnDeletedList + the stored object id) on every single use.
        gateApply("the caravan window closed", true);
        return;
    }
    InterlockedExchange(&g_creationLogs, 0);  // a fresh take-probe budget for every open
    dbChecksumProbe("e: SetTransferOpen(true)");
    // Reconcile the journal with the engine's map. Edge-triggered: SetTransferOpen(true) is
    // called EVERY FRAME while the caravan is up.
    try {
        journalReconcile();
    } catch (...) {
        logE("rescue journal: reconciliation aborted (allocation failure)");
    }
    // The open edge sweeps the whole registry - every entry that still passes the liveness test
    // is armed if the page condition holds, and the dead ones are pruned. This is what makes the
    // FIRST drag after opening the caravan work.
    gateApply("GameEngine::SetTransferOpen(true)", true);
    if (!g_pageRecords) return;
    // Reconcile our tally with the engine's own map (GameEngine::GetReagentItemCount does the
    // lookup for us, so no map node has to be walked by hand).
    try {
        // ONE in-order walk of GameEngine's own map instead of 2,190 GetReagentItemCount
        // queries - and the walk reports node PRESENCE, which GetReagentItemCount does not (it
        // returns the prototype's stack size, 0 for a freshly stored unique).
        std::map<std::string, int> live;
        const bool mapOk = reagentWalkPageRecords(&live);
        if (!mapOk) {
            logD("reagent collect: reconciliation UNAVAILABLE (the engine's reagent map could not "
                 "be walked) - keeping the mod's own tally");
            return;
        }
        int held = 0, records = 0;
        std::string summary;
        for (std::unordered_set<std::string>::const_iterator it = g_pageRecords->begin();
             it != g_pageRecords->end(); ++it) {
            std::map<std::string, int>::const_iterator f = live.find(*it);
            const int n = f == live.end() ? 0 : f->second;
            setTrackedCount(*it, n);
            if (n > 0) {
                ++records;
                held += n;
                if (summary.size() < 400) {
                    char line[300];
                    _snprintf_s(line, sizeof(line), _TRUNCATE, "%s%s x%d",
                                summary.empty() ? "" : ", ", it->c_str(), n);
                    summary += line;
                }
            }
        }
        logD("reagent collect: reconciled with the engine's map - %d of %zu boxes filled, %d items "
             "total%s%s",
             records, g_pageRecords->size(), held, summary.empty() ? "" : ": ", summary.c_str());
    } catch (...) {
        logE("reagent collect: reconciliation aborted (allocation failure)");
    }
}

// The second teardown signal. GameEngine::ExitPlayingMode is the explicit one; this is the
// observed one - the first Update after a session in which GetMainPlayer() is null.
// The two once-per-world latches behind the save-variant lines below belong to
// the WORLD, not to the session - a mode switch is a teardown, and the new mode gets its own line.
volatile LONG g_variantSaid = 0;    // the INFO line has named a readable byte
volatile LONG g_variantWarned = 0;  // the warn line has named an unreadable one

void reagentOnWorldTeardown(const char* why) {
    InterlockedExchange(&g_variantSaid, 0);
    InterlockedExchange(&g_variantWarned, 0);
    pageOnWorldTeardown();
    // The live stop goes with the world here too - this is the observed teardown, the fallback
    // for when ExitPlayingMode does not come, and re-arming writes no engine memory. The plate's
    // own teardown is NOT called from here: it releases textures, and that belongs to the
    // explicit signal (ut_live.cpp calls it again at every HUD build).
    liveOnWorldTeardown();
    // And the mode itself. A character who has left the world can no longer vouch for
    // which collection is the live one, and the next one may be the other mode - so the paint
    // gate closes again until a world says so. `followSaveVariant` re-opens it on its next tick
    // (and, on the switch path, `journalFollowMode` immediately below does it in this very call).
    journalForgetMode();
    gateDisarmAll(why);  // unconditionally, and before anything else
    registryClear(why);
    try {
        teardownTables(why);
    } catch (...) {
    }
    // The private table's prototypes belong to the world that created them,
    // exactly like ut_live's display cache - and so does the per-world parity report.
    storeOnWorldTeardown();
}

void reagentLateLoadTick(bool gameThread) {
    // The worker's stall line names the thread that stopped advancing frames.
    if (gameThread) InterlockedExchange(&g_gameTid, (LONG)GetCurrentThreadId());
    liveTick(gameThread);
    if (gameThread) gateFollowLiveGroup();  // the gate follows the shown group
    if (gameThread) {
        identityTakeSiteTick();  // the arm-site scan, retried while the DRM stub is still up
        depositSiteTick();       // the same, for the two exe deposit call sites
        bindingsLateReport();    // one line once every exe binding has reported
        identityArmTick();       // drop an arm nothing consumed
        takeWatchTick();
        rescueTick();
        storeTick(true);  // the reagent_save switch and the stack-0 probe
    } else {
        // Worker thread: the file writes the rescue kit owes. None of them may run on the render
        // thread.
        journalService();
        storeTick(false);
        mpPendingTick();  // the bag watchdog, mod-owned memory only
    }
    if (!g_cfg.dbLoad) return;
    // A load is running on another thread: the detours own the overlay while it does.
    if (InterlockedCompareExchange(&g_dbBusy, 0, 0)) return;
    if (InterlockedCompareExchange(&g_dbLoaded, 0, 0)) {
        if (gameThread) verifyOverride();
        return;
    }
    if (!g_gd.ppEngine || !*g_gd.ppEngine || !p_GetDbChecksum) return;
    // 0 until LoadMainDatabase has finished; non-zero means the detour missed its chance.
    unsigned int sum = 0;
    __try {
        sum = p_GetDbChecksum(*g_gd.ppEngine);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (!sum) return;
    logD("reagent db: LoadMainDatabase detour never fired (checksum 0x%08X already set) - "
         "loading from %s instead",
         sum, gameThread ? "the game thread" : "the worker thread");
    loadOurArchive(*g_gd.ppEngine, gameThread ? "game-thread fallback" : "worker fallback");
}

// The captured GameEngine, published for ut_store.cpp's one-byte writes.
GdGameEngine* reagentGameEngine() {
    return g_gameEngine;
}

// See ut_reagent.h for the contract. Both buffers are static and this is
// game-thread-only code (showBox, from applyLayout, from the PresentSurface tick) - the
// same discipline the identity buffers next door already keep.
unsigned int reagentBuildIdentityProto(const char* record, unsigned int stack,
                                       const char** why) {
    static UtReplicaCapture s_entry;
    static UtIdentityOverlay s_ov;
    *why = "?";
    if (!record || !*record) {
        *why = "no record";
        return 0;
    }
    if (!o_ItemCreateItem) {
        *why = "Item::CreateItem is not hooked, so its trampoline does not exist";
        return 0;
    }
    if (!g_replicaSize || g_replicaSize > 0x200 || !g_replicaStackOff) {
        *why = "the replica layout (size / stack mirror) was never decoded";
        return 0;
    }
    if (!journalGet(record, &s_entry)) {
        *why = "no journal entry";
        return 0;
    }
    if (s_entry.replicaLen != g_replicaSize) {
        *why = "the journal replica is a different length from this build's";
        return 0;
    }
    if (!utBuildSwapOverlay(s_entry, s_entry.replica, s_entry.replicaLen, g_replicaStackOff,
                            &s_ov)) {
        *why = s_ov.why;
        return 0;
    }
    GdItem* fresh = swapCreateItem(s_ov.replica);
    if (!fresh) {
        *why = "Item::CreateItem returned nothing";
        return 0;
    }
    const unsigned int id = safeObjectId(fresh);
    if (!id) {
        *why = "the new prototype has no object id";
        return 0;
    }
    const void* maxFn = itemVtableSlot(fresh, 0x620);
    if (maxFn) swapSetMaxStack(fresh, maxFn);
    setStackSafe(fresh, id, stack ? stack : 1);
    *why = "ok";
    return id;
}

// The census. ONE in-order walk of the engine's reagent map, the same one `reagentWalkHeld`
// makes - it is nodes-in-the-map work (a well-used save holds a few hundred), never 3,288
// per-record lookups. The string work is outside the SEH frames, exactly as
// `reagentWalkPageRecords` does it, because MSVC forbids mixing the two in one frame.
//
// The SAME walk also collects EVERY row of ours, holding or empty, with what the engine holds in
// it. The takeover census (`ut_store.cpp`) names those rows: `reagents.gst` must hold NONE of our
// records, and a file that still has some is a defect the user has to be able to act on, which
// means seeing the record paths. `reagentStoreCensus` is the four-argument wrapper over the same
// walk.
bool reagentStoreCensusEx(UtStoreCensus* out, char (*emptyOut)[256], int cap,
                          char (*ourOut)[256], int* ourHeld, int ourCap, int* ourCopied) {
    if (ourCopied) *ourCopied = 0;
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!p_GetPlayerReagents || !g_gameEngine || !g_pageRecords || g_pageRecords->empty()) {
        return false;
    }
    out->pageRecords = (int)g_pageRecords->size();
    static const int kMaxNodes = 4096;
    std::vector<const void*> nodes;
    try {
        nodes.resize(kMaxNodes);
    } catch (...) {
        return false;
    }
    const int n = reagentCollectNodes(&nodes[0], kMaxNodes);
    if (n < 0) return false;   // inconclusive: the probe must not act on this
    out->nodes = n;
    for (int i = 0; i < n; ++i) {
        char raw[400];
        if (!nodeKeyCopy(nodes[i], raw, sizeof(raw))) {
            ++out->badKeys;
            continue;
        }
        try {
            std::string key(raw);
            if (g_pageRecords->find(key) == g_pageRecords->end()) continue;
        } catch (...) {
            ++out->badKeys;
            continue;
        }
        ++out->rows;
        // `heldOf` is the engine's real holding for one node: the stored PROTOTYPE's stack, not
        // the stale `ReagentData::count` at node+0x44. -1 means the node was null, the prototype
        // id was 0, the object is gone, or it failed the liveness triple - never a stack of 0, so
        // the probe can never mistake an unreadable row for an empty one.
        const int h = heldOf(nodes[i]);
        if (ourOut && ourCopied && *ourCopied < ourCap) {
            _snprintf_s(ourOut[*ourCopied], 256, _TRUNCATE, "%s", raw);
            if (ourHeld) ourHeld[*ourCopied] = h;
            ++*ourCopied;
        }
        if (h < 0) {
            ++out->unreadable;
            continue;
        }
        if (h > 0) {
            ++out->holding;
            continue;
        }
        ++out->empty;
        if (emptyOut && out->emptyCopied < cap) {
            _snprintf_s(emptyOut[out->emptyCopied], 256, _TRUNCATE, "%s", raw);
            ++out->emptyCopied;
        }
    }
    return true;
}

bool reagentStoreCensus(UtStoreCensus* out, char (*emptyOut)[256], int cap) {
    return reagentStoreCensusEx(out, emptyOut, cap, nullptr, nullptr, 0, nullptr);
}

unsigned int reagentMapProtoId(const char* record) {
    if (!record || !*record) return 0;
    try {
        std::string key(record);
        toLower(&key);
        bool mapOk = false;
        const void* node = findReagentNode(key, &mapOk);
        if (!node) return 0;
        ProtoInfo pi;
        memset(&pi, 0, sizeof(pi));
        readProtoFields(node, &pi);
        return pi.protoId;
    } catch (...) {
        return 0;
    }
}

// The fingerprint must NOT be an FNV of the raw ItemReplicaInfo bytes. The replica embeds MSVC
// std::strings, and every one of them longer than 15 characters (the record path at +0x08 for a
// start) is a HEAP POINTER; the mod's prototype is a fresh `Item::CreateItem` whose
// `SetItemReplicaInfo` deep-copies into NEW allocations, so two copies of one and the same
// identity would fingerprint differently by construction and could never compare equal.
//
// So the fingerprint is over the DECODED identity: `captureReplicaRaw` is the mod's own
// per-probe-SEH slot scan (the same one the journal capture uses), and what is hashed is
//   * every replica byte that is NOT inside a recorded 0x20-byte std::string window, with the
//     object id (+0x00..0x08) and the stack mirror masked out, and
//   * each slot's OFFSET and its TEXT,
// which is what "the same identity" actually means. Game thread only (showBox), so the capture
// buffer is a function-static - it is 4 KB and has no business on the stack.
unsigned int reagentProtoFingerprint(unsigned int protoId) {
    static UtReplicaCapture s_cap;
    if (!protoId || !g_replicaSize || g_replicaSize > sizeof(s_cap.replica)) return 0;
    GdItem* proto = findItemById(protoId);
    if (!proto) proto = (GdItem*)reagentObjectFromId(protoId);
    if (!proto) return 0;
    memset(&s_cap, 0, sizeof(s_cap));
    // captureReplicaRaw names cap->record in its empty-heap-slot warning; give it something
    // readable rather than an empty string, since this capture is not a journal entry.
    _snprintf_s(s_cap.record, sizeof(s_cap.record), _TRUNCATE, "(parity fingerprint id=%u)",
                protoId);
    if (!captureReplicaRaw(proto, &s_cap) || s_cap.replicaLen != g_replicaSize) return 0;
    // Mask the two fields two copies of the same identity are ALLOWED to differ in.
    memset(s_cap.replica, 0, 8);  // ItemReplicaInfo+0x00, the object id
    if (g_replicaStackOff + 4 <= g_replicaSize) memset(s_cap.replica + g_replicaStackOff, 0, 4);
    static unsigned char s_mask[0x200];
    memset(s_mask, 0, sizeof(s_mask));
    for (int s = 0; s < s_cap.slotCount && s < 24; ++s) {
        for (unsigned int i = s_cap.slotOff[s]; i < s_cap.slotOff[s] + 0x20 && i < g_replicaSize;
             ++i) {
            s_mask[i] = 1;
        }
    }
    unsigned int h = 2166136261u;  // FNV-1a, 32 bit
    for (unsigned int i = 0; i < g_replicaSize; ++i) {
        if (s_mask[i]) continue;  // a std::string window: BYTES are pointers, the TEXT is below
        h ^= s_cap.replica[i];
        h *= 16777619u;
    }
    for (int s = 0; s < s_cap.slotCount && s < 24; ++s) {
        h ^= (unsigned char)(s_cap.slotOff[s] & 0xFF);
        h *= 16777619u;
        for (const char* p = s_cap.slotText[s]; *p; ++p) {
            h ^= (unsigned char)*p;
            h *= 16777619u;
        }
    }
    return h ? h : 1u;  // 0 is reserved for "could not be read"
}

unsigned int reagentSafeObjectId(const GdItem* item) {
    return safeObjectId(const_cast<GdItem*>(item));
}

// ---- THE TRANSITION RULE, exported -----------------------------------------------------------
// The ONE function the rest of the mod asks how many copies of a record are held. It never writes
// anything - not the tracked tally either, which is `collectionAccepts`' business - so a counter
// refresh can call it per record without changing a single piece of state.
// `plateOwns(record)` is `reagentHeldTotal(record,...) >= 1`.
// It keeps no second copy of the sum: it reads the three inputs the deposit path already has -
// ProtoInfo, the engine's own count and the mod's tally - and hands them to `heldSumFrom`, the
// ONE place the two halves are added (see the long comment there for why `collectionAccepts`
// calls that rather than this one).
int reagentHeldTotal(const char* record, int* fromTable, int* fromMap) {
    if (fromTable) *fromTable = 0;
    if (fromMap) *fromMap = 0;
    if (!record || !*record) return 0;
    int table = 0, map = 0, total = 0;
    try {
        std::string key(record);
        toLower(&key);
        ProtoInfo pi;
        reagentProtoInfo(key, &pi);
        total = heldSumFrom(pi, engineCount(key.c_str()), trackedCount(key), key, &table, &map);
    } catch (...) {
        // THIS CATCH DOES NOT FAIL CLOSED, and that is the right way round:
        //   * A throw here is a failure to read the MAP side (the string, the ProtoInfo or
        //     engineCount); it is not the map answering "0". `table` keeps whatever the table
        //     half had already produced, because `heldSumFrom` fills `fromTable` before it can
        //     throw only when it got that far - so it is re-read here, by itself, since
        //     `tableHeldOf` touches no engine memory at all and cannot be the thrower.
        //   * THE DEPOSIT GATE NEVER REACHES THIS CATCH. `collectionAccepts` calls `heldSumFrom`
        //     with values it read under its own guards and handles `!proto->mapOk` explicitly a
        //     few lines later, so the one decision that could duplicate an item is not taken on
        //     the strength of this 0. The callers that ARE here - `plateOwns`, a counter refresh
        //     - can only mispaint one box for one tick, and the next tick repaints it.
        map = 0;
        try {
            std::string key(record);
            toLower(&key);
            table = tableHeldOf(key.c_str());
        } catch (...) {
            table = 0;
        }
        static volatile LONG said = 0;
        if (!InterlockedExchange(&said, 1)) {
            logD("reagent held: the map side of the transition sum THREW for %s - the table half "
                 "(%d) is still exact and is what this answer is made of. The deposit path does "
                 "not use this function, so nothing is accepted or refused on the strength of it.",
                 record, table);
        }
        total = table;
    }
    if (fromTable) *fromTable = table;
    if (fromMap) *fromMap = map;
    return total;
}

// The probe's skip must prove the prototype it is about to leave un-armed really is the STACK-0
// row, and never a box the player has just put something back into. `safeStackOf` is this file's
// SEH-guarded Item::GetStackSize (-1 = unreadable);
// this is asked once per load, only for the record that already matched the mark by name.
int reagentSafeStack(const GdItem* item) {
    return safeStackOf(item);
}

bool reagentTransferOpen() {
    return InterlockedCompareExchange(&g_transferOpenNow, 0, 0) != 0;
}

// WHY NOTHING HERE EMPTIES A BOX WITH `GameEngine::TakeItemFromReagents`: that call hands NOTHING
// back. It finds the node, clamps against `count` and writes `stack - taken` through the
// prototype's vt+0x610 (0x2CF2D3); there is no GiveItemToCharacter anywhere in it, so a drained
// copy is DESTROYED, and the rescue cannot bring it back either (`runRescue` skips
// `heldOf() <= 0` and `restoreOne` refuses "the box is already empty"). The mod must never lose
// an item, so the stack-0 probe works with what a save already has: a save that has ever had an
// item taken out of one of our boxes ALREADY carries count-0 rows, and `storeProbeChooseTick`
// picks one of those and drains nothing at all. It also means the mod wraps no swallowing frame
// around an engine MUTATOR.

const char* reagentStatus() {
    // With live paging on, `uniq_page` is not the page selection - report the group the page
    // actually shows, which is what the gate reads.
    int hbPage = g_cfg.uniqPage;
    const char* hbLabel = g_pageLabel;
    if (g_cfg.livePages) {
        if (liveActive()) {
            hbPage = liveShownGroup(&hbLabel);
        } else {
            hbPage = -1;
            hbLabel = "vanilla materials (live paging not armed)";
        }
    }
    _snprintf_s(g_status, sizeof(g_status), _TRUNCATE,
                "reagent: page=%d[%s] subs=%ld db=%s(%s) flagOff=0x%X replicaOff=0x%X pageRecords=%zu "
                "registrations=%ld registry=%zu "
                "adds=%ld accepted=%ld refused=%ld takes=%ld/%ld cancels=%ld deposits=%ld "
                "drops=%ld | gateForcedOff=%ld mp=%ld mpRefused=%ld "
                "mpHost=%ld mpPlayers=%ld mpPendingRemovals=%ld "
                "depositRefused=%ld deadWrites=%ld clears=%ld | stackBumps=%ld "
                "stackRollbacks=%ld deadProtoRefused=%ld swaps=%ld/%ld idAvail=%d",
                hbPage, hbLabel, InterlockedCompareExchange(&g_pageSubs, 0, 0),
                InterlockedCompareExchange(&g_dbLoaded, 0, 0) ? "loaded" : "no", g_dbRoute,
                g_flagOffset, g_replicaOffset, g_pageRecords ? g_pageRecords->size() : 0,
                InterlockedCompareExchange(&g_gateArmed, 0, 0), registrySize(),
                InterlockedCompareExchange(&g_addCalls, 0, 0),
                InterlockedCompareExchange(&g_accepts, 0, 0),
                InterlockedCompareExchange(&g_refusals, 0, 0),
                InterlockedCompareExchange(&g_takeCalls, 0, 0),
                InterlockedCompareExchange(&g_takeIdCalls, 0, 0),
                InterlockedCompareExchange(&g_cancelCalls, 0, 0),
                InterlockedCompareExchange(&g_depositCalls, 0, 0),
                InterlockedCompareExchange(&g_dropLogs, 0, 0),
                InterlockedCompareExchange(&g_gateForcedOff, 0, 0),
                InterlockedCompareExchange(&g_mpActive, 0, 0),
                InterlockedCompareExchange(&g_mpRefusals, 0, 0),
                InterlockedCompareExchange(&g_mpHost, 0, 0),
                InterlockedCompareExchange(&g_mpPlayers, 0, 0),
                InterlockedCompareExchange(&g_mpPendingCount, 0, 0),
                InterlockedCompareExchange(&g_depositRefusals, 0, 0),
                InterlockedCompareExchange(&g_deadWrites, 0, 0),
                InterlockedCompareExchange(&g_clears, 0, 0),
                InterlockedCompareExchange(&g_stackBumps, 0, 0),
                InterlockedCompareExchange(&g_stackRollbacks, 0, 0),
                InterlockedCompareExchange(&g_deadProtoRefusals, 0, 0),
                InterlockedCompareExchange(&g_refreshes, 0, 0),
                InterlockedCompareExchange(&g_refreshFails, 0, 0),
                identityAvailable() ? 1 : 0);
    {  // the scoped gate's own state
        const size_t at = strlen(g_status);
        if (at + 2 < sizeof(g_status)) {
            _snprintf_s(g_status + at, sizeof(g_status) - at, _TRUNCATE, " | %s", gateStateText());
        }
    }
    {
        const size_t at = strlen(g_status);
        if (at + 2 < sizeof(g_status)) {
            _snprintf_s(g_status + at, sizeof(g_status) - at, _TRUNCATE,
                        " | mapOwned=%zu mapOwnedKept=%ld sbRestoreFailed=%ld", mapOwnedCount(),
                        InterlockedCompareExchange(&g_mapOwnedKept, 0, 0),
                        InterlockedCompareExchange(&g_soulboundRestoreFailed, 0, 0));
        }
    }
    {
        const size_t at = strlen(g_status);
        if (at + 2 < sizeof(g_status)) {
            _snprintf_s(g_status + at, sizeof(g_status) - at, _TRUNCATE,
                        " | journalEntries=%zu journalAdds=%ld journalDrops=%ld "
                        "journalSynth=%ld journalWrites=%ld rescueRuns=%ld rescueRestored=%ld "
                        "rescueFailed=%ld takeMirror=%d syncs=%ld/%ld "
                        "uiTakes=%ld dbProbes=%ld dbSum=0x%08X",
                        journalCount(), InterlockedCompareExchange(&g_journalAdds, 0, 0),
                        InterlockedCompareExchange(&g_journalDrops, 0, 0),
                        InterlockedCompareExchange(&g_journalSynth, 0, 0), journalWrites(),
                        InterlockedCompareExchange(&g_rescueRuns, 0, 0),
                        InterlockedCompareExchange(&g_rescueRestored, 0, 0),
                        InterlockedCompareExchange(&g_rescueFailed, 0, 0),
                        g_takeMirrorOk ? 1 : 0,
                        InterlockedCompareExchange(&g_syncCalls, 0, 0),
                        InterlockedCompareExchange(&g_syncFaults, 0, 0),
                        InterlockedCompareExchange(&g_uiTakes, 0, 0),
                        InterlockedCompareExchange(&g_dbProbes, 0, 0), g_dbSumFirst);
        }
    }
    {  // the identity machinery's own counters
        const size_t at = strlen(g_status);
        if (at + 2 < sizeof(g_status)) {
            _snprintf_s(g_status + at, sizeof(g_status) - at, _TRUNCATE,
                        " | idOff=%ld idOneStep=%ld idArms=%ld idRestores=%ld "
                        "idMisses=%ld idFaults=%ld idRescue=%ld replicaStackOff=0x%X "
                        "idSites=%ld",
                        InterlockedCompareExchange(&g_idOff, 0, 0),
                        InterlockedCompareExchange(&g_idOneStep, 0, 0),
                        InterlockedCompareExchange(&g_idArms, 0, 0),
                        InterlockedCompareExchange(&g_idRestores, 0, 0),
                        InterlockedCompareExchange(&g_idMisses, 0, 0),
                        InterlockedCompareExchange(&g_idFaults, 0, 0),
                        InterlockedCompareExchange(&g_idRescueRestores, 0, 0), g_replicaStackOff,
                        InterlockedCompareExchange(&g_idSitesOk, 0, 0));
        }
    }
    const size_t used = strlen(g_status);
    if (used + 2 < sizeof(g_status)) {
        _snprintf_s(g_status + used, sizeof(g_status) - used, _TRUNCATE, " | %s", liveStatus());
    }
    return g_status;
}

// ut_live's ONE call into this file. Game thread, caravan open, boxes captured.
void reagentAfterRelayout(int group, size_t boxes) {
    if (!boxes) return;
    char why[96];
    _snprintf_s(why, sizeof(why), _TRUNCATE, "after a relayout, group %d, %zu boxes", group,
                boxes);
    reagentSyncCaravan(why);
    g_takeWatchAt = 0;
}

// ---- the crash diagnostics, exposed to dllmain -----------------------------------------------
void reagentLogFault(const char* where, const void* record, unsigned long tid) {
    logEngineFault(where, (const EXCEPTION_RECORD*)record, tid);
}

// > 0 while THIS thread is inside a deliberate, mod-handled probe.
// Read from the vectored handler, so it does nothing but read a thread-local.
long reagentProbeDepth() {
    return g_probeDepth;
}
void reagentProbeEnter() { ++g_probeDepth; }
void reagentProbeLeave() { --g_probeDepth; }

const char* reagentIdentityState(char* out, size_t cap) {
    if (!out || !cap) return "";
    return identityStateText(out, cap);
}

static void sayVariant(int v) {
    // THE LINE A MODE MIX-UP IS DIAGNOSED FROM, and it must not be a logD: below the info
    // threshold a whole hardcore session can go by on the softcore collection with nothing in the
    // log to say the mode was never read. INFO, once per world, said for every outcome.
    if (InterlockedExchange(&g_variantSaid, 1)) return;
    // BOTH numbers, side by side, once per world. `v` is the mode and comes from
    // GameInfo::GetHardcore; the GameEngine+0x375BA byte is printed next to it and nothing
    // branches on it. Keeping it in this one line is the whole reason its reader still exists:
    // the log then shows, for one character, what each of the two said, which is the evidence a
    // mode mix-up is diagnosed from.
    const char* why = "";
    const int b = reagentSaveVariantEx(&why);
    char byteText[192];
    if (b < 0) {
        _snprintf_s(byteText, sizeof(byteText), _TRUNCATE, "unreadable, %s", why);
    } else {
        _snprintf_s(byteText, sizeof(byteText), _TRUNCATE, "%d", b);
    }
    logI("collection: mode = %s (GameInfo::GetHardcore = %d; GameEngine+0x375BA byte = %s)",
         v == 1 ? "hardcore" : "softcore", v, byteText);
}

// THE SWITCH. This is the first point in the mod at which the collection mode can be
// asked for at all - a world is live, so there is a loaded character to ask. The journal opened
// on the softcore names at start-up (no character existed yet); if this world's character is the
// other mode the collection moves to the other file here, and the world's tables move with it:
// the private table, the owned counters and the display caches are all views of the file that is
// about to close, so a mode switch is a teardown plus an init as far as they are concerned.
// Called on every Update with a main player and does nothing at all once the mode is known and
// agrees - two interlocked reads and one SEH-guarded export call.
//
// The mode is GameInfo::GetHardcore's answer, NOT the GameEngine+0x375BA byte. That byte has
// been observed reading 1 on a softcore character after a hardcore one was loaded in the same
// process, which points the player at an empty collection of the other mode; see
// readCollectionMode. Nothing here branches on the byte.
//
// The caravan's own open edge asks this too (reagentOnTransferOpen), because that edge
// is the last moment before a world's first relayout.
static void followSaveVariant() {
    const char* why = "";
    const int v = readCollectionMode(&why);
    if (v != 0 && v != 1) {
        // Unreadable, with a main player in the world - which is the state the incident ran its
        // whole length in. Keep the file we are on, paint NOTHING (the paint gate below asks
        // journalModeKnown() itself), and SAY SO once, naming the reason and the consequence.
        if (!InterlockedExchange(&g_variantWarned, 1)) {
            logI("collection: mode = unreadable (%s)", why);
            logW("collection: a character is in the world but GameInfo::GetHardcore could not be "
                 "read, so the mod cannot tell the two collections apart - the collection "
                 "stays HIDDEN (no box of ours is painted) and deposits are refused until it can");
        }
        return;
    }
    const bool wasKnown = journalModeKnown();
    if (wasKnown && journalMode() == v) {  // the usual case, every tick
        sayVariant(v);
        return;
    }
    if (journalMode() != v) {
        reagentOnWorldTeardown("the shared stash variant changed (hardcore/softcore)");
    }
    const bool moved = journalFollowMode(v);
    // `moved` is false when the mode was UNKNOWN and it agrees with the file that happens to be
    // open (the ordinary softcore start-up: mode 0, no file to move). That is NOT a no-op: the
    // paint gate hides the whole collection until this instant - the counters read `?` and not
    // one box of ours is painted. So the snapshot is refreshed on the unknown -> known transition
    // as well as on a real switch; the refresh moves the owned fingerprint, which is what asks
    // the page for the relayout that paints the boxes.
    if (moved || !wasKnown) {
        // The owned counters paint from the journal, so pull them off the NEW file at once
        // rather than leaving the other mode's counts on screen until the next second's pass.
        // Harmless before the map walk has succeeded: it answers -1 and the tick repeats it.
        plateOwnedRefresh(true);
    }
    // After the switch, so the teardown above (which clears the latch) cannot swallow the line or
    // print it twice: the line the user reads always names the mode the collection ended up on.
    sayVariant(v);
}

void reagentOnMainPlayer(GdGameEngine* engine) {
    // WHERE THE POINTER COMES FROM. The caller is GameEngine::Update's own detour (hooks.cpp),
    // so `engine` IS the GameEngine - the very object `reagentSaveVariant` reads its byte out of
    // - and capturing it HERE, on the line before `followSaveVariant`, is what makes the mode
    // readable at the first Update of a world. Every OTHER capture site sits inside a detour a
    // session may not reach until much later (the deposit path, the takes, the caravan's open
    // edge), and a null pointer answers -1 and leaves the mode unknown for the whole session.
    // There is NO exported alternative: Game.dll's export table (src/gd_exports.h) has no static
    // getter for the GameEngine singleton - its only exported GameEngine statics are the four
    // MaxTransferSacks constants, and `GAME::gEngine` (gd_runtime.cpp:59) is the ENGINE, a
    // different object. The detour's own `this` is the source, and the other detours stay as the
    // fallback for anything that runs before the first Update with a main player.
    if (engine) g_gameEngine = engine;
    followSaveVariant();  // before anything else looks at the collection
    // A world exists from here on, so the registration LOG budget
    // switches from the small menu one to the full per-world one and starts from zero.
    if (!InterlockedExchange(&g_worldActive, 1)) {
        InterlockedExchange(&g_regLogUsed, 0);
        logD("reagent gate: a world is live - the registration log budget is %d for this world "
             "(the main menu had %d)",
             kGateLogMax, kGateLogMenuMax);
    }
    static bool done = false;
    if (done) return;
    done = true;
    dbChecksumProbe("d: first Update with a main player (character in the world)");
}

// ItemReplicaInfo lives INSIDE the Item (Item + this offset, decoded from
// Item::GetItemReplicaInfo's own bytes). ut_panel reads the base-record string in place instead
// of calling the sret getter into a fixed-size static.
unsigned int reagentReplicaOffset() {
    return g_replicaOffset;
}

// =============================================================================================
// The page switch (Ctrl + wheel / Ctrl + PageUp/PageDown over the open caravan window)
//
// It is an INI WRITER, not a live rebuild, and the reason is measured, not assumed:
// `ReagentWindow::Load` (exe rva 0x132100) APPENDS its boxes. Its loop at 0x001323A0 does
// `mov ecx,0xE0; call operator new` per box, runs the ctor, calls `UIReagentItem::Load`
// (0x1F0660) on the fresh object and push_backs a 16-byte entry into the vector at
// `window + 0x380` (`[rsi+8] += 0x10`, growing through 0x000FB7A0). Nothing between the function
// entry and that loop clears the vector, and `[rsi+0x518]` is overwritten with a freshly loaded
// texture without releasing the old one. Re-running it on the same window would therefore double
// every widget, leak the old ones and leave two boxes on every pixel - so the mod does NOT call
// it.
//
// Nothing here touches engine memory: it changes one integer, writes uniquetab.ini and logs.
volatile LONG g_pageSwitches = 0;

int pageCountAvailable() {
    return g_pages ? (int)g_pages->size() : 0;
}

void switchPage(int delta) {
    const int n = pageCountAvailable();
    if (n <= 0) {
        logD("reagent page: no page list is loaded - nothing to switch to");
        return;
    }
    // -1 (the untouched vanilla page) sits before page 0, so the cycle is -1, 0 .. n-1.
    int cur = g_cfg.uniqPage;
    if (cur < -1 || cur >= n) cur = -1;
    int next = cur + delta;
    while (next < -1) next += n + 1;
    while (next > n - 1) next -= n + 1;
    g_cfg.uniqPage = next;
    const char* label = "vanilla materials";
    if (next >= 0) label = (*g_pages)[next].label.c_str();
    _snprintf_s(g_pageLabel, _TRUNCATE, "%s", label);
    InterlockedIncrement(&g_pageSwitches);
    logD("reagent page: uniq_page %d -> %d \"%s\". This is the fallback for a session where the "
         "live tab did not arm, so it is NOT written to the settings file (there is no such "
         "setting any more) and the exe only builds the page when a character is loaded.",
         cur, next, g_pageLabel);
}

bool reagentHandleMessage(unsigned int msg, WPARAM wParam, LPARAM lParam, LRESULT* result) {
    (void)lParam;
    if (!g_cfg.pageHotkeys) return false;
    if (!InterlockedCompareExchange(&g_transferOpenNow, 0, 0)) return false;
    // Nothing is consumed unless the Crafting Materials page is the caravan page actually on
    // screen (caravan+0x1728 == 3), so the hotkeys never eat a keystroke meant for another tab.
    if (!liveMaterialsVisible()) return false;
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

    // While live paging is armed the same keys act IMMEDIATELY and nothing is written
    // to engine memory outside the game thread - the message thread only moves two integers and
    // the next GameEngine::Update applies the layout.
    if (liveActive()) {
        bool consumed = false;
        if (msg == WM_MOUSEWHEEL) {
            const int ticks = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
            consumed = liveHandleWheel(ticks, ctrl);
        } else if (msg == WM_KEYDOWN) {
            consumed = liveHandleKey((int)wParam, ctrl);
        } else if (msg == WM_KEYUP && (wParam == VK_PRIOR || wParam == VK_NEXT)) {
            consumed = liveHandleKey((int)wParam, ctrl);
        }
        if (consumed) {
            if (result) *result = 0;
            return true;
        }
        return false;
    }

    if (!ctrl) return false;
    int delta = 0;
    if (msg == WM_MOUSEWHEEL) {
        const int ticks = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
        if (ticks == 0) return false;
        delta = ticks > 0 ? -1 : 1;  // wheel up = the previous page
    } else if (msg == WM_KEYDOWN) {
        if (wParam == VK_PRIOR) delta = -1;
        else if (wParam == VK_NEXT) delta = 1;
        else return false;
    } else if (msg == WM_KEYUP) {
        if (wParam != VK_PRIOR && wParam != VK_NEXT) return false;
        if (result) *result = 0;
        return true;  // swallow the matching up so the game never sees half a keystroke
    } else {
        return false;
    }
    try {
        switchPage(delta);
    } catch (...) {
    }
    if (result) *result = 0;
    return true;
}

long reagentPageSwitches() {
    return InterlockedCompareExchange(&g_pageSwitches, 0, 0);
}

// The no-op the linker falls back to when this build has no ut_live.cpp definition yet (see the
// note at the top of this file). It says so ONCE and does nothing else: the swap itself and the
// engine's own SyncCaravanReagents have already run by then.
void utLiveRelayoutVisibleFallback() {
    static volatile LONG once = 0;
    if (!InterlockedExchange(&once, 1)) {
        logD("live: liveRelayoutVisible() is not linked into this build - a prototype swap "
             "repaints through SyncCaravanReagents alone (the box shows the new item; its "
             "component sub-icons appear after the next page change)");
    }
}

// =============================================================================================
// the read-only engine surface the tab, the table and the tooltip share
// =============================================================================================
// Thin, GAME-THREAD-ONLY wrappers over the helpers above. They add no policy of their own: what
// a caller sees here is what the deposit path, the take watch and the rescue command see.

bool reagentIsCollectionRecord(const char* record) { return isOurRecordSafe(record); }

bool reagentItemIsLive(GdItem* item, unsigned int id) { return itemIsLive(item, id); }

// The mod's decoded id->Object helper (Game.dll 0x19D20, taken out of
// TakeItemFromReagents(id,int)'s own bytes). Used for the ControllerPlayer as well as for items,
// so it hands back a raw void* and the caller does the RTTI / vtable check.
void* reagentObjectFromId(unsigned int id) {
    if (!id || !p_ObjectFromId || !p_ObjectManagerGet) return nullptr;
    __try {
        void* om = p_ObjectManagerGet();
        return om ? p_ObjectFromId(om, id) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

}  // namespace ut
