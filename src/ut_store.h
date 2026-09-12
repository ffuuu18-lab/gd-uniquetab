// ut_store.h - the collection outside the game's save
//
// THE PRIVATE TABLE: our records never enter the engine's reagent map at `GameEngine+0x36D80` at
// all, so the engine never writes them into `reagents.gst`, never reads them back and never
// refunds them - and the one-way door (uninstalling the mod strips every item the engine holds
// for it) ceases to exist. The declarations in this first half deliberately change NOTHING about
// the deposit path: for them the engine's map stays authoritative for counts, takes and the
// keep-alive.
//
// What is here: THE PRIVATE TABLE - a mod-owned `record -> {count, identity}`
// table loaded from the journal, used by `showBox` to point a box at an identity prototype the
// MOD built instead of relying on `ReagentWindow::Sync` re-pointing it at the map's prototype.
//
// THREADING. Everything that touches engine memory here is GAME THREAD ONLY and is reached from
// `reagentLateLoadTick(true)` (i.e. the existing `Engine::PresentSurface` tick). The worker thread
// only ever reads published atomics through `storeStatus()`.
#pragma once

#include <windows.h>

#include "gd_runtime.h"

namespace ut {

// Called once from reagentInit, after the exports are resolved. Never fails hard.
void storeInit();

// The tick. `gameThread` mirrors reagentLateLoadTick's own argument: the engine-memory work runs
// only when it is true. Safe to call before a world exists.
void storeTick(bool gameThread);

// Called from reagentOnWorldTeardown: every id in the display table belongs to the world that
// created it, exactly like ut_live's prototype cache.
void storeOnWorldTeardown();

// One short line for the log / the stall check. Reads mod-owned atomics only.
const char* storeStatus();

// ---- the private table, display side ---------------------------------------------------------
// Returns a live object id for `record` when the private table holds it AND an identity prototype
// could be built for it, else 0. GAME THREAD ONLY - it can call Item::CreateItem. `showBox` is the
// only caller; see ut_live.cpp.
//
// `record` is the ITEM record (`records/items/...`), NEVER the box record the box was Loaded
// from. Handing over `g.entries[k]`, a `records/ui/caravan/reagents/uniq/pNN/box_MM.dbr` path,
// silently paints nothing: a box record has no journal entry, so every stored item bows out at
// the `stored != YES` line without a word in the log.
// nullptr / "" means "this box stands for no item" (the vanilla materials page) and is silent.
// Every OTHER way out that returns 0 names its reason (one line per world, counted per reason,
// reported in the parity and NOTHING TO COMPARE lines).
unsigned int storeDisplayProtoId(const char* record);

// (There is deliberately no `storeDisplayArmed()` pre-check for `showBox`: `storeDisplayProtoId`'s
// own first line already asks the paint gate, so such a predicate could only duplicate it - and a
// predicate nothing asks is dead weight.)

// ---- THE PRIVATE TABLE, the owning side ------------------------------------------------------
// `storeDisplayProtoId` appears in the block below and is already declared above. A repeated
// declaration of the same function is legal, and the duplicate is kept on purpose so that this
// block reads as one self-contained description of the table.
//
// WHAT THE TABLE IS, in this build: there is NO second container. The journal's own entry list
// IS the table - `count` on the entry (ut_rescue.h) is the row, the file is the authority and
// `ut_store.cpp` keeps only the per-WORLD prototype ids (`g_protoTable`). A fuller
// `record -> {count, identity, protoId}` map would be a second in-memory copy of the counts,
// i.e. a second authority, and two authorities drift. Everything
// below is therefore a thin, locked read of the journal plus the display cache.
struct UtReplicaCapture;  // ut_rescue.h - same namespace, declared there

// True while the table OWNS the collection (the journal is usable: it exists, has a path and is
// not read-only). The
// deposit, the take, the counts and the rescue all ask this before they choose a path.
bool storeTableOwns(void);

// The table's count for `record`, 0 when it holds none. Cheap, mod-owned, no engine call.
unsigned int storeCount(const char* record);

// Record one accepted deposit: count+1, "stored":true, the identity from `cap`. Returns false
// when the journal could not take it - and a false here MUST refuse the deposit (never a
// `true` from the choke point over an unrecorded item).
bool storeOnDeposit(const UtReplicaCapture& cap, const char* record);

// Record `n` copies leaving the page. Returns the new count. At 0 the entry is kept as history.
unsigned int storeOnTake(const char* record, unsigned int n);

// The prototype the page must paint for `record`, built lazily and cached per world (this is
// today's storeDisplayProtoId, with the count as the stack). 0 = none.
unsigned int storeDisplayProtoId(const char* record);      // unchanged name, new stack source

// ---- durability, and the read-only views -----------------------------------------------------
// `storeOnDeposit` is ALL-OR-NOTHING. It journals the row, flushes the FILE synchronously on the
// calling thread and, if either step fails, puts the previous row back exactly as it was before
// returning false. So a `true` from it means "this item is on the disk", which is the only thing
// that makes the deposit path's `return true` without `callAddOriginal()` safe.

// The two READ-ONLY views of the display cache the deposit and take paths need:
// Both are pure reads of `g_protoTable` under the display lock. NEITHER EVER BUILDS A PROTOTYPE:
// they answer "what has this world already built?", which is what the take path and the identity
// arm need, and a build from either of them would be an engine call from whatever thread asked.
// A record with no prototype yet is 0 / absent, never an error.
//
// `storeBuiltProtoId` is `storeDisplayProtoId` with the building half removed. Note the CACHED
// REFUSAL: a record the world has already refused to build is stored as 0, so 0 means both "not
// built yet" and "cannot be built in this world" - do not use it to decide whether a record
// exists, only whether there is a prototype in hand.
unsigned int storeBuiltProtoId(const char* record);

// Every record the display cache has a LIVE prototype for (a cached 0 is skipped), with the
// prototype id and the table's count for that record. `out` / `protoIds` / `counts` may each be
// null; the return is the number of rows, and it is capped at `cap` whenever `out` is given.
// The counts are read AFTER the display lock is released - the journal's lock is never taken
// under the display lock (the ordering ut_store.cpp's parity pass already obeys).
int storeCollectBuilt(char (*out)[256], unsigned int* protoIds, unsigned int* counts, int cap);

}  // namespace ut
