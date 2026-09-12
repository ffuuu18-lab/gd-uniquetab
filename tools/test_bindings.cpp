// test_bindings.cpp - the OFFLINE proof behind every binding the mod locates for itself.
// NO GAME IS LAUNCHED AND NOTHING IS WRITTEN. Two read-only inputs, both optional, both named by
// an environment variable, and each half SKIPS LOUDLY when its input is not there:
//
//   UNIQUETAB_TEST_EXE_IMAGE   the DECRYPTED exe image (the exe's .text is Steam-DRM encrypted on
//                              disk, so the bytes in the shipped file are not the bytes that run).
//                              Every exe signature must match EXACTLY the number of times it is
//                              declared to match, at the rva the dig recorded.
//   UNIQUETAB_TEST_GAME_DIR    the installed game folder. Game.dll and Engine.dll are plain on
//                              disk, so the DECODERS - the offsets the mod reads out of an
//                              exported function's own instruction bytes - can be re-run here and
//                              checked against the offsets the digs recorded. Opened READ ONLY.
//
// THE PATTERNS ARE NOT COPIED INTO THIS FILE. The five that live in the mod's own translation
// units are read OUT OF THE SOURCE at run time (argv[1] is src/), so this harness cannot drift
// away from what the mod actually scans for: change a byte in ut_live.cpp and the test scans the
// changed pattern. The two converted deposit sites are linked in from ut_bindings.cpp, which is
// the one copy the mod uses too.
//
// THE CASES:
//   1  each exe signature matches its declared number of times, at its recorded rva
//   2  a CORRUPTED pattern (one fixed byte changed) matches ZERO times - never something else
//   3  a byte changed under a WILDCARD still matches, which is what proves the mask is live
//   4  the converted deposit sites' accepted return windows are the byte ranges the rva literals
//      used to name (0x1EAB30..45 and 0x1EC650..65)
//   5  the Game.dll decoders yield the offsets the mod expects (needs UNIQUETAB_TEST_GAME_DIR),
//      including WHICH SIDE of the replica block each u32 getter falls on, and sizeof
//      (ItemReplicaInfo) decoded by the mod's own two-source reader (ut_replicasize.h): both
//      sources must give 0x190 and agree
//   6  the gate PASSES when every early binding has reported, and its line reads "13 decoded
//      (3 advisory) ... all confirmed, exe signatures pending (game thread)"
//   7  the CLASSIFICATION, row by row against an explicit list: class, phase and CRITICAL /
//      ADVISORY; a failed ADVISORY row leaves the gate open, a failed CRITICAL row closes it

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "../src/gd_exports.h"
#include "../src/gd_exports_reagent.h"
#include "../src/ut_bindings.h"
#include "../src/ut_replicasize.h"

namespace {

int g_fail = 0;
int g_ran = 0;

void ok(bool cond, const char* what, const char* detail) {
    ++g_ran;
    if (cond) {
        printf("[bindings]  PASS  %-34s %s\n", what, detail ? detail : "");
    } else {
        printf("[bindings]  FAIL  %-34s %s\n", what, detail ? detail : "");
        ++g_fail;
    }
}

bool readFile(const char* path, std::vector<unsigned char>* out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) {
        fclose(f);
        return false;
    }
    out->resize((size_t)n);
    const size_t got = fread(&(*out)[0], 1, (size_t)n, f);
    fclose(f);
    return got == (size_t)n;
}

// ---- the five patterns, read out of the mod's own sources -------------------------------------
// `const unsigned char kName[] = { 0x48, 0x8B, ... };` - every 0xNN token up to the closing brace.
bool patternFromSource(const std::string& src, const char* name, std::vector<unsigned char>* out) {
    std::string needle = "unsigned char ";
    needle += name;
    needle += "[] = {";
    size_t at = src.find(needle);
    if (at == std::string::npos) return false;
    at += needle.size();
    const size_t end = src.find('}', at);
    if (end == std::string::npos) return false;
    out->clear();
    for (size_t i = at; i + 3 < end; ++i) {
        if (src[i] == '0' && (src[i + 1] == 'x' || src[i + 1] == 'X')) {
            out->push_back((unsigned char)strtoul(src.substr(i + 2, 2).c_str(), nullptr, 16));
            i += 3;
        }
    }
    return !out->empty();
}

struct ExeSig {
    const char* file;      // the mod source it lives in
    const char* array;     // the array's name there
    const char* pretty;    // what it locates
    int expectHits;
    size_t rva;            // the 1.3.0.8 rva of the FIRST hit
};

