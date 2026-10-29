// ut_padpage.h - the category pad for a group table of any size: which button is which, and the
// pages when the buttons do not all fit with a readable caption. Pure: no Windows, no engine;
// tools\test_layout.cpp links it.
//
// LOGICAL button indexes - what the hit test returns, the click maps and the log prints:
//   0          the game's own materials page (MAT)
//   1..G       collection group i - 1, in uniq-groups.txt order, captioned by its G line's tag
//   G + 1      the owned-only filter (OWN)
//   G + 2      the next pad page (only while the pad is paged)
//   G + 3      the layout editor (EDIT; only while [advanced] edit_button=1, the default)
// A pad that holds every button draws them in that order but for EDIT, which is drawn just
// before OWN: OWN stays the last, double-wide cell. So the vanilla table (G = 24) without EDIT
// draws the 26 buttons 1.1.0 drew, one for one, and with it MAT, the groups, EDIT, OWN. A pad
// with no layout whose smallest button carries a size-6 caption is PAGED: each page draws MAT,
// K groups, NEXT, EDIT (when on) and OWN, K the largest count that fits. The page on screen is the one holding the
// group on screen; NEXT selects the first group of the next page (wrapping), so a click on it is
// an ordinary group click and the pad follows the group.
//
// ONE GRID PER BUTTON: a button of the buttons file that holds several groups shows
// them as ONE page - a virtual group whose entries are its groups' entries concatenated in group
// order, with one row window, one label, one owned filter (utUnionPlan and below). The page is
// keyed by the union's first group, so every per-group structure keeps real group indexes; a
// one-group button has no union and is exactly the page it was.
#pragma once

#include <vector>

