"""Writer for Grim Dawn ARZ v3 database archives (mirror of tools/arz.py's reader).

Layout produced (identical to the shipped archives, verified against database.arz,
GDX3.arz and a community-built basemod database.arz):

    0                   24-byte header  '<HHIIIII'
                        unk=2, version=3, recordTableStart, recordTableSize,
                        recordCount, stringTableStart, stringTableSize
    24                  record data blobs, each lz4-block compressed, back to back
    recordTableStart    record table: per record
                        stringIdx(u32), typeLen(u32), type bytes,
                        offset(u32, relative to 24), csize(u32), dsize(u32), mtime(u64)
    stringTableStart    string table: count(u32) then count * (len(u32), bytes)
    end                 16 trailing bytes

The engine's only header validation (Engine.dll `DatabaseArchive` + 0x27FC50, called
from `Engine::LoadDatabase`) is `unk >= this->minVersion` plus a self-consistent
version test that always passes; **the 16 trailing bytes are never read at load
time**, so they are written as zeros here.

Field encoding inside a record blob: (type u16, count u16, nameIdx u32, values u32*count)
with type 0 int32, 1 float32, 2 string-table index, 3 bool(int32).
"""

from __future__ import annotations

import struct
from typing import Dict, List, Sequence, Tuple

import lz4.block

from arz import ArzArchive, FT_BOOL, FT_FLOAT, FT_INT, FT_STRING, HEADER

# A fixed FILETIME so rebuilds are byte-identical (2026-09-06 00:00:00 UTC).
DEFAULT_MTIME = 133707744000000000

Field = Tuple[str, int, Sequence]           # (name, type, values)


def read_typed(arc: ArzArchive, name: str) -> List[Field]:
    """Decode one record keeping field order and field TYPE (arz.py drops the type)."""
    entry = arc.entries[name.replace("\\", "/").lower()]
    raw = lz4.block.decompress(arc._blob[24 + entry.offset:24 + entry.offset + entry.csize],
                               uncompressed_size=entry.dsize)
    out: List[Field] = []
    p, n = 0, len(raw)
    while p + 8 <= n:
        ftype, count, name_idx = struct.unpack_from("<HHI", raw, p)
        p += 8
        if ftype == FT_FLOAT:
            vals = list(struct.unpack_from("<%df" % count, raw, p)) if count else []
        elif ftype == FT_STRING:
            vals = [arc.strings[i] for i in struct.unpack_from("<%dI" % count, raw, p)]
        else:
            vals = list(struct.unpack_from("<%di" % count, raw, p)) if count else []
        p += 4 * count
        out.append((arc.strings[name_idx], ftype, vals))
    return out


class ArzWriter:
    def __init__(self) -> None:
        self._strings: List[str] = []
        self._sidx: Dict[str, int] = {}
        self._records: List[Tuple[int, str, bytes, int, int]] = []   # nameIdx,type,blob,dsize,mtime

    def _s(self, text: str) -> int:
        i = self._sidx.get(text)
        if i is None:
            i = len(self._strings)
            self._sidx[text] = i
            self._strings.append(text)
        return i

    def add(self, record_name: str, fields: Sequence[Field],
            rtype: str = "", mtime: int = DEFAULT_MTIME) -> None:
        name_idx = self._s(record_name)
        body = bytearray()
        for fname, ftype, vals in fields:
            vals = list(vals)
            body += struct.pack("<HHI", ftype, len(vals), self._s(fname))
            if ftype == FT_FLOAT:
                body += struct.pack("<%df" % len(vals), *[float(v) for v in vals])
            elif ftype == FT_STRING:
                body += struct.pack("<%dI" % len(vals), *[self._s(str(v)) for v in vals])
            elif ftype in (FT_INT, FT_BOOL):
                body += struct.pack("<%di" % len(vals), *[int(v) for v in vals])
            else:
                raise ValueError("unsupported field type %r for %s" % (ftype, fname))
        raw = bytes(body)
        blob = lz4.block.compress(raw, store_size=False)
        self._records.append((name_idx, rtype, blob, len(raw), mtime))

    def build(self) -> bytes:
        data = bytearray()
        offsets = []
        for _, _, blob, _, _ in self._records:
            offsets.append(len(data))
            data += blob
        rec_table = bytearray()
        for (name_idx, rtype, blob, dsize, mtime), off in zip(self._records, offsets):
            rt = rtype.encode("latin-1")
            rec_table += struct.pack("<II", name_idx, len(rt)) + rt
            rec_table += struct.pack("<IIIQ", off, len(blob), dsize, mtime)
        str_table = bytearray(struct.pack("<I", len(self._strings)))
        for s in self._strings:
            b = s.encode("latin-1")
            str_table += struct.pack("<I", len(b)) + b
        rec_start = 24 + len(data)
        str_start = rec_start + len(rec_table)
        head = HEADER.pack(2, 3, rec_start, len(rec_table), len(self._records),
                           str_start, len(str_table))
        return bytes(head) + bytes(data) + bytes(rec_table) + bytes(str_table) + b"\0" * 16

    def write(self, path: str) -> int:
        blob = self.build()
        with open(path, "wb") as fh:
            fh.write(blob)
        return len(blob)


def dbr_text(fields: Sequence[Field]) -> str:
    """The .dbr source form the game's own tools emit: `key,value,` per line, ';' arrays."""
    lines = []
    for fname, ftype, vals in fields:
        if ftype == FT_FLOAT:
            v = ";".join(("%g" % float(x)) for x in vals)
        else:
            v = ";".join(str(x) for x in vals)
        lines.append("%s,%s," % (fname, v))
    return "\n".join(lines) + "\n"
