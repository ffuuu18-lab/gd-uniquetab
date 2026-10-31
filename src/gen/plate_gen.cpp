// plate_gen.cpp - see plate_gen.h.
#include "gen/plate_gen.h"
#include "gen/arc_reader.h"
#include "gen/arz_reader.h"
#include "gen/catalogue_gen.h"
#include "ut_layout.h"   // utLatticeFrame: the frame under one cell, the rule the page fits by

#include <algorithm>
#include <cstdio>
#include <cstring>

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
const char* const kTransferWindow = "records/ui/caravan/caravan_transferwindow.dbr";
// The donor frame: box 3 of the vanilla materials page, the cleanest 32x32 frame on the plate.
const char* const kDonorBox = "records/ui/caravan/reagents/materials/caravan_reagent003.dbr";
const char* const kVanillaCover = "ui/caravan/caravan_transfercomponent1_bg.tex";
const char* const kStashCover = "ui/caravan/caravan_transfercoverimage.tex";

// The vanilla materials plate is drawn at its own bitmapPosition 0,1; the donor rectangle is
// in PLATE pixels, so the box coordinates are moved by that and grown by the frame's outset.
const int kPlateDX = 0, kPlateDY = 1;
const int kOutset = ut::kUtFrameOutset;

// The vanilla cover image's interior lattice, measured on 1.3.0.8: vertical lines every 32 px
// from x = 104, horizontal every 32 px from y = 4, all 1 px wide, all inside the grid.
const int kLatX0 = 104, kLatStep = 32, kLatNX = 11;
const int kLatY0 = 4, kLatNY = 20;

// Where the flat modded ground takes its colour from: the centre of the lattice cell bounded by
// x = 296/328 and y = 164/196 on the VANILLA stash cover - 16 px from every line and the calmest
// cell centre on the texture. It reads 06 06 06, the median of the whole non-gold interior.
// kGroundRgb is that value compiled in, used only when UI.arc's copy cannot be read.
const int kGroundX = 312, kGroundY = 180;
const std::uint32_t kGroundRgb = 0x060606u;

// Six hex digits, with an optional # or 0x. Anything else (empty included) is "not set".
bool parseHexRgb(const std::string& text, std::uint32_t* out) {
    std::size_t i = 0;
    if (i < text.size() && text[i] == '#') ++i;
    else if (i + 1 < text.size() && text[i] == '0' && (text[i + 1] | 0x20) == 'x') i += 2;
    if (text.size() - i != 6) return false;
    std::uint32_t v = 0;
    for (; i < text.size(); ++i) {
        const char c = text[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if ((c | 0x20) >= 'a' && (c | 0x20) <= 'f') d = (c | 0x20) - 'a' + 10;
        else return false;
        v = (v << 4) | std::uint32_t(d);
    }
    *out = v;
    return true;
}

typedef PlateSurface Image;

std::uint32_t rd32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16)
         | (std::uint32_t(p[3]) << 24);
}
void wr32(std::uint8_t* p, std::uint32_t v) {
    p[0] = std::uint8_t(v); p[1] = std::uint8_t(v >> 8);
    p[2] = std::uint8_t(v >> 16); p[3] = std::uint8_t(v >> 24);
}

