// arc_reader.cpp - see arc_reader.h.
#include "gen/arc_reader.h"
#include "gen/arz_reader.h"

#include <cstdio>
#include <cstring>

namespace gen {

namespace {

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

const std::uint32_t kArcMagic = 0x00435241;   // "ARC\0"

// True when the whole buffer is well-formed UTF-8 (the same test Python's decode makes).
bool validUtf8(const std::uint8_t* p, std::size_t n) {
    std::size_t i = 0;
    while (i < n) {
        std::uint8_t c = p[i];
        if (c < 0x80) { ++i; continue; }
        std::size_t len;
        std::uint32_t cp;
        if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; if (c < 0xC2) return false; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; if (c > 0xF4) return false; }
        else return false;
        if (i + len > n) return false;
        for (std::size_t k = 1; k < len; ++k) {
            if ((p[i + k] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (p[i + k] & 0x3F);
        }
        if ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF))
            || (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        i += len;
    }
    return true;
}

bool isSpaceByte(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'
        || (c >= 0x1C && c <= 0x1F);
}

// Python's str.strip() on the ASCII whitespace plus NBSP / NEL in their UTF-8 spelling.
std::string stripUtf8(const std::string& s) {
    std::size_t a = 0, b = s.size();
    for (;;) {
        if (a < b && isSpaceByte(std::uint8_t(s[a]))) { ++a; continue; }
        if (a + 2 <= b && std::uint8_t(s[a]) == 0xC2
            && (std::uint8_t(s[a + 1]) == 0xA0 || std::uint8_t(s[a + 1]) == 0x85)) { a += 2; continue; }
        break;
    }
    for (;;) {
        if (b > a && isSpaceByte(std::uint8_t(s[b - 1]))) { --b; continue; }
        if (b >= a + 2 && std::uint8_t(s[b - 2]) == 0xC2
            && (std::uint8_t(s[b - 1]) == 0xA0 || std::uint8_t(s[b - 1]) == 0x85)) { b -= 2; continue; }
        break;
    }
    return s.substr(a, b - a);
}

// Length of the line separator at p (Python's str.splitlines set), 0 when there is none.
std::size_t lineBreakLen(const std::uint8_t* p, std::size_t n) {
    std::uint8_t c = p[0];
    if (c == '\r') return (n > 1 && p[1] == '\n') ? 2 : 1;
    if (c == '\n' || c == '\v' || c == '\f' || c == 0x1C || c == 0x1D || c == 0x1E) return 1;
    if (c == 0xC2 && n > 1 && p[1] == 0x85) return 2;                                 // NEL
    if (c == 0xE2 && n > 2 && p[1] == 0x80 && (p[2] == 0xA8 || p[2] == 0xA9)) return 3; // LS, PS
    return 0;
}

} // namespace

// ---------------------------------------------------------------- ArcArchive
std::string ArcArchive::normKey(const std::string& name) { return ArzArchive::normKey(name); }

bool ArcArchive::load(const std::string& path, std::string* error) {
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
    if (n < 28) { if (error) *error = path + ": too short"; return false; }
    std::uint32_t magic = rd32(b), version = rd32(b + 4), nFiles = rd32(b + 8), nRecs = rd32(b + 12);
    std::uint32_t recSize = rd32(b + 16), strSize = rd32(b + 20), recOff = rd32(b + 24);
    if (magic != kArcMagic || version != 3) { if (error) *error = path + ": not an ARC v3"; return false; }
    std::uint64_t strOff = std::uint64_t(recOff) + recSize;
    std::uint64_t entOff = strOff + strSize;
    if (std::uint64_t(nRecs) * 12 > recSize || entOff + std::uint64_t(nFiles) * 44 > n) {
        if (error) *error = path + ": tables outside the file";
        return false;
    }
    m_parts.reserve(nRecs);
    for (std::uint32_t i = 0; i < nRecs; ++i) {
        const std::uint8_t* p = b + recOff + 12 * i;
        Part pt{rd32(p), rd32(p + 4), rd32(p + 8)};
        if (std::uint64_t(pt.offset) + pt.csize > n) { if (error) *error = path + ": part outside the file"; return false; }
        m_parts.push_back(pt);
    }
    m_entries.reserve(nFiles);
    for (std::uint32_t i = 0; i < nFiles; ++i) {
        const std::uint8_t* p = b + entOff + 44 * i;
        ArcEntry e;
        e.etype = rd32(p);
        e.offset = rd32(p + 4);
        e.csize = rd32(p + 8);
        e.dsize = rd32(p + 12);
        e.ftime = rd64(p + 20);
        e.numParts = rd32(p + 28);
        e.firstPart = rd32(p + 32);
        std::uint32_t nlen = rd32(p + 36), noff = rd32(p + 40);
        if (std::uint64_t(noff) + nlen > strSize) { if (error) *error = path + ": entry name outside the string table"; return false; }
        if (e.numParts == 0) {
            if (std::uint64_t(e.offset) + e.dsize > n) { if (error) *error = path + ": entry data outside the file"; return false; }
        } else if (std::uint64_t(e.firstPart) + e.numParts > m_parts.size()) {
            if (error) *error = path + ": entry parts outside the part table";
            return false;
        }
        e.name = latin1ToUtf8(b + strOff + noff, nlen);
        for (char& c : e.name) if (c == '\\') c = '/';
        e.key = normKey(e.name);
        m_index[e.key] = m_entries.size();
        m_entries.push_back(std::move(e));
    }
    return true;
}

const ArcEntry* ArcArchive::entry(const std::string& name) const {
    auto it = m_index.find(normKey(name));
    return it == m_index.end() ? nullptr : &m_entries[it->second];
}

bool ArcArchive::read(const ArcEntry& e, std::vector<std::uint8_t>& out) const {
    out.clear();
    if (e.numParts == 0) {
        if (std::uint64_t(e.offset) + e.dsize > m_blob.size()) return false;
        out.assign(m_blob.begin() + e.offset, m_blob.begin() + e.offset + e.dsize);
        return true;
    }
    if (std::uint64_t(e.firstPart) + e.numParts > m_parts.size()) return false;
    for (std::uint32_t i = e.firstPart; i < e.firstPart + e.numParts; ++i) {
        const Part& pt = m_parts[i];
        if (std::uint64_t(pt.offset) + pt.csize > m_blob.size()) return false;
        std::size_t at = out.size();
        if (pt.csize == pt.dsize) {
            out.insert(out.end(), m_blob.begin() + pt.offset, m_blob.begin() + pt.offset + pt.csize);
        } else {
            out.resize(at + pt.dsize);
            if (!lz4BlockDecode(m_blob.data() + pt.offset, pt.csize, out.data() + at, pt.dsize))
                return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------- ArcSet
bool ArcSet::load(const std::string& gameDir, const std::string& relName, std::string* error) {
    m_archives.clear();
    m_index.clear();
    static const char* const kSub[4][2] = {
        {"database", ""}, {"gdx1", "gdx1\\"}, {"gdx2", "gdx2\\"}, {"gdx3", "gdx3\\"}};
    std::vector<ArcArchive> loaded;
    loaded.reserve(4);
    for (const auto& s : kSub) {
        std::string full = gameDir + "\\" + s[1] + relName;
        FILE* f = std::fopen(full.c_str(), "rb");
        if (!f) continue;
        std::fclose(f);
        ArcArchive a;
        a.setTag(s[0]);
        if (!a.load(full, error)) return false;
        loaded.push_back(std::move(a));
    }
    if (loaded.empty()) { if (error) *error = "no " + relName + " under " + gameDir; return false; }
    m_archives = std::move(loaded);
    for (std::size_t ai = 0; ai < m_archives.size(); ++ai)
        for (const ArcEntry& e : m_archives[ai].entries()) m_index[e.key] = ai;
    return true;
}

bool ArcSet::contains(const std::string& name) const {
    return m_index.find(ArcArchive::normKey(name)) != m_index.end();
}

std::string ArcSet::sourceOf(const std::string& name) const {
    auto it = m_index.find(ArcArchive::normKey(name));
    return it == m_index.end() ? std::string() : m_archives[it->second].tag();
}

bool ArcSet::read(const std::string& name, std::vector<std::uint8_t>& out) const {
    auto it = m_index.find(ArcArchive::normKey(name));
    if (it == m_index.end()) return false;
    const ArcArchive& a = m_archives[it->second];
    const ArcEntry* e = a.entry(name);
    return e && a.read(*e, out);
}

// ---------------------------------------------------------------- tag files
void parseTagFile(const std::vector<std::uint8_t>& data,
                  std::unordered_map<std::string, std::string>& tags) {
    std::vector<std::uint8_t> conv;
    const std::uint8_t* p = data.data();
    std::size_t n = data.size();
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) { p += 3; n -= 3; }
    if (!validUtf8(p, n)) {
        // Python's fallback reads the WHOLE file (BOM included) as latin-1.
        std::string s = latin1ToUtf8(data.data(), data.size());
        conv.assign(s.begin(), s.end());
        p = conv.data();
        n = conv.size();
    }
    std::size_t i = 0;
    while (i < n) {
        std::size_t j = i, brk = 0;
        while (j < n && (brk = lineBreakLen(p + j, n - j)) == 0) ++j;
        std::string line(reinterpret_cast<const char*>(p + i), j - i);
        i = j + brk;
        if (line.empty() || line.compare(0, 2, "//") == 0 || line[0] == '#') continue;
        std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = stripUtf8(line.substr(0, eq));
        if (k.empty() || k[0] == '<') continue;
        tags[k] = line.substr(eq + 1);
    }
}

bool loadTextTags(const std::string& gameDir, const std::string& lang,
                  std::unordered_map<std::string, std::string>& tags, std::string* error) {
    ArcSet set;
    if (!set.load(gameDir, "resources\\Text_" + lang + ".arc", error)) return false;
    std::vector<std::uint8_t> data;
    for (const ArcArchive& a : set.archives()) {
        for (const ArcEntry& e : a.entries()) {
            if (e.key.size() < 4 || e.key.compare(e.key.size() - 4, 4, ".txt") != 0) continue;
            if (!a.read(e, data)) {
                if (error) *error = a.path() + ": cannot read " + e.name;
                return false;
            }
            if (!data.empty()) parseTagFile(data, tags);
        }
    }
    return true;
}

} // namespace gen
