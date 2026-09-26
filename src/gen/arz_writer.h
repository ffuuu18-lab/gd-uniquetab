// arz_writer.h - writes an ARZ v3 database archive, the format arz_reader.h reads.
//
// Port of tools/arzw.py, which is what built the shipped uniq-pages.arz; that file's header is
// the authoritative description of the layout. The record data is emitted as one LZ4 block per
// record holding LITERALS ONLY - a valid block every LZ4 decoder accepts, and the engine's
// included, because the last sequence of a block is allowed to be literals with no match. It
// costs file size and no correctness: the overlay is written once per game patch and read once
// per launch.
//
// Strings are interned in first-use order, exactly as the Python writer does, so the two
// archives carry the same string table for the same record set. The bodies still differ,
// because LZ4 has many valid encodings of the same bytes; identity is checked by DECODING both.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace gen {

// One field as it goes into a record: int and bool use raw, float uses raw (the stored bit
// pattern, so a copied field is bit-preserving), string uses strs.
struct ArzOutField {
    std::string name;
    std::uint16_t type = 0;
    std::vector<std::uint32_t> raw;
    std::vector<std::string> strs;
};

ArzOutField outInt(const char* name, std::int32_t v);
ArzOutField outStr(const char* name, const std::string& v);
ArzOutField outStrs(const char* name, const std::vector<std::string>& v);

// The bytes of a UTF-8 string back in latin-1, which is what the archives hold. A code point
// above U+00FF cannot be stored and becomes '?'; no record this writer emits has one.
std::string utf8ToLatin1(const std::string& s);

// src as one LZ4 block of literals. Appends to out.
void lz4BlockStoreLiterals(const std::uint8_t* src, std::size_t n, std::vector<std::uint8_t>& out);

class ArzWriter {
public:
    // The fixed FILETIME tools/arzw.py stamps a record with when the caller has no better one,
    // so a rebuild of the same input is the same file.
    static const std::uint64_t kDefaultMtime = 133707744000000000ull;

    void add(const std::string& record, const std::vector<ArzOutField>& fields,
             const std::string& rtype = std::string(), std::uint64_t mtime = kDefaultMtime);
    std::size_t recordCount() const { return m_records.size(); }
    std::size_t stringCount() const { return m_strings.size(); }
    // The whole archive. Empty only when nothing was added.
    std::vector<std::uint8_t> build() const;

private:
    struct Rec {
        std::uint32_t nameIdx = 0;
        std::string rtype;
        std::vector<std::uint8_t> blob;
        std::uint32_t dsize = 0;
        std::uint64_t mtime = 0;
    };
    std::uint32_t intern(const std::string& text);

    std::vector<std::string> m_strings;                          // latin-1, in first-use order
    std::unordered_map<std::string, std::uint32_t> m_index;
    std::vector<Rec> m_records;
};

} // namespace gen