// TEX v2 wrapping an uncompressed 24/32-bit DDSR surface - what every caravan cover is. A
// compressed or mipmapped one is refused by name, and the caller falls back.
bool texDecode(const std::vector<std::uint8_t>& blob, Image& out, std::string* why) {
    if (blob.size() < 16 || std::memcmp(blob.data(), "TEX\x02", 4) != 0) {
        if (why) *why = "not a TEX v2 file";
        return false;
    }
    const std::uint8_t* p = blob.data() + 12;
    const std::size_t n = blob.size() - 12;
    if (n < 132 || std::memcmp(p, "DDSR", 4) != 0) {
        if (why) *why = "not a DDSR surface";
        return false;
    }
    const std::uint32_t hsize = rd32(p + 4), hh = rd32(p + 12), ww = rd32(p + 16),
                        mips = rd32(p + 28);
    const std::uint32_t fourCC = rd32(p + 4 + 72 + 8), bpp = rd32(p + 4 + 72 + 12);
    char buf[128];
    if (hsize != 124) {
        if (why) { std::snprintf(buf, sizeof buf, "unexpected DDS header size %u", hsize); *why = buf; }
        return false;
    }
    if (fourCC != 0 || (bpp != 24 && bpp != 32)) {
        if (why) {
            std::snprintf(buf, sizeof buf, "only uncompressed 24/32-bit surfaces are handled "
                          "(fourCC=%u bpp=%u)", fourCC, bpp);
            *why = buf;
        }
        return false;
    }
    if (mips > 1) {
        if (why) { std::snprintf(buf, sizeof buf, "mipmapped source (%u)", mips); *why = buf; }
        return false;
    }
    if (ww == 0 || ww > 4096 || hh == 0 || hh > 4096) {
        if (why) *why = "implausible surface size";
        return false;
    }
    const std::size_t stride = bpp / 8;
    const std::size_t need = std::size_t(ww) * hh * stride;
    const std::size_t off = 4 + hsize;
    if (n - off < need) {
        if (why) { std::snprintf(buf, sizeof buf, "short surface: %zu of %zu bytes", n - off, need); *why = buf; }
        return false;
    }
    out.resize(int(ww), int(hh));
    const std::uint8_t* src = p + off;
    std::uint8_t* dst = out.px.data();
    for (std::size_t i = 0, count = std::size_t(ww) * hh; i < count; ++i) {
        dst[0] = src[2];                         // BGR(A) -> RGBA
        dst[1] = src[1];
        dst[2] = src[0];
        dst[3] = stride == 4 ? src[3] : 255;     // a 24-bit surface is opaque
        src += stride;
        dst += 4;
    }
    return true;
}

// The exact byte layout tools/make_plates.py writes, which is what byte identity rests on.
void texEncode(const Image& img, std::vector<std::uint8_t>& out) {
    const std::size_t dds = 4 + 124;
    out.assign(12 + dds + std::size_t(img.w) * img.h * 4, 0);
    std::memcpy(out.data(), "TEX\x02", 4);
    wr32(out.data() + 4, 0);
    wr32(out.data() + 8, std::uint32_t(dds + std::size_t(img.w) * img.h * 4));
    std::uint8_t* p = out.data() + 12;
    std::memcpy(p, "DDSR", 4);
    wr32(p + 4, 124);
    wr32(p + 8, 0x1007);
    wr32(p + 12, std::uint32_t(img.h));
    wr32(p + 16, std::uint32_t(img.w));
    wr32(p + 28, 1);                              // mipmap count
    std::uint8_t* pf = p + 4 + 72;                // pixel format
    wr32(pf + 0, 32);
    wr32(pf + 4, 0x40);
    wr32(pf + 12, 32);
    std::uint8_t* q = p + 4 + 124;
    for (std::size_t i = 0, count = std::size_t(img.w) * img.h; i < count; ++i) {
        const std::uint8_t* s = img.px.data() + i * 4;
        q[0] = s[2]; q[1] = s[1]; q[2] = s[0]; q[3] = s[3];
        q += 4;
    }
}

bool isGold(const std::uint8_t* p) {
    return p[3] > 40 && p[0] > 55 && p[1] > 45 && int(p[0]) > int(p[2]) + 8;
}

void goldMask(const Image& img, std::vector<char>& m) {
    m.assign(std::size_t(img.w) * img.h, 0);
    for (std::size_t i = 0, n = m.size(); i < n; ++i) m[i] = isGold(img.px.data() + i * 4) ? 1 : 0;
}

// The lattice band. halo widens each line: a line is 1 px of gold flanked by 1 px of black, so
// the ERASE mask needs halo 1 while the ornament test needs halo 0 (and the gold).
void latticeMask(const Image& img, int halo, std::vector<char>& m) {
    m.assign(std::size_t(img.w) * img.h, 0);
    const int x0 = kLatX0, x1 = kLatX0 + kLatStep * (kLatNX - 1);
    const int y0 = kLatY0, y1 = kLatY0 + kLatStep * (kLatNY - 1);
    auto set = [&](int x, int y) {
        if (x >= 0 && x < img.w && y >= 0 && y < img.h) m[std::size_t(y) * img.w + x] = 1;
    };
    for (int i = 0; i < kLatNX; ++i)
        for (int d = -halo; d <= halo; ++d)
            for (int y = std::max(0, y0 - halo); y <= std::min(img.h - 1, y1 + halo); ++y)
                set(kLatX0 + kLatStep * i + d, y);
    for (int i = 0; i < kLatNY; ++i)
        for (int d = -halo; d <= halo; ++d)
            for (int x = std::max(0, x0 - halo); x <= std::min(img.w - 1, x1 + halo); ++x)
                set(x, kLatY0 + kLatStep * i + d);
    if (halo == 0)
        for (std::size_t i = 0, n = m.size(); i < n; ++i)
            if (m[i] && !isGold(img.px.data() + i * 4)) m[i] = 0;
}

