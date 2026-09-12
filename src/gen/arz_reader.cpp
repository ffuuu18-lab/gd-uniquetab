// arz_reader.cpp - see arz_reader.h.
#include "gen/arz_reader.h"

#include <cstdio>
#include <cstring>

namespace gen {

namespace {

std::uint16_t rd16(const std::uint8_t* p) { return std::uint16_t(p[0] | (p[1] << 8)); }
std::uint32_t rd32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16)
         | (std::uint32_t(p[3]) << 24);
}
std::uint64_t rd64(const std::uint8_t* p) {
    return std::uint64_t(rd32(p)) | (std::uint64_t(rd32(p + 4)) << 32);
}

bool readFile(const std::string& path, std::vector<std::uint8_t>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    if (n < 0) { std::fclose(f); return false; }
    std::fseek(f, 0, SEEK_SET);
    out.resize(std::size_t(n));
    std::size_t got = n ? std::fread(out.data(), 1, out.size(), f) : 0;
    std::fclose(f);
    return got == out.size();
}

} // namespace

bool lz4BlockDecode(const std::uint8_t* src, std::size_t srcLen,
                    std::uint8_t* dst, std::size_t dstLen) {
    std::size_t ip = 0, op = 0;
    if (dstLen == 0) return srcLen == 0 || (srcLen == 1 && src[0] == 0);
    for (;;) {
        if (ip >= srcLen) return false;
        std::uint8_t token = src[ip++];
        std::size_t lit = token >> 4;
        if (lit == 15) {
            std::uint8_t b;
            do {
                if (ip >= srcLen) return false;
                b = src[ip++];
                lit += b;
            } while (b == 255);
        }
        if (lit > srcLen - ip || lit > dstLen - op) return false;
        std::memcpy(dst + op, src + ip, lit);
        ip += lit;
        op += lit;
        if (ip == srcLen) return op == dstLen;       // the last sequence carries literals only
        if (ip + 2 > srcLen) return false;
        std::size_t off = std::size_t(src[ip]) | (std::size_t(src[ip + 1]) << 8);
        ip += 2;
        if (off == 0 || off > op) return false;
        std::size_t mlen = token & 15;
        if (mlen == 15) {
            std::uint8_t b;
            do {
                if (ip >= srcLen) return false;
                b = src[ip++];
                mlen += b;
            } while (b == 255);
        }
        mlen += 4;
        if (mlen > dstLen - op) return false;
        const std::uint8_t* m = dst + op - off;
        for (std::size_t i = 0; i < mlen; ++i) dst[op + i] = m[i];   // overlap-safe byte copy
        op += mlen;
        if (op == dstLen) return ip == srcLen;
    }
}

std::string latin1ToUtf8(const std::uint8_t* p, std::size_t n) {
    std::string s;
    s.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        std::uint8_t c = p[i];
        if (c < 0x80) {
            s.push_back(char(c));
        } else {
            s.push_back(char(0xC0 | (c >> 6)));
            s.push_back(char(0x80 | (c & 0x3F)));
        }
    }
    return s;
}

// ---------------------------------------------------------------- ArzRecord
const ArzField* ArzRecord::find(const char* name) const {
    for (std::size_t i = fields.size(); i-- > 0;)
        if (fields[i].name == name) return &fields[i];
    return nullptr;
}

std::string ArzRecord::str(const char* name) const {
    const ArzField* f = find(name);
    if (!f || f->raw.empty()) return std::string();
    if (f->type == kFtString) return f->strs.empty() ? std::string() : f->strs[0];
    if (f->type == kFtFloat) {
        float v;
        std::memcpy(&v, &f->raw[0], 4);
        char buf[64];
        std::snprintf(buf, sizeof buf, "%g", double(v));
        return buf;
    }
    return std::to_string(std::int32_t(f->raw[0]));
}

std::int32_t ArzRecord::i32(const char* name) const {
    const ArzField* f = find(name);
    if (!f || f->raw.empty()) return 0;
    if (f->type == kFtString) {
        if (f->strs.empty()) return 0;
        char* end = nullptr;
        long v = std::strtol(f->strs[0].c_str(), &end, 10);
        return (end && *end == 0 && !f->strs[0].empty()) ? std::int32_t(v) : 0;
    }
    if (f->type == kFtFloat) {
        float v;
        std::memcpy(&v, &f->raw[0], 4);
        return std::int32_t(v);
    }
    return std::int32_t(f->raw[0]);
}

// ---------------------------------------------------------------- ArzArchive
std::string ArzArchive::normKey(const std::string& name) {
    std::string k = name;
    for (char& c : k) {
        if (c == '\\') c = '/';
        else if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    }
    return k;
}

