// ut_caps.h - every bound a collection can reach, sized from the data it bounds.
//
// No Windows, no engine and no global in it (the `ut_ownedfold.h` precedent), so
// `tools\test_store.cpp` (`--caps`) links the very arithmetic the mod runs.
//
// THE RULE (HANDOFF section 2, invariant 8): no buffer, cap or walk that holds one entry per
// record - a journal row, a catalogue record, an engine map node - is a fixed number. Each one is
// sized from the count it holds and grown when that count grows; the only fixed numbers left are
// CEILINGS that protect memory or end a walk over a corrupt structure, each with its bytes stated
// and each far above the largest catalogue there is (Grimarillion: 19,089 paged records; the base
// game 7,184 on 1.3.1.1, Reign of Terror 10,262, DeathGrimDawn 10,431, all with monster
// infrequents).
#pragma once

#include <stddef.h>

#include <vector>

namespace ut {

// Rows to hold for `rows` records: `rows + headroom`, at least 1, at most `ceiling`.
inline int utRowsFor(long long rows, int headroom, int ceiling) {
    if (rows < 0) rows = 0;
    long long want = rows + (long long)headroom;
    if (want < 1) want = 1;
    if (want > (long long)ceiling) want = (long long)ceiling;
    return (int)want;
}

// ---- 1. the owned fold's record buffer (ut_plate.cpp `plateTableFold`) ----------------------
// One row is a 256-byte record path plus its 4-byte count, both outputs of
// `journalCollectStored` in ONE allocation: the records first, then the counts (`cap * 256` is a
// multiple of 4, so the counts start aligned).
const size_t kUtTableStride = 256 + sizeof(unsigned int);   // 260 bytes a row
// Rows on top of the journal's when the buffer is sized, so a session of deposits does not grow
// it one row at a time.
const int kUtTableHeadroom = 256;
// The memory ceiling: 65,536 rows x 260 B = 17,039,360 bytes (16.25 MiB), 3.4 x Grimarillion's
// 19,089 records. The journal itself is refused over 64 MB (`readWholeFile`; a row is ~560 B, so
// ~120,000 rows can be read), so a file past this ceiling can exist: the walk then still COUNTS
// every row - `journalCollectStored` returns the whole count whatever it copies - and the caller
// adds the rows it could not copy to the label's number (ut_plate.cpp), with one WARN.
const int kUtTableRowsCeiling = 65536;

struct UtTableBuf {
    std::vector<char> bytes;
    int cap = 0;   // rows the buffer holds
    char (*recs())[256] { return cap ? (char(*)[256]) & bytes[0] : nullptr; }
    unsigned int* counts() { return cap ? (unsigned int*)&bytes[(size_t)cap * 256] : nullptr; }
};

// Grows `b` to `rows` rows (never shrinks; the old contents are not kept - every walk refills the
// whole buffer). False = the allocation failed and `b` is exactly what it was.
inline bool utTableBufGrow(UtTableBuf* b, int rows) {
    if (rows <= b->cap) return true;
    try {
        std::vector<char> next((size_t)rows * kUtTableStride);
        b->bytes.swap(next);
        b->cap = rows;
    } catch (...) {
        return false;
    }
    return true;
}

struct UtTableWalk {
    int rows = 0;              // every TABLE-OWNED row the journal holds - exact, always
    int copied = 0;            // how many of them are in the buffer (== rows unless truncated)
    int grewFrom = 0;          // the buffer's rows before this walk ...
    int grewTo = 0;            // ... and after it; 0 = it did not grow
    bool truncated = false;    // rows > copied: the ceiling, or a failed allocation
    bool allocFailed = false;
};

// One walk of the private table into `b`, sized from the data. `hint` is a row count to size
// for before the first walk (0 = none: the walk itself finds out - an empty buffer makes the
// first try a count-only walk, so a table that holds nothing allocates nothing); a walk that
// finds more rows than the buffer holds (deposits since) grows it to those rows + 256 and walks
// again. `collect` has `journalCollectStored`'s shape and contract:
// (char (*out)[256], unsigned int* counts, int cap) returns EVERY row and copies
// min(rows, cap); a null `out` only counts.
template <class Collect>
UtTableWalk utTableWalk(UtTableBuf* b, long long hint, Collect collect,
                        int ceiling = kUtTableRowsCeiling) {
    UtTableWalk w;
    const int start = b->cap;
    const int want = hint > 0 ? utRowsFor(hint, kUtTableHeadroom, ceiling) : 0;
    if (want > b->cap && !utTableBufGrow(b, want)) w.allocFailed = true;
    int n = 0;
    // Three tries: the journal can gain rows between two walks (a deposit on the game thread
    // while the worker writes), and each try sizes for the count the previous one returned.
    for (int tries = 0; tries < 3; ++tries) {
        n = collect(b->recs(), b->counts(), b->cap);
        if (n <= b->cap) break;
        const int next = utRowsFor(n, kUtTableHeadroom, ceiling);
        if (next <= b->cap) break;   // at the ceiling
        if (!utTableBufGrow(b, next)) {
            w.allocFailed = true;
            break;
        }
    }
    if (n < 0) n = 0;
    w.rows = n;
    w.copied = n < b->cap ? n : b->cap;
    w.truncated = w.copied < w.rows;
    if (b->cap != start) {
        w.grewFrom = start;
        w.grewTo = b->cap;
    }
    return w;
}

// ---- 2. the engine's reagent map walks (ut_reagent.cpp, ut_plate.cpp) -----------------------
// The map holds the vanilla crafting materials plus whatever rows of ours an older build put
// there: a format-3 journal's rows are map-owned (ut_rescue.h, "WHAT `count` MEANS"), and the
// custom games' page sets without monster infrequents are past 4,096 records (Reign of Terror
// 7,043, DeathGrimDawn 7,328; 5,824 and 5,786 of them not blueprints). Nothing of ours enters the
// map in this build - the takeover census names any row that is there - but a legacy map can be
// larger than any fixed guess, so every walk collects ALL nodes: `utCollectAll` sizes the node
// vector from the count the walk itself returns.
const int kUtMapNodesStart = 4096;   // 32 KB of pointers; the vanilla map is far smaller
const int kUtMapNodesHeadroom = 64;
// The only fixed number left in a map walk: the in-order walk gives up after this many nodes, so
// a corrupt or cyclic tree cannot spin the game thread. 1,048,576 nodes is 55 x Grimarillion's
// record count; the node vector at that size is 8 MB.
const int kUtMapWalkCeiling = 1 << 20;

// `collect(T* out, int cap)` returns the number of nodes the map holds (writing min(that, cap))
// or < 0 when the walk failed. Returns how many nodes are in `v` (all of them) or < 0.
template <class T, class Collect>
int utCollectAll(std::vector<T>* v, Collect collect, int start = kUtMapNodesStart,
                 int ceiling = kUtMapWalkCeiling) {
    try {
        if ((int)v->size() < start) v->resize((size_t)start);
    } catch (...) {
        return -1;
    }
    int n = -1;
    for (int tries = 0; tries < 3; ++tries) {
        n = collect(&(*v)[0], (int)v->size());
        if (n < 0) return n;
        if (n <= (int)v->size()) break;
        const int next = utRowsFor(n, kUtMapNodesHeadroom, ceiling);
        if (next <= (int)v->size()) break;
        try {
            v->resize((size_t)next);
        } catch (...) {
            break;
        }
    }
    return n < (int)v->size() ? n : (int)v->size();
}

// ---- 3. the private table's per-world prototype cap (ut_store.cpp `protoCapNow`) ------------
// One mod-owned display prototype (an engine `Item`) per record the page paints, built lazily and
// cached per world. The cap stops a record that cannot be built from being retried into a leak;
// it is derived from the journal and CEILINGED by the view's catalogue - no page can paint a
// record it does not have, so twice the paged records plus 64 is the most a world can ask for.
//
//     cap     = max(320, paintable x 2 + 64)            paintable = stored + not yet reconciled
//     ceiling = max(320, paged x 2 + 64), at most 65,536 (paged unknown: 65,536)
//
// Memory: an `Item` holds its `ItemReplicaInfo` at +0x538, 0x190 bytes long (ut_reagent.cpp), so
// one prototype is at least 1,736 bytes before what it loads. Base game 7,184 x 2 + 64 = 14,432;
// Grimarillion 19,089 x 2 + 64 = 38,242 (the ceiling holds it with 1.7 x headroom); the hard
// ceiling's worst case is 65,536 x 1,736 B = 114 MB, reachable only by a catalogue of more than
// 32,736 records browsed whole in one world - and a world builds one per record it SHOWS.
const int kUtProtoCapFloor = 320;
const int kUtProtoCapCeiling = 65536;

inline int utProtoCapCeiling(size_t paged) {
    if (!paged) return kUtProtoCapCeiling;
    size_t c = paged * 2 + 64;
    if (c < (size_t)kUtProtoCapFloor) c = (size_t)kUtProtoCapFloor;
    if (c > (size_t)kUtProtoCapCeiling) c = (size_t)kUtProtoCapCeiling;
    return (int)c;
}

inline int utProtoCapFor(size_t paintable, size_t paged) {
    size_t cap = paintable * 2 + 64;
    if (cap < (size_t)kUtProtoCapFloor) cap = (size_t)kUtProtoCapFloor;
    const size_t ceiling = (size_t)utProtoCapCeiling(paged);
    if (cap > ceiling) cap = ceiling;
    return (int)cap;
}

}  // namespace ut