// 3 px bands: take the nearest clean pixel sideways, then diagonally. Each direction reads a
// SNAPSHOT of the image as it was when that direction started, exactly as the numpy roll does,
// and never samples a masked or gold pixel (the ornament sits against the first lattice line).
void eraseLines(Image& img, const std::vector<char>& mask, const std::vector<char>& gold) {
    static const int kDirs[14][2] = {{0, 5}, {0, -5}, {5, 0}, {-5, 0}, {5, 5}, {5, -5},
                                     {-5, 5}, {-5, -5}, {0, 9}, {0, -9}, {9, 0}, {-9, 0},
                                     {13, 0}, {-13, 0}};
    std::vector<char> todo = mask;
    std::vector<char> forbidden(mask.size());
    for (std::size_t i = 0; i < mask.size(); ++i) forbidden[i] = (mask[i] || gold[i]) ? 1 : 0;
    std::vector<std::uint8_t> snap;
    for (int d = 0; d < 14; ++d) {
        if (std::find(todo.begin(), todo.end(), 1) == todo.end()) break;
        const int dy = kDirs[d][0], dx = kDirs[d][1];
        snap = img.px;
        for (int y = 0; y < img.h; ++y) {
            const int sy = y + dy;
            if (sy < 0 || sy >= img.h) continue;
            for (int x = 0; x < img.w; ++x) {
                const std::size_t i = std::size_t(y) * img.w + x;
                if (!todo[i]) continue;
                const int sx = x + dx;
                if (sx < 0 || sx >= img.w) continue;
                const std::size_t j = std::size_t(sy) * img.w + sx;
                if (forbidden[j]) continue;
                std::memcpy(img.px.data() + i * 4, snap.data() + j * 4, 4);
                todo[i] = 0;
            }
        }
    }
}

// A 9-slice ALPHA STAMP cut from one real vanilla frame: only the gold line pixels carry, so
// the donor plate's own (different) ground never contaminates the cover-image ground.
struct Stamp {
    int c = 6, t = 2;
    Image tl, tr, bl, br, top, bot, left, right;
};

void cut(const Image& src, int x, int y, int w, int h, Image& out) {
    out.resize(w, h);
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i) {
            const std::uint8_t* s = src.at(x + i, y + j);
            std::uint8_t* d = out.at(i, j);
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
            d[3] = isGold(s) ? 255 : 0;
        }
}

bool makeStamp(const Image& donor, int x, int y, int w, int h, Stamp& st) {
    if (x < 0 || y < 0 || x + w > donor.w || y + h > donor.h) return false;
    const int c = st.c, t = st.t;
    cut(donor, x, y, c, c, st.tl);
    cut(donor, x + w - c, y, c, c, st.tr);
    cut(donor, x, y + h - c, c, c, st.bl);
    cut(donor, x + w - c, y + h - c, c, c, st.br);
    cut(donor, x + c, y, w - 2 * c, t, st.top);
    cut(donor, x + c, y + h - t, w - 2 * c, t, st.bot);
    cut(donor, x, y + c, t, h - 2 * c, st.left);
    cut(donor, x + w - t, y + c, t, h - 2 * c, st.right);
    return true;
}

// The stamp's ground stays put; only the line is transferred. tileW/tileH > 0 repeat the strip
// to that length, which is what tile_h / tile_v do before the blit.
void blit(Image& dst, const Image& src, int px, int py, int tileW, int tileH) {
    const int w = tileW > 0 ? tileW : src.w;
    const int h = tileH > 0 ? tileH : src.h;
    for (int j = 0; j < h; ++j) {
        const int dy = py + j;
        if (dy < 0 || dy >= dst.h) continue;
        for (int i = 0; i < w; ++i) {
            const int dx = px + i;
            if (dx < 0 || dx >= dst.w) continue;
            const std::uint8_t* s = src.at(i % src.w, j % src.h);
            if (!s[3]) continue;
            std::uint8_t* d = dst.at(dx, dy);
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
        }
    }
}

