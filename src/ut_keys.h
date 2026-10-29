// ut_keys.h - the [keys] section of the main uniquetab.ini, its pure half: the grammar, the parse
// of one line into a binding, every refusal and its reason, the canonical text, the conflict pass,
// the matcher the window procedure asks, the per-VK set of swallowed KEYUPs and the decode of the
// window messages it looks at. Standard headers only (no windows.h): the message numbers, the
// virtual-key codes and the engine's click types it needs are spelled out below, and
// tools\test_keys.cpp checks every one of them against windows.h.
//
// THE ACTIONS (UtKeyAction; the [keys] rows of ut_config.cpp, in this order - the order is the
// conflict pass's "later line"):
//   rows_up       PageUp            one window of rows up (a collection group)          key site
//   rows_down     PageDown          one window of rows down                             key site
//   button_prev   Ctrl+PageUp       the previous pad button (the materials page too)    key site
//   button_next   Ctrl+PageDown     the next pad button                                 key site
//   rows_wheel    Wheel             the wheel over the page: one row per notch          wheel site
//   button_wheel  Ctrl+Wheel        the wheel anywhere: one button per notch            wheel site
//   editor_open   Ctrl+RightClick   a click on the pad / a pinned label, or a key       click / key
// The defaults are the behaviour before [keys] existed, input for input (test_keys proves it over
// every message x VK x modifier).
//
// THE GRAMMAR (case-insensitive, blanks around '+' allowed):
//   value    := "" (the default) | "none" | chord
//   chord    := { modifier "+" } key            each modifier at most once, the key last
//   modifier := Ctrl | Control | Shift | Alt
//   key      := A..Z | 0..9 | F1..F24 | PageUp | PgUp | PageDown | PgDn | Home | End | Insert |
//               Ins | Delete | Del | Up | Down | Left | Right | Space | Tab | Enter | Return |
//               Backspace | Num0..Num9 | NumMul | NumPlus | NumMinus | NumDot | NumDiv |
//               Wheel | RightClick | MiddleClick | Mouse4 | Mouse5     (LeftClick and Esc are
//               words it knows only to refuse them)
// REFUSED (utKeyParse returns the reason, a static noun phrase the caller logs as
// "<key>=<value> is <reason>"): an unknown word, a modifier twice, two keys, a key before a
// modifier, a '+' with nothing on one side, modifiers without a key, `none` with anything else;
// the wrong kind for the action (a step takes a key or MiddleClick / Mouse4 / Mouse5, a wheel row
// takes Wheel, editor_open takes RightClick or a key); LeftClick anywhere; MiddleClick for
// editor_open (this release); a TEXT key (a letter, a digit, Space, Enter, Tab, Backspace, a
// numpad digit or sign) without Ctrl or Alt - the stash search box would get it too; Esc with
// anything; Alt with F4, Tab or Space.
//
// THE BINDING. UtKeyBind {kind, code, mods}: kind key (code = the VK), button (a mouse button
// bound to a step: code = VK_MBUTTON 0x04 / VK_XBUTTON1 0x05 / VK_XBUTTON2 0x06 - the window
// messages WM_MBUTTONDOWN and WM_XBUTTONDOWN XBUTTON1 / XBUTTON2, decoded to those VKs by
// utKeyDecode, so the eaten set covers them too), wheel (code 0), click (editor_open on the pad:
// code = the engine's MouseEvent DOWN type, RightClick 2), none (the action is off). mods = bit 0
// Ctrl, bit 1 Shift, bit 2 Alt.
//
// THE MATCH (utKeyFind). Among the actions of the site whose code matches and whose modifiers are
// ALL held, the one with the MOST modifiers wins; a tie (possible only between two different
// chords both held, e.g. Ctrl+PageUp and Shift+PageUp with Ctrl+Shift down) goes to the earlier
// action in table order. The conflict pass (utKeysDerive) has already turned the later of two
// IDENTICAL chords of one site into none. A click (utKeyClickMatches) follows the same rule:
// its modifiers all held, extra ones allowed - so Ctrl+Shift+RightClick still opens the editor
// with the default, as before.
//
// THE KEYUP RULE (UtKeyEaten): a consumed KEYDOWN (or button DOWN) sets its VK's bit, a not
// consumed one clears it; the UP of that VK is swallowed only when its bit is set, and clears it.
// Per VK, not per chord: a modifier released in between does not matter.
//
// SYSKEY. WM_SYSKEYDOWN / WM_SYSKEYUP (what Windows sends while Alt is held, and for F10) are
// decoded only when utKeysWantSys says some key-site action names Alt or F10; otherwise they are
// not looked at, as before [keys].
#pragma once

