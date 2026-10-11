// test_padinput.cpp - the screen pad's press rules, proved without a game: src\ut_padinput.h, the
// real header the GetMouseEvent detour in ut_plate.cpp runs.
//
//   tools\build_test_padinput.bat        (needs a vcvars64 shell; the .bat calls it itself)
//
//   1. the event record: the stride decoder over the 1.3.0.8 bytes, utPadApply (rewrite / clear
//      touch exactly the type and the bytes 0x10..0x11), utPadReadEvent;
//   2. the rectangles: inside / outside / NaN, the button lookup, freshness at every edge;
//   3. INERT: nothing published, shown = 0, stale, not proven - every event type at every point is
//      untouched and nothing acts (the state before anything is published);
//   4. the button press: LEFT DOWN on a button acts once, every event type passed on during the
//      press is cleared, the LEFT UP is rewritten and ends it, the next event is untouched;
//   5. the ground, the label, the item-on-cursor rule (consumed, no action, a press all the same);
//   6. outside: nothing touched, a press started outside is the world's;
//   7. the polled release (two generations of grace, an UP in either still ends it normally), a
//      lost UP followed by a new DOWN, a press that outlives its rectangles;
//   8. idempotence per Update generation: a repeat replays the bytes and never acts again;
//   9. the editor hand-over: Ctrl+RIGHT DOWN on the pad and on the label, its RIGHT UP, a plain
//      RIGHT DOWN untouched; with the editor open the LEFT DOWN / MOVE / LEFT UP are the editor's;
//  10. the header's purity; with a game's x64\DirectInput.dll (optional, read only) the export by
//      name, its stride, its first instructions and the device vtable's slots +0x10 / +0x38;
//  11. every logical button kind (MAT, the buttons, OWN, NEXT), the adopted drag (an
//      editor drag begun in the window: cleared everywhere, its MOVEs and UP the editor's), a
//      right press ended by the next LEFT DOWN, a DOWN on which the poll ends a press;
//  12. recorded worlds replayed through the machine AND the editor
//      (ut_layoutedit.h): world 1 (the pinned label, the tall pad dragged out of the window, held
//      still, released with its UP lost, then the screen toolbar) and world 5 (the sequence that
//      worked live), each without and with the adopted drag; the Ctrl+right open from
//      the screen with its RIGHT UP lost, then CANCEL at once;
//  13. the window is the window's while the editor is open - a point inside the window's
//      published client rect is untouched (a LEFT DOWN only noted to the editor), a press begun on
//      the screen still runs to its UP, the adopted drag untouched inside and cleared outside, the
//      polled release inside, the window's freshness on its own;
//  14. recorded worlds replayed through both routes (a campaign one and a Reign of Terror one,
//      the pad pinned all world so the in-window probe
//      never ran): in the grounds-only wiring the part brought back into the window cannot be
//      moved, resized or saved (the window route refuses every click but CANCEL) - in the window
//      wiring it can, out and in again, resize inside, the toolbar inside and outside; section
//      12's world 1 in the window wiring.
// Exit code 0 = all of it held. Argument 1: the repository root (for the purity check).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "../src/ut_padinput.h"
#include "../src/ut_layoutedit.h"   // section 12: the editor half of the replays

using namespace ut;

static int g_fails = 0;
static int g_checks = 0;
static void check(bool ok, const char* what) {
    ++g_checks;
    printf("  %-78s %s\n", what, ok ? "OK" : "FAIL");
    if (!ok) ++g_fails;
}

// A top-right pad at 1920x1080: ground 1500,40 400x60, three buttons, a label under it.
static UtPadScreenRects makeRects(unsigned long long tick) {
    UtPadScreenRects r;
    memset(&r, 0, sizeof(r));
    r.shown = 1;
    r.tick = tick;
    r.pad = UtPadScreenRect{1500.0f, 40.0f, 400.0f, 60.0f};
    r.label = UtPadScreenRect{1500.0f, 104.0f, 400.0f, 24.0f};
    r.buttonCount = 3;
    r.buttons[0] = UtPadScreenButton{UtPadScreenRect{1504.0f, 44.0f, 60.0f, 24.0f}, 0};    // MAT
    r.buttons[1] = UtPadScreenButton{UtPadScreenRect{1568.0f, 44.0f, 60.0f, 24.0f}, 1};    // a group
    r.buttons[2] = UtPadScreenButton{UtPadScreenRect{1830.0f, 70.0f, 60.0f, 24.0f}, 25};   // OWN
    return r;
}

static UtPadInGates gates() {
    UtPadInGates g;
    memset(&g, 0, sizeof(g));
    g.fresh = 1;
    g.proven = 1;
    g.leftUp = 0;
    g.rightUp = 1;
    return g;
}

static UtPadInEvent ev(int type, float x, float y) { return UtPadInEvent{type, x, y}; }

static bool untouched(const UtPadInOut& o) {
    return !o.rewrite && !o.clear && o.act == kUtPadActNone;
}

// A fresh 0x1C-byte record as GetMouseEvent copies it.
static void record(unsigned char* b, int type, float x, float y, unsigned char l, unsigned char r) {
    memset(b, 0xAB, kUtMouseEventSize);
    memcpy(b + 0, &type, 4);
    memcpy(b + 4, &x, 4);
    memcpy(b + 8, &y, 4);
    b[0x10] = l;
    b[0x11] = r;
}

static const int kAllTypes[] = {kUtEvtMove, kUtEvtLeftDown, kUtEvtRightDown, 3, 4, 5, 6, 7, 8,
                                kUtEvtLeftUp, kUtEvtRightUp, 0xB, 0xC, kUtEvtWheelUp,
                                kUtEvtWheelDown};
static const int kTypeCount = (int)(sizeof(kAllTypes) / sizeof(kAllTypes[0]));

// ---- optional: the real DirectInput.dll, read as a file --------------------------------------
struct Pe {
    std::vector<unsigned char> img;
    unsigned long long base = 0;
    struct Sec { unsigned va, vsz, raw, rsz; };
    std::vector<Sec> secs;
    const unsigned char* at(unsigned rva, size_t n) const {
        for (const Sec& s : secs) {
            const unsigned span = s.vsz > s.rsz ? s.vsz : s.rsz;
            if (rva >= s.va && rva + n <= s.va + span) {
                const size_t off = (size_t)s.raw + (rva - s.va);
                if (off + n <= img.size()) return img.data() + off;
            }
        }
        return nullptr;
    }
};

static unsigned u32(const unsigned char* p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}
static unsigned long long u64(const unsigned char* p) {
    return (unsigned long long)u32(p) | ((unsigned long long)u32(p + 4) << 32);
}

static bool loadPe(const std::string& path, Pe* pe) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") || !f) return false;
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) pe->img.insert(pe->img.end(), buf, buf + n);
    fclose(f);
    if (pe->img.size() < 0x400 || pe->img[0] != 'M' || pe->img[1] != 'Z') return false;
    const unsigned nt = u32(&pe->img[0x3C]);
    if (nt + 0x108 > pe->img.size() || u32(&pe->img[nt]) != 0x4550) return false;
    const unsigned nsec = pe->img[nt + 6] | (pe->img[nt + 7] << 8);
    const unsigned optSize = pe->img[nt + 20] | (pe->img[nt + 21] << 8);
    const unsigned opt = nt + 24;
    if ((pe->img[opt] | (pe->img[opt + 1] << 8)) != 0x20B) return false;   // PE32+
    pe->base = u64(&pe->img[opt + 24]);
    const unsigned sh = opt + optSize;
    for (unsigned i = 0; i < nsec; ++i) {
        const unsigned char* s = &pe->img[sh + i * 40];
        pe->secs.push_back(Pe::Sec{u32(s + 12), u32(s + 8), u32(s + 20), u32(s + 16)});
    }
    return true;
}

static unsigned exportRva(const Pe& pe, const char* name) {
    const unsigned nt = u32(&pe.img[0x3C]);
    const unsigned opt = nt + 24;
    const unsigned dirRva = u32(&pe.img[opt + 112]);
    const unsigned char* d = pe.at(dirRva, 40);
    if (!d) return 0;
    const unsigned nNames = u32(d + 24), funcs = u32(d + 28), names = u32(d + 32),
                   ords = u32(d + 36);
    for (unsigned i = 0; i < nNames; ++i) {
        const unsigned char* np = pe.at(names + i * 4, 4);
        const unsigned char* op = pe.at(ords + i * 2, 2);
        if (!np || !op) return 0;
        const unsigned char* s = pe.at(u32(np), 1);
        if (!s || strcmp((const char*)s, name) != 0) continue;
        const unsigned ord = op[0] | (op[1] << 8);
        const unsigned char* fp = pe.at(funcs + ord * 4, 4);
        return fp ? u32(fp) : 0;
    }
    return 0;
}

static std::string findDirectInput() {
    const char* vars[] = {"UNIQUETAB_TEST_GAME_DIR", "UNIQUETAB_TEST_DBMODS_DIR"};
    for (const char* v : vars) {
        const char* dir = getenv(v);
        if (!dir || !*dir) continue;
        std::string base = dir;
        while (!base.empty() && (base.back() == '\\' || base.back() == '/')) base.pop_back();
        const std::string tries[] = {base + "\\x64\\DirectInput.dll",
                                     base + "\\..\\x64\\DirectInput.dll"};
        for (const std::string& t : tries) {
            FILE* f = nullptr;
            if (!fopen_s(&f, t.c_str(), "rb") && f) {
                fclose(f);
                return t;
            }
        }
    }
    return std::string();
}

// ---- section 13/14: the two-route rig (the editor's two routes, either wiring) ----------------
// The two pure halves wired as ut_plate (scrBody / scrEditor; editMouse: the window's mouse slot)
// and ut_panel (editTickDraw, editPublishScreen, panelLayoutEditMouse) wire them, on any geometry.
//   wiring 8, grounds only: the grounds of the screen parts; the window route accepts a click
//             only once the in-window pad's own probe proved the space (`winProven`);
//   wiring 9, shipped: the window's client rect published too (the screen input leaves the inside
//             to the window), the in-window LEFT DOWN noted, the window route proven by the note.
struct RigGeo {
    float S, OX, OY;
    int coverW, coverH, screenW, screenH;
};
struct Rig9 {
    RigGeo g;
    int wiring;
    int winProven;             // the in-window pad's own probe ran this world
    UtPadInState s;
    UtEdit e;
    UtPadScreenRects pub;
    UtEdPumpNote note;
    int winLatch;              // panelLayoutEditMouse's g_editWinProven
    unsigned long long frame, gen;
    int idx;
    float mx, my;
    int physL;
    int worldHeld, worldUps;   // outside the window, with no rewrite
    int lastAct;               // the editor's answer to the last LEFT DOWN it got
    int downsToEditor, downsViaScreen, downsViaWindow, notes;
    int lastDownRewritten;     // the machine's verdict on the last LEFT DOWN
};

static bool rigIn(const Rig9& r, float x, float y) {
    return x >= r.g.OX && x < r.g.OX + (float)r.g.coverW * r.g.S && y >= r.g.OY &&
           y < r.g.OY + (float)r.g.coverH * r.g.S;
}
static float rigX(const Rig9& r, float lx) { return r.g.OX + lx * r.g.S; }
static float rigY(const Rig9& r, float ly) { return r.g.OY + ly * r.g.S; }
static unsigned rigNow(const Rig9& r) { return 1000u + (unsigned)r.frame * 16u; }

static bool rigOpen(Rig9* r, const RigGeo& g, int wiring, int winProven, const UtRect& grid,
                    const UtRect& pad, int padScr, const UtRect& label, int labelScr) {
    memset(r, 0, sizeof(*r));
    r->g = g;
    r->wiring = wiring;
    r->winProven = winProven;
    utPadInReset(&r->s);
    UtEditOpenIn in = {};
    in.inForce[kUtLayGrid] = grid;
    in.inForce[kUtLayPad] = pad;
    in.inForce[kUtLayLabel] = label;
    in.def[kUtLayGrid] = grid;
    in.def[kUtLayPad] = UtRect{102, 25, 320, 44};
    in.def[kUtLayLabel] = UtRect{105, 8, 317, 16};
    in.padFallback = in.def[kUtLayPad];
    in.plate = UtRect{0, 0, g.coverW, g.coverH};
    in.maxCellW = 64;
    in.maxCellH = 128;
    in.rowGap = 2;
    in.cellW = 32;
    in.cellH = 32;
    in.scale = g.S;
    in.coverW = g.coverW;
    in.coverH = g.coverH;
    in.group = 12;
    in.now = 1000;
    in.screenIn[kUtLayPad] = padScr;
    in.screenIn[kUtLayLabel] = labelScr;
    in.screenKnown = 1;
    in.screenW = g.screenW;
    in.screenH = g.screenH;
    in.originX = g.OX;
    in.originY = g.OY;
    return utEditOpen(&r->e, in);
}

