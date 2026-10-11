// test_layout.cpp - the layout model, proved without a game: src\ut_layout.h, the real header.
//
//   tools\build_test_layout.bat        (needs a vcvars64 shell; the .bat calls it itself)
//
//   1. the text forms: "x,y,w,h" and the "<W>x<H>" cover suffix, good and bad;
//   2. the caption arithmetic: the pen rule and buttonTagSize's numbers at the five UI scales
//      ut_panel.cpp documents;
//   3. THE PAD: today's rectangle 102,25,320,44 is fitted back to the 9 x 3 cells of 34 x 14 with
//      a size-10 caption at 1.000, every button where padCellRec puts it, and so is the default
//      pad over the whole legal pad_h x pad_gap window; a side block and the refusals;
//   4. THE GRID: every G line of data\oracle\uniq-groups.txt is reproduced by utGridFit from the
//      P line's rectangle (7- and 9-field G lines both read), the frame's box count is the largest
//      group, and the modded views' frames (Baul 144, X-Mod 150, Reign of Terror 150) and a user
//      rectangle on Reign of Terror;
//   4b. the painted lattice (utLatticeFrame): every cell of the page's fit framed - two recorded
//      Reign of Terror grids 46,72,928,520 and 46,218,928,374 (29 columns, the 29th flush with
//      the plate's edge), the cut frame, the corners, vanilla's derived grid never cut;
//   5. the label: today's band and refusal, and the height bound;
//   6. utLayoutResolve: every refusal reason, the overlap order and the fall-back chain;
//   7. the pad on the game's own materials page: the one rule at seven UI scales - the pad in
//      force where it is over any boxes, vanilla clear of its 24 boxes at every pad key, the
//      padmat_ rectangle (wins; off the plate or too small refused) and the log's text;
//   7b. a user's Reign of Terror rectangles, as logged, on its materials page (two box
//      models, nine UI scales): the whole pad where it is, every button inside it, the boxes it
//      covers counted (29 of 116, as logged), padmat_ wins, and the log's text;
//   8. what the live page runs: the plate out of the P line, today's ini drawn exactly as 1.1.0
//      (every legal pad_y x pad_h x pad_gap cell for cell with its caption, the label), a user
//      grid and pad moving the page as the research's user test sets them, the refusals, the row
//      kept across a cols change, and the stamp's canonical text;
//   9. the pad keyed by the group table (ut_padpage.h);
//  10. THE SCREEN PARTS (pad_screen / label_screen): the key's text form; the corner placement at
//      1920x1080 / 1.0, 2560x1440 / 0.928 and 3440x1440 / 0.7 for all four corners; the shift
//      into the viewport; the record <-> client round trip at the three logged window origins;
//      the resolver: a screen pad in force (no kUtLayoutOutside), the grid refusal, the fit,
//      large and no-view refusals, the fall-back to the in-window rectangle, the label's own key,
//      a screen label yielding to a screen pad, the grid never a screen part, the in-window rules
//      without the pinned pad and the chain when the window moves under it; the materials page
//      keeps a screen pad whole (padmat_ not used); the source path; a pinned part's per-cover
//      line ignored, not judged (a recorded world replayed: no refusal for the pad, the
//      stale label line out of the input, the DEBUG text); the editor's nearest corner.
//  12. the layout plates' resolver (utLayoutPlatesGrid) sees the screen keys - the
//      recorded DeathGrimDawn SAVE replayed with and without pad_screen, the
//      live page agreeing both ways, and the plate set the page shows (utPlateSetChoice).
// Exit code 0 = all of it held. Argument 1: the repository root (for data\oracle\).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <chrono>
#include <string>
#include <vector>

#include "../src/ut_layout.h"
#include "../src/ut_padmat.h"
#include "../src/ut_padpage.h"
#include "../src/ut_screendraw.h"
#include "../src/gen/ut_groups.h"

using namespace ut;

static int g_fails = 0;
static void check(bool ok, const char* what) {
    printf("  %-70s %s\n", what, ok ? "OK" : "FAIL");
    if (!ok) ++g_fails;
}

// The 26 captions of the pad today (ut_panel.cpp kButtons), in button order.
static const char* const kTags[26] = {"MAT", "HEL", "SHO", "CHE", "GLO", "BEL", "PAN", "BOO", "AMU",
                                      "MED", "RNG", "OFF", "SHD", "AX1", "DAG", "MC1", "SCP", "SW1",
                                      "GUN", "AX2", "MC2", "SPR", "SW2", "GN2", "REL", "OWN"};
static const UtCaptions kCaps = {kTags, 26};

// ut_panel.cpp's padCellRec, written out: the reference the fitter must agree with.
static UtRect todayCell(int padY, int padH, int gap, int index) {
    const int row = index / 9, col = index % 9;
    UtRect r = {105 + col * (34 + gap), padY + row * (padH + gap), index == 25 ? 2 * 34 + gap : 34,
                padH};
    return r;
}

static bool readFile(const std::string& path, std::string* out) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") || !f) return false;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out->append(buf, n);
    fclose(f);
    return true;
}

static std::vector<std::string> splitTabs(const std::string& line) {
    std::vector<std::string> v;
    size_t at = 0;
    for (;;) {
        const size_t t = line.find('\t', at);
        v.push_back(line.substr(at, t == std::string::npos ? std::string::npos : t - at));
        if (t == std::string::npos) break;
        at = t + 1;
    }
    return v;
}

static UtLayoutInput vanillaInput() {
    UtLayoutInput in = {};
    in.plate = {0, 1, 438, 627};   // bmpX, bmpY, plateW, plateH: usable 552 = 627 - (76 - 1)
    in.def[kUtLayGrid] = {105, 76, 333, 552};
    in.def[kUtLayPad] = utPadDefault(25, 14, 1);
    in.def[kUtLayLabel] = utLabelDefault();
    in.maxCellW = 64;
    in.maxCellH = 128;
    in.rowGap = kUtRowGap;
    in.buttons = 26;
    in.padGap = 1;
    in.scale = 1.0f;
    in.captionMax = kUtCaptionMaxDefault;
    in.widest = utWidestCaptionPx;
    in.widestCtx = &kCaps;
    in.labelSize = 13;
    return in;
}