#include <stddef.h>
#include <string.h>

// ---- the numbers this header speaks (windows.h's, checked in tools\test_keys.cpp) ------------
enum {
    kUtKeyMsgKeyDown = 0x0100,      // WM_KEYDOWN
    kUtKeyMsgKeyUp = 0x0101,        // WM_KEYUP
    kUtKeyMsgSysKeyDown = 0x0104,   // WM_SYSKEYDOWN
    kUtKeyMsgSysKeyUp = 0x0105,     // WM_SYSKEYUP
    kUtKeyMsgMButtonDown = 0x0207,  // WM_MBUTTONDOWN
    kUtKeyMsgMButtonUp = 0x0208,    // WM_MBUTTONUP
    kUtKeyMsgMouseWheel = 0x020A,   // WM_MOUSEWHEEL
    kUtKeyMsgXButtonDown = 0x020B,  // WM_XBUTTONDOWN
    kUtKeyMsgXButtonUp = 0x020C,    // WM_XBUTTONUP
    kUtKeyWheelDelta = 120          // WHEEL_DELTA
};
enum {
    kUtVkMButton = 0x04, kUtVkXButton1 = 0x05, kUtVkXButton2 = 0x06,
    kUtVkBack = 0x08, kUtVkTab = 0x09, kUtVkReturn = 0x0D, kUtVkEscape = 0x1B,
    kUtVkSpace = 0x20, kUtVkPrior = 0x21, kUtVkNext = 0x22, kUtVkEnd = 0x23, kUtVkHome = 0x24,
    kUtVkLeft = 0x25, kUtVkUp = 0x26, kUtVkRight = 0x27, kUtVkDown = 0x28,
    kUtVkInsert = 0x2D, kUtVkDelete = 0x2E,
    kUtVkNumpad0 = 0x60, kUtVkMultiply = 0x6A, kUtVkAdd = 0x6B, kUtVkSubtract = 0x6D,
    kUtVkDecimal = 0x6E, kUtVkDivide = 0x6F,
    kUtVkF1 = 0x70, kUtVkF4 = 0x73, kUtVkF10 = 0x79, kUtVkF24 = 0x87
};
// The engine's MouseEvent DOWN types (DirectInputDevice::Update): 1 left, 2 right, 3 middle.
enum { kUtKeyClickLeft = 1, kUtKeyClickRight = 2, kUtKeyClickMiddle = 3 };

// ---- the model -------------------------------------------------------------------------------
enum UtKeyKind {
    kUtKeyNone = 0,   // "none": the action is off
    kUtKeyKey,        // a keyboard key: code = its VK
    kUtKeyButton,     // a mouse button bound to a step: code = kUtVkMButton / kUtVkXButton1 / 2
    kUtKeyWheel,      // the mouse wheel: code 0
    kUtKeyClick       // editor_open's engine click: code = kUtKeyClickRight
};
enum { kUtKeyModCtrl = 1, kUtKeyModShift = 2, kUtKeyModAlt = 4 };

enum UtKeyAction {
    kUtKeyActNone = -1,
    kUtKeyActRowsUp = 0,
    kUtKeyActRowsDown,
    kUtKeyActButtonPrev,
    kUtKeyActButtonNext,
    kUtKeyActRowsWheel,
    kUtKeyActButtonWheel,
    kUtKeyActEditorOpen,
    kUtKeyActCount
};

// Where an input comes from: a key or mouse-button DOWN of the window procedure, the wheel, or the
// engine's click on the pad.
enum UtKeySite { kUtKeySiteKey = 0, kUtKeySiteWheel, kUtKeySiteClick };

struct UtKeyBind {
    int kind;   // UtKeyKind
    int code;   // see UtKeyKind
    int mods;   // kUtKeyMod* bits
};

// The seven bindings in force - a POD inside UtConfig (g_cfg.keys), derived from the [keys] rows
// at the parse. The defaults ARE the behaviour before [keys].
struct UtKeys {
    UtKeyBind rowsUp = {kUtKeyKey, kUtVkPrior, 0};
    UtKeyBind rowsDown = {kUtKeyKey, kUtVkNext, 0};
    UtKeyBind buttonPrev = {kUtKeyKey, kUtVkPrior, kUtKeyModCtrl};
    UtKeyBind buttonNext = {kUtKeyKey, kUtVkNext, kUtKeyModCtrl};
    UtKeyBind rowsWheel = {kUtKeyWheel, 0, 0};
    UtKeyBind buttonWheel = {kUtKeyWheel, 0, kUtKeyModCtrl};
    UtKeyBind editorOpen = {kUtKeyClick, kUtKeyClickRight, kUtKeyModCtrl};
};