void drawFrame(Image& dst, const Stamp& st, int x, int y, int w, int h) {
    const int c = st.c, t = st.t;
    if (w < 2 * c + 2 || h < 2 * c + 2) return;
    blit(dst, st.tl, x, y, 0, 0);
    blit(dst, st.tr, x + w - c, y, 0, 0);
    blit(dst, st.bl, x, y + h - c, 0, 0);
    blit(dst, st.br, x + w - c, y + h - c, 0, 0);
    blit(dst, st.top, x + c, y, w - 2 * c, 0);
    blit(dst, st.bot, x + c, y + h - t, w - 2 * c, 0);
    blit(dst, st.left, x, y + c, 0, h - 2 * c);
    blit(dst, st.right, x + w - t, y + c, 0, h - 2 * c);
}

// The plate's own size: extra rows are the last one repeated, extra columns are cut.
void fitTo(Image& img, int w, int h) {
    if (img.w == w && img.h == h) return;
    Image out;
    out.resize(w, h);
    for (int y = 0; y < h; ++y) {
        const int sy = std::min(y, img.h - 1);
        for (int x = 0; x < w; ++x) {
            const std::uint8_t* s = img.at(std::min(x, img.w - 1), sy);
            std::memcpy(out.at(x, y), s, 4);
        }
    }
    img = out;
}

void fitMask(std::vector<char>& m, int mw, int mh, int w, int h) {
    if (mw == w && mh == h) return;
    std::vector<char> out(std::size_t(w) * h, 0);
    for (int y = 0; y < h; ++y) {
        const int sy = std::min(y, mh - 1);
        for (int x = 0; x < w; ++x) out[std::size_t(y) * w + x] = m[std::size_t(sy) * mw + std::min(x, mw - 1)];
    }
    m.swap(out);
}

// A bitmapName out of the merged view: a mod .arc first (rooted at its own prefix), then UI.arc.
bool readCover(const GameData& g, const ModView* mods, const std::string& bitmap,
               std::vector<std::uint8_t>& out) {
    if (bitmap.empty()) return false;
    if (mods && mods->texRead(bitmap, out)) return true;
    if (!g.ui) return false;
    const std::string key = ArcArchive::normKey(bitmap);
    if (key.compare(0, 3, "ui/") == 0 && g.ui->read(key.substr(3), out)) return true;
    return g.ui->read(key, out);
}

} // namespace

bool plateDecodeTex(const std::vector<std::uint8_t>& blob, PlateSurface& out, std::string* why) {
    return texDecode(blob, out, why);
}

bool plateReadCover(const GameData& g, const ModView* mods, const std::string& bitmap,
                    PlateSurface& out, std::string* why) {
    std::vector<std::uint8_t> blob;
    if (!readCover(g, mods, bitmap, blob)) {
        if (why) *why = "no such texture";
        return false;
    }
    return texDecode(blob, out, why);
}

std::string plateCoverName(int plateW, int plateH, const std::string& plateTag) {
    char name[160];
    std::snprintf(name, sizeof name, "uniq_plate_cover_%dx%d%s%s", plateW, plateH,
                  plateTag.empty() ? "" : "_", plateTag.c_str());
    return name;
}

std::string plateLeafName(int cellW, int cellH, int plateW, int plateH, const std::string& plateTag,
                          const std::string& layoutHash) {
    // THE NAME. A custom view always carries the plate size and its tag, whatever its cover
    // size: every view's plates share one override folder, and a custom game with the vanilla
    // cover size would otherwise write the main campaign's names over a different ground. A
    // start-up view keeps the shipped name on the vanilla cover and carries the plate size on any
    // other. A user grid's set always carries the cover, the tag and _L<hash>, so it can never
    // be mistaken for (or overwrite) a derived set. ut_plate.cpp builds the same names.
    char name[192];
    const bool vanilla = plateW == 438 && plateH == 627;
    if (!layoutHash.empty())
        std::snprintf(name, sizeof name, "uniq_plate_%dx%d_%dx%d%s%s_L%s", cellW, cellH, plateW,
                      plateH, plateTag.empty() ? "" : "_", plateTag.c_str(), layoutHash.c_str());
    else if (!plateTag.empty())
        std::snprintf(name, sizeof name, "uniq_plate_%dx%d_%dx%d_%s", cellW, cellH, plateW,
                      plateH, plateTag.c_str());
    else if (vanilla)
        std::snprintf(name, sizeof name, "uniq_plate_%dx%d", cellW, cellH);
    else
        std::snprintf(name, sizeof name, "uniq_plate_%dx%d_%dx%d", cellW, cellH, plateW, plateH);
    return name;
}

