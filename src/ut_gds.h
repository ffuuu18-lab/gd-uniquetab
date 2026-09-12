// ut_gds.h - the GD STASH IMPORT FILE (.gds), and nothing else.
//
// WHY. `uniq-export.csv` is a spreadsheet nothing reads. The export wants to be a file a real
// program consumes: GD Stash (v1.90b, Java, its own Derby database) imports and exports items as
// `.gds`, so the mod writes `uniq-export.gds` beside the journal and the CSV is kept but
// defaults OFF.
//
// This header is the FORMAT and nothing but the format: a plain struct of the 20 fields one
// record carries plus a pure encoder that appends them to a std::string. No Win32, no engine,
// no allocation policy - so `tools\test_journal.cpp` links it exactly as the mod does and
// `tools\gds_read.py` is a line-for-line transcription of the same spec.
//
// ===== THE SPEC, DECOMPILED (GDStash.jar 1.90b, javap -c -p) ==================================
//
// WRITER  org.gdstash.db.DBStashItem.writeDBStashItemListGDS(File, Charset, List<DBStashItem>)
//         -> intToBytes4(3)  intToBytes4(list.size())  then item.writeGDS(...) per item
// READER  org.gdstash.db.DBStashItem.loadGDS(File, Charset)
//         -> readInt() = version, readInt() = count, then `count` x new DBStashItem().readGDS(
//            in, path, charset, version). The version is NOT validated - it is only compared
//            (`>= 2`, `>= 3`) to decide which fields are present. A read failure costs THAT
//            ITEM only; the others still import.
// UI      org.gdstash.ui.GDMassImportPane$GDStashExportListener / $GDStashLoadListener - the
//         two buttons whose icons are `mass_gds_exp.png` / `mass_gds_load.png`, file filter
//         `*.gds`, and both hand `GDConstants.CHARSET_STASH` down, which is **UTF-8**
//         (`STR_CHARSET_STASH = "UTF-8"`).
//
// PRIMITIVES
//   int      GDWriter.intToBytes4 / GDReader.readInt - 4 bytes, LITTLE ENDIAN, signed.
//   string   GDWriter.writeStringUByte: a null string writes ONE 0x00 byte; otherwise ONE byte
//            holding the UTF-8 byte count, then those bytes. `FileOutputStream.write(int)`
//            keeps the LOW 8 BITS ONLY, so a string of 256+ bytes would corrupt the stream -
//            the mod refuses such a field instead (see `utGdsAppendString`). GDReader.
//            readStringUByte reads the length byte; 0 comes back as **null**, not as "".
//   bool     one byte, non-zero = true.
//
// ONE RECORD, in exactly this order (readGDS's own order; `v` = the file's version int):
//    1  itemID            string   the item's own DBR record
//    2  prefixID          string
//    3  suffixID          string
//    4  modifierID        string
//    5  transmuteID       string   the ILLUSION (transmuted appearance)
//    6  seed              int
//    7  relicID           string   the inserted component / relic ("materia")
//    8  relicBonusID      string   its completion bonus
//    9  relicSeed         int
//   10  enchantmentID     string   the augment
//   11  enchantmentLevel  int
//   12  enchantmentSeed   int
//   13  ascendantID       string   v >= 2 ONLY
//   14  ascendant2hID     string   v >= 2 ONLY
//   15  var1              int
//   16  stackCount        int
//   17  rerollsUsed       int      v >= 2 ONLY
//   18  affixRerollsUsed  int      v >= 3 ONLY
//   19  hardcore          bool     the item came out of a HARDCORE character's stash
//   20  charname          string   which character it came from; may be null
//
// We write version 3 - what GD Stash 1.90b itself writes - so every field is present.
//
// ===== HOW EACH FIELD IS FILLED FROM THE JOURNAL ==============================================
//
// The 18 typed fields 1..18 are the `ItemReplicaInfo` struct in ITS OWN serialisation order.
// `ItemReplicaInfo::WriteProperties` (Game.dll rva 0x570810, decoded and quoted in
// ut_rescue.cpp's format-3 block) writes:
//
//    str +0x08, +0x28, +0x48, +0x70, +0x100, u32 +0x68, str +0x90, +0xB0, u32 +0xD0,
//    str +0xD8, u32 +0xF8, +0xFC, str +0x120, +0x140, u32 +0x160, +0x178, +0x180, +0x17C
//
// which is 18 fields whose TYPES match the .gds list above one for one
// (s s s s s i s s i s i i s s i i i i). So the mapping is not a guess from position alone -
// it is two independently decoded orders that agree field for field:
//
//    itemID <- the journal's `record` (== slot +0x008, "baseRecord", on 357 of the user's 358
//              real entries)            prefixID <- +0x028   suffixID <- +0x048
//    modifierID <- +0x070               transmuteID <- +0x100 (the journal calls it `illusion`)
//    seed <- u32 +0x068                 relicID <- +0x090 (`component`)
//    relicBonusID <- +0x0B0 (`completionBonus`)             relicSeed <- u32 +0x0D0
//    enchantmentID <- +0x0D8 (`augment`)                    enchantmentLevel <- u32 +0x0F8
//    enchantmentSeed <- u32 +0x0FC      ascendantID <- +0x120   ascendant2hID <- +0x140
//    var1 <- u32 +0x160                 stackCount <- the journal's own `stack`
//    rerollsUsed <- u32 +0x180          affixRerollsUsed <- u32 +0x17C
//
// AND THIS RESOLVES ONE OPEN QUESTION IN ut_rescue.cpp: the "UNIDENTIFIED u32 adjacent to the
// component slot" at +0x0D0 is GD Stash's `relicSeed`. It stays inside `raw` and is still not
// NAMED in the journal (the journal names string slots only), but the .gds writer knows what
// it is. Every one of those u32s sits immediately AFTER a 0x20-byte string window, so none of
// them can ever fall inside a recorded slot - which is why reading them out of the journal's
// reconstructed blob is sound.
//
// WHAT THE JOURNAL CANNOT FILL, and why the default is safe:
//   * `hardcore` - not an item property at all. It is which STASH the item came out of, and
//     the mod has no hardcore character. 0 (softcore) is the only honest answer, and GD Stash's
//     own `transfer_hardcore` restriction is what decides where the item may go afterwards.
//   * `charname` - the same: the exporting stash's character. Written as the null string (one
//     0x00 byte), which is exactly what GD Stash writes for a shared-stash item.
//   * There is NO soulbound and NO untradeable field in this format. GD Stash derives both from
//     the item's own record through its `DBItem`, so the journal's UT_JF_* flags have nowhere
//     to go and nothing to fix; `transfer_soulbound=false` in GDStash.ini keeps working exactly
//     as it does for an item GD Stash imported from a real stash file.
//   * an entry with `"len":0` (a reconcile-synthesised row: the mod knows the record is in the
//     page but never saw its replica) exports the record and nothing else - no affixes, no
//     component, seed 0. GD Stash will show a base item of that record. It is not a lie: the
//     mod never knew more than that.
#ifndef UT_GDS_H
#define UT_GDS_H