// One action's [keys] row name and default text.
struct UtKeyActionInfo {
    const char* name;   // "rows_up"
    const char* def;    // "PageUp"
};
static const UtKeyActionInfo kUtKeyActions[kUtKeyActCount] = {
    {"rows_up", "PageUp"},          {"rows_down", "PageDown"},
    {"button_prev", "Ctrl+PageUp"}, {"button_next", "Ctrl+PageDown"},
    {"rows_wheel", "Wheel"},        {"button_wheel", "Ctrl+Wheel"},
    {"editor_open", "Ctrl+RightClick"},
};

inline bool utKeyBindSame(const UtKeyBind& a, const UtKeyBind& b) {
    return a.kind == b.kind && a.code == b.code && a.mods == b.mods;
}

inline UtKeyBind* utKeyBindOf(UtKeys* k, int action) {
    switch (action) {
        case kUtKeyActRowsUp: return &k->rowsUp;
        case kUtKeyActRowsDown: return &k->rowsDown;
        case kUtKeyActButtonPrev: return &k->buttonPrev;
        case kUtKeyActButtonNext: return &k->buttonNext;
        case kUtKeyActRowsWheel: return &k->rowsWheel;
        case kUtKeyActButtonWheel: return &k->buttonWheel;
        case kUtKeyActEditorOpen: return &k->editorOpen;
        default: return nullptr;
    }
}
inline const UtKeyBind* utKeyBindOf(const UtKeys& k, int action) {
    return utKeyBindOf(const_cast<UtKeys*>(&k), action);
}

// The action whose [keys] row is `name` (case-insensitive), or kUtKeyActNone.
inline int utKeyActionOf(const char* name) {
    if (!name) return kUtKeyActNone;
    for (int a = 0; a < kUtKeyActCount; ++a) {
        const char* p = kUtKeyActions[a].name;
        const char* q = name;
        while (*p && *q) {
            char c = *q;
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (c != *p) break;
            ++p;
            ++q;
        }
        if (!*p && !*q) return a;
    }
    return kUtKeyActNone;
}

// The site a binding's input arrives at: key and button kinds the key site, the wheel the wheel
// site, a click the click site; -1 for none.
inline int utKeySiteOf(const UtKeyBind& b) {
    switch (b.kind) {
        case kUtKeyKey:
        case kUtKeyButton: return kUtKeySiteKey;
        case kUtKeyWheel: return kUtKeySiteWheel;
        case kUtKeyClick: return kUtKeySiteClick;
        default: return -1;
    }
}

inline int utKeyModsOf(bool ctrl, bool shift, bool alt) {
    return (ctrl ? kUtKeyModCtrl : 0) | (shift ? kUtKeyModShift : 0) | (alt ? kUtKeyModAlt : 0);
}
inline int utKeyModCount(int mods) {
    return ((mods & kUtKeyModCtrl) ? 1 : 0) + ((mods & kUtKeyModShift) ? 1 : 0) +
           ((mods & kUtKeyModAlt) ? 1 : 0);
}

// ---- the refusals' words (static; the caller logs "<key>=<value> is <reason> - ignored") ----
#define UT_KEY_WHY_UNKNOWN "not a key this build knows (Ctrl, Shift or Alt and one key, joined by '+', or none)"
#define UT_KEY_WHY_TOO_LONG "too long to be a key"
#define UT_KEY_WHY_MOD_TWICE "a modifier given twice"
#define UT_KEY_WHY_TWO_KEYS "two keys in one chord - one key, with Ctrl, Shift or Alt in front"
#define UT_KEY_WHY_KEY_FIRST "a key before a modifier - the key goes last (Ctrl+PageUp)"
#define UT_KEY_WHY_EMPTY_PART "a '+' with nothing on one side"
#define UT_KEY_WHY_NO_KEY "only modifiers - a key has to follow them"
#define UT_KEY_WHY_NONE_ALONE "none together with something else - none stands alone"
#define UT_KEY_WHY_KIND_STEP "not a key or a mouse button - this one takes a key, MiddleClick, Mouse4 or Mouse5"
#define UT_KEY_WHY_KIND_WHEEL "not the wheel - this one takes Wheel, with Ctrl, Shift or Alt if you like"
#define UT_KEY_WHY_KIND_EDITOR "not RightClick or a key - this one takes one of those"
#define UT_KEY_WHY_LEFT "the left click, which is the buttons' own"
#define UT_KEY_WHY_MIDDLE_EDITOR "MiddleClick, which cannot open the editor in this release - use RightClick or a key"
#define UT_KEY_WHY_TEXT "a key that would also type into the stash search box - add Ctrl or Alt"
#define UT_KEY_WHY_ESC "Esc, the game's own close key"
#define UT_KEY_WHY_WINDOWS "a Windows shortcut (Alt+F4, Alt+Tab, Alt+Space)"

