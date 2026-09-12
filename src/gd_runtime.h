// gd_runtime.h - the engine functions this mod resolves by name, and the raw structs it passes.
//
// Every mangled name comes from src/gd_exports.h (generated from the real export tables) or
// src/gd_exports_extra.h (the same, copied by script for a few more names);
// nothing here is typed by hand. Calling convention notes are in gd_exports.h: on x64 a
// non-static member function takes the object pointer as the first argument and the Microsoft
// x64 ABI is identical for __cdecl/__thiscall/__stdcall, so plain function pointers work.
#pragma once

#include <windows.h>

// Opaque engine handles - never dereferenced except where a comment says so.
struct GdEngine;
struct GdGraphicsEngine;
struct GdCanvas;
struct GdTexture;        // GAME::GraphicsTexture
struct GdRenderTexture;  // GAME::RenderTexture
struct GdFont;           // GAME::GraphicsFont2
struct GdGameEngine;
struct GdPlayer;         // GAME::Player
struct GdItem;           // GAME::Item  (derives from GAME::Object)
struct GdSack;           // GAME::InventorySack

// GAME::Rect and GAME::Color, both verified in game:
// Rect = {x, y, w, h}; Color = {r, g, b, a} in 0..1 through the engine's tone curve.
struct GdRect {
    float a, b, c, d;
};
struct GdColor {
    float r, g, b, a;
};

// ---- function pointer types -----------------------------------------------------------------
typedef GdGraphicsEngine*(__cdecl* PfnEngine_GetGraphicsEngine)(GdEngine*);
typedef void(__cdecl* PfnEngine_PresentSurface)(GdEngine*);
typedef unsigned int(__cdecl* PfnEngine_GetFrameCount)(GdEngine*);

typedef GdCanvas*(__cdecl* PfnGfx_GetCanvas)(GdGraphicsEngine*);              // returns a reference
typedef const GdTexture*(__cdecl* PfnGfx_LoadTexture)(GdGraphicsEngine*, const void* stdString);
typedef const GdFont*(__cdecl* PfnGfx_LoadFont)(GdGraphicsEngine*, const void* stdString);
// GraphicsEngine::GetUIScaleFactor - OPTIONAL. The mod refuses the category pad
// when it is missing and falls back to the filler-box measurement, it never guesses 1.0.
typedef float(__cdecl* PfnGfx_GetUIScaleFactor)(const GdGraphicsEngine*);

typedef const GdRenderTexture*(__cdecl* PfnTexture_GetTexture)(const GdTexture*);
typedef const GdRenderTexture*(__cdecl* PfnTexture_GetTextureIdx)(const GdTexture*, int);
typedef int(__cdecl* PfnTexture_GetWidth)(const GdTexture*);
typedef int(__cdecl* PfnTexture_GetHeight)(const GdTexture*);
typedef bool(__cdecl* PfnTexture_GetIsReadyToUse)(const GdTexture*);

typedef int(__cdecl* PfnCanvas_GetWidth)(const GdCanvas*);
typedef int(__cdecl* PfnCanvas_GetHeight)(const GdCanvas*);
typedef void(__cdecl* PfnCanvas_RenderRectSolid)(GdCanvas*, const GdRect*, const GdColor*);
typedef void(__cdecl* PfnCanvas_RenderRectTex)(GdCanvas*, const GdRect* dst, const GdRect* uv,
                                               const GdRenderTexture*, const GdColor*);
// RenderText2d(int x, int y, Color const&, char const*, GraphicsFont2 const*, int size,
//              GraphicsXAlign, GraphicsYAlign, FontStyleFlag, FontLayout)
typedef void(__cdecl* PfnCanvas_RenderText2d)(GdCanvas*, int, int, const GdColor*, const char*,
                                              const GdFont*, int, int, int, int, int);

// GAME::Viewport - opaque; GraphicsCanvas::GetViewport() hands one back and
// GraphicsPrimitiveDrawer::Enable2DMode takes it straight back, so its layout stays unknown.
struct GdViewport;

