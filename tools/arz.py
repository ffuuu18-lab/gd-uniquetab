"""Reusable reader for Grim Dawn ARZ database archives (1.3.0.8, format version 3).

Format (measured against the shipped databases):
  header 24 bytes, little endian '<HHIIIII':
      unk(u16), version(u16, =3), recordTableStart(u32), recordTableSize(u32),
      recordCount(u32), stringTableStart(u32), stringTableSize(u32)
  string table at stringTableStart: count(u32) then count * (len(u32), bytes)
  record table at recordTableStart: recordCount entries of
      stringIdx(u32)          -> record path, e.g. records/items/...dbr
      typeLen(u32), type(bytes)
      offset(u32)             -> data at 24 + offset
      csize(u32), dsize(u32)  -> compressed / uncompressed size
      time(u64)               -> FILETIME
  record data = lz4.block.decompress(blob, uncompressed_size=dsize), a stream of
      type(u16), count(u16), nameIdx(u32), values(u32 * count)
      type 0 = int32, 1 = float32, 2 = string-table index, 3 = bool(int32)

Usage:
    from arz import ArzArchive, ArzDatabase
    db = ArzDatabase.load_game(GAME_DIR)   # database.arz < GDX1 < GDX2 < GDX3
    rec = db.get("records/items/.../foo.dbr")   # dict[str, list|scalar]
"""

from __future__ import annotations

import os
import struct
from typing import Dict, Iterator, List, Optional, Tuple

import lz4.block

HEADER = struct.Struct("<HHIIIII")

FT_INT = 0
FT_FLOAT = 1
FT_STRING = 2
FT_BOOL = 3

# Load order: later archives override earlier ones (same rule the engine uses).
ARZ_LOAD_ORDER = [
    ("database", os.path.join("database", "database.arz")),
    ("gdx1", os.path.join("gdx1", "database", "GDX1.arz")),
    ("gdx2", os.path.join("gdx2", "database", "GDX2.arz")),
    ("gdx3", os.path.join("gdx3", "database", "GDX3.arz")),
]


class ArzRecordEntry:
    __slots__ = ("name", "rtype", "offset", "csize", "dsize", "mtime")

    def __init__(self, name: str, rtype: str, offset: int, csize: int, dsize: int, mtime: int):
        self.name = name
        self.rtype = rtype
        self.offset = offset
        self.csize = csize
        self.dsize = dsize
        self.mtime = mtime


class ArzArchive:
    """One .arz file. Records are decompressed lazily and cached."""

    def __init__(self, path: str, tag: Optional[str] = None):
        self.path = path
        self.tag = tag or os.path.splitext(os.path.basename(path))[0]
        with open(path, "rb") as fh:
            self._blob = fh.read()
        (self.unk, self.version, self.rec_start, self.rec_size,
         self.rec_count, self.str_start, self.str_size) = HEADER.unpack_from(self._blob, 0)
        if self.version != 3:
            raise ValueError(f"{path}: unexpected ARZ version {self.version}")
        self.strings: List[str] = self._read_string_table()
        self.entries: Dict[str, ArzRecordEntry] = {}
        self._order: List[str] = []
        self._read_record_table()
        self._cache: Dict[str, Dict[str, object]] = {}

    # -- tables -----------------------------------------------------------
    def _read_string_table(self) -> List[str]:
        b = self._blob
        p = self.str_start
        (count,) = struct.unpack_from("<I", b, p)
        p += 4
        out: List[str] = []
        append = out.append
        unpack_from = struct.unpack_from
        for _ in range(count):
            (ln,) = unpack_from("<I", b, p)
            p += 4
            append(b[p:p + ln].decode("latin-1"))
            p += ln
        return out

    def _read_record_table(self) -> None:
        b = self._blob
        p = self.rec_start
        strings = self.strings
        for _ in range(self.rec_count):
            (name_idx,) = struct.unpack_from("<I", b, p)
            p += 4
            (type_len,) = struct.unpack_from("<I", b, p)
            p += 4
            rtype = b[p:p + type_len].decode("latin-1")
            p += type_len
            offset, csize, dsize = struct.unpack_from("<III", b, p)
            p += 12
            (mtime,) = struct.unpack_from("<Q", b, p)
            p += 8
            name = strings[name_idx].replace("\\", "/")
            key = name.lower()
            self.entries[key] = ArzRecordEntry(name, rtype, offset, csize, dsize, mtime)
            self._order.append(key)

    # -- records ----------------------------------------------------------
    def __contains__(self, name: str) -> bool:
        return name.replace("\\", "/").lower() in self.entries

    def __len__(self) -> int:
        return len(self.entries)

    def keys(self) -> Iterator[str]:
        """Record keys (lower-cased paths) in archive order."""
        return iter(self._order)

    def real_name(self, name: str) -> Optional[str]:
        e = self.entries.get(name.replace("\\", "/").lower())
        return e.name if e else None

    def get(self, name: str) -> Optional[Dict[str, object]]:
        key = name.replace("\\", "/").lower()
        hit = self._cache.get(key)
        if hit is not None:
            return hit
        entry = self.entries.get(key)
        if entry is None:
            return None
        rec = self._decode(entry)
        self._cache[key] = rec
        return rec

    def _decode(self, entry: ArzRecordEntry) -> Dict[str, object]:
        start = 24 + entry.offset
        raw = lz4.block.decompress(self._blob[start:start + entry.csize],
                                   uncompressed_size=entry.dsize)
        out: Dict[str, object] = {}
        strings = self.strings
        p = 0
        n = len(raw)
        unpack_from = struct.unpack_from
        while p + 8 <= n:
            ftype, count, name_idx = unpack_from("<HHI", raw, p)
            p += 8
            end = p + 4 * count
            if ftype == FT_INT or ftype == FT_BOOL:
                vals = list(unpack_from("<%di" % count, raw, p)) if count else []
            elif ftype == FT_FLOAT:
                vals = list(unpack_from("<%df" % count, raw, p)) if count else []
            elif ftype == FT_STRING:
                idx = unpack_from("<%dI" % count, raw, p) if count else ()
                vals = [strings[i] if i < len(strings) else "" for i in idx]
            else:
                vals = list(unpack_from("<%dI" % count, raw, p)) if count else []
            p = end
            field = strings[name_idx] if name_idx < len(strings) else "?%d" % name_idx
            out[field] = vals[0] if count == 1 else vals
        return out


