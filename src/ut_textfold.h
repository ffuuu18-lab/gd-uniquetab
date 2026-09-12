// ut_textfold.h - one case-folding rule for both sides of the tier-C name match.
//
// The engine hands `ReagentWindow::UpdateSearch` a needle it has
// already trimmed and LOWERCASED, as UTF-16; `catalogue.bin` holds display names
// as UTF-8. Rather than guess that both are plain ASCII, both are decoded to code points,
// lowercased by the SAME rule and re-encoded as UTF-8 - so the match itself is a plain `strstr`
// over two strings produced identically. No locale, no Windows call, no allocation at match time.
//
// Only ASCII is lowercased, and accents are neither folded nor stripped. That is deliberate, and
// it is what makes the "under-reports and never over-reports" promise PROVABLE rather than
// hoped-for - see `utFoldCp`.
//
// Pure: no Windows, no engine, no globals. `ut_plate.cpp` uses it and `tools\test_rowfold.cpp`
// links this very header.
#pragma once

#include <string>

namespace ut {

// Simple (1:1) lowercase over ASCII, and ASCII ONLY. Every other code point comes back unchanged.
//
// Lowercasing Latin-1 (0xC0..0xDE) and Latin Extended-A as well cannot be proved equivalent to
// the engine's own `ctype<wchar_t>::tolower` (exe 0x136200, applied to BOTH the needle and the
// item's rollover text). If the game's locale leaves a capital A-umlaut alone, a needle
// "a-umlaut" fails the ENGINE's match while passing a mod fold that folded it: an OVER-report,
// the one direction that must never happen.
//
// Restricted to ASCII the promise is provable. The engine's fold is this fold composed with a
// per-code-point map that leaves ASCII alone; applying the same length-preserving 1:1 map to both
// sides of a substring relation can only ever REMOVE matches, never add one. So "the mod matched"
// implies "the engine would have matched the same name", which is exactly what R4 asks.
//
// Nothing is lost on the shipped data: `catalogue.bin` carries no accented display name at all
// (measured - zero multi-byte UTF-8 sequences anywhere inside a name), so the Latin-1 rules could
// never have fired on the name side to begin with.
inline unsigned int utFoldCp(unsigned int c) {
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

inline void utAppendUtf8(std::string* out, unsigned int c) {
    if (c < 0x80) {
        out->push_back((char)c);
    } else if (c < 0x800) {
        out->push_back((char)(0xC0 | (c >> 6)));
        out->push_back((char)(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
        out->push_back((char)(0xE0 | (c >> 12)));
        out->push_back((char)(0x80 | ((c >> 6) & 0x3F)));
        out->push_back((char)(0x80 | (c & 0x3F)));
    } else {
        out->push_back((char)(0xF0 | (c >> 18)));
        out->push_back((char)(0x80 | ((c >> 12) & 0x3F)));
        out->push_back((char)(0x80 | ((c >> 6) & 0x3F)));
        out->push_back((char)(0x80 | (c & 0x3F)));
    }
}

// UTF-8 in, folded UTF-8 out. Malformed input is resynchronised, never looped on: the lead byte
// is always consumed before the continuation bytes are read.
inline void utFoldUtf8(const char* in, std::string* out) {
    out->clear();
    if (!in) return;
    const unsigned char* p = (const unsigned char*)in;
    while (*p) {
        unsigned int c = *p;
        int n = 0;
        if (c < 0x80) {
            n = 0;
        } else if ((c & 0xE0) == 0xC0) {
            c &= 0x1Fu;
            n = 1;
        } else if ((c & 0xF0) == 0xE0) {
            c &= 0x0Fu;
            n = 2;
        } else if ((c & 0xF8) == 0xF0) {
            c &= 0x07u;
            n = 3;
        } else {
            ++p;               // a stray continuation byte: drop it and resynchronise
            continue;
        }
        ++p;                   // past the lead byte, so the loop always advances
        bool bad = false;
        for (int i = 0; i < n; ++i) {
            if ((*p & 0xC0) != 0x80) {
                bad = true;
                break;
            }
            c = (c << 6) | (unsigned int)(*p & 0x3F);
            ++p;
        }
        if (bad) continue;
        utAppendUtf8(out, utFoldCp(c));
    }
}

// UTF-16 in (the engine's needle), folded UTF-8 out. Surrogate pairs are combined; a lone
// surrogate is passed through, which is harmless because it can only ever fail to match.
inline void utFoldWide(const wchar_t* in, int n, std::string* out) {
    out->clear();
    if (!in) return;
    for (int i = 0; i < n; ++i) {
        unsigned int c = (unsigned short)in[i];
        if (c >= 0xD800u && c <= 0xDBFFu && i + 1 < n) {
            const unsigned int lo = (unsigned short)in[i + 1];
            if (lo >= 0xDC00u && lo <= 0xDFFFu) {
                c = 0x10000u + ((c - 0xD800u) << 10) + (lo - 0xDC00u);
                ++i;
            }
        }
        utAppendUtf8(out, utFoldCp(c));
    }
}

}  // namespace ut