const ExeSig kExeSigs[] = {
    {"ut_plate.cpp", "kSigWndLoad", "exe.ReagentWindowLoad", 1, 0x132100},
    {"ut_live.cpp", "kSigLoad", "exe.UIReagentItemLoad", 1, 0x1F0660},
    {"ut_live.cpp", "kSigSetItem", "exe.UIReagentItemSetItem", 1, 0x1EE2E0},
    {"ut_live.cpp", "kSigSetPos", "exe.UIReagentItemSetPos", 1, 0x1EE830},
    {"ut_reagent.cpp", "kSigTakeReplica", "exe.takeReplicaPair", 2, 0x132B4F},
};

// A pattern with no wildcards, for utBindScan.
int scanPlain(const std::vector<unsigned char>& img, const std::vector<unsigned char>& pat,
              const unsigned char** hits, int maxHits) {
    std::vector<unsigned char> mask(pat.size(), 1);
    ut::UtBindPattern p = {"", &pat[0], &mask[0], pat.size(), 1, 0};
    return ut::utBindScan(&img[0], &img[0] + img.size(), p, hits, maxHits);
}

void runExeHalf(const char* imagePath, const char* srcDir) {
    std::vector<unsigned char> img;
    if (!readFile(imagePath, &img)) {
        printf("[bindings] ***** the exe image could not be read: %s *****\n", imagePath);
        ++g_fail;
        return;
    }
    printf("[bindings] exe image: %llu bytes\n", (unsigned long long)img.size());

    // -- case 1: every pattern the mod scans for, read out of the mod's own source --------------
    for (size_t i = 0; i < sizeof(kExeSigs) / sizeof(kExeSigs[0]); ++i) {
        const ExeSig& s = kExeSigs[i];
        std::string path = std::string(srcDir) + "\\" + s.file;
        std::vector<unsigned char> raw;
        char detail[256];
        if (!readFile(path.c_str(), &raw)) {
            _snprintf_s(detail, sizeof(detail), _TRUNCATE, "could not read %s", s.file);
            ok(false, s.pretty, detail);
            continue;
        }
        std::string text((const char*)&raw[0], raw.size());
        std::vector<unsigned char> pat;
        if (!patternFromSource(text, s.array, &pat)) {
            _snprintf_s(detail, sizeof(detail), _TRUNCATE, "%s not found in %s", s.array, s.file);
            ok(false, s.pretty, detail);
            continue;
        }
        const unsigned char* hits[4] = {nullptr, nullptr, nullptr, nullptr};
        const int n = scanPlain(img, pat, hits, 4);
        const size_t rva = hits[0] ? (size_t)(hits[0] - &img[0]) : 0;
        _snprintf_s(detail, sizeof(detail), _TRUNCATE,
                    "%s (%zu bytes from %s): %d hit(s), first at rva 0x%zX, expected %d at 0x%zX",
                    s.array, pat.size(), s.file, n, rva, s.expectHits, s.rva);
        ok(n == s.expectHits && rva == s.rva, s.pretty, detail);
    }

    // -- the two converted deposit sites, from ut_bindings.cpp itself ---------------------------
    const ut::UtBindPattern* sites[2] = {&ut::kUtSigDepositSiteA, &ut::kUtSigDepositSiteB};
    for (int i = 0; i < 2; ++i) {
        const unsigned char* hits[4] = {nullptr, nullptr, nullptr, nullptr};
        const int n = ut::utBindScan(&img[0], &img[0] + img.size(), *sites[i], hits, 4);
        const size_t rva = hits[0] ? (size_t)(hits[0] - &img[0]) : 0;
        char detail[256];
        _snprintf_s(detail, sizeof(detail), _TRUNCATE,
                    "%zu bytes: %d hit(s), first at rva 0x%zX, expected 1 at 0x%zX",
                    sites[i]->len, n, rva, sites[i]->rva1308);
        ok(n == 1 && rva == sites[i]->rva1308, sites[i]->name, detail);

        // -- case 4: the accepted return window IS the old rva literal ------------------------
        if (n == 1) {
            const size_t lo = rva + (i == 0 ? ut::kUtSiteAWindowLo : ut::kUtSiteBWindowLo);
            const size_t hi = rva + (i == 0 ? ut::kUtSiteAWindowHi : ut::kUtSiteBWindowHi);
            const size_t wantLo = i == 0 ? 0x1EAB30 : 0x1EC650;
            const size_t wantHi = i == 0 ? 0x1EAB45 : 0x1EC665;
            _snprintf_s(detail, sizeof(detail), _TRUNCATE,
                        "return window 0x%zX..0x%zX, the literal it replaced was 0x%zX..0x%zX", lo,
                        hi, wantLo, wantHi);
            ok(lo == wantLo && hi == wantHi, "the window is the old literal", detail);
        }
    }

    // -- case 2: a CORRUPTED pattern must be NOT FOUND, never matched somewhere else ------------
    {
        std::vector<unsigned char> bytes(ut::kUtSigDepositSiteA.bytes,
                                         ut::kUtSigDepositSiteA.bytes +
                                             ut::kUtSigDepositSiteA.len);
        std::vector<unsigned char> mask(ut::kUtSigDepositSiteA.mask,
                                        ut::kUtSigDepositSiteA.mask + ut::kUtSigDepositSiteA.len);
        bytes[8] = (unsigned char)(bytes[8] ^ 0xFF);  // a FIXED byte: `mov edx,[rbp+0x208]`
        ut::UtBindPattern bad = {"corrupted site A", &bytes[0], &mask[0], bytes.size(), 1, 0};
        const unsigned char* hits[4] = {nullptr, nullptr, nullptr, nullptr};
        const int n = ut::utBindScan(&img[0], &img[0] + img.size(), bad, hits, 4);
        char detail[160];
        _snprintf_s(detail, sizeof(detail), _TRUNCATE,
                    "one fixed byte changed -> %d hit(s); anything but 0 would be a WRONG MATCH",
                    n);
        ok(n == 0, "a corrupted pattern is NOT FOUND", detail);
    }

    // -- case 3: a byte changed under a WILDCARD still matches ----------------------------------
    {
        std::vector<unsigned char> bytes(ut::kUtSigDepositSiteA.bytes,
                                         ut::kUtSigDepositSiteA.bytes +
                                             ut::kUtSigDepositSiteA.len);
        std::vector<unsigned char> mask(ut::kUtSigDepositSiteA.mask,
                                        ut::kUtSigDepositSiteA.mask + ut::kUtSigDepositSiteA.len);
        bytes[4] = (unsigned char)(bytes[4] ^ 0xFF);  // inside the je displacement: wildcarded
        ut::UtBindPattern wild = {"wildcarded site A", &bytes[0], &mask[0], bytes.size(), 1, 0};
        const unsigned char* hits[4] = {nullptr, nullptr, nullptr, nullptr};
        const int n = ut::utBindScan(&img[0], &img[0] + img.size(), wild, hits, 4);
        const size_t rva = hits[0] ? (size_t)(hits[0] - &img[0]) : 0;
        char detail[200];
        _snprintf_s(detail, sizeof(detail), _TRUNCATE,
                    "a byte under a wildcard changed -> %d hit(s) at 0x%zX; the mask is live", n,
                    rva);
        ok(n == 1 && rva == ut::kUtSigDepositSiteA.rva1308, "the wildcard mask is live", detail);
    }
}

