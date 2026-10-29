// test_keys.cpp - the [keys] section's pure half, proved without a game: src\ut_keys.h, the real
// header the settings parse and the window procedure run.
//
//   tools\build_test_keys.bat        (needs a vcvars64 shell; the .bat calls it itself)
//
//   1. the numbers the header spells out (messages, VKs, WHEEL_DELTA) are windows.h's;
//   2. the grammar: every name and alias, case and blanks, none, "" = the default, every
//      modifier combination;
//   3. the canonical text: every legal binding of every action round-trips, the defaults' text is
//      the rows' default text;
//   4. every refusal with its reason: unknown, too long, a modifier twice, two keys, a key first,
//      a '+' with nothing beside it, modifiers alone, none with something, the wrong kind for
//      each action, LeftClick, MiddleClick for editor_open, a text key bare and with Shift, Esc
//      with anything, Alt+F4 / Alt+Tab / Alt+Space;
//   5. the defaults: UtKeys{} = the parse of the seven default texts = utKeysDerive(nothing);
//   6. the conflict pass: the later of two identical chords of one site is none, both named;
//      a refused text keeps the default;
//   7. the matcher: most modifiers wins, a tie the earlier action, remaps, the click match, the
//      SYSKEY gate;
//   8. THE EQUIVALENCE: with the defaults, for every message (KEYDOWN, SYSKEYDOWN, KEYUP,
//      SYSKEYUP, MOUSEWHEEL) x VK 0..255 x Ctrl/Shift/Alt, the site built on this header decides
//      exactly what reagentHandleMessage decided before [keys] (the live branch and the
//      fallback), and over 20000 pseudo-random events the per-VK eaten set swallows exactly the
//      KEYUPs the two-bit rule did;
//   9. the eaten set: any VK, a modifier released in between, a let-through DOWN clears, an UP
//      without a DOWN;
//  10. the window-message decode (the button messages, the XBUTTON word, the wheel's sign and a
//      sub-notch delta);
//  11. the header's purity.
// Exit code 0 = all of it held. Argument 1: the repository root (for the purity check).
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#include "../src/ut_keys.h"

static int g_fails = 0;
static int g_checks = 0;
static void check(bool ok, const char* what) {
    ++g_checks;
    printf("  %-86s %s\n", what, ok ? "OK" : "FAIL");
    if (!ok) ++g_fails;
}

static bool same(const UtKeyBind& a, int kind, int code, int mods) {
    return a.kind == kind && a.code == code && a.mods == mods;
}

// parse for `action`; true when legal and equal to {kind, code, mods}
static bool parsesTo(const char* text, int action, int kind, int code, int mods) {
    UtKeyBind b = {-1, -1, -1};
    const char* why = utKeyParse(text, action, &b);
    if (why) {
        printf("    \"%s\" for %s refused: %s\n", text, kUtKeyActions[action].name, why);
        return false;
    }
    if (!same(b, kind, code, mods)) {
        printf("    \"%s\" for %s -> {%d,%d,%d}, wanted {%d,%d,%d}\n", text,
               kUtKeyActions[action].name, b.kind, b.code, b.mods, kind, code, mods);
        return false;
    }
    return true;
}

// refused for `action` with exactly `reason`, and the output untouched
static bool refusedAs(const char* text, int action, const char* reason) {
    UtKeyBind b = {77, 77, 77};
    const char* why = utKeyParse(text, action, &b);
    const bool ok = why && !strcmp(why, reason) && same(b, 77, 77, 77);
    if (!ok)
        printf("    \"%s\" for %s: got \"%s\", wanted \"%s\"\n", text, kUtKeyActions[action].name,
               why ? why : "(legal)", reason);
    return ok;
}

static UtKeys keysFrom(const char* const texts[kUtKeyActCount]) {
    UtKeys k;
    utKeysDerive(texts, &k, nullptr, 0);
    return k;
}

// ---- today's decision: reagentHandleMessage before [keys], transcribed ------------------------
// The live branch: the wheel -> Ctrl ? the button wheel : the row wheel; a KEYDOWN of PageUp /
// PageDown -> Ctrl ? the previous / next button : a window of rows up / down; anything else
// (another VK, WM_SYSKEYDOWN, any UP) asks for no action. Shift and Alt are read nowhere.
static int todayLive(unsigned msg, int vk, int mods) {
    const bool ctrl = (mods & kUtKeyModCtrl) != 0;
    if (msg == WM_MOUSEWHEEL) return ctrl ? kUtKeyActButtonWheel : kUtKeyActRowsWheel;
    if (msg == WM_KEYDOWN && vk == VK_PRIOR) return ctrl ? kUtKeyActButtonPrev : kUtKeyActRowsUp;
    if (msg == WM_KEYDOWN && vk == VK_NEXT) return ctrl ? kUtKeyActButtonNext : kUtKeyActRowsDown;
    return kUtKeyActNone;
}
// The fallback (live paging not armed): only with Ctrl down; the wheel and PageUp / PageDown
// switch the page (the button actions).
static int todayFallback(unsigned msg, int vk, int mods) {
    if (!(mods & kUtKeyModCtrl)) return kUtKeyActNone;
    if (msg == WM_MOUSEWHEEL) return kUtKeyActButtonWheel;
    if (msg == WM_KEYDOWN && vk == VK_PRIOR) return kUtKeyActButtonPrev;
    if (msg == WM_KEYDOWN && vk == VK_NEXT) return kUtKeyActButtonNext;
    return kUtKeyActNone;
}

// The site as the window procedure builds it on this header: decode, then find. The live branch
// takes every step and wheel action (editor_open as a key too); the fallback only the button
// actions.
static int siteLive(const UtKeys& k, unsigned msg, unsigned long long wParam, int mods) {
    const UtKeyEvent ev = utKeyDecode(msg, wParam, utKeysWantSys(k));
    if (ev.what == kUtKeyEvDown) return utKeyFind(k, kUtKeySiteKey, ev.code, mods);
    if (ev.what == kUtKeyEvWheel) return utKeyFind(k, kUtKeySiteWheel, 0, mods);
    return kUtKeyActNone;
}
static int siteFallback(const UtKeys& k, unsigned msg, unsigned long long wParam, int mods) {
    const int a = siteLive(k, msg, wParam, mods);
    return (a == kUtKeyActButtonPrev || a == kUtKeyActButtonNext || a == kUtKeyActButtonWheel)
               ? a
               : kUtKeyActNone;
}

static unsigned g_rand = 12345;
static unsigned rnd() {
    g_rand = g_rand * 1103515245u + 12345u;
    return (g_rand >> 16) & 0x7FFF;
}

