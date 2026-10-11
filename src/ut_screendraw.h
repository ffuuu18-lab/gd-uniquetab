#pragma once
// The SCREEN pad's draw, its pure half: the ground and its 1-px frame, the rectangles the
// screen pad's input reads (ut_padinput.h's UtPadScreenRects), and the proof that the draw site
// comes after the caravan in the HUD's own Draw.
//
// The draw site (src\ut_plate.cpp, "the SCREEN pad's draw site"). The HUD's Draw (vtable slot
// +0x20; 1.3.0.8 exe 0x20A860) draws its window children in a fixed order through
// `lea rcx,[rdi+<member>]; mov rax,[rcx]; ...; call [rax+0x20]`: the caravan (hud+0x4FD08) at
// 0x20B731, then 19 more children, then the ITEM ROLLOVER (hud+0xB800, vtable 0x31B370, Draw
// 0x236D00 - the item tooltip and its compare boxes: the main rollover at +0x50, the vector of
// side boxes at +0x2B8..+0x2C0, placed left/right of it) at 0x20B925, then the yes/no dialog
// (hud+0x7378), the cinematic text, the cursor item ([hud+0x4DEF8]) and the full-screen fades.
// The UI root (exe 0xBFD30) is drawn AFTER the world view that owns the HUD (Engine
// Display::Render 0x5F410 walks the widgets last-registered first: the world view, added on
// entering a game at 0xFB08, before the UI root added at start-up 0x13015), and it draws the
// button-hover tooltips (UI root+0x658, exe 0xEF780: a 500 ms delay, a per-frame text list).
// So the HEAD of the item rollover's Draw is after every HUD child that is not a tooltip, a
// dialog or the cursor item, and before every tooltip: "one level below the tooltips".
//
// Pure: <string.h> and the two headers it extends, nothing of Windows; tools\test_layout.cpp
// links it.
#include <string.h>

#include "ut_layout.h"
#include "ut_padinput.h"