// ---- the decoder half --------------------------------------------------------------------------
// Game.dll and Engine.dll are plain on disk, so the exported function's bytes can be read straight
// out of the file. These decoders MIRROR the ones in src/ut_reagent.cpp (decodeFlagOffset and
// friends); if one of those changes, this half has to change with it, and that is written down
// here as the harness's one maintenance cost.
struct Pe {
    std::vector<unsigned char> raw;
    std::vector<std::pair<unsigned long, unsigned long> > secs;  // (va, ptrToRaw), plus size
    std::vector<unsigned long> secSize;
    std::vector<std::string> secName;
    unsigned long exportRva = 0;

    bool load(const char* path) {
        if (!readFile(path, &raw) || raw.size() < 0x200) return false;
        const unsigned long lfa = *(const unsigned long*)&raw[0x3C];
        if (lfa + 0x100 >= raw.size()) return false;
        const unsigned short nsec = *(const unsigned short*)&raw[lfa + 6];
        const unsigned short optSize = *(const unsigned short*)&raw[lfa + 20];
        const size_t secOff = lfa + 24 + optSize;
        for (unsigned short i = 0; i < nsec; ++i) {
            const size_t o = secOff + (size_t)i * 40;
            if (o + 40 > raw.size()) return false;
            const unsigned long vsize = *(const unsigned long*)&raw[o + 8];
            const unsigned long va = *(const unsigned long*)&raw[o + 12];
            const unsigned long rsize = *(const unsigned long*)&raw[o + 16];
            const unsigned long ptr = *(const unsigned long*)&raw[o + 20];
            secs.push_back(std::make_pair(va, ptr));
            secSize.push_back(vsize > rsize ? vsize : rsize);
            char nm[9] = {0};
            memcpy(nm, &raw[o], 8);
            secName.push_back(nm);
        }
        exportRva = *(const unsigned long*)&raw[lfa + 24 + 112];  // DataDirectory[0].VirtualAddress
        return exportRva != 0;
    }
    // The .text section, for a signature scan. Returns false when there is none.
    bool textRange(const unsigned char** lo, const unsigned char** hi, unsigned long* va) const {
        for (size_t i = 0; i < secs.size(); ++i) {
            if (secName[i] != ".text") continue;
            *lo = &raw[secs[i].second];
            *hi = *lo + secSize[i];
            *va = secs[i].first;
            return true;
        }
        return false;
    }
    size_t toFile(unsigned long rva) const {
        for (size_t i = 0; i < secs.size(); ++i) {
            if (rva >= secs[i].first && rva < secs[i].first + secSize[i]) {
                return secs[i].second + (rva - secs[i].first);
            }
        }
        return 0;
    }
    // The bytes at an rva, or null when it maps nowhere.
    const unsigned char* at(unsigned long rva) const {
        const size_t f = rva ? toFile(rva) : 0;
        return f && f < raw.size() ? &raw[f] : nullptr;
    }
    // The bytes of an exported function, by decorated name. 0 = not exported.
    const unsigned char* fn(const char* name) const { return at(fnRva(name)); }
    unsigned long fnRva(const char* name) const {
        const size_t e = toFile(exportRva);
        if (!e || e + 40 > raw.size()) return 0;
        const unsigned long nNames = *(const unsigned long*)&raw[e + 24];
        const unsigned long fnRva = *(const unsigned long*)&raw[e + 28];
        const unsigned long nameRva = *(const unsigned long*)&raw[e + 32];
        const unsigned long ordRva = *(const unsigned long*)&raw[e + 36];
        const size_t fnT = toFile(fnRva), nameT = toFile(nameRva), ordT = toFile(ordRva);
        if (!fnT || !nameT || !ordT) return 0;
        for (unsigned long i = 0; i < nNames; ++i) {
            const unsigned long nrva = *(const unsigned long*)&raw[nameT + i * 4];
            const size_t at = toFile(nrva);
            if (!at || at >= raw.size()) continue;
            if (strcmp((const char*)&raw[at], name) != 0) continue;
            const unsigned short ord = *(const unsigned short*)&raw[ordT + i * 2];
            return *(const unsigned long*)&raw[fnT + (size_t)ord * 4];
        }
        return 0;
    }
};