int main(int argc, char** argv) {
    const std::string root = argc > 1 ? argv[1] : ".";
    char buf[64];

    // ------------------------------------------------------------------------------------------
    printf("1. the numbers the header spells out are windows.h's\n");
    check(kUtKeyMsgKeyDown == WM_KEYDOWN && kUtKeyMsgKeyUp == WM_KEYUP &&
              kUtKeyMsgSysKeyDown == WM_SYSKEYDOWN && kUtKeyMsgSysKeyUp == WM_SYSKEYUP &&
              kUtKeyMsgMButtonDown == WM_MBUTTONDOWN && kUtKeyMsgMButtonUp == WM_MBUTTONUP &&
              kUtKeyMsgMouseWheel == WM_MOUSEWHEEL && kUtKeyMsgXButtonDown == WM_XBUTTONDOWN &&
              kUtKeyMsgXButtonUp == WM_XBUTTONUP && kUtKeyWheelDelta == WHEEL_DELTA,
          "the nine window messages and WHEEL_DELTA");
    check(kUtVkMButton == VK_MBUTTON && kUtVkXButton1 == VK_XBUTTON1 && kUtVkXButton2 == VK_XBUTTON2 &&
              kUtVkBack == VK_BACK && kUtVkTab == VK_TAB && kUtVkReturn == VK_RETURN &&
              kUtVkEscape == VK_ESCAPE && kUtVkSpace == VK_SPACE && kUtVkPrior == VK_PRIOR &&
              kUtVkNext == VK_NEXT && kUtVkEnd == VK_END && kUtVkHome == VK_HOME &&
              kUtVkLeft == VK_LEFT && kUtVkUp == VK_UP && kUtVkRight == VK_RIGHT &&
              kUtVkDown == VK_DOWN && kUtVkInsert == VK_INSERT && kUtVkDelete == VK_DELETE &&
              kUtVkNumpad0 == VK_NUMPAD0 && kUtVkMultiply == VK_MULTIPLY && kUtVkAdd == VK_ADD &&
              kUtVkSubtract == VK_SUBTRACT && kUtVkDecimal == VK_DECIMAL &&
              kUtVkDivide == VK_DIVIDE && kUtVkF1 == VK_F1 && kUtVkF4 == VK_F4 &&
              kUtVkF10 == VK_F10 && kUtVkF24 == VK_F24 && XBUTTON1 == 1 && XBUTTON2 == 2,
          "every virtual-key code, and XBUTTON1 / XBUTTON2 = 1 / 2");
    check(kUtKeyActCount == 7 && !strcmp(kUtKeyActions[0].name, "rows_up") &&
              !strcmp(kUtKeyActions[6].name, "editor_open") &&
              utKeyActionOf("BUTTON_NEXT") == kUtKeyActButtonNext && utKeyActionOf("rows") < 0 &&
              utKeyActionOf("rows_up_") < 0 && utKeyActionOf(nullptr) < 0,
          "seven actions in table order; utKeyActionOf by name, any case, exact");

    // ------------------------------------------------------------------------------------------
    printf("\n2. the grammar: every name and alias, case and blanks\n");
    {
        struct Named {
            const char* text;
            int code;
        };
        static const Named kKeys[] = {
            {"PageUp", VK_PRIOR}, {"PgUp", VK_PRIOR}, {"PageDown", VK_NEXT}, {"PgDn", VK_NEXT},
            {"Home", VK_HOME},    {"End", VK_END},    {"Insert", VK_INSERT}, {"Ins", VK_INSERT},
            {"Delete", VK_DELETE}, {"Del", VK_DELETE}, {"Up", VK_UP},        {"Down", VK_DOWN},
            {"Left", VK_LEFT},    {"Right", VK_RIGHT}, {"F1", VK_F1},        {"F10", VK_F10},
            {"F12", VK_F12},      {"F24", VK_F24},
        };
        int bad = 0;
        for (const Named& n : kKeys)
            if (!parsesTo(n.text, kUtKeyActRowsUp, kUtKeyKey, n.code, 0)) ++bad;
        for (int f = 1; f <= 24; ++f) {
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "f%d", f);
            if (!parsesTo(buf, kUtKeyActRowsDown, kUtKeyKey, VK_F1 + f - 1, 0)) ++bad;
        }
        check(bad == 0, "the named keys, the aliases and F1..F24 (any case) are keys, no modifier");
        bad = 0;
        static const Named kText[] = {
            {"Space", VK_SPACE}, {"Tab", VK_TAB},   {"Enter", VK_RETURN},   {"Return", VK_RETURN},
            {"Backspace", VK_BACK}, {"NumMul", VK_MULTIPLY}, {"NumPlus", VK_ADD},
            {"NumMinus", VK_SUBTRACT}, {"NumDot", VK_DECIMAL}, {"NumDiv", VK_DIVIDE},
        };
        for (const Named& n : kText) {
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "Ctrl+%s", n.text);
            if (!parsesTo(buf, kUtKeyActButtonPrev, kUtKeyKey, n.code, kUtKeyModCtrl)) ++bad;
        }
        for (char c = 'A'; c <= 'Z'; ++c) {
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "alt+%c", c - 'A' + 'a');
            if (!parsesTo(buf, kUtKeyActButtonNext, kUtKeyKey, c, kUtKeyModAlt)) ++bad;
        }
        for (char c = '0'; c <= '9'; ++c) {
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "Ctrl+%c", c);
            if (!parsesTo(buf, kUtKeyActRowsUp, kUtKeyKey, c, kUtKeyModCtrl)) ++bad;
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "Ctrl+Num%c", c);
            if (!parsesTo(buf, kUtKeyActRowsUp, kUtKeyKey, VK_NUMPAD0 + (c - '0'), kUtKeyModCtrl)) ++bad;
        }
        check(bad == 0, "letters, digits, the numpad, Space / Tab / Enter / Backspace with Ctrl or Alt");
        check(parsesTo("Wheel", kUtKeyActRowsWheel, kUtKeyWheel, 0, 0) &&
                  parsesTo("Ctrl+Wheel", kUtKeyActButtonWheel, kUtKeyWheel, 0, kUtKeyModCtrl) &&
                  parsesTo("shift+alt+WHEEL", kUtKeyActRowsWheel, kUtKeyWheel, 0,
                           kUtKeyModShift | kUtKeyModAlt),
              "the wheel, with and without modifiers");
        check(parsesTo("MiddleClick", kUtKeyActRowsUp, kUtKeyButton, VK_MBUTTON, 0) &&
                  parsesTo("Mouse4", kUtKeyActButtonPrev, kUtKeyButton, VK_XBUTTON1, 0) &&
                  parsesTo("ctrl+mouse5", kUtKeyActButtonNext, kUtKeyButton, VK_XBUTTON2, kUtKeyModCtrl),
              "a step bound to MiddleClick / Mouse4 / Mouse5 is a button: its VK");
        check(parsesTo("Ctrl+RightClick", kUtKeyActEditorOpen, kUtKeyClick, kUtKeyClickRight, kUtKeyModCtrl) &&
                  parsesTo("RightClick", kUtKeyActEditorOpen, kUtKeyClick, 2, 0) &&
                  parsesTo("Alt+RightClick", kUtKeyActEditorOpen, kUtKeyClick, 2, kUtKeyModAlt) &&
                  parsesTo("F8", kUtKeyActEditorOpen, kUtKeyKey, VK_F8, 0) &&
                  parsesTo("Ctrl+E", kUtKeyActEditorOpen, kUtKeyKey, 'E', kUtKeyModCtrl),
              "editor_open: RightClick is the engine's type 2 with its modifiers, or a key");
        check(parsesTo("  Ctrl  +  Shift +PageUp ", kUtKeyActRowsUp, kUtKeyKey, VK_PRIOR,
                       kUtKeyModCtrl | kUtKeyModShift) &&
                  parsesTo("\tcontrol+pgdn", kUtKeyActRowsUp, kUtKeyKey, VK_NEXT, kUtKeyModCtrl) &&
                  parsesTo("CTRL+SHIFT+ALT+HOME", kUtKeyActRowsUp, kUtKeyKey, VK_HOME, 7),
              "blanks around '+', Control = Ctrl, any case");
        check(parsesTo("none", kUtKeyActRowsUp, kUtKeyNone, 0, 0) &&
                  parsesTo(" NONE ", kUtKeyActEditorOpen, kUtKeyNone, 0, 0) &&
                  parsesTo("None", kUtKeyActRowsWheel, kUtKeyNone, 0, 0),
              "none turns any action off");
        check(parsesTo("", kUtKeyActButtonPrev, kUtKeyKey, VK_PRIOR, kUtKeyModCtrl) &&
                  parsesTo("   ", kUtKeyActRowsWheel, kUtKeyWheel, 0, 0) &&
                  parsesTo(nullptr, kUtKeyActEditorOpen, kUtKeyClick, 2, kUtKeyModCtrl),
              "\"\" (or blanks, or no text) is the action's default");
        bad = 0;
        static const char* const kModNames[3] = {"Ctrl", "Shift", "Alt"};
        for (int m = 0; m < 8; ++m) {
            std::string t;
            for (int i = 0; i < 3; ++i)
                if (m & (1 << i)) t += std::string(kModNames[i]) + "+";
            if (!parsesTo((t + "End").c_str(), kUtKeyActRowsUp, kUtKeyKey, VK_END, m)) ++bad;
            std::string r;   // the same modifiers in the other order
            for (int i = 2; i >= 0; --i)
                if (m & (1 << i)) r += std::string(kModNames[i]) + "+";
            if (!parsesTo((r + "Wheel").c_str(), kUtKeyActButtonWheel, kUtKeyWheel, 0, m)) ++bad;
        }
        check(bad == 0, "every modifier combination, in either order, is the same set of bits");
    }

    // ------------------------------------------------------------------------------------------
    printf("\n3. the canonical text round-trips\n");
    {
        int legal = 0, bad = 0;
        static const char* const kWords[] = {
            "PageUp", "PgDn", "Home", "End", "Ins", "Del", "Up", "Down", "Left", "Right",
            "Space", "Tab", "Enter", "Return", "Backspace", "NumMul", "NumPlus", "NumMinus",
            "NumDot", "NumDiv", "Wheel", "RightClick", "MiddleClick", "Mouse4", "Mouse5", "none",
            "Q", "7", "F3", "F10", "F24", "Num0", "Num9"};
        static const char* const kModNames[3] = {"Ctrl+", "Shift+", "Alt+"};
        for (int a = 0; a < kUtKeyActCount; ++a)
            for (const char* w : kWords)
                for (int m = 0; m < 8; ++m) {
                    std::string t;
                    for (int i = 0; i < 3; ++i)
                        if (m & (1 << i)) t += kModNames[i];
                    t += w;
                    UtKeyBind b, b2;
                    if (utKeyParse(t.c_str(), a, &b)) continue;
                    ++legal;
                    utKeyText(b, buf, sizeof(buf));
                    if (utKeyParse(buf, a, &b2) || !utKeyBindSame(b, b2)) {
                        printf("    %s for %s -> \"%s\" does not parse back\n", t.c_str(),
                               kUtKeyActions[a].name, buf);
                        ++bad;
                    }
                }
        printf("    %d legal bindings tried\n", legal);
        check(legal > 300 && bad == 0, "every legal binding of every action: text -> parse is itself");
        const UtKeys d;
        bad = 0;
        for (int a = 0; a < kUtKeyActCount; ++a) {
            utKeyText(*utKeyBindOf(d, a), buf, sizeof(buf));
            if (strcmp(buf, kUtKeyActions[a].def) != 0) {
                printf("    %s: \"%s\" vs the row's \"%s\"\n", kUtKeyActions[a].name, buf,
                       kUtKeyActions[a].def);
                ++bad;
            }
        }
        check(bad == 0, "the defaults' canonical text is each row's default text, letter for letter");
        UtKeyBind b;
        utKeyParse("alt+shift+ctrl+pgup", kUtKeyActRowsUp, &b);
        check(!strcmp(utKeyText(b, buf, sizeof(buf)), "Ctrl+Shift+Alt+PageUp"),
              "the canonical order is Ctrl, Shift, Alt, and the key's own name (PgUp -> PageUp)");
        utKeyParse("ctrl+return", kUtKeyActRowsUp, &b);
        check(!strcmp(utKeyText(b, buf, sizeof(buf)), "Ctrl+Enter"), "Return is written Enter");
        utKeyParse("mouse4", kUtKeyActRowsUp, &b);
        check(!strcmp(utKeyText(b, buf, sizeof(buf)), "Mouse4"), "Mouse4 is a button and is written so");
        const UtKeyBind off = {kUtKeyNone, 0, 0};
        const UtKeyBind odd = {kUtKeyKey, 0xE7, kUtKeyModCtrl};
        char tiny[8];
        check(!strcmp(utKeyText(off, buf, sizeof(buf)), "none") &&
                  !strcmp(utKeyText(odd, buf, sizeof(buf)), "Ctrl+?") &&
                  utKeyText(off, tiny, sizeof(tiny))[0] == 0,
              "none is \"none\", a code no word names is \"?\", a short buffer gets nothing");
    }

    // ------------------------------------------------------------------------------------------
    printf("\n4. every refusal, with its reason\n");
    {
        check(refusedAs("PageUpp", kUtKeyActRowsUp, UT_KEY_WHY_UNKNOWN) &&
                  refusedAs("Ctrl+Foo", kUtKeyActRowsUp, UT_KEY_WHY_UNKNOWN) &&
                  refusedAs("Strg+A", kUtKeyActRowsUp, UT_KEY_WHY_UNKNOWN) &&
                  refusedAs("F0", kUtKeyActRowsUp, UT_KEY_WHY_UNKNOWN) &&
                  refusedAs("F25", kUtKeyActRowsUp, UT_KEY_WHY_UNKNOWN) &&
                  refusedAs("F05", kUtKeyActRowsUp, UT_KEY_WHY_UNKNOWN) &&
                  refusedAs("Num10", kUtKeyActRowsUp, UT_KEY_WHY_UNKNOWN) &&
                  refusedAs("Ctrl-PageUp", kUtKeyActRowsUp, UT_KEY_WHY_UNKNOWN) &&
                  refusedAs("!", kUtKeyActRowsUp, UT_KEY_WHY_UNKNOWN),
              "an unknown word (a typo, another language's Ctrl, F0 / F25 / F05, Num10, '-', a sign)");
        check(refusedAs("Ctrl+Ctrl+A", kUtKeyActRowsUp, UT_KEY_WHY_MOD_TWICE) &&
                  refusedAs("Control+ctrl+PageUp", kUtKeyActRowsUp, UT_KEY_WHY_MOD_TWICE) &&
                  refusedAs("Shift+Alt+Shift+Wheel", kUtKeyActRowsWheel, UT_KEY_WHY_MOD_TWICE),
              "a modifier twice (Control and Ctrl are one)");
        check(refusedAs("Ctrl+Q+W", kUtKeyActRowsUp, UT_KEY_WHY_TWO_KEYS) &&
                  refusedAs("PageUp+PageDown", kUtKeyActRowsUp, UT_KEY_WHY_TWO_KEYS) &&
                  refusedAs("Ctrl+Wheel+RightClick", kUtKeyActEditorOpen, UT_KEY_WHY_TWO_KEYS) &&
                  refusedAs("a+b+c+d+e+f+g+h+i", kUtKeyActRowsUp, UT_KEY_WHY_TWO_KEYS),
              "two keys (Ctrl+Q+W, two named keys, nine parts)");
        check(refusedAs("PageUp+Ctrl", kUtKeyActRowsUp, UT_KEY_WHY_KEY_FIRST) &&
                  refusedAs("Wheel+Shift", kUtKeyActRowsWheel, UT_KEY_WHY_KEY_FIRST),
              "a key before a modifier");
        check(refusedAs("Ctrl+", kUtKeyActRowsUp, UT_KEY_WHY_EMPTY_PART) &&
                  refusedAs("Ctrl+ ", kUtKeyActRowsUp, UT_KEY_WHY_EMPTY_PART) &&
                  refusedAs("+PageUp", kUtKeyActRowsUp, UT_KEY_WHY_EMPTY_PART) &&
                  refusedAs("Ctrl++PageUp", kUtKeyActRowsUp, UT_KEY_WHY_EMPTY_PART) &&
                  refusedAs("+", kUtKeyActRowsUp, UT_KEY_WHY_EMPTY_PART),
              "a trailing '+', a leading one, two in a row, '+' alone");
        check(refusedAs("Ctrl", kUtKeyActRowsUp, UT_KEY_WHY_NO_KEY) &&
                  refusedAs("Ctrl+Shift", kUtKeyActRowsWheel, UT_KEY_WHY_NO_KEY),
              "modifiers without a key");
        check(refusedAs("Ctrl+none", kUtKeyActRowsUp, UT_KEY_WHY_NONE_ALONE) &&
                  refusedAs("none+PageUp", kUtKeyActRowsUp, UT_KEY_WHY_NONE_ALONE),
              "none with anything else");
        check(refusedAs("Wheel", kUtKeyActRowsUp, UT_KEY_WHY_KIND_STEP) &&
                  refusedAs("Ctrl+RightClick", kUtKeyActButtonNext, UT_KEY_WHY_KIND_STEP) &&
                  refusedAs("PageUp", kUtKeyActRowsWheel, UT_KEY_WHY_KIND_WHEEL) &&
                  refusedAs("Mouse4", kUtKeyActButtonWheel, UT_KEY_WHY_KIND_WHEEL) &&
                  refusedAs("RightClick", kUtKeyActRowsWheel, UT_KEY_WHY_KIND_WHEEL) &&
                  refusedAs("Mouse4", kUtKeyActEditorOpen, UT_KEY_WHY_KIND_EDITOR) &&
                  refusedAs("Ctrl+Wheel", kUtKeyActEditorOpen, UT_KEY_WHY_KIND_EDITOR) &&
                  refusedAs("Mouse5", kUtKeyActEditorOpen, UT_KEY_WHY_KIND_EDITOR),
              "the wrong kind: rows_up=Wheel, a step on RightClick, rows_wheel=PageUp, editor_open=Mouse4");
        bool left = true;
        for (int a = 0; a < kUtKeyActCount; ++a)
            left = refusedAs("LeftClick", a, UT_KEY_WHY_LEFT) &&
                   refusedAs("Ctrl+Shift+LeftClick", a, UT_KEY_WHY_LEFT) && left;
        check(left, "LeftClick anywhere, with or without modifiers (the buttons' own)");
        check(refusedAs("MiddleClick", kUtKeyActEditorOpen, UT_KEY_WHY_MIDDLE_EDITOR) &&
                  refusedAs("Ctrl+MiddleClick", kUtKeyActEditorOpen, UT_KEY_WHY_MIDDLE_EDITOR),
              "MiddleClick for editor_open (this release)");
        int bad = 0;
        static const char* const kTextKeys[] = {
            "A", "z", "0", "9", "Space", "Enter", "Return", "Tab", "Backspace", "Num0", "Num9",
            "NumMul", "NumPlus", "NumMinus", "NumDot", "NumDiv"};
        for (const char* t : kTextKeys) {
            if (!refusedAs(t, kUtKeyActRowsUp, UT_KEY_WHY_TEXT)) ++bad;
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "Shift+%s", t);
            if (!refusedAs(buf, kUtKeyActEditorOpen, UT_KEY_WHY_TEXT)) ++bad;
        }
        check(bad == 0, "a text key bare or with Shift only: the stash search box would get it too");
        check(parsesTo("Ctrl+Shift+A", kUtKeyActRowsUp, kUtKeyKey, 'A', kUtKeyModCtrl | kUtKeyModShift) &&
                  parsesTo("Alt+Enter", kUtKeyActRowsUp, kUtKeyKey, VK_RETURN, kUtKeyModAlt) &&
                  parsesTo("Shift+PageUp", kUtKeyActRowsUp, kUtKeyKey, VK_PRIOR, kUtKeyModShift) &&
                  parsesTo("Delete", kUtKeyActRowsUp, kUtKeyKey, VK_DELETE, 0) &&
                  parsesTo("F5", kUtKeyActRowsUp, kUtKeyKey, VK_F5, 0),
              "with Ctrl or Alt a text key is fine; Shift+PageUp, Delete and F5 are not text keys");
        check(refusedAs("Esc", kUtKeyActRowsUp, UT_KEY_WHY_ESC) &&
                  refusedAs("Escape", kUtKeyActEditorOpen, UT_KEY_WHY_ESC) &&
                  refusedAs("Ctrl+Esc", kUtKeyActButtonPrev, UT_KEY_WHY_ESC) &&
                  refusedAs("Alt+Esc", kUtKeyActButtonNext, UT_KEY_WHY_ESC) &&
                  refusedAs("Esc", kUtKeyActRowsWheel, UT_KEY_WHY_ESC),
              "Esc with anything (the game's own close key; Ctrl+Esc / Alt+Esc are Windows')");
        check(refusedAs("Alt+F4", kUtKeyActRowsUp, UT_KEY_WHY_WINDOWS) &&
                  refusedAs("Alt+Tab", kUtKeyActButtonNext, UT_KEY_WHY_WINDOWS) &&
                  refusedAs("Alt+Shift+Tab", kUtKeyActButtonPrev, UT_KEY_WHY_WINDOWS) &&
                  refusedAs("Alt+Space", kUtKeyActEditorOpen, UT_KEY_WHY_WINDOWS) &&
                  refusedAs("Ctrl+Alt+F4", kUtKeyActRowsDown, UT_KEY_WHY_WINDOWS) &&
                  parsesTo("Ctrl+F4", kUtKeyActRowsDown, kUtKeyKey, VK_F4, kUtKeyModCtrl) &&
                  parsesTo("F4", kUtKeyActRowsDown, kUtKeyKey, VK_F4, 0),
              "Alt+F4, Alt+Tab, Alt+Space (with anything else too); Ctrl+F4 and F4 are fine");
        std::string longText(70, 'A');
        check(refusedAs(longText.c_str(), kUtKeyActRowsUp, UT_KEY_WHY_TOO_LONG) &&
                  refusedAs("Ctrl", 99, UT_KEY_WHY_UNKNOWN) && refusedAs("Ctrl", -1, UT_KEY_WHY_UNKNOWN),
              "a value over 64 characters; an action that does not exist");
        UtKeyBind b;
        const char* why = utKeyParse("Ctrl+Q+W", kUtKeyActButtonNext, &b);
        printf("    the log would say: button_next=Ctrl+Q+W is %s - ignored\n", why ? why : "?");
        why = utKeyParse("Q", kUtKeyActButtonNext, &b);
        printf("    the log would say: button_next=Q is %s - ignored\n", why ? why : "?");
        check(why && strstr(why, "search box") && strstr(why, "Ctrl or Alt"),
              "the text-key reason names the search box and the fix");
    }

    // ------------------------------------------------------------------------------------------
    printf("\n5. the defaults\n");
    {
        const UtKeys d;
        int bad = 0;
        for (int a = 0; a < kUtKeyActCount; ++a) {
            UtKeyBind b;
            if (utKeyParse(kUtKeyActions[a].def, a, &b) || !utKeyBindSame(b, *utKeyBindOf(d, a))) ++bad;
        }
        check(bad == 0, "UtKeys{} = the parse of the seven default texts");
        UtKeys k;
        memset(&k, 0x5A, sizeof(k));
        UtKeyConflict c[kUtKeyActCount];
        const int n = utKeysDerive(nullptr, &k, c, kUtKeyActCount);
        check(n == 0 && !memcmp(&k, &d, sizeof(k)), "utKeysDerive(no texts) = UtKeys{}, no conflict");
        const char* const blanks[kUtKeyActCount] = {"", "", "", "", "", "", ""};
        const UtKeys kb = keysFrom(blanks);
        check(!memcmp(&kb, &d, sizeof(k)), "seven empty texts = the defaults too");
        check(same(d.rowsUp, kUtKeyKey, VK_PRIOR, 0) && same(d.rowsDown, kUtKeyKey, VK_NEXT, 0) &&
                  same(d.buttonPrev, kUtKeyKey, VK_PRIOR, kUtKeyModCtrl) &&
                  same(d.buttonNext, kUtKeyKey, VK_NEXT, kUtKeyModCtrl) &&
                  same(d.rowsWheel, kUtKeyWheel, 0, 0) &&
                  same(d.buttonWheel, kUtKeyWheel, 0, kUtKeyModCtrl) &&
                  same(d.editorOpen, kUtKeyClick, 2, kUtKeyModCtrl),
              "the defaults spelled out: PageUp, PageDown, Ctrl+PageUp, Ctrl+PageDown, Wheel, "
              "Ctrl+Wheel, Ctrl+RightClick");
        check(!utKeysWantSys(d) && !utKeysWantButtons(d), "the defaults look at no SYSKEY message and no button");
    }

    // ------------------------------------------------------------------------------------------
    printf("\n6. the conflict pass\n");
    {
        UtKeyConflict c[kUtKeyActCount];
        const char* t1[kUtKeyActCount] = {"Ctrl+PageUp", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
        UtKeys k;
        int n = utKeysDerive(t1, &k, c, kUtKeyActCount);
        check(n == 1 && c[0].earlier == kUtKeyActRowsUp && c[0].later == kUtKeyActButtonPrev &&
                  same(c[0].chord, kUtKeyKey, VK_PRIOR, kUtKeyModCtrl) &&
                  same(k.rowsUp, kUtKeyKey, VK_PRIOR, kUtKeyModCtrl) && k.buttonPrev.kind == kUtKeyNone,
              "rows_up=Ctrl+PageUp: button_prev (the later line) is none, both named, the chord kept");
        const char* t2[kUtKeyActCount] = {nullptr, nullptr, nullptr, nullptr, nullptr, "Wheel", nullptr};
        n = utKeysDerive(t2, &k, c, kUtKeyActCount);
        check(n == 1 && c[0].earlier == kUtKeyActRowsWheel && c[0].later == kUtKeyActButtonWheel &&
                  k.buttonWheel.kind == kUtKeyNone && same(k.rowsWheel, kUtKeyWheel, 0, 0),
              "button_wheel=Wheel: the same as rows_wheel, button_wheel is none");
        const char* t3[kUtKeyActCount] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "PageDown"};
        n = utKeysDerive(t3, &k, c, kUtKeyActCount);
        check(n == 1 && c[0].earlier == kUtKeyActRowsDown && c[0].later == kUtKeyActEditorOpen &&
                  k.editorOpen.kind == kUtKeyNone,
              "editor_open=PageDown (a key) is the same chord as rows_down: editor_open is none");
        const char* t4[kUtKeyActCount] = {"F3", "F3", "F3", nullptr, nullptr, nullptr, "F3"};
        n = utKeysDerive(t4, &k, c, kUtKeyActCount);
        check(n == 3 && c[0].earlier == 0 && c[0].later == 1 && c[1].earlier == 0 && c[1].later == 2 &&
                  c[2].earlier == 0 && c[2].later == 6 && same(k.rowsUp, kUtKeyKey, VK_F3, 0) &&
                  k.rowsDown.kind == kUtKeyNone && k.buttonPrev.kind == kUtKeyNone &&
                  k.editorOpen.kind == kUtKeyNone,
              "four lines on F3: the first keeps it, the three later are none, each named with it");
        const char* t5[kUtKeyActCount] = {"Ctrl+PageUp", nullptr, "Shift+PageUp", nullptr, nullptr, nullptr, nullptr};
        n = utKeysDerive(t5, &k, c, kUtKeyActCount);
        check(n == 0, "rows_up=Ctrl+PageUp with button_prev=Shift+PageUp: no conflict (different chords)");
        const char* t6[kUtKeyActCount] = {"none", nullptr, nullptr, nullptr, "none", "none", "none"};
        n = utKeysDerive(t6, &k, c, kUtKeyActCount);
        check(n == 0 && k.rowsUp.kind == kUtKeyNone && k.editorOpen.kind == kUtKeyNone,
              "several none lines never conflict");
        const char* t7[kUtKeyActCount] = {"Ctrl+Q+W", "Wheel", "Q", nullptr, "PageUp", nullptr, "MiddleClick"};
        n = utKeysDerive(t7, &k, c, kUtKeyActCount);
        const UtKeys d;
        check(n == 0 && !memcmp(&k, &d, sizeof(k)), "refused texts keep their action's default");
        const char* t8[kUtKeyActCount] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "RightClick"};
        n = utKeysDerive(t8, &k, c, kUtKeyActCount);
        check(n == 0 && same(k.editorOpen, kUtKeyClick, 2, 0),
              "editor_open=RightClick is a click: no key action can collide with it");
        const char* t9[kUtKeyActCount] = {"MiddleClick", "Mouse4", "Ctrl+MiddleClick", "Mouse4", nullptr, nullptr, nullptr};
        n = utKeysDerive(t9, &k, c, 1);   // a one-entry list: the count is still every pair
        check(n == 1 && c[0].earlier == kUtKeyActRowsDown && c[0].later == kUtKeyActButtonNext &&
                  k.buttonNext.kind == kUtKeyNone && same(k.buttonPrev, kUtKeyButton, VK_MBUTTON, kUtKeyModCtrl),
              "buttons: rows_down and button_next on Mouse4 collide; MiddleClick and Ctrl+MiddleClick do not");
    }

    // ------------------------------------------------------------------------------------------
    printf("\n7. the matcher\n");
    {
        const UtKeys d;
        const int C = kUtKeyModCtrl, S = kUtKeyModShift, A = kUtKeyModAlt;
        check(utKeyFind(d, kUtKeySiteKey, VK_PRIOR, 0) == kUtKeyActRowsUp &&
                  utKeyFind(d, kUtKeySiteKey, VK_PRIOR, S) == kUtKeyActRowsUp &&
                  utKeyFind(d, kUtKeySiteKey, VK_PRIOR, C) == kUtKeyActButtonPrev &&
                  utKeyFind(d, kUtKeySiteKey, VK_PRIOR, C | S) == kUtKeyActButtonPrev &&
                  utKeyFind(d, kUtKeySiteKey, VK_NEXT, C | A) == kUtKeyActButtonNext &&
                  utKeyFind(d, kUtKeySiteKey, VK_NEXT, A) == kUtKeyActRowsDown &&
                  utKeyFind(d, kUtKeySiteKey, 'A', C) == kUtKeyActNone,
              "defaults: Shift+PageUp = rows, Ctrl+Shift+PageUp = button, Alt+PageDown = rows (if seen)");
        check(utKeyFind(d, kUtKeySiteWheel, 0, 0) == kUtKeyActRowsWheel &&
                  utKeyFind(d, kUtKeySiteWheel, 0, A) == kUtKeyActRowsWheel &&
                  utKeyFind(d, kUtKeySiteWheel, 0, S) == kUtKeyActRowsWheel &&
                  utKeyFind(d, kUtKeySiteWheel, 0, C) == kUtKeyActButtonWheel &&
                  utKeyFind(d, kUtKeySiteWheel, 0, C | S | A) == kUtKeyActButtonWheel &&
                  utKeyFind(d, kUtKeySiteWheel, 1234, 0) == kUtKeyActRowsWheel,
              "defaults: the wheel is rows, Alt+wheel rows, Ctrl+anything+wheel a button");
        check(utKeyFind(d, kUtKeySiteClick, 2, C) == kUtKeyActEditorOpen &&
                  utKeyFind(d, kUtKeySiteClick, 2, 0) == kUtKeyActNone &&
                  utKeyClickMatches(d.editorOpen, 2, C) && utKeyClickMatches(d.editorOpen, 2, C | S) &&
                  !utKeyClickMatches(d.editorOpen, 2, S) && !utKeyClickMatches(d.editorOpen, 1, C) &&
                  !utKeyClickMatches(d.editorOpen, 3, C),
              "defaults: Ctrl+right click opens the editor (Ctrl+Shift too, as before); plain, left, middle do not");
        const char* t1[kUtKeyActCount] = {nullptr, nullptr, nullptr, "Ctrl+F3", nullptr, nullptr, nullptr};
        UtKeys k = keysFrom(t1);
        check(utKeyFind(k, kUtKeySiteKey, VK_F3, C) == kUtKeyActButtonNext &&
                  utKeyFind(k, kUtKeySiteKey, VK_F3, 0) == kUtKeyActNone &&
                  utKeyFind(k, kUtKeySiteKey, VK_NEXT, C) == kUtKeyActRowsDown &&
                  utKeyFind(k, kUtKeySiteKey, VK_NEXT, 0) == kUtKeyActRowsDown,
              "button_next=Ctrl+F3: Ctrl+F3 steps, bare F3 nothing, Ctrl+PageDown is now rows_down's");
        const char* t2[kUtKeyActCount] = {"Shift+PageUp", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
        k = keysFrom(t2);
        check(utKeyFind(k, kUtKeySiteKey, VK_PRIOR, 0) == kUtKeyActNone &&
                  utKeyFind(k, kUtKeySiteKey, VK_PRIOR, S) == kUtKeyActRowsUp &&
                  utKeyFind(k, kUtKeySiteKey, VK_PRIOR, C) == kUtKeyActButtonPrev &&
                  utKeyFind(k, kUtKeySiteKey, VK_PRIOR, C | S) == kUtKeyActRowsUp,
              "a tie (Ctrl+Shift with Shift+PageUp and Ctrl+PageUp): the earlier action, rows_up");
        const char* t3[kUtKeyActCount] = {nullptr, nullptr, nullptr, nullptr, "Alt+Wheel", "Wheel", nullptr};
        k = keysFrom(t3);
        check(utKeyFind(k, kUtKeySiteWheel, 0, 0) == kUtKeyActButtonWheel &&
                  utKeyFind(k, kUtKeySiteWheel, 0, A) == kUtKeyActRowsWheel &&
                  utKeyFind(k, kUtKeySiteWheel, 0, C | A) == kUtKeyActRowsWheel,
              "rows_wheel=Alt+Wheel, button_wheel=Wheel: the plain wheel steps buttons, Alt+wheel rows");
        const char* t4[kUtKeyActCount] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "F8"};
        k = keysFrom(t4);
        check(utKeyFind(k, kUtKeySiteKey, VK_F8, 0) == kUtKeyActEditorOpen &&
                  utKeyFind(k, kUtKeySiteKey, VK_F8, C) == kUtKeyActEditorOpen &&
                  !utKeyClickMatches(k.editorOpen, 2, C) && utKeyFind(k, kUtKeySiteClick, 2, C) < 0,
              "editor_open=F8: the key site finds it, no click opens the editor any more");
        const char* t5[kUtKeyActCount] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "Alt+RightClick"};
        k = keysFrom(t5);
        check(utKeyClickMatches(k.editorOpen, 2, A) && !utKeyClickMatches(k.editorOpen, 2, C) &&
                  !utKeysWantSys(k),
              "editor_open=Alt+RightClick: Alt+right click opens, Ctrl+right click not; no SYSKEY needed");
        const char* t6[kUtKeyActCount] = {"Alt+PageUp", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
        const char* t7[kUtKeyActCount] = {nullptr, nullptr, nullptr, "F10", nullptr, nullptr, nullptr};
        const char* t8[kUtKeyActCount] = {nullptr, nullptr, nullptr, nullptr, "Alt+Wheel", nullptr, nullptr};
        const char* t9[kUtKeyActCount] = {"Alt+Mouse4", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
        check(utKeysWantSys(keysFrom(t6)) && utKeysWantSys(keysFrom(t7)) &&
                  !utKeysWantSys(keysFrom(t8)) && !utKeysWantSys(keysFrom(t9)) &&
                  utKeysWantButtons(keysFrom(t9)),
              "SYSKEY only for a KEY with Alt or F10 (not Alt+Wheel, not Alt+Mouse4)");
        k = keysFrom(t6);
        check(siteLive(k, WM_SYSKEYDOWN, VK_PRIOR, A) == kUtKeyActRowsUp &&
                  siteLive(k, WM_KEYDOWN, VK_PRIOR, 0) == kUtKeyActNone &&
                  siteLive(k, WM_SYSKEYDOWN, VK_NEXT, A) == kUtKeyActRowsDown,
              "rows_up=Alt+PageUp: Alt+PageUp (a SYSKEYDOWN) steps rows; Alt+PageDown is rows_down's");
        k = keysFrom(t9);
        check(siteLive(k, WM_XBUTTONDOWN, (unsigned long long)XBUTTON1 << 16, A) == kUtKeyActRowsUp &&
                  siteLive(k, WM_XBUTTONDOWN, (unsigned long long)XBUTTON1 << 16, 0) == kUtKeyActNone &&
                  siteLive(k, WM_XBUTTONDOWN, (unsigned long long)XBUTTON2 << 16, A) == kUtKeyActNone,
              "rows_up=Alt+Mouse4: the XBUTTON1 DOWN with Alt steps rows; Mouse5 nothing");
    }

    // ------------------------------------------------------------------------------------------
    printf("\n8. THE EQUIVALENCE: the defaults decide exactly what the window procedure did\n");
    {
        const UtKeys d;
        static const unsigned kMsgs[] = {WM_KEYDOWN, WM_SYSKEYDOWN, WM_KEYUP, WM_SYSKEYUP, WM_MOUSEWHEEL};
        long tried = 0, liveBad = 0, fallBad = 0;
        for (unsigned msg : kMsgs)
            for (int vk = 0; vk < 256; ++vk)
                for (int mods = 0; mods < 8; ++mods) {
                    for (int sign = 0; sign < 2; ++sign) {
                        unsigned long long wp = (unsigned long long)vk;
                        if (msg == WM_MOUSEWHEEL) {
                            const short delta = (short)(sign ? -WHEEL_DELTA * (1 + vk % 3) : WHEEL_DELTA * (1 + vk % 3));
                            wp = ((unsigned long long)(unsigned short)delta << 16) | (unsigned)(vk & 0x1F);
                        } else if (sign) {
                            continue;
                        }
                        ++tried;
                        if (siteLive(d, msg, wp, mods) != todayLive(msg, vk, mods)) {
                            if (liveBad < 5) printf("    live: msg %04x vk %02x mods %d\n", msg, vk, mods);
                            ++liveBad;
                        }
                        if (siteFallback(d, msg, wp, mods) != todayFallback(msg, vk, mods)) {
                            if (fallBad < 5) printf("    fallback: msg %04x vk %02x mods %d\n", msg, vk, mods);
                            ++fallBad;
                        }
                        if (msg == WM_MOUSEWHEEL) {
                            const UtKeyEvent ev = utKeyDecode(msg, wp, false);
                            const int today = GET_WHEEL_DELTA_WPARAM((WPARAM)wp) / WHEEL_DELTA;
                            if (ev.ticks != today) ++liveBad;
                        }
                    }
                }
        printf("    %ld inputs\n", tried);
        check(tried == 5 * 256 * 8 + 256 * 8 && liveBad == 0,
              "the live branch: every message x VK 0..255 x Ctrl/Shift/Alt (and the wheel's ticks) - identical");
        check(fallBad == 0, "the fallback branch (Ctrl + the wheel / PageUp / PageDown) - identical");

        // the KEYUP rule: the two-bit s_pageKeyEaten against the per-VK set, over a long run of
        // DOWNs and UPs of PageUp, PageDown and two keys no action has, the handler sometimes
        // refusing (inputAllowed false) - every UP's verdict must agree
        unsigned twoBit = 0;
        UtKeyEaten eaten = {};
        static const int kVks[4] = {VK_PRIOR, VK_NEXT, 'A', VK_F3};
        long ups = 0, swallowed = 0, upBad = 0;
        for (int i = 0; i < 20000; ++i) {
            const int vk = kVks[rnd() % 4];
            const int mods = (int)(rnd() % 8);
            const bool handlerTakes = (rnd() % 4) != 0;
            if (rnd() % 2) {
                // today: liveHandleKey consumes only PageUp / PageDown, when the handler takes it
                const bool todayConsumed = (vk == VK_PRIOR || vk == VK_NEXT) && handlerTakes;
                if (vk == VK_PRIOR || vk == VK_NEXT) {
                    const unsigned bit = vk == VK_PRIOR ? 1u : 2u;
                    twoBit = todayConsumed ? (twoBit | bit) : (twoBit & ~bit);
                }
                const int a = siteLive(d, WM_KEYDOWN, (unsigned long long)vk, mods);
                const bool consumed = a != kUtKeyActNone && handlerTakes;
                if (consumed != todayConsumed) ++upBad;
                utKeyEatDown(&eaten, vk, consumed);
            } else {
                bool todaySwallow = false;
                if (vk == VK_PRIOR || vk == VK_NEXT) {
                    const unsigned bit = vk == VK_PRIOR ? 1u : 2u;
                    todaySwallow = (twoBit & bit) != 0;
                    twoBit &= ~bit;
                }
                const UtKeyEvent ev = utKeyDecode(WM_KEYUP, (unsigned long long)vk, utKeysWantSys(d));
                const bool swallow = ev.what == kUtKeyEvUp && utKeyEatUp(&eaten, ev.code);
                if (swallow != todaySwallow) ++upBad;
                ++ups;
                if (swallow) ++swallowed;
            }
        }
        printf("    %ld UPs, %ld swallowed\n", ups, swallowed);
        check(upBad == 0 && swallowed > 1000 && swallowed < ups,
              "20000 random DOWNs / UPs: the per-VK set swallows exactly the UPs the two-bit rule did");
    }

    // ------------------------------------------------------------------------------------------
    printf("\n9. the eaten set\n");
    {
        UtKeyEaten e = {};
        bool ok = true;
        for (int vk = 0; vk < 256; ++vk) ok = ok && !utKeyEaten(e, vk) && !utKeyEatUp(&e, vk);
        check(ok, "nothing eaten at first: every UP passes");
        for (int vk = 0; vk < 256; ++vk) utKeyEatDown(&e, vk, (vk % 3) == 0);
        int set = 0;
        for (int vk = 0; vk < 256; ++vk) set += utKeyEaten(e, vk) ? 1 : 0;
        ok = set == 86;
        for (int vk = 0; vk < 256; ++vk) ok = ok && utKeyEatUp(&e, vk) == ((vk % 3) == 0);
        for (int vk = 0; vk < 256; ++vk) ok = ok && !utKeyEatUp(&e, vk);
        check(ok, "any VK 0..255: the UP after a consumed DOWN is swallowed once, a second UP passes");
        // a remapped key: button_next=Ctrl+F3, the Ctrl released before F3 goes up
        const char* t[kUtKeyActCount] = {nullptr, nullptr, nullptr, "Ctrl+F3", nullptr, nullptr, nullptr};
        const UtKeys k = keysFrom(t);
        const int a = siteLive(k, WM_KEYDOWN, VK_F3, kUtKeyModCtrl);
        utKeyEatDown(&e, VK_F3, a != kUtKeyActNone);
        const UtKeyEvent up = utKeyDecode(WM_KEYUP, VK_F3, utKeysWantSys(k));   // Ctrl is up now
        check(a == kUtKeyActButtonNext && up.what == kUtKeyEvUp && utKeyEatUp(&e, up.code),
              "Ctrl+F3 consumed, Ctrl released, then F3's KEYUP: still swallowed (per VK, not per chord)");
        utKeyEatDown(&e, VK_NEXT, true);
        utKeyEatDown(&e, VK_NEXT, false);   // an auto-repeat the handler let through
        check(!utKeyEatUp(&e, VK_NEXT), "a let-through DOWN after a consumed one clears the bit");
        utKeyEatDown(&e, VK_MBUTTON, true);
        const UtKeyEvent mup = utKeyDecode(WM_MBUTTONUP, 0, false);
        check(mup.what == kUtKeyEvUp && mup.code == VK_MBUTTON && utKeyEatUp(&e, mup.code),
              "a consumed middle button DOWN: its MBUTTONUP is swallowed the same way");
        utKeyEatDown(&e, -1, true);
        utKeyEatDown(&e, 256, true);
        utKeyEatDown(nullptr, 5, true);
        check(!utKeyEatUp(&e, -1) && !utKeyEatUp(&e, 256) && !utKeyEatUp(nullptr, 5),
              "a VK outside 0..255 or no set is never eaten");
    }

    // ------------------------------------------------------------------------------------------
    printf("\n10. the window-message decode\n");
    {
        UtKeyEvent ev = utKeyDecode(WM_KEYDOWN, VK_NEXT, false);
        bool ok = ev.what == kUtKeyEvDown && ev.site == kUtKeySiteKey && ev.code == VK_NEXT;
        ev = utKeyDecode(WM_KEYUP, VK_NEXT, false);
        ok = ok && ev.what == kUtKeyEvUp && ev.code == VK_NEXT;
        ev = utKeyDecode(WM_KEYDOWN, 0x1234521, false);   // only the low byte is the VK
        ok = ok && ev.code == 0x21;
        check(ok, "KEYDOWN / KEYUP: the VK");
        ok = utKeyDecode(WM_SYSKEYDOWN, VK_PRIOR, false).what == kUtKeyEvNone &&
             utKeyDecode(WM_SYSKEYUP, VK_PRIOR, false).what == kUtKeyEvNone &&
             utKeyDecode(WM_SYSKEYDOWN, VK_PRIOR, true).what == kUtKeyEvDown &&
             utKeyDecode(WM_SYSKEYUP, VK_F10, true).what == kUtKeyEvUp &&
             utKeyDecode(WM_SYSKEYUP, VK_F10, true).code == VK_F10;
        check(ok, "SYSKEYDOWN / SYSKEYUP only when wanted");
        ok = utKeyDecode(WM_MBUTTONDOWN, MK_MBUTTON, false).what == kUtKeyEvDown &&
             utKeyDecode(WM_MBUTTONDOWN, MK_MBUTTON, false).code == VK_MBUTTON &&
             utKeyDecode(WM_XBUTTONDOWN, ((unsigned long long)XBUTTON1 << 16) | MK_XBUTTON1, false).code == VK_XBUTTON1 &&
             utKeyDecode(WM_XBUTTONUP, (unsigned long long)XBUTTON2 << 16, false).what == kUtKeyEvUp &&
             utKeyDecode(WM_XBUTTONUP, (unsigned long long)XBUTTON2 << 16, false).code == VK_XBUTTON2 &&
             utKeyDecode(WM_XBUTTONDOWN, 3ull << 16, false).what == kUtKeyEvNone &&
             utKeyDecode(WM_XBUTTONDOWN, 0, false).what == kUtKeyEvNone;
        check(ok, "the middle and X buttons: their VKs (XBUTTON1 / 2 in the high word), anything else nothing");
        const auto wheel = [](int delta) {
            return utKeyDecode(WM_MOUSEWHEEL, ((unsigned long long)(unsigned short)(short)delta << 16) | MK_CONTROL, false);
        };
        ok = wheel(120).what == kUtKeyEvWheel && wheel(120).site == kUtKeySiteWheel &&
             wheel(120).ticks == 1 && wheel(-120).ticks == -1 && wheel(360).ticks == 3 &&
             wheel(-240).ticks == -2 && wheel(60).ticks == 0 && wheel(-119).ticks == 0 &&
             wheel(250).ticks == 2;
        check(ok, "the wheel: delta / WHEEL_DELTA, the sign kept, a sub-notch delta 0 (as before)");
        ok = utKeyDecode(WM_LBUTTONDOWN, 0, true).what == kUtKeyEvNone &&
             utKeyDecode(WM_RBUTTONDOWN, 0, true).what == kUtKeyEvNone &&
             utKeyDecode(WM_CHAR, 'a', true).what == kUtKeyEvNone &&
             utKeyDecode(WM_MOUSEMOVE, 0, true).what == kUtKeyEvNone;
        check(ok, "left / right buttons, WM_CHAR and a move are not [keys]' inputs");
    }

    // ------------------------------------------------------------------------------------------
    printf("\n11. the header's purity\n");
    {
        std::string text;
        FILE* f = nullptr;
        const std::string path = root + "\\src\\ut_keys.h";
        if (!fopen_s(&f, path.c_str(), "rb") && f) {
            char b[4096];
            size_t n;
            while ((n = fread(b, 1, sizeof(b), f)) > 0) text.append(b, n);
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
        check(text.find("GetKeyState(") == std::string::npos &&
                  text.find("GetAsyncKeyState(") == std::string::npos &&
                  text.find("extern ") == std::string::npos,
              "it reads no keyboard state and no setting: the callers hand it in");
    }

    printf("\n%d checks, %d failed - %s\n", g_checks, g_fails,
           g_fails ? "KEYS TESTS FAILED" : "KEYS TESTS PASSED");
    return g_fails ? 1 : 0;
}
