"""Generate src/gd_exports_reagent.h: the reagent-page mangled names, copied VERBATIM.

Every name below is looked up in tools/exports/{Game,Engine}-x64-exports.txt by its exact
mangled spelling and written out only if it is present, so no mangled name is ever typed by
hand into a source file. Run after any game update; a missing name is a hard error.

    python gen_reagent_header.py [--exports <dir>] [--out <file>]
"""
from __future__ import annotations

import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEFAULT_EXPORTS = os.path.join(HERE, "exports")
DEFAULT_OUT = os.path.join(ROOT, "src", "gd_exports_reagent.h")

# macro name -> (dll, exact mangled export, one-line purpose)
WANTED = [
    ("GD_ITEM_ISREAGENTCOMPATIBLE", "Game",
     "?IsReagentCompatible@Item@GAME@@QEBA_NXZ",
     "READ ONLY - not hooked; its 7 code bytes carry the craftingMaterial field offset"),
    ("GD_ITEM_LOAD", "Game",
     "?Load@Item@GAME@@UEAAXAEBVLoadTable@2@@Z",
     "post-detour: the point where the engine writes Item+off from the record"),
    ("GD_GAMEENGINE_ADDITEMTOREAGENTS", "Game",
     "?AddItemToReagents@GameEngine@GAME@@QEAA_NI@Z",
     "log only: the call the reagent box makes for an explicit drop"),
    ("GD_GAMEENGINE_TAKEITEMFROMREAGENTS_NAME", "Game",
     "?TakeItemFromReagents@GameEngine@GAME@@QEAAHAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@H@Z",
     "log only: the withdraw path"),
    ("GD_GAMEENGINE_TAKEITEMFROMREAGENTS_ID", "Game",
     "?TakeItemFromReagents@GameEngine@GAME@@QEAAHIH@Z",
     "log only: the id overload of the withdraw path (tail-calls the string one)"),
    ("GD_GAMEENGINE_GETREAGENTITEMCOUNT", "Game",
     "?GetReagentItemCount@GameEngine@GAME@@QEBAHAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@AEAV?$vector@I@mem@@@Z",
     "THE authoritative count for one record - the engine's own map lookup"),
    ("GD_ITEM_GETITEMREPLICAINFO", "Game",
     "?GetItemReplicaInfo@Item@GAME@@UEBAXAEAUItemReplicaInfo@2@@Z",
     "READ ONLY - `lea rdx,[rcx+disp32]` gives the ItemReplicaInfo offset inside Item"),
    ("GD_ITEM_CTOR", "Game",
     "??0Item@GAME@@QEAA@XZ",
     "READ ONLY: the member constructed right after the ItemReplicaInfo pins the block's size"),
    ("GD_ITEM_GETSEEDREROLLS", "Game",
     "?GetSeedRerolls@Item@GAME@@QEBAIXZ",
     "READ ONLY - its 6 code bytes carry the seedRerolls field offset (Ascended state)"),
    ("GD_ITEM_GETAFFIXREROLLS", "Game",
     "?GetAffixRerolls@Item@GAME@@QEBAIXZ",
     "READ ONLY - its 6 code bytes carry the affixRerolls field offset"),
    ("GD_ITEM_GETPREFIXCLASSIFICATION", "Game",
     "?GetPrefixClassification@Item@GAME@@QEBA?AW4ItemClassification@2@XZ",
     "READ ONLY - its 6 code bytes carry the prefix ItemClassification offset"),
    ("GD_ITEM_GETSUFFIXCLASSIFICATION", "Game",
     "?GetSuffixClassification@Item@GAME@@QEBA?AW4ItemClassification@2@XZ",
     "READ ONLY - its 6 code bytes carry the suffix ItemClassification offset"),
    ("GD_ITEM_CREATEITEM", "Game",
     "?CreateItem@Item@GAME@@SAPEAV12@AEBUItemReplicaInfo@2@@Z",
     "THE creation call, and the identity substitution point"),
    ("GD_CONTROLLERCHAR_CREATEITEMININVENTORY", "Game",
     "?CreateItemInInventory@ControllerCharacter@GAME@@UEAAPEAVItem@2@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z",
     "observed: a creation route of the reagent-box take (record -> item)"),
    ("GD_GAMEENGINE_CREATEITEMFORCHARACTER", "Game",
     "?CreateItemForCharacter@GameEngine@GAME@@QEAAXIAEBVWorldCoords@2@AEAUItemReplicaInfo@2@PEAV?$basic_string@GU?$char_traits@G@std@@V?$allocator@G@2@@std@@@Z",
     "observed: the other creation route a take could use"),
    ("GD_CURSORITEMMOVE_CANCEL", "Game",
     "?Cancel@CursorHandlerItemMove@GAME@@UEAA_NXZ",
     "log only: the 4th AddItemToReagents caller (a drop the UI could not place)"),
    ("GD_GAMEENGINE_GETPLAYERREAGENTS", "Game",
     "?GetPlayerReagents@GameEngine@GAME@@QEBAAEBV?$map@V?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@UReagentData@GAME@@@mem@@XZ",
     "`lea rax,[rcx+0x36D80]; ret` - THE reagent map, walked for the node"),
    ("GD_ITEM_GETSTACKSIZE_R", "Game",
     "?GetStackSize@Item@GAME@@UEBAIXZ",
     "`mov eax,[rcx+0x88C]; ret` - the prototype's stack size == the box count"),
    ("GD_ITEM_SETSTACKSIZE", "Game",
     "?SetStackSize@Item@GAME@@UEAAXI@Z",
     "the only stack write the mod makes - `mov [rcx+0x88C],edx; mov [rcx+0x6B0],edx; ret`"),
    ("GD_ITEMEQUIPMENT_INCREMENTSTACK", "Game",
     "?IncrementStack@ItemEquipment@GAME@@UEAA_NIAEAI@Z",
     "READ ONLY: its ADDRESS is the folded `xor al,al; ret` no-op stub (525 exports)"),
    ("GD_ITEM_INCREMENTSTACK", "Game",
     "?IncrementStack@Item@GAME@@UEAA_NIAEAI@Z",
     "READ ONLY: the real base implementation, for the log line only"),
    ("GD_PLAYERINVCTRL_DEPOSITREAGENTS", "Game",
     "?DepositReagents@PlayerInventoryCtrl@GAME@@QEAAXXZ",
     "auto-deposit button: our gate is disarmed while this is on the stack"),
    ("GD_SACK_DEPOSITSACKINTOREAGENTS", "Game",
     "?DepositSackIntoReagents@InventorySack@GAME@@QEAA_NPEAVControllerPlayer@2@@Z",
     "auto-deposit: same"),
    ("GD_GAMEENGINE_DEPOSITTRANSFERREAGENTS", "Game",
     "?DepositTransferReagents@GameEngine@GAME@@QEAAXXZ",
     "auto-deposit: same"),
    ("GD_CURSORITEMMOVE_PRIMARYREAGENTACTIVATE", "Game",
     "?PrimaryReagentActivate@CursorHandlerItemMove@GAME@@UEAA_NXZ",
     "log: the explicit drop of the cursor item on a reagent box"),
    ("GD_CURSORITEMMOVE_QUICKDROPINREAGENTS", "Game",
     "?QuickDropInReagents@CursorHandlerItemMove@GAME@@UEAA_NXZ",
     "log: the shift-click / quick drop path into the reagent page"),
    ("GD_ITEM_ISSOULBOUND", "Game",
     "?IsSoulbound@Item@GAME@@QEBA?B_NXZ",
     "READ ONLY - its 7 code bytes carry the `soulbound` field offset (drop condition 1)"),
    ("GD_ITEM_ISUNTRADEABLE", "Game",
     "?IsUntradeable@Item@GAME@@QEBA?B_NXZ",
     "READ ONLY - its 7 code bytes carry the `untradeable` field offset (drop condition 2)"),
    ("GD_OBJECTMANAGER_DESTROYOBJECTEX", "Engine",
     "?DestroyObjectEx@ObjectManager@GAME@@QEAAXPEAVObject@2@PEBDH@Z",
     "keeps the item registry free of dangling pointers"),
    ("GD_OBJECTMANAGER_ISOBJECTONDELETEDLIST", "Engine",
     "?IsObjectOnDeletedList@ObjectManager@GAME@@QEAA_NPEAVObject@2@@Z",
     "liveness check before EVERY write into an Item"),
    ("GD_OBJECTMANAGER_ISOBJECTIDONDELETEDLIST", "Engine",
     "?IsObjectIdOnDeletedList@ObjectManager@GAME@@QEAA_NI@Z",
     "the id-keyed half of the same liveness check"),
    ("GD_ENGINE_GETGAMEINFO", "Engine",
     "?GetGameInfo@Engine@GAME@@QEAAPEAVGameInfo@2@XZ",
     "the GameInfo the multiplayer flag lives on"),
    ("GD_GAMEINFO_GETISMULTIPLAYER", "Engine",
     "?GetIsMultiPlayer@GameInfo@GAME@@QEBA_NXZ",
     "THE session mode (GameInfo+0x1C0) - a gate only while mp_collect=0"),
    ("GD_GAMEINFO_GETISSERVER", "Engine",
     "?GetIsServer@GameInfo@GAME@@QEBA_NXZ",
     "OPTIONAL telemetry only (GameInfo+0x1C1) - am I the host? Never a gate"),
    ("GD_GAMEINFO_GETNUMOFPLAYERS", "Engine",
     "?GetNumOfPlayers@GameInfo@GAME@@QEBAIXZ",
     "OPTIONAL telemetry only (GameInfo+0x1B8). lobby size vs live roster is unproven - never gate on it"),
    ("GD_GAMEINFO_GETMODE", "Engine",
     "?GetMode@GameInfo@GAME@@QEBAIXZ",
     "OPTIONAL telemetry only (GameInfo+0x1CC) - the save-header game mode"),
    ("GD_GAMEINFO_GETHARDCORE", "Engine",
     "?GetHardcore@GameInfo@GAME@@QEBA_NXZ",
     "THE collection mode - the loaded character's own hardcore flag. A const method "
     "returning bool. GameEngine+0x375BA is NOT this (it read 1 on softcore characters)"),
    ("GD_ENGINE_ISNETWORKENABLED", "Engine",
     "?IsNetworkEnabled@Engine@GAME@@QEAA_NXZ",
     "OPTIONAL telemetry only (Engine+0x6A8). 0 = the stub, i.e. commands execute LOCALLY; 1 = they round-trip. Never a gate"),
    ("GD_ENGINE_ISNETWORKSERVER", "Engine",
     "?IsNetworkServer@Engine@GAME@@QEAA_NXZ",
     "OPTIONAL telemetry only (Engine+0x668 == +0x678). a host round-trips through its own loopback. Never a gate"),
    ("GD_ENGINE_ISNETWORKCLIENT", "Engine",
     "?IsNetworkClient@Engine@GAME@@QEAA_NXZ",
     "OPTIONAL telemetry only (Engine+0x668 == +0x670). Engine state, not lobby state. Never a gate"),
    ("GD_GAMEENGINE_EXITPLAYINGMODE", "Game",
     "?ExitPlayingMode@GameEngine@GAME@@QEAAXXZ",
     "world teardown - the item registry is cleared here"),
    ("GD_ENGINE_LOADMAINDATABASE", "Engine",
     "?LoadMainDatabase@Engine@GAME@@AEAAXXZ",
     "post-detour: the moment every game .arz is loaded and the checksum is already taken"),
    ("GD_ENGINE_LOADDATABASE", "Engine",
     "?LoadDatabase@Engine@GAME@@AEAA_NAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z",
     "called by us with the absolute path of uniq.arz"),
    ("GD_ENGINE_LOADADDITIONALDATABASES", "Engine",
     "?LoadAdditionalDatabases@Engine@GAME@@QEAAXAEBV?$vector@V?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@mem@@@Z",
     "fallback route (takes DIRECTORIES, not files)"),
    ("GD_ENGINE_GETDATABASEARCHIVECHECKSUM", "Engine",
     "?GetDatabaseArchiveChecksum@Engine@GAME@@QEAAIXZ",
     "0 until LoadMainDatabase finishes: the late-load fallback polls it"),
    ("GD_ENGINE_HASLOADEDCUSTOMDATABASE", "Engine",
     "?HasLoadedCustomDatabase@Engine@GAME@@QEBA_NXZ",
     "must stay false: proves we did not go through InitializeMod"),
    ("GD_SINGLETON_OBJECTMANAGER_GET", "Engine",
     "?Get@?$Singleton@VObjectManager@GAME@@@GAME@@SAPEAVObjectManager@2@XZ",
     "verification: the ObjectManager singleton"),
    ("GD_OBJECTMANAGER_LOADTABLEFILE", "Engine",
     "?LoadTableFile@ObjectManager@GAME@@QEAAPEBVLoadTable@2@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z",
     "DETOURED - the exe asks it for the reagent page record; we substitute the path"),
    ("GD_OBJECTMANAGER_GETLOADTABLE", "Engine",
     "?GetLoadTable@ObjectManager@GAME@@QEBAAEBVLoadTable@2@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z",
     "DETOURED - the other half of the pair ReagentWindow::Load calls"),
    ("GD_LOADTABLEBINARY_GETNUMELEMENTSFORFIELD", "Engine",
     "?GetNumElementsForField@LoadTableBinary@GAME@@UEBAIPEBD@Z",
     "verification: how many entries the record's reagentBoxes array has"),
    # ---- the rescue command mirrors the exe's own reagent take -----------------------------
    # Grim Dawn.exe 0x132B10..0x132C1D, disassembled out of the decrypted exe image. Every step of
    # it is one of these exports; none of the four Item vtable slots it uses (0x410 PlayDropSound,
    # 0x590 GetItemReplicaInfo, 0x610 SetStackSize, 0x618 GetStackSize) is overridden by ANY of
    # the 19 ??_7Item*@GAME@@6BObject@1@@ vftables, so calling the exports directly is identical
    # to the exe's virtual dispatch.
    ("GD_GAMEENGINE_GETMAINPLAYER", "Game",
     "?GetMainPlayer@GameEngine@GAME@@QEBAPEAVPlayer@2@XZ",
     "rescue: step 1 of the exe take - who receives the item"),
    ("GD_PLAYER_ISINVENTORYSPACEAVAILABLE", "Game",
     "?IsInventorySpaceAvailable@Player@GAME@@UEBA_NPEBVItem@2@@Z",
     "rescue: Player vtable +0x938 - the exe's room check before it creates anything"),
    ("GD_PLAYER_PLAYINVENTORYFULLSOUND", "Game",
     "?PlayInventoryFullSound@Player@GAME@@QEAAXXZ",
     "rescue: what the exe does when the room check fails"),
    ("GD_GAMEENGINE_GETITEMMAXSTACKSIZE", "Game",
     "?GetItemMaxStackSize@GameEngine@GAME@@QEBAIXZ",
     "rescue: the clamp the exe applies to the count it takes"),
    ("GD_CHARACTER_GETCONTROLLERID", "Game",
     "?GetControllerId@Character@GAME@@QEBA?BIXZ",
     "rescue: the controller that owns the receiving inventory"),
    ("GD_CONTROLLERCHAR_SENDADDITEMTOINVENTORY", "Game",
     "?SendAddItemToInventory@ControllerCharacter@GAME@@QEAAXI@Z",
     "rescue: THE placement call - the exe take's only inventory write"),
    ("GD_PLAYER_GIVEITEMTOCHARACTER", "Game",
     "?GiveItemToCharacter@Player@GAME@@UEAAXPEAVItem@2@_N1@Z",
     "rescue: Player vtable +0x578, called right after the placement"),
    ("GD_ITEM_PLAYDROPSOUND", "Game",
     "?PlayDropSound@Item@GAME@@UEAAXXZ",
     "rescue: Item vtable +0x410, the last call of the exe take"),
    ("GD_GAMEENGINE_ADDITEMTOTRANSFER", "Game",
     "?AddItemToTransfer@GameEngine@GAME@@QEAA_NII_N@Z",
     "rescue FALLBACK: the transfer stash, used when the inventory is full"),
    ("GD_SACK_ISSPACEFORITEM", "Game",
     "?IsSpaceForItem@InventorySack@GAME@@QEBA_NPEBVItem@2@_N@Z",
     "rescue FALLBACK: read only - the sack-level room test"),
    ("GD_ITEM_VFTABLE_OBJECT", "Game",
     "??_7Item@GAME@@6BObject@1@@",
     "READ ONLY: cross-check that slots 0x410/0x590/0x610/0x618 are the exports above"),
    ("GD_PLAYER_VFTABLE_OBJECT", "Game",
     "??_7Player@GAME@@6BObject@1@@",
     "READ ONLY: cross-check that slots 0x578/0x938 are the two Player exports above"),
    ("GD_LOADTABLEBINARY_VFTABLE", "Engine",
     "??_7LoadTableBinary@GAME@@6B@",
     "verification: only call the Binary accessor when the object really is one"),

    # ---- the box-badge repaint -------------------------------------------------------------
    # GameEngine::SyncCaravanReagents is `mov rcx,[rcx+0x19B0]; jmp [rax+0x88]`
    # = GameUIInterface vtable slot +0x88 = ReagentWindow::Sync (exe 0x1324A0..0x13270D), the
    # BOX-BADGE REPAINT. It walks the box vector at window+0x380, map::finds each box's
    # reagentName and, at 0x13266B, does `if (box[0x30] != node->itemId) box->vt[0xA8](id,true)`.
    # Nothing else in the engine repaints a badge, which is why every relayout left stored items
    # looking empty.
    ("GD_GAMEENGINE_SYNCCARAVANREAGENTS", "Game",
     "?SyncCaravanReagents@GameEngine@GAME@@QEAAXXZ",
     "THE box-badge repaint - called after every live relayout"),
    ("GD_GAMEENGINE_READPLAYERREAGENTS", "Game",
     "?ReadPlayerReagents@GameEngine@GAME@@QEAA_NAEAVCheckedReader@2@@Z",
     "THE load bracket - every stored prototype is created inside it"),

    # ---- player and inventory accessors ----------------------------------------------------
    # Every one of these is EXPORTED BY NAME: no exe RVA and no signature. The two offsets
    # (Player+0x46C8 the player name, Player+0x16C0 the ControllerPlayer id) are decoded at run
    # time out of GetPlayerName's and GetPlayerCtrl's own bytes; a caller that needs one refuses
    # to run if its pattern does not match.
    ("GD_GAMEENGINE_ISGAMELOADING", "Game",
     "?IsGameLoading@GameEngine@GAME@@QEBA_NXZ",
     "half of MENU READY / CHARACTER LOADED"),
    ("GD_PLAYER_GETPLAYERNAME", "Game",
     "?GetPlayerName@Player@GAME@@QEBAPEBGXZ",
     "READ ONLY - `lea rax,[rcx+disp32]` gives the name std::wstring offset"),
    ("GD_PLAYER_GETMAXIMUMSACKS", "Game",
     "?GetMaximumSacks@Player@GAME@@SAIXZ",
     "static - how many inventory sacks this install has"),
    ("GD_PLAYER_GETSACK", "Game",
     "?GetSack@Player@GAME@@QEAAPEAVInventorySack@2@H@Z",
     "one inventory sack (NULL entries are normal)"),
    ("GD_SACK_GETINVENTORY_CONST", "Game",
     "?GetInventory@InventorySack@GAME@@QEBAAEBV?$map@IVRect@GAME@@@mem@@XZ",
     "mem::map<u32 objectId, Rect> - every item id in that sack"),
    ("GD_PLAYERINVCTRL_REMOVEITEM", "Game",
     "?RemoveItem@PlayerInventoryCtrl@GAME@@QEAA_NI_N@Z",
     "exe site A 0x1EAB4E - step 2 of the deposit, ONLY on AddItem true"),
    ("GD_CONTROLLERCHAR_SENDREMOVEITEMFROMINVENTORY", "Game",
     "?SendRemoveItemFromInventory@ControllerCharacter@GAME@@QEAAXI@Z",
     "exe site A 0x1EAB5D - step 3 of the deposit"),
    ("GD_CONTROLLERPLAYER_GETINVENTORYCTRL", "Game",
     "?GetInventoryCtrl@ControllerPlayer@GAME@@QEAAAEAVPlayerInventoryCtrl@2@XZ",
     "`lea rax,[rcx+0x470]` - the RemoveItem `this`, never open-coded"),
    ("GD_CURSORHANDLER_GETPLAYERCTRL", "Game",
     "?GetPlayerCtrl@CursorHandler@GAME@@IEAAPEAVControllerPlayer@2@XZ",
     "READ ONLY - its bytes carry Player+0x16C0, the ControllerPlayer id"),
    ("GD_PLAYERINVCTRL_GETNUMBEROFSACKS", "Game",
     "?GetNumberOfSacks@PlayerInventoryCtrl@GAME@@QEBAIXZ",
     "READ ONLY 0x3DAA10 `([rcx+0x28]-[rcx+0x20])>>3` - how many bags the "
     "PlayerInventoryCtrl RemoveItem searches"),
    ("GD_PLAYERINVCTRL_GETSACK", "Game",
     "?GetSack@PlayerInventoryCtrl@GAME@@QEAAPEAVInventorySack@2@H@Z",
     "READ ONLY 0x3DA770 `[[rcx+0x20]+rdx*8]` - one of those bags (no bounds "
     "check: clamp with GetNumberOfSacks)"),
    ("GD_SACK_CONTAINSITEM_ID", "Game",
     "?ContainsItem@InventorySack@GAME@@QEBA_NI@Z",
     "READ ONLY 0x30BBD0 - the same sack+0x20 map and the same node+0x1C key "
     "PlayerInventoryCtrl::RemoveItem walks, so it answers 'will the caller's removal find it'"),
    ("GD_GAMEENGINE_SETAUTOSAVE", "Game",
     "?SetAutoSave@GameEngine@GAME@@QEAAX_N@Z",
     "`mov byte [rcx+0x35],dl; ret` - off for the run, restored on every exit"),
    ("GD_GAMEENGINE_GETAUTOSAVE", "Game",
     "?GetAutoSave@GameEngine@GAME@@QEBA_NXZ",
     "the matching getter, so the step can assert what it did"),
    # ---- the item rollover (tooltip). ALL FIVE ARE OPTIONAL - a
    # miss turns `tooltip_mark` off for the session and can never fail start-up.
    ("GD_ITEM_GETUIDISPLAYTEXT", "Game",
     "?GetUIDisplayText@Item@GAME@@UEBAXPEBVCharacter@2@AEAV?$vector@UGameTextLine@GAME@@@mem@@_N@Z",
     "THE tooltip assembly (0x3118A0, Item vtable +0x458). ItemEquipment calls "
     "it with a plain rel32 at 0x3299AF, so one detour covers the whole equipment collection"),
    ("GD_ITEMEQUIPMENT_GETUIDISPLAYTEXT", "Game",
     "?GetUIDisplayText@ItemEquipment@GAME@@UEBAXPEBVCharacter@2@AEAV?$vector@UGameTextLine@GAME@@@mem@@_N@Z",
     "it CALLS the base (0x3299AF) and then appends its own lines, so the latch "
     "taken inside the base sees a PARTIAL vector - hooking the override too is what makes the "
     "line count an exact fingerprint for the whole equipment collection"),
    ("GD_ITEMARTIFACT_GETUIDISPLAYTEXT", "Game",
     "?GetUIDisplayText@ItemArtifact@GAME@@UEBAXPEBVCharacter@2@AEAV?$vector@UGameTextLine@GAME@@@mem@@_N@Z",
     "relics are ItemArtifact in this collection and the override does NOT call "
     "the base, so it needs its own detour"),
    ("GD_ITEMRELIC_GETUIDISPLAYTEXT", "Game",
     "?GetUIDisplayText@ItemRelic@GAME@@UEBAXPEBVCharacter@2@AEAV?$vector@UGameTextLine@GAME@@@mem@@_N@Z",
     "the other override that does not call the base - cheap, so it is covered too"),
    ("GD_GAMETEXTLINETOSTRING", "Game",
     "?GameTextLineToString@GAME@@YAXAEBV?$vector@UGameTextLine@GAME@@@mem@@AEAV?$list@UGameTextString@GAME@@@3@@Z",
     "THE layout entry point (0x302A90). Reads the vector at [+0] and [+8] only "
     "and writes nothing, which is what makes a borrowed N+1 array safe"),
    ("GD_GAMETEXTLINE_CTOR_W", "Game",
     "??0GameTextLine@GAME@@QEAA@W4GameTextClass@1@AEBV?$basic_string@GU?$char_traits@G@std@@V?$allocator@G@2@@std@@_NPEBVGraphicsTexture@1@M@Z",
     "the wide-string GameTextLine constructor (0x2FEC10) - called ONCE per "
     "process per state, so the mod never hand-builds an engine struct"),
]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--exports", default=DEFAULT_EXPORTS)
    ap.add_argument("--out", default=DEFAULT_OUT)
    args = ap.parse_args()
    tables = {}
    for dll in ("Game", "Engine"):
        path = os.path.join(args.exports, "%s-x64-exports.txt" % dll)
        tables[dll] = set(line.rstrip("\n") for line in open(path, encoding="latin-1"))
    out = ["// gd_exports_reagent.h - the reagent-page mangled names.",
           "//",
           "// GENERATED by tools/gen_reagent_header.py - do not hand-edit. Every name is",
           "// copied verbatim out of tools/exports/{Game,Engine}-x64-exports.txt (1.3.0.8 x64)",
           "// after being found there, so no mangled name is ever typed by hand.",
           "",
           "#pragma once",
           ""]
    missing = []
    for macro, dll, name, why in WANTED:
        if name not in tables[dll]:
            missing.append((macro, dll, name))
            continue
        out.append("// %s.dll : %s" % (dll, why))
        out.append('#define %s "%s"' % (macro, name))
        out.append("")
    if missing:
        for macro, dll, name in missing:
            print("MISSING in %s-x64-exports.txt: %s (%s)" % (dll, name, macro), file=sys.stderr)
        return 1
    dst = args.out
    with open(dst, "w", encoding="ascii", newline="\n") as fh:
        fh.write("\n".join(out))
    print("wrote %s (%d names)" % (dst, len(WANTED)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