bool ArzArchive::load(const std::string& path, std::string* error) {
    m_path = path;
    if (m_tag.empty()) {
        std::size_t s = path.find_last_of("/\\");
        std::string base = s == std::string::npos ? path : path.substr(s + 1);
        std::size_t d = base.find_last_of('.');
        m_tag = d == std::string::npos ? base : base.substr(0, d);
    }
    if (!readFile(path, m_blob)) { if (error) *error = "cannot read " + path; return false; }
    const std::uint8_t* b = m_blob.data();
    const std::size_t n = m_blob.size();
    if (n < 24) { if (error) *error = path + ": too short"; return false; }
    std::uint16_t version = rd16(b + 2);
    std::uint32_t recStart = rd32(b + 4), recSize = rd32(b + 8), recCount = rd32(b + 12);
    std::uint32_t strStart = rd32(b + 16), strSize = rd32(b + 20);
    if (version != 3) { if (error) *error = path + ": ARZ version is not 3"; return false; }
    if (std::uint64_t(recStart) + recSize > n || std::uint64_t(strStart) + strSize > n
        || strSize < 4) {
        if (error) *error = path + ": table offsets outside the file";
        return false;
    }
    // string table
    {
        std::size_t p = strStart, end = std::size_t(strStart) + strSize;
        std::uint32_t count = rd32(b + p);
        p += 4;
        m_strings.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            if (p + 4 > end) { if (error) *error = path + ": string table truncated"; return false; }
            std::uint32_t len = rd32(b + p);
            p += 4;
            if (len > end - p) { if (error) *error = path + ": string table truncated"; return false; }
            m_strings.push_back(latin1ToUtf8(b + p, len));
            p += len;
        }
    }
    // record table
    {
        std::size_t p = recStart, end = std::size_t(recStart) + recSize;
        m_entries.reserve(recCount);
        for (std::uint32_t i = 0; i < recCount; ++i) {
            if (p + 8 > end) { if (error) *error = path + ": record table truncated"; return false; }
            std::uint32_t nameIdx = rd32(b + p), typeLen = rd32(b + p + 4);
            p += 8;
            if (typeLen > end - p || end - p - typeLen < 20) {
                if (error) *error = path + ": record table truncated";
                return false;
            }
            ArzEntry e;
            e.rtype = latin1ToUtf8(b + p, typeLen);
            p += typeLen;
            e.offset = rd32(b + p);
            e.csize = rd32(b + p + 4);
            e.dsize = rd32(b + p + 8);
            e.mtime = rd64(b + p + 12);
            p += 20;
            if (nameIdx >= m_strings.size()) { if (error) *error = path + ": record name index"; return false; }
            if (std::uint64_t(24) + e.offset + e.csize > n) {
                if (error) *error = path + ": record data outside the file";
                return false;
            }
            e.name = m_strings[nameIdx];
            for (char& c : e.name) if (c == '\\') c = '/';
            e.key = normKey(e.name);
            m_index[e.key] = m_entries.size();
            m_entries.push_back(std::move(e));
        }
    }
    return true;
}

const ArzEntry* ArzArchive::entry(const std::string& name) const {
    auto it = m_index.find(normKey(name));
    return it == m_index.end() ? nullptr : &m_entries[it->second];
}

bool ArzArchive::decode(const ArzEntry& e, ArzRecord& out) const {
    out.fields.clear();
    if (std::uint64_t(24) + e.offset + e.csize > m_blob.size()) return false;
    std::vector<std::uint8_t> raw(e.dsize);
    if (!lz4BlockDecode(m_blob.data() + 24 + e.offset, e.csize, raw.data(), e.dsize)) return false;
    std::size_t p = 0, n = raw.size();
    while (p + 8 <= n) {
        ArzField f;
        f.type = rd16(raw.data() + p);
        std::uint16_t count = rd16(raw.data() + p + 2);
        std::uint32_t nameIdx = rd32(raw.data() + p + 4);
        p += 8;
        std::size_t end = p + std::size_t(count) * 4;
        if (end > n) return false;
        f.name = nameIdx < m_strings.size() ? m_strings[nameIdx] : "?" + std::to_string(nameIdx);
        f.raw.reserve(count);
        for (std::uint16_t i = 0; i < count; ++i) f.raw.push_back(rd32(raw.data() + p + i * 4));
        if (f.type == kFtString) {
            f.strs.reserve(count);
            for (std::uint32_t idx : f.raw)
                f.strs.push_back(idx < m_strings.size() ? m_strings[idx] : std::string());
        }
        p = end;
        out.fields.push_back(std::move(f));
    }
    return true;
}

// ---------------------------------------------------------------- ArzDatabase
std::vector<std::pair<std::string, std::string>> ArzDatabase::loadOrder() {
    return {
        {"database", "database\\database.arz"},
        {"gdx1", "gdx1\\database\\GDX1.arz"},
        {"gdx2", "gdx2\\database\\GDX2.arz"},
        {"gdx3", "gdx3\\database\\GDX3.arz"},
    };
}

bool ArzDatabase::loadGame(const std::string& gameDir, std::string* error) {
    m_archives.clear();
    m_index.clear();
    m_winners.clear();
    std::vector<ArzArchive> loaded;
    loaded.reserve(4);
    for (const auto& lo : loadOrder()) {
        std::string full = gameDir + "\\" + lo.second;
        FILE* f = std::fopen(full.c_str(), "rb");
        if (!f) continue;
        std::fclose(f);
        ArzArchive a;
        a.setTag(lo.first);
        if (!a.load(full, error)) return false;
        loaded.push_back(std::move(a));
    }
    if (loaded.empty()) { if (error) *error = "no .arz archive under " + gameDir; return false; }
    m_archives = std::move(loaded);
    for (std::size_t ai = 0; ai < m_archives.size(); ++ai) {
        for (const ArzEntry& e : m_archives[ai].entries()) {
            auto it = m_index.find(e.key);
            if (it == m_index.end()) {
                m_index[e.key] = m_winners.size();
                m_winners.emplace_back(ai, &e);
            } else {
                m_winners[it->second] = {ai, &e};
            }
        }
    }
    return true;
}

bool ArzDatabase::lookup(const std::string& name, std::size_t* archive, const ArzEntry** entry) const {
    auto it = m_index.find(ArzArchive::normKey(name));
    if (it == m_index.end()) return false;
    if (archive) *archive = m_winners[it->second].first;
    if (entry) *entry = m_winners[it->second].second;
    return true;
}

bool ArzDatabase::get(const std::string& name, ArzRecord& out) const {
    std::size_t ai;
    const ArzEntry* e;
    if (!lookup(name, &ai, &e)) return false;
    return m_archives[ai].decode(*e, out);
}

} // namespace gen