namespace utkeys_detail {

enum WordClass {
    kWordUnknown = 0,
    kWordMod,      // code = the kUtKeyMod* bit
    kWordNone,
    kWordKey,      // code = the VK; text = the stash search box gets it
    kWordWheel,
    kWordRight,
    kWordMiddle,
    kWordMouse4,
    kWordMouse5,
    kWordLeft,
    kWordEsc
};

struct Word {
    int cls;
    int code;
    bool text;
};

inline char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

inline bool eqi(const char* a, size_t n, const char* b) {
    size_t i = 0;
    for (; i < n && b[i]; ++i)
        if (lower(a[i]) != lower(b[i])) return false;
    return i == n && b[i] == 0;
}

struct NamedWord {
    const char* name;
    int cls;
    int code;
    bool text;
};
static const NamedWord kNamed[] = {
    {"ctrl", kWordMod, kUtKeyModCtrl, false},     {"control", kWordMod, kUtKeyModCtrl, false},
    {"shift", kWordMod, kUtKeyModShift, false},   {"alt", kWordMod, kUtKeyModAlt, false},
    {"none", kWordNone, 0, false},
    {"pageup", kWordKey, kUtVkPrior, false},      {"pgup", kWordKey, kUtVkPrior, false},
    {"pagedown", kWordKey, kUtVkNext, false},     {"pgdn", kWordKey, kUtVkNext, false},
    {"home", kWordKey, kUtVkHome, false},         {"end", kWordKey, kUtVkEnd, false},
    {"insert", kWordKey, kUtVkInsert, false},     {"ins", kWordKey, kUtVkInsert, false},
    {"delete", kWordKey, kUtVkDelete, false},     {"del", kWordKey, kUtVkDelete, false},
    {"up", kWordKey, kUtVkUp, false},             {"down", kWordKey, kUtVkDown, false},
    {"left", kWordKey, kUtVkLeft, false},         {"right", kWordKey, kUtVkRight, false},
    {"space", kWordKey, kUtVkSpace, true},        {"tab", kWordKey, kUtVkTab, true},
    {"enter", kWordKey, kUtVkReturn, true},       {"return", kWordKey, kUtVkReturn, true},
    {"backspace", kWordKey, kUtVkBack, true},
    {"nummul", kWordKey, kUtVkMultiply, true},    {"numplus", kWordKey, kUtVkAdd, true},
    {"numminus", kWordKey, kUtVkSubtract, true},  {"numdot", kWordKey, kUtVkDecimal, true},
    {"numdiv", kWordKey, kUtVkDivide, true},
    {"wheel", kWordWheel, 0, false},
    {"rightclick", kWordRight, kUtKeyClickRight, false},
    {"middleclick", kWordMiddle, kUtVkMButton, false},
    {"mouse4", kWordMouse4, kUtVkXButton1, false},
    {"mouse5", kWordMouse5, kUtVkXButton2, false},
    {"leftclick", kWordLeft, kUtKeyClickLeft, false},
    {"esc", kWordEsc, kUtVkEscape, false},        {"escape", kWordEsc, kUtVkEscape, false},
};

// One trimmed word (not NUL-terminated: `n` bytes).
inline Word wordOf(const char* s, size_t n) {
    Word w = {kWordUnknown, 0, false};
    if (n == 1) {
        const char c = lower(s[0]);
        if (c >= 'a' && c <= 'z') return Word{kWordKey, 'A' + (c - 'a'), true};
        if (c >= '0' && c <= '9') return Word{kWordKey, c, true};
        return w;
    }
    for (const NamedWord& nw : kNamed)
        if (eqi(s, n, nw.name)) return Word{nw.cls, nw.code, nw.text};
    // F1..F24
    if (lower(s[0]) == 'f' && n >= 2 && n <= 3) {
        int v = 0;
        for (size_t i = 1; i < n; ++i) {
            if (s[i] < '0' || s[i] > '9') return w;
            v = v * 10 + (s[i] - '0');
        }
        if (s[1] == '0') return w;   // F01 is not a name
        if (v >= 1 && v <= 24) return Word{kWordKey, kUtVkF1 + v - 1, false};
        return w;
    }
    // Num0..Num9
    if (n == 4 && lower(s[0]) == 'n' && lower(s[1]) == 'u' && lower(s[2]) == 'm' && s[3] >= '0' &&
        s[3] <= '9')
        return Word{kWordKey, kUtVkNumpad0 + (s[3] - '0'), true};
    return w;
}

inline bool blank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

}  // namespace utkeys_detail