// One frame: the editor's tick and its publication (editPublishScreen of the wiring).
static void rigFrame(Rig9* r) {
    ++r->frame;
    UtEdTick t = {};
    t.now = rigNow(*r);
    t.group = 12;
    t.scale = r->g.S;
    t.focused = 1;
    t.coverW = r->g.coverW;
    t.coverH = r->g.coverH;
    t.layoutOk = 1;
    t.physL = r->physL;
    t.haveCursor = 1;
    t.cx = r->mx - r->g.OX;
    t.cy = r->my - r->g.OY;
    t.screenKnown = 1;
    t.screenW = r->g.screenW;
    t.screenH = r->g.screenH;
    t.originX = r->g.OX;
    t.originY = r->g.OY;
    int moved = 0;
    utEditTick(&r->e, t, &moved);
    memset(&r->pub, 0, sizeof(r->pub));
    if (r->e.state == kUtEdClosed) return;
    UtRectF pad, label;
    utEditScreenGrounds(r->e, &pad, &label);
    const UtRectF win = r->wiring >= 9 ? utEditWindowClient(r->e) : UtRectF{0, 0, 0, 0};
    if (pad.w > 0.0f || label.w > 0.0f || win.w > 0.0f) {
        r->pub.shown = 1;
        r->pub.tick = r->frame;
        if (pad.w > 0.0f) r->pub.pad = UtPadScreenRect{pad.x + r->g.OX, pad.y + r->g.OY, pad.w, pad.h};
        if (label.w > 0.0f)
            r->pub.label = UtPadScreenRect{label.x + r->g.OX, label.y + r->g.OY, label.w, label.h};
        if (win.w > 0.0f) r->pub.window = UtPadScreenRect{win.x + r->g.OX, win.y + r->g.OY, win.w, win.h};
    }
}

// One event of the pump: the machine first, then whoever the event still reaches.
static void rigEvent(Rig9* r, int type) {
    UtPadInGates g;
    memset(&g, 0, sizeof(g));
    g.fresh = utPadScreenRectsFresh(r->pub, r->frame) ? 1 : 0;
    g.proven = 1;   // the screen input's own probe (these worlds logged it)
    g.editorOpen = r->e.state != kUtEdClosed ? 1 : 0;
    g.leftUp = r->physL ? 0 : 1;
    g.rightUp = 1;
    g.editorDrag = r->e.state == kUtEdDrag ? 1 : 0;
    g.windowFresh = utPadScreenWindowFresh(r->pub, r->frame) ? 1 : 0;
    const UtPadInOut o = utPadInput(&r->s, r->gen, r->idx++, ev(type, r->mx, r->my), r->pub, g);
    const float lx = r->mx - r->g.OX, ly = r->my - r->g.OY;
    if (type == kUtEvtLeftDown) r->lastDownRewritten = o.rewrite;
    if (o.act == kUtPadActEditor) {   // scrEditor: proven = true
        if (o.editorType == kUtEvtLeftDown) {
            r->lastAct = utEditLeftDown(&r->e, lx, ly, 1, r->physL, 0, 0);
            ++r->downsToEditor;
            ++r->downsViaScreen;
        } else if (o.editorType == kUtEvtMove) {
            utEditDragTo(&r->e, lx, ly);
        } else if (o.editorType == kUtEvtLeftUp) {
            utEditLeftUp(&r->e, lx, ly);
        }
    } else if (o.act == kUtPadActEditorNote) {   // scrEditor(kUtEvtEditorNote)
        if (r->e.state != kUtEdClosed) utEditNoteDown(&r->note, lx, ly, rigNow(*r));
        ++r->notes;
    }
    if (o.rewrite) return;   // a MOVE with no held byte: nobody sees a click
    const int held = o.clear ? 0 : r->physL;
    if (rigIn(*r, r->mx, r->my)) {
        // the caravan's own slot -> editMouse -> panelLayoutEditMouse (LEFT DOWN / LEFT UP)
        if (type == kUtEvtLeftDown && r->e.state != kUtEdClosed) {
            bool ok = r->winProven != 0 || r->winLatch != 0;
            if (!ok && r->wiring >= 9 && utEditNoteProves(&r->note, lx, ly, rigNow(*r))) {
                ok = true;
                r->winLatch = 1;
            }
            r->lastAct = utEditLeftDown(&r->e, lx, ly, ok ? 1 : 0, r->physL, 0, 0);
            ++r->downsToEditor;
            ++r->downsViaWindow;
        } else if (type == kUtEvtLeftUp) {
            utEditLeftUp(&r->e, lx, ly);
        }
    } else {
        if (held) ++r->worldHeld;
        if (type == kUtEvtLeftUp) ++r->worldUps;
    }
}

static void rigUpdate(Rig9* r) {
    ++r->gen;
    r->idx = 0;
}

// A drag from client (x0, y0) to (x1, y1) in `steps` frames, a still half second, the release.
static void rigDrag(Rig9* r, float x0, float y0, float x1, float y1, int steps, bool upLost) {
    r->mx = x0;
    r->my = y0;
    r->physL = 1;
    rigUpdate(r);
    rigEvent(r, kUtEvtLeftDown);
    rigFrame(r);
    for (int i = 1; i <= steps; ++i) {
        r->mx = x0 + (x1 - x0) * (float)i / (float)steps;
        r->my = y0 + (y1 - y0) * (float)i / (float)steps;
        rigUpdate(r);
        rigEvent(r, kUtEvtMove);
        rigFrame(r);
    }
    for (int i = 0; i < 30; ++i) {
        rigUpdate(r);
        rigEvent(r, kUtEvtMove);
        rigFrame(r);
    }
    r->physL = 0;
    rigUpdate(r);
    rigEvent(r, upLost ? kUtEvtMove : kUtEvtLeftUp);
    rigFrame(r);
    for (int i = 0; i < 3; ++i) {
        rigUpdate(r);
        rigEvent(r, kUtEvtMove);
        rigFrame(r);
    }
}

// A drag by record px (dx, dy) from the window-local record point (lx, ly).
static void rigDragL(Rig9* r, float lx, float ly, float dx, float dy, bool upLost = false) {
    rigDrag(r, rigX(*r, lx), rigY(*r, ly), rigX(*r, lx + dx), rigY(*r, ly + dy), 8, upLost);
}

static int rigClick(Rig9* r, float x, float y) {
    r->mx = x;
    r->my = y;
    r->physL = 1;
    rigUpdate(r);
    r->lastAct = -1;
    rigEvent(r, kUtEvtLeftDown);
    r->physL = 0;
    rigEvent(r, kUtEvtLeftUp);
    rigFrame(r);
    return r->lastAct;
}

static int rigTool(Rig9* r, int b) {
    const UtRect& t = r->e.tool[b];
    return rigClick(r, rigX(*r, (float)t.x + 3.0f), rigY(*r, (float)t.y + 3.0f));
}

static bool rectIs(const UtRect& a, int x, int y, int w, int h) {
    return a.x == x && a.y == y && a.w == w && a.h == h;
}

