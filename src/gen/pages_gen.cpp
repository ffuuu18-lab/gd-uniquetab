// pages_gen.cpp - see pages_gen.h.
#include "gen/pages_gen.h"
#include "gen/arc_reader.h"
#include "gen/arz_reader.h"
#include "gen/catalogue_gen.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <unordered_set>

namespace gen {

namespace {

const char* const kMaterialWindow = "records/ui/caravan/caravan_materialwindow.dbr";
const char* const kFrameRec = "records/ui/caravan/uniq_frame.dbr";
const int kNMax = 160;             // boxes in the frame; also the most a page carries
const int kColX0 = 105, kRowY0 = 76;
const int kUsableW = 438 - kColX0, kUsableH = 627 - kRowY0;
const int kRowGap = 2;

struct SlotInfo { const char* slot; const char* label; };
// Layout order: armour, jewellery, off-hands, weapons, relics.
const SlotInfo kSlotOrder[] = {
    {"head", "Helms"}, {"shoulders", "Shoulders"}, {"chest", "Chest"}, {"hands", "Gloves"},
    {"waist", "Belts"}, {"legs", "Pants"}, {"feet", "Boots"}, {"amulet", "Amulets"},
    {"medal", "Medals"}, {"ring", "Rings"}, {"offhand", "Off-hands"}, {"shield", "Shields"},
    {"axe1h", "1H Axes"}, {"dagger", "Daggers"}, {"mace1h", "1H Maces"}, {"scepter", "Scepters"},
    {"sword1h", "1H Swords"}, {"ranged1h", "Guns"}, {"axe2h", "2H Axes"}, {"mace2h", "2H Maces"},
    {"spear2h", "Spears"}, {"sword2h", "2H Swords"}, {"ranged2h", "2H Guns"}, {"relic", "Relics"},
};

int slotOrder(const std::string& slot) {
    for (int i = 0; i < int(sizeof kSlotOrder / sizeof *kSlotOrder); ++i)
        if (slot == kSlotOrder[i].slot) return i;
    return 99;
}
std::string slotLabel(const std::string& slot) {
    for (const SlotInfo& s : kSlotOrder) if (slot == s.slot) return s.label;
    return slot.empty() ? "?" : slot;
}
int rankOf(const std::string& c) {
    if (c == "Legendary") return 0;
    if (c == "Epic") return 1;
    if (c == "Rare") return 2;
    if (c == "Common") return 3;
    return 9;
}

// Python's str.lower() for ASCII and the Latin-1 capitals (UTF-8 C3 80..C3 9E except C3 97).
std::string lowerName(const std::string& s) {
    std::string o = s;
    for (std::size_t i = 0; i < o.size(); ++i) {
        unsigned char c = (unsigned char)o[i];
        if (c >= 'A' && c <= 'Z') o[i] = char(c + 32);
        else if (c == 0xC3 && i + 1 < o.size()) {
            unsigned char d = (unsigned char)o[i + 1];
            if (d >= 0x80 && d <= 0x9E && d != 0x97) o[i + 1] = char(d + 0x20);
            ++i;
        }
    }
    return o;
}

std::uint32_t rd32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16)
         | (std::uint32_t(p[3]) << 24);
}

// (w, h) of a .tex: "TEX" then a DDS header at +12 (height at +24, width at +28).
bool texSize(const std::vector<std::uint8_t>& d, int& w, int& h) {
    if (d.size() < 32 || d[0] != 'T' || d[1] != 'E' || d[2] != 'X') return false;
    std::uint32_t hh = rd32(d.data() + 24), ww = rd32(d.data() + 28);
    if (ww == 0 || ww > 1024 || hh == 0 || hh > 1024) return false;
    w = int(ww);
    h = int(hh);
    return true;
}

struct Picked {
    const CatalogueItem* it;
    int order, rank, w, h;
    std::string lname;
};

void grid(int cw, int ch, int& cols, int& rows) {
    cols = std::max(1, kUsableW / cw);
    rows = std::max(1, kUsableH / (ch + kRowGap));
    while (cols * rows > kNMax && rows > 1) --rows;
}

std::string fmt(const char* f, int a, int b = 0) {
    char buf[160];
    std::snprintf(buf, sizeof buf, f, a, b);
    return buf;
}

} // namespace

