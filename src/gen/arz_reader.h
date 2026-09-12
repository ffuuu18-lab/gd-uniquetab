// arz_reader.h - reader for the game's ARZ database archives (format version 3).
//
// Layout (little endian):
//   header 24 B: unk u16, version u16 (3), recordTableStart u32, recordTableSize u32,
//                recordCount u32, stringTableStart u32, stringTableSize u32
//   string table: count u32, then count x (len u32, bytes)
//   record table: recordCount x (nameIdx u32, typeLen u32, type bytes, offset u32,
//                 csize u32, dsize u32, time u64); record data lives at 24 + offset
//   record data: one LZ4 block (dsize bytes) holding fields of
//                type u16, count u16, nameIdx u32, count x u32
//                type 0 int32, 1 float32, 2 string-table index, 3 bool (int32)
//
// Every read is bounds-checked: a corrupt file makes load() return false with a message,
// nothing here throws. tools/arz.py is the Python reference this mirrors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace gen {

// Decodes one raw LZ4 block into exactly dstLen bytes. False on any malformed input.
bool lz4BlockDecode(const std::uint8_t* src, std::size_t srcLen,
                    std::uint8_t* dst, std::size_t dstLen);

// Bytes of a latin-1 string re-encoded as UTF-8 (the archives are latin-1; the outputs UTF-8).
std::string latin1ToUtf8(const std::uint8_t* p, std::size_t n);

enum : std::uint16_t { kFtInt = 0, kFtFloat = 1, kFtString = 2, kFtBool = 3 };

struct ArzField {
    std::string name;
    std::uint16_t type = 0;
    std::vector<std::uint32_t> raw;       // the u32 words as stored
    std::vector<std::string> strs;        // resolved for type 2, else empty
};

struct ArzRecord {
    std::vector<ArzField> fields;         // in file order; the LAST field of a name wins
    const ArzField* find(const char* name) const;
    std::string str(const char* name) const;     // first string of the field, "" if absent
    std::int32_t i32(const char* name) const;    // first value as int, 0 if absent
};

struct ArzEntry {
    std::string name;                     // path with '/' separators, original case
    std::string key;                      // lower-cased name
    std::string rtype;
    std::uint32_t offset = 0, csize = 0, dsize = 0;
    std::uint64_t mtime = 0;
};

class ArzArchive {
public:
    bool load(const std::string& path, std::string* error);
    const std::string& path() const { return m_path; }
    const std::string& tag() const { return m_tag; }
    void setTag(const std::string& t) { m_tag = t; }
    std::size_t recordCount() const { return m_entries.size(); }
    std::size_t stringCount() const { return m_strings.size(); }
    const std::vector<ArzEntry>& entries() const { return m_entries; }
    const std::vector<std::string>& strings() const { return m_strings; }
    // Entry by key ("records/..." any case, '\\' or '/'); nullptr when absent.
    const ArzEntry* entry(const std::string& name) const;
    // Decodes one entry. False when the block is corrupt.
    bool decode(const ArzEntry& e, ArzRecord& out) const;
    static std::string normKey(const std::string& name);

private:
    std::string m_path, m_tag;
    std::vector<std::uint8_t> m_blob;
    std::vector<std::string> m_strings;
    std::vector<ArzEntry> m_entries;
    std::unordered_map<std::string, std::size_t> m_index;
};

// database.arz < GDX1 < GDX2 < GDX3: the last archive that has a record wins.
class ArzDatabase {
public:
    // Loads the archives that exist under gameDir in load order; false when none loads.
    bool loadGame(const std::string& gameDir, std::string* error);
    const std::vector<ArzArchive>& archives() const { return m_archives; }
    std::size_t size() const { return m_index.size(); }
    // The winning archive index and entry for a key, or false.
    bool lookup(const std::string& name, std::size_t* archive, const ArzEntry** entry) const;
    bool get(const std::string& name, ArzRecord& out) const;
    // Every winning (archive index, entry) in first-seen order.
    const std::vector<std::pair<std::size_t, const ArzEntry*>>& winners() const { return m_winners; }
    // The relative paths this reader looks for, in load order.
    static std::vector<std::pair<std::string, std::string>> loadOrder();

private:
    std::vector<ArzArchive> m_archives;
    std::unordered_map<std::string, std::size_t> m_index;        // key -> index in m_winners
    std::vector<std::pair<std::size_t, const ArzEntry*>> m_winners;
};

} // namespace gen