// `0F B6 81 <disp32> C3` = movzx eax, byte ptr [rcx + disp32] ; ret
unsigned long decodeFlagOffset(const unsigned char* p) {
    if (!p) return 0;
    if (p[0] != 0x0F || p[1] != 0xB6 || p[2] != 0x81) return 0;
    if (p[7] != 0xC3) return 0;
    return *(const unsigned long*)(p + 3);
}

// `8B 81 <disp32> C3` = mov eax, dword ptr [rcx + disp32] ; ret  - the u32/enum getters. Byte for
// byte what ut_reagent.cpp's decodeDwordOffset does, run here over the file instead of over the
// loaded module.
unsigned long decodeDwordOffset(const unsigned char* p) {
    if (!p) return 0;
    if (p[0] != 0x8B || p[1] != 0x81 || p[6] != 0xC3) return 0;
    return *(const unsigned long*)(p + 2);
}

// `48 8B C2 / 48 8D 91 <disp32>` - the lea in Item::GetItemReplicaInfo that says where the
// ItemReplicaInfo lives inside the Item.
unsigned long decodeReplicaOffset(const unsigned char* p) {
    if (!p) return 0;
    if (p[0] != 0x48 || p[1] != 0x8B || p[2] != 0xC2) return 0;
    if (p[3] != 0x48 || p[4] != 0x8D || p[5] != 0x91) return 0;
    return *(const unsigned long*)(p + 6);
}