bool generatePages(const GameData& g, const std::vector<CatalogueItem>& items,
                   PagesOutput& out, std::string* error) {
    out = PagesOutput();
    if (!g.db || !g.items) { if (error) *error = "game data not loaded"; return false; }

    // the vanilla page's boxes: the frame lists them first
    ArzRecord van;
    if (!g.db->get(kMaterialWindow, van)) {
        if (error) *error = std::string(kMaterialWindow) + " is not in the database";
        return false;
    }
    const ArzField* vb = van.find("reagentBoxes");
    if (!vb || vb->strs.empty()) { if (error) *error = "the vanilla materials page has no reagentBoxes"; return false; }
    std::vector<std::string> vanBoxes = vb->strs;
    if (int(vanBoxes.size()) > kNMax) { if (error) *error = "more vanilla boxes than the frame holds"; return false; }

    // the collection: relics at every classification, curated extras, Epic/Legendary equipment
    std::vector<Picked> picked;
    std::map<std::string, std::pair<bool, std::pair<int, int>>> sizes;
    std::vector<std::uint8_t> data;
    for (const CatalogueItem& it : items) {
        bool in = it.isRelic || it.isExtra
               || (it.isEquipment && (it.classification == "Epic" || it.classification == "Legendary"));
        if (!in) continue;
        auto s = sizes.find(it.bitmap);
        if (s == sizes.end()) {
            std::pair<bool, std::pair<int, int>> v{false, {0, 0}};
            if (!it.bitmap.empty()) {
                std::string key = ArzArchive::normKey(it.bitmap);
                bool got = g.items->read(key, data);
                if (!got && key.compare(0, 6, "items/") == 0) got = g.items->read(key.substr(6), data);
                if (got) v.first = texSize(data, v.second.first, v.second.second);
            }
            s = sizes.emplace(it.bitmap, v).first;
        }
        const auto& v = s->second;
        if (!(v.first && v.second.first <= kUsableW && v.second.second <= kUsableH)) {
            ++out.skipped;                       // an icon too big for the whole band
            continue;
        }
        Picked p{&it, slotOrder(it.slot), rankOf(it.classification), v.second.first, v.second.second,
                 lowerName(it.name)};
        picked.push_back(std::move(p));
    }
    std::sort(picked.begin(), picked.end(), [](const Picked& a, const Picked& b) {
        if (a.order != b.order) return a.order < b.order;
        if (a.rank != b.rank) return a.rank < b.rank;
        int c = a.lname.compare(b.lname);
        if (c != 0) return c < 0;
        return a.it->record < b.it->record;
    });

    // groups in first-seen slot order, each with its own grid
    struct Group { std::string slot; std::vector<const Picked*> items; int cw = 0, ch = 0, cols = 0, rows = 0; };
    std::vector<Group> groups;
    for (const Picked& p : picked) {
        if (groups.empty() || groups.back().slot != p.it->slot) { groups.push_back(Group()); groups.back().slot = p.it->slot; }
        Group& gr = groups.back();
        gr.items.push_back(&p);
        gr.cw = std::max(gr.cw, p.w);
        gr.ch = std::max(gr.ch, p.h);
    }
    for (Group& gr : groups) grid(gr.cw, gr.ch, gr.cols, gr.rows);

    // pages, and the three files
    std::string& groupsTxt = out.groups;
    groupsTxt += "F\t" + std::string(kFrameRec) + fmt("\t%d\t%d", kNMax, int(vanBoxes.size()))
               + fmt("\t%d\t%d", kColX0, kRowY0) + fmt("\t%d\r\n", kRowGap);
    for (const std::string& r : vanBoxes) groupsTxt += "V\t" + r + "\r\n";
    int page = 0;
    for (std::size_t gi = 0; gi < groups.size(); ++gi) {
        Group& gr = groups[gi];
        std::size_t per = std::size_t(gr.cols) * std::size_t(gr.rows);
        std::size_t nPages = (gr.items.size() + per - 1) / per;
        std::string label = slotLabel(gr.slot);
        groupsTxt += fmt("G\t%d\t", int(gi)) + label
                   + fmt("\t%d\t%d", gr.cols, gr.rows) + fmt("\t%d\t%d", gr.cw, gr.ch)
                   + fmt("\t%d\r\n", int(gr.items.size()));
        for (std::size_t sub = 0; sub < nPages; ++sub, ++page) {
            std::size_t from = sub * per, to = std::min(gr.items.size(), from + per);
            std::string pageRec = fmt("records/ui/caravan/uniq_p%02d.dbr", page);
            out.pages += fmt("%d\t", page) + pageRec + "\t" + label
                       + fmt(" %d/%d", int(sub + 1), int(nPages)) + fmt("\t%d\r\n", int(to - from));
            for (std::size_t n = from; n < to; ++n) {
                std::string box = fmt("records/ui/caravan/reagents/uniq/p%02d/box_%02d.dbr", page, int(n - from + 1));
                const std::string& rec = gr.items[n]->it->record;
                out.records += rec + "\r\n";
                groupsTxt += "E\t" + box + "\t" + rec + "\r\n";
                out.boxRecords.push_back(box);
                ++out.itemCount;
            }
        }
        PagesOutput::Group og;
        og.label = label;
        og.entries = gr.items.size();
        og.pages = nPages;
        out.groupList.push_back(og);
    }
    out.pageCount = std::size_t(page);
    return true;
}

bool checkPageCapacity(const std::string& pagesArzPath, const PagesOutput& out, std::string* error) {
    ArzArchive arz;
    if (!arz.load(pagesArzPath, error)) return false;
    std::unordered_set<std::string> have;
    for (const ArzEntry& e : arz.entries()) have.insert(e.key);
    // walk the E lines group by group so the message can name the group that does not fit
    std::size_t at = 0;
    for (const PagesOutput::Group& gr : out.groupList) {
        std::size_t missing = 0;
        for (std::size_t i = 0; i < gr.entries; ++i, ++at)
            if (!have.count(ArzArchive::normKey(out.boxRecords[at]))) ++missing;
        if (missing) {
            if (error) *error = "group " + gr.label + " needs " + std::to_string(gr.entries)
                              + " boxes but " + std::to_string(missing)
                              + " of them are not in uniq-pages.arz (" + std::to_string(gr.entries - missing)
                              + " available)";
            return false;
        }
    }
    return true;
}

} // namespace gen
