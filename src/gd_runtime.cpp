#include "gd_runtime.h"

#include "gd_exports.h"
#include "gd_exports_extra.h"
#include "ut_log.h"

namespace ut {

GdRuntime g_gd;

namespace {

// Resolve one export, log the result, and count a miss when the symbol is required.
void* resolveOne(HMODULE mod, const char* dllName, const char* mangled, const char* pretty,
                 bool required) {
    void* p = mod ? (void*)GetProcAddress(mod, mangled) : nullptr;
    if (p) {
        // Counted so the bindings gate can say how many of the mod's facts are name-resolved -
        // the class that survives any patch which keeps the names, i.e. every minor one.
        ++g_gd.resolvedByName;
        logT("  %-14s %-38s -> %p", dllName, pretty, p);
    } else if (required) {
        logE("  %-14s %-38s -> MISSING (REQUIRED)", dllName, pretty);
        ++g_gd.missingRequired;
    } else {
        logD("  %-14s %-38s -> MISSING", dllName, pretty);
    }
    return p;
}

}  // namespace

bool waitForGameModules(DWORD timeoutMs) {
    const DWORD start = GetTickCount();
    for (;;) {
        HMODULE e = GetModuleHandleA("Engine.dll");
        HMODULE g = GetModuleHandleA("Game.dll");
        if (e && g) {
            g_gd.engineDll = e;
            g_gd.gameDll = g;
            logD("modules ready after %lu ms: Engine.dll=%p Game.dll=%p",
                 GetTickCount() - start, (void*)e, (void*)g);
            return true;
        }
        if (GetTickCount() - start > timeoutMs) {
            logE("GIVING UP after %lu ms: Engine.dll=%p Game.dll=%p (one or both never loaded)",
                 GetTickCount() - start, (void*)e, (void*)g);
            return false;
        }
        Sleep(50);
    }
}

bool resolveExports() {
    HMODULE e = g_gd.engineDll;
    HMODULE g = g_gd.gameDll;
    g_gd.missingRequired = 0;

    logT("resolving exports by name (GetProcAddress):");

    // Engine.dll -- gEngine is a DATA export: the address of the GAME::Engine* variable.
    g_gd.ppEngine = (GdEngine**)resolveOne(e, "Engine.dll", GD_ENGINE_GENGINE, "GAME::gEngine (data)", true);
    g_gd.EngineGetGraphicsEngine = (PfnEngine_GetGraphicsEngine)resolveOne(
        e, "Engine.dll", GD_ENGINE_GETGRAPHICSENGINE, "Engine::GetGraphicsEngine", true);
    g_gd.EnginePresentSurface = (PfnEngine_PresentSurface)resolveOne(
        e, "Engine.dll", GD_ENGINE_PRESENTSURFACE, "Engine::PresentSurface", true);
    g_gd.EngineGetFrameCount = (PfnEngine_GetFrameCount)resolveOne(
        e, "Engine.dll", GD_ENGINE_GETFRAMECOUNT, "Engine::GetFrameCount", true);
    g_gd.GfxGetCanvas = (PfnGfx_GetCanvas)resolveOne(
        e, "Engine.dll", GD_GFXENGINE_GETCANVAS, "GraphicsEngine::GetCanvas", true);

    // Engine.dll -- drawing set.
    g_gd.GfxLoadTexture = (PfnGfx_LoadTexture)resolveOne(
        e, "Engine.dll", GD_GFXENGINE_LOADTEXTURE, "GraphicsEngine::LoadTexture", true);
    g_gd.GfxLoadFont = (PfnGfx_LoadFont)resolveOne(
        e, "Engine.dll", GD_GFXENGINE_LOADFONT, "GraphicsEngine::LoadFont", true);
    g_gd.GfxGetUIScaleFactor = (PfnGfx_GetUIScaleFactor)resolveOne(
        e, "Engine.dll", GD_GFXENGINE_GETUISCALEFACTOR, "GraphicsEngine::GetUIScaleFactor",
        false);
    g_gd.TextureGetTexture = (PfnTexture_GetTexture)resolveOne(
        e, "Engine.dll", GD_TEXTURE_GETTEXTURE_2, "GraphicsTexture::GetTexture", true);
    g_gd.TextureGetTextureIdx = (PfnTexture_GetTextureIdx)resolveOne(
        e, "Engine.dll", GD_TEXTURE_GETTEXTURE, "GraphicsTexture::GetTexture(int)", false);
    g_gd.TextureGetWidth = (PfnTexture_GetWidth)resolveOne(
        e, "Engine.dll", GD_TEXTURE_GETWIDTH, "GraphicsTexture::GetWidth", false);
    g_gd.TextureGetHeight = (PfnTexture_GetHeight)resolveOne(
        e, "Engine.dll", GD_TEXTURE_GETHEIGHT, "GraphicsTexture::GetHeight", false);
    g_gd.TextureGetIsReadyToUse = (PfnTexture_GetIsReadyToUse)resolveOne(
        e, "Engine.dll", GD_TEXTURE_GETISREADYTOUSE, "GraphicsTexture::GetIsReadyToUse", false);
    g_gd.CanvasGetWidth = (PfnCanvas_GetWidth)resolveOne(
        e, "Engine.dll", GD_CANVAS_GETWIDTH, "GraphicsCanvas::GetWidth", true);
    g_gd.CanvasGetHeight = (PfnCanvas_GetHeight)resolveOne(
        e, "Engine.dll", GD_CANVAS_GETHEIGHT, "GraphicsCanvas::GetHeight", true);
    g_gd.CanvasRenderRectSolid = (PfnCanvas_RenderRectSolid)resolveOne(
        e, "Engine.dll", GD_CANVAS_RENDERRECT_2, "GraphicsCanvas::RenderRect(solid)", true);
    g_gd.CanvasRenderRectTex = (PfnCanvas_RenderRectTex)resolveOne(
        e, "Engine.dll", GD_CANVAS_RENDERRECT, "GraphicsCanvas::RenderRect(tex)", true);
    g_gd.CanvasRenderText2d = (PfnCanvas_RenderText2d)resolveOne(
        e, "Engine.dll", GD_CANVAS_RENDERTEXT2D_3, "GraphicsCanvas::RenderText2d", true);
    g_gd.CanvasGetViewport = (PfnCanvas_GetViewport)resolveOne(
        e, "Engine.dll", GD_CANVAS_GETVIEWPORT, "GraphicsCanvas::GetViewport", false);

    // Engine.dll -- the primitive drawer (icon_mode=1 only; never touched otherwise).
    g_gd.DrawerCtor = (PfnDrawer_Ctor)resolveOne(
        e, "Engine.dll", GD_DRAW_0GRAPHICSPRIMITIVEDRAWER, "GraphicsPrimitiveDrawer::ctor", false);
    g_gd.DrawerEnable2DMode = (PfnDrawer_Enable2DMode)resolveOne(
        e, "Engine.dll", GD_DRAW_ENABLE2DMODE, "GraphicsPrimitiveDrawer::Enable2DMode", false);
    g_gd.DrawerSetTexture0 = (PfnDrawer_SetTexture0)resolveOne(
        e, "Engine.dll", GD_DRAW_SETTEXTURE0, "GraphicsPrimitiveDrawer::SetTexture0", false);
    g_gd.DrawerSetColor = (PfnDrawer_SetColor)resolveOne(
        e, "Engine.dll", GD_DRAW_SETCOLOR, "GraphicsPrimitiveDrawer::SetColor", false);
    g_gd.DrawerEnd = (PfnDrawer_Void)resolveOne(
        e, "Engine.dll", GD_DRAW_END, "GraphicsPrimitiveDrawer::End", false);
    g_gd.DrawerFlush = (PfnDrawer_Void)resolveOne(
        e, "Engine.dll", GD_DRAW_FLUSH, "GraphicsPrimitiveDrawer::Flush", false);
    g_gd.Draw2DRectangle = (PfnDraw2DRectangle)resolveOne(
        e, "Engine.dll", GD_DRAW_DRAW2DRECTANGLE, "GAME::Draw2DRectangle(dest,uv)", false);

    // Engine.dll -- GAME::Object accessors (Item derives from Object) and the window proc.
    g_gd.ObjectGetObjectId = (PfnObject_GetObjectId)resolveOne(
        e, "Engine.dll", GD_OBJ_GETOBJECTID, "Object::GetObjectId", true);
    g_gd.ObjectGetObjectName = (PfnObject_GetObjectName)resolveOne(
        e, "Engine.dll", GD_OBJ_GETOBJECTNAME, "Object::GetObjectName", false);
    g_gd.WindowProc = (PfnWindow_WindowProc)resolveOne(
        e, "Engine.dll", GD_WINDOW_WINDOWPROC, "WinWindow::WindowProc", false);

    // Game.dll -- GameEngine
    g_gd.GameSetTransferOpen = (PfnGameEngine_SetTransferOpen)resolveOne(
        g, "Game.dll", GD_GAMEENGINE_SETTRANSFEROPEN, "GameEngine::SetTransferOpen", true);
    g_gd.GameUpdate = (PfnGameEngine_Update)resolveOne(
        g, "Game.dll", GD_GAMEENGINE_UPDATE, "GameEngine::Update", true);
    g_gd.GameIsTransferOpen = (PfnGameEngine_IsTransferOpen)resolveOne(
        g, "Game.dll", GD_GAMEENGINE_ISTRANSFEROPEN, "GameEngine::IsTransferOpen", true);
    g_gd.GameGetPlayerTransfer = (PfnGameEngine_GetPlayerTransfer)resolveOne(
        g, "Game.dll", GD_GAMEENGINE_GETPLAYERTRANSFER, "GameEngine::GetPlayerTransfer", true);
    g_gd.GameGetInventoryCellSize = (PfnGameEngine_GetInventoryCellSize)resolveOne(
        g, "Game.dll", GD_GAMEENGINE_GETINVENTORYCELLSIZE, "GameEngine::GetInventoryCellSize", false);
    g_gd.GameGetItemBackground = (PfnGameEngine_GetItemBackground)resolveOne(
        g, "Game.dll", GD_GAMEENGINE_GETITEMBACKGROUND, "GameEngine::GetItemBackground", false);
    g_gd.GameDisplayCaravanWindow = (PfnGameEngine_DisplayCaravanWindow)resolveOne(
        g, "Game.dll", GD_GAMEENGINE_DISPLAYCARAVANWINDOW, "GameEngine::DisplayCaravanWindow", false);
    g_gd.GameGetMainPlayer = (PfnGameEngine_GetMainPlayer)resolveOne(
        g, "Game.dll", GD_GAMEENGINE_GETMAINPLAYER, "GameEngine::GetMainPlayer", false);
    g_gd.GameSetSelectedTransferSack = (PfnGameEngine_SetSelectedTransferSackNumber)resolveOne(
        g, "Game.dll", GD_GAMEENGINE_SETSELECTEDTRANSFERSACKNUMBER,
        "GameEngine::SetSelectedTransferSackNumber", false);
    g_gd.GameGetSelectedTransferSack = (PfnGameEngine_GetSelectedTransferSackNumber)resolveOne(
        g, "Game.dll", GD_GAMEENGINE_GETSELECTEDTRANSFERSACKNUMBER,
        "GameEngine::GetSelectedTransferSackNumber", false);

    // Static const data exports: how many transfer tabs the column shows per DLC.
    g_gd.MaxTransferSacks = (const unsigned int*)resolveOne(
        g, "Game.dll", GD_GAMEENGINE_MAXTRANSFERSACKS, "GameEngine::MaxTransferSacks", false);
    g_gd.Exp1MaxTransferSacks = (const unsigned int*)resolveOne(
        g, "Game.dll", GD_GAMEENGINE_EXPANSION1MAXTRANSFERSACKS, "GameEngine::Exp1MaxTransferSacks", false);
    g_gd.Exp2MaxTransferSacks = (const unsigned int*)resolveOne(
        g, "Game.dll", GD_GAMEENGINE_EXPANSION2MAXTRANSFERSACKS, "GameEngine::Exp2MaxTransferSacks", false);
    g_gd.Exp3MaxTransferSacks = (const unsigned int*)resolveOne(
        g, "Game.dll", GD_GAMEENGINE_EXPANSION3MAXTRANSFERSACKS, "GameEngine::Exp3MaxTransferSacks", false);
    logD("  transfer tab counts: base=%u x1=%u x2=%u x3=%u",
         g_gd.MaxTransferSacks ? *g_gd.MaxTransferSacks : 0u,
         g_gd.Exp1MaxTransferSacks ? *g_gd.Exp1MaxTransferSacks : 0u,
         g_gd.Exp2MaxTransferSacks ? *g_gd.Exp2MaxTransferSacks : 0u,
         g_gd.Exp3MaxTransferSacks ? *g_gd.Exp3MaxTransferSacks : 0u);

    // Game.dll -- Item accessors (read-only) and the three sack entry points we OBSERVE.
    g_gd.ItemGetItemReplicaInfo = (PfnItem_GetItemReplicaInfo)resolveOne(
        g, "Game.dll", GD_ITEM_GETITEMREPLICAINFO, "Item::GetItemReplicaInfo", false);
    g_gd.ItemGetItemClassification = (PfnItem_GetItemClassification)resolveOne(
        g, "Game.dll", GD_ITEM_GETITEMCLASSIFICATION, "Item::GetItemClassification", false);
    g_gd.ItemGetBitmap = (PfnItem_GetBitmap)resolveOne(
        g, "Game.dll", GD_ITEM_GETBITMAP, "Item::GetBitmap", false);
    g_gd.SackAddItem = (PfnSack_AddItem)resolveOne(
        g, "Game.dll", GD_SACK_ADDITEM_2, "InventorySack::AddItem(Item*)", true);
    g_gd.SackAddItemVec = (PfnSack_AddItemVec)resolveOne(
        g, "Game.dll", GD_SACK_ADDITEM, "InventorySack::AddItem(Vec2)", true);
    g_gd.SackRemoveItem = (PfnSack_RemoveItem)resolveOne(
        g, "Game.dll", GD_SACK_REMOVEITEM, "InventorySack::RemoveItem", true);

    // Game.dll / Engine.dll -- exact grid geometry, interaction blocking, draw capture.
    g_gd.SackGridToPixels = (PfnSack_GridToPixels)resolveOne(
        g, "Game.dll", GD_SACK_GRIDTOPIXELS, "InventorySack::GridToPixels", false);
    g_gd.SackPixelsToGrid = (PfnSack_GridToPixels)resolveOne(
        g, "Game.dll", GD_SACK_PIXELSTOGRID, "InventorySack::PixelsToGrid", false);
    g_gd.SackGetGridWidth = (PfnSack_GetU)resolveOne(
        g, "Game.dll", GD_SACK_GETGRIDWIDTH, "InventorySack::GetGridWidth", false);
    g_gd.SackGetGridHeight = (PfnSack_GetU)resolveOne(
        g, "Game.dll", GD_SACK_GETGRIDHEIGHT, "InventorySack::GetGridHeight", false);
    g_gd.SackGetCellWidth = (PfnSack_GetU)resolveOne(
        g, "Game.dll", GD_SACK_GETCELLWIDTH, "InventorySack::GetCellWidth", false);
    g_gd.SackGetCellHeight = (PfnSack_GetU)resolveOne(
        g, "Game.dll", GD_SACK_GETCELLHEIGHT, "InventorySack::GetCellHeight", false);
    g_gd.SackGetItemUnderPoint = (PfnSack_GetItemUnderPoint)resolveOne(
        g, "Game.dll", GD_SACK_GETITEMUNDERPOINT, "InventorySack::GetItemUnderPoint", false);
    g_gd.SackGetRectUnderPoint = (PfnSack_GetRectUnderPoint)resolveOne(
        g, "Game.dll", GD_SACK_GETRECTUNDERPOINT, "InventorySack::GetRectUnderPoint", false);
    g_gd.GameGetTransferSack = (PfnGameEngine_GetTransferSack)resolveOne(
        g, "Game.dll", GD_GAMEENGINE_GETTRANSFERSACK, "GameEngine::GetTransferSack", false);
    g_gd.GameAddItemToTransferVec = (PfnGameEngine_AddItemToTransferVec)resolveOne(
        g, "Game.dll", GD_GAMEENGINE_ADDITEMTOTRANSFER_VEC, "GameEngine::AddItemToTransfer(Vec2)",
        false);
    g_gd.GameAddItemToTransferId = (PfnGameEngine_AddItemToTransferId)resolveOne(
        g, "Game.dll", GD_GAMEENGINE_ADDITEMTOTRANSFER_ID, "GameEngine::AddItemToTransfer(id)",
        false);
    g_gd.CanvasRenderStyledRectTex = (PfnCanvas_RenderStyledRectTex)resolveOne(
        e, "Engine.dll", GD_CANVAS_RENDERSTYLEDRECT, "GraphicsCanvas::RenderStyledRect(tex)",
        false);
    g_gd.CanvasRenderStyledRectFlat = (PfnCanvas_RenderStyledRectFlat)resolveOne(
        e, "Engine.dll", GD_CANVAS_RENDERSTYLEDRECT_2, "GraphicsCanvas::RenderStyledRect(flat)",
        false);
    g_gd.CanvasRenderShadedRect = (PfnCanvas_RenderShadedRect)resolveOne(
        e, "Engine.dll", GD_CANVAS_RENDERSHADEDRECT, "GraphicsCanvas::RenderShadedRect", false);
    g_gd.Draw2DOrientedQuad = (PfnDraw2DOrientedQuad)resolveOne(
        e, "Engine.dll", GD_DRAW_DRAW2DORIENTEDQUAD, "GAME::Draw2DOrientedQuad", false);
    g_gd.CanvasGetClippingRect = (PfnCanvas_GetClippingRect)resolveOne(
        e, "Engine.dll", GD_CANVAS_GETCLIPPINGRECT, "GraphicsCanvas::GetClippingRect", false);

    if (g_gd.missingRequired == 0) {
        logD("all required exports resolved");
        return true;
    }
    logE("ERROR: %d required export(s) MISSING - hooks will not be installed",
         g_gd.missingRequired);
    return false;
}

}  // namespace ut
