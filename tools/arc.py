"""Reader for Grim Dawn ARC v3 resource archives (Text_EN.arc, Items.arc, UI.arc ...).

Layout verified against resources/Text_EN.arc (magic 'ARC\\0', version 3):
  header 28 bytes '<7I': magic, version, numFileEntries, numDataRecords,
                         recordTableSize, stringTableSize, recordTableOffset
  data-record table at recordTableOffset:  numDataRecords * '<3I' (partOffset, csize, dsize)
  string table       at recordTableOffset + recordTableSize (stringTableSize bytes, '\\0'-separated)
  file-entry table   at recordTableOffset + recordTableSize + stringTableSize:
      numFileEntries * '<5I Q 4I' = 44 bytes:
        entryType, fileOffset, compressedSize, decompressedSize, crc/hash,
        fileTime(u64), numParts, firstPartIndex, nameLength, nameOffset
  Each part is an LZ4 block; a part whose csize == dsize is stored raw.

Only listing + extraction are implemented (the prototype never writes archives).
"""

from __future__ import annotations

import os
import struct
from typing import Dict, Iterator, List, Optional

import lz4.block

HEADER = struct.Struct("<7I")
DATAREC = struct.Struct("<3I")
FILEENT = struct.Struct("<IIIIIQIIII")
ARC_MAGIC = 0x00435241


class ArcEntry:
    __slots__ = ("name", "etype", "offset", "csize", "dsize", "num_parts",
                 "first_part", "ftime")

    def __init__(self, name, etype, offset, csize, dsize, num_parts, first_part, ftime):
        self.name = name
        self.etype = etype
        self.offset = offset
        self.csize = csize
        self.dsize = dsize
        self.num_parts = num_parts
        self.first_part = first_part
        self.ftime = ftime

    def __repr__(self):
        return f"<ArcEntry {self.name} {self.dsize}B parts={self.num_parts}>"


class ArcArchive:
    def __init__(self, path: str, tag: Optional[str] = None):
        self.path = path
        self.tag = tag or os.path.splitext(os.path.basename(path))[0]
        with open(path, "rb") as fh:
            self._blob = fh.read()
        (magic, self.version, n_files, n_recs,
         rec_size, str_size, rec_off) = HEADER.unpack_from(self._blob, 0)
        if magic != ARC_MAGIC or self.version != 3:
            raise ValueError(f"{path}: not an ARC v3 archive (magic={magic:#x} ver={self.version})")
        b = self._blob
        self._parts = [DATAREC.unpack_from(b, rec_off + 12 * i) for i in range(n_recs)]
        str_off = rec_off + rec_size
        ent_off = str_off + str_size
        self.entries: Dict[str, ArcEntry] = {}
        self._order: List[str] = []
        for i in range(n_files):
            (etype, foff, csize, dsize, _crc, ftime,
             nparts, first_part, nlen, noff) = FILEENT.unpack_from(b, ent_off + 44 * i)
            name = b[str_off + noff: str_off + noff + nlen].decode("latin-1")
            name = name.replace("\\", "/")
            e = ArcEntry(name, etype, foff, csize, dsize, nparts, first_part, ftime)
            self.entries[name.lower()] = e
            self._order.append(name.lower())

    def __len__(self) -> int:
        return len(self.entries)

    def __contains__(self, name: str) -> bool:
        return name.replace("\\", "/").lower() in self.entries

    def keys(self) -> Iterator[str]:
        return iter(self._order)

    def names(self) -> List[str]:
        return [self.entries[k].name for k in self._order]

    def read(self, name: str) -> Optional[bytes]:
        e = self.entries.get(name.replace("\\", "/").lower())
        if e is None:
            return None
        if e.num_parts == 0:                      # stored whole, uncompressed
            return self._blob[e.offset:e.offset + e.dsize]
        out = bytearray()
        for i in range(e.first_part, e.first_part + e.num_parts):
            poff, pcs, pds = self._parts[i]
            chunk = self._blob[poff:poff + pcs]
            out += chunk if pcs == pds else lz4.block.decompress(chunk, uncompressed_size=pds)
        return bytes(out)


class ArcSet:
    """Several ARCs of the same kind stacked in load order (later wins)."""

    def __init__(self, archives: List[ArcArchive]):
        self.archives = archives
        self._index: Dict[str, int] = {}
        for i, a in enumerate(archives):
            for k in a.keys():
                self._index[k] = i

    @classmethod
    def load(cls, game_dir: str, rel_name: str) -> "ArcSet":
        """rel_name like 'resources/Text_EN.arc'; searches ., gdx1, gdx2, gdx3."""
        arcs = []
        for tag, sub in (("database", ""), ("gdx1", "gdx1"), ("gdx2", "gdx2"), ("gdx3", "gdx3")):
            p = os.path.join(game_dir, sub, rel_name) if sub else os.path.join(game_dir, rel_name)
            if os.path.isfile(p):
                arcs.append(ArcArchive(p, tag))
        if not arcs:
            raise FileNotFoundError(f"no {rel_name} under {game_dir}")
        return cls(arcs)

    def __contains__(self, name: str) -> bool:
        return name.replace("\\", "/").lower() in self._index

    def __len__(self) -> int:
        return len(self._index)

    def keys(self) -> Iterator[str]:
        return iter(self._index)

    def source_of(self, name: str) -> Optional[str]:
        i = self._index.get(name.replace("\\", "/").lower())
        return self.archives[i].tag if i is not None else None

    def read(self, name: str) -> Optional[bytes]:
        i = self._index.get(name.replace("\\", "/").lower())
        return self.archives[i].read(name) if i is not None else None


def parse_tag_file(data: bytes) -> Dict[str, str]:
    """`tagName=Text` lines; skips comments/blank lines. Latin-1 with a UTF-8 fallback."""
    try:
        text = data.decode("utf-8-sig")
    except UnicodeDecodeError:
        text = data.decode("latin-1")
    out: Dict[str, str] = {}
    for line in text.splitlines():
        if not line or line.startswith("//") or line.startswith("#") or "=" not in line:
            continue
        k, _, v = line.partition("=")
        k = k.strip()
        if k and not k.startswith("<"):
            out[k] = v
    return out


def load_text_tags(game_dir: str, lang: str = "EN") -> Dict[str, str]:
    """Merged tag -> English text over the four Text_<lang>.arc archives (later wins)."""
    tags: Dict[str, str] = {}
    aset = ArcSet.load(game_dir, os.path.join("resources", f"Text_{lang}.arc"))
    for arc in aset.archives:                 # base first, gdx3 last
        for key in arc.keys():
            if not key.endswith(".txt"):
                continue
            data = arc.read(key)
            if data:
                tags.update(parse_tag_file(data))
    return tags


if __name__ == "__main__":
    import sys

    import gdpath

    GAME = gdpath.game_dir()
    if len(sys.argv) > 1 and sys.argv[1] == "tags":
        t = load_text_tags(GAME)
        print(f"{len(t)} tags")
        for k in sys.argv[2:]:
            print(f"{k} = {t.get(k)!r}")
    else:
        rel = sys.argv[1] if len(sys.argv) > 1 else os.path.join("resources", "Text_EN.arc")
        aset = ArcSet.load(GAME, rel)
        for a in aset.archives:
            print(f"{a.tag:9s} {len(a):7d} files  {a.path}")
        print(f"{'merged':9s} {len(aset):7d} files")
        for k in list(aset.keys())[:20]:
            print("  ", k)
