// ut_miroll.h - monster infrequents (MIs) in the private table: ONE ROLL PER RECORD, the stored
// roll named in words, and the KIND seam to the catalogue. Pure logic: no engine call, no Win32,
// no global, no allocation - so tools\test_store.cpp, tools\test_journal.cpp and
// tools\test_tooltip.cpp prove the rules the shipped build runs.
//
// WHY THE CAP. A journal row is `record -> {count, ONE ItemReplicaInfo}`: journalDepositCommit
// REPLACES the row's identity with the newest capture and raises the count. An MI is a fixed
// Rare record whose prefix, suffix and seed are ROLLED, so two deposits of one MI record at
// `max_per_record = 2` would be count 2 with roll B - both takes return roll B and roll A is
// gone. A unique has no affixes, which is why the one-identity row was enough until now. So an MI
// record holds ONE roll whatever `max_per_record` says, and a second deposit of the same record
// is REFUSED through the deposit gate's existing refusal path (collectionAccepts -> `deposit
// REFUSED: <record> - <reason>`, the item stays on the cursor / in the bag), with a reason that
// names the stored roll so the player knows which copy the box already holds.
//
// WHAT A ROLL IS - the slots, established from the engine and from a decoded affixed item:
//   * THE ENGINE. `ItemReplicaInfo::WriteProperties` (Game.dll 1.3.0.8 rva 0x570810) is the one
//     serialiser the saves call for an item (EquipmentCtrl at 0x278A00 for its twelve slots,
//     Player at 0x3C74B0, PlayerInventoryCtrl at 0x3DD800, GameEngine at 0x2CA4B0 twice). It
//     writes, in order: string +0x08, string +0x28, string +0x48, string +0x70, string +0x100,
//     u32 +0x68, string +0x90, string +0xB0, u32 +0xD0, string +0xD8, u32 +0xF8, u32 +0xFC,
//     string +0x120, string +0x140, u32 +0x160, +0x178, +0x180, +0x17C (the `lea rdx,[rbx+N]` /
//     `mov edx,[rbx+N]` before each of its 18 writer calls).
//     The TRANSFER STASH is written through it: the save at 0x2C5F01 names its file through
//     0x2B4AC0 (`lea rdx,"transfer"` at 0x2B4D0E), serialises through 0x2CA4B0 - whose item
//     loop calls WriteProperties at 0x2CAD6D / 0x2CAF64 - and hands the buffer to
//     SaveManager::DirectWrite at 0x2C5FCD. So a transfer.gst item's fields ARE these, in order.
//   * THE ITEM. A 1.3.0.8 transfer.gst snapshot decoded offline (the stash cipher and the same
//     field order) holds SIX affixed items, and on
//     every one of them field 2 is a record under `records/items/lootaffixes/prefix/` and field 3
//     one under `records/items/lootaffixes/suffix/`. One of the six IS an MI:
//         records/items/gearaccessories/rings/b305c_ring.dbr   (Rare, level 55)
//         field 2 = records/items/lootaffixes/prefix/b_ar007_ar_b.dbr          (a Rare prefix)
//         field 3 = records/items/lootaffixes/suffix/a030b_off_dmg%fire_05_je.dbr
//         field 6 = seed 2115467035
//     So field 2 <- +0x28 is the PREFIX, field 3 <- +0x48 the SUFFIX and field 6 <- +0x68 the
//     SEED. That is the item the offline round trip in test_journal / test_store reproduces.
// A roll is therefore {prefix +0x28, suffix +0x48, seed +0x68}. The seed IS the rolled numbers
// (Grim Dawn recomputes every stat from record + affixes + seed when it builds the item).
//
// THE KIND SEAM. Which records are MIs is the catalogue's word (the catalogue marks each entry
// with a KIND; an MI is an equipment record whose own `itemClassification` is Rare, the engine's
// rule in Item::GetSymbolBitmapName 0x3115F0). The one accessor is
//     bool gdut::catIsMi(const gdut::ItemView& entry)      (declared in src\model\catalogue.h)
// `utCatIsMiOf` finds it by argument-dependent lookup when it is declared and answers false when
// it is not, so this file builds - and behaves as if MIs did not exist, with no MI anywhere - on
// a catalogue that has no KIND mark; `utCatHasKindOf` says which of the two this build is
// (the start-up log line `mi: ...` prints it, and the tests print it too).
#pragma once

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <utility>