bool generatePlates(const GameData& g, const ModView* mods, const UiGeometry& geo,
                    const std::vector<CellGrid>& cells, const std::string& groundHex,
                    const std::string& plateTag, PlateSink sink, void* ctx, PlateReport& rep,
                    std::string* error, const std::string& layoutHash, int rowGap) {
    if (!g.db || !g.ui) { if (error) *error = "game data not loaded"; return false; }
    auto rec = [&](const std::string& name, ArzRecord& r) {
        return (mods && mods->get(name, r)) || g.db->get(name, r);
    };
    char line[320];
    std::string why;

    // ---- the ground: what a normal stash tab shows under this rectangle -------------------
    ArzRecord win, tw, scov, page;
    if (!rec(kCaravanWindow, win) || !rec(kMaterialWindow, page)) {
        if (error) *error = "the caravan window records are not in the database";
        return false;
    }
    std::string twName = win.str("TransferWindow");
    if (twName.empty()) twName = kTransferWindow;
    std::string covName;
    if (rec(twName, tw)) covName = tw.str("TransferPlate");
    std::string stashBitmap;
    int sx = 0, sy = 0;
    if (!covName.empty() && rec(covName, scov)) {
        stashBitmap = scov.str("bitmapName");
        sx = tw.i32("WindowLocationX") + scov.i32("bitmapPositionX");
        sy = tw.i32("WindowLocationY") + scov.i32("bitmapPositionY");
    }
    const int matX = page.i32("WindowLocationX") + geo.bmpX;
    const int matY = page.i32("WindowLocationY") + geo.bmpY;

    Image ground, under;   // under: the stash cover's pixels under the page, for the cover plate
    bool vanillaGround = false;
    std::vector<std::uint8_t> blob;
    if (!stashBitmap.empty() && readCover(g, mods, stashBitmap, blob)
        && texDecode(blob, ground, &why)) {
        const int offX = matX - sx, offY = matY - sy;
        vanillaGround = stashBitmap == kStashCover && ground.w == 438 && ground.h == 626;
        if (ground.w == geo.plateW && offX == 0) {
            std::snprintf(line, sizeof line, "ground: the whole stash cover %s %dx%d",
                          stashBitmap.c_str(), ground.w, ground.h);
            rep.ground = line;
        } else if (offX >= 0 && offX + geo.plateW <= ground.w && offY >= 0
                   && offY + geo.plateH <= ground.h) {
            Image c;
            c.resize(geo.plateW, geo.plateH);
            for (int y = 0; y < geo.plateH; ++y)
                std::memcpy(c.at(0, y), ground.at(offX, offY + y), std::size_t(geo.plateW) * 4);
            std::snprintf(line, sizeof line, "ground: the stash cover %s %dx%d cut at %d,%d",
                          stashBitmap.c_str(), ground.w, ground.h, offX, offY);
            rep.ground = line;
            ground = c;
            under = c;
            vanillaGround = false;   // the lattice is measured on the whole texture, not a cut
        } else {
            std::snprintf(line, sizeof line,
                          "ground: a flat one - the page's rectangle (%d,%d %dx%d) is not "
                          "inside the stash cover %s %dx%d",
                          offX, offY, geo.plateW, geo.plateH, stashBitmap.c_str(), ground.w,
                          ground.h);
            rep.ground = line;
            ground.px.clear();
            vanillaGround = false;
        }
    } else {
        std::snprintf(line, sizeof line,
                      "ground: a flat one - the stash cover %s cannot be read (%s)",
                      stashBitmap.empty() ? "record" : stashBitmap.c_str(),
                      why.empty() ? "no such texture" : why.c_str());
        rep.ground = line;
        ground.px.clear();
    }
    // A MODDED GROUND IS A FLAT FILL - no pixel of the mod's art is copied. See plate_gen.h.
    if (!vanillaGround) {
        std::uint32_t rgb = 0;
        char note[320];
        if (parseHexRgb(groundHex, &rgb)) {
            std::snprintf(note, sizeof note,
                          "plate ground: a flat %06X, from plate_ground in uniquetab.ini - no "
                          "pixel of the mod's own cover is copied", rgb);
        } else {
            Image van;
            std::string vwhy;
            if (readCover(g, nullptr, kStashCover, blob) && texDecode(blob, van, &vwhy)
                && van.w > kGroundX && van.h > kGroundY) {
                const std::uint8_t* s = van.at(kGroundX, kGroundY);
                rgb = (std::uint32_t(s[0]) << 16) | (std::uint32_t(s[1]) << 8) | s[2];
                std::snprintf(note, sizeof note,
                              "plate ground: a flat %06X, sampled from the vanilla %s at %d,%d - "
                              "no pixel of the mod's own cover is copied", rgb, kStashCover,
                              kGroundX, kGroundY);
            } else {
                rgb = kGroundRgb;
                std::snprintf(note, sizeof note,
                              "plate ground: a flat %06X - the vanilla %s could not be sampled "
                              "(%s), so the built-in tone is used", rgb, kStashCover,
                              vwhy.empty() ? "no such texture" : vwhy.c_str());
                rep.warnings.push_back(note);
            }
        }
        ground.resize(geo.plateW, geo.plateH);
        for (std::size_t i = 0, n = ground.px.size(); i < n; i += 4) {
            ground.px[i] = std::uint8_t(rgb >> 16);
            ground.px[i + 1] = std::uint8_t(rgb >> 8);
            ground.px[i + 2] = std::uint8_t(rgb);
            ground.px[i + 3] = 255;
        }
        rep.fill = note;
        rep.fillRgb = rgb;
        rep.flat = true;
    }
    if (under.w != geo.plateW || under.h != geo.plateH) under = ground;
    if (ground.w != geo.plateW || ground.h != geo.plateH) {
        // only the vanilla pair differs, and only by the one row the widget's rectangle wants
        if (ground.w < geo.plateW) {
            std::snprintf(line, sizeof line, "the ground is %dx%d but the plate is %dx%d",
                          ground.w, ground.h, geo.plateW, geo.plateH);
            if (error) *error = line;
            return false;
        }
    }

    // The lattice and the ornament are measured on the vanilla texture, so they run only there.
    std::vector<char> ornament;
    Image cov;
    if (vanillaGround) {
        std::vector<char> lat, gold;
        latticeMask(ground, 1, lat);
        goldMask(ground, gold);
        cov = ground;                              // untouched, for the ornament pass
        eraseLines(ground, lat, gold);
        latticeMask(cov, 0, lat);
        ornament.assign(gold.size(), 0);
        for (std::size_t i = 0; i < gold.size(); ++i) ornament[i] = (gold[i] && !lat[i]) ? 1 : 0;
        fitMask(ornament, cov.w, cov.h, geo.plateW, geo.plateH);
        fitTo(cov, geo.plateW, geo.plateH);
    }
    fitTo(ground, geo.plateW, geo.plateH);

    // ---- the frame donor: always a real vanilla frame out of UI.arc ------------------------
    ArzRecord box;
    if (!g.db->get(kDonorBox, box)) {
        if (error) *error = "the vanilla reagent box that donates the frame is missing";
        return false;
    }
    Image donor;
    if (!readCover(g, nullptr, kVanillaCover, blob) || !texDecode(blob, donor, &why)) {
        if (error) *error = std::string("cannot read the vanilla materials plate: ") + why;
        return false;
    }
    Stamp st;
    const int dx0 = box.i32("itemBoxX") - kPlateDX - kOutset;
    const int dy0 = box.i32("itemBoxY") - kPlateDY - kOutset;
    if (!makeStamp(donor, dx0, dy0, 32 + 2 * kOutset, 32 + 2 * kOutset, st)) {
        if (error) *error = "the donor frame is not inside the vanilla materials plate";
        return false;
    }
    donor.px.clear();
    donor.px.shrink_to_fit();

    // ---- one plate per cell size ------------------------------------------------------------
    // Every cell of the grid's cols x rows (utGridFit's, the page's) gets its frame, cut to the
    // plate where its outset crosses the edge (ut_layout.h utLatticeFrame).
    if (rowGap < 0) rowGap = ut::kUtRowGap;
    Image img;
    std::vector<std::uint8_t> tex;
    for (const CellGrid& cell : cells) {
        img = ground;
        int n = 0;
        for (int r = 0; r < cell.rows; ++r) {
            for (int c = 0; c < cell.cols; ++c) {
                ut::UtRect f;
                if (!ut::utLatticeFrame(geo.x0, geo.y0, cell.cellW, cell.cellH, rowGap, c, r,
                                        geo.bmpX, geo.bmpY, img.w, img.h, &f))
                    continue;
                drawFrame(img, st, f.x, f.y, f.w, f.h);
                ++n;
            }
        }
        if (!ornament.empty())                     // the vanilla border always wins
            for (std::size_t i = 0; i < ornament.size(); ++i)
                if (ornament[i]) std::memcpy(img.px.data() + i * 4, cov.px.data() + i * 4, 4);
        const std::string name =
            plateLeafName(cell.cellW, cell.cellH, geo.plateW, geo.plateH, plateTag, layoutHash);
        texEncode(img, tex);
        if (sink && !sink(name, tex, cell, n, ctx)) {
            if (error) *error = std::string("the plate ") + name + " could not be handed over";
            return false;
        }
        ++rep.painted;
        rep.frames += std::size_t(n);
    }
    // THE COVER PLATE of a widened page: what was under the page, with the record's own cover
    // blended on at its own place, so the Crafting Materials page looks as it did. Derived set only.
    if (geo.widePlate && layoutHash.empty()) {
        img = under;
        ArzRecord crec;
        Image art;
        if (rec(page.str("TransferPlate"), crec) && readCover(g, mods, crec.str("bitmapName"), blob)
            && texDecode(blob, art, &why)) {
            const int cx = crec.i32("bitmapPositionX") - geo.bmpX, cy = crec.i32("bitmapPositionY") - geo.bmpY;
            for (int y = 0; y < art.h; ++y) {
                if (cy + y < 0 || cy + y >= img.h) continue;
                for (int x = 0; x < art.w; ++x) {
                    if (cx + x < 0 || cx + x >= img.w) continue;
                    const std::uint8_t* s = art.at(x, y);
                    std::uint8_t* d = img.at(cx + x, cy + y);
                    const int a = s[3];
                    for (int k = 0; k < 3; ++k) d[k] = std::uint8_t((s[k] * a + d[k] * (255 - a) + 127) / 255);
                    d[3] = 255;
                }
            }
        } else {
            rep.warnings.push_back("the page's own cover could not be read for the cover plate (" +
                                   (why.empty() ? std::string("no such texture") : why) + ")");
        }
        texEncode(img, tex);
        const std::string name = plateCoverName(geo.plateW, geo.plateH, plateTag);
        if (sink && !sink(name, tex, CellGrid(), 0, ctx)) {
            if (error) *error = std::string("the plate ") + name + " could not be handed over";
            return false;
        }
        ++rep.painted;
    }
    return true;
}

