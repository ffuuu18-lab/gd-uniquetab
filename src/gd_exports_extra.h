// gd_exports_extra.h - hand-selected additions to the generated src/gd_exports.h.
//
// gd_exports.h is produced by tools/gen_exports_header.py and must not be hand-edited.
// The names below are needed by the collection view and were not in that generated set; each one was copied
// VERBATIM (by script, never typed) out of
// tools/exports/Engine-x64-exports.txt for Grim Dawn 1.3.0.8 x64.
//
//   public: bool __cdecl GAME::GraphicsTexture::GetIsReadyToUse(void)const __ptr64
//   -> true once the pixels are uploaded and the texture can actually be drawn. At the main
//      menu item icons load (right dimensions, non-null RenderTexture) but draw nothing, so
//      every icon is gated on this each frame.

#pragma once

#define GD_TEXTURE_GETISREADYTOUSE "?GetIsReadyToUse@GraphicsTexture@GAME@@QEBA_NXZ"

// --- more names (copied verbatim by script from tools/exports/Game-x64-exports.txt) ---
//
//   public: bool __cdecl GAME::GameEngine::AddItemToTransfer(unsigned int,class GAME::Vec2 const &,unsigned int,bool)
//   public: bool __cdecl GAME::GameEngine::AddItemToTransfer(unsigned int,unsigned int,bool)
//   ** WRITE ** - never called by the mod. Detoured ONLY so that a drop onto the mod panel
//   can be refused (return false, the item stays on the cursor).

#define GD_GAMEENGINE_ADDITEMTOTRANSFER_VEC "?AddItemToTransfer@GameEngine@GAME@@QEAA_NIAEBVVec2@2@I_N@Z"
#define GD_GAMEENGINE_ADDITEMTOTRANSFER_ID "?AddItemToTransfer@GameEngine@GAME@@QEAA_NII_N@Z"

// --- the UI scale factor (copied verbatim by script from tools/exports/Engine-x64-exports.txt) ---
//
//   public: float __cdecl GAME::GraphicsEngine::GetUIScaleFactor(void)const __ptr64
//   Engine.dll rva 0x91FF0, exactly one export at that RVA (no stub folding), and the very
//   function UIReagentItem::Load calls at exe 0x1F06A0 to place every item box and
//   ReagentWindow::Load at 0x132167 to write window+0x40/+0x44 = WindowLocationX/Y * s.
//   Range [0.700, 2.000]. gen_exports_header.py does not select it, so it lives here.

#define GD_GFXENGINE_GETUISCALEFACTOR "?GetUIScaleFactor@GraphicsEngine@GAME@@QEBAMXZ"