// Parses one [keys] value for `action` into `out`. Null = legal (`out` written); else the reason
// it is refused (static text) and `out` untouched. "" (or only blanks) is the action's default.
inline const char* utKeyParse(const char* text, int action, UtKeyBind* out) {
    using namespace utkeys_detail;
    if (action < 0 || action >= kUtKeyActCount) return UT_KEY_WHY_UNKNOWN;
    const char* s = text ? text : "";
    while (blank(*s)) ++s;
    if (!*s) s = kUtKeyActions[action].def;
    if (strlen(s) > 64) return UT_KEY_WHY_TOO_LONG;
    // split at '+', each part trimmed
    enum { kMaxParts = 8 };
    Word words[kMaxParts];
    int n = 0;
    const char* p = s;
    for (;;) {
        const char* e = p;
        while (*e && *e != '+') ++e;
        const char* a = p;
        const char* b = e;
        while (a < b && blank(*a)) ++a;
        while (b > a && blank(b[-1])) --b;
        if (a == b) {
            // "Ctrl+" / "+A" / "Ctrl++A"; a value that is blank throughout was handled above
            return UT_KEY_WHY_EMPTY_PART;
        }
        if (n >= kMaxParts) return UT_KEY_WHY_TWO_KEYS;
        words[n++] = wordOf(a, (size_t)(b - a));
        if (!*e) break;
        p = e + 1;
    }
    int mods = 0, keys = 0, keyAt = -1;
    bool none = false;
    for (int i = 0; i < n; ++i) {
        const Word& w = words[i];
        if (w.cls == kWordUnknown) return UT_KEY_WHY_UNKNOWN;
        if (w.cls == kWordNone) none = true;
    }
    if (none) {
        if (n > 1) return UT_KEY_WHY_NONE_ALONE;
        out->kind = kUtKeyNone;
        out->code = 0;
        out->mods = 0;
        return nullptr;
    }
    for (int i = 0; i < n; ++i) {
        const Word& w = words[i];
        if (w.cls == kWordMod) {
            if (mods & w.code) return UT_KEY_WHY_MOD_TWICE;
            mods |= w.code;
        } else {
            ++keys;
            if (keyAt < 0) keyAt = i;
        }
    }
    if (keys == 0) return UT_KEY_WHY_NO_KEY;
    if (keys > 1) return UT_KEY_WHY_TWO_KEYS;
    if (keyAt != n - 1) return UT_KEY_WHY_KEY_FIRST;
    const Word& k = words[keyAt];
    if (k.cls == kWordLeft) return UT_KEY_WHY_LEFT;
    if (k.cls == kWordEsc) return UT_KEY_WHY_ESC;

    UtKeyBind b = {kUtKeyNone, 0, mods};
    const bool step = action <= kUtKeyActButtonNext;
    const bool wheel = action == kUtKeyActRowsWheel || action == kUtKeyActButtonWheel;
    if (step) {
        if (k.cls == kWordKey) {
            b.kind = kUtKeyKey;
            b.code = k.code;
        } else if (k.cls == kWordMiddle || k.cls == kWordMouse4 || k.cls == kWordMouse5) {
            b.kind = kUtKeyButton;
            b.code = k.code;
        } else {
            return UT_KEY_WHY_KIND_STEP;
        }
    } else if (wheel) {
        if (k.cls != kWordWheel) return UT_KEY_WHY_KIND_WHEEL;
        b.kind = kUtKeyWheel;
        b.code = 0;
    } else {   // editor_open
        if (k.cls == kWordRight) {
            b.kind = kUtKeyClick;
            b.code = kUtKeyClickRight;
        } else if (k.cls == kWordMiddle) {
            return UT_KEY_WHY_MIDDLE_EDITOR;
        } else if (k.cls == kWordKey) {
            b.kind = kUtKeyKey;
            b.code = k.code;
        } else {
            return UT_KEY_WHY_KIND_EDITOR;
        }
    }
    if (b.kind == kUtKeyKey) {
        if ((mods & kUtKeyModAlt) &&
            (b.code == kUtVkF4 || b.code == kUtVkTab || b.code == kUtVkSpace))
            return UT_KEY_WHY_WINDOWS;
        if (k.text && !(mods & (kUtKeyModCtrl | kUtKeyModAlt))) return UT_KEY_WHY_TEXT;
    }
    *out = b;
    return nullptr;
}