void runDecoderHalf(const char* gameDir) {
    Pe game;
    // The mod is x64, so the Game.dll that matters is the one in the game's x64 folder - the
    // 32-bit build in the root would decode to nonsense. The root is tried second only so that a
    // folder which IS the x64 folder still works.
    std::string gdll = std::string(gameDir) + "\\x64\\Game.dll";
    if (!game.load(gdll.c_str())) gdll = std::string(gameDir) + "\\Game.dll";
    if (!game.load(gdll.c_str())) {
        printf("[bindings] ***** Game.dll could not be read at %s *****\n", gdll.c_str());
        ++g_fail;
        return;
    }
    printf("[bindings] Game.dll: %llu bytes, read only\n", (unsigned long long)game.raw.size());

    struct Case {
        const char* name;
        const char* mangled;
        unsigned long expect;
    };
    const Case cases[] = {
        {"item.craftingMaterial", GD_ITEM_ISREAGENTCOMPATIBLE, 0xC64},
        {"item.soulbound", GD_ITEM_ISSOULBOUND, 0xC00},
        {"item.untradeable", GD_ITEM_ISUNTRADEABLE, 0xC02},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const unsigned long got = decodeFlagOffset(game.fn(cases[i].mangled));
        char detail[200];
        _snprintf_s(detail, sizeof(detail), _TRUNCATE,
                    "decoded 0x%lX out of the export's own bytes, 1.3.0.8 recorded 0x%lX", got,
                    cases[i].expect);
        ok(got == cases[i].expect, cases[i].name, detail);
    }

    // -- the four u32 getters, and WHERE each one sits -------------------------------------------
    // Asking all four to land inside [replicaInfo, replicaInfo + 0x190) turns the whole mod off
    // on a healthy 1.3.0.8, because two of them never can: prefixClass and
    // suffixClass are ITEM fields well above the block. Both facts are asserted here so the rule
    // in ut_reagent.cpp cannot drift back: the offsets themselves, and which side of the block's
    // end each one falls on.
    {
        const unsigned long replica = decodeReplicaOffset(game.fn(GD_ITEM_GETITEMREPLICAINFO));
        // sizeof(ItemReplicaInfo), by the mod's own two-source reader over the file's bytes.
        const unsigned char* ctor = game.fn(GD_ITEM_CTOR);
        const unsigned int fromCtor =
            ctor ? ut::utReplicaSizeFromItemCtor(ctor, 0x120, (unsigned int)replica) : 0;
        int rel = 0;
        unsigned long assignRva = 0;
        unsigned int fromAssign = 0;
        const unsigned long getRva = game.fnRva(GD_ITEM_GETITEMREPLICAINFO);
        const size_t after = ut::utReplicaAssignJump(game.at(getRva), 0x12, &rel);
        if (after) {
            assignRva = getRva + (unsigned long)after + (unsigned long)rel;
            fromAssign = ut::utReplicaSizeFromAssign(game.at(assignRva), 0x400);
        }
        char sdetail[240];
        _snprintf_s(sdetail, sizeof(sdetail), _TRUNCATE,
                    "Item::Item at 0x%lX: the member after the block at +0x%lX gives 0x%X, "
                    "1.3.0.8 recorded 0x190",
                    game.fnRva(GD_ITEM_CTOR), replica + fromCtor, fromCtor);
        ok(fromCtor == 0x190, "item.replicaSize from Item::Item", sdetail);
        _snprintf_s(sdetail, sizeof(sdetail), _TRUNCATE,
                    "ItemReplicaInfo::operator= at 0x%lX (the jmp in GetItemReplicaInfo): the "
                    "largest store gives 0x%X, 1.3.0.8 recorded 0x190 at 0x38840",
                    assignRva, fromAssign);
        ok(fromAssign == 0x190 && assignRva == 0x38840,
           "item.replicaSize from operator=", sdetail);
        _snprintf_s(sdetail, sizeof(sdetail), _TRUNCATE,
                    "0x%X and 0x%X: equal, 8-aligned, 0x100..0x200", fromCtor, fromAssign);
        ok(ut::utReplicaSizeConfirmed(fromCtor, fromAssign), "the two size sources agree",
           sdetail);
        // A block that grew by 8 (the ctor says so, operator= does not) must NOT confirm.
        ok(!ut::utReplicaSizeConfirmed(fromCtor + 8, fromAssign) &&
               !ut::utReplicaSizeConfirmed(0, 0) && !ut::utReplicaSizeConfirmed(0x84, 0x84),
           "disagreeing or implausible sizes are refused", "0x198/0x190, 0/0, 0x84/0x84");
        const unsigned long replicaSize = fromCtor;
        const unsigned long seed = decodeDwordOffset(game.fn(GD_ITEM_GETSEEDREROLLS));
        const unsigned long affix = decodeDwordOffset(game.fn(GD_ITEM_GETAFFIXREROLLS));
        const unsigned long prefix = decodeDwordOffset(game.fn(GD_ITEM_GETPREFIXCLASSIFICATION));
        const unsigned long suffix = decodeDwordOffset(game.fn(GD_ITEM_GETSUFFIXCLASSIFICATION));
        char detail[240];

        _snprintf_s(detail, sizeof(detail), _TRUNCATE,
                    "decoded 0x%lX out of the lea in Item::GetItemReplicaInfo, 1.3.0.8 recorded "
                    "0x538 (+0x%lX bytes)", replica, replicaSize);
        ok(replica == 0x538, "item.replicaInfo", detail);

        _snprintf_s(detail, sizeof(detail), _TRUNCATE,
                    "seedRerolls 0x%lX and affixRerolls 0x%lX, block 0x%lX..0x%lX", seed, affix,
                    replica, replica + replicaSize);
        ok(seed == 0x6B8 && affix == 0x6B4 && seed >= replica && seed < replica + replicaSize &&
               affix >= replica && affix + 4 <= replica + replicaSize,
           "the reroll counts ARE in the block", detail);

        _snprintf_s(detail, sizeof(detail), _TRUNCATE,
                    "prefixClass 0x%lX and suffixClass 0x%lX are ABOVE the block end 0x%lX - the "
                    "an in-the-block rule could never hold for them",
                    prefix, suffix, replica + replicaSize);
        ok(prefix == 0x880 && suffix == 0x884 && prefix >= replica + replicaSize,
           "the class fields are NOT in the block", detail);

        _snprintf_s(detail, sizeof(detail), _TRUNCATE,
                    "non-zero, below 0x2000, and exactly four bytes apart: 0x%lX / 0x%lX", prefix,
                    suffix);
        ok(prefix != 0 && prefix < 0x2000 && suffix == prefix + 4,
           "the rule the mod uses instead holds", detail);
    }

    // -- the cursor-drag call site: the one code address that is still located by signature ------
    {
        const unsigned char *lo = nullptr, *hi = nullptr;
        unsigned long va = 0;
        char detail[256];
        if (!game.textRange(&lo, &hi, &va)) {
            ok(false, ut::kUtSigDragSite.name, "Game.dll has no .text section");
        } else {
            const unsigned char* hits[4] = {nullptr, nullptr, nullptr, nullptr};
            const int n = ut::utBindScan(lo, hi, ut::kUtSigDragSite, hits, 4);
            const unsigned long rva = hits[0] ? (unsigned long)(hits[0] - lo) + va : 0;
            _snprintf_s(detail, sizeof(detail), _TRUNCATE,
                        "%zu bytes: %d hit(s), first at Game.dll +0x%lX, expected 1 at 0x%zX",
                        ut::kUtSigDragSite.len, n, rva, ut::kUtSigDragSite.rva1308);
            ok(n == 1 && (size_t)rva == ut::kUtSigDragSite.rva1308, ut::kUtSigDragSite.name,
               detail);
            if (n == 1) {
                _snprintf_s(detail, sizeof(detail), _TRUNCATE,
                            "return window 0x%lX..0x%lX, the literal it replaced was "
                            "0x17396D..0x173980",
                            rva + (unsigned long)ut::kUtDragWindowLo,
                            rva + (unsigned long)ut::kUtDragWindowHi);
                ok(rva + ut::kUtDragWindowLo == 0x17396D && rva + ut::kUtDragWindowHi == 0x173980,
                   "the drag window is the old literal", detail);
            }
        }
    }
}