int main(int argc, char** argv) {
    const std::string root = argc > 1 ? argv[1] : ".";

    // ---- 1. text forms -------------------------------------------------------------------------
    printf("1. \"x,y,w,h\" and \"<W>x<H>\"\n");
    {
        UtRect r = {};
        check(utRectParse("105,76,333,552", &r) && r.x == 105 && r.y == 76 && r.w == 333 && r.h == 552,
              "105,76,333,552");
        check(utRectParse(" 5 , -6 ,7, 8 ", &r) && r.x == 5 && r.y == -6 && r.w == 7 && r.h == 8,
              "blanks around the numbers, a negative position");
        const char* bad[] = {"", "1,2,3", "1,2,3,4,5", "1,2,0,4", "1,2,3,-4", "a,2,3,4", "1;2;3;4",
                             "1,2,3,4x", "123456,1,1,1", "1,,3,4"};
        int refused = 0;
        for (const char* b : bad) refused += utRectParse(b, &r) ? 0 : 1;
        check(refused == (int)(sizeof(bad) / sizeof(bad[0])), "ten malformed values are all refused");
        char buf[32];
        check(utRectFormat({102, 25, 320, 44}, buf, sizeof(buf)) && !strcmp(buf, "102,25,320,44"),
              "the format is the parse's own");
        int w = 0, h = 0;
        check(utCoverKeyParse("438x627", &w, &h) && w == 438 && h == 627, "438x627");
        const char* badKeys[] = {"438X627", "438x", "x627", "0x627", "438x627x", "12345x1", "43 8x627",
                                 ""};
        refused = 0;
        for (const char* b : badKeys) refused += utCoverKeyParse(b, &w, &h) ? 0 : 1;
        check(refused == (int)(sizeof(badKeys) / sizeof(badKeys[0])), "eight bad cover suffixes refused");
        check(utCoverKeyFormat(974, 659, buf, sizeof(buf)) && !strcmp(buf, "974x659"), "974x659");
    }

    // ---- 2. captions ---------------------------------------------------------------------------
    // The numbers ut_panel.cpp's buttonTagSize comment gives for the pad's own cells.
    printf("\n2. the caption arithmetic\n");
    {
        check(utWidestCaptionPx(10, &kCaps) == 29, "the widest of the 26 captions at size 10 is 29 px");
        check(utWidestCaptionPx(6, &kCaps) == 16, "and 16 px at size 6");
        struct C { int w, h, size; } cases[] = {{23, 9, 6}, {31, 13, 9}, {34, 14, 10}, {42, 17, 12},
                                               {68, 28, 14}};
        int bad = 0;
        for (const C& c : cases) {
            const int got = utCaptionSize(c.w, c.h, 14, utWidestCaptionPx, &kCaps);
            if (got != c.size) {
                printf("    %dx%d: size %d, want %d\n", c.w, c.h, got, c.size);
                ++bad;
            }
        }
        check(bad == 0, "23x9 -> 6, 31x13 -> 9, 34x14 -> 10, 42x17 -> 12, 68x28 -> 14");
        check(utCaptionSize(68, 28, 24, utWidestCaptionPx, &kCaps) > 14,
              "a higher ceiling lets the 68x28 cell carry a larger caption");
        check(utCaptionSize(30, 7, 14, utWidestCaptionPx, &kCaps) == 0, "a 7 px cell carries none");
    }

    // ---- 3. the pad ----------------------------------------------------------------------------
    printf("\n3. the pad\n");
    {
        const UtRect today = {102, 25, 320, 44};
        check(utRectEq(utPadDefault(25, 14, 1), today), "utPadDefault(25,14,1) is 102,25,320,44");
        const UtPadFit f = utPadFit(today, 26, 1, 1.0f, 14, utWidestCaptionPx, &kCaps);
        check(f.ok && f.cols == 9 && f.rows == 3 && f.cellW == 34 && f.cellH == 14,
              "today's rectangle is fitted back to 9 x 3 cells of 34 x 14");
        check(f.caption == 10 && f.empty == 0, "with the size-10 caption and no empty cell");
        int moved = 0;
        for (int i = 0; i < 26; ++i)
            if (!utRectEq(utPadCellRect(f, i), todayCell(25, 14, 1, i))) ++moved;
        check(moved == 0, "every one of the 26 buttons is where padCellRec puts it");
        const UtPadFit f7 = utPadFit(today, 26, 1, 0.7f, 14, utWidestCaptionPx, &kCaps);
        check(f7.ok && f7.cols == 9 && f7.rows == 3 && f7.caption == 6,
              "at UI scale 0.700 the same 9 x 3, caption 6");

        // The whole legal window of the [advanced] keys: the default pad never changes shape.
        int off = 0, cases = 0;
        for (int ph = 12; ph <= 20; ++ph)
            for (int gap = 0; gap <= 4; ++gap) {
                const UtRect d = utPadDefault(25, ph, gap);
                const UtPadFit g = utPadFit(d, 26, gap, 1.0f, 14, utWidestCaptionPx, &kCaps);
                ++cases;
                bool same = g.ok && g.cols == 9 && g.rows == 3 && g.cellW == 34 && g.cellH == ph;
                for (int i = 0; same && i < 26; ++i)
                    same = utRectEq(utPadCellRect(g, i), todayCell(25, ph, gap, i));
                if (!same) {
                    printf("    pad_h=%d pad_gap=%d: %dx%d of %dx%d\n", ph, gap, g.cols, g.rows, g.cellW,
                           g.cellH);
                    ++off;
                }
            }
        printf("    %d pad_h x pad_gap combinations\n", cases);
        check(off == 0, "pad_h 12..20 x pad_gap 0..4: always today's 9 x 3, button for button");

        // The two neighbours the research names: 2 rows give 21 px cells, 4 rows 10 px ones.
        check((320 - 6 - 13) / 14 == 21 && (44 - 3) / 4 == 10, "(2 rows: 21 px wide, 4 rows: 10 px tall)");

        const UtPadFit side = utPadFit({8, 100, 160, 150}, 26, 1, 1.0f, 14, utWidestCaptionPx, &kCaps);
        printf("    160x150 side block: %d x %d of %d x %d, caption %d, %d empty\n", side.cols, side.rows,
               side.cellW, side.cellH, side.caption, side.empty);
        check(side.ok && side.cols == 4 && side.rows == 7 && side.cellW == 37 && side.cellH == 20 &&
                  side.caption == 12 && side.empty == 1,
              "a 160x150 side block: 4 x 7 cells of 37 x 20, caption 12, one empty");
        const UtRect own = utPadCellRect(side, 25);
        check(own.x == 8 + 3 + 1 * 38 && own.y == 100 + 6 * 21 && own.w == 2 * 37 + 1,
              "its OWN button takes the two cells after the 25th");

        const UtPadFit big = utPadFit({102, 30, 320, 80}, 26, 1, 1.0f, 14, utWidestCaptionPx, &kCaps);
        printf("    102,30,320,80: %d x %d of %d x %d, caption %d\n", big.cols, big.rows, big.cellW,
               big.cellH, big.caption);
        check(big.ok && big.cols == 7 && big.rows == 4 && big.cellW == 44 && big.cellH == 19 &&
                  big.caption == 14 && big.empty == 1,
              "102,30,320,80 re-fits to 7 x 4 of 44 x 19: caption 14, the ceiling");
        const UtRect block = {8, 100, 420, 200};
        const UtPadFit b14 = utPadFit(block, 26, 1, 1.0f, 14, utWidestCaptionPx, &kCaps);
        const UtPadFit b24 = utPadFit(block, 26, 1, 1.0f, 24, utWidestCaptionPx, &kCaps);
        printf("    8,100,420,200: ceiling 14 -> %d x %d caption %d; ceiling 24 -> %d x %d caption %d\n",
               b14.cols, b14.rows, b14.caption, b24.cols, b24.rows, b24.caption);
        check(b14.ok && b14.caption == 14 && b24.ok && b24.caption > 14,
              "pad_caption_max is the ceiling: 14 stops at 14, 24 goes past it");

        // The double-wide button never wraps: 13 columns would start it in the last column.
        UtPadFit wrap = {};
        wrap.ok = 1;
        wrap.cols = 13;
        wrap.rows = 3;
        wrap.cellW = 30;
        wrap.cellH = 14;
        wrap.gap = 1;
        wrap.buttons = 26;
        const UtRect w25 = utPadCellRect(wrap, 25);
        check(w25.x == 0 && w25.y == 2 * 15 && w25.w == 61,
              "a double-wide button that would wrap moves to the next row");

        check(!utPadFit({102, 25, 100, 44}, 26, 1, 1.0f, 14, utWidestCaptionPx, &kCaps).ok,
              "a 100x44 pad has no legal layout for 26 buttons");
        check(!utPadFit(today, 60, 1, 1.0f, 14, utWidestCaptionPx, &kCaps).ok,
              "60 buttons do not fit today's rectangle: refused, never shrunk below size 6");
        check(utPadFit(today, 20, 1, 1.0f, 14, utWidestCaptionPx, &kCaps).ok,
              "20 buttons do (the count is a parameter)");
    }

    // ---- 4. the grid ---------------------------------------------------------------------------
    printf("\n4. the grid\n");
    {
        std::string text;
        const std::string path = root + "\\data\\oracle\\uniq-groups.txt";
        if (!readFile(path, &text)) {
            printf("    %s is not here (the public tree carries no fixtures) - the oracle half is "
                   "SKIPPED\n",
                   path.c_str());
        } else {
            UtRect usable = {0, 0, 0, 0};
            int plateW = 0, plateH = 0;
            int frameBoxes = -1, rowGap = -1, groups = 0, bad = 0, largest = 0, shapes9 = 0;
            std::vector<int> cws, chs;   // the distinct cell sizes, as pages_gen collects them
            size_t at = 0;
            while (at < text.size()) {
                size_t e = text.find('\n', at);
                if (e == std::string::npos) e = text.size();
                std::string line = text.substr(at, e - at);
                at = e + 1;
                if (!line.empty() && line.back() == '\r') line.pop_back();
                const std::vector<std::string> f = splitTabs(line);
                if (f[0] == "F" && f.size() >= 7) {
                    frameBoxes = atoi(f[2].c_str());
                    rowGap = atoi(f[6].c_str());
                } else if (f[0] == "P" && f.size() >= 7) {
                    usable = {atoi(f[1].c_str()), atoi(f[2].c_str()), atoi(f[3].c_str()),
                              atoi(f[4].c_str())};
                    plateW = atoi(f[5].c_str());
                    plateH = atoi(f[6].c_str());
                } else if (f[0] == "G") {
                    // G idx name cols rows cellW cellH count [id tag]
                    if (f.size() != 8 && f.size() != 10) {
                        printf("    a G line with %d fields\n", (int)f.size() - 1);
                        ++bad;
                        continue;
                    }
                    if (f.size() == 10) ++shapes9;
                    const int cols = atoi(f[3].c_str()), rows = atoi(f[4].c_str());
                    const int cw = atoi(f[5].c_str()), ch = atoi(f[6].c_str());
                    bool have = false;
                    for (size_t i = 0; i < cws.size(); ++i)
                        if (cws[i] == cw && chs[i] == ch) have = true;
                    if (!have) {
                        cws.push_back(cw);
                        chs.push_back(ch);
                    }
                    // fitted under the frame's own count (the F line), as pages_gen fits it
                    const UtGrid g = utGridFit(usable, cw, ch, rowGap, frameBoxes);
                    if (g.cols != cols || g.rows != rows) {
                        printf("    %s: %dx%d, the oracle says %dx%d\n", f[2].c_str(), g.cols, g.rows,
                               cols, rows);
                        ++bad;
                    }
                    if (g.cols * g.rows > largest) largest = g.cols * g.rows;
                    ++groups;
                }
            }
            printf("    %d G line(s) (%d of them with the id and tag fields), usable %d,%d %dx%d, row "
                   "gap %d\n",
                   groups, shapes9, usable.x, usable.y, usable.w, usable.h, rowGap);
            check(groups == 25 && bad == 0,
                  "every G line of the oracle (24 slot groups + Blueprints) is reproduced by utGridFit under the F line's count");
            const int want = utFrameBoxes(plateW, plateH, cws.data(), chs.data(), (int)cws.size(),
                                          rowGap);
            printf("    the F line carries %d boxes; utFrameBoxes over the %dx%d cover and %d cell "
                   "size(s) = %d; the largest group shows %d\n",
                   frameBoxes, plateW, plateH, (int)cws.size(), want, largest);
            check(frameBoxes == want && frameBoxes == 13 * 18,
                  "the vanilla F line's box count is the cover's fit: 438x627 at 32 px = 13 x 18 = 234");
            check(largest == 160 && largest < frameBoxes,
                  "and the vanilla derived page is not capped by it: its largest group is still "
                  "10 x 16 = 160");
        }
        // THE FRAME'S BOX COUNT per view (utFrameBoxes): the whole cover's uncapped fit at the
        // view's cell sizes, 160..512.
        {
            const int c32[1] = {32}, all[4] = {32, 64, 64, 64}, allH[4] = {32, 64, 96, 128};
            const int big[1] = {64}, bigH[1] = {128};
            struct F { const char* name; int w, h; const int *cw, *ch; int n, want; } covers[] = {
                {"vanilla 438x627, 32 px: 13 x 18", 438, 627, c32, c32, 1, 234},
                {"vanilla 438x627, the four vanilla cell sizes: the 32 px one wins", 438, 627, all,
                 allH, 4, 234},
                {"Loot Ascension 346x625: 10 x 18", 346, 625, c32, c32, 1, 180},
                {"Reign of Terror 974x659: 30 x 19 = 570, the ceiling", 974, 659, c32, c32, 1, 512},
                {"X-Mod 975x968: 30 x 28 = 840, the ceiling", 975, 968, c32, c32, 1, 512},
                {"a cover with only 64x128 cells: 6 x 4 = 24, the floor", 438, 627, big, bigH, 1, 160},
                {"no cell size at all: the floor", 438, 627, c32, c32, 0, 160},
                {"a tiny cover: the floor", 10, 10, c32, c32, 1, 160},
            };
            for (const F& c : covers) {
                const int got = utFrameBoxes(c.w, c.h, c.cw, c.ch, c.n, kUtRowGap);
                char what[200];
                _snprintf_s(what, sizeof(what), _TRUNCATE, "frame boxes, %s = %d (got %d)", c.name,
                            c.want, got);
                check(got == c.want, what);
            }
            check(kUtFrameBoxFloor == 160 && kUtFrameBoxMax == 512, "the floor 160, the ceiling 512");
            check(utFrameBoxes(974, 659, nullptr, nullptr, 3, kUtRowGap) == kUtFrameBoxFloor,
                  "null cell arrays give the floor");
        }
        // The modded views (test_catalogue's kGeoCases) and the new caravans, 32 px cells, each
        // under its own frame's count.
        struct V { const char* name; UtRect r; int cap, cols, rows, capped; } views[] = {
            {"Loot Ascension 310x550 (frame 180)", {128, 76, 310, 550}, 180, 9, 16, 0},
            {"X-Mod 967x866 (frame 512): 30 x 25 wanted", {8, 76, 967, 866}, 512, 30, 17, 1},
            {"Reign of Terror 962x584 (frame 512)", {12, 76, 962, 584}, 512, 30, 17, 0},
            {"a user grid on Reign of Terror, 400x584", {12, 76, 400, 584}, 512, 12, 17, 0},
            {"a Reign of Terror grid 46,218,928,374", {46, 218, 928, 374}, 512, 29, 11, 0},
            {"the same grid under the old 160 frame", {46, 218, 928, 374}, 160, 29, 5, 1},
            {"vanilla's whole cover (frame 234)", {0, 1, 438, 627}, 234, 13, 18, 0},
        };
        for (const V& v : views) {
            const UtGrid g = utGridFit(v.r, 32, 32, kUtRowGap, v.cap);
            char what[200];
            _snprintf_s(what, sizeof(what), _TRUNCATE, "%s: %d x %d = %d boxes%s", v.name, v.cols,
                        v.rows, v.cols * v.rows, v.capped ? " (capped)" : "");
            check(g.cols == v.cols && g.rows == v.rows && g.capped == v.capped, what);
        }
        // the Reign of Terror belts (64x32) take the whole height too now
        const UtGrid belts = utGridFit({12, 76, 962, 584}, 64, 32, kUtRowGap, 512);
        check(belts.cols == 15 && belts.rows == 17 && belts.capped == 0,
              "Reign of Terror's belts: 15 x 17 = 255 under the 512 frame (were 15 x 10)");
        // a user grid anywhere on a cover is never capped below the ceiling: every rectangle
        // inside the vanilla cover fits under its 234, at every vanilla cell size
        {
            const int cws[4] = {32, 64, 64, 64}, chs[4] = {32, 64, 96, 128};
            const int n = utFrameBoxes(438, 627, cws, chs, 4, kUtRowGap);
            int cappedAny = 0;
            for (int w = 32; w <= 438; w += 7)
                for (int h = 34; h <= 627; h += 9)
                    for (int i = 0; i < 4; ++i)
                        if (utGridFit({0, 1, w, h}, cws[i], chs[i], kUtRowGap, n).capped) ++cappedAny;
            check(cappedAny == 0, "no user grid inside the vanilla cover is capped by its 234 frame");
        }
        const UtGrid tiny = utGridFit({0, 0, 10, 10}, 64, 128, kUtRowGap, kUtFrameBoxFloor);
        check(tiny.cols == 1 && tiny.rows == 1 && tiny.capped == 0,
              "a rectangle smaller than a cell is still 1 x 1 (pages_gen's max(1, ...))");
    }

    // ---- 4b. the painted lattice ----------------------------------------------------------------
    // The plate painter (src\gen\plate_gen.cpp, tools\make_plates.py) frames EVERY cell of the
    // grid utGridFit gives the page, through utLatticeFrame. Two recorded Reign of Terror grids
    // (974x659, P line 12,76 962x584, so the plate is drawn at 0,1): 928 px = 29 columns of
    // 32 px, the 29th flush with the plate's right edge at 974 - the old rule (the frame's 2 px
    // outset must fit) dropped it, so the relics stood 29 wide on a 28-column lattice.
    printf("\n4b. the painted lattice\n");
    {
        const int pw = 974, ph = 659;
        const UtRect P = utPlateRectFromP(12, 76, 962, 584, pw, ph);
        check(P.x == 0 && P.y == 1, "Reign of Terror's plate is drawn at 0,1 (bmpX, bmpY)");
        const int cws[7] = {64, 64, 64, 32, 64, 32, 64}, chs[7] = {64, 96, 32, 32, 128, 96, 160};
        struct G { const char* name; UtRect r; int cols32, rows32; } grids[2] = {
            {"46,72,928,520 (ROT)", {46, 72, 928, 520}, 29, 15},
            {"46,218,928,374 (ROT)", {46, 218, 928, 374}, 29, 11},
        };
        for (const G& gr : grids) {
            int framed = 0, cells = 0, outside = 0, notAround = 0, oldRule = 0;
            for (int i = 0; i < 7; ++i) {
                const UtGrid g = utGridFit(gr.r, cws[i], chs[i], kUtRowGap, kUtFrameBoxMax);
                for (int r = 0; r < g.rows; ++r) {
                    for (int c = 0; c < g.cols; ++c) {
                        ++cells;
                        UtRect f;
                        if (!utLatticeFrame(gr.r.x, gr.r.y, cws[i], chs[i], kUtRowGap, c, r, P.x,
                                            P.y, pw, ph, &f))
                            continue;
                        ++framed;
                        if (f.x < 0 || f.y < 0 || f.x + f.w > pw || f.y + f.h > ph) ++outside;
                        const int bx = gr.r.x + cws[i] * c - P.x;
                        const int by = gr.r.y + (chs[i] + kUtRowGap) * r - P.y;
                        if (f.x > bx || f.y > by || f.x + f.w < bx + cws[i] ||
                            f.y + f.h < by + chs[i])
                            ++notAround;
                        if (cws[i] == 32 && chs[i] == 32 && bx + 32 + kUtFrameOutset <= pw &&
                            by + 32 + kUtFrameOutset <= ph)
                            ++oldRule;
                    }
                }
            }
            char what[240];
            _snprintf_s(what, sizeof(what), _TRUNCATE,
                        "grid %s: every one of the %d cells at the 7 ROT cell sizes is framed "
                        "(%d), inside the plate (%d out), around its cell (%d not)",
                        gr.name, cells, framed, outside, notAround);
            check(framed == cells && outside == 0 && notAround == 0, what);
            const UtGrid g32 = utGridFit(gr.r, 32, 32, kUtRowGap, kUtFrameBoxMax);
            _snprintf_s(what, sizeof(what), _TRUNCATE,
                        "grid %s at 32 px: the page's %d x %d and the lattice's are one fit; the "
                        "old outset rule framed %d = 28 x %d", gr.name, g32.cols, g32.rows,
                        oldRule, gr.rows32);
            check(g32.cols == gr.cols32 && g32.rows == gr.rows32 && oldRule == 28 * gr.rows32,
                  what);
        }
        UtRect f = {0, 0, 0, 0};
        const bool last = utLatticeFrame(46, 72, 32, 32, kUtRowGap, 28, 0, P.x, P.y, pw, ph, &f);
        check(last && f.x == 940 && f.y == 69 && f.w == 34 && f.h == 36,
              "the 29th column (cell 942..973) keeps its frame, cut to the plate: 940,69 34 x 36");
        const bool first = utLatticeFrame(46, 72, 32, 32, kUtRowGap, 0, 0, P.x, P.y, pw, ph, &f);
        check(first && f.x == 44 && f.y == 69 && f.w == 36 && f.h == 36,
              "an inner cell's frame is the cell grown by 2: 44,69 36 x 36");
        const bool row14 = utLatticeFrame(46, 72, 32, 32, kUtRowGap, 28, 14, P.x, P.y, pw, ph, &f);
        check(row14 && f.y == 72 + 34 * 14 - 1 - 2 && f.h == 36,
              "the row pitch is the cell height + the row gap (row 15's frame at 545)");
        check(!utLatticeFrame(46, 72, 32, 32, kUtRowGap, 29, 0, P.x, P.y, pw, ph, &f),
              "a 30th column would leave the plate: no frame");
        check(utLatticeFrame(0, 1, 32, 32, kUtRowGap, 0, 0, 0, 1, pw, ph, &f) && f.x == 0 &&
                  f.y == 0 && f.w == 34 && f.h == 34,
              "a cell at the plate's top-left corner: the frame is cut on the left and the top");
        check(!utLatticeFrame(-10, 76, 32, 32, kUtRowGap, 0, 0, 0, 1, pw, ph, &f),
              "a cell that starts off the plate gets no frame");
        check(utLatticeFrame(46, 72, 32, 32, -5, 0, 1, P.x, P.y, pw, ph, &f) &&
                  f.y == 72 + 32 - 1 - 2,
              "a negative row gap is 0");
        // The vanilla derived grid (105,76 333x552 on 438x627 at 0,1) is never cut: the shipped
        // plates are the same bytes under the new rule.
        {
            const int vw[7] = {32, 32, 32, 64, 64, 64, 64}, vh[7] = {32, 96, 128, 32, 64, 96, 128};
            int cut = 0, none = 0, n = 0;
            for (int i = 0; i < 7; ++i) {
                const UtGrid g = utGridFit({105, 76, 333, 552}, vw[i], vh[i], kUtRowGap, 234);
                for (int r = 0; r < g.rows; ++r)
                    for (int c = 0; c < g.cols; ++c) {
                        ++n;
                        if (!utLatticeFrame(105, 76, vw[i], vh[i], kUtRowGap, c, r, 0, 1, 438,
                                            627, &f))
                            ++none;
                        else if (f.w != vw[i] + 4 || f.h != vh[i] + 4)
                            ++cut;
                    }
            }
            char what[160];
            _snprintf_s(what, sizeof(what), _TRUNCATE,
                        "vanilla's derived grid: all %d frames whole (%d cut, %d missing)", n, cut,
                        none);
            check(n > 0 && cut == 0 && none == 0, what);
        }
    }

    // ---- 5. the label --------------------------------------------------------------------------
    printf("\n5. the label\n");
    {
        const UtRect def = utLabelDefault();
        check(def.x == 105 && def.y == 8 && def.x + def.w == 422 && def.y + def.h == 24,
              "the default is x 105..422, the band y 8..24");
        UtLabelFit f = utLabelFit(def, 13, 1.0f);
        check(f.ok && f.size == 13 && !f.bounded && f.x == 105 && f.top == 9 && f.limitPx == 317,
              "plate_label_size 13 at 1.000: size 13 at 105,9, 317 px wide");
        // 1.1.0: fs = round(size * s), never under 9, refused when 9 + fs / s passes 24.
        int differ = 0;
        const float scales[] = {0.7f, 0.8f, 0.928f, 1.0f, 1.25f, 1.5f, 2.0f};
        for (float s : scales)
            for (int size = 8; size <= 32; ++size) {
                int fs = (int)((float)size * s + 0.5f);
                if (fs < 9) fs = 9;
                const bool today = !(9.0f + (float)fs / s > 24.0f);
                const UtLabelFit g = utLabelFit(def, size, s);
                if (today && !(g.ok && g.size == fs && !g.bounded)) ++differ;
                if (!today && g.ok && !g.bounded) ++differ;
            }
        check(differ == 0, "wherever 1.1.0 draws the label, the size is the same (7 scales x 8..32)");
        f = utLabelFit(def, 16, 1.0f);
        check(f.ok && f.bounded && f.size == 15,
              "size 16, which 1.1.0 drops, is brought down to the band: 15");
        f = utLabelFit({105, 8, 317, 9}, 13, 1.0f);
        check(!f.ok, "a band 9 px high cannot hold 9 px of text under its 1 px top: refused");
        f = utLabelFit({105, 8, 5, 16}, 13, 1.0f);
        check(!f.ok, "a band 5 px wide is refused");
        f = utLabelFit({105, 8, 317, 30}, 13, 2.0f);
        check(f.ok && f.size == 26 && f.limitPx == 634, "at 2.000 a taller band carries size 26");
    }

    // ---- 6. resolving the user's rectangles ------------------------------------------------------
    printf("\n6. utLayoutResolve\n");
    {
        UtLayoutInput in = vanillaInput();
        UtLayout L = utLayoutResolve(in);
        check(utRectEq(L.rect[kUtLayGrid], in.def[kUtLayGrid]) &&
                  utRectEq(L.rect[kUtLayPad], in.def[kUtLayPad]) &&
                  utRectEq(L.rect[kUtLayLabel], in.def[kUtLayLabel]) && !L.user[0] && !L.user[1] &&
                  !L.user[2] && L.why[0] == kUtLayoutUnset,
              "no keys: the three derived rectangles, which is what 1.1.0 draws");
        check(L.pad.ok && L.pad.cols == 9 && L.pad.rows == 3 && L.pad.caption == 10 && L.label.ok &&
                  L.label.size == 13,
              "with today's pad and label fits");
        check(utRectInside(in.def[kUtLayGrid], in.plate) && utRectInside(in.def[kUtLayPad], in.plate) &&
                  utRectInside(in.def[kUtLayLabel], in.plate),
              "and the derived rectangles are inside the plate (0,1 438x627)");

        // A recorded test of the plan, step by step.
        const UtRect grid1 = {105, 120, 333, 460};
        in.user[kUtLayGrid] = &grid1;
        L = utLayoutResolve(in);
        check(L.user[kUtLayGrid] && utRectEq(L.rect[kUtLayGrid], grid1), "grid_438x627=105,120,333,460 is in force");
        const UtGrid g = utGridFit(L.rect[kUtLayGrid], 64, 64, kUtRowGap, kUtFrameBoxFloor);
        check(g.cols == 5 && g.rows == 6, "the helms there are 5 x 6");
        const UtRect pad1 = {102, 30, 320, 80};
        in.user[kUtLayPad] = &pad1;
        L = utLayoutResolve(in);
        check(L.user[kUtLayPad] && L.pad.ok && L.pad.caption > 10,
              "pad_438x627=102,30,320,80 is in force with bigger buttons");
        const UtRect pad2 = {102, 30, 320, 120};   // reaches y 150, into the grid
        in.user[kUtLayPad] = &pad2;
        L = utLayoutResolve(in);
        check(!L.user[kUtLayPad] && L.why[kUtLayPad] == kUtLayoutOverlap &&
                  L.against[kUtLayPad] == kUtLayGrid && L.user[kUtLayGrid],
              "a pad over the user grid falls back, against the grid; the grid stays");
        printf("    the WARN would say: pad_438x627=102,30,320,120 %s (the %s)\n",
               utLayoutWhyText(L.why[kUtLayPad]), utLayoutPartName(L.against[kUtLayPad]));

        // A user grid that meets the DERIVED pad: the user rectangle yields, all derived.
        in = vanillaInput();
        const UtRect grid2 = {105, 40, 333, 500};
        in.user[kUtLayGrid] = &grid2;
        L = utLayoutResolve(in);
        check(!L.user[kUtLayGrid] && L.why[kUtLayGrid] == kUtLayoutOverlap &&
                  L.against[kUtLayGrid] == kUtLayPad,
              "a grid over the derived pad falls back itself");
        // ...but with a user pad out of its way, the same grid stands.
        const UtRect pad3 = {8, 560, 420, 60};
        in.user[kUtLayPad] = &pad3;
        L = utLayoutResolve(in);
        check(L.user[kUtLayGrid] && L.user[kUtLayPad], "with the pad moved below it, the grid stands");
        // And a label over that pad: the label (the later part) falls back; the derived label is
        // clear of both, so nothing else moves.
        const UtRect label1 = {10, 570, 300, 16};
        in.user[kUtLayLabel] = &label1;
        L = utLayoutResolve(in);
        check(!L.user[kUtLayLabel] && L.why[kUtLayLabel] == kUtLayoutOverlap &&
                  L.against[kUtLayLabel] == kUtLayPad && L.user[kUtLayGrid] && L.user[kUtLayPad],
              "a label over the user pad falls back; grid and pad stay");
        // A chain: the user pad meets the user grid -> the pad falls back; the derived pad then
        // meets the user grid -> the grid falls back too. All derived, always legal.
        in = vanillaInput();
        const UtRect pad4 = {102, 100, 320, 44};
        in.user[kUtLayGrid] = &grid2;
        in.user[kUtLayPad] = &pad4;
        L = utLayoutResolve(in);
        check(!L.user[kUtLayGrid] && !L.user[kUtLayPad] && L.why[kUtLayPad] == kUtLayoutOverlap &&
                  L.why[kUtLayGrid] == kUtLayoutOverlap,
              "a chain of overlaps ends with every part derived");

        in = vanillaInput();
        const UtRect outside = {105, 76, 400, 552};
        in.user[kUtLayGrid] = &outside;
        L = utLayoutResolve(in);
        check(L.why[kUtLayGrid] == kUtLayoutOutside && !L.user[kUtLayGrid], "a grid past the plate: outside");
        const UtRect small = {105, 400, 60, 200};   // narrower than a 64 px cell
        in.user[kUtLayGrid] = &small;
        L = utLayoutResolve(in);
        check(L.why[kUtLayGrid] == kUtLayoutGridSmall, "a grid narrower than the largest cell: too small");
        const UtRect short1 = {105, 400, 333, 129};   // 128 + 2 needed
        in.user[kUtLayGrid] = &short1;
        L = utLayoutResolve(in);
        check(L.why[kUtLayGrid] == kUtLayoutGridSmall, "one row short of the tallest cell and its gap: too small");
        const UtRect justRight = {105, 400, 333, 130};
        in.user[kUtLayGrid] = &justRight;
        L = utLayoutResolve(in);
        check(L.user[kUtLayGrid], "exactly the tallest cell and its gap: in force");
        in.user[kUtLayGrid] = nullptr;
        const UtRect padSmall = {102, 25, 120, 30};
        in.user[kUtLayPad] = &padSmall;
        L = utLayoutResolve(in);
        check(L.why[kUtLayPad] == kUtLayoutPadNoFit && L.pad.ok && L.pad.cols == 9,
              "a pad with no room for 26 buttons: refused, today's pad drawn");
        in.user[kUtLayPad] = nullptr;
        const UtRect labelSmall = {105, 8, 317, 8};
        in.user[kUtLayLabel] = &labelSmall;
        L = utLayoutResolve(in);
        check(L.why[kUtLayLabel] == kUtLayoutLabelSmall, "an 8 px label band: too small");

        // Reign of Terror: the page is wide; a user grid on its left two fifths plus the default pad.
        UtLayoutInput rot = vanillaInput();
        rot.plate = {0, 1, 974, 659};
        rot.def[kUtLayGrid] = {12, 76, 962, 584};
        rot.maxCellW = 64;
        rot.maxCellH = 128;
        const UtRect rotGrid = {12, 76, 400, 584};
        rot.user[kUtLayGrid] = &rotGrid;
        L = utLayoutResolve(rot);
        check(L.user[kUtLayGrid] && !L.user[kUtLayPad] && L.pad.ok,
              "Reign of Terror: grid_974x659=12,76,400,584 with the default pad");
        int bad = 0;
        for (int p = 0; p < kUtLayParts; ++p)
            for (int q = p + 1; q < kUtLayParts; ++q)
                if (utRectsMeet(L.rect[p], L.rect[q])) ++bad;
        check(bad == 0, "and the three in force are pairwise disjoint");
    }

    // ---- 7. the pad on the game's own materials page --------------------------------------------
    // ONE rule (utMatPadResolve): padmat_ when it is set, inside the page and fits the buttons,
    // else the pad in force - drawn WHERE IT IS, over whatever boxes lie under it (they are
    // counted, never a reason to move or shrink the pad). Vanilla's boxes start at y 74 or below,
    // so the derived pad never meets one and draws exactly as before.
    printf("\n7. the materials page\n");
    {
        const float scales[] = {0.7f, 0.8f, 0.928f, 1.0f, 1.25f, 1.5f, 2.0f};
        int disagree = 0, cases = 0;
        // the boxes a scaled pad meets, counted one by one: what `under` must say
        auto meets = [](const UtRect& r, const UtRectF* b, int n, float s) {
            int k = 0;
            for (int i = 0; i < n; ++i)
                if (utRectsOverlap(utScaledRectF(r, s), b[i])) ++k;
            return k;
        };
        for (float s : scales) {
            UtLayoutInput in = vanillaInput();
            in.scale = s;
            const UtLayout L = utLayoutResolve(in);
            const UtRect pad = L.rect[kUtLayPad];
            // Boxes in SCALED window-local px, as the live widgets report them.
            UtRectF van[2] = {{103 * s, 74 * s, 32 * s, 32 * s}, {137 * s, 74 * s, 32 * s, 32 * s}};
            UtMatPad m = utMatPadResolve(in, L, nullptr, van, 2, s);
            ++cases;
            if (!(m.mode == kUtMatPadFull && utRectEq(m.rect, pad) && m.under == 0 && !m.user &&
                  m.src == kUtMatSrcDerived && m.padmatWhy == kUtMatWhyUnset))
                ++disagree;
            // the 15 top boxes of a modded page (phase 0's model): the pad stays where it is
            const int topX[15] = {33, 73, 117, 191, 231, 270, 335, 399, 463, 527, 591, 655, 753, 817, 881};
            UtRectF top[15];
            for (int i = 0; i < 15; ++i)
                top[i] = UtRectF{floorf(topX[i] * s + 0.5f), floorf((i < 6 ? 15 : 13) * s + 0.5f),
                                 floorf(32 * s + 0.5f), floorf(32 * s + 0.5f)};
            m = utMatPadResolve(in, L, nullptr, top, 15, s);
            ++cases;
            if (!(m.mode == kUtMatPadFull && utRectEq(m.rect, pad) && m.under > 0 &&
                  m.under == meets(pad, top, 15, s)))
                ++disagree;
            // the band walled off, and the whole page: still the whole pad, where it is
            UtRectF wall[1] = {{100 * s, 20 * s, 330 * s, 60 * s}};
            UtRectF all[1] = {{0, 0, 440 * s, 640 * s}};
            for (int k = 0; k < 2; ++k) {
                m = utMatPadResolve(in, L, nullptr, k ? all : wall, 1, s);
                ++cases;
                if (!(m.mode == kUtMatPadFull && utRectEq(m.rect, pad) && m.under == 1)) ++disagree;
            }
        }
        printf("    %d cases over 7 UI scales\n", cases);
        check(disagree == 0, "no box, the top boxes, the band or the page walled: the pad in force "
                             "where it is, the covered boxes counted");

        // Vanilla draws exactly as before: the same rectangle and cells as the collection pages
        // at every pad_y x pad_h x pad_gap the band takes, over the 24 boxes from y 76.
        {
            int same = 0, all = 0;
            for (int py = 25; py <= 31; ++py)
                for (int ph = 12; ph <= 14; ++ph)
                    for (int pg = 0; pg <= 1; ++pg) {
                        if (!utPadDefaultInBand(py, ph, pg, nullptr)) continue;
                        UtLayoutInput in = vanillaInput();
                        in.def[kUtLayPad] = utPadDefault(py, ph, pg);
                        in.padGap = pg;
                        const UtLayout L = utLayoutResolve(in);
                        UtRectF van[24];
                        for (int i = 0; i < 24; ++i)
                            van[i] = UtRectF{(float)(128 + (i % 4) * 64), (float)(76 + (i / 4) * 64),
                                             32.0f, 32.0f};
                        const UtMatPad m = utMatPadResolve(in, L, nullptr, van, 24, 1.0f);
                        ++all;
                        if (m.mode == kUtMatPadFull && utRectEq(m.rect, L.rect[kUtLayPad]) &&
                            m.under == 0)
                            ++same;
                    }
            printf("    vanilla's 24 boxes from y 76: %d of %d pad keys drawn where they are, "
                   "clear\n", same, all);
            check(all > 0 && same == all, "vanilla: the pad in force, clear of the 24 boxes, at "
                                          "every pad key in the band");
        }

        UtLayoutInput in = vanillaInput();
        in.plate = {0, 1, 974, 659};
        UtLayout L = utLayoutResolve(in);
        UtRectF rotBoxes[15];
        for (int i = 0; i < 15; ++i) rotBoxes[i] = {(float)(33 + i * 60), (float)(13 + (i % 3)), 32, 32};
        const UtRect padmat = {300, 560, 400, 60};
        UtMatPad m = utMatPadResolve(in, L, &padmat, rotBoxes, 15, 1.0f);
        check(m.mode == kUtMatPadFull && m.user && utRectEq(m.rect, padmat) &&
                  m.src == kUtMatSrcPadmat && m.padmatWhy == kUtMatWhyChosen && m.under == 0,
              "padmat_ inside the page: the pad there");
        const UtRect padmatOver = {30, 30, 400, 60};
        m = utMatPadResolve(in, L, &padmatOver, rotBoxes, 15, 1.0f);
        check(m.mode == kUtMatPadFull && utRectEq(m.rect, padmatOver) &&
                  m.under == meets(padmatOver, rotBoxes, 15, 1.0f) && m.under > 0,
              "padmat_ over the boxes: still the pad there, the boxes it covers counted");
        const UtRect padmatTop = {0, 0, 400, 60};
        m = utMatPadResolve(in, L, &padmatTop, rotBoxes, 15, 1.0f);
        check(m.mode == kUtMatPadFull && utRectEq(m.rect, L.rect[kUtLayPad]) &&
                  m.padmatWhy == kUtMatWhyOutside && m.src == kUtMatSrcDerived,
              "padmat_ off the plate (y 0 above its top 1): refused, the pad in force");
        const UtRect padmatSmall = {300, 560, 40, 10};
        m = utMatPadResolve(in, L, &padmatSmall, rotBoxes, 15, 1.0f);
        check(m.mode == kUtMatPadFull && utRectEq(m.rect, L.rect[kUtLayPad]) &&
                  m.padmatWhy == kUtMatWhyNoFit,
              "padmat_ too small for the buttons: refused, the pad in force");
        // A user pad: drawn where the user put it, whatever lies under it.
        const UtRect upad = {500, 20, 320, 44};
        in.user[kUtLayPad] = &upad;
        L = utLayoutResolve(in);
        UtRectF wall[1] = {{490, 10, 340, 70}};
        m = utMatPadResolve(in, L, nullptr, wall, 1, 1.0f);
        check(L.user[kUtLayPad] && m.mode == kUtMatPadFull && utRectEq(m.rect, upad) && m.user &&
                  m.src == kUtMatSrcPad && m.under == 1,
              "a user pad fully covered: the user's pad, where it is");
        m = utMatPadResolve(in, L, &padmat, wall, 1, 1.0f);
        check(utRectEq(m.rect, padmat) && m.src == kUtMatSrcPadmat && m.under == 0,
              "padmat_ wins over the user's pad on the materials page");
        UtLayout none = L;
        none.pad.ok = 0;
        check(utMatPadResolve(in, none, nullptr, wall, 1, 1.0f).mode == kUtMatPadNone,
              "no pad in force: nothing on the materials page either");
        in.user[kUtLayPad] = nullptr;
        // the log's text
        L = utLayoutResolve(in);
        UtRectF van[2] = {{103, 74, 32, 32}, {137, 74, 32, 32}};
        m = utMatPadResolve(in, L, nullptr, van, 2, 1.0f);
        char text[256];
        utMatPadText(m, "438x627", 2, nullptr, text, sizeof(text));
        printf("    %s\n", text);
        check(!strcmp(text, "the pad 102,25,320,44 (the derived pad) leaves the page's 2 live "
                            "box(es) clear"),
              "the text: the derived pad, clear");
        m = utMatPadResolve(in, L, &padmatTop, rotBoxes, 15, 1.0f);
        utMatPadText(m, "974x659", 15, &padmatTop, text, sizeof(text));
        printf("    %s\n", text);
        check(strstr(text, "(the derived pad) covers ") != nullptr &&
                  strstr(text, "of the page's 15 live box(es); padmat_974x659=0,0,400,60 is "
                               "refused: not inside the page") != nullptr,
              "the text: the covered count and the refused padmat_");
    }

    // ---- 7b. recorded Reign of Terror rectangles on its materials page ----------------------
    // A test logged: pad_974x659=52,1,780,153 (16 x 2 cells of 47 x 76 at UI scale 0.878, over
    // the page's top boxes), label_974x659=428,165,317,23, grid_974x659=910,76,64,584, 31
    // buttons then (30 with the class-word groups: ... GN2, BOW, SPE, REL ...), and "29 live
    // box(es) under the pad". The rule: the big pad
    // stays where it is, every button works over the boxes. Two box models: the 15 top boxes of
    // section 7 (left six 32 x 48, right nine 64 x 64, the box at 117,15 64 px wide - the derived
    // pad then meets the 7 boxes the test logged) and all 116 box records of the page (those 15,
    // else 64 x 64 where a neighbour sits 60..68 px away in the same row, else 32 x 48).
    printf("\n7b. recorded Reign of Terror rectangles on the materials page\n");
    {
        static const char* const kRotTags[30] = {
            "MAT", "HEL", "SHO", "CHE", "GLO", "BEL", "PAN", "BOO", "AMU", "MED", "RNG",
            "OFF", "SHD", "AX1", "DAG", "MC1", "SCP", "SW1", "GUN", "AX2", "MC2", "SPR",
            "SW2", "GN2", "BOW", "SPE", "REL", "JWL", "OTH", "OWN"};
        static const UtCaptions kRotCaps = {kRotTags, 30};
        static const int kTop15[15][2] = {{33, 15},  {73, 15},  {117, 15}, {191, 15}, {231, 15},
                                          {270, 15}, {335, 13}, {399, 13}, {463, 13}, {527, 13},
                                          {591, 13}, {655, 13}, {753, 13}, {817, 13}, {881, 13}};
        static const int kAll116[116][2] = {
            {117, 15},  {33, 15},   {152, 175}, {113, 160}, {191, 15},  {231, 15},  {270, 15},
            {191, 175}, {73, 15},   {33, 160},  {33, 88},   {73, 88},   {113, 88},  {152, 88},
            {191, 88},  {231, 88},  {270, 88},  {73, 160},  {234, 160}, {335, 13},  {399, 13},
            {463, 13},  {527, 13},  {335, 77},  {403, 77},  {463, 77},  {527, 77},  {591, 77},
            {655, 77},  {335, 173}, {403, 173}, {463, 173}, {335, 286}, {370, 286}, {405, 286},
            {440, 286}, {475, 286}, {547, 286}, {583, 286}, {619, 286}, {655, 286}, {691, 286},
            {780, 286}, {816, 286}, {852, 286}, {888, 286}, {924, 286}, {819, 191}, {853, 191},
            {887, 191}, {735, 406}, {769, 406}, {803, 406}, {819, 125}, {853, 125}, {890, 125},
            {35, 470},  {101, 470}, {167, 470}, {239, 406}, {305, 406}, {371, 406}, {437, 406},
            {737, 125}, {737, 191}, {753, 13},  {817, 13},  {881, 13},  {10, 540},  {74, 540},
            {138, 540}, {202, 540}, {266, 540}, {551, 406}, {585, 406}, {619, 406}, {653, 406},
            {687, 406}, {650, 540}, {714, 540}, {778, 540}, {842, 540}, {906, 540}, {591, 13},
            {655, 13},  {527, 173}, {591, 173}, {655, 173}, {849, 406}, {883, 406}, {917, 406},
            {35, 240},  {73, 240},  {111, 240}, {35, 310},  {73, 310},  {111, 310}, {147, 237},
            {148, 274}, {183, 255}, {147, 308}, {148, 344}, {183, 326}, {232, 256}, {270, 276},
            {330, 540}, {394, 540}, {458, 540}, {522, 540}, {586, 540}, {187, 433}, {35, 398},
            {73, 398},  {111, 398}, {187, 398}, {149, 398}};
        // record px boxes of the two models
        std::vector<UtRect> top15, all116;
        for (int i = 0; i < 15; ++i)
            top15.push_back(i == 2  ? UtRect{kTop15[i][0], kTop15[i][1], 64, 48}
                            : i < 6 ? UtRect{kTop15[i][0], kTop15[i][1], 32, 48}
                                    : UtRect{kTop15[i][0], kTop15[i][1], 64, 64});
        for (int i = 0; i < 116; ++i) {
            bool wide = false;
            for (int j = 0; j < 116 && !wide; ++j) {
                if (j == i || abs(kAll116[j][1] - kAll116[i][1]) > 1) continue;
                const int dx = abs(kAll116[j][0] - kAll116[i][0]);
                wide = dx >= 60 && dx <= 68;
            }
            UtRect b = wide ? UtRect{kAll116[i][0], kAll116[i][1], 64, 64}
                            : UtRect{kAll116[i][0], kAll116[i][1], 32, 48};
            for (const UtRect& t : top15)
                if (t.x == b.x && t.y == b.y) b = t;
            all116.push_back(b);
        }
        const UtRect uGrid = {910, 76, 64, 584}, uPad = {52, 1, 780, 153},
                     uLabel = {428, 165, 317, 23};
        const UtRect oldPad = {512, 1, 320, 44}, oldLabel = {511, 51, 317, 23};
        auto rotInput = [&](float s) {
            UtLayoutInput r = vanillaInput();
            r.plate = utPlateRectFromP(12, 76, 962, 584, 974, 659);
            r.def[kUtLayGrid] = {12, 76, 962, 584};
            r.buttons = 30;
            r.widestCtx = &kRotCaps;
            r.labelSize = 13;
            r.scale = s;
            return r;
        };
        // the boxes a scaled rectangle meets, one by one
        auto meets = [](const UtRect& r, const std::vector<UtRect>& bx, float s) {
            int k = 0;
            const UtRectF f = utScaledRectF(r, s);
            for (const UtRect& b : bx)
                if (utRectsOverlap(f, utScaledRectF(b, s))) ++k;
            return k;
        };
        // true when the decision is the whole pad at `want`, every button inside it, the boxes it
        // covers counted exactly
        auto padWhere = [&](const UtMatPad& m, const UtRect& want, const UtPadFit& fit,
                            const std::vector<UtRect>& bx, float s) {
            if (m.mode != kUtMatPadFull || !utRectEq(m.rect, want)) return false;
            if (m.under != meets(want, bx, s)) return false;
            for (int i = 0; i < fit.buttons; ++i)
                if (!utRectInside(utPadCellRect(fit, i), want)) return false;
            return true;
        };
        const float scales[] = {0.7f, 0.8f, 0.878f, 0.928f, 1.0f, 1.004f, 1.25f, 1.5f, 2.0f};
        const std::vector<UtRect>* models[2] = {&top15, &all116};
        const char* const modelName[2] = {"15 top boxes", "116 boxes"};
        int bad = 0, cases = 0, user3 = 0, covered = 0;
        for (float s : scales) {
            UtLayoutInput in = rotInput(s);
            in.user[kUtLayGrid] = &uGrid;
            in.user[kUtLayPad] = &uPad;
            in.user[kUtLayLabel] = &uLabel;
            const UtLayout L = utLayoutResolve(in);
            if (L.user[kUtLayGrid] && L.user[kUtLayPad] && L.user[kUtLayLabel]) ++user3;
            for (int k = 0; k < 2; ++k) {
                std::vector<UtRectF> bf;
                for (const UtRect& b : *models[k]) bf.push_back(utScaledRectF(b, s));
                const UtMatPad m = utMatPadResolve(in, L, nullptr, bf.data(), (int)bf.size(), s);
                ++cases;
                const bool ok = padWhere(m, uPad, L.pad, *models[k], s) && m.src == kUtMatSrcPad &&
                                m.user && m.under > 0;
                if (!ok) ++bad;
                if (m.under > 0) ++covered;
                if (s == 0.878f || !ok) {
                    char text[256];
                    utMatPadText(m, "974x659", (int)bf.size(), nullptr, text, sizeof(text));
                    printf("    %.3f %s: pad %d x %d of %d x %d - %s\n", s, modelName[k],
                           L.pad.cols, L.pad.rows, L.pad.cellW, L.pad.cellH, text);
                }
                if (s == 0.878f && k == 0)
                    check(L.pad.cols == 16 && L.pad.rows == 2 && L.pad.cellW == 47 &&
                              L.pad.cellH == 76 && L.pad.caption == 14,
                          "the recorded pad at 0.878 is the logged 16 x 2 cells of 47 x 76, "
                          "caption 14");
                if (s == 0.878f && k == 1) {
                    check(m.under == 29, "116 boxes: the recorded pad covers 29 of them, as logged");
                    char text[256];
                    utMatPadText(m, "974x659", (int)bf.size(), nullptr, text, sizeof(text));
                    check(!strcmp(text, "the pad 52,1,780,153 (pad_974x659) covers 29 of the "
                                        "page's 116 live box(es)"),
                          "the log's text for it");
                }
            }
            // the pad before it, 512,1,320,44 with the label at 511,51: one button at 719,1 then
            UtLayoutInput o = rotInput(s);
            o.user[kUtLayGrid] = &uGrid;
            o.user[kUtLayPad] = &oldPad;
            o.user[kUtLayLabel] = &oldLabel;
            const UtLayout OL = utLayoutResolve(o);
            for (int k = 0; k < 2; ++k) {
                std::vector<UtRectF> bf;
                for (const UtRect& b : *models[k]) bf.push_back(utScaledRectF(b, s));
                const UtMatPad m = utMatPadResolve(o, OL, nullptr, bf.data(), (int)bf.size(), s);
                ++cases;
                if (!OL.user[kUtLayPad] || !padWhere(m, oldPad, OL.pad, *models[k], s)) ++bad;
            }
            // the derived 11 x 3 pad (no user rectangle): where it is, over 7 boxes as logged
            UtLayoutInput d = rotInput(s);
            const UtLayout DL = utLayoutResolve(d);
            for (int k = 0; k < 2; ++k) {
                std::vector<UtRectF> bf;
                for (const UtRect& b : *models[k]) bf.push_back(utScaledRectF(b, s));
                const UtMatPad m = utMatPadResolve(d, DL, nullptr, bf.data(), (int)bf.size(), s);
                ++cases;
                if (!padWhere(m, DL.rect[kUtLayPad], DL.pad, *models[k], s) ||
                    m.src != kUtMatSrcDerived || m.user)
                    ++bad;
                if ((s == 1.004f || s == 0.878f) && k == 0) {
                    printf("    %.3f the derived pad %d x %d of %d x %d at %d,%d: %d box(es) "
                           "under it\n",
                           s, DL.pad.cols, DL.pad.rows, DL.pad.cellW, DL.pad.cellH,
                           m.rect.x, m.rect.y, m.under);
                    check(DL.pad.cols == 11 && DL.pad.rows == 3 && m.under == 7 &&
                              m.rect.x == 102 && m.rect.y == 25,
                          "the derived 11 x 3 pad at 1.004 and 0.878: drawn at 102,25 over the 7 "
                          "boxes the test logged");
                }
            }
            // padmat_ wins; off the plate it is refused and the user's pad stays
            const UtRect pmIn = {300, 600, 400, 58}, pmOut = {0, 0, 300, 40};
            std::vector<UtRectF> bf;
            for (const UtRect& b : all116) bf.push_back(utScaledRectF(b, s));
            const UtMatPad mi = utMatPadResolve(in, L, &pmIn, bf.data(), (int)bf.size(), s);
            const UtPadFit pf = utPadFit(pmIn, 30, in.padGap, s, in.captionMax, in.widest,
                                         in.widestCtx);
            ++cases;
            if (!pf.ok || !padWhere(mi, pmIn, pf, all116, s) || mi.src != kUtMatSrcPadmat) ++bad;
            const UtMatPad mo = utMatPadResolve(in, L, &pmOut, bf.data(), (int)bf.size(), s);
            ++cases;
            if (!padWhere(mo, uPad, L.pad, all116, s) || mo.padmatWhy != kUtMatWhyOutside) ++bad;
        }
        printf("    %d cases over %d UI scales and 2 box models\n", cases,
               (int)(sizeof(scales) / sizeof(scales[0])));
        check(user3 == (int)(sizeof(scales) / sizeof(scales[0])),
              "the three recorded ROT rectangles are in force at every UI scale");
        check(covered == 2 * (int)(sizeof(scales) / sizeof(scales[0])),
              "the recorded pad covers boxes in both models at every UI scale");
        check(bad == 0, "every decision is the whole pad where it is (the recorded one, the old one, the "
                        "derived one, padmat_ inside the page), every button inside it, the "
                        "covered boxes counted; padmat_ off the plate refused");

        // the log text: the refused padmat_, and cut, never overrun
        UtLayoutInput in = rotInput(0.878f);
        in.user[kUtLayGrid] = &uGrid;
        in.user[kUtLayPad] = &uPad;
        in.user[kUtLayLabel] = &uLabel;
        const UtLayout L = utLayoutResolve(in);
        std::vector<UtRectF> bf;
        for (const UtRect& b : all116) bf.push_back(utScaledRectF(b, 0.878f));
        const UtRect pm = {0, 0, 300, 40};
        const UtMatPad m = utMatPadResolve(in, L, &pm, bf.data(), (int)bf.size(), 0.878f);
        char text[256];
        utMatPadText(m, "974x659", (int)bf.size(), &pm, text, sizeof(text));
        printf("    with padmat_ 0,0,300,40: %s\n", text);
        check(!strcmp(text, "the pad 52,1,780,153 (pad_974x659) covers 29 of the page's 116 live "
                            "box(es); padmat_974x659=0,0,300,40 is refused: not inside the page"),
              "the text names the pad, its covered boxes and the refused padmat_");
        char small[40];
        const int n = utMatPadText(m, "974x659", (int)bf.size(), &pm, small, sizeof(small));
        check(n < (int)sizeof(small) && strlen(small) < sizeof(small) && n == (int)strlen(small),
              "the text is cut, never overrun");
    }

    // ---- 8. from the ini to the layout in force (what the live page runs) ------------------------
    printf("\n8. the ini to the page\n");
    {
        // The plate rectangle out of the P line, on the four covers the research measured.
        check(utRectEq(utPlateRectFromP(105, 76, 333, 552, 438, 627), UtRect{0, 1, 438, 627}) &&
                  utRectEq(utPlateRectFromP(128, 76, 310, 550, 346, 625), UtRect{92, 1, 346, 625}) &&
                  utRectEq(utPlateRectFromP(8, 76, 967, 866, 975, 968), UtRect{0, -26, 975, 968}) &&
                  utRectEq(utPlateRectFromP(12, 76, 962, 584, 974, 659), UtRect{0, 1, 974, 659}),
              "the plate from the P line: vanilla 0,1  Baul 92,1  X-Mod 0,-26  Reign of Terror 0,1");

        UtLayoutSource src = {};
        src.x0 = 105;
        src.y0 = 76;
        src.usableW = 333;
        src.usableH = 552;
        src.plateW = 438;
        src.plateH = 627;
        src.maxCellW = 64;
        src.maxCellH = 128;
        src.rowGap = kUtRowGap;
        src.padY = 25;
        src.padH = 14;
        src.padGap = 1;
        src.labelSize = 13;
        src.captionMax = 14;
        src.buttons = 26;
        src.widest = utWidestCaptionPx;
        src.widestCtx = &kCaps;

        // TODAY'S INI (no [layout] line): every legal pad_y x pad_h x pad_gap at seven UI scales
        // gives 1.1.0's pad cell for cell (padCellRec) and 1.1.0's caption (buttonTagSize over the
        // drawn rects); every illegal one gives no pad; the label is 1.1.0's wherever 1.1.0 drew it.
        const float scales[] = {0.7f, 0.8f, 0.928f, 1.0f, 1.25f, 1.5f, 2.0f};
        int combos = 0, bad = 0, refusedOk = 0, labelCases = 0, labelBad = 0, bounded = 0;
        for (float s : scales) {
            src.scale = s;
            for (int padY = 8; padY <= 60; ++padY)
                for (int padH = 12; padH <= 20; ++padH)
                    for (int gap = 0; gap <= 4; ++gap) {
                        src.padY = padY;
                        src.padH = padH;
                        src.padGap = gap;
                        UtLayoutInput in;
                        UtRect store[kUtLayParts];
                        const UtLayout L = utLayoutFromSource(src, &in, store);
                        const int h = 3 * padH + 2 * gap;
                        const bool legal = padY >= 25 && padY + h - 1 <= 69;   // x: 102 + 312 + 8*gap - 1 <= 422
                        const bool legalX = 102 + 6 + 306 + 8 * gap - 1 <= 422;
                        if (!(legal && legalX)) {
                            if (!L.pad.ok) ++refusedOk; else ++bad;
                            continue;
                        }
                        ++combos;
                        if (!L.pad.ok || L.pad.cols != 9 || L.pad.rows != 3 || L.user[kUtLayPad]) {
                            ++bad;
                            continue;
                        }
                        int minW = 1 << 24, minH = 1 << 24;
                        for (int i = 0; i < 26; ++i) {
                            const UtRect c = utPadCellRect(L.pad, i);
                            const UtRect t = todayCell(padY, padH, gap, i);
                            if (!utRectEq(c, t)) ++bad;
                            if (i < 25) {
                                const UtRectF f = utScaledRectF(t, s);
                                const int w = (int)(f.w + 0.5f), hh = (int)(f.h + 0.5f);
                                if (w < minW) minW = w;
                                if (hh < minH) minH = hh;
                            }
                        }
                        // ut_panel.cpp's buttonTagSize, the 1.1.0 body
                        int top = (minH * 3) / 4;
                        if (top > minH - 2) top = minH - 2;
                        if (top > 14) top = 14;
                        int fs = 0;
                        for (int cand = top; cand >= 6; --cand)
                            if (utWidestCaptionPx(cand, &kCaps) <= minW - 1) {
                                fs = cand;
                                break;
                            }
                        if (fs != L.pad.caption) ++bad;
                    }
            src.padY = 25;
            src.padH = 14;
            src.padGap = 1;
            for (int size = 8; size <= 32; ++size) {
                src.labelSize = size;
                UtLayoutInput in;
                UtRect store[kUtLayParts];
                const UtLayout L = utLayoutFromSource(src, &in, store);
                // 1.1.0: fs = round(size * s), at least 9, dropped when 9 + fs / s > 24
                int fs = (int)((float)size * s + 0.5f);
                if (fs < 9) fs = 9;
                ++labelCases;
                if ((float)9 + (float)fs / s > 24.0f) {
                    if (!L.label.ok || !L.label.bounded || L.label.size >= fs) ++labelBad;
                    ++bounded;
                } else if (!L.label.ok || L.label.size != fs || L.label.x != 105 || L.label.top != 9 ||
                           L.label.limitPx != (int)(317.0f * s + 0.5f)) {
                    ++labelBad;
                }
            }
            src.labelSize = 13;
        }
        printf("    %d legal pad keys x scales, %d refused like 1.1.0; %d label cases (%d brought down)\n",
               combos, refusedOk, labelCases, bounded);
        check(bad == 0 && combos > 0 && refusedOk > 0,
              "today's ini: 1.1.0's pad cell for cell and its caption at every scale, or no pad");
        check(labelBad == 0, "today's ini: 1.1.0's label wherever it drew one; a taller size is brought down");

        // The research's user test on the vanilla cover: a lower grid, then a bigger pad above it.
        src.scale = 1.0f;
        const char* grid = "105,120,333,460";
        const char* pad = "102,30,320,80";
        src.grid = grid;
        UtLayoutInput in;
        UtRect store[kUtLayParts];
        UtLayout L = utLayoutFromSource(src, &in, store);
        check(L.user[kUtLayGrid] && L.rect[kUtLayGrid].x == 105 && L.rect[kUtLayGrid].y == 120 &&
                  !L.user[kUtLayPad] && L.pad.cols == 9,
              "grid_438x627=105,120,333,460: the grid moves, today's pad stays");
        // per group: 64x64 -> 5 x 6 (460 / 66), 32x32 -> 10 x 13, 64x128 -> 5 x 3, under the 160 cap
        const UtGrid g64 = utLiveGroupGrid(L, 5, 8, 64, 64, kUtRowGap, 160);
        const UtGrid g32 = utLiveGroupGrid(L, 10, 16, 32, 32, kUtRowGap, 160);
        const UtGrid g128 = utLiveGroupGrid(L, 5, 4, 64, 128, kUtRowGap, 160);
        printf("    the moved grid: 64x64 %d x %d, 32x32 %d x %d, 64x128 %d x %d\n", g64.cols, g64.rows,
               g32.cols, g32.rows, g128.cols, g128.rows);
        check(g64.cols == 5 && g64.rows == 6 && g32.cols == 10 && g32.rows == 13 && g128.cols == 5 &&
                  g128.rows == 3,
              "each group re-fitted in the user's rectangle: 5 x 6, 10 x 13, 5 x 3");
        const UtGrid c144 = utLiveGroupGrid(L, 10, 16, 32, 32, kUtRowGap, 120);
        check(c144.cols == 10 && c144.rows == 12 && c144.capped, "the live frame's box count caps it");
        src.grid = nullptr;
        L = utLayoutFromSource(src, &in, store);
        const UtGrid gen = utLiveGroupGrid(L, 5, 8, 64, 64, kUtRowGap, 160);
        check(!L.user[kUtLayGrid] && gen.cols == 5 && gen.rows == 8,
              "no grid_ key: the G line's own cols x rows, untouched");

        src.grid = grid;
        src.pad = pad;
        L = utLayoutFromSource(src, &in, store);
        check(L.user[kUtLayGrid] && L.user[kUtLayPad] && L.pad.cols == 7 && L.pad.rows == 4 &&
                  L.pad.cellW == 44 && L.pad.cellH == 19 && L.pad.caption == 14,
              "+ pad_438x627=102,30,320,80: the pad re-fits to 7 x 4 of 44 x 19, caption 14");
        const char* padOver = "102,30,320,120";   // reaches y 149: over the moved grid
        src.pad = padOver;
        L = utLayoutFromSource(src, &in, store);
        check(L.user[kUtLayGrid] && !L.user[kUtLayPad] && L.why[kUtLayPad] == kUtLayoutOverlap &&
                  L.against[kUtLayPad] == kUtLayGrid && L.pad.cols == 9,
              "a pad over the grid: refused (overlaps the grid), today's pad drawn");
        printf("    refusal text: pad %s (the %s)\n", utLayoutWhyText(L.why[kUtLayPad]),
               utLayoutPartName(L.against[kUtLayPad]));
        const char* junk = "1,2,3";
        src.pad = junk;
        L = utLayoutFromSource(src, &in, store);
        check(!in.user[kUtLayPad] && L.why[kUtLayPad] == kUtLayoutUnset,
              "a value that does not parse counts as no key");
        src.pad = nullptr;
        src.grid = nullptr;
        src.padY = 8;   // 1.1.0 refuses this pad (above the band): no pad, nothing else moves
        L = utLayoutFromSource(src, &in, store);
        check(!L.pad.ok && L.label.ok && !L.user[kUtLayGrid], "pad_y=8: no pad, as in 1.1.0");
        src.padY = 25;

        // The row across a cols change keeps the first visible item.
        check(utRowKeep(4, 5, 10) == 2 && utRowKeep(3, 10, 5) == 6 && utRowKeep(7, 5, 5) == 7 &&
                  utRowKeep(0, 5, 3) == 0 && utRowKeep(5, 0, 3) == 0,
              "utRowKeep: row 4 of 5 -> row 2 of 10, row 3 of 10 -> row 6 of 5");

        // The generator's grid: the same resolution, no UI scale needed, no caption table needed.
        {
            UtLayoutSource gsrc = src;
            gsrc.scale = 0.0f;
            gsrc.widest = nullptr;
            gsrc.widestCtx = nullptr;
            int user = -1;
            UtRect gr = utLayoutGridRect(gsrc, &user);
            check(user == 0 && utRectEq(gr, UtRect{105, 76, 333, 552}),
                  "utLayoutGridRect: no grid_ key -> the P line's rectangle");
            gsrc.grid = grid;
            gr = utLayoutGridRect(gsrc, &user);
            check(user == 1 && utRectEq(gr, UtRect{105, 120, 333, 460}),
                  "utLayoutGridRect: grid_438x627=105,120,333,460 -> that rectangle");
            const char* overPad = "105,40,333,500";   // meets today's pad (y 25..68)
            gsrc.grid = overPad;
            gr = utLayoutGridRect(gsrc, &user);
            check(user == 0 && utRectEq(gr, UtRect{105, 76, 333, 552}),
                  "utLayoutGridRect: a grid over today's pad falls back, as on the live page");
        }

        // The generator's stamp text.
        char canon[64];
        check(utLayoutCanonical(" 105 , +120,333 ,460 ", canon, sizeof(canon)) > 0 &&
                  !strcmp(canon, "105,120,333,460") && utLayoutCanonical("1,2,3", canon, sizeof(canon)) == 0 &&
                  canon[0] == 0 && utLayoutCanonical(nullptr, canon, sizeof(canon)) == 0,
              "utLayoutCanonical: blanks and + gone, junk and null give nothing");
    }

    // ---- 9. the pad keyed by the group table (ut_padpage.h) ------------------------------------
    {
        printf("\n9. the pad keyed by the G lines' ids and tags, and its pages\n");
        const UtPadPages v = utPadPaginate(24, [](int) { return true; });
        bool ident = v.pages == 1 && v.buttons == 26 && v.perPage == 24;
        for (int i = 0; i < 26; ++i) ident = ident && utPadSlotLogical(v, 0, i) == i;
        check(ident && utPadOwnIndex(v) == 25 && utPadLogicalCount(v) == 26 &&
                  utPadSlotLogical(v, 0, 26) == -1,
              "24 groups that fit: one page, slot i IS button i (1.1.0's order), OWN last");
        const float scales[7] = {0.7f, 0.8f, 0.928f, 1.0f, 1.25f, 1.5f, 2.0f};
        const UtRect today = utPadDefault(25, 14, 1);
        struct View {
            const char* dir;
            size_t groups;
        };
        // the shipped default (collect_mi=1): DeathGrimDawn has 30 groups (+ spear1h "Artifacts")
        const View views[3] = {{"", 25}, {"\\views\\reignofterror", 29}, {"\\views\\deathgrimdawn", 30}};
        for (const View& vw : views) {
            const std::string path = root + "\\data\\oracle" + vw.dir + "\\uniq-groups.txt";
            std::string text;
            if (!readFile(path, &text)) {
                printf("    %s is not here - SKIPPED\n", path.c_str());
                continue;
            }
            std::vector<std::string> tags(1, "MAT"), ids;
            size_t at = 0;
            while (at < text.size()) {
                size_t e = text.find('\n', at);
                if (e == std::string::npos) e = text.size();
                std::string line = text.substr(at, e - at);
                at = e + 1;
                if (!line.empty() && line.back() == '\r') line.pop_back();
                utg::GLine g;
                if (line.compare(0, 2, "G\t") == 0 && utg::parseGLine(line, g)) {
                    ids.push_back(g.id);
                    tags.push_back(g.tag);
                }
            }
            tags.push_back("OWN");
            std::vector<const char*> ptr;
            for (const std::string& t : tags) ptr.push_back(t.c_str());
            const UtCaptions caps = {ptr.data(), (int)ptr.size()};
            check(ids.size() == vw.groups, (std::string("the G lines of ") + path + " give the group count").c_str());
            if (!*vw.dir) {
                // 1.1.0's MAT + 24 groups, then 1.2.0's Blueprints, then OWN
                bool same = tags.size() == 27 && tags[25] == "BLU" && tags[26] == kTags[25];
                for (size_t i = 0; same && i < 25; ++i) same = tags[i] == kTags[i];
                check(same, "vanilla: the 27 captions keyed by the G lines' tags are 1.1.0's table with BLU after REL");
            }
            bool all = true;
            for (float s : scales) {
                const int n = (int)ptr.size();
                const UtPadPages pg = utPadPaginate((int)ids.size(), [&](int k) {
                    return utPadFit(today, k, 1, s, 14, utWidestCaptionPx, &caps).ok != 0;
                });
                const UtPadFit f = utPadFit(today, n, 1, s, 14, utWidestCaptionPx, &caps);
                all = all && pg.pages == 1 && pg.buttons == n && f.ok && f.caption >= kUtCaptionMin;
                if (s == 1.0f)
                    printf("    %-22s %zu groups: %d buttons, %d x %d cells of %d x %d, caption %d\n",
                           !*vw.dir ? "vanilla" : vw.dir + 7, ids.size(), n, f.cols, f.rows, f.cellW,
                           f.cellH, f.caption);
                if (!*vw.dir && s == 1.0f)   // 1.1.0's 26 buttons were 9 x 3 cells of 34 x 14; BLU makes 27
                    check(f.cols == 10 && f.rows == 3 && f.cellW == 30 && f.cellH == 14 && f.caption == 10,
                          "vanilla at UI scale 1: 27 buttons in 10 x 3 cells of 30 x 14, caption 10 (1.1.0's size)");
            }
            check(all, "every button fits today's pad at the seven UI scales - no page needed");
        }
        // Other (the generator's own last group) is a G line like any other: it gets its own
        // button, captioned by its tag.
        {
            utg::GLine r;
            r.index = 24;
            r.label = "Other";
            r.cols = 5; r.rows = 8; r.cellW = 64; r.cellH = 64; r.count = 3;
            r.id = "other";
            r.tag = "OTH";
            utg::GLine back;
            const bool ok = utg::parseGLine(utg::formatGLine(r), back);
            std::vector<const char*> p(kTags, kTags + 25);
            p.push_back(back.tag.c_str());
            p.push_back("OWN");
            const UtCaptions c = {p.data(), (int)p.size()};
            const UtPadFit f = utPadFit(today, 27, 1, 1.0f, 14, utWidestCaptionPx, &c);
            const UtPadPages pg = utPadPaginate(25, [&](int k) {
                return utPadFit(today, k, 1, 1.0f, 14, utWidestCaptionPx, &c).ok != 0;
            });
            check(ok && back.id == "other" && f.ok && pg.pages == 1 &&
                      utPadSlotLogical(pg, 0, 25) == 25 && !strcmp(p[25], "OTH") &&
                      utPadOwnIndex(pg) == 26,
                  "Other (id other, tag OTH) is button 25 of 27, drawn like any group");
        }
        // A table that cannot fit is paged: MAT, K groups, NEXT, OWN on every page.
        {
            std::vector<const char*> p(1, "MAT");
            for (int i = 0; i < 120; ++i) p.push_back(kTags[1 + i % 24]);
            p.push_back("OWN");
            p.push_back(">>");
            const UtCaptions c = {p.data(), (int)p.size()};
            const UtPadPages pg = utPadPaginate(120, [&](int k) {
                return utPadFit(today, k, 1, 1.0f, 14, utWidestCaptionPx, &c).ok != 0;
            });
            bool once = pg.pages > 1 && pg.perPage >= 1 && pg.buttons == pg.perPage + 3;
            std::vector<int> seen(120, 0);
            for (int page = 0; once && page < pg.pages; ++page) {
                once = utPadSlotLogical(pg, page, 0) == 0 &&
                       utPadSlotLogical(pg, page, pg.buttons - 1) == utPadOwnIndex(pg) &&
                       utPadSlotLogical(pg, page, pg.buttons - 2) == utPadNextIndex(pg);
                for (int s = 1; s <= pg.perPage; ++s) {
                    const int b = utPadSlotLogical(pg, page, s);
                    if (b < 0) continue;
                    ++seen[(size_t)(b - 1)];
                    once = once && utPadPageOf(pg, b - 1) == page;
                }
            }
            for (int g = 0; g < 120; ++g) once = once && seen[(size_t)g] == 1;
            check(once, "120 groups in today's pad: paged, every group on exactly one page, MAT first, "
                        "NEXT and OWN last");
            check(utPadNextTarget(pg, 0) == pg.perPage && utPadNextTarget(pg, pg.pages - 1) == 0 &&
                      utPadLogicalCount(pg) == 123,
                  "NEXT selects the next page's first group, and the last page wraps to group 0");
            const UtPadFit f = utPadFit(today, pg.buttons, 1, 1.0f, 14, utWidestCaptionPx, &c);
            const UtPadFit f1 = utPadFit(today, pg.buttons + 1, 1, 1.0f, 14, utWidestCaptionPx, &c);
            check(f.ok && f.caption >= kUtCaptionMin && !f1.ok,
                  "K is the largest page that still carries a readable caption");
            printf("    120 groups at UI scale 1: %d pages of %d, %d x %d cells, caption %d\n", pg.pages,
                   pg.perPage, f.cols, f.rows, f.caption);
        }
    }

    // ---- 10. the screen parts: pad_screen / label_screen ----------------------------------------
    printf("\n10. pad_screen / label_screen: placed at a screen corner, resolved against the page\n");
    {
        // 10a. the text form.
        UtScreenRect r = {};
        check(utScreenRectParse("tr,8,8,320,44", &r) && r.corner == kUtCornerTR && r.x == 8 &&
                  r.y == 8 && r.w == 320 && r.h == 44,
              "tr,8,8,320,44");
        check(utScreenRectParse(" BL , 0 ,16384, 1,1 ", &r) && r.corner == kUtCornerBL && r.x == 0 &&
                  r.y == 16384 && r.w == 1 && r.h == 1,
              "blanks, upper case, the bounds 0 and 16384");
        const char* bad[] = {"",           "tr",           "tr,1,2,3",       "tr,1,2,3,4,5",
                             "tr,-1,2,3,4", "tr,1,2,0,4",  "tr,1,2,3,0",     "xx,1,2,3,4",
                             "t,1,2,3,4",  "trr,1,2,3,4",  "tr 1,2,3,4",     "tr,1,2,3,16385",
                             "tr,,2,3,4",  "tr,1,2,3,4x",  "1,2,3,4",        "tr,-0,2,3,4",
                             "tl,1,2,3,4;", "lt,1,2,3,4"};
        int refused = 0;
        for (const char* b : bad) refused += utScreenRectParse(b, &r) ? 0 : 1;
        check(refused == (int)(sizeof(bad) / sizeof(bad[0])),
              "eighteen malformed values refused (a minus sign anywhere, 0 sizes, 16385, junk)");
        bool fmt = true;
        char buf[48];
        for (int c = 0; c < 4; ++c) {
            const UtScreenRect a = {c, 12, 345, 320, 44};
            UtScreenRect b = {};
            fmt = fmt && utScreenRectFormat(a, buf, sizeof(buf)) > 0 && utScreenRectParse(buf, &b) &&
                  utScreenRectEq(a, b);
        }
        check(fmt && utScreenRectFormat({kUtCornerTR, 8, 8, 320, 44}, buf, sizeof(buf)) &&
                  !strcmp(buf, "tr,8,8,320,44") && !strcmp(utCornerName(kUtCornerBR), "br"),
              "the format is the parse's own, for all four corners");
        check(strcmp(utLayoutWhyText(kUtLayoutScreenOverGrid), "?") != 0 &&
                  strcmp(utLayoutWhyText(kUtLayoutScreenLarge), "?") != 0 &&
                  strcmp(utLayoutWhyText(kUtLayoutScreenNoView), "?") != 0,
              "the three new refusals have their words");

        // 10b. the corner placement: {corner, 8, 8, 320, 44}, hand-computed client px.
        struct Sc {
            int W, H;
            float s;
            int pw, ph, ix;   // floorf(v*s + 0.5f) of 320, 44 and 8
            float ox, oy;     // a logged window origin at that scale
        } scs[3] = {{1920, 1080, 1.0f, 320, 44, 8, 1169.0f, 451.0f},
                    {2560, 1440, 0.928f, 297, 41, 7, 172.0f, 133.7f},
                    {3440, 1440, 0.7f, 224, 31, 6, 297.0f, 195.6f}};
        bool place = true, trip = true, inside = true;
        for (const Sc& sc : scs) {
            for (int c = 0; c < 4; ++c) {
                const UtScreenRect q = {c, 8, 8, 320, 44};
                int sh = -1;
                const UtRectF f = utScreenPlace(q, sc.W, sc.H, sc.s, &sh);
                const bool right = c == kUtCornerTR || c == kUtCornerBR;
                const bool bottom = c == kUtCornerBL || c == kUtCornerBR;
                const float wx = right ? (float)(sc.W - sc.ix - sc.pw) : (float)sc.ix;
                const float wy = bottom ? (float)(sc.H - sc.ix - sc.ph) : (float)sc.ix;
                const bool ok = f.x == wx && f.y == wy && f.w == (float)sc.pw && f.h == (float)sc.ph &&
                                sh == 0;
                if (!ok)
                    printf("    %dx%d %.3f %s: %.1f,%.1f %.0fx%.0f (want %.0f,%.0f %dx%d)\n", sc.W, sc.H,
                           sc.s, utCornerName(c), f.x, f.y, f.w, f.h, wx, wy, sc.pw, sc.ph);
                place = place && ok;
                // The record <-> client round trip at the logged window origin.
                const UtScreenSpot sp = utScreenLocate(q, 1, sc.W, sc.H, sc.ox, sc.oy, sc.s);
                const UtRectF back = utLocalToClient(sp.local, sc.ox, sc.oy, sc.s);
                const float tol = sc.s / 2.0f + 1.0f;
                trip = trip && sp.why == kUtLayoutOk && sp.local.w == 320 && sp.local.h == 44 &&
                       back.x == sp.px.x && back.y == sp.px.y && back.w == sp.px.w && back.h == sp.px.h &&
                       fabsf(sp.px.x - f.x) <= tol && fabsf(sp.px.y - f.y) <= tol &&
                       fabsf(sp.px.w - f.w) <= 1.0f && fabsf(sp.px.h - f.h) <= 1.0f;
                inside = inside && sp.px.x >= 0.0f && sp.px.y >= 0.0f &&
                         sp.px.x + sp.px.w <= (float)sc.W && sp.px.y + sp.px.h <= (float)sc.H;
                const UtRect rl = utScreenToLocal(sp.px, 320, 44, sc.ox, sc.oy, sc.s);
                const UtRectF again = utLocalToClient(rl, sc.ox, sc.oy, sc.s);
                trip = trip && fabsf(again.x - sp.px.x) <= tol && fabsf(again.y - sp.px.y) <= tol;
            }
        }
        check(place, "1920x1080/1.0, 2560x1440/0.928, 3440x1440/0.7: all four corners where the "
                     "arithmetic says, unshifted");
        printf("    tr,8,8,320,44: 1592,8 320x44 / 2256,7 297x41 / 3210,6 224x31\n");
        check(trip, "client -> window-local record px -> client lands within s/2 + 1 px at the "
                    "three logged origins (1169,451 / 172,133.7 / 297,195.6), size kept");
        check(inside, "and every drawn rectangle is on the screen");
        {
            const UtScreenSpot sp = utScreenLocate({kUtCornerTR, 8, 8, 320, 44}, 1, 1920, 1080, 1169.0f,
                                                   451.0f, 1.0f);
            check(utRectEq(sp.local, UtRect{1592 - 1169, 8 - 451, 320, 44}) && sp.px.x == 1592.0f &&
                      sp.px.y == 8.0f,
                  "at 1.0 with a whole-pixel origin the round trip is exact: local 423,-443");
        }

        // 10c. the shift into the viewport (never resized).
        {
            int sh = 0;
            UtRectF f = utScreenPlace({kUtCornerTR, 3000, 8, 320, 44}, 1920, 1080, 1.0f, &sh);
            check(sh == 1 && f.x == 0.0f && f.y == 8.0f && f.w == 320.0f,
                  "tr,3000,...: the offset passes the left edge - shifted to x 0, same size");
            f = utScreenPlace({kUtCornerTL, 1800, 1050, 320, 44}, 1920, 1080, 1.0f, &sh);
            check(sh == 1 && f.x == 1600.0f && f.y == 1036.0f, "tl,1800,1050: back to 1600,1036");
            f = utScreenPlace({kUtCornerBR, 0, 0, 320, 44}, 1920, 1080, 1.0f, &sh);
            check(sh == 0 && f.x == 1600.0f && f.y == 1036.0f, "br,0,0 touches the corner, unshifted");
            f = utScreenPlace({kUtCornerBL, 5000, 3000, 320, 44}, 3440, 1440, 0.7f, &sh);
            check(sh == 1 && f.x == 3216.0f && f.y == 0.0f && f.w == 224.0f && f.h == 31.0f,
                  "bl,5000,3000 at 3440x1440/0.7: 3216,0 224x31");
            const UtScreenSpot sp = utScreenLocate({kUtCornerTL, 0, 0, 320, 44}, 1, 2560, 1440, 172.0f,
                                                   133.7f, 0.928f);
            check(sp.why == kUtLayoutOk && sp.px.x >= 0.0f && sp.px.y >= 0.0f,
                  "tl,0,0 at 0.928 with a fractional origin: the rounding never puts it off screen");
            check(utScreenLocate({kUtCornerTL, 0, 0, 1920, 40}, 1, 1920, 1080, 0.0f, 0.0f, 1.0f).why ==
                          kUtLayoutScreenLarge &&
                      utScreenLocate({kUtCornerTL, 0, 0, 320, 44}, 0, 1920, 1080, 0.0f, 0.0f, 1.0f).why ==
                          kUtLayoutScreenNoView &&
                      utScreenLocate({kUtCornerTL, 0, 0, 320, 44}, 1, 0, 0, 0.0f, 0.0f, 1.0f).why ==
                          kUtLayoutScreenNoView,
                  "as wide as the screen: large; no frame or no screen size yet: no view");
        }

        // 10d. the resolver. The vanilla page on a 1920x1080 screen at 1.0, the window at 10,200
        //      (its grid 105,76,333,552 is client x 115..448, y 276..828).
        UtLayoutInput base = vanillaInput();
        base.screenKnown = 1;
        base.screenW = 1920;
        base.screenH = 1080;
        base.originX = 10.0f;
        base.originY = 200.0f;
        UtLayoutInput in = base;
        in.screenSet[kUtLayPad] = 1;
        in.screenRect[kUtLayPad] = {kUtCornerTR, 8, 8, 320, 44};
        UtLayout L = utLayoutResolve(in);
        check(L.screen[kUtLayPad] && L.user[kUtLayPad] && L.why[kUtLayPad] == kUtLayoutOk &&
                  L.screenWhy[kUtLayPad] == kUtLayoutOk &&
                  utRectEq(L.rect[kUtLayPad], UtRect{1582, -192, 320, 44}) &&
                  L.screenPx[kUtLayPad].x == 1592.0f && L.screenPx[kUtLayPad].y == 8.0f,
              "pad_screen=tr,8,8,320,44 is in force: local 1582,-192 (far off the plate - no "
              "'outside'), drawn at 1592,8");
        check(L.pad.ok && L.pad.cols == 9 && L.pad.rows == 3 && L.pad.cellW == 34 && L.pad.caption == 10,
              "utPadFit unchanged on it: today's 9 x 3 cells of 34 x 14, caption 10");
        check(!L.user[kUtLayGrid] && !L.user[kUtLayLabel] && !L.screen[kUtLayLabel] &&
                  L.screenWhy[kUtLayLabel] == kUtLayoutUnset && L.screenWhy[kUtLayGrid] == kUtLayoutUnset &&
                  utRectEq(L.rect[kUtLayLabel], utLabelDefault()) && L.label.ok,
              "the grid and the label stay where they were");
        const UtRect padUser = {102, 25, 320, 50};   // clear of the derived grid and label
        in.user[kUtLayPad] = &padUser;
        L = utLayoutResolve(in);
        check(L.screen[kUtLayPad] && utRectEq(L.rect[kUtLayPad], UtRect{1582, -192, 320, 44}),
              "with pad_438x627 set too, the screen pad wins");

        // The grid refusal: the pad over the window's grid falls back to the in-window rectangle.
        in.screenRect[kUtLayPad] = {kUtCornerTL, 150, 300, 320, 44};
        L = utLayoutResolve(in);
        check(!L.screen[kUtLayPad] && L.screenWhy[kUtLayPad] == kUtLayoutScreenOverGrid &&
                  L.screenAgainst[kUtLayPad] == kUtLayGrid && L.user[kUtLayPad] &&
                  utRectEq(L.rect[kUtLayPad], padUser) && L.why[kUtLayPad] == kUtLayoutOk,
              "tl,150,300 meets the grid: refused (over the grid), the user's pad_438x627 in force");
        printf("    the WARN would say: pad_screen=tl,150,300,320,44 %s\n",
               utLayoutWhyText(L.screenWhy[kUtLayPad]));
        in.user[kUtLayPad] = nullptr;
        L = utLayoutResolve(in);
        check(!L.screen[kUtLayPad] && !L.user[kUtLayPad] && L.why[kUtLayPad] == kUtLayoutUnset &&
                  utRectEq(L.rect[kUtLayPad], in.def[kUtLayPad]) && L.pad.ok && L.pad.cols == 9,
              "without it, the derived pad: the fall-back is what the page has without the key");
        // Over the window's top band (the label's place), clear of the grid: allowed, it is drawn
        // above the page and only the grid is protected.
        in.screenRect[kUtLayPad] = {kUtCornerTL, 115, 205, 320, 44};
        L = utLayoutResolve(in);
        check(L.screen[kUtLayPad] && utRectEq(L.rect[kUtLayLabel], utLabelDefault()) &&
                  !L.user[kUtLayLabel] && L.why[kUtLayLabel] == kUtLayoutUnset,
              "over the window's label band but clear of the grid: in force, the label untouched");
        // Touching edges do not meet: client y 232..276 against the grid's 276.
        in.screenRect[kUtLayPad] = {kUtCornerTL, 115, 232, 320, 44};
        L = utLayoutResolve(in);
        check(L.screen[kUtLayPad], "a pad whose bottom edge touches the grid's top is in force");
        in.screenRect[kUtLayPad] = {kUtCornerTL, 115, 233, 320, 44};
        L = utLayoutResolve(in);
        check(!L.screen[kUtLayPad] && L.screenWhy[kUtLayPad] == kUtLayoutScreenOverGrid,
              "one px lower it meets the grid: refused");

        // The fit refusals, large, no view.
        in.screenRect[kUtLayPad] = {kUtCornerTR, 8, 8, 60, 44};
        L = utLayoutResolve(in);
        check(L.screenWhy[kUtLayPad] == kUtLayoutPadNoFit && !L.screen[kUtLayPad] && L.pad.ok &&
                  utRectEq(L.rect[kUtLayPad], in.def[kUtLayPad]),
              "60 x 44 cannot hold 26 buttons: refused, today's pad drawn");
        in.screenRect[kUtLayPad] = {kUtCornerTR, 0, 0, 1920, 44};
        L = utLayoutResolve(in);
        check(L.screenWhy[kUtLayPad] == kUtLayoutScreenLarge && !L.screen[kUtLayPad],
              "as wide as the screen: refused");
        in.screenRect[kUtLayPad] = {kUtCornerTR, 8, 8, 320, 44};
        in.screenKnown = 0;
        L = utLayoutResolve(in);
        check(L.screenWhy[kUtLayPad] == kUtLayoutScreenNoView && !L.screen[kUtLayPad] &&
                  utRectEq(L.rect[kUtLayPad], in.def[kUtLayPad]),
              "before the window's place is known: the in-window pad, said as 'cannot be placed yet'");
        in.screenKnown = 1;
        in.scale = 0.1f;
        L = utLayoutResolve(in);
        check(!L.screen[kUtLayPad] && L.screenWhy[kUtLayPad] == kUtLayoutScreenNoView,
              "an unusable UI scale places nothing");
        in.scale = 1.0f;

        // The label's own key.
        in = base;
        in.screenSet[kUtLayLabel] = 1;
        in.screenRect[kUtLayLabel] = {kUtCornerTL, 12, 6, 317, 16};
        L = utLayoutResolve(in);
        check(L.screen[kUtLayLabel] && !L.screen[kUtLayPad] && L.label.ok && L.label.size == 13 &&
                  utRectEq(L.rect[kUtLayLabel], UtRect{2, -194, 317, 16}) &&
                  utRectEq(L.rect[kUtLayPad], in.def[kUtLayPad]),
              "label_screen=tl,12,6,317,16 alone: the label at 12,6 (local 2,-194), size 13; the pad "
              "stays in the window");
        in.screenRect[kUtLayLabel] = {kUtCornerTL, 12, 6, 317, 8};
        L = utLayoutResolve(in);
        check(L.screenWhy[kUtLayLabel] == kUtLayoutLabelSmall && !L.screen[kUtLayLabel] &&
                  utRectEq(L.rect[kUtLayLabel], utLabelDefault()),
              "an 8 px label band: too small, the label back in the window");
        // Both pinned and meeting: the label (the later part) yields to the pad.
        in.screenSet[kUtLayPad] = 1;
        in.screenRect[kUtLayPad] = {kUtCornerTR, 8, 8, 320, 44};
        in.screenRect[kUtLayLabel] = {kUtCornerTR, 8, 30, 317, 16};
        L = utLayoutResolve(in);
        check(L.screen[kUtLayPad] && !L.screen[kUtLayLabel] && L.screenWhy[kUtLayLabel] == kUtLayoutOverlap &&
                  L.screenAgainst[kUtLayLabel] == kUtLayPad && utRectEq(L.rect[kUtLayLabel], utLabelDefault()),
              "a screen label over the screen pad yields to it (back in the window)");
        in.screenRect[kUtLayLabel] = {kUtCornerTR, 8, 52, 317, 16};
        L = utLayoutResolve(in);
        check(L.screen[kUtLayPad] && L.screen[kUtLayLabel], "right under it: both pinned");

        // The grid is never a screen part.
        in = base;
        in.screenSet[kUtLayGrid] = 1;
        in.screenRect[kUtLayGrid] = {kUtCornerTL, 0, 0, 400, 400};
        L = utLayoutResolve(in);
        check(!L.screen[kUtLayGrid] && L.screenWhy[kUtLayGrid] == kUtLayoutUnset &&
                  utRectEq(L.rect[kUtLayGrid], in.def[kUtLayGrid]),
              "a screen rectangle for the grid is never read");

        // Without the pinned pad in the window, a user grid over the derived pad's place stands;
        // when the window moves under the screen pad, the pad falls back and the chain runs.
        in = base;
        const UtRect grid2 = {105, 40, 333, 500};
        in.user[kUtLayGrid] = &grid2;
        L = utLayoutResolve(in);
        check(!L.user[kUtLayGrid] && L.why[kUtLayGrid] == kUtLayoutOverlap,
              "no screen key: a grid over the derived pad falls back (as in 6)");
        in.screenSet[kUtLayPad] = 1;
        in.screenRect[kUtLayPad] = {kUtCornerTR, 8, 8, 320, 44};
        L = utLayoutResolve(in);
        check(L.user[kUtLayGrid] && L.why[kUtLayGrid] == kUtLayoutOk && L.screen[kUtLayPad],
              "with the pad pinned to the screen, the same grid is in force");
        in.originX = 1500.0f;   // the window moved right: its grid now runs under the corner
        in.originY = 0.0f;
        L = utLayoutResolve(in);
        check(!L.screen[kUtLayPad] && L.screenWhy[kUtLayPad] == kUtLayoutScreenOverGrid &&
                  !L.user[kUtLayGrid] && L.why[kUtLayGrid] == kUtLayoutOverlap &&
                  L.against[kUtLayGrid] == kUtLayPad && !L.user[kUtLayPad] && L.pad.ok,
              "the window moved under it: the pad falls back, the grid then meets the derived pad and "
              "falls back too - all derived, legal by construction");

        // 10e. the materials page keeps a screen pad whole at its corner.
        in = base;
        in.screenSet[kUtLayPad] = 1;
        in.screenRect[kUtLayPad] = {kUtCornerTR, 8, 8, 320, 44};
        L = utLayoutResolve(in);
        const UtRect padmat = {102, 25, 320, 44};
        const UtRectF box[2] = {utScaledRectF(UtRect{1600 - 10, 10 - 200, 32, 32}, 1.0f),
                                utScaledRectF(UtRect{120, 90, 32, 32}, 1.0f)};
        UtMatPad m = utMatPadResolve(in, L, &padmat, box, 2, 1.0f);
        check(m.mode == kUtMatPadFull && m.src == kUtMatSrcScreen && m.user &&
                  utRectEq(m.rect, L.rect[kUtLayPad]) && m.padmatWhy == kUtMatWhyScreen && m.under == 1,
              "the whole pad stays at its corner; padmat_ is not used; a box under it is counted");
        char text[320];
        utMatPadText(m, "438x627", 2, &padmat, text, sizeof(text));
        printf("    %s\n", text);
        check(strstr(text, "(pad_screen) covers 1 of the page's 2") != nullptr &&
                  strstr(text, "padmat_438x627=102,25,320,44 is not used: the pad is pinned to the screen") !=
                      nullptr,
              "the log's text names pad_screen and why padmat_ is not used");
        m = utMatPadResolve(in, L, nullptr, box + 1, 1, 1.0f);
        check(m.mode == kUtMatPadFull && m.src == kUtMatSrcScreen && m.padmatWhy == kUtMatWhyUnset &&
                  m.under == 0,
              "without padmat_: the same pad, the page's boxes clear (never 'not inside the page')");

        // 10f. the source path (what the live page runs).
        UtLayoutSource src = {};
        src.x0 = 105;
        src.y0 = 76;
        src.usableW = 333;
        src.usableH = 552;
        src.plateW = 438;
        src.plateH = 627;
        src.maxCellW = 64;
        src.maxCellH = 128;
        src.rowGap = kUtRowGap;
        src.padY = 25;
        src.padH = 14;
        src.padGap = 1;
        src.labelSize = 13;
        src.captionMax = 14;
        src.buttons = 26;
        src.scale = 1.0f;
        src.widest = utWidestCaptionPx;
        src.widestCtx = &kCaps;
        src.padScreen = "tr,8,8,320,44";
        src.labelScreen = "garbage";
        src.screenKnown = 1;
        src.screenW = 1920;
        src.screenH = 1080;
        src.originX = 10.0f;
        src.originY = 200.0f;
        UtLayoutInput sin;
        UtRect store[kUtLayParts];
        L = utLayoutFromSource(src, &sin, store);
        check(L.screen[kUtLayPad] && sin.screenSet[kUtLayPad] && !sin.screenSet[kUtLayLabel] &&
                  L.screenWhy[kUtLayLabel] == kUtLayoutUnset && !sin.screenSet[kUtLayGrid],
              "utLayoutFromSource: pad_screen parsed into the input by value, a value that does not "
              "parse is no key");
        UtLayoutInput copy = sin;
        check(utRectEq(utLayoutResolve(copy).rect[kUtLayPad], L.rect[kUtLayPad]),
              "a copy of the input resolves to the same (nothing points into the caller)");
        int user = 1;
        const UtRect gr = utLayoutGridRect(src, &user);
        check(user == 0 && utRectEq(gr, UtRect{105, 76, 333, 552}),
              "utLayoutGridRect (the generator) never depends on a screen key");
        src.padScreen = "";
        L = utLayoutFromSource(src, &sin, store);
        check(!L.screen[kUtLayPad] && L.screenWhy[kUtLayPad] == kUtLayoutUnset &&
                  utRectEq(L.rect[kUtLayPad], UtRect{102, 25, 320, 44}),
              "an empty pad_screen is no key: today's pad, exactly");

        // 10f2. THE PER-COVER LINE OF A PINNED PART. A recorded first world:
        //      grid_438x627=101,432,320,196, pad_438x627=42,8,371,319 in the window, the label
        //      pinned (label_screen=tl,330,168,333,23) with a stale label_438x627=103,4,333,23,
        //      the page not drawn yet. The log said "pad ... is refused - it overlaps another part
        //      of the page (the label)" and "label ... is refused - overlaps (the pad)".
        {
            UtLayoutInput w1 = vanillaInput();
            w1.scale = 0.8777f;
            const UtRect g1 = {101, 432, 320, 196}, p1 = {42, 8, 371, 319}, l1 = {103, 4, 333, 23};
            w1.user[kUtLayGrid] = &g1;
            w1.user[kUtLayPad] = &p1;
            w1.user[kUtLayLabel] = &l1;
            w1.screenSet[kUtLayLabel] = 1;
            w1.screenRect[kUtLayLabel] = {kUtCornerTL, 330, 168, 333, 23};
            w1.screenKnown = 0;   // "cannot be placed yet ... until the page has been drawn once"
            L = utLayoutResolve(w1);
            check(L.user[kUtLayGrid] && L.why[kUtLayGrid] == kUtLayoutOk && L.user[kUtLayPad] &&
                      L.why[kUtLayPad] == kUtLayoutOk && utRectEq(L.rect[kUtLayPad], p1),
                  "world 1 before the first draw: the grid and the pad in force - the pad is not "
                  "refused for meeting the label waiting for its screen corner");
            check(L.screenWhy[kUtLayLabel] == kUtLayoutScreenNoView && !L.screen[kUtLayLabel] &&
                      L.ignored[kUtLayLabel] && !L.ignored[kUtLayPad] && !L.user[kUtLayLabel] &&
                      L.why[kUtLayLabel] == kUtLayoutUnset &&
                      utRectEq(L.rect[kUtLayLabel], utLabelDefault()),
                  "the stale label_438x627 is ignored (not refused): the label waits at its "
                  "derived place, said once at DEBUG");
            // the first draw publishes the screen: the label goes to its corner
            w1.screenKnown = 1;
            w1.screenW = 1920;
            w1.screenH = 1080;
            w1.originX = 0.0f;
            w1.originY = 77.2f;
            L = utLayoutResolve(w1);
            check(L.screen[kUtLayLabel] && L.ignored[kUtLayLabel] &&
                      L.why[kUtLayLabel] == kUtLayoutOk && L.user[kUtLayPad] &&
                      utRectEq(L.rect[kUtLayPad], p1) && L.user[kUtLayGrid],
                  "once placed: the label at its corner, its per-cover line still ignored, the "
                  "pad and the grid in force");
            // a screen key refused for a reason of its own puts the part back in the window,
            // where its per-cover line is judged again (today's rule; its WARN is legitimate)
            w1.screenRect[kUtLayLabel] = {kUtCornerTL, 330, 168, 333, 6};
            L = utLayoutResolve(w1);
            check(L.screenWhy[kUtLayLabel] == kUtLayoutLabelSmall && !L.ignored[kUtLayLabel] &&
                      (L.why[kUtLayPad] == kUtLayoutOverlap ||
                       L.why[kUtLayLabel] == kUtLayoutOverlap),
                  "label_screen refused (too small): the label is an in-window part again and its "
                  "per-cover line meets the pad - judged as before");
            // the pinned pad's per-cover line too (the recorded ini: both parts pinned)
            UtLayoutInput w5 = w1;
            w5.screenRect[kUtLayLabel] = {kUtCornerTL, 330, 168, 333, 23};
            w5.screenSet[kUtLayPad] = 1;
            w5.screenRect[kUtLayPad] = {kUtCornerTL, 120, 37, 735, 79};
            const UtRect pOff = {500, 700, 10, 10};   // outside the plate: would be refused
            w5.user[kUtLayPad] = &pOff;
            L = utLayoutResolve(w5);
            check(L.screen[kUtLayPad] && L.screen[kUtLayLabel] && L.ignored[kUtLayPad] &&
                      L.ignored[kUtLayLabel] && L.why[kUtLayPad] == kUtLayoutOk &&
                      L.user[kUtLayGrid],
                  "both pinned: a per-cover pad line off the plate is not judged (no 'not inside "
                  "the page')");
            w5.screenKnown = 0;
            L = utLayoutResolve(w5);
            check(!L.screen[kUtLayPad] && L.ignored[kUtLayPad] &&
                      L.why[kUtLayPad] == kUtLayoutUnset && L.why[kUtLayLabel] == kUtLayoutUnset &&
                      L.user[kUtLayGrid] && L.why[kUtLayGrid] == kUtLayoutOk,
                  "both waiting for the first draw: neither line is judged, the grid in force");
            // no screen key: the same lines are judged exactly as before (the log's WARNs)
            UtLayoutInput w0 = w1;
            w0.screenSet[kUtLayLabel] = 0;
            L = utLayoutResolve(w0);
            check(!L.ignored[kUtLayLabel] && !L.ignored[kUtLayPad] &&
                      L.why[kUtLayLabel] == kUtLayoutOverlap,
                  "without label_screen the label line is an in-window part and meets the pad");
            // the source path: the ignored line leaves the input, its value stays in the store
            UtLayoutSource s1 = {};
            s1.x0 = 105;
            s1.y0 = 76;
            s1.usableW = 333;
            s1.usableH = 552;
            s1.plateW = 438;
            s1.plateH = 627;
            s1.maxCellW = 64;
            s1.maxCellH = 128;
            s1.rowGap = kUtRowGap;
            s1.padY = 25;
            s1.padH = 14;
            s1.padGap = 1;
            s1.labelSize = 13;
            s1.captionMax = 14;
            s1.buttons = 26;
            s1.scale = 0.8777f;
            s1.widest = utWidestCaptionPx;
            s1.widestCtx = &kCaps;
            s1.grid = "101,432,320,196";
            s1.pad = "42,8,371,319";
            s1.label = "103,4,333,23";
            s1.labelScreen = "tl,330,168,333,23";
            UtLayoutInput i1;
            UtRect st1[kUtLayParts];
            L = utLayoutFromSource(s1, &i1, st1);
            check(L.ignored[kUtLayLabel] && i1.user[kUtLayLabel] == nullptr &&
                      utRectEq(st1[kUtLayLabel], l1) && i1.user[kUtLayPad] &&
                      L.why[kUtLayPad] == kUtLayoutOk,
                  "utLayoutFromSource: the ignored label line leaves the input (no refusal WARN, "
                  "no red outline) and stays parsed for the DEBUG line; the pad in force");
            UtLayoutInput c1 = i1;
            const UtLayout L2 = utLayoutResolve(c1);
            check(utRectEq(L2.rect[kUtLayPad], L.rect[kUtLayPad]) &&
                      utRectEq(L2.rect[kUtLayLabel], L.rect[kUtLayLabel]) &&
                      L2.why[kUtLayPad] == kUtLayoutOk,
                  "and resolving that input again gives the same rectangles");
            char dbg[200];
            const int n = utLayoutIgnoredText(kUtLayLabel, "438x627", s1.label, s1.labelScreen,
                                              dbg, sizeof(dbg));
            printf("    the DEBUG line: layout: %s\n", dbg);
            check(n > 0 && !strcmp(dbg, "label_438x627=103,4,333,23 is ignored while the label is "
                                        "on the screen (label_screen=tl,330,168,333,23)"),
                  "the DEBUG line's text");
            char tiny[8];
            check(utLayoutIgnoredText(kUtLayPad, "438x627", "1,2,3,4", "tr,1,1,1,1", tiny,
                                      sizeof(tiny)) == 7 &&
                      strlen(tiny) == 7,
                  "the DEBUG text is cut, never overrun");
        }

        // 10g. the editor's nearest corner: back to the same key at 1.0, within a pixel at
        //      the other scales.
        bool near = true;
        for (const Sc& sc : scs) {
            for (int c = 0; c < 4; ++c) {
                const UtScreenRect q = {c, 8, 8, 320, 44};
                const UtRectF f = utScreenPlace(q, sc.W, sc.H, sc.s, nullptr);
                const UtScreenRect b = utScreenFromClient(f, sc.W, sc.H, sc.s);
                const UtRectF g = utScreenPlace(b, sc.W, sc.H, sc.s, nullptr);
                near = near && b.corner == c && fabsf(g.x - f.x) <= 1.0f && fabsf(g.y - f.y) <= 1.0f &&
                       fabsf(g.w - f.w) <= 1.0f && fabsf(g.h - f.h) <= 1.0f;
                if (sc.s == 1.0f) near = near && utScreenRectEq(b, q);
            }
        }
        const UtScreenRect mid = utScreenFromClient({1500.0f, 900.0f, 320.0f, 44.0f}, 1920, 1080, 1.0f);
        check(near && mid.corner == kUtCornerBR && mid.x == 100 && mid.y == 136,
              "utScreenFromClient: the nearest corner, the key placed again lands where it was");
    }

    // -------------------------------------------------------------------------------------------
    printf("11. the screen draw: the frame, what the input reads, the draw site\n");
    {
        // 11a. the frame: ground + four edges tile the outer rectangle exactly once, at whole and
        //      fractional grounds and 1 / 2 px lines; the ground keeps its own rounded pixels.
        struct G {
            UtRectF g;
            int line;
        };
        const G gs[] = {{{1592.0f, 8.0f, 320.0f, 44.0f}, 1},
                        {{2256.4f, 7.4f, 297.2f, 41.1f}, 1},
                        {{3210.0f, 6.0f, 224.0f, 31.0f}, 1},
                        {{10.6f, 900.5f, 640.0f, 88.0f}, 2},
                        {{0.0f, 0.0f, 3.0f, 2.0f}, 1}};
        bool tiled = true, kept = true;
        for (const G& t : gs) {
            const UtScreenFrame f = utScreenFrame(t.g, t.line);
            kept = kept && f.x == utScreenRound(t.g.x) && f.y == utScreenRound(t.g.y) &&
                   f.x + f.w == utScreenRound(t.g.x + t.g.w) &&
                   f.y + f.h == utScreenRound(t.g.y + t.g.h) && f.ox == f.x - t.line &&
                   f.oy == f.y - t.line && f.ow == f.w + 2 * t.line && f.oh == f.h + 2 * t.line;
            std::vector<int> cover((size_t)f.ow * (size_t)f.oh, 0);
            auto paint = [&](int x, int y, int w, int h) {
                for (int yy = y; yy < y + h; ++yy)
                    for (int xx = x; xx < x + w; ++xx) {
                        if (xx < f.ox || yy < f.oy || xx >= f.ox + f.ow || yy >= f.oy + f.oh) {
                            tiled = false;
                            continue;
                        }
                        ++cover[(size_t)(yy - f.oy) * (size_t)f.ow + (size_t)(xx - f.ox)];
                    }
            };
            paint(f.x, f.y, f.w, f.h);
            paint(f.top[0], f.top[1], f.top[2], f.top[3]);
            paint(f.left[0], f.left[1], f.left[2], f.left[3]);
            paint(f.bottom[0], f.bottom[1], f.bottom[2], f.bottom[3]);
            paint(f.right[0], f.right[1], f.right[2], f.right[3]);
            for (int c : cover) tiled = tiled && c == 1;
        }
        check(kept, "utScreenFrame: the ground keeps its rounded edges, the frame is outside");
        check(tiled, "  the ground and the four edges cover the outer rectangle exactly once");
        const UtScreenFrame f0 = utScreenFrame({1592.0f, 8.0f, 320.0f, 44.0f}, 1);
        check(f0.top[0] == 1591 && f0.top[1] == 7 && f0.top[2] == 322 && f0.bottom[1] == 52 &&
                  f0.right[0] == 1912 && f0.right[1] == 8 && f0.right[3] == 44 && f0.left[3] == 44,
              "  tr,8,8,320,44 at 1920x1080 / 1.0: the edges by hand");
        const UtScreenFrame fz = utScreenFrame({5.0f, 5.0f, 10.0f, 10.0f}, 0);
        check(fz.line == 1 && fz.ow == 12, "  a 0 px line is one pixel (never a missing frame)");

        // 11b. what the screen pad's input reads (ut_padinput.h): the pad and the label as their
        //      whole frames, every button rounded the way the draw rounds it, its logical index.
        static UtPadScreenRects pub;
        memset(&pub, 0x5A, sizeof(pub));
        utScreenPubReset(&pub);
        check(!pub.shown && pub.buttonCount == 0 && pub.tick == 0 && !utPadScreenRectValid(pub.pad),
              "utScreenPubReset: nothing shown, nothing in it (inert)");
        const float ox = 172.0f, oy = 133.7f;   // a logged window origin at UI scale 0.928
        const UtScreenFrame pf = utScreenFrame({2256.4f, 7.4f, 297.2f, 41.1f}, 1);
        utScreenPubPad(&pub, pf);
        const bool b1 = utScreenPubButton(&pub, ox, oy, 2087.3f, -125.2f, 31.6f, 13.4f, 0);
        const bool b2 = utScreenPubButton(&pub, ox, oy, 2119.6f, -125.2f, 31.2f, 13.4f, 7);
        const bool bNeg = utScreenPubButton(&pub, ox, oy, 2119.6f, -125.2f, 31.2f, 13.4f, -1);
        const bool bEmpty = utScreenPubButton(&pub, ox, oy, 2119.6f, -125.2f, 0.2f, 13.4f, 3);
        check(pub.shown && pub.pad.x == (float)pf.ox && pub.pad.w == (float)pf.ow && b1 && b2 &&
                  !bNeg && !bEmpty && pub.buttonCount == 2,
              "utScreenPubPad / utScreenPubButton: the frame, two buttons, none empty or unmapped");
        const UtPadScreenButton& k = pub.buttons[0];
        check(k.r.x == (float)utScreenRound(ox + 2087.3f) &&
                  k.r.y == (float)utScreenRound(oy - 125.2f) &&
                  k.r.w == (float)utScreenRound(31.6f) && k.r.h == (float)utScreenRound(13.4f) &&
                  k.index == 0 && pub.buttons[1].index == 7,
              "  a button is drawn and published with the same rounding, with its logical index");
        check(utPadScreenButtonAt(pub, k.r.x + 1.0f, k.r.y + 1.0f) == 0 &&
                  utPadScreenButtonAt(pub, pub.buttons[1].r.x + 2.0f, pub.buttons[1].r.y + 2.0f) ==
                      7 &&
                  utPadScreenInAny(pub, (float)pf.ox, (float)pf.oy) &&
                  !utPadScreenInAny(pub, (float)pf.ox - 1.0f, (float)pf.oy),
              "  the reader: the buttons by point, the 1-px frame eats a click, outside does not");
        const UtScreenFrame lf = utScreenFrame({1600.0f, 60.0f, 300.0f, 20.0f}, 1);
        utScreenPubLabel(&pub, lf);
        check(utPadScreenInAny(pub, 1700.0f, 70.0f) && utPadScreenButtonAt(pub, 1700.0f, 70.0f) < 0,
              "utScreenPubLabel: the label's frame eats a click, it is no button");
        int added = 0;
        for (int i = 0; i < 200; ++i)
            added += utScreenPubButton(&pub, 0.0f, 0.0f, (float)i, 0.0f, 1.0f, 1.0f, i) ? 1 : 0;
        check(pub.buttonCount == kUtPadScreenMaxButtons && added == kUtPadScreenMaxButtons - 2,
              "  the button list stops at kUtPadScreenMaxButtons (128)");

        // 11c. the site proof over the HUD's own draw calls (synthetic code, the exe's shape).
        auto call = [](std::vector<unsigned char>& v, unsigned off) {
            const unsigned char c[] = {0x48, 0x8D, 0x8F, (unsigned char)(off & 0xFF),
                                       (unsigned char)((off >> 8) & 0xFF),
                                       (unsigned char)((off >> 16) & 0xFF),
                                       (unsigned char)(off >> 24), 0x48, 0x8B, 0x01, 0x41,
                                       0x0F, 0x28, 0xDB, 0x4C, 0x8D, 0x44, 0x24, 0x50, 0x48,
                                       0x8B, 0xD6, 0xFF, 0x50, 0x20};
            v.insert(v.end(), c, c + sizeof(c));
        };
        std::vector<unsigned char> ok, swapped, missing, twice, noCall;
        call(ok, 0x30DD8);
        call(ok, 0x4FD08);
        call(ok, 0x52258);
        call(ok, 0xB800);
        call(ok, 0x7378);
        call(swapped, 0xB800);
        call(swapped, 0x4FD08);
        call(missing, 0x4FD08);
        call(twice, 0x4FD08);
        call(twice, 0xB800);
        call(twice, 0xB800);
        const unsigned char leaOnly[] = {0x48, 0x8D, 0x8F, 0x00, 0xB8, 0x00, 0x00, 0x90, 0x90, 0xC3};
        call(noCall, 0x4FD08);
        noCall.insert(noCall.end(), leaOnly, leaOnly + sizeof(leaOnly));
        long cAt = -2, rAt = -2;
        const UtSiteProof p1 = utHudSiteProof(ok.data(), ok.size(), 0x4FD08, 0xB800, &cAt, &rAt);
        check(p1 == kUtSiteProved && cAt == 25 && rAt == 75,
              "utHudSiteProof: the rollover drawn once, after the caravan: proved");
        check(utHudSiteProof(swapped.data(), swapped.size(), 0x4FD08, 0xB800, nullptr, nullptr) ==
                      kUtSiteRolloverFirst &&
                  utHudSiteProof(missing.data(), missing.size(), 0x4FD08, 0xB800, nullptr,
                                 nullptr) == kUtSiteNoRolloverCall &&
                  utHudSiteProof(twice.data(), twice.size(), 0x4FD08, 0xB800, nullptr, nullptr) ==
                      kUtSiteNoRolloverCall &&
                  utHudSiteProof(noCall.data(), noCall.size(), 0x4FD08, 0xB800, nullptr,
                                 nullptr) == kUtSiteNoRolloverCall &&
                  utHudSiteProof(ok.data(), ok.size(), 0x4FD0C, 0xB800, nullptr, nullptr) ==
                      kUtSiteNoCaravanCall,
              "  refused: drawn first, absent, twice, a lea without the call, a wrong offset");
        check(kUtScreenSiteChosen == kUtScreenSiteRollover,
              "the site in force is the rollover head (the Draw tail is the fallback)");

        // 11d. the same proof over the 1.3.0.8 exe image (UNIQUETAB_TEST_EXE_IMAGE, a memory image:
        //      file offset = rva): the HUD Draw 0x20A860 draws the caravan at 0x20B731 and the item
        //      rollover at 0x20B925; the rollover's Draw 0x236D00 walks its compare boxes (+0x2B8).
        const char* img = getenv("UNIQUETAB_TEST_EXE_IMAGE");
        FILE* fi = nullptr;
        if (img && *img) fi = fopen(img, "rb");
        if (!fi) {
            printf("  (UNIQUETAB_TEST_EXE_IMAGE not set or unreadable - the exe half SKIPPED)\n");
        } else {
            std::vector<unsigned char> im;
            fseek(fi, 0, SEEK_END);
            const long n = ftell(fi);
            fseek(fi, 0, SEEK_SET);
            if (n > 0x400000) {
                im.resize((size_t)n);
                if (fread(im.data(), 1, (size_t)n, fi) != (size_t)n) im.clear();
            }
            fclose(fi);
            if (im.size() < 0x320000) {
                check(false, "the exe image is too small to be a 1.3.0.8 memory image");
            } else {
                const size_t hud = 0x20A860;
                long c2 = -1, r2 = -1;
                const UtSiteProof p2 = utHudSiteProof(&im[hud], 0x1C00, 0x4FD08, 0xB800, &c2, &r2);
                check(p2 == kUtSiteProved && hud + (size_t)c2 == 0x20B731 &&
                          hud + (size_t)r2 == 0x20B925,
                      "the exe: the HUD Draw draws the caravan at 0x20B731, the rollover at 0x20B925");
                // the HUD vtable 0x31A580 and the rollover's 0x31B370 hold relocated pointers, so
                // their slots are compared by difference
                unsigned long long d20 = 0, d38 = 0, r10 = 0, r20 = 0;
                memcpy(&d20, &im[0x31A580 + 0x20], 8);
                memcpy(&d38, &im[0x31A580 + 0x38], 8);
                memcpy(&r10, &im[0x31B370 + 0x10], 8);
                memcpy(&r20, &im[0x31B370 + 0x20], 8);
                check(d38 - d20 == 0x20BFD0 - 0x20A860 && r20 - r10 == 0x236D00 - 0x236B90 &&
                          r20 - d20 == 0x236D00 - 0x20A860,
                      "  the HUD's vtable +0x20 is that Draw; the rollover's +0x20 is 0x236D00");
                const unsigned char vec[] = {0x48, 0x8B, 0x83, 0xB8, 0x02, 0x00, 0x00};
                bool walks = false;
                for (size_t i = 0x236D00; i + sizeof(vec) <= 0x236EA9; ++i)
                    walks = walks || memcmp(&im[i], vec, sizeof(vec)) == 0;
                check(walks && im[0x236D0A] == 0x48 && im[0x236D0B] == 0x83 &&
                          im[0x236D0C] == 0x79 && im[0x236D0D] == 0x38,
                      "  the rollover's Draw: shown only with a target (+0x38), walks its boxes");
            }
        }
    }

    // ---- 12. the layout plates see the screen keys ---------------------------------------------
    printf("12. the layout plates see the screen keys; the plate set under the grid\n");
    // A recorded SAVE in DeathGrimDawn: grid_438x627=101,33,320,594 and
    // pad_438x627=10,4,413,90 in the view's file, pad_screen=tr,357,121,998,91 in the main file (the
    // screen keys' home before ini_version 42). The
    // live page took the grid ("grid 101,33,320,594 (user), pad 107,-193,998,91 (user)" at UI scale
    // 0.878) but the layout-plates thread refused it - it never passed the screen keys, so the pad's
    // in-window line met the grid - and the derived lattice (from y=76) stayed under the items.
    {
        UtLayoutSource dgd = {};   // what layWantNow builds: the P line, the keys, no scale
        dgd.x0 = 105;
        dgd.y0 = 76;
        dgd.usableW = 333;
        dgd.usableH = 552;
        dgd.plateW = 438;
        dgd.plateH = 627;
        dgd.maxCellW = 64;
        dgd.maxCellH = 128;
        dgd.rowGap = kUtRowGap;
        dgd.grid = "101,33,320,594";
        dgd.pad = "10,4,413,90";
        dgd.label = nullptr;
        dgd.padY = 25;
        dgd.padH = 14;
        dgd.padGap = 1;
        dgd.labelSize = 13;
        dgd.captionMax = 14;
        const char* const padScr = "tr,357,121,998,91";
        const UtRect userGrid = {101, 33, 320, 594}, derivedGrid = {105, 76, 333, 552};

        // 12a. the thread, with pad_screen: the user grid, and the set it paints
        UtLayoutSource th = dgd;
        th.padScreen = padScr;
        th.labelScreen = "";
        UtRect gr = {};
        char canon[64] = "x";
        int tUser = utLayoutPlatesGrid(th, &gr, canon, sizeof(canon));
        check(tUser == 1 && utRectEq(gr, userGrid) && !strcmp(canon, "101,33,320,594"),
              "the SAVE replayed: the layout plates take grid 101,33,320,594 with the pad on the screen");
        {
            char setText[96];
            snprintf(setText, sizeof(setText), "grid_%dx%d=%s", th.plateW, th.plateH, canon);
            check(!strcmp(setText, "grid_438x627=101,33,320,594"),
                  "  the repaint is requested for grid_438x627=101,33,320,594 (the set's text)");
        }
        // a screen known to the caller changes nothing: the thread never places the key
        th.screenKnown = 1;
        th.screenW = 2560;
        th.screenH = 1440;
        th.originX = 600.0f;
        th.originY = 40.0f;
        check(utLayoutPlatesGrid(th, &gr, canon, sizeof(canon)) == 1 && utRectEq(gr, userGrid),
              "  the thread's answer never depends on a screen it does not see");
        // the derived pad (no pad_ line) pinned the same way
        th = dgd;
        th.pad = nullptr;
        th.padScreen = padScr;
        check(utLayoutPlatesGrid(th, &gr, canon, sizeof(canon)) == 1 && utRectEq(gr, userGrid),
              "  without the view's pad_ line (the derived pad 102,25,320,44): the user grid too");
        // the label pinned by its own key (the derived label 105,8,317,16 does not meet y=33 anyway)
        th.labelScreen = "tl,330,168,333,23";
        check(utLayoutPlatesGrid(th, &gr, canon, sizeof(canon)) == 1 && utRectEq(gr, userGrid),
              "  label_screen set as well: the user grid");

        // 12b. the thread WITHOUT pad_screen: refused, as the live page refuses it (the 11:19 WARN)
        th = dgd;
        th.padScreen = nullptr;
        th.labelScreen = nullptr;
        canon[0] = 'x';
        const int tNoKey = utLayoutPlatesGrid(th, &gr, canon, sizeof(canon));
        check(tNoKey == 0 && utRectEq(gr, derivedGrid) && canon[0] == 0,
              "no pad_screen: the layout plates refuse the grid (the pad in the window meets it)");
        th.padScreen = "";
        th.pad = nullptr;
        check(utLayoutPlatesGrid(th, &gr, canon, sizeof(canon)) == 0 && utRectEq(gr, derivedGrid),
              "  an empty pad_screen is no key; the derived pad 102,25,320,44 meets it as well");

        // 12c. the live page at 0.878 on a 2560x1440 screen, the window where the logged line
        //      puts the pad (107,-193 local: originX = 2560 - (357 + 998 + 107) * s, originY =
        //      (121 + 193) * s)
        const float s = 0.878f;
        UtLayoutSource live = dgd;
        live.scale = s;
        live.buttons = 26;
        live.widest = utWidestCaptionPx;
        live.widestCtx = &kCaps;
        live.padScreen = padScr;
        live.labelScreen = "";
        live.screenKnown = 1;
        live.screenW = 2560;
        live.screenH = 1440;
        live.originX = 2560.0f - (357 + 998 + 107) * s;
        live.originY = (121 + 193) * s;
        UtLayoutInput lin;
        UtRect lstore[kUtLayParts];
        UtLayout LL = utLayoutFromSource(live, &lin, lstore);
        check(LL.user[kUtLayGrid] && utRectEq(LL.rect[kUtLayGrid], userGrid) && LL.screen[kUtLayPad] &&
                  LL.user[kUtLayPad] && abs(LL.rect[kUtLayPad].x - 107) <= 1 &&
                  abs(LL.rect[kUtLayPad].y + 193) <= 1 && LL.rect[kUtLayPad].w == 998 &&
                  LL.rect[kUtLayPad].h == 91 && LL.ignored[kUtLayPad],
              "the live page as logged: grid 101,33,320,594 (user), pad 107,-193,998,91 (user)");
        const int liveWithKey = LL.user[kUtLayGrid] ? 1 : 0;
        check(liveWithKey == tUser, "  both resolvers agree: the user grid (the plates follow the items)");
        live.screenKnown = 0;   // before the page's first draw
        LL = utLayoutFromSource(live, &lin, lstore);
        check(LL.user[kUtLayGrid] && utRectEq(LL.rect[kUtLayGrid], userGrid) &&
                  LL.screenWhy[kUtLayPad] == kUtLayoutScreenNoView,
              "  before the first draw the live page agrees as well (the pad waits to be placed)");
        // without the key the live page refuses the grid for the pad - the logged WARN
        live.screenKnown = 1;
        live.padScreen = "";
        LL = utLayoutFromSource(live, &lin, lstore);
        check(!LL.user[kUtLayGrid] && LL.why[kUtLayGrid] == kUtLayoutOverlap &&
                  LL.against[kUtLayGrid] == kUtLayPad && utRectEq(LL.rect[kUtLayGrid], derivedGrid) &&
                  !LL.user[kUtLayPad] && lin.user[kUtLayPad] != nullptr,
              "no pad_screen on the live page: grid_438x627 refused (the pad), pad_438x627 refused");
        const int liveNoKey = LL.user[kUtLayGrid] ? 1 : 0;
        check(liveNoKey == tNoKey, "  both resolvers agree: the derived grid");
        // a screen key the live page refuses for a reason of its own (over the grid): the pad is
        // back in the window, the grid refused there - the thread (no screen) still paints the set
        live.padScreen = "tl,1400,400,300,60";
        LL = utLayoutFromSource(live, &lin, lstore);
        th = dgd;
        th.padScreen = "tl,1400,400,300,60";
        const int tOver = utLayoutPlatesGrid(th, &gr, canon, sizeof(canon));
        const int liveOver = LL.user[kUtLayGrid] ? 1 : 0;
        check(LL.screenWhy[kUtLayPad] == kUtLayoutScreenOverGrid && liveOver == 0 && tOver == 1,
              "a screen pad over the grid: the live page has the derived grid, the thread the user's");

        // 12d. the plate set the page shows (ut_plate.cpp plateNames): the layout plates only under
        //      the live page's user grid, the derived plates only under its derived grid
        check(utPlateSetChoice(tUser, 1, liveWithKey) == kUtPlateSetLayout,
              "plate set, the fixed SAVE: the user grid's plates once painted");
        check(utPlateSetChoice(tUser, 0, liveWithKey) == kUtPlateSetPlain,
              "  while they are painted: the plain cover (as for any moved grid)");
        check(utPlateSetChoice(0, 1, liveWithKey) == kUtPlateSetPlain,
              "  the thread without a set under the live user grid (the bug): the plain cover, never "
              "the derived lattice");
        check(utPlateSetChoice(tNoKey, 1, liveNoKey) == kUtPlateSetDerived,
              "  no pad_screen: the derived plates under the derived grid");
        check(utPlateSetChoice(tOver, 1, liveOver) == kUtPlateSetDerived &&
                  utPlateSetChoice(tOver, 0, liveOver) == kUtPlateSetDerived,
              "  the screen pad over the grid: the derived plates, never the _L set under the "
              "derived grid");
        check(utPlateSetChoice(1, 1, -1) == kUtPlateSetLayout &&
                  utPlateSetChoice(1, 0, -1) == kUtPlateSetPlain &&
                  utPlateSetChoice(0, 1, -1) == kUtPlateSetDerived,
              "  the live layout not resolved yet (no UI scale): the thread's answer, as before");
        check(!strcmp(utPlateSetName(kUtPlateSetLayout), "the layout plates") &&
                  !strcmp(utPlateSetName(kUtPlateSetPlain), "the plain cover") &&
                  !strcmp(utPlateSetName(kUtPlateSetDerived), "the derived plates") &&
                  !strcmp(utPlateSetName(7), "?"),
              "  the sets' names");
    }

    printf("\n%s (%d failure(s))\n", g_fails ? "FAILED" : "PASSED", g_fails);
    return g_fails ? 1 : 0;
}