namespace ut {

// ---- the roll ---------------------------------------------------------------------------------
const unsigned int kUtRollPrefixOff = 0x028;
const unsigned int kUtRollSuffixOff = 0x048;
const unsigned int kUtRollSeedOff = 0x068;

struct UtRollKey {
    char prefix[160];  // the prefix affix record, "" = none
    char suffix[160];  // the suffix affix record, "" = none
    unsigned int seed;
    bool known;        // false = the replica was too short or a slot window covered the seed
};

inline void utRollClear(UtRollKey* k) {
    if (!k) return;
    k->prefix[0] = 0;
    k->suffix[0] = 0;
    k->seed = 0;
    k->known = false;
}

// The roll out of a capture-shaped identity: the recorded string slots (offset + text) and the
// raw blob. A slot that was not recorded reads as "" (an empty std::string window is recorded as
// an empty slot on a real capture, so absent and empty mean the same thing: no affix). The seed
// is the u32 at +0x68 unless a recorded string window covers it (then the blob is not this layout
// and the roll is not known).
inline bool utRollFromSlots(const unsigned int* slotOff, const char (*slotText)[160], int slotCount,
                            const unsigned char* replica, unsigned int replicaLen, UtRollKey* out) {
    if (!out) return false;
    utRollClear(out);
    if (!replica || replicaLen < kUtRollSeedOff + 4) return false;
    for (int i = 0; i < slotCount; ++i) {
        const unsigned int off = slotOff[i];
        if (off <= kUtRollSeedOff && kUtRollSeedOff < off + 0x20) return false;
        if (off == kUtRollPrefixOff) {
            strncpy_s(out->prefix, sizeof(out->prefix), slotText[i], _TRUNCATE);
        } else if (off == kUtRollSuffixOff) {
            strncpy_s(out->suffix, sizeof(out->suffix), slotText[i], _TRUNCATE);
        }
    }
    memcpy(&out->seed, replica + kUtRollSeedOff, 4);
    out->known = true;
    return true;
}

// Two rolls are the same roll when the prefix, the suffix (record paths, case-insensitive) and
// the seed all match. Unknown never matches anything.
inline bool utRollSame(const UtRollKey& a, const UtRollKey& b) {
    if (!a.known || !b.known) return false;
    return a.seed == b.seed && _stricmp(a.prefix, b.prefix) == 0 &&
           _stricmp(a.suffix, b.suffix) == 0;
}

// "records/items/lootaffixes/prefix/b_ar007_ar_b.dbr" -> "b_ar007_ar_b". The catalogue carries no
// affix names (affixes are not collection items), so the record's leaf is the honest word.
inline void utRollLeaf(const char* record, char* out, size_t cap) {
    if (!out || !cap) return;
    out[0] = 0;
    if (!record || !*record) return;
    const char* s = record;
    for (const char* p = record; *p; ++p) {
        if (*p == '/' || *p == '\\') s = p + 1;
    }
    strncpy_s(out, cap, s, _TRUNCATE);
    const size_t n = strlen(out);
    if (n > 4 && _stricmp(out + n - 4, ".dbr") == 0) out[n - 4] = 0;
}

// The roll in words, for the refusal reason, the rescue report and the log:
//   "Chthonian Devourer" level 55: prefix b_ar007_ar_b, suffix a030b_off_dmg%fire_05_je,
//   seed 2115467035
// `name` "" and `level` <= 0 are left out; an empty affix reads "no prefix" / "no suffix".
inline void utRollDescribe(const UtRollKey& k, const char* name, int level, char* out,
                           size_t cap) {
    if (!out || !cap) return;
    out[0] = 0;
    char head[200];
    head[0] = 0;
    if (name && *name && level > 0) {
        _snprintf_s(head, sizeof(head), _TRUNCATE, "\"%s\" level %d: ", name, level);
    } else if (name && *name) {
        _snprintf_s(head, sizeof(head), _TRUNCATE, "\"%s\": ", name);
    } else if (level > 0) {
        _snprintf_s(head, sizeof(head), _TRUNCATE, "level %d: ", level);
    }
    if (!k.known) {
        _snprintf_s(out, cap, _TRUNCATE, "%sroll not readable", head);
        return;
    }
    char p[160], s[160];
    utRollLeaf(k.prefix, p, sizeof(p));
    utRollLeaf(k.suffix, s, sizeof(s));
    _snprintf_s(out, cap, _TRUNCATE, "%s%s%s, %s%s, seed %u", head, p[0] ? "prefix " : "no prefix",
                p, s[0] ? "suffix " : "no suffix", s, k.seed);
}

// ---- the cap ----------------------------------------------------------------------------------
// The record's KIND as the deposit gate sees it: 1 = an MI, 0 = not (or this build's catalogue
// has no KIND mark), -1 = unknown (the catalogue could not be read, or the record is not in it).
const int kUtCapAccept = 0;
const int kUtCapRefuseMax = 1;         // max_per_record (the existing reason)
const int kUtCapRefuseMiRoll = 2;      // an MI record already holds its one roll
const int kUtCapRefuseKindUnknown = 3; // a second copy of a record whose kind cannot be read

// `held` is what the collection holds of the record now (the table's count, heldSumFrom).
// THE ORDER: the MI test comes first, so a second MI roll is refused with the reason that names
// the stored roll even at max_per_record = 1, where the max test alone would also refuse it. An
// UNKNOWN kind refuses a second copy only when max_per_record would otherwise allow one (0 =
// unlimited, or above 1): a second copy of an MI would cost its first roll, and "I cannot tell"
// must never become "it is not one".
inline int utRecordCapDecide(int held, int maxPerRecord, int kind) {
    if (kind == 1 && held >= 1) return kUtCapRefuseMiRoll;
    if (maxPerRecord > 0 && held >= maxPerRecord) return kUtCapRefuseMax;
    if (kind < 0 && held >= 1) return kUtCapRefuseKindUnknown;
    return kUtCapAccept;
}

// ---- the KIND seam ----------------------------------------------------------------------------
// No function named `catIsMi` may be declared in namespace ut: the lookup below must find the
// catalogue's own (argument-dependent, in the entry's namespace) or nothing.
namespace mikind_detail {
template <class E>
auto probe(const E& e, int) -> decltype(static_cast<bool>(catIsMi(e))) {
    return static_cast<bool>(catIsMi(e));
}
template <class E>
bool probe(const E&, long) {
    return false;
}
template <class E>
constexpr auto has(int) -> decltype(static_cast<void>(catIsMi(std::declval<const E&>())), true) {
    return true;
}
template <class E>
constexpr bool has(long) {
    return false;
}
}  // namespace mikind_detail

// The entry's KIND mark through the catalogue's `catIsMi(entry)`; false when there is none.
template <class E>
bool utCatIsMiOf(const E& entry) {
    return mikind_detail::probe(entry, 0);
}

// True when `catIsMi(const E&)` is declared for this entry type, i.e. this build can see MIs.
template <class E>
constexpr bool utCatHasKindOf() {
    return mikind_detail::has<E>(0);
}

}  // namespace ut