// The canonical name of a binding's key part ("PageUp", "A", "F5", "Wheel", "RightClick"), or
// null for none or a code no word names.
inline const char* utKeyCodeName(const UtKeyBind& b, char* tmp, size_t cap) {
    using namespace utkeys_detail;
    if (!tmp || cap < 8) return nullptr;
    switch (b.kind) {
        case kUtKeyWheel: return "Wheel";
        case kUtKeyClick:
            return b.code == kUtKeyClickRight ? "RightClick"
                                              : (b.code == kUtKeyClickMiddle ? "MiddleClick" : nullptr);
        case kUtKeyButton:
            if (b.code == kUtVkMButton) return "MiddleClick";
            if (b.code == kUtVkXButton1) return "Mouse4";
            if (b.code == kUtVkXButton2) return "Mouse5";
            return nullptr;
        case kUtKeyKey: break;
        default: return nullptr;
    }
    const int c = b.code;
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
        tmp[0] = (char)c;
        tmp[1] = 0;
        return tmp;
    }
    if (c >= kUtVkF1 && c <= kUtVkF24) {
        const int f = c - kUtVkF1 + 1;
        int i = 0;
        tmp[i++] = 'F';
        if (f >= 10) tmp[i++] = (char)('0' + f / 10);
        tmp[i++] = (char)('0' + f % 10);
        tmp[i] = 0;
        return tmp;
    }
    if (c >= kUtVkNumpad0 && c <= kUtVkNumpad0 + 9) {
        tmp[0] = 'N';
        tmp[1] = 'u';
        tmp[2] = 'm';
        tmp[3] = (char)('0' + (c - kUtVkNumpad0));
        tmp[4] = 0;
        return tmp;
    }
    switch (c) {
        case kUtVkPrior: return "PageUp";
        case kUtVkNext: return "PageDown";
        case kUtVkHome: return "Home";
        case kUtVkEnd: return "End";
        case kUtVkInsert: return "Insert";
        case kUtVkDelete: return "Delete";
        case kUtVkUp: return "Up";
        case kUtVkDown: return "Down";
        case kUtVkLeft: return "Left";
        case kUtVkRight: return "Right";
        case kUtVkSpace: return "Space";
        case kUtVkTab: return "Tab";
        case kUtVkReturn: return "Enter";
        case kUtVkBack: return "Backspace";
        case kUtVkMultiply: return "NumMul";
        case kUtVkAdd: return "NumPlus";
        case kUtVkSubtract: return "NumMinus";
        case kUtVkDecimal: return "NumDot";
        case kUtVkDivide: return "NumDiv";
        default: return nullptr;
    }
}

// The canonical text of a binding: "none", or the modifiers in the order Ctrl, Shift, Alt and the
// key's canonical name, joined by '+' ("Ctrl+Shift+PageUp"). Parsing it gives the binding back.
// Writes "?" for a binding no word names. Returns `out`.
inline const char* utKeyText(const UtKeyBind& b, char* out, size_t cap) {
    if (!out || cap == 0) return out;
    out[0] = 0;
    if (cap < 40) return out;   // the longest is "Ctrl+Shift+Alt+MiddleClick" (26)
    if (b.kind == kUtKeyNone) {
        memcpy(out, "none", 5);
        return out;
    }
    char tmp[8];
    const char* name = utKeyCodeName(b, tmp, sizeof(tmp));
    size_t n = 0;
    auto put = [&](const char* s) {
        const size_t len = strlen(s);
        if (n + len + 1 > cap) return;
        memcpy(out + n, s, len);
        n += len;
        out[n] = 0;
    };
    if (b.mods & kUtKeyModCtrl) put("Ctrl+");
    if (b.mods & kUtKeyModShift) put("Shift+");
    if (b.mods & kUtKeyModAlt) put("Alt+");
    put(name ? name : "?");
    return out;
}

