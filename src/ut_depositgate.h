// ut_depositgate.h - the ONE question `hk_AddItemToReagents` asks, as pure logic - and nothing
// else.
//
// THE RULE THE MOD IS HELD TO: the game's own save files stay clean and fully vanilla. Nothing
// but real reagents may ever end up in `reagents.gst`; everything the mod stores belongs in the
// mod's own files.
//
// So with the private table on there are exactly TWO answers for one of our page records, and
// "hand it to the engine" is not one of them:
//
//   kUtDepTable   - the mod journals the row, flushes it to disk and returns TRUE WITHOUT calling
//                   the original. That `true` is the caller's licence to destroy the source, so it
//                   is only ever given when the caller's own removal is proved to happen exactly
//                   once (see kUtDepCaller* below).
//   kUtDepRefuse* - `return false` without calling the original. The mod adds nothing, removes
//                   nothing, and the item is exactly where it was.
//
// A third answer is conceivable - fall through to `callAddOriginal` and let the record become
// MAP-OWNED - and it is exactly the one that refills `reagents.gst` behind the player's back: a
// fall-through on any common path puts hundreds of the mod's items into the game's save. The
// verdict enum below therefore has no such value.
//
// WHY IT IS A HEADER. `ut_store.cpp` and `ut_reagent.cpp` cannot be linked without the engine, so
// a policy that lives inside them can only be argued about, never run. This is the
// `ut_paintgate.h` / `ut_ledger.h` / `ut_rowmath.h` shape - no Windows, no engine, no file, no
// globals - so `tools\test_store.cpp` proves the real decision function the shipped build calls,
// not a copy of it. In particular it proves the property the whole rule rests on:
// **every verdict except kUtDepTable is reached without the journal being touched at all.**
#pragma once

namespace ut {

// WHICH caller reached the choke point, because they need DIFFERENT proofs that the source is
// removed exactly once. Every address below was read off a dump of the loaded (decrypted) exe
// image and Game.dll.
enum UtDepositCallerKind {
    // Not one of the two the mod has read instruction by instruction. exe site B, the "deposit
    // all" button (`InventorySack::DepositSackIntoReagents`), and anything a future patch adds.
    kUtDepCallerUnknown = 0,
    // `CursorHandlerItemMove::PrimaryReagentActivate` (Game.dll 0x1738E0), return address
    // 0x173972. The source is THE CURSOR SLOT: 0x173995 SendRemoveItemFromInventory takes the id
    // the handler read at 0x1738FF from cursorHandler+0x30, and 0x17399A writes 0 back into that
    // slot. No container is searched, so the removal cannot miss and the proof is unconditional.
    kUtDepCallerDrag = 1,
    // exe quick-move site A, return address 0x1EAB3E. TRUE -> 0x1EAB4E
    // `PlayerInventoryCtrl::RemoveItem(ic, id, true)` then 0x1EAB5D SendRemoveItemFromInventory.
    // RemoveItem searches the `mem::vector<InventorySack*>` at `ic+0x20..0x28`
    // - the character's own bags - AND NOTHING ELSE. So this caller's removal is guaranteed only
    // while the item is in one of those bags, which is what the bag proof asks the engine.
    kUtDepCallerExeA = 2
};

// The bag proof's tri-state, as `bagsHoldItem` (ut_reagent.cpp) reports it.
const int kUtBagNotAsked = -2;   // the defaults, and every path where the question is meaningless
const int kUtBagCannotAsk = -1;  // an export missing, the `this` chain did not validate, a fault
const int kUtBagNo = 0;          // in NONE of the bags RemoveItem searches
const int kUtBagYes = 1;         // in one of them

enum UtDepositVerdict {
    kUtDepTable = 0,             // journal it, flush it, return true without the original
    kUtDepRefuseTableOff,        // the journal cannot own anything right now
    kUtDepRefuseFaulted,         // the choke point could not classify this deposit at all
    kUtDepRefuseCaller,          // a caller whose removal the mod has not proved
    kUtDepRefuseBag,             // site A, and the item is not in a bag RemoveItem searches
    kUtDepRefuseMapUnreadable,   // the engine's map could not be read: fail closed
    kUtDepRefuseMapNode,         // reagents.gst still has a row for this record
    kUtDepRefuseCapture          // the identity could not be captured, so the row cannot be written
};

// Everything the decision is allowed to look at. Filled by the choke point from what it has
// already read; this header computes nothing of its own and calls nothing.
struct UtDepositFacts {
    int caller;        // UtDepositCallerKind
    int bagProof;      // kUtBag*
    bool tableOwns;    // storeTableOwns() - the journal is writable, has a path AND the mod
                       // knows which of the two collections is this character's
    bool faulted;      // the choke point's own classification threw (`key` was cleared)
    bool mapOk;        // the engine's reagent map could be read at all
    bool mapNode;      // ... and it still has a node for this record (even an EMPTY one)
    bool journal;      // journal=1
    bool haveCap;      // the ItemReplicaInfo was copied off the live item
    bool capUsable;    // ... and it carries the base-record slot at +0x08
};

// THE decision. The order matters and is the order the log lines read in:
//   1. the table cannot OWN anything this session - the journal is read-only (a newer file
//      format) or has no path. Gating the whole block on `storeTableOwns()` would fall through to
//      the engine here, which is a hole in the rule above; so the CALLER enters this function for
//      every one of our records and the fact is decided here instead. An unwritable row is a lost
//      item, so there is nothing to do but refuse;
//   2. the choke point's own classification threw. `key` is cleared in that catch, and a cleared
//      key must not read as "not ours". When the item was KNOWN to be one of ours, refusing costs
//      nothing and keeps the rule;
//   3. an unproved caller;
//   4. site A without the bag proof (kUtBagYes is the ONLY accepting value - "could not ask"
//      fails closed, exactly like every other unreadable engine answer in this mod);
//   5. an unreadable map (fail closed: the mod cannot tell whether the file already has the row);
//   6. a record the engine's map still has a node for - `ReagentWindow::Sync` re-points a box
//      whose record HAS a node (exe 0x13266B) and the engine never deletes a node on a take, so a
//      table row for it would be painted over and the take would die at 0x132AE0;
//   7. a capture the table cannot write down.
// Only when none of those fires does the journal get touched.
inline int utDepositDecide(const UtDepositFacts& f) {
    if (!f.tableOwns) return kUtDepRefuseTableOff;
    if (f.faulted) return kUtDepRefuseFaulted;
    if (f.caller == kUtDepCallerUnknown) return kUtDepRefuseCaller;
    if (f.caller == kUtDepCallerExeA && f.bagProof != kUtBagYes) return kUtDepRefuseBag;
    if (!f.mapOk) return kUtDepRefuseMapUnreadable;
    if (f.mapNode) return kUtDepRefuseMapNode;
    if (!f.journal || !f.haveCap || !f.capUsable) return kUtDepRefuseCapture;
    return kUtDepTable;
}

// True when the verdict means "the journal must not be touched by this deposit". The choke point
// asserts on it and `tools\test_store.cpp` proves it for every fact combination.
inline bool utDepositIsRefusal(int verdict) { return verdict != kUtDepTable; }

}  // namespace ut
