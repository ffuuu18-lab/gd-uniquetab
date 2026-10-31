// pages_gen.cpp - see pages_gen.h.
#include "gen/pages_gen.h"
#include "gen/arc_reader.h"
#include "gen/arz_reader.h"
#include "gen/arz_writer.h"
#include "gen/catalogue_gen.h"
#include "gen/plate_gen.h"
#include "gen/ui_geometry.h"
#include "gen/ut_groups.h"
#include "gen/ut_groups_default.h"
#include "ut_layout.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <unordered_set>

namespace gen {

namespace {

const char* const kMaterialWindow = "records/ui/caravan/caravan_materialwindow.dbr";
const char* const kFrameRec = "records/ui/caravan/uniq_frame.dbr";
const char* const kPlainPlate = "records/ui/caravan/caravan_transfercoverimage.dbr";
const char* const kCoverRec = "records/ui/caravan/uniq_cover.dbr";
const char* const kBoxTpl = "database/templates/ingameui/uireagentitem.tpl";
const char* const kPageFmt = "records/ui/caravan/uniq_p%02d.dbr";
const char* const kBoxFmt = "records/ui/caravan/reagents/uniq/p%02d/box_%02d.dbr";
const char* const kHideFmt = "records/ui/caravan/reagents/uniq/hide_%03d.dbr";

// THE CAP. The frame is the one window the engine ever builds for the tab; its boxes are then
// re-pointed in place, so no page can show more boxes than the frame carries. The count is the
// view's own (ut::utFrameBoxes): the most boxes any grid on the cover can show at the view's
// cell sizes, 160..512 - so neither the derived grid nor a user grid anywhere on the cover is
// capped below the ceiling, and only a cover bigger than 512 cells paginates further instead.
// The frame ALWAYS carries all of them, whatever the layout needs today, so a live layout change
// never needs a regeneration of the overlay.
const int kRowGap = 2;             // the vanilla 64 px rows sit on a 66 px pitch
const int kHideXY = -4000;         // record pixels, scaled by the UI scale - always off-screen

// What the group rules read of a catalogue record: fromMod = only a mod layer has it; a class
// the catalogue has no slot for ("other") is unknown, so an auto group can take it.
utg::Item groupItem(const CatalogueItem& it) {
    utg::Item g;
    g.record = it.record;
    g.cls = it.cls;
    g.rank = it.classification;
    if (it.isSetPiece) g.flags |= utg::kFlagSet;
    if (it.isRelic) g.flags |= utg::kFlagRelic;
    // an MI of a per-class group's class (a Rare staff / hunting spear) is equipment to the rules
    // too, so a game's `rank = Rare, flag = equipment` excludes take it like any other MI
    if (it.isEquipment || it.isMi) g.flags |= utg::kFlagEquipment;
    if (it.isBlueprint) g.flags |= utg::kFlagBlueprint;
    if (it.isAugment) g.flags |= utg::kFlagAugment;
    if (it.isExtra) g.flags |= utg::kFlagExtra;
    if (utg::normPath(it.record).compare(0, 22, "records/items/faction/") == 0) g.flags |= utg::kFlagFaction;
    g.fromMod = it.fromMod;
    g.knownClass = it.slot != "other";
    return g;
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
const std::size_t kTexHeader = 32;          // magic, then height and width at 24 and 28

bool texSize(const std::vector<std::uint8_t>& d, int& w, int& h) {
    if (d.size() < kTexHeader || d[0] != 'T' || d[1] != 'E' || d[2] != 'X') return false;
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

// The grid of one cell size in the derived rectangle (ut_layout.h's arithmetic, which the
// live side relayouts with) under the frame's box count. True when the cap took rows away.
bool grid(const UiGeometry& geo, int cw, int ch, int cap, int& cols, int& rows) {
    const ut::UtGrid g = ut::utGridFit({0, 0, geo.usableW, geo.usableH}, cw, ch, kRowGap, cap);
    cols = g.cols;
    rows = g.rows;
    return g.capped != 0;
}

std::string fmt(const char* f, int a, int b = 0) {
    char buf[160];
    std::snprintf(buf, sizeof buf, f, a, b);
    return buf;
}

// The record as the engine will see it: the mod layer first, then the base game.
bool merged(const GameData& g, const ModView* mods, const char* name, ArzRecord& out) {
    if (mods && !mods->empty() && mods->get(name, out)) return true;
    return g.db->get(name, out);
}

// One reagent box record: the template, the position, and the item it shows.
std::vector<ArzOutField> boxFields(int x, int y, const std::string& item) {
    return {outStr("templateName", kBoxTpl), outStr("FileDescription", "UIItemReagent"),
            outInt("itemBoxX", x), outInt("itemBoxY", y), outStr("reagentName", item)};
}

// A page or the frame: every field of the materials window, with its box list replaced and -
// when `plate` names one - that record in place of the vanilla transfer plate.
std::vector<ArzOutField> windowFields(const ArzRecord& src, const std::vector<std::string>& boxes,
                                      const std::string& plate) {
    std::vector<ArzOutField> out;
    bool hadBoxes = false;
    for (const ArzField& f : src.fields) {
        ArzOutField o;
        o.name = f.name;
        o.type = f.type;
        if (f.name == "reagentBoxes") {
            o.type = kFtString;
            o.strs = boxes;
            hadBoxes = true;
        } else if (!plate.empty() && f.name == "TransferPlate") {
            o.type = kFtString;
            o.strs.push_back(plate);
        } else {
            o.raw = f.raw;
            o.strs = f.strs;
        }
        out.push_back(std::move(o));
    }
    if (!hadBoxes) out.push_back(outStrs("reagentBoxes", boxes));
    return out;
}

} // namespace

const utg::GroupSet* defaultGroupSet(std::string* error) {
    struct Parsed { utg::GroupSet set; std::string why; bool ok = false; };
    static const Parsed p = [] {
        Parsed r;
        r.ok = utg::parseGroups(utg::defaultGroupsIni(), r.set, &r.why);
        return r;
    }();
    if (!p.ok) {
        if (error) *error = "the built-in groups template does not parse: " + p.why;
        return nullptr;
    }
    return &p.set;
}

bool generatePages(const GameData& g, const std::vector<CatalogueItem>& items,
                   PagesOutput& out, std::string* error, const ModView* mods,
                   const GroupsInput* groupsIn) {
    out = PagesOutput();
    if (!g.db || !g.items) { if (error) *error = "game data not loaded"; return false; }
    const utg::GroupSet* rules = groupsIn && groupsIn->set ? groupsIn->set : defaultGroupSet(error);
    if (!rules) return false;

    // The grid rectangle and the plate are read out of the merged database, not compiled in.
    if (!(mods ? deriveGeometry(g, *mods, out.geo, error)
               : deriveGeometry(g, out.geo, error))) return false;

    // The materials window the engine will build, mod layer included: its fields are what the
    // frame and every page copy, so the frame follows a mod that MOVES the page
    // (WindowLocationX/Y) without a profile of its own. Its boxes are the real ones the frame
    // lists first, so a character loads with a page bit-identical to the one it would have had.
    ArzRecord van;
    if (!merged(g, mods, kMaterialWindow, van)) {
        if (error) *error = std::string(kMaterialWindow) + " is not in the database";
        return false;
    }
    const ArzField* vb = van.find("reagentBoxes");
    if (!vb || vb->strs.empty()) { if (error) *error = "the materials page has no reagentBoxes"; return false; }
    std::vector<std::string> vanBoxes = vb->strs;
    out.frameX = van.i32("WindowLocationX");
    out.frameY = van.i32("WindowLocationY");
    // The record type and timestamp of the window record, so a generated page looks to the
    // engine exactly like the record it was copied from.
    std::string winType;
    std::uint64_t winTime = ArzWriter::kDefaultMtime;
    {
        const ArzEntry* e = nullptr;
        if (g.db->lookup(kMaterialWindow, nullptr, &e) && e) { winType = e->rtype; winTime = e->mtime; }
    }
    // The filler boxes park an existing reagent far outside the window; an empty reagentName is
    // not known to be accepted by the engine's box Load, so the first real box's item is reused.
    ArzRecord vanBox0;
    std::string hideItem;
    if (merged(g, mods, vanBoxes[0].c_str(), vanBox0)) hideItem = vanBox0.str("reagentName");
    if (hideItem.empty()) { if (error) *error = "cannot read reagentName out of " + vanBoxes[0]; return false; }

    // The collection is what the group rules put on a page. The built-in rules are 1.1.0's:
    // relics at every classification, curated extras, Epic/Legendary equipment, by slot.
    std::vector<utg::Item> gitems;
    gitems.reserve(items.size());
    for (const CatalogueItem& it : items) gitems.push_back(groupItem(it));
    utg::Assignment assign;
    static const std::vector<std::string> kNoKeys;
    utg::assignGroups(*rules, gitems, groupsIn ? groupsIn->viewKeys : kNoKeys, assign);
    std::vector<Picked> picked;
    std::map<std::string, std::pair<bool, std::pair<int, int>>> sizes;
    std::vector<std::uint8_t> data;
    for (std::size_t ii = 0; ii < items.size(); ++ii) {
        const CatalogueItem& it = items[ii];
        const int grp = assign.groupOf[ii];
        if (grp < 0) continue;
        auto s = sizes.find(it.bitmap);
        if (s == sizes.end()) {
            std::pair<bool, std::pair<int, int>> v{false, {0, 0}};
            // the mod's own icon first, as the engine loads it; then the base game's
            int mw = 0, mh = 0;
            if (mods && !mods->empty() && mods->texSize(it.bitmap, mw, mh)) {
                v.first = mw > 0 && mw <= 1024 && mh > 0 && mh <= 1024;
                v.second = {mw, mh};
            } else if (!it.bitmap.empty()) {
                std::string key = ArzArchive::normKey(it.bitmap);
                // only the 32-byte .tex header is wanted, never the bitmap behind it
                bool got = g.items->read(key, data, kTexHeader);
                if (!got && key.compare(0, 6, "items/") == 0)
                    got = g.items->read(key.substr(6), data, kTexHeader);
                if (!got && g.levelArt && key.compare(0, 10, "level art/") == 0)
                    got = g.levelArt->read(key.substr(10), data, kTexHeader);
                if (got) v.first = texSize(data, v.second.first, v.second.second);
            }
            s = sizes.emplace(it.bitmap, v).first;
        }
        const auto& v = s->second;
        if (!(v.first && v.second.first <= out.geo.usableW
              && v.second.second <= out.geo.usableH)) {
            ++out.skipped;                       // an icon too big for the whole band
            continue;
        }
        Picked p{&it, grp, rankOf(it.classification), v.second.first, v.second.second,
                 lowerName(stripColourCodes(it.name))};
        picked.push_back(std::move(p));
    }
    std::sort(picked.begin(), picked.end(), [](const Picked& a, const Picked& b) {
        if (a.order != b.order) return a.order < b.order;
        if (a.rank != b.rank) return a.rank < b.rank;
        int c = a.lname.compare(b.lname);
        if (c != 0) return c < 0;
        return a.it->record < b.it->record;
    });

    // the groups that received a record, in display order, each with its own grid
    struct Group { int def = -1; std::vector<const Picked*> items; int cw = 0, ch = 0, cols = 0, rows = 0; };
    std::vector<Group> groups;
    for (const Picked& p : picked) {
        if (groups.empty() || groups.back().def != p.order) { groups.push_back(Group()); groups.back().def = p.order; }
        Group& gr = groups.back();
        gr.items.push_back(&p);
        gr.cw = std::max(gr.cw, p.w);
        gr.ch = std::max(gr.ch, p.h);
    }
    // The frame's box count, from the cover and the groups' distinct cell sizes, before any grid
    // is fitted under it.
    {
        std::vector<int> cws, chs;
        for (const Group& gr : groups) {
            bool have = false;
            for (std::size_t i = 0; i < cws.size(); ++i)
                if (cws[i] == gr.cw && chs[i] == gr.ch) { have = true; break; }
            if (!have) { cws.push_back(gr.cw); chs.push_back(gr.ch); }
        }
        // The budget is fitted to the cover, or to the grid's rectangle when that runs past it.
        out.frameBoxes = ut::utFrameBoxes(std::max(out.geo.plateW, out.geo.usableW),
                                          std::max(out.geo.plateH, out.geo.usableH), cws.data(),
                                          chs.data(), int(cws.size()), kRowGap);
    }
    if (int(vanBoxes.size()) > out.frameBoxes) { if (error) *error = "more materials boxes than the frame holds"; return false; }
    for (Group& gr : groups) {
        // the rectangle would hold more boxes than the frame carries: the group paginates instead
        if (grid(out.geo, gr.cw, gr.ch, out.frameBoxes, gr.cols, gr.rows)) ++out.cappedGroups;
    }
    // The distinct cell sizes, ascending - one cover plate is painted per entry, and every
    // group of a size shares its grid, so the first one that has it is the whole answer.
    for (const Group& gr : groups) {
        bool have = false;
        for (const CellGrid& c : out.cells)
            if (c.cellW == gr.cw && c.cellH == gr.ch) { have = true; break; }
        if (have) continue;
        CellGrid c;
        c.cellW = gr.cw; c.cellH = gr.ch; c.cols = gr.cols; c.rows = gr.rows;
        out.cells.push_back(c);
    }
    std::sort(out.cells.begin(), out.cells.end(), [](const CellGrid& a, const CellGrid& b) {
        return a.cellW != b.cellW ? a.cellW < b.cellW : a.cellH < b.cellH;
    });

    // The frame carries the whole count (out.frameBoxes, above), whatever the pages need today:
    // a layout the user changes in the game then relayouts the same boxes without a new overlay.

    // pages, and the three files
    ArzWriter arz;
    std::string& groupsTxt = out.groups;
    groupsTxt += "F\t" + std::string(kFrameRec) + fmt("\t%d\t%d", out.frameBoxes, int(vanBoxes.size()))
               + fmt("\t%d\t%d", out.geo.x0, out.geo.y0) + fmt("\t%d\r\n", kRowGap);
    groupsTxt += out.geo.line() + "\r\n";
    for (const std::string& r : vanBoxes) groupsTxt += "V\t" + r + "\r\n";
    int page = 0;
    for (std::size_t gi = 0; gi < groups.size(); ++gi) {
        Group& gr = groups[gi];
        std::size_t per = std::size_t(gr.cols) * std::size_t(gr.rows);
        std::size_t nPages = (gr.items.size() + per - 1) / per;
        const utg::GroupDef& def = assign.groups[std::size_t(gr.def)];
        const std::string& label = def.label;
        utg::GLine gl;
        gl.index = int(gi);
        gl.label = label;
        gl.cols = gr.cols; gl.rows = gr.rows; gl.cellW = gr.cw; gl.cellH = gr.ch;
        gl.count = int(gr.items.size());
        gl.id = def.id;
        gl.tag = def.tag;
        groupsTxt += utg::formatGLine(gl) + "\r\n";
        for (std::size_t sub = 0; sub < nPages; ++sub, ++page) {
            std::size_t from = sub * per, to = std::min(gr.items.size(), from + per);
            std::string pageRec = fmt(kPageFmt, page);
            out.pages += fmt("%d\t", page) + pageRec + "\t" + label
                       + fmt(" %d/%d", int(sub + 1), int(nPages)) + fmt("\t%d\r\n", int(to - from));
            std::vector<std::string> pageBoxes;
            pageBoxes.reserve(to - from);
            for (std::size_t n = from; n < to; ++n) {
                std::string box = fmt(kBoxFmt, page, int(n - from + 1));
                const std::string& rec = gr.items[n]->it->record;
                out.records += rec + "\r\n";
                groupsTxt += "E\t" + box + "\t" + rec + "\r\n";
                const int col = int((n - from) % std::size_t(gr.cols));
                const int row = int((n - from) / std::size_t(gr.cols));
                arz.add(box, boxFields(out.geo.x0 + gr.cw * col,
                                       out.geo.y0 + (gr.ch + kRowGap) * row, rec));
                pageBoxes.push_back(box);
                out.boxRecords.push_back(box);
                ++out.itemCount;
            }
            arz.add(pageRec, windowFields(van, pageBoxes, kPlainPlate), winType, winTime);
        }
        PagesOutput::Group og;
        og.label = label;
        og.id = def.id;
        og.tag = def.tag;
        og.entries = gr.items.size();
        og.pages = nPages;
        out.groupList.push_back(og);
    }
    out.pageCount = std::size_t(page);

    // The frame: the real materials boxes first, then fillers parked off-screen, so the page a
    // character loads with is the one it would have had and every later box is re-pointed.
    std::vector<std::string> frameBoxes = vanBoxes;
    for (int n = 0; n < out.frameBoxes - int(vanBoxes.size()); ++n) {
        std::string rec = fmt(kHideFmt, n);
        arz.add(rec, boxFields(kHideXY, kHideXY, hideItem));
        frameBoxes.push_back(rec);
    }
    // A page wider than its cover: the frame's plate is the painted cover plate of the page's
    // size (plate_gen), so the engine freezes the plate widget's draw rectangle at the page -
    // the record's own cover fields, with the plate's name and the page's position.
    std::string frameCover;
    if (out.geo.widePlate) {
        ArzRecord crec;
        if (!merged(g, mods, van.str("TransferPlate").c_str(), crec)) {
            if (error) *error = "the materials page has no readable TransferPlate record";
            return false;
        }
        std::vector<ArzOutField> cf;
        for (const ArzField& f : crec.fields) {
            if (f.name == "bitmapName" || f.name == "bitmapPositionX" || f.name == "bitmapPositionY") continue;
            ArzOutField o;
            o.name = f.name; o.type = f.type; o.raw = f.raw; o.strs = f.strs;
            cf.push_back(std::move(o));
        }
        const std::string tag = groupsIn ? groupsIn->plateTag : std::string();
        cf.push_back(outStr("bitmapName", "ui/caravan/" + plateCoverName(out.geo.plateW, out.geo.plateH, tag) + ".tex"));
        cf.push_back(outInt("bitmapPositionX", out.geo.bmpX));
        cf.push_back(outInt("bitmapPositionY", out.geo.bmpY));
        arz.add(kCoverRec, cf);
        frameCover = kCoverRec;
    }
    arz.add(kFrameRec, windowFields(van, frameBoxes, frameCover), winType, winTime);
    out.arz = arz.build();
    out.arzRecords = arz.recordCount();
    return true;
}

bool checkPageCapacity(const std::string& pagesArzPath, const PagesOutput& out, std::string* error) {
    ArzArchive arz;
    if (!arz.load(pagesArzPath, error)) return false;
    if (arz.recordCount() != out.arzRecords) {
        if (error) *error = "the overlay holds " + std::to_string(arz.recordCount())
                          + " records, the layout wrote " + std::to_string(out.arzRecords);
        return false;
    }
    std::unordered_set<std::string> have;
    for (const ArzEntry& e : arz.entries()) have.insert(e.key);
    const ArzEntry* fr = arz.entry(kFrameRec);
    ArzRecord frame;
    if (!fr || !arz.decode(*fr, frame)) {
        if (error) *error = "the overlay has no readable frame record";
        return false;
    }
    const ArzField* fb = frame.find("reagentBoxes");
    if (!fb || int(fb->strs.size()) != out.frameBoxes) {
        if (error) *error = "the frame carries " + std::to_string(fb ? fb->strs.size() : 0)
                          + " boxes, the layout wrote " + std::to_string(out.frameBoxes);
        return false;
    }
    // walk the E lines group by group so the message can name the group that does not fit
    std::size_t at = 0;
    for (const PagesOutput::Group& gr : out.groupList) {
        std::size_t missing = 0;
        for (std::size_t i = 0; i < gr.entries; ++i, ++at)
            if (!have.count(ArzArchive::normKey(out.boxRecords[at]))) ++missing;
        if (missing) {
            if (error) *error = "group " + gr.label + " needs " + std::to_string(gr.entries)
                              + " boxes but " + std::to_string(missing)
                              + " of them are not in the overlay (" + std::to_string(gr.entries - missing)
                              + " available)";
            return false;
        }
    }
    return true;
}

} // namespace gen
