"""pe_imports.py - the import table of a PE, one DLL per line.

Reads the file directly, so it needs no toolchain: `python tools\\pe_imports.py bin\\uniquetab.asi`.
Used to prove which C runtime pieces the binary actually pulls in - VCRUNTIME140_1.dll, for
instance, is imported only when the compiler emits FH4 exception tables.

With -v every imported name is listed under its DLL.
"""

import struct
import sys


def read_u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def read_u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def sections(b):
    lfanew = read_u32(b, 0x3C)
    if b[lfanew:lfanew + 4] != b"PE\0\0":
        raise SystemExit("not a PE file")
    n_sect = read_u16(b, lfanew + 6)
    opt_size = read_u16(b, lfanew + 20)
    opt = lfanew + 24
    magic = read_u16(b, opt)
    # data directory entry 1 is the import table (entry 0 is the exports)
    dir_off = opt + (112 if magic == 0x20B else 96) + 8
    imp_rva = read_u32(b, dir_off)
    sect = []
    o = opt + opt_size
    for _ in range(n_sect):
        va = read_u32(b, o + 12)
        vsize = read_u32(b, o + 8)
        raw = read_u32(b, o + 20)
        rsize = read_u32(b, o + 16)
        sect.append((va, vsize, raw, rsize))
        o += 40
    return sect, imp_rva, magic


def to_off(sect, rva):
    for va, vsize, raw, rsize in sect:
        if va <= rva < va + max(vsize, rsize):
            return raw + (rva - va)
    return None


def cstr(b, o):
    end = b.index(b"\0", o)
    return b[o:end].decode("ascii", "replace")


def imports(path):
    with open(path, "rb") as f:
        b = f.read()
    sect, imp_rva, magic = sections(b)
    if not imp_rva:
        return []
    o = to_off(sect, imp_rva)
    out = []
    while True:
        orig_thunk = read_u32(b, o)
        name_rva = read_u32(b, o + 12)
        first_thunk = read_u32(b, o + 16)
        if not name_rva and not first_thunk:
            break
        dll = cstr(b, to_off(sect, name_rva))
        names = []
        thunk = to_off(sect, orig_thunk or first_thunk)
        step = 8 if magic == 0x20B else 4
        fmt = "<Q" if step == 8 else "<I"
        high = 1 << (63 if step == 8 else 31)
        while True:
            v = struct.unpack_from(fmt, b, thunk)[0]
            if not v:
                break
            if v & high:
                names.append("#%d" % (v & 0xFFFF))
            else:
                names.append(cstr(b, to_off(sect, (v & 0x7FFFFFFF) + 2)))
            thunk += step
        out.append((dll, names))
        o += 20
    return out


def main(argv):
    verbose = "-v" in argv
    args = [a for a in argv[1:] if a != "-v"]
    if not args:
        print(__doc__)
        return 2
    for path in args:
        print(path)
        for dll, names in imports(path):
            print("  %s  (%d)" % (dll, len(names)))
            if verbose:
                for n in names:
                    print("      %s" % n)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