namespace ut {

// ---- the ground and its frame -----------------------------------------------------------------
// The screen pad / label ground is `ground` (client px, the layout's L.screenPx, opaque kPadBack);
// its frame is `line` px wide, OUTSIDE the ground (the ground keeps the in-window pad's exact
// pixels, the cells and their gutters inside it never move): the top and left edges in the light
// line colour, the bottom and right edges in the dark one - the buttons' own border-box look.
// Whole pixels: the edges are rounded once (floor(v + 0.5)), every part is built from them.
struct UtScreenFrame {
    int x, y, w, h;          // the ground, rounded
    int ox, oy, ow, oh;      // the whole frame (ground + line on every side): the click-eater
    int top[4], left[4];     // x, y, w, h of the light edges
    int bottom[4], right[4]; // x, y, w, h of the dark edges
    int line;
};

inline int utScreenRound(float v) { return (int)(v >= 0.0f ? v + 0.5f : v - 0.5f); }

inline UtScreenFrame utScreenFrame(const UtRectF& ground, int line) {
    UtScreenFrame f;
    memset(&f, 0, sizeof(f));
    if (line < 1) line = 1;
    const int x0 = utScreenRound(ground.x), y0 = utScreenRound(ground.y);
    const int x1 = utScreenRound(ground.x + ground.w), y1 = utScreenRound(ground.y + ground.h);
    f.line = line;
    f.x = x0;
    f.y = y0;
    f.w = x1 - x0;
    f.h = y1 - y0;
    f.ox = x0 - line;
    f.oy = y0 - line;
    f.ow = f.w + 2 * line;
    f.oh = f.h + 2 * line;
    // top (light) and bottom (dark) run the whole outer width and own the four corners; left
    // (light) and right (dark) run the ground's height between them - no pixel drawn twice
    const int top[4] = {f.ox, f.oy, f.ow, line};
    const int left[4] = {f.ox, y0, line, f.h};
    const int bottom[4] = {f.ox, y1, f.ow, line};
    const int right[4] = {x1, y0, line, f.h};
    memcpy(f.top, top, sizeof(top));
    memcpy(f.left, left, sizeof(left));
    memcpy(f.bottom, bottom, sizeof(bottom));
    memcpy(f.right, right, sizeof(right));
    return f;
}

// ---- what the screen pad's input reads ---------------------------------------------------------
// Starts an empty publication (shown = 0, nothing in it).
inline void utScreenPubReset(UtPadScreenRects* p) {
    if (p) memset(p, 0, sizeof(*p));
}

// The pad: its whole frame (the ground and the 1-px edge are both click-eaters) and shown = 1.
inline void utScreenPubPad(UtPadScreenRects* p, const UtScreenFrame& f) {
    if (!p || f.w <= 0 || f.h <= 0) return;
    p->pad = UtPadScreenRect{(float)f.ox, (float)f.oy, (float)f.ow, (float)f.oh};
    p->shown = 1;
}

// One button as DRAWN: `r` window-local scaled px (the PadPlan's), `originX/Y` the origin the
// layout was resolved for, rounded exactly as the draw rounds it (`round(origin + r.x)` and
// `round(r.w)`), so the click and the paint agree to the pixel. `logical` = the logical button
// (what panelButtonHit returns). False when full or the rectangle is empty.
inline bool utScreenPubButton(UtPadScreenRects* p, float originX, float originY, float rx, float ry,
                              float rw, float rh, int logical) {
    if (!p || logical < 0) return false;
    if (p->buttonCount < 0) p->buttonCount = 0;
    if (p->buttonCount >= kUtPadScreenMaxButtons) return false;
    const int x = utScreenRound(originX + rx), y = utScreenRound(originY + ry);
    const int w = utScreenRound(rw), h = utScreenRound(rh);
    if (w <= 0 || h <= 0) return false;
    UtPadScreenButton& b = p->buttons[p->buttonCount++];
    b.r = UtPadScreenRect{(float)x, (float)y, (float)w, (float)h};
    b.index = logical;
    return true;
}

// The label: its whole frame, shown = 1.
inline void utScreenPubLabel(UtPadScreenRects* p, const UtScreenFrame& f) {
    if (!p || f.w <= 0 || f.h <= 0) return;
    p->label = UtPadScreenRect{(float)f.ox, (float)f.oy, (float)f.ow, (float)f.oh};
    p->shown = 1;
}

// ---- the draw site, proved from the HUD's own code ----------------------------------------------
// The offset of the HUD's draw call for the child at `member`: the first
// `48 8D 8F <member:le32>` (lea rcx,[rdi+member]) followed within `window` bytes by `FF 50 20`
// (call [rax+0x20]) inside `code[0..n)`; -1 when absent. Used on the LIVE HUD Draw (read under
// SEH by ut_plate.cpp) and offline over the exe image (test_layout).
inline long utHudChildDrawAt(const unsigned char* code, size_t n, unsigned member, size_t window = 24) {
    if (!code || n < 10) return -1;
    const unsigned char lea[7] = {0x48, 0x8D, 0x8F, (unsigned char)(member & 0xFF),
                                  (unsigned char)((member >> 8) & 0xFF),
                                  (unsigned char)((member >> 16) & 0xFF),
                                  (unsigned char)((member >> 24) & 0xFF)};
    for (size_t i = 0; i + sizeof(lea) <= n; ++i) {
        if (memcmp(code + i, lea, sizeof(lea)) != 0) continue;
        const size_t lim = i + sizeof(lea) + window;
        for (size_t j = i + sizeof(lea); j + 3 <= n && j + 3 <= lim; ++j) {
            if (code[j] == 0xFF && code[j + 1] == 0x50 && code[j + 2] == 0x20) return (long)i;
        }
    }
    return -1;
}

// How many times that draw call appears (the proof wants exactly one per child).
inline int utHudChildDrawCount(const unsigned char* code, size_t n, unsigned member,
                               size_t window = 24) {
    int c = 0;
    size_t at = 0;
    while (at < n) {
        const long i = utHudChildDrawAt(code + at, n - at, member, window);
        if (i < 0) break;
        ++c;
        at += (size_t)i + 7;
    }
    return c;
}

// The verdict the site needs: the item rollover is drawn by the HUD exactly once, the caravan
// exactly once, and the rollover AFTER the caravan. 0 = proved; else the reason.
enum UtSiteProof {
    kUtSiteProved = 0,
    kUtSiteNoCaravanCall,    // the caravan's draw call is not in the HUD's Draw (count != 1)
    kUtSiteNoRolloverCall,   // the rollover's draw call is not there (count != 1)
    kUtSiteRolloverFirst,    // the rollover is drawn BEFORE the caravan: it would be under it
};

inline UtSiteProof utHudSiteProof(const unsigned char* code, size_t n, unsigned caravanOff,
                                  unsigned rolloverOff, long* caravanAt, long* rolloverAt) {
    const long c = utHudChildDrawAt(code, n, caravanOff);
    const long r = utHudChildDrawAt(code, n, rolloverOff);
    if (caravanAt) *caravanAt = c;
    if (rolloverAt) *rolloverAt = r;
    if (c < 0 || utHudChildDrawCount(code, n, caravanOff) != 1) return kUtSiteNoCaravanCall;
    if (r < 0 || utHudChildDrawCount(code, n, rolloverOff) != 1) return kUtSiteNoRolloverCall;
    if (r < c) return kUtSiteRolloverFirst;
    return kUtSiteProved;
}

inline const char* utSiteProofText(UtSiteProof p) {
    switch (p) {
        case kUtSiteProved: return "proved";
        case kUtSiteNoCaravanCall: return "the HUD's Draw has no single draw call for the caravan";
        case kUtSiteNoRolloverCall:
            return "the HUD's Draw has no single draw call for the item rollover";
        case kUtSiteRolloverFirst: return "the HUD draws the item rollover before the caravan";
    }
    return "?";
}

// The one ini-less switch: where the screen parts are drawn. The rollover head is the site; the
// Draw tail (the ReagentWindow::Draw detour, under the 19 HUD children drawn after the caravan,
// possibly the minimap) is the documented fallback - change this constant and rebuild.
enum UtScreenSite { kUtScreenSiteRollover = 0, kUtScreenSiteDrawTail = 1 };
constexpr UtScreenSite kUtScreenSiteChosen = kUtScreenSiteRollover;

}  // namespace ut