class ArzDatabase:
    """Several archives stacked in load order; the last one that has a record wins."""

    def __init__(self, archives: List[ArzArchive]):
        self.archives = archives            # in load order, lowest priority first
        self._index: Dict[str, int] = {}    # record key -> index of winning archive
        for i, arc in enumerate(archives):
            for key in arc.keys():
                self._index[key] = i

    @classmethod
    def load_game(cls, game_dir: str, tags: Optional[List[str]] = None) -> "ArzDatabase":
        arcs: List[ArzArchive] = []
        for tag, rel in ARZ_LOAD_ORDER:
            if tags is not None and tag not in tags:
                continue
            full = os.path.join(game_dir, rel)
            if os.path.isfile(full):
                arcs.append(ArzArchive(full, tag))
        if not arcs:
            raise FileNotFoundError(f"no .arz archives found under {game_dir}")
        return cls(arcs)

    def __len__(self) -> int:
        return len(self._index)

    def __contains__(self, name: str) -> bool:
        return name.replace("\\", "/").lower() in self._index

    def keys(self) -> Iterator[str]:
        return iter(self._index)

    def source_of(self, name: str) -> Optional[str]:
        i = self._index.get(name.replace("\\", "/").lower())
        return self.archives[i].tag if i is not None else None

    def real_name(self, name: str) -> Optional[str]:
        i = self._index.get(name.replace("\\", "/").lower())
        return self.archives[i].real_name(name) if i is not None else None

    def get(self, name: str) -> Optional[Dict[str, object]]:
        i = self._index.get(name.replace("\\", "/").lower())
        return self.archives[i].get(name) if i is not None else None

    def items(self) -> Iterator[Tuple[str, str, Dict[str, object]]]:
        """Yield (key, source tag, record) for every winning record."""
        for key, i in self._index.items():
            arc = self.archives[i]
            rec = arc.get(key)
            if rec is not None:
                yield key, arc.tag, rec

    def overridden(self) -> Dict[str, List[str]]:
        """Records present in more than one archive -> list of tags (load order)."""
        seen: Dict[str, List[str]] = {}
        for arc in self.archives:
            for key in arc.keys():
                seen.setdefault(key, []).append(arc.tag)
        return {k: v for k, v in seen.items() if len(v) > 1}


def sfield(rec: Dict[str, object], name: str, default: str = "") -> str:
    v = rec.get(name, default)
    if isinstance(v, list):
        v = v[0] if v else default
    return v if isinstance(v, str) else (default if v is None else str(v))


def ifield(rec: Dict[str, object], name: str, default: int = 0) -> int:
    v = rec.get(name, default)
    if isinstance(v, list):
        v = v[0] if v else default
    try:
        return int(v)
    except (TypeError, ValueError):
        return default


def ffield(rec: Dict[str, object], name: str, default: float = 0.0) -> float:
    v = rec.get(name, default)
    if isinstance(v, list):
        v = v[0] if v else default
    try:
        return float(v)
    except (TypeError, ValueError):
        return default


if __name__ == "__main__":  # smoke test / CLI dump
    import sys
    import json

    import gdpath

    GAME = gdpath.game_dir()
    db = ArzDatabase.load_game(GAME)
    if len(sys.argv) > 1:
        rec = db.get(sys.argv[1])
        if rec is None:
            print("not found:", sys.argv[1])
            sys.exit(1)
        print("# source:", db.source_of(sys.argv[1]))
        print(json.dumps(rec, indent=1, default=str))
    else:
        for arc in db.archives:
            print(f"{arc.tag:9s} {len(arc):7d} records  strings={len(arc.strings):7d}  {arc.path}")
        print(f"{'merged':9s} {len(db):7d} records")