int main(int argc, char** argv) {
    const std::string root = argc > 1 ? argv[1] : ".";
    const unsigned long long T = 1000;   // the frame the rectangles were drawn at
    const UtPadScreenRects R = makeRects(T);

    // ---------------------------------------------------------------------------------------
    printf("1. the event record\n");
    {
        // DirectInput.dll 1.3.0.8, GetMouseEvent (rva 0x4840), the whole 0x2E-byte body.
        const unsigned char body[] = {0x49, 0x63, 0xC0, 0x4C, 0x6B, 0xC0, 0x1C, 0x48, 0x8B, 0x81,
                                      0xB8, 0x02, 0x00, 0x00, 0x41, 0x0F, 0x10, 0x04, 0x00, 0xF2,
                                      0x41, 0x0F, 0x10, 0x4C, 0x00, 0x10, 0x41, 0x8B, 0x44, 0x00,
                                      0x18, 0x0F, 0x11, 0x02, 0xF2, 0x0F, 0x11, 0x4A, 0x10, 0x89,
                                      0x42, 0x18, 0x48, 0x8B, 0xC2, 0xC3};
        check(utPadDecodeEventStride(body, sizeof(body)) == (int)kUtMouseEventSize,
              "the 1.3.0.8 GetMouseEvent body decodes the 0x1C stride");
        unsigned char other[sizeof(body)];
        memcpy(other, body, sizeof(body));
        other[6] = 0x20;
        check(utPadDecodeEventStride(other, sizeof(other)) == 0x20,
              "a changed stride is decoded as it is (and refused by the mod: != 0x1C)");
        other[3] = 0x90;
        check(utPadDecodeEventStride(other, sizeof(other)) == 0,
              "no imul r8,rax,imm8 in the first 16 bytes = 0 (not found)");
        check(utPadDecodeEventStride(nullptr, 10) == 0 && utPadDecodeEventStride(body, 5) == 0,
              "a null or too short body = 0");

        unsigned char b[kUtMouseEventSize];
        record(b, kUtEvtLeftDown, 1530.0f, 50.0f, 1, 1);
        UtPadInOut o;
        memset(&o, 0, sizeof(o));
        o.rewrite = 1;
        utPadApply(b, o);
        int type = -1;
        float x = 0, y = 0;
        memcpy(&type, b, 4);
        memcpy(&x, b + 4, 4);
        memcpy(&y, b + 8, 4);
        bool rest = true;
        for (size_t i = 0x0C; i < kUtMouseEventSize; ++i) {
            if (i == 0x10 || i == 0x11) continue;
            if (b[i] != 0xAB) rest = false;
        }
        check(type == kUtEvtMove && x == 1530.0f && y == 50.0f && b[0x10] == 0 && b[0x11] == 0 &&
                  rest,
              "a rewrite: type -> MOVE, bytes 0x10..0x11 -> 0, x/y and every other byte kept");
        record(b, kUtEvtWheelUp, 10.0f, 20.0f, 1, 1);
        memset(&o, 0, sizeof(o));
        o.clear = 1;
        utPadApply(b, o);
        memcpy(&type, b, 4);
        check(type == kUtEvtWheelUp && b[0x10] == 0 && b[0x11] == 0 && b[0x12] == 0xAB,
              "a clear: the type kept, bytes 0x10..0x11 -> 0, byte 0x12 kept");
        record(b, kUtEvtLeftDown, 10.0f, 20.0f, 1, 0);
        memset(&o, 0, sizeof(o));
        utPadApply(b, o);
        memcpy(&type, b, 4);
        check(type == kUtEvtLeftDown && b[0x10] == 1, "an untouched decision writes nothing");
        record(b, kUtEvtRightDown, 12.5f, 7.25f, 0, 1);
        const UtPadInEvent e = utPadReadEvent(b);
        check(e.type == kUtEvtRightDown && e.x == 12.5f && e.y == 7.25f,
              "utPadReadEvent reads type, x and y at +0 / +4 / +8");
    }

    // ---------------------------------------------------------------------------------------
    printf("2. the rectangles\n");
    {
        check(utPadScreenIn(R.pad, 1500.0f, 40.0f) && !utPadScreenIn(R.pad, 1900.0f, 40.0f) &&
                  !utPadScreenIn(R.pad, 1499.9f, 50.0f) && !utPadScreenIn(R.pad, 1600.0f, 100.0f),
              "inside is [x, x+w) x [y, y+h)");
        const float nan = nanf("");
        check(!utPadScreenInAny(R, nan, 50.0f) && !utPadScreenInAny(R, 1600.0f, nan),
              "a NaN point is inside nothing");
        check(!utPadScreenIn(UtPadScreenRect{0, 0, 0, 10}, 0, 5) && !utPadScreenIn(UtPadScreenRect{0, 0, 10, -1}, 1, 0),
              "a rectangle with w or h <= 0 is absent");
        check(utPadScreenButtonAt(R, 1510.0f, 50.0f) == 0 && utPadScreenButtonAt(R, 1570.0f, 60.0f) == 1 &&
                  utPadScreenButtonAt(R, 1850.0f, 80.0f) == 25,
              "the button lookup returns the LOGICAL index (MAT 0, a group 1, OWN 25)");
        check(utPadScreenButtonAt(R, 1700.0f, 50.0f) == -1 && utPadScreenButtonAt(R, 1510.0f, 110.0f) == -1,
              "the pad's ground and the label have no button");
        UtPadScreenRects odd = R;
        odd.buttons[3] = UtPadScreenButton{UtPadScreenRect{100.0f, 100.0f, 50.0f, 50.0f}, 7};
        odd.buttonCount = 4;
        check(utPadScreenButtonAt(odd, 110.0f, 110.0f) == -1,
              "a button published outside the pad's ground is never hit");
        odd = R;
        odd.buttonCount = 100000;
        check(utPadScreenButtonAt(odd, 1510.0f, 50.0f) == 0, "a wild buttonCount is clamped");
        check(utPadScreenRectsFresh(R, T) && utPadScreenRectsFresh(R, T + kUtPadScreenStaleTicks),
              "fresh at the drawn frame and kUtPadScreenStaleTicks frames later");
        check(!utPadScreenRectsFresh(R, T + kUtPadScreenStaleTicks + 1), "stale one frame after that");
        check(!utPadScreenRectsFresh(R, T - 1), "a frame BEFORE the draw (a wrap) is stale");
        UtPadScreenRects z = R;
        z.tick = 0;
        check(!utPadScreenRectsFresh(z, 3), "never stamped (tick 0) is never fresh");
        z = R;
        z.shown = 0;
        check(!utPadScreenRectsFresh(z, T), "shown = 0 is never fresh");
        z = R;
        z.pad = UtPadScreenRect{0, 0, 0, 0};
        check(utPadScreenRectsFresh(z, T), "a label alone is fresh");
        z.label = UtPadScreenRect{0, 0, 0, 0};
        check(!utPadScreenRectsFresh(z, T), "neither a pad nor a label is never fresh");
        UtPadScreenRects none;
        memset(&none, 0, sizeof(none));
        check(!utPadScreenRectsFresh(none, 0) && !utPadScreenRectsFresh(none, T),
              "the zeroed struct (nothing published) is never fresh");
    }

    // ---------------------------------------------------------------------------------------
    printf("3. INERT until fresh rectangles and a proven space\n");
    {
        UtPadScreenRects none;
        memset(&none, 0, sizeof(none));
        const float pts[][2] = {{1510, 50}, {1700, 50}, {1600, 110}, {10, 10}, {1920, 1080}};
        int bad = 0, n = 0;
        UtPadInState s;
        utPadInReset(&s);
        unsigned long long gen = 1;
        for (int variant = 0; variant < 4; ++variant) {
            for (const auto& p : pts) {
                for (int t = 0; t < kTypeCount; ++t) {
                    for (int mod = 0; mod < 4; ++mod) {
                        UtPadInGates g = gates();
                        g.ctrl = mod & 1;
                        g.editorOpen = (mod >> 1) & 1;
                        const UtPadScreenRects* rr = &R;
                        if (variant == 0) { rr = &none; g.fresh = utPadScreenRectsFresh(none, T); }
                        if (variant == 1) g.fresh = utPadScreenRectsFresh(R, T + 50);
                        if (variant == 2) g.proven = 0;
                        if (variant == 3) { g.fresh = 0; g.proven = 0; }
                        const UtPadInOut o =
                            utPadInput(&s, gen++, 0, ev(kAllTypes[t], p[0], p[1]), *rr, g);
                        ++n;
                        if (!untouched(o) || s.press != kUtPressNone) ++bad;
                    }
                }
            }
        }
        char what[120];
        _snprintf_s(what, sizeof(what), _TRUNCATE,
                    "%d events (nothing published / stale / unproven) - all untouched, no press", n);
        check(bad == 0, what);
        // And the bytes: the detour's whole effect with nothing published is the original's copy.
        unsigned char b[kUtMouseEventSize], keep[kUtMouseEventSize];
        record(b, kUtEvtLeftDown, 1510.0f, 50.0f, 1, 0);
        memcpy(keep, b, sizeof(b));
        UtPadInGates g = gates();
        g.fresh = utPadScreenRectsFresh(none, T);
        const UtPadInOut o = utPadInput(&s, gen++, 0, ev(kUtEvtLeftDown, 1510.0f, 50.0f), none, g);
        utPadApply(b, o);
        check(memcmp(b, keep, sizeof(b)) == 0 && o.why == kUtPadWhyInert,
              "a LEFT DOWN on where a button would be, nothing published: bytes identical");
    }

    // ---------------------------------------------------------------------------------------
    printf("4. the button press\n");
    {
        UtPadInState s;
        utPadInReset(&s);
        UtPadInGates g = gates();
        unsigned long long gen = 10;
        UtPadInOut o = utPadInput(&s, gen, 0, ev(kUtEvtLeftDown, 1570.0f, 60.0f), R, g);
        check(o.rewrite && o.act == kUtPadActButton && o.button == 1 && o.why == kUtPadWhyButton &&
                  s.press == kUtPressButton,
              "LEFT DOWN on a button: rewritten, the button's action, a press");
        int bad = 0, acts = 0;
        int idx = 1;
        for (int t = 0; t < kTypeCount; ++t) {
            if (kAllTypes[t] == kUtEvtLeftUp || kAllTypes[t] == kUtEvtLeftDown) continue;
            o = utPadInput(&s, gen, idx++, ev(kAllTypes[t], 300.0f + t, 900.0f), R, g);
            if (!o.clear || o.rewrite || o.why != kUtPadWhyHeld) ++bad;
            if (o.act != kUtPadActNone) ++acts;
        }
        check(bad == 0 && acts == 0,
              "every other event type passed on during the press: bytes cleared, type kept");
        ++gen;
        o = utPadInput(&s, gen, 0, ev(kUtEvtMove, 1570.0f, 60.0f), R, g);
        check(o.clear && !o.rewrite && o.act == kUtPadActNone,
              "a MOVE back over the same button in a later generation: cleared, no second action");
        o = utPadInput(&s, gen, 1, ev(kUtEvtLeftUp, 200.0f, 200.0f), R, g);
        check(o.rewrite && o.ended == 1 && o.why == kUtPadWhyUp && s.press == kUtPressNone,
              "the LEFT UP, wherever it is: rewritten, the press ends");
        o = utPadInput(&s, gen, 2, ev(kUtEvtMove, 1570.0f, 60.0f), R, g);
        check(untouched(o), "the next MOVE (over the button): untouched - hover is the game's");
        o = utPadInput(&s, gen, 3, ev(kUtEvtLeftDown, 1850.0f, 80.0f), R, g);
        check(o.rewrite && o.act == kUtPadActButton && o.button == 25,
              "a click on OWN: its logical index handed over");
        o = utPadInput(&s, gen, 4, ev(kUtEvtLeftUp, 1850.0f, 80.0f), R, g);
        check(o.rewrite && o.ended == 1, "and its UP");
        // the bytes over a whole click
        unsigned char b[kUtMouseEventSize];
        ++gen;
        record(b, kUtEvtLeftDown, 1510.0f, 50.0f, 1, 0);
        o = utPadInput(&s, gen, 0, utPadReadEvent(b), R, g);
        utPadApply(b, o);
        int type = -1;
        memcpy(&type, b, 4);
        const bool downOk = type == kUtEvtMove && b[0x10] == 0;
        record(b, kUtEvtMove, 1600.0f, 300.0f, 1, 0);
        o = utPadInput(&s, gen, 1, utPadReadEvent(b), R, g);
        utPadApply(b, o);
        memcpy(&type, b, 4);
        const bool moveOk = type == kUtEvtMove && b[0x10] == 0;
        record(b, kUtEvtLeftUp, 1600.0f, 300.0f, 0, 0);
        o = utPadInput(&s, gen, 2, utPadReadEvent(b), R, g);
        utPadApply(b, o);
        memcpy(&type, b, 4);
        check(downOk && moveOk && type == kUtEvtMove,
              "the bytes over a whole click: no consumer ever sees a DOWN, a held byte or an UP");
    }

    // ---------------------------------------------------------------------------------------
    printf("5. the ground, the label, an item on the cursor\n");
    {
        UtPadInState s;
        utPadInReset(&s);
        UtPadInGates g = gates();
        UtPadInOut o = utPadInput(&s, 20, 0, ev(kUtEvtLeftDown, 1700.0f, 50.0f), R, g);
        check(o.rewrite && o.act == kUtPadActNone && o.why == kUtPadWhyGround &&
                  s.press == kUtPressKept,
              "LEFT DOWN on the pad's ground: consumed, no action, a press");
        o = utPadInput(&s, 20, 1, ev(kUtEvtMove, 10.0f, 10.0f), R, g);
        check(o.clear, "its held MOVE out over the world: cleared");
        o = utPadInput(&s, 20, 2, ev(kUtEvtLeftUp, 10.0f, 10.0f), R, g);
        check(o.rewrite && o.ended == 1, "its UP over the world: rewritten");
        o = utPadInput(&s, 21, 0, ev(kUtEvtLeftDown, 1600.0f, 110.0f), R, g);
        check(o.rewrite && o.act == kUtPadActNone && o.why == kUtPadWhyGround,
              "LEFT DOWN on the label: consumed, no action");
        o = utPadInput(&s, 21, 1, ev(kUtEvtLeftUp, 1600.0f, 110.0f), R, g);
        check(o.rewrite && o.ended == 1, "and its UP");
        g.cursorItem = 1;
        o = utPadInput(&s, 22, 0, ev(kUtEvtLeftDown, 1570.0f, 60.0f), R, g);
        check(o.rewrite && o.act == kUtPadActNone && o.why == kUtPadWhyCursorItem &&
                  s.press == kUtPressKept,
              "ITEM ON THE CURSOR, LEFT DOWN on a button: consumed, NO action (the item stays)");
        o = utPadInput(&s, 22, 1, ev(kUtEvtLeftUp, 1570.0f, 60.0f), R, g);
        check(o.rewrite && o.ended == 1, "its UP: rewritten - no drop reaches the world either");
        g.editorOpen = 1;
        o = utPadInput(&s, 23, 0, ev(kUtEvtLeftDown, 1570.0f, 60.0f), R, g);
        check(o.rewrite && o.act == kUtPadActEditor && o.editorType == kUtEvtLeftDown &&
                  o.why == kUtPadWhyEditor && s.press == kUtPressEditor,
              "an item on the cursor with the editor open: consumed (the item stays), handed to "
              "the editor - which acts on CANCEL only");
        o = utPadInput(&s, 23, 1, ev(kUtEvtLeftUp, 1570.0f, 60.0f), R, g);
        check(o.rewrite && o.ended == 1 && o.act == kUtPadActEditor,
              "and its UP: rewritten - no drop reaches the world - and handed to the editor");
        g.editorOpen = 0;
        g.ctrl = 1;
        o = utPadInput(&s, 24, 0, ev(kUtEvtRightDown, 1700.0f, 50.0f), R, g);
        check(o.rewrite && o.act == kUtPadActNone && o.why == kUtPadWhyCursorItem,
              "an item on the cursor, Ctrl+RIGHT DOWN on the pad: consumed, no editor");
        o = utPadInput(&s, 24, 1, ev(kUtEvtRightUp, 1700.0f, 50.0f), R, g);
        check(o.rewrite && o.ended == 1, "and its RIGHT UP");
    }

    // ---------------------------------------------------------------------------------------
    printf("6. outside the rectangles\n");
    {
        UtPadInState s;
        utPadInReset(&s);
        UtPadInGates g = gates();
        int bad = 0;
        int idx = 0;
        for (int t = 0; t < kTypeCount; ++t) {
            for (int mod = 0; mod < 2; ++mod) {
                g.ctrl = mod;
                const UtPadInOut o = utPadInput(&s, 30, idx++, ev(kAllTypes[t], 1499.0f, 50.0f), R, g);
                if (!untouched(o) || s.press != kUtPressNone) ++bad;
            }
        }
        check(bad == 0, "every event type, Ctrl up or down, one pixel left of the pad: untouched");
        g.ctrl = 0;
        UtPadInOut o = utPadInput(&s, 31, 0, ev(kUtEvtLeftDown, 800.0f, 500.0f), R, g);
        check(untouched(o) && s.press == kUtPressNone, "a LEFT DOWN on the world: the world's");
        o = utPadInput(&s, 31, 1, ev(kUtEvtMove, 1570.0f, 60.0f), R, g);
        check(untouched(o), "dragged onto a button: the held MOVE is still the world's");
        o = utPadInput(&s, 31, 2, ev(kUtEvtLeftUp, 1570.0f, 60.0f), R, g);
        check(untouched(o), "released over the button: the UP is the world's, nothing acts");
        o = utPadInput(&s, 32, 0, ev(kUtEvtRightDown, 1570.0f, 60.0f), R, g);
        check(o.rewrite && o.act == kUtPadActNone && o.why == kUtPadWhyGround &&
                  s.press == kUtPressRight,
              "a plain RIGHT DOWN on a pad button: consumed, no action (no skill under the pad)");
        o = utPadInput(&s, 32, 1, ev(kUtEvtMove, 1575.0f, 62.0f), R, g);
        check(o.clear && !o.rewrite && s.press == kUtPressRight,
              "  the MOVE during that right press: the held bytes cleared");
        o = utPadInput(&s, 32, 2, ev(kUtEvtRightUp, 1575.0f, 62.0f), R, g);
        check(o.rewrite && o.ended && s.press == kUtPressNone,
              "  its RIGHT UP: rewritten, the press ends");
        o = utPadInput(&s, 33, 0, ev(kUtEvtRightDown, 1600.0f, 110.0f), R, g);
        check(o.rewrite && o.act == kUtPadActNone && s.press == kUtPressRight,
              "a plain RIGHT DOWN on the label: consumed, no action");
        o = utPadInput(&s, 33, 1, ev(kUtEvtRightUp, 1600.0f, 110.0f), R, g);
        o = utPadInput(&s, 34, 0, ev(kUtEvtRightDown, 800.0f, 500.0f), R, g);
        check(untouched(o) && s.press == kUtPressNone,
              "a plain RIGHT DOWN on the world: the world's (the skill click)");
        o = utPadInput(&s, 34, 1, ev(0x11, 1570.0f, 60.0f), R, g);
        check(untouched(o), "the wheel over the pad: passes (zooms the camera)");
    }

    // ---------------------------------------------------------------------------------------
    printf("7. the polled release, a lost UP, a press that outlives its rectangles\n");
    {
        UtPadInState s;
        utPadInReset(&s);
        UtPadInGates g = gates();
        UtPadInOut o = utPadInput(&s, 40, 0, ev(kUtEvtLeftDown, 1570.0f, 60.0f), R, g);
        g.leftUp = 1;   // released, but no UP event ever arrives
        o = utPadInput(&s, 40, 1, ev(kUtEvtMove, 1570.0f, 60.0f), R, g);
        const bool g0 = o.clear && s.press == kUtPressButton;
        o = utPadInput(&s, 41, 0, ev(kUtEvtMove, 1570.0f, 60.0f), R, g);
        const bool g1 = o.clear && s.press == kUtPressButton;
        o = utPadInput(&s, 42, 0, ev(kUtEvtMove, 1570.0f, 60.0f), R, g);
        check(g0 && g1, "the poll says up: the press holds through that generation and the next");
        check(o.ended == 2 && o.clear && s.press == kUtPressNone,
              "two generations later with no UP: the polled release ends it (that event cleared)");
        o = utPadInput(&s, 42, 1, ev(kUtEvtMove, 1570.0f, 60.0f), R, g);
        check(untouched(o), "the event after it: untouched");

        g.leftUp = 0;
        o = utPadInput(&s, 50, 0, ev(kUtEvtLeftDown, 1700.0f, 50.0f), R, g);
        g.leftUp = 1;
        o = utPadInput(&s, 50, 1, ev(kUtEvtMove, 1700.0f, 50.0f), R, g);
        o = utPadInput(&s, 51, 0, ev(kUtEvtLeftUp, 1700.0f, 50.0f), R, g);
        check(o.rewrite && o.ended == 1,
              "the poll saw up first, the UP came one generation later: rewritten as its UP");

        g.leftUp = 0;
        o = utPadInput(&s, 60, 0, ev(kUtEvtLeftDown, 1570.0f, 60.0f), R, g);
        g.leftUp = 1;
        o = utPadInput(&s, 60, 1, ev(kUtEvtMove, 1570.0f, 60.0f), R, g);
        g.leftUp = 0;   // pressed again before the grace ran out
        o = utPadInput(&s, 61, 0, ev(kUtEvtMove, 1570.0f, 60.0f), R, g);
        o = utPadInput(&s, 62, 0, ev(kUtEvtMove, 1570.0f, 60.0f), R, g);
        o = utPadInput(&s, 63, 0, ev(kUtEvtMove, 1570.0f, 60.0f), R, g);
        check(o.clear && s.press == kUtPressButton, "a poll that goes down again re-arms the grace");
        o = utPadInput(&s, 63, 1, ev(kUtEvtLeftDown, 1510.0f, 50.0f), R, g);
        check(o.ended == 3 && o.rewrite && o.act == kUtPadActButton && o.button == 0 &&
                  s.press == kUtPressButton && s.pressButton == 0,
              "a LEFT DOWN while the press is open (lost UP): the old one ends, the new one acts");
        o = utPadInput(&s, 63, 2, ev(kUtEvtLeftUp, 1510.0f, 50.0f), R, g);
        o = utPadInput(&s, 64, 0, ev(kUtEvtLeftDown, 1700.0f, 50.0f), R, g);
        o = utPadInput(&s, 64, 1, ev(kUtEvtLeftDown, 800.0f, 500.0f), R, g);
        check(o.ended == 3 && untouched(o) && s.press == kUtPressNone,
              "a lost UP then a DOWN on the world: the old press ends, the DOWN is the world's");

        // the rectangles go stale mid-press (the caravan closed): the press still runs to its UP
        utPadInReset(&s);
        g = gates();
        o = utPadInput(&s, 70, 0, ev(kUtEvtLeftDown, 1570.0f, 60.0f), R, g);
        g.fresh = 0;
        o = utPadInput(&s, 71, 0, ev(kUtEvtMove, 1570.0f, 60.0f), R, g);
        const bool held = o.clear;
        o = utPadInput(&s, 71, 1, ev(kUtEvtLeftUp, 1570.0f, 60.0f), R, g);
        check(held && o.rewrite && o.ended == 1,
              "rectangles stale mid-press: still cleared, and the UP still rewritten");
        o = utPadInput(&s, 72, 0, ev(kUtEvtLeftDown, 1570.0f, 60.0f), R, g);
        check(untouched(o), "and the next DOWN with stale rectangles: untouched");
    }

    // ---------------------------------------------------------------------------------------
    printf("8. idempotence per Update generation\n");
    {
        UtPadInState s;
        utPadInReset(&s);
        UtPadInGates g = gates();
        const UtPadInOut a = utPadInput(&s, 80, 0, ev(kUtEvtLeftDown, 1850.0f, 80.0f), R, g);
        const UtPadInOut b = utPadInput(&s, 80, 0, ev(kUtEvtLeftDown, 1850.0f, 80.0f), R, g);
        check(a.act == kUtPadActButton && b.act == kUtPadActNone && b.repeat && b.rewrite &&
                  a.rewrite,
              "the same (generation, index) twice: the same rewrite, the OWN toggle only once");
        unsigned char x1[kUtMouseEventSize], x2[kUtMouseEventSize];
        record(x1, kUtEvtLeftDown, 1850.0f, 80.0f, 1, 0);
        record(x2, kUtEvtLeftDown, 1850.0f, 80.0f, 1, 0);
        utPadApply(x1, a);
        utPadApply(x2, b);
        check(memcmp(x1, x2, sizeof(x1)) == 0, "byte for byte the same record both times");
        const UtPadInOut c = utPadInput(&s, 80, 1, ev(kUtEvtMove, 10.0f, 10.0f), R, g);
        const UtPadInOut d = utPadInput(&s, 80, 1, ev(kUtEvtMove, 10.0f, 10.0f), R, g);
        check(c.clear && d.clear && d.repeat, "a held event repeated: cleared both times");
        const UtPadInOut u1 = utPadInput(&s, 80, 2, ev(kUtEvtLeftUp, 10.0f, 10.0f), R, g);
        const UtPadInOut u2 = utPadInput(&s, 80, 2, ev(kUtEvtLeftUp, 10.0f, 10.0f), R, g);
        check(u1.rewrite && u1.ended == 1 && u2.rewrite && u2.repeat && u2.ended == 0,
              "the UP repeated: rewritten both times, the press ended once");
        // a stuck generation counter: the same key, a different event = a new decision
        const UtPadInOut k1 = utPadInput(&s, 80, 3, ev(kUtEvtLeftDown, 1850.0f, 80.0f), R, g);
        const UtPadInOut k2 = utPadInput(&s, 80, 3, ev(kUtEvtLeftUp, 1850.0f, 80.0f), R, g);
        const UtPadInOut k3 = utPadInput(&s, 80, 3, ev(kUtEvtLeftDown, 800.0f, 500.0f), R, g);
        check(k1.act == kUtPadActButton && !k2.repeat && k2.ended == 1 && !k3.repeat &&
                  untouched(k3),
              "the same key with a DIFFERENT event (a stuck counter): decided anew, a world "
              "click is never eaten by an old decision");
        const UtPadInOut n1 = utPadInput(&s, 81, 0, ev(kUtEvtLeftDown, 1850.0f, 80.0f), R, g);
        check(n1.act == kUtPadActButton && !n1.repeat,
              "the same index in the NEXT generation is a new event: it acts");
        utPadInput(&s, 81, 1, ev(kUtEvtLeftUp, 1850.0f, 80.0f), R, g);
        // the editor hand-over is not repeated either
        g.editorOpen = 1;
        const UtPadInOut e1 = utPadInput(&s, 82, 0, ev(kUtEvtLeftDown, 1700.0f, 50.0f), R, g);
        const UtPadInOut e2 = utPadInput(&s, 82, 0, ev(kUtEvtLeftDown, 1700.0f, 50.0f), R, g);
        check(e1.act == kUtPadActEditor && e2.act == kUtPadActNone && e2.rewrite,
              "a repeat of an editor hand-over: rewritten, not handed over twice");
    }

    // ---------------------------------------------------------------------------------------
    printf("9. the editor hand-over\n");
    {
        UtPadInState s;
        utPadInReset(&s);
        UtPadInGates g = gates();
        g.ctrl = 1;
        UtPadInOut o = utPadInput(&s, 90, 0, ev(kUtEvtRightDown, 1700.0f, 50.0f), R, g);
        check(o.rewrite && o.act == kUtPadActEditor && o.editorType == kUtEvtRightDown &&
                  o.why == kUtPadWhyCtrlRight && s.press == kUtPressRight,
              "Ctrl+RIGHT DOWN on the pad's ground: handed to the editor's entry, rewritten");
        o = utPadInput(&s, 90, 1, ev(kUtEvtMove, 1710.0f, 50.0f), R, g);
        check(o.clear && o.act == kUtPadActNone, "its held MOVE: cleared");
        o = utPadInput(&s, 90, 2, ev(kUtEvtRightUp, 1710.0f, 50.0f), R, g);
        check(o.rewrite && o.ended == 1 && s.press == kUtPressNone, "its RIGHT UP: rewritten");
        o = utPadInput(&s, 91, 0, ev(kUtEvtRightDown, 1570.0f, 60.0f), R, g);
        check(o.act == kUtPadActEditor && o.editorType == kUtEvtRightDown,
              "Ctrl+RIGHT DOWN on a button: the editor's entry too (not the button)");
        g.rightUp = 1;
        o = utPadInput(&s, 92, 0, ev(kUtEvtMove, 1570.0f, 60.0f), R, g);
        o = utPadInput(&s, 93, 0, ev(kUtEvtMove, 1570.0f, 60.0f), R, g);
        o = utPadInput(&s, 94, 0, ev(kUtEvtMove, 1570.0f, 60.0f), R, g);
        check(o.ended == 2 && s.press == kUtPressNone,
              "a Ctrl+right press whose RIGHT UP is lost: the polled RIGHT button ends it");
        o = utPadInput(&s, 95, 0, ev(kUtEvtRightDown, 1600.0f, 110.0f), R, g);
        check(o.rewrite && o.act == kUtPadActEditor && o.editorType == kUtEvtRightDown,
              "Ctrl+RIGHT DOWN on the label: handed to the editor");
        o = utPadInput(&s, 95, 1, ev(kUtEvtRightUp, 1600.0f, 110.0f), R, g);
        o = utPadInput(&s, 96, 0, ev(kUtEvtRightDown, 1000.0f, 500.0f), R, g);
        check(untouched(o), "Ctrl+RIGHT DOWN outside: untouched");

        // the editor is open
        utPadInReset(&s);
        g = gates();
        g.editorOpen = 1;
        o = utPadInput(&s, 100, 0, ev(kUtEvtMove, 1700.0f, 50.0f), R, g);
        check(!o.rewrite && !o.clear && o.act == kUtPadActEditor && o.editorType == kUtEvtMove,
              "editor open, a MOVE inside: handed to the editor, bytes untouched");
        o = utPadInput(&s, 100, 1, ev(kUtEvtMove, 900.0f, 500.0f), R, g);
        check(untouched(o), "editor open, a MOVE outside with no press: untouched");
        o = utPadInput(&s, 100, 2, ev(kUtEvtLeftDown, 1570.0f, 60.0f), R, g);
        check(o.rewrite && o.act == kUtPadActEditor && o.editorType == kUtEvtLeftDown &&
                  s.press == kUtPressEditor,
              "editor open, LEFT DOWN on a button: the editor's (not the button's), a drag");
        o = utPadInput(&s, 101, 0, ev(kUtEvtMove, 600.0f, 700.0f), R, g);
        check(o.clear && o.act == kUtPadActEditor && o.editorType == kUtEvtMove,
              "the drag's MOVE far outside: handed to the editor, held bytes cleared");
        o = utPadInput(&s, 101, 1, ev(kUtEvtWheelDown, 600.0f, 700.0f), R, g);
        check(o.clear && o.act == kUtPadActNone, "a wheel during the drag: cleared, not handed over");
        o = utPadInput(&s, 101, 2, ev(kUtEvtLeftUp, 600.0f, 700.0f), R, g);
        check(o.rewrite && o.act == kUtPadActEditor && o.editorType == kUtEvtLeftUp &&
                  o.ended == 1 && s.press == kUtPressNone,
              "the drag's LEFT UP outside: handed to the editor and rewritten");
        o = utPadInput(&s, 102, 0, ev(kUtEvtLeftUp, 1700.0f, 50.0f), R, g);
        check(!o.rewrite && !o.clear && o.act == kUtPadActEditor && o.editorType == kUtEvtLeftUp,
              "a stray LEFT UP inside, no press: handed over, never consumed");
        g.ctrl = 1;
        o = utPadInput(&s, 103, 0, ev(kUtEvtRightDown, 1700.0f, 50.0f), R, g);
        check(o.rewrite && o.act == kUtPadActNone,
              "Ctrl+RIGHT DOWN with the editor already open: consumed, nothing to enter");
        o = utPadInput(&s, 103, 1, ev(kUtEvtRightUp, 1700.0f, 50.0f), R, g);
        check(o.rewrite && o.ended == 1, "and its RIGHT UP");
        g.proven = 0;
        o = utPadInput(&s, 104, 0, ev(kUtEvtLeftDown, 1700.0f, 50.0f), R, g);
        check(untouched(o), "editor open but the space not proven: untouched");
    }

    // ---------------------------------------------------------------------------------------
    printf("11. the button kinds, the adopted drag, a right press and a new DOWN\n");
    {
        // ut_padpage.h's numbering: 0 MAT, 1..G the buttons, G + 1 OWN, G + 2 NEXT while paged.
        check(utPadButtonKind(0, 23, 0) == kUtPadKindMat && utPadButtonKind(0, 0, 0) == kUtPadKindMat,
              "logical 0 is MAT (the game's own materials page) - with any table");
        check(utPadButtonKind(1, 23, 0) == kUtPadKindGroup &&
                  utPadButtonKind(23, 23, 0) == kUtPadKindGroup,
              "1..G are the buttons of groups");
        check(utPadButtonKind(24, 23, 0) == kUtPadKindOwn && utPadButtonKind(24, 23, 1) == kUtPadKindOwn,
              "G + 1 is OWN, paged or not");
        check(utPadButtonKind(25, 23, 1) == kUtPadKindNext && utPadButtonKind(25, 23, 0) == kUtPadKindNone,
              "G + 2 is NEXT only while the pad is paged");
        check(utPadButtonKind(-1, 23, 1) == kUtPadKindNone && utPadButtonKind(26, 23, 1) == kUtPadKindNone &&
                  utPadButtonKind(1, -4, 0) == kUtPadKindOwn,
              "out of range: none; a negative table counts 0 buttons");
        // a recorded screen pad (23 buttons): every logical index it publishes
        int kinds[5] = {0, 0, 0, 0, 0};
        for (int i = -2; i < 30; ++i) ++kinds[utPadButtonKind(i, 23, 0)];
        check(kinds[kUtPadKindMat] == 1 && kinds[kUtPadKindGroup] == 23 && kinds[kUtPadKindOwn] == 1 &&
                  kinds[kUtPadKindNext] == 0,
              "a 23-button pad, not paged: one MAT, 23 buttons, one OWN, no NEXT");
        for (int i = 0; i < 5; ++i) kinds[i] = 0;
        for (int i = -2; i < 40; ++i) ++kinds[utPadButtonKind(i, 28, 1)];
        check(kinds[kUtPadKindMat] == 1 && kinds[kUtPadKindGroup] == 28 && kinds[kUtPadKindOwn] == 1 &&
                  kinds[kUtPadKindNext] == 1,
              "a 28-button pad, paged: one MAT, 28 buttons, one OWN, one NEXT");
        // MAT on the screen pad acts like a button (the action is ut_plate's padClickTarget: -1)
        UtPadInState s;
        utPadInReset(&s);
        UtPadInGates g = gates();
        UtPadInOut o = utPadInput(&s, 300, 0, ev(kUtEvtLeftDown, 1510.0f, 50.0f), R, g);
        check(o.act == kUtPadActButton && o.button == 0 && utPadButtonKind(o.button, 23, 0) == kUtPadKindMat,
              "a LEFT DOWN on the screen pad's MAT: the button action with logical 0 (MAT)");
        utPadInput(&s, 300, 1, ev(kUtEvtLeftUp, 1510.0f, 50.0f), R, g);

        // THE ADOPTED DRAG: the editor drags (begun in the window), nothing open here
        utPadInReset(&s);
        g = gates();
        g.editorOpen = 1;
        g.editorDrag = 1;
        o = utPadInput(&s, 310, 0, ev(kUtEvtMove, 900.0f, 300.0f), R, g);
        check(o.clear && !o.rewrite && o.act == kUtPadActEditor && o.editorType == kUtEvtMove &&
                  s.press == kUtPressEditor && s.adopted,
              "editor mid-drag, a MOVE over the world: adopted - bytes cleared, handed to the editor");
        o = utPadInput(&s, 310, 1, ev(kUtEvtWheelUp, 900.0f, 300.0f), R, g);
        check(o.clear && o.act == kUtPadActNone, "a wheel during it: cleared, not handed over");
        o = utPadInput(&s, 311, 0, ev(kUtEvtLeftUp, 905.0f, 290.0f), R, g);
        check(o.clear && !o.rewrite && o.ended == 1 && o.act == kUtPadActEditor &&
                  o.editorType == kUtEvtLeftUp && s.press == kUtPressNone && !s.adopted,
              "its LEFT UP over the world: handed to the editor, CLEARED not rewritten (the caravan "
              "saw the DOWN)");
        g.editorDrag = 0;
        o = utPadInput(&s, 311, 1, ev(kUtEvtMove, 905.0f, 290.0f), R, g);
        check(untouched(o), "the drag over: the next MOVE over the world is untouched");
        g.editorDrag = 1;
        g.proven = 0;
        o = utPadInput(&s, 312, 0, ev(kUtEvtMove, 905.0f, 290.0f), R, g);
        check(untouched(o) && s.press == kUtPressNone, "not proven: nothing adopted");
        g.proven = 1;
        o = utPadInput(&s, 313, 0, ev(kUtEvtLeftDown, 905.0f, 290.0f), R, g);
        check(untouched(o) && s.press == kUtPressNone,
              "a LEFT DOWN is never adopted: outside the rectangles it is the world's");
        o = utPadInput(&s, 314, 0, ev(kUtEvtMove, 905.0f, 290.0f), R, g);
        g.leftUp = 1;   // released, the UP lost
        o = utPadInput(&s, 314, 1, ev(kUtEvtMove, 905.0f, 290.0f), R, g);
        o = utPadInput(&s, 315, 0, ev(kUtEvtMove, 905.0f, 290.0f), R, g);
        o = utPadInput(&s, 316, 0, ev(kUtEvtMove, 905.0f, 290.0f), R, g);
        check(o.ended == 2 && s.press == kUtPressNone, "an adopted drag whose UP is lost: the poll ends it");

        // a RIGHT press (Ctrl+right-click that opened the editor) whose RIGHT UP is lost, then at
        // once a LEFT DOWN on the pad: the right press ends there and the LEFT DOWN is the editor's
        utPadInReset(&s);
        g = gates();
        g.ctrl = 1;
        o = utPadInput(&s, 320, 0, ev(kUtEvtRightDown, 1700.0f, 50.0f), R, g);
        check(o.act == kUtPadActEditor && s.press == kUtPressRight, "Ctrl+RIGHT DOWN: the editor opens");
        g.ctrl = 0;
        g.editorOpen = 1;
        g.rightUp = 1;
        o = utPadInput(&s, 321, 0, ev(kUtEvtLeftDown, 1700.0f, 50.0f), R, g);
        check(o.ended == 3 && o.rewrite && o.act == kUtPadActEditor && o.editorType == kUtEvtLeftDown &&
                  s.press == kUtPressEditor,
              "the next LEFT DOWN with the RIGHT UP lost: the right press ends, the DOWN is the "
              "editor's (was: swallowed as a held event)");
        // a press the poll ends on a DOWN: that DOWN is judged on its own
        utPadInReset(&s);
        g = gates();
        g.ctrl = 1;
        o = utPadInput(&s, 330, 0, ev(kUtEvtRightDown, 1700.0f, 50.0f), R, g);
        g.ctrl = 0;
        g.rightUp = 1;
        o = utPadInput(&s, 330, 1, ev(kUtEvtMove, 1700.0f, 50.0f), R, g);
        o = utPadInput(&s, 331, 0, ev(kUtEvtMove, 1700.0f, 50.0f), R, g);
        o = utPadInput(&s, 332, 0, ev(kUtEvtRightDown, 1570.0f, 60.0f), R, g);
        check(o.ended == 2 && o.rewrite && s.press == kUtPressRight && o.why == kUtPadWhyGround,
              "a RIGHT DOWN on which the poll ends the old press: judged on its own (consumed on "
              "the pad, a press of its own)");
    }

    // ---------------------------------------------------------------------------------------
    printf("12. recorded worlds replayed: the machine AND the editor\n");
    {
        // World 1 (campaign, 02:05) and world 5 (campaign, 02:22) of the session: the 438x627
        // cover at UI scale 0.878, the caravan window at 317,275.2 (384 x 550 px) on a 1600 x 900
        // client, the label PINNED at world start (label_screen=tl,330,168,333,23 -> client
        // 290,147), the pad in the window, the editor opened from the in-window pad. The rig wires
        // the two pure halves as ut_plate (scrBody / scrEditor) and ut_panel (editMouse,
        // editTickDraw, editPublishScreen, the screen site) wire them.
        struct Rig {
            UtPadInState s;
            UtEdit e;
            UtPadScreenRects pub;
            unsigned long long frame, gen;
            int idx;
            float mx, my;          // the mouse, client px
            int physL;
            bool adopt;            // the adoption gate wired (false = no adoption)
            int worldHeld;         // events the world view saw with a held byte, outside the window
            int worldUps;          // LEFT UPs the world saw
            int lastAct;           // the editor's answer to the last LEFT DOWN it got
            int downsToEditor;
        };
        const float S = 0.878f, OX = 317.0f, OY = 275.2f, WW = 384.0f, WH = 550.0f;
        const auto inWindow = [&](float x, float y) {
            return x >= OX && x < OX + WW && y >= OY && y < OY + WH;
        };
        const auto open = [&](Rig* r, const UtRect& grid, const UtRect& pad) {
            memset(r, 0, sizeof(*r));
            utPadInReset(&r->s);
            UtEditOpenIn in = {};
            in.inForce[kUtLayGrid] = grid;
            in.inForce[kUtLayPad] = pad;
            in.inForce[kUtLayLabel] = UtRect{-31, -146, 333, 23};
            in.def[kUtLayGrid] = UtRect{105, 76, 333, 552};
            in.def[kUtLayPad] = UtRect{102, 25, 320, 44};
            in.def[kUtLayLabel] = UtRect{105, 8, 317, 16};
            in.padFallback = in.def[kUtLayPad];
            in.plate = UtRect{0, 0, 438, 627};
            in.maxCellW = 64;
            in.maxCellH = 128;
            in.rowGap = 2;
            in.cellW = 32;
            in.cellH = 32;
            in.scale = S;
            in.coverW = 438;
            in.coverH = 627;
            in.group = 12;
            in.now = 1000;
            in.screenIn[kUtLayLabel] = 1;
            in.screenKnown = 1;
            in.screenW = 1600;
            in.screenH = 900;
            in.originX = OX;
            in.originY = OY;
            return utEditOpen(&r->e, in);
        };
        // One frame: the editor's tick (the polled button and cursor) and its publication.
        const auto frame = [&](Rig* r) {
            ++r->frame;
            UtEdTick t = {};
            t.now = 1000 + (unsigned)r->frame * 16;
            t.group = 12;
            t.scale = S;
            t.focused = 1;
            t.coverW = 438;
            t.coverH = 627;
            t.layoutOk = 1;
            t.physL = r->physL;
            t.haveCursor = 1;
            t.cx = r->mx - OX;
            t.cy = r->my - OY;
            t.screenKnown = 1;
            t.screenW = 1600;
            t.screenH = 900;
            t.originX = OX;
            t.originY = OY;
            int moved = 0;
            utEditTick(&r->e, t, &moved);
            memset(&r->pub, 0, sizeof(r->pub));
            UtRectF pad, label;
            utEditScreenGrounds(r->e, &pad, &label);
            if (pad.w > 0.0f || label.w > 0.0f) {
                r->pub.shown = 1;
                r->pub.tick = r->frame;
                if (pad.w > 0.0f) r->pub.pad = UtPadScreenRect{pad.x + OX, pad.y + OY, pad.w, pad.h};
                if (label.w > 0.0f)
                    r->pub.label = UtPadScreenRect{label.x + OX, label.y + OY, label.w, label.h};
            }
        };
        // One event of the pump: the machine first, then whoever the event still reaches.
        const auto event = [&](Rig* r, int type) {
            UtPadInGates g;
            memset(&g, 0, sizeof(g));
            g.fresh = utPadScreenRectsFresh(r->pub, r->frame) ? 1 : 0;
            g.proven = 1;
            g.editorOpen = r->e.state != kUtEdClosed ? 1 : 0;
            g.leftUp = r->physL ? 0 : 1;
            g.rightUp = 1;
            g.editorDrag = r->adopt && r->e.state == kUtEdDrag ? 1 : 0;
            const UtPadInEvent v = ev(type, r->mx, r->my);
            const UtPadInOut o = utPadInput(&r->s, r->gen, r->idx++, v, r->pub, g);
            const float lx = r->mx - OX, ly = r->my - OY;
            if (o.act == kUtPadActEditor) {
                if (o.editorType == kUtEvtLeftDown) {
                    r->lastAct = utEditLeftDown(&r->e, lx, ly, 1, r->physL, 0, 0);
                    ++r->downsToEditor;
                } else if (o.editorType == kUtEvtMove) {
                    utEditDragTo(&r->e, lx, ly);
                } else if (o.editorType == kUtEvtLeftUp) {
                    utEditLeftUp(&r->e, lx, ly);
                }
            }
            if (o.rewrite) return;   // a MOVE with no held byte: nobody sees a click
            const int held = o.clear ? 0 : r->physL;
            if (inWindow(r->mx, r->my)) {
                // the caravan's own slot -> the page's editMouse (LEFT DOWN / LEFT UP only)
                if (type == kUtEvtLeftDown) {
                    r->lastAct = utEditLeftDown(&r->e, lx, ly, 1, r->physL, 0, 0);
                    ++r->downsToEditor;
                } else if (type == kUtEvtLeftUp) {
                    utEditLeftUp(&r->e, lx, ly);
                }
            } else {
                if (held) ++r->worldHeld;
                if (type == kUtEvtLeftUp) ++r->worldUps;
            }
        };
        const auto update = [&](Rig* r) {
            ++r->gen;
            r->idx = 0;
        };
        // A drag of the pad's body from (x0, y0) to (x1, y1) in `steps` frames of one MOVE each,
        // a still second, then the release: `upLost` = no UP event (the recorded log shows one).
        const auto drag = [&](Rig* r, float x0, float y0, float x1, float y1, int steps, bool upLost) {
            r->mx = x0;
            r->my = y0;
            r->physL = 1;
            update(r);
            event(r, kUtEvtLeftDown);
            frame(r);
            for (int i = 1; i <= steps; ++i) {
                r->mx = x0 + (x1 - x0) * (float)i / (float)steps;
                r->my = y0 + (y1 - y0) * (float)i / (float)steps;
                update(r);
                event(r, kUtEvtMove);
                frame(r);
            }
            for (int i = 0; i < 60; ++i) {   // "after I stop moving them for a sec"
                update(r);
                event(r, kUtEvtMove);
                frame(r);
            }
            r->physL = 0;
            update(r);
            event(r, upLost ? kUtEvtMove : kUtEvtLeftUp);
            frame(r);
            for (int i = 0; i < 3; ++i) {
                update(r);
                event(r, kUtEvtMove);
                frame(r);
            }
        };
        const auto click = [&](Rig* r, float x, float y) {
            r->mx = x;
            r->my = y;
            r->physL = 1;
            update(r);
            event(r, kUtEvtLeftDown);
            r->physL = 0;
            event(r, kUtEvtLeftUp);
            frame(r);
            return r->lastAct;
        };
        const auto toolbar = [&](const Rig& r, int b, float* x, float* y) {
            *x = OX + ((float)r.e.tool[b].x + 3.0f) * S;
            *y = OY + ((float)r.e.tool[b].y + 3.0f) * S;
        };

        for (int pass = 0; pass < 2; ++pass) {
            const bool adopt = pass == 1;
            char what[200];
            // WORLD 1: the tall pad 42,8,371,319 in the window, dragged by its body up and out over
            // the world, held still, released with the UP lost; then the screen toolbar.
            Rig r1;
            check(open(&r1, UtRect{101, 432, 320, 196}, UtRect{42, 8, 371, 319}) && r1.e.scr[kUtLayLabel] == 1,
                  adopt ? "world 1 (adopted): the editor opens, the pinned label in screen mode"
                        : "world 1 (no adoption): the editor opens, the pinned label in screen mode");
            r1.adopt = adopt;
            frame(&r1);
            check(r1.pub.shown && utPadScreenIn(r1.pub.label, 300.0f, 155.0f) && !utPadScreenRectValid(r1.pub.pad),
                  "  the editor publishes the pinned label's ground, the pad (in the window) none");
            drag(&r1, OX + 150.0f * S, OY + 150.0f * S, 700.0f, 120.0f, 12, true);
            _snprintf_s(what, sizeof(what), _TRUNCATE,
                        "  the pad dragged out by its body: on the screen at %d,%d, the drag ended by "
                        "the poll", r1.e.rect[kUtLayPad].x, r1.e.rect[kUtLayPad].y);
            check(r1.e.state == kUtEdOpen && r1.e.scr[kUtLayPad] == 1 &&
                      r1.s.press == kUtPressNone && utPadScreenRectValid(r1.pub.pad),
                  what);
            if (adopt) {
                check(r1.worldHeld == 0,
                      "  ADOPTED: not one event of the drag reached the world with a held byte (the "
                      "adopted drag: cleared everywhere)");
            } else {
                _snprintf_s(what, sizeof(what), _TRUNCATE,
                            "  no adoption: %d event(s) of the editor's drag reached the world view "
                            "with the left button held - a \"hold to move\" under the drag",
                            r1.worldHeld);
                check(r1.worldHeld > 0, what);
            }
            float sx = 0, sy = 0, cx = 0, cy = 0;
            toolbar(r1, kUtEdSave, &sx, &sy);
            toolbar(r1, kUtEdCancel, &cx, &cy);
            check(!inWindow(sx, sy) && utPadScreenIn(r1.pub.pad, sx, sy),
                  "  the toolbar travelled with the pad onto the screen, inside its published ground");
            // the pad again, by its body on the screen, back and forth (it stays movable)
            const UtRect before = r1.e.rect[kUtLayPad];
            const float bx = OX + ((float)before.x + 20.0f) * S, by = OY + ((float)before.y + 40.0f) * S;
            drag(&r1, bx, by, bx - 60.0f, by + 10.0f, 6, false);
            check(r1.e.state == kUtEdOpen && !utRectEq(r1.e.rect[kUtLayPad], before) &&
                      r1.e.scr[kUtLayPad] == 1,
                  "  the pad on the screen dragged again: it moves (the UP delivered this time)");
            toolbar(r1, kUtEdSave, &sx, &sy);
            toolbar(r1, kUtEdCancel, &cx, &cy);
            const int downs = r1.downsToEditor;
            const int a = click(&r1, sx, sy);
            check(r1.downsToEditor == downs + 1 && (a == kUtEdActSave || a == kUtEdActSaveRefused),
                  "  SAVE on the screen toolbar reaches the editor");
            check(click(&r1, cx, cy) == kUtEdActCancel && r1.e.state == kUtEdClosed,
                  "  CANCEL on the screen toolbar closes the editor");
            check(r1.worldUps <= 1 && (adopt ? r1.worldHeld == 0 : true),
                  adopt ? "  ADOPTED: the world saw at most the window drag's own (cleared) UP"
                        : "  no adoption: the toolbar answered too (the poll had ended the drag)");

            // WORLD 5: the pad 42,8,371,79 dragged out, then on the screen,
            // released over its ground each time, SAVE - the sequence that worked live.
            Rig r5;
            open(&r5, UtRect{101, 137, 320, 490}, UtRect{42, 8, 371, 79});
            r5.adopt = adopt;
            frame(&r5);
            drag(&r5, OX + 200.0f * S, OY + 40.0f * S, 168.0f, 150.0f, 8, false);
            const bool out = r5.e.scr[kUtLayPad] == 1 && r5.e.state == kUtEdOpen;
            const UtRect p1 = r5.e.rect[kUtLayPad];
            const float qx = OX + ((float)p1.x + 30.0f) * S, qy = OY + ((float)p1.y + 30.0f) * S;
            drag(&r5, qx, qy, qx + 300.0f, qy - 60.0f, 8, false);
            const bool again = !utRectEq(r5.e.rect[kUtLayPad], p1) && r5.e.state == kUtEdOpen;
            float s5x = 0, s5y = 0;
            toolbar(r5, kUtEdSave, &s5x, &s5y);
            const int a5 = click(&r5, s5x, s5y);
            check(out && again && (a5 == kUtEdActSave || a5 == kUtEdActSaveRefused),
                  adopt ? "world 5 (adopted): out, moved on the screen, SAVE answers - as live"
                        : "world 5 (no adoption): out, moved on the screen, SAVE answers - as live");
            if (adopt)
                check(r5.worldHeld == 0 && r5.worldUps <= 1,
                      "  ADOPTED: world 5 - no held byte reached the world, at most one cleared UP");
        }

        // The Ctrl+right-click that opens the editor FROM THE SCREEN (recorded in Reign of Terror),
        // its RIGHT UP lost, then at once SAVE / a drag on the screen pad: adoption hands the LEFT
        // DOWN to the editor; without adoption the right press, waiting for its poll, swallows it.
        {
            Rig r;
            open(&r, UtRect{101, 137, 320, 490}, UtRect{-242, -277, 735, 79});
            r.e.scr[kUtLayPad] = 1;
            utEditPlaceToolbar(&r.e);
            r.adopt = true;
            frame(&r);
            float sx = 0, sy = 0;
            toolbar(r, kUtEdCancel, &sx, &sy);
            UtPadInGates g;
            memset(&g, 0, sizeof(g));
            g.fresh = 1;
            g.proven = 1;
            g.ctrl = 1;
            r.mx = sx;
            r.my = sy + 20.0f;
            update(&r);
            utPadInput(&r.s, r.gen, r.idx++, ev(kUtEvtRightDown, r.mx, r.my), r.pub, g);   // opens it
            check(r.s.press == kUtPressRight, "the screen Ctrl+right press is open (its UP never comes)");
            r.mx = sx;
            r.my = sy;
            const int downs = r.downsToEditor;
            r.physL = 1;
            event(&r, kUtEvtLeftDown);   // the same generation: the poll cannot have run
            check(r.downsToEditor == downs + 1 && r.lastAct == kUtEdActCancel && r.e.state == kUtEdClosed,
                  "  at once a click on CANCEL on the screen toolbar: the editor's, it closes");
        }
    }

    // ---------------------------------------------------------------------------------------
    printf("13. the window is the window's while the editor is open\n");
    {
        // A screen pad ground that reaches INTO the window (the ROT pad 267,3,735,79 across the
        // right edge) and the window's client rect, both published by the open editor.
        UtPadScreenRects r = makeRects(100);
        r.buttonCount = 0;
        r.pad = UtPadScreenRect{300.0f, 250.0f, 700.0f, 80.0f};
        r.label = UtPadScreenRect{0.0f, 0.0f, 0.0f, 0.0f};
        r.window = UtPadScreenRect{77.0f, 247.0f, 855.0f, 578.0f};
        UtPadInGates g = gates();
        g.editorOpen = 1;
        g.windowFresh = 1;
        UtPadInState s;
        utPadInReset(&s);
        // inside the window AND the pad's ground: grounds-only made it the editor's (rewritten)
        UtPadInOut o = utPadInput(&s, 1, 0, ev(kUtEvtLeftDown, 400.0f, 300.0f), r, g);
        check(!o.rewrite && !o.clear && o.act == kUtPadActEditorNote && o.editorType == kUtEvtLeftDown &&
                  o.why == kUtPadWhyWindow && s.press == kUtPressNone,
              "a LEFT DOWN inside the window (and the screen pad's ground): untouched, only noted");
        g.windowFresh = 0;
        utPadInReset(&s);
        o = utPadInput(&s, 2, 0, ev(kUtEvtLeftDown, 400.0f, 300.0f), r, g);
        check(o.rewrite && o.act == kUtPadActEditor && s.press == kUtPressEditor,
              "  the same DOWN with no window published (grounds only): rewritten, an editor press");
        g.windowFresh = 1;
        utPadInReset(&s);
        o = utPadInput(&s, 3, 0, ev(kUtEvtLeftDown, 950.0f, 300.0f), r, g);
        check(o.rewrite && o.act == kUtPadActEditor && s.press == kUtPressEditor,
              "a LEFT DOWN on the ground OUTSIDE the window: the editor's through the screen input");
        // that press runs to its UP wherever it goes: its UP inside the window is rewritten
        o = utPadInput(&s, 3, 1, ev(kUtEvtMove, 500.0f, 300.0f), r, g);
        check(o.clear && o.act == kUtPadActEditor && o.editorType == kUtEvtMove,
              "  its MOVE inside the window: cleared and handed over (a press of the screen input)");
        o = utPadInput(&s, 3, 2, ev(kUtEvtLeftUp, 500.0f, 300.0f), r, g);
        check(o.rewrite && o.ended == 1 && o.act == kUtPadActEditor && o.editorType == kUtEvtLeftUp,
              "  its UP inside the window: rewritten and handed over (the caravan never saw the DOWN)");
        utPadInReset(&s);
        int touched = 0, acted = 0;
        const int types[] = {kUtEvtMove, kUtEvtLeftUp, kUtEvtRightDown, kUtEvtRightUp, kUtEvtWheelUp};
        for (int t : types) {
            o = utPadInput(&s, 4, t, ev(t, 400.0f, 300.0f), r, g);
            if (o.rewrite || o.clear) ++touched;
            if (o.act != kUtPadActNone) ++acted;
        }
        check(touched == 0 && acted == 0 && s.press == kUtPressNone,
              "  every other event type inside the window: untouched, nothing handed over");
        g.proven = 0;
        o = utPadInput(&s, 5, 0, ev(kUtEvtLeftDown, 400.0f, 300.0f), r, g);
        check(!o.rewrite && o.act == kUtPadActNone, "  the space unproven: no note either");
        g.proven = 1;
        g.editorOpen = 0;
        o = utPadInput(&s, 6, 0, ev(kUtEvtLeftDown, 400.0f, 300.0f), r, g);
        check(o.act != kUtPadActEditorNote && o.why != kUtPadWhyWindow,
              "  the editor closed: the window rect means nothing (the screen pad's own rules)");
        utPadInReset(&s);
        g.editorOpen = 1;
        // freshness of the window alone (every part in the window: no pad, no label)
        UtPadScreenRects w;
        memset(&w, 0, sizeof(w));
        w.shown = 1;
        w.tick = 100;
        w.window = r.window;
        check(utPadScreenWindowFresh(w, 100) && utPadScreenWindowFresh(w, 104) &&
                  !utPadScreenWindowFresh(w, 105) && !utPadScreenWindowFresh(w, 99) &&
                  !utPadScreenRectsFresh(w, 100),
              "the window alone is fresh for 4 frames; the grounds' freshness needs a pad or label");
        w.shown = 0;
        check(!utPadScreenWindowFresh(w, 100), "  retracted (shown = 0): no window");
        w.shown = 1;
        g.fresh = 0;   // nothing on the screen
        o = utPadInput(&s, 7, 0, ev(kUtEvtLeftDown, 400.0f, 300.0f), w, g);
        check(!o.rewrite && o.act == kUtPadActEditorNote,
              "  every part in the window: an in-window DOWN is still noted (the window route's proof)");
        o = utPadInput(&s, 7, 1, ev(kUtEvtLeftDown, 10.0f, 10.0f), w, g);
        check(untouched(o) && o.act == kUtPadActNone, "  a DOWN over the world: the world's, untouched");

        // THE ADOPTED DRAG with the window published: inside untouched, outside the adopted-drag rules.
        utPadInReset(&s);
        g = gates();
        g.editorOpen = 1;
        g.windowFresh = 1;
        g.editorDrag = 1;
        o = utPadInput(&s, 10, 0, ev(kUtEvtMove, 400.0f, 600.0f), w, g);
        check(untouched(o) && s.press == kUtPressEditor && s.adopted == 1 && o.why == kUtPadWhyWindow,
              "an editor drag begun in the window: adopted, its MOVE inside the window untouched");
        o = utPadInput(&s, 10, 1, ev(kUtEvtMove, 20.0f, 100.0f), w, g);
        check(o.clear && !o.rewrite && o.act == kUtPadActEditor && o.editorType == kUtEvtMove,
              "  over the world: cleared and handed to the editor (as adopted)");
        o = utPadInput(&s, 10, 2, ev(kUtEvtMove, 500.0f, 400.0f), w, g);
        check(untouched(o) && s.press == kUtPressEditor, "  back inside: untouched again");
        o = utPadInput(&s, 10, 3, ev(kUtEvtLeftUp, 500.0f, 400.0f), w, g);
        check(untouched(o) && o.ended == 1 && s.press == kUtPressNone && o.act == kUtPadActNone,
              "  its UP inside the window: untouched (the caravan's), the press over");
        utPadInReset(&s);
        o = utPadInput(&s, 11, 0, ev(kUtEvtMove, 20.0f, 100.0f), w, g);
        o = utPadInput(&s, 11, 1, ev(kUtEvtLeftUp, 20.0f, 100.0f), w, g);
        check(o.clear && !o.rewrite && o.ended == 1 && o.act == kUtPadActEditor,
              "  an adopted drag's UP over the world: cleared, handed over (as adopted, unchanged)");
        // the polled release while the cursor rests inside the window
        utPadInReset(&s);
        o = utPadInput(&s, 20, 0, ev(kUtEvtMove, 400.0f, 600.0f), w, g);
        g.editorDrag = 0;
        g.leftUp = 1;
        o = utPadInput(&s, 21, 0, ev(kUtEvtMove, 400.0f, 600.0f), w, g);
        o = utPadInput(&s, 22, 0, ev(kUtEvtMove, 400.0f, 600.0f), w, g);
        const bool open22 = s.press == kUtPressEditor;
        o = utPadInput(&s, 23, 0, ev(kUtEvtMove, 400.0f, 600.0f), w, g);
        check(open22 && untouched(o) && o.ended == 2 && s.press == kUtPressNone,
              "  its UP lost inside the window: the polled release ends it, the events untouched");
    }

    // ---------------------------------------------------------------------------------------
    printf("14. recorded worlds replayed: the editor's two routes\n");
    {
        // The campaign world: the 438x627 cover at UI scale 0.878, the window
        // at 317,275.2 on a 1600 x 900 client (pad_screen=tl,242,63 <-> local -120,-251), the pad
        // pinned all world - so the in-window pad's own probe never ran (no "buttons: consumption
        // ENABLED" line in the whole session; "screen pad input: consumption ENABLED").
        const RigGeo camp = {0.878f, 317.0f, 275.2f, 438, 627, 1600, 900};
        // Reign of Terror: 974x659, the window at 77.264,247.596
        // (pad_screen=tl,242,63 <-> local 154,-219).
        const RigGeo rot = {0.878f, 77.264f, 247.596f, 974, 659, 1600, 900};
        char what[220];
        for (int wiring = 8; wiring <= 9; ++wiring) {
            const bool fixed = wiring == 9;
            const char* tag = fixed ? "window wiring" : "grounds-only wiring";
            Rig9 r;
            check(rigOpen(&r, camp, wiring, 0, UtRect{101, 137, 320, 490}, UtRect{-120, -251, 735, 79}, 1,
                          UtRect{100, 85, 333, 48}, 0) &&
                      r.e.scr[kUtLayPad] == 1 && r.e.scr[kUtLayLabel] == 0,
                  fixed ? "campaign (window wiring): the editor opens, the pad on the screen"
                        : "campaign (grounds-only wiring): the editor opens, the pad on the screen");
            rigFrame(&r);
            // the pad -120,-251 -> -147,-201 (on the screen): its body, on the screen
            rigDragL(&r, 180.0f, -211.0f, -27.0f, 50.0f);
            _snprintf_s(what, sizeof(what), _TRUNCATE,
                        "  %s: the pad dragged on the screen -> %d,%d,%d,%d", tag,
                        r.e.rect[kUtLayPad].x, r.e.rect[kUtLayPad].y, r.e.rect[kUtLayPad].w,
                        r.e.rect[kUtLayPad].h);
            check(rectIs(r.e.rect[kUtLayPad], -147, -201, 735, 79) && r.e.scr[kUtLayPad] == 1, what);
            // then -> 243,-201,345,57: its bottom-left corner, on the screen
            rigDragL(&r, -147.0f, -122.0f, 390.0f, -22.0f);
            _snprintf_s(what, sizeof(what), _TRUNCATE,
                        "  its bottom-left corner on the screen -> %d,%d,%d,%d",
                        r.e.rect[kUtLayPad].x, r.e.rect[kUtLayPad].y, r.e.rect[kUtLayPad].w,
                        r.e.rect[kUtLayPad].h);
            check(rectIs(r.e.rect[kUtLayPad], 243, -201, 345, 57) && r.e.scr[kUtLayPad] == 1, what);
            // then -> 62,12,345,57: its body from the screen wholly into the window
            rigDragL(&r, 343.0f, -171.0f, -181.0f, 213.0f);
            check(rectIs(r.e.rect[kUtLayPad], 62, 12, 345, 57) && r.e.scr[kUtLayPad] == 0 &&
                      r.e.state == kUtEdOpen && r.s.press == kUtPressNone,
                  "  its body dragged from the screen into the window: 62,12,345,57, window mode");
            check(!utPadScreenRectValid(r.pub.pad) && !utPadScreenRectValid(r.pub.label) &&
                      (fixed ? utPadScreenRectValid(r.pub.window) : !r.pub.shown),
                  fixed ? "  window wiring: no ground any more, only the window's rect"
                        : "  grounds-only wiring: nothing published any more (the grounds were NOT over the "
                          "in-window pad)");
            // the recorded clicks in the window (the log: silence, then Esc)
            const int viaWin = r.downsViaWindow;
            rigDragL(&r, 150.0f, 50.0f, 20.0f, 30.0f);
            check(r.lastDownRewritten == 0 && r.downsViaWindow == viaWin + 1,
                  "  the pad's body grabbed in the window: the DOWN passes the screen input UNTOUCHED "
                  "and reaches the window's slot");
            if (fixed) {
                check(rectIs(r.e.rect[kUtLayPad], 82, 42, 345, 57) && r.winLatch == 1 && r.notes >= 1,
                      "  WINDOW WIRING: it moves (82,42) - the window's click proven by the screen input's note");
            } else {
                _snprintf_s(what, sizeof(what), _TRUNCATE,
                            "  grounds-only wiring: it does NOT move (still %d,%d): the window route refused it "
                            "- the in-window probe never ran this world (act %d = pass)",
                            r.e.rect[kUtLayPad].x, r.e.rect[kUtLayPad].y, r.lastAct);
                check(rectIs(r.e.rect[kUtLayPad], 62, 12, 345, 57) && r.lastAct == kUtEdActPass, what);
            }
            // resize inside: its bottom-right corner
            const UtRect p0 = r.e.rect[kUtLayPad];
            rigDragL(&r, (float)(p0.x + p0.w), (float)(p0.y + p0.h), -30.0f, 20.0f);
            if (fixed)
                check(rectIs(r.e.rect[kUtLayPad], p0.x, p0.y, p0.w - 30, p0.h + 20) && r.e.scr[kUtLayPad] == 0,
                      "  WINDOW WIRING: its bottom-right corner in the window resizes it (window mode kept)");
            else
                check(utRectEq(r.e.rect[kUtLayPad], p0), "  grounds-only wiring: the resize in the window is refused too");
            // the toolbar inside the window
            float tx = rigX(r, (float)r.e.tool[kUtEdSave].x + 3.0f), ty = rigY(r, (float)r.e.tool[kUtEdSave].y + 3.0f);
            check(rigIn(r, tx, ty), "  the toolbar travelled with the pad into the window");
            const int aSave = rigTool(&r, kUtEdSave);
            if (fixed)
                check(aSave == kUtEdActSave || aSave == kUtEdActSaveRefused,
                      "  WINDOW WIRING: SAVE on the toolbar in the window answers");
            else
                check(aSave == kUtEdActPass, "  grounds-only wiring: SAVE in the window does nothing (refused)");
            if (!fixed) {
                // CANCEL needs no proof in either wiring; the log has no CANCEL line
                // in either span, so a CANCEL that takes no click is not reproduced here.
                check(rigTool(&r, kUtEdCancel) == kUtEdActCancel && r.e.state == kUtEdClosed,
                      "  grounds-only wiring: CANCEL in the window does answer (no proof needed)");
                continue;
            }
            // out again: the pad grabbed in the window and dragged out over the world (adopted)
            const UtRect p1 = r.e.rect[kUtLayPad];
            const int held0 = r.worldHeld;
            rigDrag(&r, rigX(r, (float)p1.x + 40.0f), rigY(r, (float)p1.y + 20.0f), 700.0f, 120.0f, 12, true);
            check(r.e.scr[kUtLayPad] == 1 && r.e.state == kUtEdOpen && r.worldHeld == held0 &&
                      r.s.press == kUtPressNone,
                  "  WINDOW WIRING: out again from the window (UP lost over the world): on the screen, no held "
                  "byte reached the world");
            tx = rigX(r, (float)r.e.tool[kUtEdSave].x + 3.0f);
            ty = rigY(r, (float)r.e.tool[kUtEdSave].y + 3.0f);
            check(!rigIn(r, tx, ty) && utPadScreenIn(r.pub.pad, tx, ty),
                  "  the toolbar on the screen again, inside the published ground");
            const int aSave2 = rigTool(&r, kUtEdSave);
            check((aSave2 == kUtEdActSave || aSave2 == kUtEdActSaveRefused) && r.lastDownRewritten == 1,
                  "  SAVE on the screen toolbar: the screen input's (rewritten), it answers");
            // and in once more, then RESET and CANCEL in the window
            const UtRect p2 = r.e.rect[kUtLayPad];
            const float gx = rigX(r, (float)p2.x + 30.0f), gy = rigY(r, (float)p2.y + 40.0f);
            rigDrag(&r, gx, gy, rigX(r, 70.0f), rigY(r, 52.0f), 10, false);
            check(r.e.scr[kUtLayPad] == 0, "  in again from the screen: window mode");
            check(rigTool(&r, kUtEdReset) == kUtEdActReset &&
                      utRectEq(r.e.rect[kUtLayPad], r.e.def[kUtLayPad]),
                  "  RESET on the toolbar in the window answers");
            check(rigTool(&r, kUtEdCancel) == kUtEdActCancel && r.e.state == kUtEdClosed,
                  "  CANCEL on the toolbar in the window closes the editor");
            check(r.worldUps <= 2, "  the world saw at most the lost-UP drag's cleared events, no click");
        }

        // Reign of Terror.
        for (int wiring = 8; wiring <= 9; ++wiring) {
            const bool fixed = wiring == 9;
            Rig9 r;
            rigOpen(&r, rot, wiring, 0, UtRect{46, 72, 928, 520}, UtRect{154, -219, 735, 79}, 1,
                    UtRect{105, 8, 317, 16}, 0);
            rigFrame(&r);
            // the pad 154,-219 -> 267,3 (on the screen): its body, released in the window
            rigDragL(&r, 454.0f, -179.0f, 113.0f, 222.0f);
            check(rectIs(r.e.rect[kUtLayPad], 267, 3, 735, 79) && r.e.scr[kUtLayPad] == 1,
                  fixed ? "ROT (window wiring): the pad across the window's right edge, screen mode"
                        : "ROT (grounds-only wiring): the pad across the window's right edge, screen mode");
            // then -> 419,3,583,57: its bottom-left corner, INSIDE the window
            rigDragL(&r, 267.0f, 82.0f, 152.0f, -22.0f);
            if (fixed)
                check(r.lastDownRewritten == 0 && r.downsViaWindow >= 1,
                      "  WINDOW WIRING: the corner grabbed inside the window: the DOWN untouched "
                      "(the window's slot takes it)");
            else
                check(r.lastDownRewritten == 1 && r.downsViaScreen >= 2,
                      "  grounds-only wiring: the corner inside the window: the DOWN rewritten (the screen "
                      "pad's ground covered the window there) and handed over");
            check(rectIs(r.e.rect[kUtLayPad], 419, 3, 583, 57) && r.e.scr[kUtLayPad] == 1,
                  "  the resize works either way (as live): 419,3,583,57 on the screen");
            // then -> 419,3,544,57: its right edge, outside the window, released inside
            rigDragL(&r, 1002.0f, 30.0f, -39.0f, 0.0f);
            check(rectIs(r.e.rect[kUtLayPad], 419, 3, 544, 57) && r.e.scr[kUtLayPad] == 0 &&
                      r.lastDownRewritten == 1,
                  "  its right edge grabbed on the screen: 419,3,544,57, wholly inside = window");
            // then silence until Esc
            rigDragL(&r, 600.0f, 40.0f, -100.0f, 0.0f);
            if (fixed)
                check(rectIs(r.e.rect[kUtLayPad], 319, 3, 544, 57),
                      "  WINDOW WIRING: the pad's body in the window moves it (319,3)");
            else
                check(rectIs(r.e.rect[kUtLayPad], 419, 3, 544, 57) && r.lastAct == kUtEdActPass,
                      "  grounds-only wiring: the pad's body in the window does nothing - the bug's state");
            const int a = rigTool(&r, kUtEdSave);
            check(fixed ? (a == kUtEdActSave || a == kUtEdActSaveRefused) : a == kUtEdActPass,
                  fixed ? "  WINDOW WIRING: SAVE in the window answers" : "  grounds-only wiring: SAVE in the window refused");
        }

        // Section 12's worlds, window wiring (the window published, the note): world 1 with the
        // in-window probe proven (as it was that world) and without it.
        for (int proven = 1; proven >= 0; --proven) {
            Rig9 r;
            rigOpen(&r, camp, 9, proven, UtRect{101, 432, 320, 196}, UtRect{42, 8, 371, 319}, 0,
                    UtRect{-31, -146, 333, 23}, 1);
            rigFrame(&r);
            check(r.pub.shown && utPadScreenIn(r.pub.label, 300.0f, 155.0f) &&
                      !utPadScreenRectValid(r.pub.pad) && utPadScreenRectValid(r.pub.window),
                  proven ? "world 1, window wiring: the pinned label's ground and the window published"
                         : "world 1, window wiring, the in-window probe not run: the same publication");
            rigDrag(&r, rigX(r, 150.0f), rigY(r, 150.0f), 700.0f, 120.0f, 12, true);
            check(r.e.state == kUtEdOpen && r.e.scr[kUtLayPad] == 1 && r.s.press == kUtPressNone &&
                      r.worldHeld == 0,
                  "  the pad dragged out by its body, UP lost: on the screen, no held byte to the world");
            const UtRect before = r.e.rect[kUtLayPad];
            const float bx = rigX(r, (float)before.x + 20.0f), by = rigY(r, (float)before.y + 40.0f);
            rigDrag(&r, bx, by, bx - 60.0f, by + 10.0f, 6, false);
            check(r.e.state == kUtEdOpen && !utRectEq(r.e.rect[kUtLayPad], before) && r.e.scr[kUtLayPad] == 1,
                  "  dragged again on the screen: it moves");
            const int a = rigTool(&r, kUtEdSave);
            check(a == kUtEdActSave || a == kUtEdActSaveRefused, "  SAVE on the screen toolbar answers");
            check(rigTool(&r, kUtEdCancel) == kUtEdActCancel && r.e.state == kUtEdClosed &&
                      r.worldHeld == 0 && r.worldUps <= 1,
                  "  CANCEL on the screen toolbar closes; the world saw no held byte, at most one UP");
        }
    }

    // ---------------------------------------------------------------------------------------
    printf("10. purity, and the real DirectInput.dll (optional)\n");
    {
        std::string text;
        FILE* f = nullptr;
        const std::string path = root + "\\src\\ut_padinput.h";
        if (!fopen_s(&f, path.c_str(), "rb") && f) {
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
            fclose(f);
        }
        size_t includes = 0, at = 0;
        bool onlyAllowed = !text.empty();
        while ((at = text.find("#include", at)) != std::string::npos) {
            const size_t eol = text.find('\n', at);
            const std::string line = text.substr(at, eol - at);
            if (line.find("<stddef.h>") == std::string::npos &&
                line.find("<string.h>") == std::string::npos)
                onlyAllowed = false;
            ++includes;
            at = eol;
        }
        check(onlyAllowed && includes == 2, "the header includes only <stddef.h> and <string.h>");

        const std::string dll = findDirectInput();
        if (dll.empty()) {
            printf("  DirectInput.dll: SKIPPED (no UNIQUETAB_TEST_GAME_DIR / _DBMODS_DIR game with "
                   "x64\\DirectInput.dll)\n");
        } else {
            Pe pe;
            const bool loaded = loadPe(dll, &pe);
            check(loaded, "DirectInput.dll opened read only and parsed as PE32+");
            if (loaded) {
                const unsigned gme = exportRva(
                    pe, "?GetMouseEvent@DirectInputDevice@GAME@@UEBA?AUMouseEvent@InputDevice@2@H@Z");
                const unsigned upd = exportRva(pe, "?Update@DirectInputDevice@GAME@@UEAAXXZ");
                const unsigned vt = exportRva(pe, "??_7DirectInputDevice@GAME@@6B@");
                char what[160];
                _snprintf_s(what, sizeof(what), _TRUNCATE,
                            "the exports by name: GetMouseEvent 0x%X, Update 0x%X, the vtable 0x%X",
                            gme, upd, vt);
                check(gme && upd && vt, what);
                const unsigned char* body = gme ? pe.at(gme, 0x2E) : nullptr;
                check(body && utPadDecodeEventStride(body, 0x2E) == (int)kUtMouseEventSize,
                      "GetMouseEvent's own imul names the 0x1C-byte record the detour rewrites");
                // MinHook copies at least 5 bytes: they must hold no RIP-relative operand. The
                // 1.3.0.8 body opens with movsxd rax,r8d (3) + imul r8,rax,imm8 (4).
                check(body && body[0] == 0x49 && body[1] == 0x63 && body[2] == 0xC0 &&
                          body[3] == 0x4C && body[4] == 0x6B,
                      "GetMouseEvent opens with movsxd + imul (no RIP-relative byte to relocate)");
                const unsigned char* ub = upd ? pe.at(upd, 8) : nullptr;
                check(ub && ub[0] == 0x4C && ub[1] == 0x8B && ub[2] == 0xDC && ub[3] == 0x55 &&
                          ub[4] == 0x57,
                      "Update opens with mov r11,rsp; push rbp; push rdi (5 plain bytes)");
                const unsigned char* slots = vt ? pe.at(vt, 0x40) : nullptr;
                const bool s10 = slots && u64(slots + 0x10) == pe.base + upd;
                const bool s38 = slots && u64(slots + 0x38) == pe.base + gme;
                check(s10 && s38,
                      "the device vtable: slot +0x10 is Update, slot +0x38 is GetMouseEvent (the "
                      "pump's call) - the mod's run-time cross-check, offline");
                if (gme == 0x4840 && upd == 0x31A0 && vt == 0x9850)
                    printf("  (the 1.3.0.8 rvas: 0x4840 / 0x31A0 / 0x9850)\n");
            }
        }
    }

    printf("%d checks, %d failed - %s\n", g_checks, g_fails,
           g_fails ? "PAD INPUT TESTS FAILED" : "PAD INPUT TESTS PASSED");
    return g_fails ? 1 : 0;
}
