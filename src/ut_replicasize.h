// ut_replicasize.h - sizeof(ItemReplicaInfo), decoded out of Game.dll's own exported bytes from
// TWO independent sources that must agree. Pure byte readers: no Windows, no SEH, no mod state,
// so tools/test_bindings.cpp runs exactly this code over the file on disk.
//
//   A  Item::Item (??0Item@GAME@@QEAA@XZ) constructs its members in declaration order:
//        48 8D 8F <replicaOff>  lea rcx,[rdi+0x538]     ; the ItemReplicaInfo
//        E8 <rel32> 90          call ItemReplicaInfo::ItemReplicaInfo ; nop
//        48 8D 8F <next>        lea rcx,[rdi+0x6C8]     ; the member right after the block
//        E8 <rel32>
//      so `next - replicaOff` is the block's extent (0x190 on 1.3.0.8).
//   B  ItemReplicaInfo::operator= (not exported; the direct jmp target of Item::GetItemReplicaInfo,
//      `E9 <rel32>` at +0xD) is a straight-line member-wise copy, dst in rbx: the largest
//      destination displacement plus that member's width, rounded up to 8, is the same extent.
//      The walk knows only the instruction shapes such a copy is made of and stops at `ret`; any
//      other byte makes it return 0, which is a decode failure, never a guess.
#pragma once

#include <stddef.h>
#include <string.h>

