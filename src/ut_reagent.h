// ut_reagent.h - the engine's own fixed-slot reagent page (the caravan's Crafting Materials
// page) carries the collection. This file owns the engine handles of that page: the database
// override, the crafting-material gate, the deposit/take detours, the reagent map reads and the
// crash diagnostics.
//
// Two mechanisms:
//
//  1. DATABASE. data/uniq/uniq.arz overrides records/ui/caravan/caravan_materialwindow.dbr so the
//     Crafting Materials page lists the mod's boxes, one per unique. It is loaded by calling
//     Engine::LoadDatabase from a post-detour on Engine::LoadMainDatabase - i.e. after the game's
//     own archives and after Engine::GetDatabaseArchiveChecksum has already been computed, so the
//     multiplayer database checksum is unchanged and HasLoadedCustomDatabase stays false.
//     Nothing in the game folder is modified; uniq.arz lives next to the DLL.
//
//  2. GATE. What may enter a reagent box is a single bool at Item + 0xC64, loaded from the
//     record field `craftingMaterial` by Item::Load. The exported getter
//     Item::IsReagentCompatible has zero callers - AddItemToReagents and QuickDropInReagents read
//     the byte inline - so the only lever is the byte. The mod therefore
//       * reads the OFFSET out of IsReagentCompatible's own 7 code bytes (never hard-coded),
//       * sets the byte to 1 on items whose record is on our page (post-detour on Item::Load,
//         plus the InventorySack::AddItem detours),
//       * and clears it again for the whole registry while any of DepositReagents /
//         DepositSackIntoReagents / DepositTransferReagents is on the stack, so the
//         auto-deposit buttons never vacuum uniques out of an inventory.
//     Nothing is created, deleted or moved by the mod; the engine does all of that itself when
//     the user drops an item on a box.
#pragma once

#include <windows.h>

#include "gd_runtime.h"

