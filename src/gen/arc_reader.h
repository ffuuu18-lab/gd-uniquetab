// arc_reader.h - reader for the game's ARC v3 resource archives (Text_EN.arc, Items.arc,
// UI.arc and the expansions' copies).
//
// Layout (little endian):
//   header 28 B: magic 'ARC\0', version 3, numFileEntries, numDataRecords, recordTableSize,
//                stringTableSize, recordTableOffset
//   data-record table at recordTableOffset: numDataRecords x (partOffset, csize, dsize)
//   string table at recordTableOffset + recordTableSize ('\0'-separated names)
//   file-entry table after the string table: numFileEntries x 44 B
//                (entryType, fileOffset, csize, dsize, hash, fileTime u64, numParts,
//                 firstPart, nameLength, nameOffset)
//   a part is an LZ4 block; a part whose csize == dsize is stored raw; numParts == 0 means
//   the whole entry is stored raw at fileOffset.
//
// Bounds-checked like arz_reader; a corrupt archive fails to load, nothing throws.
// tools/arc.py is the Python reference this mirrors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace gen {

struct ArcEntry {
    std::string name;                     // '/' separators, original case
    std::string key;                      // lower-cased name
    std::uint32_t etype = 0, offset = 0, csize = 0, dsize = 0, numParts = 0, firstPart = 0;
    std::uint64_t ftime = 0;
};

class ArcArchive {
public:
    bool load(const std::string& path, std::string* error);
    const std::string& path() const { return m_path; }
    const std::string& tag() const { return m_tag; }
    void setTag(const std::string& t) { m_tag = t; }
    std::size_t size() const { return m_entries.size(); }
    const std::vector<ArcEntry>& entries() const { return m_entries; }
    const ArcEntry* entry(const std::string& name) const;
    bool read(const ArcEntry& e, std::vector<std::uint8_t>& out) const;
    static std::string normKey(const std::string& name);

private:
    struct Part { std::uint32_t offset, csize, dsize; };
    std::string m_path, m_tag;
    std::vector<std::uint8_t> m_blob;
    std::vector<Part> m_parts;
    std::vector<ArcEntry> m_entries;
    std::unordered_map<std::string, std::size_t> m_index;
};

// The same archive name under ., gdx1, gdx2, gdx3; the last one that has an entry wins.
class ArcSet {
public:
    bool load(const std::string& gameDir, const std::string& relName, std::string* error);
    const std::vector<ArcArchive>& archives() const { return m_archives; }
    std::size_t size() const { return m_index.size(); }
    bool contains(const std::string& name) const;
    // Tag of the winning archive, "" when absent.
    std::string sourceOf(const std::string& name) const;
    bool read(const std::string& name, std::vector<std::uint8_t>& out) const;

private:
    std::vector<ArcArchive> m_archives;
    std::unordered_map<std::string, std::size_t> m_index;      // key -> archive index
};

// `tag=Text` lines of one tag file, appended to tags (later lines win). The file is UTF-8
// (a BOM is dropped); when it is not valid UTF-8 it is read as latin-1.
void parseTagFile(const std::vector<std::uint8_t>& data,
                  std::unordered_map<std::string, std::string>& tags);

// Every .txt of the four Text_<lang>.arc merged, base first, so a later archive wins.
bool loadTextTags(const std::string& gameDir, const std::string& lang,
                  std::unordered_map<std::string, std::string>& tags, std::string* error);

} // namespace gen