typedef const GdViewport*(__cdecl* PfnCanvas_GetViewport)(const GdCanvas*);

// GAME::GraphicsPrimitiveDrawer - constructed in place in a zeroed scratch buffer that is far
// larger than the object can plausibly be, and never destroyed (one static instance).
typedef void*(__cdecl* PfnDrawer_Ctor)(void* self, GdCanvas*);
typedef void(__cdecl* PfnDrawer_Enable2DMode)(void* self, const GdViewport*, float);
typedef void(__cdecl* PfnDrawer_SetTexture0)(void* self, const GdRenderTexture*);
typedef void(__cdecl* PfnDrawer_SetColor)(void* self, const GdColor*);
typedef void(__cdecl* PfnDrawer_Void)(void* self);  // End / Flush
typedef void(__cdecl* PfnDraw2DRectangle)(void* drawer, const GdRect* dest, const GdRect* uv);

typedef void(__cdecl* PfnGameEngine_SetTransferOpen)(GdGameEngine*, bool);
typedef void(__cdecl* PfnGameEngine_SetSelectedTransferSackNumber)(GdGameEngine*, unsigned int);
typedef unsigned int(__cdecl* PfnGameEngine_GetSelectedTransferSackNumber)(const GdGameEngine*);
typedef bool(__cdecl* PfnGameEngine_IsTransferOpen)(const GdGameEngine*);
typedef void(__cdecl* PfnGameEngine_Update)(GdGameEngine*, int);
typedef void*(__cdecl* PfnGameEngine_GetPlayerTransfer)(GdGameEngine*);  // mem::vector<InventorySack*>&
typedef void(__cdecl* PfnGameEngine_GetInventoryCellSize)(GdGameEngine*, float*, float*);
typedef const GdTexture*(__cdecl* PfnGameEngine_GetItemBackground)(const GdGameEngine*, int);
typedef bool(__cdecl* PfnGameEngine_DisplayCaravanWindow)(GdGameEngine*, unsigned int);
typedef GdPlayer*(__cdecl* PfnGameEngine_GetMainPlayer)(const GdGameEngine*);

// GAME::Object accessors. Item derives from Object, so an Item* is a valid `this` here.
typedef unsigned int(__cdecl* PfnObject_GetObjectId)(const void*);
typedef const char*(__cdecl* PfnObject_GetObjectName)(const void*);

// GAME::Item. GetItemClassification returns a scalar enum (EAX, no sret).
typedef void(__cdecl* PfnItem_GetItemReplicaInfo)(const GdItem*, void* replicaInfo);
typedef int(__cdecl* PfnItem_GetItemClassification)(const GdItem*, bool);
typedef const GdTexture*(__cdecl* PfnItem_GetBitmap)(const GdItem*);

// GAME::InventorySack. OBSERVED ONLY - the detours call the original and never these pointers.
typedef bool(__cdecl* PfnSack_AddItem)(GdSack*, GdItem*, bool, bool);
typedef bool(__cdecl* PfnSack_AddItemVec)(GdSack*, const void* vec2, GdItem*, bool);
typedef bool(__cdecl* PfnSack_RemoveItem)(GdSack*, unsigned int);

// GAME::WinWindow::WindowProc - a private *static* member, so no `this`.
typedef LRESULT(__cdecl* PfnWindow_WindowProc)(HWND, UINT, WPARAM, LPARAM);

// ---- inventory grid geometry, interaction blocking, draw capture ----------------------------
// GAME::Vec2 is two floats. A by-value 8-byte class return is either RAX-packed or sret,
// depending on how MSVC classified the type; a caller must call GridToPixels through the
// SRET-shaped prototype and detect at run time which one actually happened, so only this one
// shape is declared.
typedef void*(__cdecl* PfnSack_GridToPixels)(const GdSack*, void* sret, const void* vec2In);
typedef unsigned int(__cdecl* PfnSack_GetU)(const GdSack*);
// GetItemUnderPoint(Vec2 by value): the Vec2 arrives in RDX either packed as two floats or as
// a pointer - the detour never decodes it, it only blocks.
typedef unsigned int(__cdecl* PfnSack_GetItemUnderPoint)(const GdSack*, unsigned long long);
typedef void*(__cdecl* PfnSack_GetRectUnderPoint)(const GdSack*, GdRect* sret, unsigned long long);
typedef GdSack*(__cdecl* PfnGameEngine_GetTransferSack)(GdGameEngine*, int);
typedef bool(__cdecl* PfnGameEngine_AddItemToTransferVec)(GdGameEngine*, unsigned int,
                                                          const void* vec2, unsigned int, bool);
