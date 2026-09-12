// ut_generate.cpp - see ut_generate.h.
#include "ut_generate.h"

#include "gen/generate.h"
#include "ut_config.h"
#include "ut_log.h"
#include "ut_paths.h"

#include <cctype>
#include <string>

namespace ut {

namespace {

// The ini value names a file suffix: letters and digits only, up to seven of them, else EN.
std::string textLanguage() {
    std::string lang;
    for (const char* p = g_cfg.textLanguage; *p && lang.size() < 7; ++p) {
        if (!std::isalnum((unsigned char)*p)) { lang.clear(); break; }
        lang += (char)std::toupper((unsigned char)*p);
    }
    if (lang.empty()) {
        if (g_cfg.textLanguage[0]) {
            logW("config: text_language=%s is not a Text_<lang>.arc suffix - using EN",
                 g_cfg.textLanguage);
        }
        lang = "EN";
    }
    return lang;
}

struct StartCtx {
    const char* gameDir;
    const char* modDir;
    const char* pagesArz;
    const std::string* lang;
};

// Printed before the archives are read, so the log says why the start-up pauses for a few seconds.
void sayStart(const gen::GenReport& report, void* ctx) {
    const StartCtx* c = (const StartCtx*)ctx;
    logI("catalogue: building from the game's database (%s, names from Text_%s.arc) ...",
         report.reason.empty() ? "no reason recorded" : report.reason.c_str(), c->lang->c_str());
    logD("catalogue: game folder \"%s\", output folder \"%s\", page archive \"%s\"", c->gameDir,
         c->modDir, c->pagesArz);
    logFlush();
}

}  // namespace

void generateEnsure(HMODULE selfModule) {
    char gameDir[MAX_PATH] = {0};
    char modDir[MAX_PATH] = {0};
    char pagesArz[MAX_PATH] = {0};
    if (!utGameDirA(gameDir, sizeof(gameDir))) {
        logE("catalogue: the game folder could not be resolved from the host exe's own path - "
             "the data files in the mod folder are used as they are");
        return;
    }
    utModDirA(selfModule, modDir, sizeof(modDir));
    utModFile(selfModule, "uniq-pages.arz", pagesArz, sizeof(pagesArz));
    const std::string lang = textLanguage();

    gen::GenReport report;
    StartCtx ctx = {gameDir, modDir, pagesArz, &lang};
    const bool ok = gen::ensureOutputsGuarded(gameDir, modDir, pagesArz, lang, report, &sayStart,
                                              &ctx);
    if (ok && report.upToDate) {
        logD("catalogue: catalogue.stamp matches the game files (Text_%s) - catalogue.bin and "
             "the uniq-*.txt files in \"%s\" are current",
             lang.c_str(), modDir);
        return;
    }
    for (const std::string& w : report.warnings) logW("catalogue: %s", w.c_str());
    if (ok) {
        logI("catalogue: built - %s", report.summary.c_str());
    } else {
        logE("catalogue: generation FAILED - %s. The files already in \"%s\" are kept",
             report.error.c_str(), modDir);
    }
}

}  // namespace ut