namespace ut {

const int kUtPadButtonsMax = 64;   // buttons drawn at once; a larger table is paged

struct UtPadPages {
    int groups;    // G
    int perPage;   // K groups per page (G when not paged)
    int pages;     // 1 = not paged
    int buttons;   // buttons drawn per page: G + 2 + edit, or K + 3 + edit when paged
    int edit;      // 1 = the EDIT button (logical G + 3) is on the pad
};

inline int utPadOwnIndex(const UtPadPages& p) { return p.groups + 1; }
inline int utPadNextIndex(const UtPadPages& p) { return p.groups + 2; }
inline int utPadEditIndex(const UtPadPages& p) { return p.groups + 3; }
// One past the largest logical index (NEXT only while paged, EDIT only while on - an unpaged
// pad with EDIT has no button G + 2).
inline int utPadLogicalCount(const UtPadPages& p) {
    return p.edit ? p.groups + 4 : p.groups + (p.pages > 1 ? 3 : 2);
}

// `fits(n)` says whether the pad in force lays out n buttons with a readable caption. Every
// button when they fit (and n <= kUtPadButtonsMax); else the largest K with K + 3 (+ EDIT)
// fitting; else the whole table unpaged (the pad in force then draws nothing, as an unfitting
// pad always did). `edit` = 1 adds the EDIT button to every page.
template <class Fits>
inline UtPadPages utPadPaginate(int groups, int edit, Fits fits) {
    UtPadPages p = {groups < 0 ? 0 : groups, 0, 1, 0, edit ? 1 : 0};
    p.perPage = p.groups;
    p.buttons = p.groups + 2 + p.edit;
    if (p.buttons <= kUtPadButtonsMax && fits(p.buttons)) return p;
    int k = p.groups - 1;
    if (k > kUtPadButtonsMax - 3 - p.edit) k = kUtPadButtonsMax - 3 - p.edit;
    for (; k >= 1; --k) {
        if (fits(k + 3 + p.edit)) {
            p.perPage = k;
            p.pages = (p.groups + k - 1) / k;
            p.buttons = k + 3 + p.edit;
            return p;
        }
    }
    return p;
}
// The pad without the EDIT button.
template <class Fits>
inline UtPadPages utPadPaginate(int groups, Fits fits) {
    return utPadPaginate(groups, 0, fits);
}

// The page group `group` is on (0 for the vanilla page and while not paged).
inline int utPadPageOf(const UtPadPages& p, int group) {
    if (p.pages <= 1 || p.perPage < 1 || group < 0) return 0;
    const int pg = group / p.perPage;
    return pg < p.pages ? pg : p.pages - 1;
}

// The logical index drawn in `slot` (0 .. buttons - 1) of page `page`; -1 = an empty slot (the
// last page's unused group cells) or a slot out of range. OWN is always the last slot (the wide
// one), EDIT the slot before it while on, NEXT the slot before those while paged.
inline int utPadSlotLogical(const UtPadPages& p, int page, int slot) {
    if (slot < 0 || slot >= p.buttons) return -1;
    if (slot == 0) return 0;
    if (slot == p.buttons - 1) return utPadOwnIndex(p);
    if (p.edit && slot == p.buttons - 2) return utPadEditIndex(p);
    if (p.pages <= 1) return slot;
    if (slot == p.buttons - 2 - p.edit) return utPadNextIndex(p);
    const int g = page * p.perPage + (slot - 1);
    return g < p.groups ? g + 1 : -1;
}

// NEXT's target group: the first group of the page after `page`, the first page after the last.
inline int utPadNextTarget(const UtPadPages& p, int page) {
    if (p.pages <= 1 || p.perPage < 1) return 0;
    return ((page + 1) % p.pages) * p.perPage;
}

// ---- the pad keyed by BUTTONS (uniquetab-buttons.ini: a button may hold several groups, a group
// may be on none - [unassigned]) ----------------------------------------------------------------

// Ctrl+wheel / Ctrl+PageUp/Down walk the pad's BUTTONS in the order the pad draws them (the
// buttons file's order), never the group indexes (a group table whose order differs from the
// file's - DGD's Blueprints, group 25, appended once as the LAST button after REL GEM AUG OTH -
// would make a walk by index visit BLU between REL and GEM). The stops: the materials page (-1),
// then per button, in button order, the group its click selects - its FIRST group, the rep of its
// union (one grid per button), so a several-group button is one stop. `buttonOf(g)` = group g's
// button, 0-based in file order, -1 = on no button ([unassigned]: never a stop - the wheel
// reaches what the pad reaches); n = the group table's size. One button per group (no buttons
// file, the generated file: buttonOf(g) = g) is the plain group order. MAT / OWN / NEXT
// are not buttons of the file and not stops (the materials page is the one stop of MAT).
template <class ButtonOf>
inline std::vector<int> utPadWheelStops(int n, ButtonOf buttonOf) {
    if (n < 0) n = 0;
    std::vector<int> first((size_t)(n > 0 ? n : 1), -1);   // per button: its first group (B <= n)
    for (int g = 0; g < n; ++g) {
        const int b = buttonOf(g);
        if (b >= 0 && b < n && first[(size_t)b] < 0) first[(size_t)b] = g;
    }
    std::vector<int> stops(1, -1);
    for (int b = 0; b < n; ++b)
        if (first[(size_t)b] >= 0) stops.push_back(first[(size_t)b]);
    return stops;
}

// One step (`step` < 0 back, > 0 on) from `cur` over those stops, wrapping. `cur` = any group of
// a button (a union's other member too: a restored state) steps from that button; -1 and a group
// on no button (a page reached another way) step from the materials page. The result is always a
// stop: -1 or the first group of a button.
template <class ButtonOf>
inline int utPadWheelButton(int cur, int step, int n, ButtonOf buttonOf) {
    if (n < 0) n = 0;
    const std::vector<int> stops = utPadWheelStops(n, buttonOf);
    const int b = cur >= 0 && cur < n ? buttonOf(cur) : -1;
    int at = 0;
    if (b >= 0)
        for (size_t i = 1; i < stops.size(); ++i)
            if (buttonOf(stops[i]) == b) {
                at = (int)i;
                break;
            }
    const int count = (int)stops.size();
    int next = at + (step < 0 ? -1 : 1);
    if (next < 0) next = count - 1;
    if (next >= count) next = 0;
    return stops[(size_t)next];
}

// The label's {owned} / {entries} for the button holding `groups` (its groups, any order): both
// summed over them; an unknown owned count (< 0) of any of them makes the sum unknown (-1), an
// unknown entry count (< 0) counts 0. One group = that group's own numbers.
template <class Owned, class Entries>
inline void utPadButtonCounts(const int* groups, int count, Owned owned, Entries entries, int* ownedOut,
                              int* entriesOut) {
    int o = 0, e = 0;
    bool known = true;
    for (int i = 0; i < count; ++i) {
        const int go = owned(groups[i]);
        if (go < 0) known = false;
        else o += go;
        const int ge = entries(groups[i]);
        if (ge > 0) e += ge;
    }
    if (ownedOut) *ownedOut = known ? o : -1;
    if (entriesOut) *entriesOut = e;
}

// ---- one grid per button: the union of a several-group button's groups ----------------------

// The unions of a group table of `n` groups. `groupsOf(g, out, cap)` writes the groups of g's
// button into out (at most cap, ascending = group order) and returns how many it has (0 = g is on
// no button). A button with two or more groups is one union, keyed by its FIRST group (the rep);
// every other group - alone on its button or on none - is its own rep and has no union.
struct UtUnionPlan {
    std::vector<int> rep;                    // per group: the group whose page shows it
    std::vector<int> unionOf;                // per group: its union (index into members) when it
                                             // is a union's rep, else -1
    std::vector<std::vector<int>> members;   // per union: its groups, ascending
};

template <class GroupsOf>
inline UtUnionPlan utUnionPlan(int n, GroupsOf groupsOf) {
    UtUnionPlan p;
    if (n < 0) n = 0;
    p.rep.resize((size_t)n);
    p.unionOf.assign((size_t)n, -1);
    for (int g = 0; g < n; ++g) p.rep[(size_t)g] = g;
    std::vector<int> buf((size_t)(n > 0 ? n : 1));
    for (int g = 0; g < n; ++g) {
        if (p.rep[(size_t)g] != g) continue;   // already placed in an earlier group's union
        int k = groupsOf(g, buf.data(), n);
        if (k > n) k = n;
        std::vector<int> m;
        for (int i = 0; i < k; ++i)
            if (buf[(size_t)i] >= 0 && buf[(size_t)i] < n) m.push_back(buf[(size_t)i]);
        for (size_t i = 1; i < m.size(); ++i)   // group order, whatever the caller gave
            for (size_t j = i; j > 0 && m[j - 1] > m[j]; --j) {
                const int t = m[j - 1];
                m[j - 1] = m[j];
                m[j] = t;
            }
        if (m.size() < 2 || m[0] != g) continue;   // alone (or not this group's button: no union)
        for (int x : m) p.rep[(size_t)x] = g;
        p.unionOf[(size_t)g] = (int)p.members.size();
        p.members.push_back(m);
    }
    return p;
}

// The group whose page shows `group`: its union's first group; -1 (the materials page) and an
// index outside the table are returned unchanged.
inline int utUnionRep(const UtUnionPlan& p, int group) {
    if (group < 0 || (size_t)group >= p.rep.size()) return group;
    return p.rep[(size_t)group];
}

// The members of the union `group` is shown in (itself alone when it has none). Never empty for
// a group of the table; empty for -1.
inline std::vector<int> utUnionMembers(const UtUnionPlan& p, int group) {
    if (group < 0) return std::vector<int>();
    const int r = utUnionRep(p, group);
    if ((size_t)r < p.unionOf.size() && p.unionOf[(size_t)r] >= 0)
        return p.members[(size_t)p.unionOf[(size_t)r]];
    return std::vector<int>(1, group);
}

// The union's cell: the smallest cell size of the view (by area, then width) that holds every
// member's cell, so the page keeps a plate painted for its lattice; else the largest width by the
// largest height (no plate of the view has that size: the page shows the plain cover). `exact` =
// 1 when the cell is one of the view's. One member = its own cell.
struct UtCell {
    int w, h;
};
inline UtCell utUnionCell(const std::vector<UtCell>& members, const std::vector<UtCell>& view, int* exact) {
    UtCell need = {1, 1};
    for (const UtCell& c : members) {
        if (c.w > need.w) need.w = c.w;
        if (c.h > need.h) need.h = c.h;
    }
    int best = -1;
    for (size_t i = 0; i < view.size(); ++i) {
        const UtCell& c = view[i];
        if (c.w < need.w || c.h < need.h) continue;
        if (best < 0) { best = (int)i; continue; }
        const UtCell& b = view[(size_t)best];
        const long a1 = (long)c.w * c.h, a0 = (long)b.w * b.h;
        if (a1 < a0 || (a1 == a0 && c.w < b.w)) best = (int)i;
    }
    if (exact) *exact = best >= 0 ? 1 : 0;
    return best >= 0 ? view[(size_t)best] : need;
}

// Entry `k` of a union: the members' entries concatenated in group order, each group's in its own
// order. `count(g)` = group g's entry count (< 0 counts 0). False past the end.
template <class Count>
inline bool utUnionLocate(const std::vector<int>& members, Count count, int k, int* group, int* index) {
    if (k < 0) return false;
    for (int g : members) {
        int c = count(g);
        if (c < 0) c = 0;
        if (k < c) {
            if (group) *group = g;
            if (index) *index = k;
            return true;
        }
        k -= c;
    }
    return false;
}

// The union row whose first item is the first visible item of `group`'s own page at `row` (plain
// rows, `groupCols` wide): that item's place in the union - the members before `group` first -
// over the union's `unionCols`. A group that is no member keeps its row.
template <class Count>
inline int utUnionRowOf(const std::vector<int>& members, Count count, int group, int row, int groupCols,
                        int unionCols) {
    if (row < 0) row = 0;
    long first = (long)row * (long)(groupCols > 0 ? groupCols : 1);
    bool member = false;
    for (int g : members) {
        if (g == group) {
            member = true;
            break;
        }
        const int c = count(g);
        if (c > 0) first += c;
    }
    if (!member) return row;
    return unionCols > 0 ? (int)(first / unionCols) : 0;
}

// Does any of `groups` carry the search mark (a button's mark: tier B marks the union's first
// group, tiers A and C the group of the matching record)?
template <class Marked>
inline bool utUnionMarked(const int* groups, int count, Marked marked) {
    for (int i = 0; i < count; ++i)
        if (marked(groups[i])) return true;
    return false;
}

}  // namespace ut
