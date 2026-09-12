// ut_ownedfold.h - the PURE arithmetic of the display side of the private table.
//
// Header-only on purpose, and for the same reason `ut_rowmath.h` and `ut_textfold.h` are:
// `tools\test_rowfold.cpp` then links THE VERY CODE the mod runs, not a copy of it. Nothing in
// this file touches Windows, the engine, the journal, its lock, or a global - it is three rules
// over plain data.
//
// THERE IS DELIBERATELY NO `count from the "stored" mark` HELPER HERE. Reading a row's count off
// its `"stored"` mark (YES -> one copy) contradicts what a count means in format 4: `count` is
// the number of copies that live ONLY in the mod's own file, a format-3 file upgrades with EVERY
// count 0, and a `"stored":true` row inherited from `reagents.gst` is MAP-OWNED (ut_rescue.h).
// The count comes from `journalCollectStored` and from nowhere else.
//
// WHAT IS DELIBERATELY NOT HERE: the transition SUM itself, `held = tableCount + mapHeld`. That
// arithmetic belongs to exactly ONE function - `reagentHeldTotal()` - so that it cannot drift,
// and a second copy of it in a header everyone includes is precisely how it would.
// What is here is the DISPLAY side's own three questions: is this row inventory at
// all, what shape must its key have to join the owned set, and how does a row move the change
// detector.
#pragma once

#include <stddef.h>

namespace ut {

// The count-0 invariant: `count >= 1` is the ONLY authorisation to paint a box, to let a
// take succeed, or to count a record as owned. A row at count 0 is HISTORY, never inventory.
inline bool utOwnedRowIsInventory(unsigned int count) {
    return count >= 1u;
}

// The owned set is keyed exactly the way the engine's own map keys are copied out of it
// (`nodeKeyCopy`, ut_plate.cpp): ASCII lower case, with '\' folded to '/'. `plateOwns()` is a
// plain hash lookup over those strings and is CASE-SENSITIVE, so a journal key that arrives in
// any other shape would become a second, invisible member of the set - owned by nobody.
//
// False (and `out` left empty) when there is no room for the whole key: a truncated key is a
// WRONG key, and a wrong key would silently claim a record the user does not own.
inline bool utOwnedNormaliseKey(const char* in, char* out, size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = 0;
    if (!in || !*in || cap < 2) return false;
    size_t n = 0;
    for (; in[n] && n + 1 < cap; ++n) {
        char c = in[n];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c == '\\') c = '/';
        out[n] = c;
    }
    if (in[n]) {
        out[0] = 0;
        return false;
    }
    out[n] = 0;
    return n > 0;
}

// FNV-1a 64 over the normalised key and then the four bytes of the count - the same hash and the
// same constants the map half of `plateOwnedRefresh` folds its nodes with, so the two halves of
// one fingerprint are made of the same arithmetic.
inline unsigned long long utOwnedRowHash(const char* key, unsigned int count) {
    unsigned long long h = 14695981039346656037ULL;
    if (key) {
        for (const char* p = key; *p; ++p) {
            h = (h ^ (unsigned long long)(unsigned char)*p) * 1099511628211ULL;
        }
    }
    for (int b = 0; b < 4; ++b) {
        h = (h ^ (unsigned long long)((count >> (b * 8)) & 0xFFu)) * 1099511628211ULL;
    }
    return h;
}

// The rows are SUMMED into the mix, never chained through it, and that is the whole point: the
// journal's entry order is not stable the way the engine's sorted map is (an upsert replaces a row
// in place, a new record appends), so an order-sensitive fold would move the change detector - and
// ask for a relayout - once a second for ever, for no reason. Unsigned overflow wraps, which is
// exactly what keeps the sum commutative and associative.
inline void utOwnedMixRow(unsigned long long* mix, const char* key, unsigned int count) {
    if (!mix) return;
    *mix += utOwnedRowHash(key, count);
}

}  // namespace ut