bool plateWriteSink(const std::string& name, const std::vector<std::uint8_t>& tex,
                    const CellGrid&, int, void* ctx) {
    PlateWriter* w = (PlateWriter*)ctx;
    const std::string dst = w->dir + "\\" + name + ".tex";
    // Already there with exactly these bytes: the engine's file is left alone entirely.
    HANDLE h = CreateFileA(dst.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER sz;
        bool same = GetFileSizeEx(h, &sz) && std::uint64_t(sz.QuadPart) == tex.size();
        if (same) {
            std::vector<std::uint8_t> have(tex.size());
            DWORD got = 0;
            same = ReadFile(h, have.data(), DWORD(have.size()), &got, nullptr)
                   && got == have.size() && std::memcmp(have.data(), tex.data(), tex.size()) == 0;
        }
        CloseHandle(h);
        if (same) { ++w->same; w->names.push_back(name); return true; }
    }
    const std::string tmp = dst + ".tmp";
    DeleteFileA(tmp.c_str());
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f || (!tex.empty() && std::fwrite(tex.data(), 1, tex.size(), f) != tex.size())) {
        if (f) std::fclose(f);
        DeleteFileA(tmp.c_str());
        ++w->failed;
        return true;                               // a read-only install is the fallback case
    }
    std::fclose(f);
    w->renames.push_back(std::make_pair(tmp, dst));
    ++w->written;
    w->names.push_back(name);
    return true;
}

} // namespace gen
