// ut_viewprobe.h - does a custom game need a view of its own?
//
// Asked in the database detour every time a custom game is entered, from the game's own archives
// only (no base database, no text), so it costs milliseconds. Yes when the game touches the caravan
// (ModView::touchesCaravan: a records/ui/caravan/* override or a caravan texture) OR when its
// database adds or overrides a record the catalogue's mod rule could collect. A no costs nothing
// further: no generation, no folder, and the start-up view goes on serving the tab.
//
// THE ITEM TEST is the record-local half of the mod rule in gen/catalogue_gen.cpp (collectItems
// with a mod layer): the path is not on the junk list, any non-empty class but LootRandomizer and
// the five *Formula classes that are not recipes (the mod rule files a class its slot tables do not
// know under "other"), the record is a relic, an augment, a component (a mod's own gems and
// runes), a blueprint (ItemArtifactFormula, every classification), Epic or Legendary, and its
// FileDescription does not say BLANK. The name-tag half needs the text archives and is not read, so the test errs towards yes:
// a false yes costs one generation, a false no would show the start-up catalogue in a game that
// changed it. tools\test_logview.cpp proves the test admits every mod record the real rule
// collects (Reign of Terror, DeathGrimDawn, a scratch mod) and that the Crucible gets no view.
// What it does not see: a game that only DEMOTES a base unique (Legendary -> Rare) and adds nothing.
// WITH collect_mi=1 the rule also admits a monster infrequent - an equipment record of the 23 slot
// classes whose own classification is Rare (faction-store and crafted rares too) - and
// so does the probe, through the very function the catalogue uses (gen::isMiCandidate), so a mod
// that adds only MIs gets a view while the flag is on (and none while it is off).
#pragma once

#include "gen/arz_reader.h"
#include "gen/catalogue_gen.h"
#include "gen/ui_geometry.h"

#include <cstddef>
#include <cstring>
#include <string>

namespace ut {

// The mod rule's junk list (gen/catalogue_gen.cpp kJunkPrefix / kJunkPart), on a lower-cased key.
inline bool utProbeJunk(const std::string& key) {
    static const char* const kPrefix[] = {
        "records/sandbox/", "records/creatures/", "records/mod_testgrounds/",
        "records/endlessdungeon/scriptentities/",
    };
    for (const char* p : kPrefix)
        if (key.compare(0, std::strlen(p), p) == 0) return true;
    return key.find("/enemygear/") != std::string::npos;
}

// A path the probe never decodes: junk, or anything outside records/.
inline bool utProbeSkipPath(const std::string& key) {
    return key.compare(0, 8, "records/") != 0 || utProbeJunk(key);
}

// The record-local half of the mod rule: could the catalogue collect this mod record?
inline bool utProbeItemRecord(const std::string& key, const std::string& cls,
                              const std::string& classification, const std::string& fileDescription,
                              bool collectMi = false) {
    if (utProbeJunk(key)) return false;
    if (cls.empty() || cls == "LootRandomizer") return false;
    // the blueprint classes that are not recipes: never collected (catalogue_gen.cpp)
    if (cls == "ItemAscensionFormula" || cls == "AscendantAltarFormula" ||
        cls == "ItemRandomSetFormula" || cls == "ItemRerollFormula" || cls == "ItemSetFormula")
        return false;
    // a known class or not, the mod rule takes it (an unknown one as "other")
    const bool gate = cls == "ItemArtifact" || cls == "ItemEnchantment" || cls == "ItemRelic" ||
                      cls == "ItemArtifactFormula" ||
                      classification == "Epic" || classification == "Legendary" ||
                      (collectMi && gen::isMiCandidate(key, cls, classification, fileDescription));
    if (!gate) return false;
    std::string fd = fileDescription;
    for (char& c : fd)
        if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    return fd.find("BLANK") == std::string::npos;
}

struct UtViewProbe {
    bool caravan = false;        // ModView::touchesCaravan
    std::size_t items = 0;       // records utProbeItemRecord admits (the walk stops at the first unless `all`)
    std::size_t decoded = 0;     // record bodies read
    std::size_t records = 0;     // records in the game's archives
    std::string sample;          // the first admitted record, as named in the archive
    bool needsView() const { return caravan || items > 0; }
};

// The caravan first (the index alone answers it); then, when that is no, the item walk, which
// decodes only records/ paths off the junk list and stops at the first admitted record. `all`
// walks every record (the tests' census). The last layer that has a record wins it, as in the
// engine, so a record a later archive replaces is judged once, by its winner. collectMi:
// uniquetab.ini's collect_mi (the monster infrequents count as collectible).
inline void utProbeView(const gen::ModView& v, UtViewProbe& out, bool all = false,
                        bool collectMi = false) {
    out = UtViewProbe();
    out.caravan = v.touchesCaravan();
    const std::vector<gen::ArzArchive>& layers = v.arzLayers();
    for (const gen::ArzArchive& a : layers) out.records += a.recordCount();
    if (out.caravan && !all) return;
    gen::ArzRecord rec;
    for (std::size_t li = 0; li < layers.size(); ++li) {
        const gen::ArzArchive& a = layers[li];
        for (const gen::ArzEntry& e : a.entries()) {
            if (utProbeSkipPath(e.key)) continue;
            bool later = false;
            for (std::size_t lj = li + 1; lj < layers.size() && !later; ++lj)
                later = layers[lj].entry(e.key) != nullptr;
            if (later) continue;
            ++out.decoded;
            if (!a.decode(e, rec)) continue;
            if (!utProbeItemRecord(e.key, rec.str("Class"), rec.str("itemClassification"),
                                   rec.str("FileDescription"), collectMi))
                continue;
            if (!out.items++) out.sample = e.name;
            if (!all) return;
        }
    }
}

}  // namespace ut