// ---- the conflict pass and the derivation -----------------------------------------------------
// Two actions of one site with the IDENTICAL chord (kind, code and modifiers): the LATER in table
// order becomes none and the pair is reported (`earlier`, `later`), so the caller logs one WARN
// naming both. Returns the number of pairs (at most kUtKeyActCount).
struct UtKeyConflict {
    int earlier;
    int later;
    UtKeyBind chord;   // what both said
};

inline int utKeysResolveConflicts(UtKeys* k, UtKeyConflict* out, int cap) {
    int found = 0;
    for (int j = 1; j < kUtKeyActCount; ++j) {
        UtKeyBind* bj = utKeyBindOf(k, j);
        if (bj->kind == kUtKeyNone) continue;
        for (int i = 0; i < j; ++i) {
            const UtKeyBind* bi = utKeyBindOf(k, i);
            if (bi->kind == kUtKeyNone || !utKeyBindSame(*bi, *bj)) continue;
            if (out && found < cap) out[found] = UtKeyConflict{i, j, *bj};
            ++found;
            bj->kind = kUtKeyNone;
            bj->code = 0;
            bj->mods = 0;
            break;
        }
    }
    return found;
}

// The seven texts (in UtKeyAction order; null or "" = the default) -> the bindings in force: a
// text utKeyParse refuses keeps that action's DEFAULT (the caller has already logged the refusal
// when it read the line), then the conflict pass. `conflicts` (optional, kUtKeyActCount entries)
// and the return value as utKeysResolveConflicts.
inline int utKeysDerive(const char* const* texts, UtKeys* out, UtKeyConflict* conflicts, int cap) {
    UtKeys k;   // the defaults
    for (int a = 0; a < kUtKeyActCount; ++a) {
        UtKeyBind b;
        if (!utKeyParse(texts ? texts[a] : nullptr, a, &b)) *utKeyBindOf(&k, a) = b;
    }
    const int n = utKeysResolveConflicts(&k, conflicts, cap);
    *out = k;
    return n;
}

// ---- the match --------------------------------------------------------------------------------
// The action an input asks for, or kUtKeyActNone: `site` kUtKeySiteKey with `code` = the VK of a
// KEYDOWN (or utKeyDecode's VK of a button DOWN), kUtKeySiteWheel (`code` ignored),
// kUtKeySiteClick with `code` = the engine's DOWN type; `mods` = the modifiers held. Among the
// actions of the site whose code matches and whose modifiers are all held, the most modifiers
// wins, a tie the earlier action.
inline int utKeyFind(const UtKeys& k, int site, int code, int mods) {
    int best = kUtKeyActNone, bestCount = -1;
    for (int a = 0; a < kUtKeyActCount; ++a) {
        const UtKeyBind& b = *utKeyBindOf(k, a);
        if (utKeySiteOf(b) != site) continue;
        if (site != kUtKeySiteWheel && b.code != code) continue;
        if ((b.mods & ~mods) != 0) continue;
        const int c = utKeyModCount(b.mods);
        if (c > bestCount) {
            best = a;
            bestCount = c;
        }
    }
    return best;
}

// editor_open's click: the engine's DOWN `type` with the modifiers `mods` held. True when
// editor_open is a click of that type and all of its modifiers are held (extra ones allowed).
inline bool utKeyClickMatches(const UtKeyBind& b, int type, int mods) {
    return b.kind == kUtKeyClick && b.code == type && (b.mods & ~mods) == 0;
}

// True when some action bound to a KEY names Alt, or is F10: only then are WM_SYSKEYDOWN /
// WM_SYSKEYUP decoded at all.
inline bool utKeysWantSys(const UtKeys& k) {
    for (int a = 0; a < kUtKeyActCount; ++a) {
        const UtKeyBind& b = *utKeyBindOf(k, a);
        if (b.kind != kUtKeyKey) continue;   // a mouse button's messages are never SYS ones
        if ((b.mods & kUtKeyModAlt) || b.code == kUtVkF10) return true;
    }
    return false;
}