typedef bool(__cdecl* PfnGameEngine_AddItemToTransferId)(GdGameEngine*, unsigned int, unsigned int,
                                                         bool);
// GAME::Name const& and GAME::GraphicsShader2 const* are opaque pointers here.
typedef void(__cdecl* PfnCanvas_RenderStyledRectTex)(GdCanvas*, const GdRect* dst, const GdRect* uv,
                                                     const GdRenderTexture*, const void* name,
                                                     const GdColor*, float);
typedef void(__cdecl* PfnCanvas_RenderStyledRectFlat)(GdCanvas*, const GdRect* dst,
                                                      const void* name, const GdColor*);
typedef void(__cdecl* PfnCanvas_RenderShadedRect)(GdCanvas*, const GdRect* dst, const GdRect* uv,
                                                  const GdRenderTexture*, const void* shader,
                                                  const void* name, const GdColor*, float);
typedef void(__cdecl* PfnDraw2DOrientedQuad)(void* drawer, const void* a, const void* b,
                                             const void* c);
typedef const GdRect*(__cdecl* PfnCanvas_GetClippingRect)(GdCanvas*);

// ---- the resolved set ------------------------------------------------------------------------
struct GdRuntime {
    HMODULE engineDll = nullptr;
    HMODULE gameDll = nullptr;

    GdEngine** ppEngine = nullptr;  // address of the exported GAME::gEngine variable

    PfnEngine_GetGraphicsEngine EngineGetGraphicsEngine = nullptr;
    PfnEngine_PresentSurface EnginePresentSurface = nullptr;
    PfnEngine_GetFrameCount EngineGetFrameCount = nullptr;

    PfnGfx_GetCanvas GfxGetCanvas = nullptr;
    PfnGfx_LoadTexture GfxLoadTexture = nullptr;
    PfnGfx_LoadFont GfxLoadFont = nullptr;
    PfnGfx_GetUIScaleFactor GfxGetUIScaleFactor = nullptr;   // optional

    PfnTexture_GetTexture TextureGetTexture = nullptr;
    PfnTexture_GetTextureIdx TextureGetTextureIdx = nullptr;
    PfnTexture_GetWidth TextureGetWidth = nullptr;
    PfnTexture_GetHeight TextureGetHeight = nullptr;
    PfnTexture_GetIsReadyToUse TextureGetIsReadyToUse = nullptr;

    PfnCanvas_GetWidth CanvasGetWidth = nullptr;
    PfnCanvas_GetHeight CanvasGetHeight = nullptr;
    PfnCanvas_RenderRectSolid CanvasRenderRectSolid = nullptr;
    PfnCanvas_RenderRectTex CanvasRenderRectTex = nullptr;
    PfnCanvas_RenderText2d CanvasRenderText2d = nullptr;
    PfnCanvas_GetViewport CanvasGetViewport = nullptr;

    PfnDrawer_Ctor DrawerCtor = nullptr;
    PfnDrawer_Enable2DMode DrawerEnable2DMode = nullptr;
    PfnDrawer_SetTexture0 DrawerSetTexture0 = nullptr;
    PfnDrawer_SetColor DrawerSetColor = nullptr;
    PfnDrawer_Void DrawerEnd = nullptr;
    PfnDrawer_Void DrawerFlush = nullptr;
    PfnDraw2DRectangle Draw2DRectangle = nullptr;

