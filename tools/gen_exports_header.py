"""Generate src/gd_exports.h from the Engine.dll / Game.dll export dumps.

The dumps live in tools/exports/; --exports points at another copy of them.

    python gen_exports_header.py [--exports <dir>] [--out <file>] [--undname <exe>]

Every export the prototype may resolve with GetProcAddress or hook is emitted as

    // public: void __cdecl GAME::GameEngine::SetTransferOpen(bool) __ptr64
    #define GD_GAMEENGINE_SETTRANSFEROPEN "?SetTransferOpen@GameEngine@GAME@@QEAAX_N@Z"

The mangled names are copied verbatim out of the dumps (never typed by hand) and the comment is
undname.exe's output. Overloads get a numeric suffix in dump order; the comment tells them apart.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEFAULT_EXPORTS = os.path.join(HERE, "exports")
DEFAULT_OUT = os.path.join(ROOT, "src", "gd_exports.h")


def _find_undname():
    """undname.exe out of whichever MSVC toolchain is installed - never a written-down path."""
    found = shutil.which("undname")
    if found:
        return found
    vswhere = ""
    for var in ("ProgramFiles(x86)", "ProgramFiles"):
        base = os.environ.get(var)
        if not base:
            continue
        cand = os.path.join(base, "Microsoft Visual Studio", "Installer", "vswhere.exe")
        if os.path.exists(cand):
            vswhere = cand
            break
    if not vswhere:
        return ""
    try:
        out = subprocess.run(
            [vswhere, "-latest", "-products", "*",
             "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
             "-property", "installationPath"],
            check=True, stdout=subprocess.PIPE,
        ).stdout.decode("utf-8", "replace").strip().splitlines()
    except (OSError, subprocess.CalledProcessError):
        return ""
    if not out:
        return ""
    tools = os.path.join(out[0], "VC", "Tools", "MSVC")
    if not os.path.isdir(tools):
        return ""
    for ver in sorted(os.listdir(tools), reverse=True):
        cand = os.path.join(tools, ver, "bin", "Hostx64", "x64", "undname.exe")
        if os.path.exists(cand):
            return cand
    return ""


DEFAULT_UNDNAME = _find_undname()

# (section title, module, macro prefix, [regex, ...]) - a name is taken if ANY regex matches.
SELECTION = [
    ("Engine singleton and per-frame hooks", "Engine", "GD_ENGINE", [
        r"^\?gEngine@GAME@@",
        r"^\?(GetGraphicsEngine|GetGameInfo|GetFrameCount|PresentSurface|GetDatabaseArchive"
        r"|HasLoadedCustomDatabase|GetEntityRenderFilter)@Engine@GAME@@",
    ]),
    ("GraphicsEngine", "Engine", "GD_GFXENGINE", [
        r"^\?(GetCanvas|LoadTexture|LoadFont|UnloadTexture|UnloadFont|PresentSurface)@GraphicsEngine@GAME@@",
    ]),
    ("GraphicsCanvas - frame, size, viewport, clipping", "Engine", "GD_CANVAS", [
        r"^\?(BeginFrame|EndFrame|GetWidth|GetHeight|GetViewport|SetViewport"
        r"|GetClippingRect|SetClippingRect|ClearClippingRect|SetDefaultState)@GraphicsCanvas@GAME@@",
    ]),
    ("GraphicsCanvas - 2D drawing", "Engine", "GD_CANVAS", [
        r"^\?(RenderRect|RenderStyledRect|RenderShadedRect|RenderLine|RenderTriFan"
        r"|RenderHorizontalGradient|RenderVerticalGradient|DrawDynamicRect)@GraphicsCanvas@GAME@@",
    ]),
    ("GraphicsCanvas - text", "Engine", "GD_CANVAS", [
        r"^\?(RenderText2d|RenderText2dBox|RenderColoredText2d|RenderText2dParagraph"
        r"|CalcTextRect)@GraphicsCanvas@GAME@@",
    ]),
    ("GraphicsTexture / GraphicsFont2", "Engine", "GD_TEXTURE", [
        r"^\?(GetTexture|GetWidth|GetHeight|GetName)@GraphicsTexture@GAME@@",
        r"^\?(GetHeight|GetLineHeight|GetName)@GraphicsFont2@GAME@@",
    ]),
    ("Free 2D primitive helpers", "Engine", "GD_DRAW", [
        r"^\?(Draw2DRectangle|Draw2DOrientedQuad)@GAME@@YA",
        r"^\?(Enable2DMode|SetTexture0|SetColor|Begin|End|Flush)@GraphicsPrimitiveDrawer@GAME@@",
        r"^\?\?0GraphicsPrimitiveDrawer@GAME@@",
    ]),
    ("Window and input", "Engine", "GD_WINDOW", [
        r"^\?(WindowProc|GetSystemWindow|GetClientWidth|GetClientHeight|RegisterEventHandler"
        r"|UnregisterEventHandler)@WinWindow@GAME@@",
    ]),
    ("LocalizationManager (tag -> text)", "Engine", "GD_LOC", [
        r"^\?(Instance|GetLanguageString|GetLanguageTag|GetText|GetCurrentLanguage|GetLanguage)"
        r"@LocalizationManager@GAME@@",
    ]),
    ("Object lookup candidates (id -> Item*) - UNVERIFIED, see notes", "Engine", "GD_OBJ", [
        r"^\?Get@\?\$Singleton@VObjectManager@GAME@@@GAME@@",
        r"^\?(GetObjectList|GetNumObjects|GetNumDeletedObjects)@ObjectManager@GAME@@",
        r"^\?(GetObjectId|GetObjectName|GetObjectNameHash|GetObjectClass)@Object@GAME@@",
        r"^\?(Get|GetEntity)@UniqueIdMap@GAME@@",
    ]),
    ("GameEngine - stash / transfer", "Game", "GD_GAMEENGINE", [
        r"^\?(SetTransferOpen|IsTransferOpen|Update|GetPlayerTransfer|GetTransferSack"
        r"|GetSelectedTransferSackNumber|SetSelectedTransferSackNumber|GetTransferItemCount"
        r"|DisplayCaravanWindow|GetNumberOfTransferSacks|IsTransferStashLoaded)@GameEngine@GAME@@",
        r"^\?(MaxTransferSacks|Expansion1MaxTransferSacks|Expansion2MaxTransferSacks"
        r"|Expansion3MaxTransferSacks)@GameEngine@GAME@@2IB$",
    ]),
    ("GameEngine - database, UI metrics, item presentation", "Game", "GD_GAMEENGINE", [
        r"^\?(GetDatabase|GetInventoryCellSize|GetItemColor|GetItemBackground"
        r"|GetItemClassificationName|GetMainPlayer|GetGameTextStyle)@GameEngine@GAME@@",
    ]),
    ("Player - private stash", "Game", "GD_PLAYER", [
        r"^\?(GetPrivateStash|GetSack|GetItemCountInStashes|GetSelectedStashSackNumber"
        r"|SetSelectedStashSackNumber|GetMaximumSacks|GetCompatibleStash)@Player@GAME@@",
    ]),
    ("InventorySack - every exported method", "Game", "GD_SACK", [
        r"@InventorySack@GAME@@",
    ]),
    ("Item", "Game", "GD_ITEM", [
        r"^\?(CreateItem|GetItemReplicaInfo|GetItemClassification|GetBitmap|GetBitmapName"
        r"|GetStackSize|GetMaxStackSize|GetSeedRerolls|GetUIBitmapText|GetUIBitmapOverlay"
        r"|GetSymbolBitmapName|GetItemLevel|GetItemTextTag|GetItemTypeTag|GetItemType"
        r"|GetLevelRequirement|GetDropClassification|GetRolloverSize|GetUIDisplayText"
        r"|GetSimpleDescription|GetGameDescription|IsSoulbound|IsUntradeable|PassLootFilter"
        r"|CannotPickUp|CannotPickUpMultiple|IsItemAvailable|IsReagentCompatible"
        r"|CanBePlacedInTransferStash)@Item@GAME@@",
    ]),
    ("ItemEquipment", "Game", "GD_ITEMEQUIP", [
        r"^\?(GetItemSetName|GetUIDisplayText|GetItemClassification|GetBitmap|GetBitmapName"
        r"|GetSearchText|GetRelic|GetEnchantment|HasRelic|HasEnchantment|GetLevelRequirement"
        r"|GetUIBitmapOverlay)@ItemEquipment@GAME@@",
    ]),
]

# Exports that must never be called by this prototype (read-only milestones M1-M4).
FORBIDDEN = re.compile(
    r"^\?(AddItem|AddItemToTransfer|AddItemToPrivateStash|RemoveItem|RemoveAllItems|RemoveItemFrom"
    r"|DestroyAllItems|DeleteAndCreateAllItemsFor|GiveAllItems|TakeAllItems|TakeItemFrom"
    r"|ReplaceItem|SaveTransferStash|CreateItem|Sort|ArrangeUnpositionedItems|CompleteRelics"
    r"|DepositSackIntoReagents|LoadSackIntoTransmutes|AddSack|RestoreNumberOfSacks|SetDims"
    r"|SetBorder|SetBorderColor|SetSymbol|SetSymbolColor|SetButtonName)")

HEADER_TOP = r'''// gd_exports.h - generated by tools/gen_exports_header.py. DO NOT EDIT BY HAND.
//
// Grim Dawn 1.3.0.8 x64 (build 24825149).
// Exported symbol names copied verbatim from tools/exports/{Engine,Game}-x64-exports.txt;
// the comment above each macro is undname.exe output for that exact string.
//
// Usage:
//     HMODULE hEngine = GetModuleHandleA("Engine.dll");
//     HMODULE hGame    = GetModuleHandleA("Game.dll");
//     auto SetTransferOpen = reinterpret_cast<GD_GameEngine_SetTransferOpen_t>(
//         GetProcAddress(hGame, GD_GAMEENGINE_SETTRANSFEROPEN));
//
// Calling convention: every method here is a non-static member function compiled for the
// Microsoft x64 ABI, so the typedefs below take the object pointer as the first argument and
// are declared __cdecl (on x64 __cdecl/__thiscall/__stdcall all collapse to the same ABI).
// A member returning a class BY VALUE (Rect, Vec2, std::string, ...) also takes a hidden
// pointer to the return slot as the SECOND argument, before the declared ones. Those are
// marked "sret" in the notes; the typedefs in this header cover only the simple cases.
//
// Read-only rule for milestones M1-M4: names that create, move, delete or persist items are
// still listed (they are part of the class) but are marked WRITE - do not call them.

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------
// ItemClassification
// ---------------------------------------------------------------------------------------------
// GAME::ItemClassification appears in the exported signatures of
// GameEngine::GetItemColor, GameEngine::GetItemBackground and Item::GetItemClassification
// (mangled as W4ItemClassification@2@). The *names* are known from the database records
// (records/game/gameengine.dbr has ItemNameCommon / ItemNameMagical / ItemNameRare /
// ItemNameEpic / ItemNameLegendary / ItemNameQuest / ItemNameBroken / ItemNameArtifact /
// ItemNameArtifactFormula / ItemNameEnchantment / ItemNameLore / ItemNamePotion styles, and
// itemClassification values in item records are the strings Common, Magical, Rare, Epic,
// Legendary, Quest, Broken).
//
// *** THE NUMERIC ORDER IS UNVERIFIED. *** Nothing exported reveals it. Do not hard-code these
// values; at runtime, call GameEngine::GetItemClassificationName(c) for c = 0..N and read the
// returned tag, or compare Item::GetItemClassification() against a known item.
//
// Plausible (record-file order, NOT confirmed):
//   0 Common, 1 Magical, 2 Rare, 3 Epic, 4 Legendary, 5 Quest, 6 Broken
enum GD_ItemClassification_Unverified {
    GD_ITEMCLASS_UNKNOWN = -1
};

'''

TYPEDEFS = r'''
// ---------------------------------------------------------------------------------------------
// Suggested typedefs for the hook / call set (Microsoft x64 ABI, this-pointer first)
// ---------------------------------------------------------------------------------------------
// Opaque handles - never dereference these, always go through an exported accessor.
typedef struct GD_Engine        GD_Engine;
typedef struct GD_GraphicsEngine GD_GraphicsEngine;
typedef struct GD_Canvas        GD_Canvas;
typedef struct GD_Texture       GD_Texture;       // GAME::GraphicsTexture
typedef struct GD_RenderTexture GD_RenderTexture; // GAME::RenderTexture
typedef struct GD_Font          GD_Font;          // GAME::GraphicsFont2
typedef struct GD_GameEngine    GD_GameEngine;
typedef struct GD_Player        GD_Player;
typedef struct GD_Sack          GD_Sack;          // GAME::InventorySack
typedef struct GD_Item          GD_Item;
typedef struct GD_Rect          GD_Rect;          // layout unverified (probably 4 floats)
typedef struct GD_Vec2          GD_Vec2;          // layout unverified (probably 2 floats)
typedef struct GD_Color         GD_Color;         // layout unverified (probably 4 floats)
typedef struct GD_Viewport      GD_Viewport;
typedef struct GD_Name          GD_Name;

// Engine.dll
typedef GD_GraphicsEngine* (*GD_Engine_GetGraphicsEngine_t)(GD_Engine* self);
typedef void               (*GD_Engine_PresentSurface_t)(GD_Engine* self);
typedef unsigned int       (*GD_Engine_GetFrameCount_t)(GD_Engine* self);
typedef GD_Canvas*         (*GD_GfxEngine_GetCanvas_t)(GD_GraphicsEngine* self);          // returns a reference
typedef const GD_Texture*  (*GD_GfxEngine_LoadTexture_t)(GD_GraphicsEngine* self, const void* stdStringPath);
typedef const GD_Font*     (*GD_GfxEngine_LoadFont_t)(GD_GraphicsEngine* self, const void* stdStringPath);
typedef const GD_RenderTexture* (*GD_Texture_GetTexture_t)(const GD_Texture* self);
typedef int                (*GD_Canvas_GetWidth_t)(const GD_Canvas* self);
typedef int                (*GD_Canvas_GetHeight_t)(const GD_Canvas* self);
typedef void               (*GD_Canvas_RenderRectSolid_t)(GD_Canvas* self, const GD_Rect* r, const GD_Color* c);
typedef void               (*GD_Canvas_RenderRectTex_t)(GD_Canvas* self, const GD_Rect* dst, const GD_Rect* uv,
                                                        const GD_RenderTexture* tex, const GD_Color* tint);
typedef void               (*GD_Canvas_SetClippingRect_t)(GD_Canvas* self, const GD_Rect* r, bool intersect);
typedef void               (*GD_Canvas_ClearClippingRect_t)(GD_Canvas* self);
// CalcTextRect returns Rect by value -> hidden sret pointer as argument 2.
typedef GD_Rect*           (*GD_Canvas_CalcTextRect_t)(const GD_Canvas* self, GD_Rect* sret, GD_Rect text,
                                                       int x, int y, int xalign, int yalign);
// RenderText2d(int x, int y, const Color&, const Color& outline, const char*, const GraphicsFont2*,
//              int size, GraphicsXAlign, GraphicsYAlign, FontStyleFlag, FontLayout)
typedef void               (*GD_Canvas_RenderText2d_t)(GD_Canvas* self, int x, int y, const GD_Color* col,
                                                       const GD_Color* outline, const char* text,
                                                       const GD_Font* font, int size, int xalign, int yalign,
                                                       int styleFlags, int layout);

// Game.dll
typedef void        (*GD_GameEngine_SetTransferOpen_t)(GD_GameEngine* self, bool open);
typedef bool        (*GD_GameEngine_IsTransferOpen_t)(const GD_GameEngine* self);
typedef void        (*GD_GameEngine_Update_t)(GD_GameEngine* self, int deltaMs);
typedef void*       (*GD_GameEngine_GetPlayerTransfer_t)(GD_GameEngine* self);   // mem::vector<InventorySack*>&
typedef GD_Sack*    (*GD_GameEngine_GetTransferSack_t)(GD_GameEngine* self, int index);
typedef unsigned    (*GD_GameEngine_GetSelectedTransferSackNumber_t)(const GD_GameEngine* self);
typedef void        (*GD_GameEngine_SetSelectedTransferSackNumber_t)(GD_GameEngine* self, unsigned n);
typedef GD_Player*  (*GD_GameEngine_GetMainPlayer_t)(GD_GameEngine* self);
typedef void        (*GD_GameEngine_GetInventoryCellSize_t)(GD_GameEngine* self, float* w, float* h);
typedef const GD_Texture* (*GD_GameEngine_GetItemBackground_t)(const GD_GameEngine* self, int classification);
// int GetTransferItemCount(const std::string& record, const mem::vector<std::string>& a,
//                          const mem::vector<std::string>& b, mem::vector<unsigned>& out,
//                          bool f1, bool f2, bool f3) const
typedef int         (*GD_GameEngine_GetTransferItemCount_t)(const GD_GameEngine* self, const void* record,
                                                            const void* vecA, const void* vecB, void* outIds,
                                                            bool f1, bool f2, bool f3);
typedef void*       (*GD_Player_GetPrivateStash_t)(GD_Player* self);             // mem::vector<InventorySack*>&
typedef GD_Sack*    (*GD_Player_GetSack_t)(GD_Player* self, int index);
typedef const void* (*GD_Sack_GetInventory_t)(const GD_Sack* self);              // const mem::map<unsigned, Rect>&
typedef unsigned    (*GD_Sack_GetGridWidth_t)(const GD_Sack* self);
typedef bool        (*GD_Sack_ContainsItemById_t)(const GD_Sack* self, unsigned itemId);
typedef bool        (*GD_Sack_ContainsItemByRecord_t)(const GD_Sack* self, const void* stdStringRecord);
typedef void        (*GD_Item_GetItemReplicaInfo_t)(const GD_Item* self, void* outItemReplicaInfo);
typedef int         (*GD_Item_GetItemClassification_t)(const GD_Item* self, bool something);
typedef const GD_Texture* (*GD_Item_GetBitmap_t)(const GD_Item* self);
typedef void        (*GD_Item_GetBitmapName_t)(const GD_Item* self, void* outStdString);
// The InventorySack::AddItem hook signature (hooked to OBSERVE only, never called):
typedef bool        (*GD_Sack_AddItem_t)(GD_Sack* self, GD_Item* item, bool a, bool b);
'''

FOOTER = r'''
#ifdef __cplusplus
}  // extern "C"
#endif
'''


def undecorate(names, undname_exe, chunk=40):
    out = {}
    for i in range(0, len(names), chunk):
        part = names[i:i + chunk]
        try:
            res = subprocess.run([undname_exe] + part, capture_output=True, text=True, timeout=120)
        except (OSError, subprocess.SubprocessError) as exc:
            for n in part:
                out[n] = f"<undname failed: {exc}>"
            continue
        cur = None
        for line in res.stdout.splitlines():
            m = re.match(r'^Undecoration of :- "(.*)"$', line.strip())
            if m:
                cur = m.group(1)
                continue
            m = re.match(r'^is :- "(.*)"$', line.strip())
            if m and cur is not None:
                out[cur] = m.group(1)
                cur = None
        for n in part:
            out.setdefault(n, "<undname produced no output>")
    return out


def macro_name(prefix, mangled):
    m = re.match(r"^\?\??\$?([A-Za-z0-9_]+)@", mangled)
    base = m.group(1) if m else re.sub(r"[^A-Za-z0-9_]", "_", mangled)[:40]
    return f"{prefix}_{base.upper()}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exports", default=DEFAULT_EXPORTS)
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--undname", default=DEFAULT_UNDNAME)
    a = ap.parse_args()

    dumps = {}
    for mod, fn in (("Engine", "Engine-x64-exports.txt"), ("Game", "Game-x64-exports.txt")):
        p = os.path.join(a.exports, fn)
        with open(p, "r", encoding="latin-1") as fh:
            dumps[mod] = [ln.strip() for ln in fh if ln.strip()]
        print(f"{mod}: {len(dumps[mod])} exports from {p}")

    sections, all_names, taken = [], [], set()
    for title, mod, prefix, patterns in SELECTION:
        rx = [re.compile(p) for p in patterns]
        hits = [n for n in dumps[mod] if n not in taken and any(r.search(n) for r in rx)]
        taken.update(hits)
        sections.append((title, mod, prefix, hits))
        all_names.extend(hits)
    print(f"selected {len(all_names)} exports; undecorating...")
    und = undecorate(all_names, a.undname)
    bad = [n for n, v in und.items() if v.startswith("<")]
    if bad:
        print(f"WARNING: {len(bad)} names could not be undecorated, e.g. {bad[:3]}")

    lines = [HEADER_TOP]
    used = {}
    for title, mod, prefix, hits in sections:
        lines.append("// " + "-" * 93)
        lines.append(f"// {title}   [{mod}.dll]")
        lines.append("// " + "-" * 93)
        for n in hits:
            base = macro_name(prefix, n)
            used[base] = used.get(base, 0) + 1
            name = base if used[base] == 1 else f"{base}_{used[base]}"
            sig = und.get(n, "?")
            flag = "  ** WRITE - do not call in M1-M4 **" if FORBIDDEN.match(n) else ""
            sret = "  [returns by value -> hidden sret pointer as arg 2]" if re.search(
                r"QE[BA]A\?A[VU]", n) else ""
            lines.append(f"// {sig}{flag}{sret}")
            lines.append(f'#define {name} "{n}"')
        lines.append("")
    lines.append(TYPEDEFS)
    lines.append(FOOTER)

    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    with open(a.out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines))
    n_macros = sum(len(h) for _, _, _, h in sections)
    print(f"{a.out}: {n_macros} macros, {os.path.getsize(a.out)} bytes")
    dupes = {k: v for k, v in used.items() if v > 1}
    if dupes:
        print(f"overload groups: {len(dupes)} (suffixed _2.._n)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
