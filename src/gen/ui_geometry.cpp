// ui_geometry.cpp - see ui_geometry.h.
#include "gen/ui_geometry.h"
#include "gen/arc_reader.h"
#include "gen/arz_reader.h"
#include "gen/catalogue_gen.h"

#include <algorithm>
#include <cstdio>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace gen {

namespace {

const char* const kCaravanWindow = "records/ui/caravan/caravan_window.dbr";
const char* const kMaterialWindow = "records/ui/caravan/caravan_materialwindow.dbr";
const char* const kStashWindow = "records/ui/caravan/caravan_stashwindow.dbr";
const char* const kStashInventory = "records/ui/caravan/caravan_stashinventory.dbr";

// The page's top band is the mod's own: the label in y 8..24 and the 26-button pad in y 25..68
// (kFreeY0/kFreeY1, src/ut_panel.cpp). No database knows about it, so it is a constant here.
const int kPanelBandH = 76;

const std::size_t kTexHeader = 32;          // magic, then height and width at 24 and 28

std::uint32_t rd32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16)
         | (std::uint32_t(p[3]) << 24);
}

// Every path matching <dir>\\<pattern>, in the order the file system hands them back, sorted
// so two machines with the same files build the same view.
std::vector<std::string> globDir(const std::string& dir, const char* pattern) {
    std::vector<std::string> out;
    WIN32_FIND_DATAA fd;
    const std::string spec = dir + "\\" + pattern;
    HANDLE h = FindFirstFileA(spec.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        out.push_back(dir + "\\" + fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

// A cover is bigger than an item icon, so this is not pages_gen's 1024-bounded check.
bool texWH(const std::vector<std::uint8_t>& d, int& w, int& h) {
    if (d.size() < kTexHeader || d[0] != 'T' || d[1] != 'E' || d[2] != 'X') return false;
    std::uint32_t hh = rd32(d.data() + 24), ww = rd32(d.data() + 28);
    if (ww == 0 || ww > 4096 || hh == 0 || hh > 4096) return false;
    w = int(ww);
    h = int(hh);
    return true;
}

// A bitmapName is engine-relative ("ui/caravan/x.tex"); UI.arc is rooted below the "ui/".
bool coverSize(const GameData& g, const std::string& bitmap, int& w, int& h) {
    if (!g.ui || bitmap.empty()) return false;
    std::vector<std::uint8_t> data;
    std::string key = ArcArchive::normKey(bitmap);
    if (key.compare(0, 3, "ui/") == 0 && g.ui->read(key.substr(3), data, kTexHeader))
        return texWH(data, w, h);
    if (g.ui->read(key, data, kTexHeader)) return texWH(data, w, h);
    return false;
}

} // namespace

bool ModView::loadBasemods(const std::string& gameDir, std::string* error) {
    const std::string mods = gameDir + "\\mods";
    std::vector<std::string> arzs = globDir(mods, "database.arz");
    std::vector<std::string> arcs = globDir(mods, "*.arc");
    m_arz.reserve(arzs.size());
    m_arc.reserve(arcs.size());
    for (const std::string& p : arzs) {
        ArzArchive a;
        if (!a.load(p, error)) return false;
        m_arz.push_back(std::move(a));
    }
    for (const std::string& p : arcs) addArc(p);
    m_label = "/basemods";
    return true;
}

// An .arc that cannot be read is skipped and remembered, never fatal: a mod can ship a broken
// resource archive (DeathGrimDawn's text_ko.arc has a zeroed header) and the engine goes on with
// the rest, so the view does too, in the same sorted order.
void ModView::addArc(const std::string& path) {
    ArcArchive a;
    std::string why;
    if (!a.load(path, &why)) {
        // the reader's message already names the file when it starts with the path
        m_skipped.push_back(why.compare(0, path.size(), path) == 0 ? why : path + ": " + why);
        return;
    }
    m_arc.push_back(std::move(a));
}

bool ModView::loadCustom(const std::string& modDir, std::string* error) {
    std::vector<std::string> arzs = globDir(modDir + "\\database", "*.arz");
    std::vector<std::string> arcs = globDir(modDir + "\\resources", "*.arc");
    if (arzs.empty()) {
        if (error) *error = modDir + "\\database holds no .arz";
        return false;
    }
    m_arz.reserve(arzs.size());
    m_arc.reserve(arcs.size());
    for (const std::string& p : arzs) {
        ArzArchive a;
        if (!a.load(p, error)) return false;
        m_arz.push_back(std::move(a));
    }
    for (const std::string& p : arcs) addArc(p);
    // Appended, never replaced: a custom game loaded over a /basemods install is both layers.
    m_label = m_label.empty() ? modDir : m_label + " + " + modDir;
    return true;
}

namespace {

// The leaf of a mod folder, lowercased, with anything but [a-z0-9._-] folded to '-'. It is a
// folder name and part of a file name, so it takes nothing else.
std::string customLeaf(const std::string& dir) {
    std::size_t end = dir.size();
    while (end && (dir[end - 1] == '\\' || dir[end - 1] == '/')) --end;
    std::size_t beg = dir.find_last_of("\\/", end ? end - 1 : 0);
    beg = (beg == std::string::npos) ? 0 : beg + 1;
    std::string leaf;
    for (std::size_t i = beg; i < end && leaf.size() < 48; ++i) {
        char c = dir[i];
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        const bool keep = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
                          c == '_' || c == '-';
        leaf += keep ? c : '-';
    }
    return leaf;
}

} // namespace

std::string ViewSpec::tag() const {
    return customDir.empty() ? std::string() : customLeaf(customDir);
}

std::string ViewSpec::name() const {
    if (customDir.empty()) return basemods ? "basemods" : "vanilla";
    const std::string leaf = tag();
    return (basemods ? "basemods+custom-" : "custom-") + (leaf.empty() ? "mod" : leaf);
}

namespace {

bool sameNoCase(const std::string& a, const char* b) {
    std::size_t i = 0;
    for (; i < a.size() && b[i]; ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = char(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = char(y - 'A' + 'a');
        if (x != y) return false;
    }
    return i == a.size() && !b[i];
}

} // namespace

std::string customGameFolder(const std::string& enginePath, const std::string& gameDir) {
    std::string p = enginePath;
    for (char& c : p)
        if (c == '/') c = '\\';
    // a drive letter, a UNC name (two leading backslashes) or a rooted path is absolute
    std::string root;
    std::size_t at = 0;
    if (p.size() >= 2 && p[1] == ':') {
        root = p.substr(0, 2);
        at = 2;
    } else if (p.size() >= 2 && p[0] == '\\' && p[1] == '\\') {
        root = "\\";
        at = 1;
    }
    const bool absolute = !root.empty() || (!p.empty() && p[0] == '\\');
    std::vector<std::string> seg;
    std::size_t i = at;
    while (i <= p.size()) {
        std::size_t j = p.find('\\', i);
        if (j == std::string::npos) j = p.size();
        const std::string s = p.substr(i, j - i);
        if (!s.empty() && s != ".") seg.push_back(s);
        i = j + 1;
    }
    const std::size_t n = seg.size();
    if (n < 4) return std::string();
    const std::string& file = seg[n - 1];
    if (file.size() < 5 || !sameNoCase(file.substr(file.size() - 4), ".arz")) return std::string();
    if (!sameNoCase(seg[n - 2], "database") || !sameNoCase(seg[n - 4], "mods")) return std::string();
    if (seg[n - 3] == "..") return std::string();
    for (std::size_t k = 0; k + 2 < n; ++k)
        if (seg[k] == "..") return std::string();   // a path that climbs is not one the engine names
    std::string out;
    if (absolute) {
        out = root;
    } else {
        out = gameDir;
        while (!out.empty() && (out.back() == '\\' || out.back() == '/')) out.pop_back();
    }
    for (std::size_t k = 0; k + 2 < n; ++k) {
        if (!out.empty() || absolute) out += '\\';
        out += seg[k];
    }
    return out;
}

std::string findCustomGameFolder(const std::string& enginePath,
                                 const std::vector<std::string>& bases) {
    const std::size_t cut = enginePath.find_last_of("\\/");
    const std::string leaf = cut == std::string::npos ? enginePath : enginePath.substr(cut + 1);
    for (const std::string& b : bases) {
        const std::string folder = customGameFolder(enginePath, b);
        if (folder.empty()) return folder;   // not a custom game's archive, whatever the base
        const DWORD a = GetFileAttributesA((folder + "\\database\\" + leaf).c_str());
        if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) return folder;
    }
    return std::string();
}

bool loadView(const std::string& gameDir, const ViewSpec& view, ModView& out,
              std::string* error) {
    if (view.basemods && !out.loadBasemods(gameDir, error)) return false;
    if (!view.customDir.empty() && !out.loadCustom(view.customDir, error)) return false;
    return true;
}

bool ModView::touchesCaravan() const {
    static const char kPrefix[] = "records/ui/caravan/";
    const std::size_t n = sizeof(kPrefix) - 1;
    for (const ArzArchive& a : m_arz)
        for (const ArzEntry& e : a.entries())
            if (e.key.size() > n && e.key.compare(0, n, kPrefix) == 0) return true;
    for (const ArcArchive& a : m_arc)
        for (const ArcEntry& e : a.entries())
            if (e.key.find("caravan") != std::string::npos) return true;
    return false;
}

bool ModView::get(const std::string& name, ArzRecord& out) const {
    for (std::size_t i = m_arz.size(); i-- > 0;) {
        const ArzEntry* e = m_arz[i].entry(name);
        if (e && m_arz[i].decode(*e, out)) return true;
    }
    return false;
}

namespace {

// A bitmapName is tried whole, without its first path element, and by leaf name.
void texKeys(const std::string& bitmap, std::string (&keys)[3]) {
    const std::string key = ArcArchive::normKey(bitmap);
    keys[0] = keys[1] = keys[2] = key;
    const std::size_t slash = key.find('/');
    if (slash != std::string::npos) keys[1] = key.substr(slash + 1);
    const std::size_t last = key.rfind('/');
    if (last != std::string::npos) keys[2] = key.substr(last + 1);
}

} // namespace

bool ModView::texSize(const std::string& bitmap, int& w, int& h) const {
    if (bitmap.empty()) return false;
    std::string keys[3];
    texKeys(bitmap, keys);
    std::vector<std::uint8_t> data;
    for (const ArcArchive& a : m_arc)
        for (const std::string& k : keys) {
            const ArcEntry* e = a.entry(k);
            if (e && a.read(*e, data, kTexHeader) && texWH(data, w, h)) return true;
        }
    return false;
}

bool ModView::texRead(const std::string& bitmap, std::vector<std::uint8_t>& out) const {
    if (bitmap.empty()) return false;
    std::string keys[3];
    texKeys(bitmap, keys);
    for (const ArcArchive& a : m_arc)
        for (const std::string& k : keys) {
            const ArcEntry* e = a.entry(k);
            if (e && a.read(*e, out)) return true;
        }
    return false;
}

std::string UiGeometry::line() const {
    char buf[96];
    std::snprintf(buf, sizeof buf, "P\t%d\t%d\t%d\t%d\t%d\t%d\t", x0, y0, usableW, usableH,
                  plateW, plateH);
    std::string out = std::string(buf) + plate;
    // The cover's position follows from the usable rectangle (bmpX = x0 + usableW - plateW)
    // except on a page whose grid runs past its cover: then it is written out, two more fields.
    if (bmpX != x0 + usableW - plateW || bmpY != y0 + usableH - plateH) {
        std::snprintf(buf, sizeof buf, "\t%d\t%d", bmpX, bmpY);
        out += buf;
    }
    return out;
}

bool materialBoxPositions(const GameData& g, const ModView& mods,
                          std::vector<std::pair<int, int> >& out, std::string* error) {
    out.clear();
    if (!g.db) { if (error) *error = "game data not loaded"; return false; }
    auto recs = [&](const std::string& name, ArzRecord& r) {
        return mods.get(name, r) || g.db->get(name, r);
    };
    ArzRecord page;
    if (!recs(kMaterialWindow, page)) {
        if (error) *error = std::string(kMaterialWindow) + " is not in the database";
        return false;
    }
    const ArzField* boxes = page.find("reagentBoxes");
    if (!boxes) return true;
    for (const std::string& b : boxes->strs) {
        ArzRecord box;
        if (recs(b, box)) out.emplace_back(box.i32("itemBoxX"), box.i32("itemBoxY"));
    }
    return true;
}

bool deriveGeometry(const GameData& g, UiGeometry& out, std::string* error) {
    static const ModView kNone;
    return deriveGeometry(g, kNone, out, error);
}

bool deriveGeometry(const GameData& g, const ModView& mods, UiGeometry& out,
                    std::string* error) {
    if (!g.db) { if (error) *error = "game data not loaded"; return false; }
    // The merged view: a mod layer first, the base game behind it. Read through this and
    // through this only - "not overridden" has to mean "inherit the vanilla value", which is
    // how one of the two stash-UI families moves the page at all.
    auto rec = [&](const char* name, ArzRecord& r) {
        return mods.get(name, r) || g.db->get(name, r);
    };
    auto recs = [&](const std::string& name, ArzRecord& r) {
        return mods.get(name, r) || g.db->get(name, r);
    };

    ArzRecord win;
    if (!rec(kCaravanWindow, win)) {
        if (error) *error = std::string(kCaravanWindow) + " is not in the database";
        return false;
    }
    const int tabX = win.i32("tabSpacingX"), tabY = win.i32("tabSpacingY");
    // Only Y set: the tab strip runs DOWN THE LEFT (vanilla). X set: ACROSS THE TOP.
    const bool stripLeft = tabY != 0 && tabX == 0;

    ArzRecord page;
    if (!rec(kMaterialWindow, page)) {
        if (error) *error = std::string(kMaterialWindow) + " is not in the database";
        return false;
    }
    const ArzField* boxes = page.find("reagentBoxes");
    if (!boxes || boxes->strs.empty()) {
        if (error) *error = std::string(kMaterialWindow) + " names no reagent box";
        return false;
    }
    int minX = 0, minY = 0;
    bool any = false;
    for (const std::string& b : boxes->strs) {
        ArzRecord box;
        if (!recs(b, box)) continue;
        const int bxx = box.i32("itemBoxX"), byy = box.i32("itemBoxY");
        if (!any) { minX = bxx; minY = byy; any = true; }
        else { minX = std::min(minX, bxx); minY = std::min(minY, byy); }
    }
    if (!any) {
        if (error) *error = std::string(kMaterialWindow) + " names no readable reagent box";
        return false;
    }

    const std::string cover = page.str("TransferPlate");
    ArzRecord crec;
    if (cover.empty() || !recs(cover, crec)) {
        if (error) *error = "the materials page has no readable TransferPlate record";
        return false;
    }
    const std::string bitmap = crec.str("bitmapName");
    int pw = 0, ph = 0;
    if (!mods.texSize(bitmap, pw, ph) && !coverSize(g, bitmap, pw, ph)) {
        if (error) *error = "cannot read the pixel size of the cover \"" + bitmap + "\"";
        return false;
    }
    const int bx = crec.i32("bitmapPositionX"), by = crec.i32("bitmapPositionY");

    // Where a normal tab's item grid starts, translated into this page's space. The reagent
    // boxes say where the MATERIALS are, which is a different thing: X-Mod moves them into a
    // cluster in the middle of a page whose whole top-left is free.
    ArzRecord stash, inv;
    bool haveInv = false;
    if (rec(kStashWindow, stash)) {
        const std::string invName = stash.str("InventoryDBR");
        haveInv = recs(invName.empty() ? std::string(kStashInventory) : invName, inv);
    }
    int x0 = 0, y0 = 0;
    bool fromStash = false;
    int wideR = 0, wideB = 0;   // the stash grid's right and bottom edge in page space, when it is wider than the cover
    if (haveInv) {
        const int ax = stash.i32("WindowLocationX") + inv.i32("inventoryX")
                     - page.i32("WindowLocationX");
        const int ay = stash.i32("WindowLocationY") + inv.i32("inventoryY")
                     - page.i32("WindowLocationY");
        // Inside the page's own plate, or the grid belongs to something else: Baul turns the
        // stash grid into the whole 929 px window and its anchor lands 503 px left of the page.
        if (ax >= bx && ax < bx + pw && ay >= by && ay < by + ph) {
            x0 = ax;
            y0 = ay;
            fromStash = true;
        } else if (inv.i32("inventoryXSize") > pw) {
            // A stash grid WIDER than this page's cover (Baul: the whole window): the page's
            // grid takes the stash grid's rectangle, the part left of its own cover included.
            x0 = ax;
            y0 = ay;
            fromStash = true;
            wideR = ax + inv.i32("inventoryXSize");
            wideB = ay + inv.i32("inventoryYSize");
            // ... but never past the caravan's own window art (the stash cover's rectangle).
            ArzRecord tw, scov;
            std::string twName = win.str("TransferWindow");
            if (twName.empty()) twName = "records/ui/caravan/caravan_transferwindow.dbr";
            int sw = 0, sh = 0;
            if (recs(twName, tw) && recs(tw.str("TransferPlate"), scov)
                && (mods.texSize(scov.str("bitmapName"), sw, sh) || coverSize(g, scov.str("bitmapName"), sw, sh))) {
                const int artR = tw.i32("WindowLocationX") + scov.i32("bitmapPositionX") + sw - page.i32("WindowLocationX");
                const int artB = tw.i32("WindowLocationY") + scov.i32("bitmapPositionY") + sh - page.i32("WindowLocationY");
                if (artR < wideR) wideR = artR;
                if (artB < wideB) wideB = artB;
            }
        }
    }
    if (!fromStash) {
        // The page's own boxes are the only signal left, and a tab strip down the left edge is
        // cleared by the stash page's left inset exactly as it was before.
        x0 = minX;
        if (stripLeft && haveInv && inv.i32("inventoryX") != 0) x0 = inv.i32("inventoryX");
        y0 = minY;
    }
    if (y0 < kPanelBandH) y0 = kPanelBandH;   // the label and the pad own the page's top band
    int usableW = pw - (x0 - bx), usableH = ph - (y0 - by);
    if (wideR > 0) {
        usableW = wideR - x0;
        if (wideB - y0 > usableH) usableH = wideB - y0;
    }
    if (usableW < 32 || usableH < 32) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "derived a %dx%d usable rectangle", usableW, usableH);
        if (error) *error = buf;
        return false;
    }

    out.x0 = x0;
    out.y0 = y0;
    out.usableW = usableW;
    out.usableH = usableH;
    out.plateW = pw;
    out.plateH = ph;
    out.bmpX = bx;
    out.bmpY = by;
    if (wideR > 0) {
        // The plate is the whole page - the cover and the widened grid together - so one plate
        // of that size covers the grid, and the mod's own cover still fits on it (plate_gen's
        // cover plate, which pages_gen's frame record names so the engine builds the page at it).
        out.bmpX = std::min(bx, x0);
        out.bmpY = std::min(by, y0);
        out.plateW = std::max(bx + pw, x0 + usableW) - out.bmpX;
        out.plateH = std::max(by + ph, y0 + usableH) - out.bmpY;
        out.widePlate = true;
    }
    out.plate = bitmap;
    out.tabStrip = stripLeft ? "left" : "top";
    {
        char buf[256];
        std::snprintf(buf, sizeof buf,
                      "grid %d,%d %dx%d from the merged database (origin from the %s); "
                      "cover %dx%d at %d,%d; tab strip %s",
                      x0, y0, usableW, usableH,
                      fromStash ? "stash grid" : "reagent boxes",
                      pw, ph, bx, by, out.tabStrip.c_str());
        out.note = std::string(buf) + " (" + bitmap + ")";
    }
    return true;
}

} // namespace gen