    PfnGameEngine_SetTransferOpen GameSetTransferOpen = nullptr;
    PfnGameEngine_SetSelectedTransferSackNumber GameSetSelectedTransferSack = nullptr;  // hook
    PfnGameEngine_GetSelectedTransferSackNumber GameGetSelectedTransferSack = nullptr;
    PfnGameEngine_IsTransferOpen GameIsTransferOpen = nullptr;
    PfnGameEngine_Update GameUpdate = nullptr;
    PfnGameEngine_GetPlayerTransfer GameGetPlayerTransfer = nullptr;
    PfnGameEngine_GetInventoryCellSize GameGetInventoryCellSize = nullptr;
    PfnGameEngine_GetItemBackground GameGetItemBackground = nullptr;
    PfnGameEngine_DisplayCaravanWindow GameDisplayCaravanWindow = nullptr;
    PfnGameEngine_GetMainPlayer GameGetMainPlayer = nullptr;

    PfnObject_GetObjectId ObjectGetObjectId = nullptr;
    PfnObject_GetObjectName ObjectGetObjectName = nullptr;
    PfnItem_GetItemReplicaInfo ItemGetItemReplicaInfo = nullptr;
    PfnItem_GetItemClassification ItemGetItemClassification = nullptr;
    PfnItem_GetBitmap ItemGetBitmap = nullptr;

    // Static const unsigned int data exports: the tab count the column shows per DLC.
    const unsigned int* MaxTransferSacks = nullptr;      // 4
    const unsigned int* Exp1MaxTransferSacks = nullptr;  // 5
    const unsigned int* Exp2MaxTransferSacks = nullptr;  // 6
    const unsigned int* Exp3MaxTransferSacks = nullptr;  // 10

    PfnSack_AddItem SackAddItem = nullptr;         // hook target only
    PfnSack_AddItemVec SackAddItemVec = nullptr;   // hook target only
    PfnSack_RemoveItem SackRemoveItem = nullptr;   // hook target only
    PfnWindow_WindowProc WindowProc = nullptr;     // hook target only

    // ---- inventory grid geometry, interaction blocking, draw capture --------------------
    PfnSack_GridToPixels SackGridToPixels = nullptr;
    PfnSack_GridToPixels SackPixelsToGrid = nullptr;
    PfnSack_GetU SackGetGridWidth = nullptr;
    PfnSack_GetU SackGetGridHeight = nullptr;
    PfnSack_GetU SackGetCellWidth = nullptr;
    PfnSack_GetU SackGetCellHeight = nullptr;
    PfnSack_GetItemUnderPoint SackGetItemUnderPoint = nullptr;   // hook target
    PfnSack_GetRectUnderPoint SackGetRectUnderPoint = nullptr;   // hook target
    PfnGameEngine_GetTransferSack GameGetTransferSack = nullptr;
    PfnGameEngine_AddItemToTransferVec GameAddItemToTransferVec = nullptr;  // hook target
    PfnGameEngine_AddItemToTransferId GameAddItemToTransferId = nullptr;    // hook target
    PfnCanvas_RenderStyledRectTex CanvasRenderStyledRectTex = nullptr;      // hook target
    PfnCanvas_RenderStyledRectFlat CanvasRenderStyledRectFlat = nullptr;    // hook target
    PfnCanvas_RenderShadedRect CanvasRenderShadedRect = nullptr;            // hook target
    PfnDraw2DOrientedQuad Draw2DOrientedQuad = nullptr;                     // hook target
    PfnCanvas_GetClippingRect CanvasGetClippingRect = nullptr;

    // Count of required symbols that came back MISSING.
    int missingRequired = 0;
    // Count of symbols that DID resolve by name, required or not - the "by export" number in the
    // bindings gate's one-line summary.
    int resolvedByName = 0;
};

namespace ut {

extern GdRuntime g_gd;

// Blocks until GetModuleHandle finds Engine.dll and Game.dll (poll every 50 ms), then returns.
// false = the modules never appeared within timeoutMs.
bool waitForGameModules(DWORD timeoutMs);

// GetProcAddress for everything in GdRuntime; logs address or MISSING per symbol.
bool resolveExports();

}  // namespace ut