#include <string>

namespace ut {

// The .gds version this build writes. 3 = what GD Stash 1.90b writes itself, i.e. every field.
const int kUtGdsVersion = 3;

// One record's worth of fields, in the file's own order. Strings are UTF-8; an EMPTY string and
// a "null" one are the same thing on the wire (a single 0x00 length byte), so this struct has no
// separate null flag.
struct UtGdsItem {
    std::string itemID;
    std::string prefixID;
    std::string suffixID;
    std::string modifierID;
    std::string transmuteID;
    unsigned int seed = 0;
    std::string relicID;
    std::string relicBonusID;
    unsigned int relicSeed = 0;
    std::string enchantmentID;
    unsigned int enchantmentLevel = 0;
    unsigned int enchantmentSeed = 0;
    std::string ascendantID;
    std::string ascendant2hID;
    unsigned int var1 = 0;
    unsigned int stackCount = 1;
    unsigned int rerollsUsed = 0;
    unsigned int affixRerollsUsed = 0;
    bool hardcore = false;
    std::string charname;
};

// 4 bytes, little endian - GDWriter.intToBytes4.
inline void utGdsAppendU32(std::string* out, unsigned int v) {
    out->push_back((char)(unsigned char)(v & 0xFFu));
    out->push_back((char)(unsigned char)((v >> 8) & 0xFFu));
    out->push_back((char)(unsigned char)((v >> 16) & 0xFFu));
    out->push_back((char)(unsigned char)((v >> 24) & 0xFFu));
}

// One length byte then the bytes - GDWriter.writeStringUByte. Returns FALSE for a string of 256
// bytes or more: Java's `write(int)` would truncate the length to its low 8 bits and every field
// after it in the file would be read from the wrong offset, so a caller that cannot shorten the
// value must abandon the whole file rather than publish a stream that decodes as garbage. (No
// real DBR path comes close - the longest record in this build's own catalogue is well under
// 100 bytes - so this is a guard against a hand-edited journal, not against the game.)
inline bool utGdsAppendString(std::string* out, const std::string& s) {
    if (s.size() > 255) return false;
    out->push_back((char)(unsigned char)s.size());
    if (!s.empty()) out->append(s);
    return true;
}

// version + count. Nothing else is in the file header.
inline void utGdsAppendHeader(std::string* out, unsigned int count) {
    utGdsAppendU32(out, (unsigned int)kUtGdsVersion);
    utGdsAppendU32(out, count);
}

// One record, in readGDS's order, at version 3 (so no field is skipped). False = a string field
// was too long to encode; `out` may then hold a partial record and the caller must discard the
// whole buffer.
inline bool utGdsAppendItem(std::string* out, const UtGdsItem& it) {
    if (!utGdsAppendString(out, it.itemID)) return false;
    if (!utGdsAppendString(out, it.prefixID)) return false;
    if (!utGdsAppendString(out, it.suffixID)) return false;
    if (!utGdsAppendString(out, it.modifierID)) return false;
    if (!utGdsAppendString(out, it.transmuteID)) return false;
    utGdsAppendU32(out, it.seed);
    if (!utGdsAppendString(out, it.relicID)) return false;
    if (!utGdsAppendString(out, it.relicBonusID)) return false;
    utGdsAppendU32(out, it.relicSeed);
    if (!utGdsAppendString(out, it.enchantmentID)) return false;
    utGdsAppendU32(out, it.enchantmentLevel);
    utGdsAppendU32(out, it.enchantmentSeed);
    if (!utGdsAppendString(out, it.ascendantID)) return false;
    if (!utGdsAppendString(out, it.ascendant2hID)) return false;
    utGdsAppendU32(out, it.var1);
    utGdsAppendU32(out, it.stackCount);
    utGdsAppendU32(out, it.rerollsUsed);
    utGdsAppendU32(out, it.affixRerollsUsed);
    out->push_back(it.hardcore ? (char)1 : (char)0);
    if (!utGdsAppendString(out, it.charname)) return false;
    return true;
}

}  // namespace ut

#endif  // UT_GDS_H