namespace ut {

inline unsigned int utReadU32(const unsigned char* p) {
    unsigned int v = 0;
    memcpy(&v, p, 4);
    return v;
}

// Source A. `bytes` is Item::Item, `limit` how many of its bytes may be read.
inline unsigned int utReplicaSizeFromItemCtor(const unsigned char* bytes, size_t limit,
                                              unsigned int replicaOff) {
    if (!bytes || !replicaOff || limit < 24) return 0;
    for (size_t i = 0; i + 24 <= limit; ++i) {
        if (bytes[i] != 0x48 || bytes[i + 1] != 0x8D) continue;
        const unsigned char modrm = bytes[i + 2];  // lea rcx,[base+disp32]: 0x88 | base, no SIB
        if ((modrm & 0xF8) != 0x88 || (modrm & 7) == 4) continue;
        if (utReadU32(bytes + i + 3) != replicaOff) continue;
        size_t j = i + 7;
        if (bytes[j] != 0xE8) continue;
        j += 5;
        if (j < limit && bytes[j] == 0x90) ++j;
        if (j + 12 > limit) return 0;
        if (bytes[j] != 0x48 || bytes[j + 1] != 0x8D || bytes[j + 2] != modrm) return 0;
        if (bytes[j + 7] != 0xE8) return 0;
        const unsigned int next = utReadU32(bytes + j + 3);
        return next > replicaOff ? next - replicaOff : 0;
    }
    return 0;
}

// The rel32 of the direct transfer to ItemReplicaInfo::operator= inside Item::GetItemReplicaInfo
// (`48 8B C2 / 48 8D 91 <d32> / 48 8B C8 / E9 <rel32>`). Returns the offset of the byte AFTER the
// jmp (so target = fn + that + rel32), or 0 when the shape is not there.
inline size_t utReplicaAssignJump(const unsigned char* bytes, size_t limit, int* rel32) {
    if (!bytes || limit < 0x12) return 0;
    if (bytes[0] != 0x48 || bytes[1] != 0x8B || bytes[2] != 0xC2) return 0;
    if (bytes[3] != 0x48 || bytes[4] != 0x8D || bytes[5] != 0x91) return 0;
    if (bytes[0xA] != 0x48 || bytes[0xB] != 0x8B || bytes[0xC] != 0xC8) return 0;
    if (bytes[0xD] != 0xE9) return 0;
    int r = 0;
    memcpy(&r, bytes + 0xE, 4);
    if (rel32) *rel32 = r;
    return 0x12;
}

// Source B. `bytes` is ItemReplicaInfo::operator=.
inline unsigned int utReplicaSizeFromAssign(const unsigned char* bytes, size_t limit) {
    if (!bytes || limit < 16) return 0;
    unsigned int extent = 0;
    size_t i = 0;
    while (i < limit) {
        const unsigned char* p = bytes + i;
        const size_t left = limit - i;
        size_t len = 0;
        unsigned int disp = 0, width = 0;
        bool store = false;
        if (left >= 5 && p[0] == 0x48 && p[1] == 0x89 && p[2] == 0x5C && p[3] == 0x24) len = 5;
        else if (left >= 5 && p[0] == 0x48 && p[1] == 0x8B && p[2] == 0x5C && p[3] == 0x24) len = 5;
        else if (p[0] == 0x57 || p[0] == 0x5F || p[0] == 0x90) len = 1;
        else if (left >= 4 && p[0] == 0x48 && p[1] == 0x83 &&
                 (p[2] == 0xEC || p[2] == 0xC4 || p[2] == 0xC1 || p[2] == 0xC2)) len = 4;
        else if (left >= 2 && p[0] == 0x8B && p[1] == 0x02) len = 2;           // mov eax,[rdx]
        else if (left >= 2 && p[0] == 0x89 && p[1] == 0x01) { len = 2; store = true; width = 4; }
        else if (left >= 3 && p[0] == 0x48 && p[1] == 0x8B &&
                 (p[2] == 0xFA || p[2] == 0xD9 || p[2] == 0xC3)) len = 3;
        else if (left >= 3 && p[0] == 0x48 && p[1] == 0x3B && p[2] == 0xCA) len = 3;
        else if (left >= 2 && p[0] == 0x74) len = 2;                            // je (linear)
        else if (left >= 4 && p[0] == 0x49 && p[1] == 0x83 && p[2] == 0xC9) len = 4;
        else if (left >= 3 && p[0] == 0x45 && p[1] == 0x33 && p[2] == 0xC0) len = 3;
        else if (left >= 5 && p[0] == 0xE8) len = 5;
        // lea rdx,[rdi+d] (source string) / lea rcx,[rbx+d] (destination std::string, 0x20 wide)
        else if (left >= 4 && p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x57) len = 4;
        else if (left >= 7 && p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x97) len = 7;
        else if (left >= 4 && p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x4B) {
            len = 4; store = true; disp = p[3]; width = 0x20;
        } else if (left >= 7 && p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x8B) {
            len = 7; store = true; disp = utReadU32(p + 3); width = 0x20;
        }
        // loads from [rdi+d]: dword / byte / word / qword / double / float
        else if (left >= 3 && p[0] == 0x8B && p[1] == 0x47) len = 3;
        else if (left >= 6 && p[0] == 0x8B && p[1] == 0x87) len = 6;
        else if (left >= 4 && p[0] == 0x0F && (p[1] == 0xB6 || p[1] == 0xB7) && p[2] == 0x47) len = 4;
        else if (left >= 7 && p[0] == 0x0F && (p[1] == 0xB6 || p[1] == 0xB7) && p[2] == 0x87) len = 7;
        else if (left >= 4 && p[0] == 0x48 && p[1] == 0x8B && p[2] == 0x47) len = 4;
        else if (left >= 7 && p[0] == 0x48 && p[1] == 0x8B && p[2] == 0x87) len = 7;
        else if (left >= 4 && p[0] == 0x66 && p[1] == 0x8B && p[2] == 0x47) len = 4;
        else if (left >= 7 && p[0] == 0x66 && p[1] == 0x8B && p[2] == 0x87) len = 7;
        else if (left >= 5 && (p[0] == 0xF2 || p[0] == 0xF3) && p[1] == 0x0F && p[2] == 0x10 &&
                 p[3] == 0x47) len = 5;
        else if (left >= 8 && (p[0] == 0xF2 || p[0] == 0xF3) && p[1] == 0x0F && p[2] == 0x10 &&
                 p[3] == 0x87) len = 8;
        // stores to [rbx+d]: the same widths
        else if (left >= 3 && p[0] == 0x89 && p[1] == 0x43) { len = 3; store = true; disp = p[2]; width = 4; }
        else if (left >= 6 && p[0] == 0x89 && p[1] == 0x83) { len = 6; store = true; disp = utReadU32(p + 2); width = 4; }
        else if (left >= 3 && p[0] == 0x88 && p[1] == 0x43) { len = 3; store = true; disp = p[2]; width = 1; }
        else if (left >= 6 && p[0] == 0x88 && p[1] == 0x83) { len = 6; store = true; disp = utReadU32(p + 2); width = 1; }
        else if (left >= 4 && p[0] == 0x66 && p[1] == 0x89 && p[2] == 0x43) { len = 4; store = true; disp = p[3]; width = 2; }
        else if (left >= 7 && p[0] == 0x66 && p[1] == 0x89 && p[2] == 0x83) { len = 7; store = true; disp = utReadU32(p + 3); width = 2; }
        else if (left >= 4 && p[0] == 0x48 && p[1] == 0x89 && p[2] == 0x43) { len = 4; store = true; disp = p[3]; width = 8; }
        else if (left >= 7 && p[0] == 0x48 && p[1] == 0x89 && p[2] == 0x83) { len = 7; store = true; disp = utReadU32(p + 3); width = 8; }
        else if (left >= 5 && (p[0] == 0xF2 || p[0] == 0xF3) && p[1] == 0x0F && p[2] == 0x11 && p[3] == 0x43) {
            len = 5; store = true; disp = p[4]; width = p[0] == 0xF2 ? 8 : 4;
        } else if (left >= 8 && (p[0] == 0xF2 || p[0] == 0xF3) && p[1] == 0x0F && p[2] == 0x11 && p[3] == 0x83) {
            len = 8; store = true; disp = utReadU32(p + 4); width = p[0] == 0xF2 ? 8 : 4;
        } else if (p[0] == 0xC3) {
            return extent ? (extent + 7) & ~7u : 0;
        } else {
            return 0;  // a shape this walk does not know: refuse, never guess
        }
        if (store && disp < 0x10000 && disp + width > extent) extent = disp + width;
        i += len;
    }
    return 0;  // ran out of bytes before the ret
}

// The rule both the mod and the harness apply to the two sources.
inline bool utReplicaSizeConfirmed(unsigned int fromCtor, unsigned int fromAssign) {
    return fromCtor != 0 && fromCtor == fromAssign && (fromCtor & 7) == 0 && fromCtor >= 0x100 &&
           fromCtor <= 0x200;
}

}  // namespace ut