// ---- the gate itself ---------------------------------------------------------------------------
// This half needs NO input at all, so the suite is never a complete skip: the all-or-nothing rule
// is the point of the gate, and it is checked here with the real registry. ut_log writes
// nothing without logInit, so the gate's own lines go nowhere and the summary is read back with
// bindingsSummary() instead.
void runGateHalf() {
    // 1. nothing has reported yet - every EARLY binding is outstanding, so the gate must refuse.
    ok(!ut::bindingsGate(61, 0), "an unreported binding turns it off",
       "no decoder has reported, so the gate must refuse to install anything");

    // 2. every EARLY binding reports a confirmed outcome - now it must pass.
    struct Early {
        const char* name;
        unsigned long long value;
    };
    const Early early[] = {
        {"item.craftingMaterial", 0xC64}, {"item.soulbound", 0xC00},
        {"item.untradeable", 0xC02},      {"item.replicaInfo", 0x538},
        {"item.replicaSize", 0x190},      {"item.seedRerolls", 0x580},
        {"item.affixRerolls", 0x584},
        {"item.prefixClass", 0x588},      {"item.suffixClass", 0x58C},
        {"item.incrementStackSlot", 0x5F8}, {"item.stackMirror", 0x5A0},
        {"player.ctrlId", 0x16C0},        {"objectManager.objectFromId", 0x1234},
        {"gamedll.dragSite", 0x173960},
    };
    for (size_t i = 0; i < sizeof(early) / sizeof(early[0]); ++i) {
        ut::bindingsNote(early[i].name, early[i].value, true, "the offline harness said so");
    }
    ok(ut::bindingsGate(61, 0), "it passes once every early binding is confirmed",
       ut::bindingsSummary());
    printf("[bindings] the gate's INFO line reads: %s\n", ut::bindingsSummary());
    ok(strstr(ut::bindingsSummary(), "13 decoded (3 advisory)") != nullptr &&
           strstr(ut::bindingsSummary(), "15 literal (1 advisory)") != nullptr &&
           strstr(ut::bindingsSummary(), "all confirmed") != nullptr &&
           strstr(ut::bindingsSummary(), "exe signatures pending (game thread)") != nullptr,
       "a healthy game reads 13 decoded (3 advisory), all confirmed", ut::bindingsSummary());

    // 2a. THE CLASSIFICATION, row by row. Every row's class, phase and gate flag is written out
    // here by hand; a row that appears, disappears or changes flag fails offline.
    {
        struct Want {
            const char* name;
            int cls, phase, gate;
        };
        const int D = ut::UT_BIND_DECODED, S = ut::UT_BIND_STRUCTURAL, L = ut::UT_BIND_LITERAL,
                  G = ut::UT_BIND_SIGNATURE;
        const int E = ut::UT_BIND_EARLY, T = ut::UT_BIND_LATE;
        const int C = ut::UT_BIND_CRITICAL, A = ut::UT_BIND_ADVISORY;
        const Want want[] = {
            {"item.craftingMaterial", D, E, C}, {"item.soulbound", D, E, C},
            {"item.untradeable", D, E, C},      {"item.replicaInfo", D, E, C},
            {"item.replicaSize", D, E, C},      {"item.seedRerolls", D, E, C},
            {"item.affixRerolls", D, E, C},     {"item.prefixClass", D, E, A},
            {"item.suffixClass", D, E, A},      {"item.incrementStackSlot", D, E, A},
            {"item.stackMirror", D, E, C},      {"player.ctrlId", D, E, C},
            {"objectManager.objectFromId", D, E, C},
            {"msvc.treeNode", S, E, C},         {"msvc.string", S, E, C},
            {"boxVector.stride", S, E, C},      {"hud.caravanWindow", L, E, C},
            {"caravan.pages", L, E, C},         {"caravan.pageIndex", L, E, C},
            {"window.boxVector", L, E, C},      {"window.origin", L, E, C},
            {"window.plateWidget", L, E, C},    {"window.plateTexture", L, E, C},
            {"window.plateSize", L, E, C},      {"window.searchRange", L, E, C},
            {"box.protoId", L, E, C},           {"box.localPos", L, E, C},
            {"box.parentOrigin", L, E, C},      {"vtable.slotNumbers", L, E, C},
            {"gameEngine.saveVariant", L, E, A}, {"gamedll.dragSite", G, E, C},
            {"gameTextLine.size", L, E, C},     {"exe.ReagentWindowLoad", G, T, C},
            {"exe.UIReagentItemLoad", G, T, C}, {"exe.UIReagentItemSetItem", G, T, C},
            {"exe.UIReagentItemSetPos", G, T, C}, {"exe.takeReplicaPair", G, T, C},
            {"exe.depositSiteA", G, T, C},      {"exe.depositSiteB", G, T, C},
        };
        const int nWant = (int)(sizeof(want) / sizeof(want[0]));
        int bad = 0, advisory = 0;
        char firstBad[96] = "";
        for (int i = 0; i < ut::bindingsRowCount() || i < nWant; ++i) {
            const char* name = nullptr;
            int cls = -1, phase = -1, gate = -1;
            const bool have = ut::bindingsRowAt(i, &name, &cls, &phase, &gate);
            const bool match = have && i < nWant && strcmp(name, want[i].name) == 0 &&
                               cls == want[i].cls && phase == want[i].phase &&
                               gate == want[i].gate;
            if (have && gate == ut::UT_BIND_ADVISORY) ++advisory;
            if (match) continue;
            ++bad;
            if (!firstBad[0]) {
                _snprintf_s(firstBad, sizeof(firstBad), _TRUNCATE, "row %d: table %s, expected %s",
                            i, have ? name : "(none)", i < nWant ? want[i].name : "(none)");
            }
        }
        char detail[200];
        _snprintf_s(detail, sizeof(detail), _TRUNCATE, "%d rows, %d advisory, %d mismatch%s%s",
                    ut::bindingsRowCount(), advisory, bad, firstBad[0] ? ": " : "", firstBad);
        ok(bad == 0 && ut::bindingsRowCount() == nWant && advisory == 4,
           "the classification is the expected list", detail);
    }

    // 2b. THE CLASSIFICATION ITSELF. The gate counts an EARLY row that has not reported
    // as a failure, so the set of rows that must report before it has to stay exactly the set of
    // bindings obtainable WITHOUT the game thread. `early[]` above is that set written out by
    // hand; these three checks make the table agree with it, so a binding that quietly becomes
    // lazily decoded (or an exe signature that is mistakenly marked EARLY) fails HERE, offline,
    // instead of turning the mod off on a player's healthy game.
    {
        int mustReport = 0, mismatched = 0, decodedLate = 0, lateNotExe = 0;
        char firstBad[96] = "";
        for (int i = 0; i < ut::bindingsRowCount(); ++i) {
            const char* name = nullptr;
            int cls = 0, phase = 0;
            if (!ut::bindingsRowAt(i, &name, &cls, &phase)) continue;
            // STRUCTURAL and LITERAL are confirmed at every use, not at start-up: they are the
            // only EARLY rows that are not expected to report.
            const bool reports = cls != ut::UT_BIND_STRUCTURAL && cls != ut::UT_BIND_LITERAL;
            if (cls == ut::UT_BIND_DECODED && phase != ut::UT_BIND_EARLY) {
                ++decodedLate;
                if (!firstBad[0]) _snprintf_s(firstBad, sizeof(firstBad), _TRUNCATE, "%s", name);
            }
            if (phase == ut::UT_BIND_LATE) {
                // Only the exe's own .text needs the game thread: it is DRM-encrypted until the
                // Steam stub has run. Everything else is readable before the first hook.
                if (strncmp(name, "exe.", 4) != 0) {
                    ++lateNotExe;
                    if (!firstBad[0]) _snprintf_s(firstBad, sizeof(firstBad), _TRUNCATE, "%s", name);
                }
                continue;
            }
            if (!reports) continue;
            ++mustReport;
            bool listed = false;
            for (size_t j = 0; j < sizeof(early) / sizeof(early[0]); ++j) {
                if (strcmp(early[j].name, name) == 0) { listed = true; break; }
            }
            if (!listed) {
                ++mismatched;
                if (!firstBad[0]) _snprintf_s(firstBad, sizeof(firstBad), _TRUNCATE, "%s", name);
            }
        }
        char detail[256];
        _snprintf_s(detail, sizeof(detail), _TRUNCATE,
                    "%d rows must report before the gate, the harness reports %zu%s%s", mustReport,
                    sizeof(early) / sizeof(early[0]), firstBad[0] ? ", first mismatch: " : "",
                    firstBad[0] ? firstBad : "");
        ok(mismatched == 0 && mustReport == (int)(sizeof(early) / sizeof(early[0])),
           "the early set is exactly the early reporters", detail);
        _snprintf_s(detail, sizeof(detail), _TRUNCATE,
                    "%d decoded row(s) marked LATE - a decoder reads an EXPORT's bytes and needs "
                    "no game thread", decodedLate);
        ok(decodedLate == 0, "every decoder is classified EARLY", detail);
        _snprintf_s(detail, sizeof(detail), _TRUNCATE,
                    "%d late row(s) that are not exe.* - only the DRM-encrypted exe .text may be "
                    "late", lateNotExe);
        ok(lateNotExe == 0, "only exe signatures are classified LATE", detail);
    }

    // 3a. an ADVISORY row fails - nothing reads it, so the gate stays open and the line says so.
    ut::bindingsNote("item.prefixClass", 0, false, "four bytes below suffixClass");
    ok(ut::bindingsGate(61, 0) && ut::bindingsGateFailure()[0] == 0,
       "a failed advisory row leaves the gate open", ut::bindingsSummary());
    ok(strstr(ut::bindingsSummary(), "confirmed except 1 advisory") != nullptr &&
           strstr(ut::bindingsSummary(), "12 decoded (3 advisory)") != nullptr,
       "and the count line says which", ut::bindingsSummary());

    // 3b. a CRITICAL row fails - and the whole mod has to go off, naming it.
    ut::bindingsNote("player.ctrlId", 0, false, "a non-zero offset below 0x20000");
    ok(!ut::bindingsGate(61, 0) && strcmp(ut::bindingsGateFailure(), "player.ctrlId") == 0,
       "one failed critical binding turns the whole mod off, naming it",
       ut::bindingsGateFailure());
    printf("[bindings] and then it reads: %s\n", ut::bindingsSummary());
}

}  // namespace