namespace ut {

// Resolves the reagent-page exports, decodes the craftingMaterial offset and loads the record
// list (uniq-records.txt in the mod folder, else beside the .asi). Safe to call twice. Logs everything.
bool reagentInit(HMODULE selfModule);

// MinHook install for every reagent-page target; *total gets the number of targets tried.
int reagentInstall(int* total);

// Called from the InventorySack::AddItem detours, after the original ran.
void reagentNoteItem(GdItem* item);

// The other half of that - called from the InventorySack::RemoveItem detour when the original
// returned true, so the id map stays "what is in a sack the mod can see" rather than "what the
// mod once saw in a sack". Never touches the item, only the map.
void reagentForgetSackItem(unsigned int id);

// The fallback database load, used when the LoadMainDatabase detour fired too late (the worker
// thread cannot start until the loader lock is free, so the detour can miss its moment). Armed
// as soon as Engine::GetDatabaseArchiveChecksum stops being 0. Called every frame from the
// Engine::PresentSurface detour with gameThread=true - the safe place, because the game itself
// is not inside the database there - and once a second from the worker as a last resort.
// With the archive already in, a gameThread call also runs the one-shot override verification.
void reagentLateLoadTick(bool gameThread);

// The caravan window's open/close edge (GameEngine::SetTransferOpen). The engine calls it every
// frame while the window is up, so everything behind it is edge-triggered.
void reagentOnTransferOpen(GdGameEngine* engine, bool open);

// One short line for the heartbeat.
const char* reagentStatus();

// The GameEngine `this` the mod captured from its own detours (Update, AddItemToReagents,
// TakeItemFromReagents in both overloads, SetTransferOpen). It is the object
// `GetPlayerReagents` is called on, i.e. the object whose +0x36DA3 gates SaveReagents and whose
// +0x375BA is handed to GetSharedSavePath. NULL before the first tick. Never owned.
// (That byte is NOT the collection mode - see readCollectionMode in ut_reagent.cpp.)
GdGameEngine* reagentGameEngine();

// Is the caravan window open right now? The edge-triggered state GameEngine::SetTransferOpen
// publishes, read as an atomic. No engine call.
bool reagentTransferOpen();

// Object::GetObjectId behind the mod's usual SEH probe. 0 = unknown.
unsigned int reagentSafeObjectId(const GdItem* item);

// Item::GetStackSize behind the mod's usual SEH probe. -1 = it could not be read. The probe's
// keep-alive skip uses it to prove the prototype it is about to leave un-armed really is a
// STACK-0 row and not a box the player has just refilled.
int reagentSafeStack(const GdItem* item);

// ---- the display side of the private table -----------------------------------------------------
// Builds a MOD-OWNED identity prototype for `record` out of the journal, with the stack set
// to `stack`, and returns its object id (0 = it could not be built; `why` says which of the
// named refusals it was). It is the same sequence `swapPrototypeFresh` runs on a re-deposit
// - utBuildSwapOverlay over the journal entry, then Item::CreateItem's own TRAMPOLINE
// through the frame that logs a fault and continues the search - with one difference: the
// `incoming` replica is the journal entry's own blob, because there is no live engine
// prototype to read one off. Nothing is inserted into the engine's map and no node is
// touched. GAME THREAD ONLY. The object is never destroyed: it belongs to the world, like
// every prototype in ut_live's display cache.
unsigned int reagentBuildIdentityProto(const char* record, unsigned int stack,
                                       const char** why);

// ReagentData::protoId for `record`, straight out of the engine's map. 0 = no node, or the
// map could not be read. Read-only, SEH-guarded, no engine call beyond GetPlayerReagents.
unsigned int reagentMapProtoId(const char* record);

// ---- the store census: the probe's candidate comes from the ENGINE MAP ------------------------
// A take that restores an identity DROPS the journal entry, so the rows a stack-0 probe is about
// have no journal entry at all; the candidate therefore comes from the map itself.
//
// ONE in-order walk of the reagent map (the same `reagentCollectNodes` walk `reagentWalkHeld`,
// `plateOwnedRefresh` and `refreshMapOwnedFromMap` make - the map has one node per deposited
// record, not one per catalogue record, so this is nodes-in-the-map work and not one lookup per
// catalogue record), classified for the probe:
//
//   holding    - a live stored prototype with a stack of 1 or more (the collection holds it)
//   empty      - a live stored prototype with a stack of EXACTLY 0 (a row the player emptied the
//                ordinary way; `TakeItemFromReagents` decrements and never erases the node)
//   unreadable - the node is there but its prototype could not be read or is not live
//
// `empty` records are copied into `emptyOut` (lower-cased, as the map keys them) in the map's own
// sorted order, up to `cap`; `out->emptyCopied` says how many made it. Returns FALSE when the map
// could not be walked at all - the probe treats that as inconclusive and does nothing.
// Read-only: no engine write, no allocation beyond the node vector, SEH-guarded throughout.
struct UtStoreCensus {
    int nodes;        // nodes the walk saw
    int badKeys;      // nodes whose key string could not be read (they are counted nowhere else)
    int rows;         // nodes whose key is one of OUR page records
    int holding;      // of those, a live prototype with stack >= 1
    int empty;        // of those, a live prototype with stack == 0  <- the probe's candidates
                      // (`heldOf` returns -1 for EVERY unreadable case, a null node included, so
                      //  an unreadable row can never be counted here)
    int unreadable;   // of those, a prototype that could not be read
    int pageRecords;  // how many page records exist at all (uniq-records.txt)
    int emptyCopied;  // how many `empty` record paths were copied into emptyOut
};
bool reagentStoreCensus(UtStoreCensus* out, char (*emptyOut)[256], int cap);

// The same single walk, plus EVERY row of ours by name. `ourOut[i]` is the record path as the
// node spells it and `ourHeld[i]` is what the engine holds in it (the stored prototype's live
// stack; -1 = the row could not be read), for up to `ourCap` rows; `*ourCopied` says how many
// were written. The takeover census uses it to say WHICH of our records are still in
// `reagents.gst` - that list must be empty.
// GAME THREAD, caravan open, exactly like reagentStoreCensus.
bool reagentStoreCensusEx(UtStoreCensus* out, char (*emptyOut)[256], int cap,
                          char (*ourOut)[256], int* ourHeld, int ourCap, int* ourCopied);

// A fingerprint of a prototype's DECODED identity: every ItemReplicaInfo byte that is not
// inside a recorded std::string window (with the object id at +0x00 and the stack mirror at
// +replicaStackOff masked out), plus each string slot's offset and its TEXT. The raw bytes of
// a string slot are HEAP POINTERS and can never match between two copies, which is why they
// are decoded rather than hashed. Equal fingerprints mean two prototypes carry the same
// identity. 0 = the replica could not be read or the layout is unknown. GAME THREAD ONLY (it
// uses a static capture buffer).
unsigned int reagentProtoFingerprint(unsigned int protoId);

// ---- THE TRANSITION RULE ------------------------------------------------------------------------
// How many copies of `record` the player has in the collection right now, counting the mod's
// table AND any row the engine's map still holds (which is only ever non-zero before the
// collection has migrated out of the engine's map). max_per_record, the owned filter and the
// label all ask this, in ONE function so it cannot drift.
//
// `fromTable` and `fromMap` may be null; when they are not, they get the two halves, so a caller
// that must SAY which authority answered (the deposit refusal does) never recomputes either.
//   TABLE-OWNED = storeCount(record) >= 1     (mod-owned, no engine call)
//   MAP-OWNED   = the engine's map holds it with a LIVE stored prototype of stack >= 1
//   plateOwns(record) is the OR, i.e. reagentHeldTotal(record, ...) >= 1.
// It performs no engine WRITE and is safe on any thread that may read the map (the map read is
// the same SEH-guarded lookup `collectionAccepts` makes). The table half is 0 while the table
// cannot own anything, and the map's row is never added to the table's count.
int reagentHeldTotal(const char* record, int* fromTable, int* fromMap);


// Forget every remembered Item*. Called from the GameEngine::ExitPlayingMode detour and from the
// Update detour the first time a session has no main player.
void reagentOnWorldTeardown(const char* why);

// Item + this = ItemReplicaInfo, decoded from Item::GetItemReplicaInfo's own bytes. 0 = unknown.
// ut_panel uses it to read the base record in place instead of calling the getter.
unsigned int reagentReplicaOffset();

// Page switch. Ctrl + mouse wheel / Ctrl + PageUp / Ctrl + PageDown while the caravan window is
// open steps uniq_page (-1 = the untouched vanilla page, then 0..n-1) and writes it to
// uniquetab.ini. It is an INI WRITER: the exe builds the reagent page once, when the HUD is
// created, and ReagentWindow::Load APPENDS its boxes rather than clearing them (see the comment
// on switchPage), so a page change applies from the next character load on. Returns true when the
// message was consumed. Called from the WindowProc detour BEFORE the panel, and it is not gated
// by the panel's `enabled` key.
bool reagentHandleMessage(unsigned int msg, WPARAM wParam, LPARAM lParam, LRESULT* result);
long reagentPageSwitches();

// ---- after a relayout ---------------------------------------------------------------------------
// Called from ut_live's applyLayout on the GAME THREAD once a relayout has finished. It triggers
// the engine's own box-badge repaint (GameEngine::SyncCaravanReagents -> GameUIInterface vtable
// +0x88 -> ReagentWindow::Sync, exe 0x1324A0): Sync walks the box vector at window+0x380, looks
// each box's reagentName up in the reagent map and, only when the box's current item id differs,
// calls the box's own SetItem(node->itemId, true) - the same vtable slot +0xA8 the relayout uses.
// A record with NO node is skipped outright (0x132655 `cmp rbx,rdi; je`), so the vanilla page's
// empty boxes and our un-owned boxes keep exactly what the relayout put in them.
void reagentAfterRelayout(int group, size_t boxes);

// Called from the GameEngine::Update teardown watcher while a session HAS a main player: the one
// place the hardcore/softcore switch is followed. `engine` is the Update detour's own `this` -
// the GameEngine - and is CAPTURED here. The mode itself does not come through this pointer
// (GameInfo::GetHardcore is read off the Engine singleton), but the capture still feeds the
// +0x375BA byte the once-per-world info line prints beside it; pass nullptr and the last capture
// stands.
void reagentOnMainPlayer(GdGameEngine* engine);

// ---- crash diagnostics --------------------------------------------------------------------------
// dllmain's vectored exception handler hands every first-chance exception here; the mod LOGS and
// the handler then returns EXCEPTION_CONTINUE_SEARCH. `record` is an EXCEPTION_RECORD*.
void reagentLogFault(const char* where, const void* record, unsigned long tid);

// > 0 while the CALLING thread is inside one of the mod's own deliberate, SEH-handled probes
// (the replica slot scan, the prototype replica copy). The vectored handler must ignore those
// faults - they are handled, and they would otherwise both mislabel themselves and consume the
// handler's 32-line budget.
long reagentProbeDepth();
// Other files' deliberate, SEH-handled probes (ut_plate's caller-frame scan) raise the same
// thread-local around their reads so the vectored handler stays silent for them.
void reagentProbeEnter();
void reagentProbeLeave();

// The mod's identity state in one line, for the worker's stall check. Reads atomics and fixed
// buffers only - it never touches engine memory, so it is safe on a frozen game.
const char* reagentIdentityState(char* out, size_t cap);

// ---- the read-only engine surface the tab, the table and the tooltip share --------------------
// Every entry is one of this file's internal helpers given external linkage, so a caller sees
// exactly what a real deposit, a real take and the rescue command see. All of it is GAME THREAD
// only, all of it is SEH-guarded inside, and all of it is read-only.
struct UtReplicaCapture;  // ut_rescue.h - same namespace, declared there

// Is `record` one of the mod's page records (uniq-records.txt)?
bool reagentIsCollectionRecord(const char* record);
// Is `item` still a live Item whose object id is `id`?
bool reagentItemIsLive(GdItem* item, unsigned int id);
// The live object behind an object id, or nullptr.
void* reagentObjectFromId(unsigned int id);

}  // namespace ut
