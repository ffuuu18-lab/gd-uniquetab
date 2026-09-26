// arz_writer.cpp - see arz_writer.h.
#include "gen/arz_writer.h"

#include <cstring>

namespace gen {

namespace {

void put16(std::vector<std::uint8_t>& v, std::uint16_t x) {
    v.push_back(std::uint8_t(x));
    v.push_back(std::uint8_t(x >> 8));
}
void put32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(std::uint8_t(x >> (8 * i)));
}
void put64(std::vector<std::uint8_t>& v, std::uint64_t x) {
    for (int i = 0; i < 8; ++i) v.push_back(std::uint8_t(x >> (8 * i)));
}
void putBytes(std::vector<std::uint8_t>& v, const std::string& s) {
    v.insert(v.end(), s.begin(), s.end());
}

} // namespace

ArzOutField outInt(const char* name, std::int32_t v) {
    ArzOutField f;
    f.name = name;
    f.type = 0;                                   // kFtInt
    f.raw.push_back(std::uint32_t(v));
    return f;
}

ArzOutField outStr(const char* name, const std::string& v) {
    ArzOutField f;
    f.name = name;
    f.type = 2;                                   // kFtString
    f.strs.push_back(v);
    return f;
}

ArzOutField outStrs(const char* name, const std::vector<std::string>& v) {
    ArzOutField f;
    f.name = name;
    f.type = 2;
    f.strs = v;
    return f;
}

std::string utf8ToLatin1(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        std::uint8_t c = (std::uint8_t)s[i];
        if (c < 0x80) {
            o.push_back(char(c));
        } else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
            std::uint32_t cp = (std::uint32_t(c & 0x1F) << 6) | (std::uint8_t(s[i + 1]) & 0x3F);
            o.push_back(cp <= 0xFF ? char(cp) : '?');
            ++i;
        } else {
            o.push_back('?');                     // a code point latin-1 cannot hold
            while (i + 1 < s.size() && (std::uint8_t(s[i + 1]) & 0xC0) == 0x80) ++i;
        }
    }
    return o;
}

void lz4BlockStoreLiterals(const std::uint8_t* src, std::size_t n, std::vector<std::uint8_t>& out) {
    // One sequence: a token whose literal length is n, the length extension, then the literals.
    // The block ends there, which the format allows for the LAST sequence of a block.
    if (n < 15) {
        out.push_back(std::uint8_t(n << 4));
    } else {
        out.push_back(0xF0);
        std::size_t rest = n - 15;
        while (rest >= 255) { out.push_back(255); rest -= 255; }
        out.push_back(std::uint8_t(rest));
    }
    out.insert(out.end(), src, src + n);
}

std::uint32_t ArzWriter::intern(const std::string& text) {
    std::string l1 = utf8ToLatin1(text);
    auto it = m_index.find(l1);
    if (it != m_index.end()) return it->second;
    std::uint32_t idx = std::uint32_t(m_strings.size());
    m_index.emplace(l1, idx);
    m_strings.push_back(l1);
    return idx;
}

void ArzWriter::add(const std::string& record, const std::vector<ArzOutField>& fields,
                    const std::string& rtype, std::uint64_t mtime) {
    Rec r;
    r.nameIdx = intern(record);
    r.rtype = utf8ToLatin1(rtype);
    r.mtime = mtime;
    std::vector<std::uint8_t> body;
    for (const ArzOutField& f : fields) {
        const std::size_t count = f.type == 2 ? f.strs.size() : f.raw.size();
        put16(body, f.type);
        put16(body, std::uint16_t(count));
        put32(body, intern(f.name));
        if (f.type == 2) for (const std::string& s : f.strs) put32(body, intern(s));
        else for (std::uint32_t w : f.raw) put32(body, w);
    }
    r.dsize = std::uint32_t(body.size());
    lz4BlockStoreLiterals(body.data(), body.size(), r.blob);
    m_records.push_back(std::move(r));
}

std::vector<std::uint8_t> ArzWriter::build() const {
    std::vector<std::uint8_t> data, recTable, strTable;
    std::vector<std::uint32_t> offsets;
    offsets.reserve(m_records.size());
    for (const Rec& r : m_records) {
        offsets.push_back(std::uint32_t(data.size()));
        data.insert(data.end(), r.blob.begin(), r.blob.end());
    }
    for (std::size_t i = 0; i < m_records.size(); ++i) {
        const Rec& r = m_records[i];
        put32(recTable, r.nameIdx);
        put32(recTable, std::uint32_t(r.rtype.size()));
        putBytes(recTable, r.rtype);
        put32(recTable, offsets[i]);
        put32(recTable, std::uint32_t(r.blob.size()));
        put32(recTable, r.dsize);
        put64(recTable, r.mtime);
    }
    put32(strTable, std::uint32_t(m_strings.size()));
    for (const std::string& s : m_strings) {
        put32(strTable, std::uint32_t(s.size()));
        putBytes(strTable, s);
    }
    const std::uint32_t recStart = std::uint32_t(24 + data.size());
    const std::uint32_t strStart = std::uint32_t(recStart + recTable.size());
    std::vector<std::uint8_t> out;
    out.reserve(strStart + strTable.size() + 16);
    put16(out, 2);                                // the engine's only test: unk >= minVersion
    put16(out, 3);                                // format version
    put32(out, recStart);
    put32(out, std::uint32_t(recTable.size()));
    put32(out, std::uint32_t(m_records.size()));
    put32(out, strStart);
    put32(out, std::uint32_t(strTable.size()));
    out.insert(out.end(), data.begin(), data.end());
    out.insert(out.end(), recTable.begin(), recTable.end());
    out.insert(out.end(), strTable.begin(), strTable.end());
    out.insert(out.end(), 16, 0);                 // never read at load time
    return out;
}

} // namespace gen