int main(int argc, char** argv) {
    const char* srcDir = argc > 1 ? argv[1] : "src";
    printf("[bindings] the offline proof behind the mod's self-located bindings\n");

    const char* image = getenv("UNIQUETAB_TEST_EXE_IMAGE");
    if (image && *image) {
        runExeHalf(image, srcDir);
    } else {
        printf("[bindings] ***** SKIPPED the exe signatures - set UNIQUETAB_TEST_EXE_IMAGE to a\n");
        printf("[bindings]       DECRYPTED Grim Dawn.exe image to run them. The exe on disk is\n");
        printf("[bindings]       Steam-DRM encrypted, so its .text cannot be scanned from a file.\n");
        printf("[bindings]       The file is only ever READ. *****\n");
    }

    runGateHalf();  // needs no input, so this suite always checks something

    const char* dir = getenv("UNIQUETAB_TEST_GAME_DIR");
    if (dir && *dir) {
        runDecoderHalf(dir);
    } else {
        printf("[bindings] ***** SKIPPED the decoders - set UNIQUETAB_TEST_GAME_DIR to the\n");
        printf("[bindings]       installed game folder to run them. Game.dll is opened READ\n");
        printf("[bindings]       ONLY and nothing in the folder is written, ever. *****\n");
    }

    printf("[bindings] %d check(s) ran, %d failed\n", g_ran, g_fail);
    if (g_ran == 0) {
        printf("[bindings] NOTHING RAN - both inputs were missing (this is a SKIP, not a pass)\n");
        return 0;
    }
    printf(g_fail ? "[bindings] FAILURES\n" : "[bindings] ALL PASS\n");
    return g_fail ? 1 : 0;
}
