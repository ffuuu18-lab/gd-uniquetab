"""Census the GameTextClass -> UI style-name table out of Game.dll.

`GameEngine::LoadFromDatabase` registers every GameTextClass with the name of the UI
style record that paints it.  Each registration is a `lea rdx,[rip -> "<StyleName>"]`
followed (within a few instructions) by `mov dword ptr [rbp+..],<class>`.

The two RANGE arguments are how the real table is produced: the registrations sit far past
the body of `GameEngine::LoadFromDatabase` (rva 0x2B5260), so the default guess window around
that function finds only a handful of them, under the wrong names.

Usage:  python census_textclass.py 0x2FD000 0x304000   -> THE TABLE (84 classes, 0x01..0x54,
                                                          no gaps: what ut_tooltip.cpp carries)
        python census_textclass.py                     -> the default guess window; useful only
                                                          to re-locate the pattern, not a table

Needs capstone and pefile.  Reads the game folder only.
"""
from __future__ import annotations

import os
import sys

import capstone
import pefile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gdpath  # noqa: E402

pefile.MAX_SYMBOL_EXPORT_COUNT = 200000

GAME = gdpath.find_game_dir() or ""
X64 = os.path.join(GAME, "x64")


def load(dll):
    path = dll if os.path.isabs(dll) else os.path.join(X64, dll)
    pe = pefile.PE(path, fast_load=True)
    data = pe.get_memory_mapped_image()
    return pe, data


def export_table(pe):
    """pefile truncates Game.dll's 25,100 exports, so parse the directory by hand
    (pefile truncates the list it parses itself)."""
    import struct as _s
    d = pe.OPTIONAL_HEADER.DATA_DIRECTORY[0]
    base = d.VirtualAddress
    img = pe.get_memory_mapped_image()
    (_, _, _, _, _, ordbase, nfunc, nname, afunc, aname, aord) = _s.unpack_from(
        "<IIHHIIIIIII", img, base)
    out = {}
    for i in range(nname):
        (nrva,) = _s.unpack_from("<I", img, aname + 4 * i)
        (ordv,) = _s.unpack_from("<H", img, aord + 2 * i)
        (frva,) = _s.unpack_from("<I", img, afunc + 4 * ordv)
        end = img.index(b"\0", nrva)
        out[img[nrva:end].decode("latin-1")] = frva
    return out


def find_export(pe, needle):
    n = needle.decode() if isinstance(needle, bytes) else needle
    return [(k, v) for k, v in export_table(pe).items() if n in k]


def cstr(data, rva, limit=96):
    end = data.find(b"\x00", rva, rva + limit)
    if end < 0:
        return None
    s = data[rva:end]
    if not s or not all(32 <= c < 127 for c in s):
        return None
    return s.decode()


def main():
    pe, data = load("Game.dll")
    cands = find_export(pe, b"LoadFromDatabase@GameEngine")
    if not cands:
        print("LoadFromDatabase not found")
        return 1
    for name, rva in cands:
        print("export %s -> rva 0x%X" % (name, rva))
    name, rva = cands[0]
    # The registrations the mod already depends on (0x2FFE19 ItemDescription ..
    # 0x3026E2 ItemUpgrade) lie well past LoadFromDatabase's own body, so the
    # scan range is given explicitly when the caller wants the whole table.
    if len(sys.argv) >= 3:
        rva = int(sys.argv[1], 16)
        end = int(sys.argv[2], 16)
    else:
        end = rva + 0x20000
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    pending = None       # (rva_of_lea, string)
    rows = []
    for ins in md.disasm(data[rva:end], rva):
        if ins.mnemonic == "lea" and len(ins.operands) == 2:
            op0, op1 = ins.operands
            if (op0.type == capstone.x86.X86_OP_REG
                    and op1.type == capstone.x86.X86_OP_MEM
                    and op1.mem.base == capstone.x86.X86_REG_RIP):
                tgt = ins.address + ins.size + op1.mem.disp
                s = cstr(data, tgt)
                if s and s[0].isalpha() and len(s) >= 3:
                    pending = (ins.address, s)
        elif ins.mnemonic == "mov" and len(ins.operands) == 2 and pending:
            op0, op1 = ins.operands
            if (op0.type == capstone.x86.X86_OP_MEM
                    and op1.type == capstone.x86.X86_OP_IMM
                    and op0.size == 4
                    and 0 <= op1.imm <= 0x80
                    and ins.address - pending[0] < 0x40):
                rows.append((op1.imm, pending[1], pending[0]))
                pending = None
    seen = {}
    for cls, s, site in rows:
        seen.setdefault(cls, (s, site))
    print("\n%d classes" % len(seen))
    for cls in sorted(seen):
        s, site = seen[cls]
        print("  0x%02X  %-28s (lea at 0x%X)" % (cls, s, site))
    return 0


if __name__ == "__main__":
    sys.exit(main())