// True when some action is bound to a mouse button (kind button): only then do the button
// messages need looking at (utKeyDecode decodes them regardless; utKeyFind finds nothing).
inline bool utKeysWantButtons(const UtKeys& k) {
    for (int a = 0; a < kUtKeyActCount; ++a)
        if (utKeyBindOf(k, a)->kind == kUtKeyButton) return true;
    return false;
}

// ---- the window messages ----------------------------------------------------------------------
enum UtKeyEventWhat {
    kUtKeyEvNone = 0,   // not an input [keys] looks at
    kUtKeyEvDown,       // a key or button DOWN: site key, code = its VK
    kUtKeyEvUp,         // its UP: code = the VK (the eaten set decides)
    kUtKeyEvWheel       // site wheel, `ticks` = delta / WHEEL_DELTA (0 for a sub-notch delta)
};
struct UtKeyEvent {
    int what;    // UtKeyEventWhat
    int site;    // UtKeySite (key or wheel)
    int code;    // the VK (0..255) for a DOWN / UP
    int ticks;   // the wheel's whole notches, + = away from the user
};

// One window message -> what it is to [keys]. `wParam` as the window procedure got it.
// WM_SYSKEYDOWN / WM_SYSKEYUP only when `wantSys` (utKeysWantSys).
inline UtKeyEvent utKeyDecode(unsigned int msg, unsigned long long wParam, bool wantSys) {
    UtKeyEvent ev = {kUtKeyEvNone, kUtKeySiteKey, 0, 0};
    const int hi = (int)((wParam >> 16) & 0xFFFF);
    switch (msg) {
        case kUtKeyMsgSysKeyDown:
            if (!wantSys) return ev;
            ev.what = kUtKeyEvDown;
            ev.code = (int)(wParam & 0xFF);
            return ev;
        case kUtKeyMsgKeyDown:
            ev.what = kUtKeyEvDown;
            ev.code = (int)(wParam & 0xFF);
            return ev;
        case kUtKeyMsgSysKeyUp:
            if (!wantSys) return ev;
            ev.what = kUtKeyEvUp;
            ev.code = (int)(wParam & 0xFF);
            return ev;
        case kUtKeyMsgKeyUp:
            ev.what = kUtKeyEvUp;
            ev.code = (int)(wParam & 0xFF);
            return ev;
        case kUtKeyMsgMButtonDown:
        case kUtKeyMsgMButtonUp:
            ev.what = msg == kUtKeyMsgMButtonDown ? kUtKeyEvDown : kUtKeyEvUp;
            ev.code = kUtVkMButton;
            return ev;
        case kUtKeyMsgXButtonDown:
        case kUtKeyMsgXButtonUp:
            if (hi != 1 && hi != 2) return ev;
            ev.what = msg == kUtKeyMsgXButtonDown ? kUtKeyEvDown : kUtKeyEvUp;
            ev.code = hi == 1 ? kUtVkXButton1 : kUtVkXButton2;
            return ev;
        case kUtKeyMsgMouseWheel: {
            const short delta = (short)(unsigned short)hi;
            ev.what = kUtKeyEvWheel;
            ev.site = kUtKeySiteWheel;
            ev.ticks = (int)delta / kUtKeyWheelDelta;
            return ev;
        }
        default: return ev;
    }
}

// ---- the KEYUP rule, per VK ---------------------------------------------------------------------
// The window procedure's thread only. Zero-initialised = nothing eaten.
struct UtKeyEaten {
    unsigned int bits[8];
};
// A KEYDOWN / button DOWN of `vk` was consumed (`consumed`) or let through.
inline void utKeyEatDown(UtKeyEaten* s, int vk, bool consumed) {
    if (!s || vk < 0 || vk > 255) return;
    const unsigned int bit = 1u << (vk & 31);
    if (consumed) s->bits[vk >> 5] |= bit;
    else s->bits[vk >> 5] &= ~bit;
}
// The UP of `vk`: true = swallow it (its DOWN was consumed). Clears the bit either way.
inline bool utKeyEatUp(UtKeyEaten* s, int vk) {
    if (!s || vk < 0 || vk > 255) return false;
    const unsigned int bit = 1u << (vk & 31);
    const bool eaten = (s->bits[vk >> 5] & bit) != 0;
    s->bits[vk >> 5] &= ~bit;
    return eaten;
}
inline bool utKeyEaten(const UtKeyEaten& s, int vk) {
    return vk >= 0 && vk <= 255 && (s.bits[vk >> 5] & (1u << (vk & 31))) != 0;
}
