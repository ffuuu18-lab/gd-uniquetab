// ut_panel.h - the collection view: the catalogue and the ownership state fed by the
// InventorySack detours, the group label and the category pad (the 26-button strip over the
// page). Nothing in here creates, moves, deletes or saves anything: the sack detours call the
// original first and then only READ the item.
//
// Threads:
//   * game thread   - panelOnItemAdded / panelOnItemRemoved / panelOnTransferOpen
//   * render thread - panelDraw
// The Collection is guarded by one mutex.
#pragma once

#include <windows.h>

#include "gd_runtime.h"
#include "ut_layout.h"
#include "ut_padinput.h"   // UtPadScreenRects: what the screen draw publishes

namespace ut {

// Loads catalogue.bin (the mod folder, else beside the .asi) and logs the counts.
// Called once from the worker thread after the exports resolve. Safe to call twice.
bool panelInit(HMODULE selfModule);

// The view changed: the active view's catalogue.bin replaces the one in use (utViewFile), under
// the model lock, and the ownership model is recounted over the live items. A file that cannot be
// read keeps the previous catalogue (one WARN). GAME THREAD (applyViewChange).
bool panelReloadCatalogue(HMODULE selfModule);

// 1 = the active catalogue has `record`, 0 = it does not, -1 = no catalogue (unknown). Any thread.
int panelCatalogueHas(const char* record);

// How many owned journal rows no page of the active view shows - the catalogue lacks the record,
// or has it and no group takes it (the label's "+N not on these pages", utNotShownSuffix); 0 =
// none. Set by the reconcile.
void panelSetNotInDatabase(int n);

// ---- game thread -----------------------------------------------------------------------
// Both InventorySack::AddItem overloads land here after the original has run.
void panelOnItemAdded(GdItem* item);
void panelOnItemRemoved(unsigned int itemId);
// GameEngine::SetTransferOpen(open): logs the ownership summary when the stash opens.
void panelOnTransferOpen(GdGameEngine* gameEngine, bool open);

// ---- render thread ---------------------------------------------------------------------
// Draws the collection group label. Does nothing unless the Materials page is on screen, so it
// can never appear at the main menu.
void panelDraw(GdCanvas* canvas, GdGameEngine* gameEngine);

// Draw ONLY the collection group label, with an explicit window origin in screen pixels.
// ut_plate.cpp's `ReagentWindow::Draw` detour calls this at the tail of the engine's own Draw
// with the very origin that Draw computed (the caller's Vec2 + window+0x40/+0x44), so the label
// lands exactly where the PresentSurface draw put it - but under the tooltip layer instead of
// over it.  Returns true when it really drew (a collection group is on screen and the fonts
// resolved); the whole body is SEH-wrapped, and a fault is caught here rather than propagating
// into the engine's frame.
bool panelDrawGroupLabel(GdCanvas* canvas, float originX, float originY);

// The pad and the label PINNED TO THE SCREEN (pad_screen / label_screen in force), drawn
// from the screen site (ut_plate.cpp: the head of the HUD's item-rollover Draw - one level below
// the tooltips - or, as the documented fallback, the ReagentWindow::Draw tail) with the origin the
// layout was resolved for. Only while the caravan is open on the materials window; the window
// routes then draw neither part. Fills `pub` with what was painted (client px) for the screen
// pad's input; shown = 0 when nothing was. Render thread (the thread of the Draw tail).
bool panelDrawScreenParts(GdCanvas* canvas, UtPadScreenRects* pub);
// true once the screen draw faulted (latched): ut_plate then turns the site off, which puts both
// parts back into the window
bool panelScreenDrawFaulted();
// the once-per-world "on the screen" lines speak again (ut_plate's world teardown)
void panelScreenWorldReset();

// True once after the route-1 label body faulted (one-shot; reading it clears it).
// panelDrawGroupLabel's own __except is INSIDE ut_plate's drawLabelGuarded and consumes the
// exception first, so the Draw detour has to ask rather than catch - otherwise drawOff() never
// runs, route 2 never takes over, and the fault repeats on every frame.
bool panelLabelDrawFaulted();

// ---- the 26-button category PAD -----------------------------------------------------------------
// Drawn from the same tail of ReagentWindow::Draw as the label, but with its own gates: it does
// NOT inherit plate_label and it does NOT go through liveViewInfo() (which returns false for
// group -1 - that would kill the pad on the vanilla page).  Geometry, hit test, the button table
// and the click semantics all live here, so the draw and the click can never disagree.  Index 0
// is the vanilla crafting-materials page (group -1), then groups 0..23 in uniq-groups.txt order,
// then the OWN owned-only filter toggle.
//
// The layout is a 9-column x 3-row grid of 34 x pad_h record px cells with a pad_gap gutter and
// a 3 px side margin - 320 x 44 record px at x 102..421, y 25..68 with the shipped pad_y=25 /
// pad_h=14 / pad_gap=1, inside the joint free rectangle x 102..422, y 25..69 (the band that is
// flat and opaque on the game's own materials plate AND on all seven of the mod's plates).  27
// cells hold 26 buttons because OWN spans the last two of row 3.  The pad is drawn ENTIRELY in
// solid quads (fillRect/outlineRect) plus the game's savapromedium caption: it loads no engine
// texture at all.  A click is ABSOLUTE - one button, one group (and the last one toggles the
// filter instead).

// The BUILT-IN pad: the vanilla page, the 24 built-in groups and OWN - the captions in force until
// a group table loads. The pad itself is keyed by the loaded table (ut_padpage.h: MAT, one button
// per G line, OWN, and NEXT while paged); nothing is sized by this.
enum { kUtButtonCount = 26 };

// What panelButtonTarget returns for the OWN button: it selects no group at all, it toggles the
// owned-only filter (ut_live.cpp's liveToggleOwnedOnly).  Distinct from -1 (the vanilla page) and
// from -2 (an index out of range).
enum { kUtButtonFilterTarget = -3 };

// The captions in force in button order (MAT, the group table's tags, OWN; the 26 built-ins until
// a table loads), for the layout's caption fit. Lives for the process.
const UtCaptions* panelButtonCaptions();
// The same plus NEXT, for a paged pad's fit.
const UtCaptions* panelButtonCaptionsPaged();

// The caption fit's UtWidestFn (ctx = a UtCaptions*): the widest caption at `size` in the font the
// pad draws - the live metrics once the world's cross-check agreed, else the compiled table.
int widestCaptionLivePx(int size, const void* ctx);
// Changes whenever that measure may have changed (a world's fonts loaded and cross-checked, the
// strings file's captions or sizes changed).
long panelMeasureGen();
// The buttons between MAT and OWN: the buttons file's (one per [TAG] whose groups this game has),
// else one per group of the table (the 24 built-in before one loads). ut_live's layout pages them.
int panelPadButtonCount();
// The button that holds `group`, 0-based in the order the pad draws them (the buttons file's
// order; one button per group - `group` itself - while no TextWorld is resolved for the table in
// force); -1 for a group on no button ([unassigned]) and for -1 (the materials page = MAT, not a
// button of the file). Ctrl+wheel / Ctrl+PageUp/Down walk the buttons in this order
// (ut_padpage.h utPadWheelButton). Any thread (the published TextWorld is never freed).
int panelButtonOfGroup(int group);
// The button map as ut_live's one grid per button reads it (ut_padpage.h utUnionPlan): the groups
// of the button that holds `group`, ascending (at most `cap` written; returns how many; 0 = on no
// button), and a number that changes whenever that map may have changed (0 = no TextWorld for the
// group table in force: one button per group). Any thread; GAME THREAD for a consistent pair.
int panelButtonGroupsOf(int group, int* out, int cap);
long panelButtonMapGen();
// A button's own caption size cap (the file's size =), else `capMax`; its own font path (font =),
// else "" (the caption role's font). By LOGICAL index (MAT 0, the buttons, OWN, NEXT).
int panelButtonSizeCap(int logical, int capMax);
const char* panelButtonFontPath(int logical);
// What drawButtonsInner needs per button (by LOGICAL index): the caption's font (its font = line,
// else the caption font) and its width there; its size (the shared `fs`, or with a size = line the
// largest that fits that caption alone in a boxW x boxH cell); whether any of its groups carries
// the game's search mark.
const GdFont* panelButtonCaptionFont(int logical);
int panelButtonCaptionTextPx(int logical, const char* s, int size);
int panelButtonCaptionSize(int logical, int fs, int boxW, int boxH);
bool panelButtonSearchMarked(int logical);

// The buttons file uniquetab-buttons.ini of the game in force (ut_buttons.h; the retired
// uniquetab-text.ini is only folded in once). The worker's once-a-second loop calls
// panelTextFileTick: a stat, a read and a parse only on a change, the populated default written
// when the file is absent and the game's group table is known. panelTextStartup is the first look, at start-up (worker), with the two
// rollover lines resolved for it before any world: no language yet, printable ASCII only; the
// `fromFile` flags say whether the file set them; `roll` gets [tab] tooltip.roll (the words of
// the "(another roll)" line; null = not asked).
void panelTextFileTick();
void panelTextStartup(char* yes, size_t yesCap, char* no, size_t noCap, bool* yesFromFile,
                      bool* noFromFile, char* roll, size_t rollCap);
// The caption ceiling and the label size in force: size.caption_max / size.label of the strings
// file when set, else pad_caption_max / plate_label_size of uniquetab.ini. Any thread.
int panelCaptionMaxInForce();
int panelLabelSizeInForce();

// One button, in WINDOW-LOCAL SCALED pixels: the draw adds the window origin, the hit test does
// not (the engine's mouse handler has already subtracted it).
struct UtButtonRect {
    float x, y, w, h;
};

// Fills out[kUtButtonCount] and returns kUtButtonCount, or returns 0 when the pad must not be
// drawn at all (an unusable scale, or a pad_y/pad_h/pad_gap that would leave the joint free
// rectangle x 102..422 / y 25..69 - the pad is refused WHOLE, never trimmed).
// Pure arithmetic: no engine call, no lock, no allocation, no log.
// Per drawn SLOT; `logical` (optional) gets each slot's logical button, -1 = an empty slot.
int panelButtonRects(UtButtonRect* out, int* logical, int cap, float scale);

// The pad's geometry in RECORD px, for the log lines and the menu test - 0 when the ini geometry
// is refused, which is itself the answer.  RecX0/RecY0 and RecX1/RecY1 are the PAD's own outer
// rectangle (the dark ground), both edges inclusive; Row1Count is the column count (9) - the
// refusal test `<= 0` reads through it.
int panelButtonRow1Count();
int panelButtonRecX0();
int panelButtonRecX1();
int panelButtonRecY0();
int panelButtonRecY1();

// The three-letter caption drawn in a button ("MAT", "HEL", ...). Never null.
const char* panelButtonTag(int index);

// The band the CLICK PROBE records: record x 102..422 and the THREE ROW BANDS in y, scaled -
// never the whole plate (a 26-button pad must not start eating clicks in empty plate area).
// Wider than the buttons in x on purpose - a probe has to see the events that miss them too.
bool panelButtonsInBand(float lx, float ly, float scale);

// Which button contains the window-local scaled point, or -1. `scale` as above.
int panelButtonHit(float lx, float ly, float scale);

// The game's own materials page: true when the pad is on screen there and the window-local
// scaled point lies inside its rectangle (between the buttons too). The pad is drawn where it is,
// over the page's boxes, so such a point is the pad's: a LEFT DOWN there never reaches the
// engine (ut_padmat.h utPadLeftDown), and the engine's hovered-box answer is put back for every
// other event there. False on a collection page.
bool panelMatPadAt(float lx, float ly, float scale);

// What a click on button `i` selects: ABSOLUTELY the first group of its button (-1 = the vanilla
// materials page) - the page of a several-group button is the union of its groups, keyed by the
// first - or -2 when the index is out of range. The LAST button is the exception and returns
// kUtButtonFilterTarget - it selects nothing and toggles the owned-only filter instead.
// `label` (optional) gets the group's own name (a several-group button's: the button's name); it
// lives for the process and is never null.
int panelButtonTarget(int index, const char** label);

// Draws the pad. Returns true when something was painted. Writes no engine memory, allocates
// nothing, loads no texture after the first frame of a world, and logs nothing per frame.
bool panelDrawGroupButtons(GdCanvas* canvas, float originX, float originY);

// Cheap per-frame hover update, called from the Draw detour before the pad draws so a button
// under the cursor can wear its `over` art. One GetCursorPos+ScreenToClient per frame. It does
// not feed the LABEL: the label always describes the group that is on screen.
void panelButtonsHoverTick(float originX, float originY);

// True once after the pad's draw body faulted (one-shot; reading it clears it) - the same
// contract as panelLabelDrawFaulted, and for the same reason: the inner __except consumes the
// exception before ut_plate's outer one can see it.
bool panelButtonsDrawFaulted();

// The pad's SESSION kill switch. A mod-owned volatile LONG, never a g_cfg field (configReload
// replaces g_cfg once a second, so an ini-side switch comes straight back). `why` is logged once.
void panelButtonsOff(const char* why);
bool panelButtonsAreOff();

// True while the pad really painted within the last few frames - the click path's proof that
// the rects it is about to hit-test are on screen.
bool panelButtonsDrawnRecently();

// The mouse detour reports a consumed click so the button can paint its `down` state.
void panelButtonsNotePress(int index);

// Drops the three font pointers so the next world loads them again. MUST be called from
// plateOnWorldTeardown: a GraphicsFont2* held across a world teardown faults instead of
// drawing, and the fault would switch the pad off for the rest of the process.
void panelForgetUiFonts();

// There is no pad texture to preload - the pad is solid quads. This resolves the game FONT once
// per world from the GAME-THREAD TICK rather than from inside the engine's own Draw (the same
// rule the plate probe follows), because the caption is the pad's only mark and the label's own
// ensureFonts() is gated on plate_label. A no-op afterwards, and a no-op while the pad is off.
void panelButtonsPreload();

// One menu-time line naming the pad's three ini numbers and its 9 x 3 shape, one naming all 26
// captions in pad order, and one with the computed record rectangle. Logged from plateInit.
void panelButtonsAnnounce();

// ---- the captured GameEngine --------------------------------------------------------------------
// The GameEngine `this` the draw/tick detours last handed us. Read-only; null before the first
// frame with a world. ut_plate.cpp needs it to walk the engine's reagent map.
GdGameEngine* panelGameEngine();

// ---- the layout editor (ut_layoutedit.h; drawn in place of the pad) -------------------------
// The engine's mouse event types the editor reads (the ReagentWindow slot +0x38).
const int kUtMouseLeftDown = 1;
const int kUtMouseRightDown = 2;
const int kUtMouseLeftUp = 9;

// True while the editor is open (any thread).
bool panelLayoutEditOpen();

// The editor is mid-drag (ticked recently) - a drag that began in the window too; the screen
// input adopts it (ut_padinput.h "the adopted drag"). The thread of the Draw tail only.
bool panelLayoutEditDragging();

// True while the page's boxes stay parked: the editor is open and drew within the last 400 ms.
// ut_live's applyLayout parks every box while this holds (any thread).
bool panelLayoutEditing();

// One mouse event at the window-local SCREEN point (lx, ly). RIGHT DOWN with Ctrl on the pad's
// ground opens the editor; while it is open LEFT DOWN on its toolbar, a handle or a rectangle is
// its own and LEFT UP ends a drag (never consumed). `cursorEmpty`, `inBox` and `proven` are the
// pad's own consumption rules. True = consumed (the engine's handler must not run).
bool panelLayoutEditMouse(int type, float lx, float ly, bool cursorEmpty, bool inBox, bool proven);

// ---- diagnostics --------------------------------------------------------------------------
// "cat=<n> add=123 rem=45 matched=78 unknown=3" - for the heartbeat line
// (`cat=` is the loaded catalogue item count).
const char* panelStatus();

}  // namespace ut
