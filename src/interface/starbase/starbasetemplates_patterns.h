// Reviewed Ghidra instruction signatures; source evidence in re/starbase_templates.md.
#pragma once
namespace starbasetemplates {
inline constexpr char kComponentCommand[] =
    "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 48 89 4C 24 08 57 41 56 41 57 48 83 EC "
    "20 8B F2 4D 8B F9 48 8B 15 ?? ?? ?? ?? 49 8B E8 4C 8B F1 48 85 D2 74 2D 45 8B 80 98 "
    "00 00 00 41 8B C0 25 FF FF FF 00 3B 42 20 73 19 8B C8 48 8B 42 18 48 03 C9 48 8B 5C "
    "C8 08 48 85 DB 74 06 44 39 43 18 74 07 48 8B 1D ?? ?? ?? ?? 48 8B 03 48 8B CB FF 50 "
    "20 84 C0 74 1E 48 8B 83 68 01 00 00 8B 88 10 05 00 00 48 C1 E9 12 F6 C1 01 74 08 8B "
    "BB 04 01 00 00 EB 06 8B BD 80 08 00 00 BB FF FF FF FF 48 85 ED 74 03 8B 5D 08 B9 20 "
    "00 00 00 E8 ?? ?? ?? ?? 8B 4C 24 60 33 D2 48 89 44 24 40 48 8B 6C 24 50 4C 89 78 08 "
    "89 4C 24 44 89 5C 24 40 48 8B 4C 24 40 48 8B 5C 24 48 48 89 48 10 48 8D 0D ?? ?? ?? "
    "?? 48 89 08 48 8D 0D ?? ?? ?? ?? 49 89 0E 41 C7 46 08 FF FF FF FF 41 89 56 0C 41 C7 "
    "46 10 00 00 FF FF 66 41 89 56 14 41 88 56 16 41 89 56 18 49 89 46 20 49 8B C6 41 89 "
    "76 28 48 8B 74 24 58 41 89 7E 2C 48 83 C4 20 41 5F 41 5E 5F C3 ";
inline constexpr char kUpgradeCommand[] =
    "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 41 56 41 57 48 83 EC 20 8B F2 4D 8B "
    "F9 48 8B 15 ?? ?? ?? ?? 49 8B E8 4C 8B F1 48 85 D2 74 2D 45 8B 80 98 00 00 00 41 8B "
    "C0 25 FF FF FF 00 3B 42 20 73 19 8B C8 48 8B 42 18 48 03 C9 48 8B 5C C8 08 48 85 DB "
    "74 06 44 39 43 18 74 07 48 8B 1D ?? ?? ?? ?? 48 8B 03 48 8B CB FF 50 20 84 C0 74 1E "
    "48 8B 83 68 01 00 00 8B 88 10 05 00 00 48 C1 E9 12 F6 C1 01 74 08 8B BB 04 01 00 00 "
    "EB 06 8B BD 80 08 00 00 BB FF FF FF FF 48 85 ED 74 03 8B 5D 08 B9 18 00 00 00 E8 ?? "
    "?? ?? ?? 48 89 44 24 40 48 8D 0D ?? ?? ?? ?? 48 8B 6C 24 50 33 D2 4C 89 78 08 89 58 "
    "10 48 8B 5C 24 48 48 89 08 48 8D 0D ?? ?? ?? ?? 49 89 0E 41 C7 46 08 FF FF FF FF 41 "
    "89 56 0C 41 C7 46 10 00 00 FF FF 66 41 89 56 14 41 88 56 16 41 89 56 18 49 89 46 20 "
    "49 8B C6 41 89 76 28 48 8B 74 24 58 41 89 7E 2C 48 83 C4 20 41 5F 41 5E 5F C3 ";
inline constexpr char kModuleFinished[] =
    "48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 10 FF FF FF 48 81 EC F0 "
    "01 00 00 41 0F B6 D8 4C 8B F1 45 33 FF 44 89 BD 40 01 00 00 48 8B 15 ?? ?? ?? ?? 48 "
    "85 D2 74 2A 44 8B 41 10 41 8B C0 25 FF FF FF 00 3B 42 20 73 19 8B C8 48 03 C9 48 8B "
    "42 18 48 8B 7C C8 08 48 85 FF 74 06 44 39 47 08 74 07 ";
inline constexpr char kBuildingFinished[] =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 50 41 0F B6 F8 48 8B F1 48 "
    "8B 15 ?? ?? ?? ?? 48 85 D2 74 2A 44 8B 41 10 41 8B C0 25 FF FF FF 00 3B 42 20 73 19 "
    "8B C8 48 03 C9 48 8B 42 18 48 8B 5C C8 08 48 85 DB 74 06 44 39 43 08 74 07 48 8B 1D "
    "?? ?? ?? ?? 4C 63 46 14 44 3B 83 04 06 00 00 7C 09 ";
inline constexpr char kQueueWalk[] =
    "40 53 57 48 83 EC 28 48 8B 15 ?? ?? ?? ?? 48 8B F9 48 85 D2 74 2D 44 8B 81 98 00 00 "
    "00 41 8B C0 25 FF FF FF 00 3B 42 20 73 19 8B C8 48 8B 42 18 48 03 C9 48 8B 5C C8 08 "
    "48 85 DB 74 06 44 39 43 18 74 07 48 8B 1D ?? ?? ?? ?? 48 8B 03 48 8B CB FF 50 20 84 "
    "C0 74 1E 48 8B 83 68 01 00 00 8B 88 10 05 00 00 48 C1 E9 12 F6 C1 01 74 08 8B 9B 04 "
    "01 00 00 EB 06 8B 9F 80 08 00 00 48 89 6C 24 40 8B CB 48 89 74 24 50 4C 89 74 24 58 "
    "4C 89 7C 24 20 E8 ?? ?? ?? ?? 84 C0 0F 85 FF 00 00 00 48 8B 15 ?? ?? ?? ?? 48 85 D2 "
    "74 24 8B C3 25 FF FF FF 00 3B 42 20 73 18 8B C8 48 8B 42 18 48 03 C9 48 8B 74 C8 08 "
    "48 85 F6 74 05 39 5E 08 74 07 48 8B 35 ?? ?? ?? ?? 44 8B 76 2C 45 33 FF 41 8B DF 45 "
    "85 F6 0F 8E B1 00 00 00 0F 1F 84 00 00 00 00 00 48 8B 15 ?? ?? ?? ?? 48 85 D2 74 31 "
    "48 8B 46 20 48 63 CB 44 8B 04 88 41 8B C0 25 FF FF FF 00 3B 42 20 73 19 8B C8 48 8B "
    "42 18 48 03 C9 48 8B 4C C8 08 48 85 C9 74 06 44 39 41 08 74 07 48 8B 0D ?? ?? ?? ?? "
    "48 8B 49 18 4C 8D 44 24 48 48 8D 15 ?? ?? ?? ?? 4C 89 7C 24 48 48 8B 01 FF 90 A0 00 "
    "00 00 84 C0 74 38 48 8B 7C 24 48 48 8B CF 48 8B 07 FF 50 10 48 8B 50 08 48 8B E8 48 "
    "8D 48 08 FF 52 08 84 C0 74 18 8B 8F 2C 05 00 00 85 C9 78 0E 3B 4D 2C 7D 09 80 BF 68 "
    "04 00 00 0A 74 0F FF C3 41 3B DE 0F 8C 57 FF FF FF 32 C0 EB 02 B0 01 4C 8B 7C 24 20 "
    "4C 8B 74 24 58 48 8B 74 24 50 48 8B 6C 24 40 48 83 C4 28 5F 5B C3 ";
inline constexpr char kOwner[] = "48 89 5C 24 08 57 48 83 EC 20 48 8B F9 E8 ?? ?? ?? ?? 48 8B 50 18 48 8B D8 48 8D 48 "
                                 "18 FF 52 08 84 C0 75 4F 48 8B 15 ?? ?? ?? ?? 48 85 D2 74 2D 44 8B 87 98 00 00 00 41 "
                                 "8B C0 25 FF FF FF 00 3B 42 20 73 19 8B C8 48 8B 42 18 48 03 C9 48 8B 4C C8 08 48 85 "
                                 "C9 74 06 44 39 41 18 74 07 48 8B 0D ?? ?? ?? ?? 48 8B 5C 24 30 48 83 C4 20 5F E9 ?? "
                                 "?? ?? ?? 48 8B C3 48 8B 5C 24 30 48 83 C4 20 5F C3 ";
inline constexpr char kCanUpgrade[] =
    "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 41 56 41 57 48 8D 6C 24 B9 48 81 EC "
    "A0 00 00 00 49 8B D8 4C 8B F2 48 8B F1 48 8B 02 48 8B CA FF 50 08 84 C0 0F 84 F2 02 "
    "00 00 40 B7 01 48 8B D3 48 8B CE E8 ?? ?? ?? ?? 84 C0 74 0C 48 85 DB 0F 84 D7 02 00 "
    "00 40 32 FF 48 8B CE E8 ?? ?? ?? ?? 45 33 FF 84 C0 0F 84 FC 00 00 00 48 85 DB 0F 84 "
    "B8 02 00 00 41 8D 57 01 48 8B CB E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 89 45 C7 C7 "
    "45 CF 1F 00 00 00 44 88 7D D3 0F 28 45 C7 66 0F 7F 45 C7 45 33 C9 45 33 C0 48 8D 55 "
    "C7 48 8D 4D D7 E8 ?? ?? ?? ?? 90 0F 10 08 66 0F 6F C1 66 0F 73 D8 08 66 0F 7E C0 4C "
    "63 C0 44 89 7D E7 0F 57 C0 F3 0F 7F 45 EF 66 0F 6F 05 ?? ?? ?? ?? F3 0F 7F 45 07 44 "
    "88 7D F7 66 48 0F 7E CA 48 8D 4D E7 E8 ?? ?? ?? ?? 90 48 8D 4D D7 E8 ?? ?? ?? ?? 90 "
    "4C 8D 45 E7 33 D2 48 8D 4D 17 E8 ?? ?? ?? ?? 90 48 8D 50 10 48 83 7A 18 10 72 03 48 "
    "8B 12 4C 8B 40 20 48 8B CB E8 ?? ?? ?? ?? 90 48 83 7D 3F 10 72 0F 83 7D 17 01 74 09 "
    "48 8B 4D 27 E8 ?? ?? ?? ?? 66 0F 6F 05 ?? ?? ?? ?? F3 0F 7F 45 37 C6 45 27 00 48 83 "
    "7D 0F 10 72 0F 83 7D E7 01 74 09 48 8B 4D F7 E8 ?? ?? ?? ?? 40 32 FF 48 8B 8E 90 08 "
    "00 00 4C 3B F1 0F 84 23 01 00 00 48 8B 89 00 01 00 00 48 8B 01 FF 50 08 84 C0 0F 85 "
    "0E 01 00 00 48 85 DB 0F 84 97 01 00 00 BA 01 00 00 00 48 8B CB E8 ?? ?? ?? ?? 48 8D "
    "05 ?? ?? ?? ?? 48 89 45 C7 C7 45 CF 1C 00 00 00 C6 45 D3 00 0F 28 45 C7 66 0F 7F 45 "
    "D7 45 33 C9 45 33 C0 48 8D 55 D7 48 8D 4D C7 E8 ?? ?? ?? ?? 90 0F 10 08 66 0F 6F C1 "
    "66 0F 73 D8 08 66 0F 7E C0 4C 63 C0 44 89 7D E7 0F 57 C0 F3 0F 7F 45 EF 66 0F 6F 05 "
    "?? ?? ?? ?? F3 0F 7F 45 07 C6 45 F7 00 66 48 0F 7E CA 48 8D 4D E7 E8 ?? ?? ?? ?? 90 "
    "48 8D 4D C7 E8 ?? ?? ?? ?? 90 4C 8D 45 E7 33 D2 48 8D 4D 17 E8 ?? ?? ?? ?? 90 48 8D "
    "50 10 48 83 7A 18 10 72 03 48 8B 12 4C 8B 40 20 48 8B CB E8 ?? ?? ?? ?? 90 48 83 7D "
    "3F 10 72 0F 83 7D 17 01 74 09 48 8B 4D 27 E8 ?? ?? ?? ?? 66 0F 6F 05 ?? ?? ?? ?? F3 "
    "0F 7F 45 37 C6 45 27 00 48 83 7D 0F 10 0F 82 A8 00 00 00 83 7D E7 01 0F 84 9E 00 00 "
    "00 48 8B 4D F7 E8 ?? ?? ?? ?? 40 32 FF 40 0F B6 C7 E9 94 00 00 00 4D 8B BE F8 00 00 "
    "00 48 8B CE E8 ?? ?? ?? ?? 48 8B D0 49 8B CF E8 ?? ?? ?? ?? 84 C0 74 74 48 8B CE E8 "
    "?? ?? ?? ?? 48 8B D0 4C 8B C3 49 8B CF E8 ?? ?? ?? ?? 84 C0 75 08 48 85 DB 74 55 40 "
    "32 FF 48 8B CE E8 ?? ?? ?? ?? 48 8B D0 4C 8B C3 49 8B CF E8 ?? ?? ?? ?? 84 C0 75 08 "
    "48 85 DB 74 33 40 32 FF 48 8B 8E 90 08 00 00 4C 3B F1 74 1E 4C 8B C3 48 8B D6 48 8B "
    "89 00 01 00 00 E8 ?? ?? ?? ?? 84 C0 75 08 48 85 DB 74 09 40 32 FF 40 0F B6 C7 EB 02 "
    "32 C0 4C 8D 9C 24 A0 00 00 00 49 8B 5B 20 49 8B 73 28 49 8B 7B 30 49 8B E3 41 5F 41 "
    "5E 5D C3 ";
inline constexpr char kOccupation[] =
    "40 53 48 83 EC 70 48 8B DA 48 8D 94 24 90 00 00 00 E8 ?? ?? ?? ?? 83 38 FF 0F 84 E2 "
    "00 00 00 48 85 DB 0F 84 D1 00 00 00 BA 01 00 00 00 48 8B CB E8 ?? ?? ?? ?? 48 8D 05 "
    "?? ?? ?? ?? 48 89 44 24 20 C7 44 24 28 19 00 00 00 C6 44 24 2C 00 0F 28 44 24 20 66 "
    "0F 7F 44 24 20 45 33 C9 45 33 C0 48 8D 54 24 20 48 8D 4C 24 30 E8 ?? ?? ?? ?? 90 0F "
    "10 08 66 0F 6F C1 66 0F 73 D8 08 66 0F 7E C0 4C 63 C0 C7 44 24 40 00 00 00 00 0F 57 "
    "C0 F3 0F 7F 44 24 48 66 0F 6F 05 ?? ?? ?? ?? F3 0F 7F 44 24 60 C6 44 24 50 00 66 48 "
    "0F 7E CA 48 8D 4C 24 40 E8 ?? ?? ?? ?? 90 48 8D 4C 24 30 E8 ?? ?? ?? ?? 90 48 8D 54 "
    "24 50 48 83 7C 24 68 10 48 0F 43 54 24 50 4C 8B 44 24 60 48 8B CB E8 ?? ?? ?? ?? 90 "
    "48 83 7C 24 68 10 72 11 83 7C 24 40 01 74 0A 48 8B 4C 24 50 E8 ?? ?? ?? ?? B0 01 48 "
    "83 C4 70 5B C3 32 C0 48 83 C4 70 5B C3 ";
inline constexpr char kReadModule[] =
    "48 89 5C 24 08 57 48 83 EC 20 48 8B DA 48 8B F9 41 81 E8 69 2C 00 00 74 5A 41 81 E8 "
    "D6 0D 00 00 74 37 48 8B CA 41 83 F8 0D 74 0F 48 8B 5C 24 30 48 83 C4 20 5F E9 ?? ?? "
    "?? ?? 48 8B 15 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 4F 08 48 89 41 08 48 8B 5C 24 30 48 "
    "83 C4 20 5F C3 48 8B 51 08 48 8B CB 48 83 C2 10 48 8B 5C 24 30 48 83 C4 20 5F E9 ?? "
    "?? ?? ?? 4C 8B 41 08 48 8D 15 ?? ?? ?? ?? 48 8B 8B 88 02 00 00 49 83 C0 14 E8 ?? ?? "
    "?? ?? 85 C0 75 08 48 8B CB E8 ?? ?? ?? ?? 48 8B 5C 24 30 48 83 C4 20 5F C3 ";
inline constexpr char kReadBuilding[] =
    "48 89 5C 24 08 57 48 83 EC 20 48 8B DA 48 8B F9 41 81 E8 69 2C 00 00 74 5A 41 81 E8 "
    "D6 0D 00 00 74 37 48 8B CA 41 83 F8 0E 74 0F 48 8B 5C 24 30 48 83 C4 20 5F E9 ?? ?? "
    "?? ?? 48 8B 15 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 4F 08 48 89 41 08 48 8B 5C 24 30 48 "
    "83 C4 20 5F C3 48 8B 51 08 48 8B CB 48 83 C2 10 48 8B 5C 24 30 48 83 C4 20 5F E9 ?? "
    "?? ?? ?? 4C 8B 41 08 48 8D 15 ?? ?? ?? ?? 48 8B 8B 88 02 00 00 49 83 C0 14 E8 ?? ?? "
    "?? ?? 85 C0 75 08 48 8B CB E8 ?? ?? ?? ?? 48 8B 5C 24 30 48 83 C4 20 5F C3 ";
inline constexpr char kReadLevel[] =
    "40 53 48 83 EC 20 4C 8B CA 48 8B D9 41 81 E8 3F 3A 00 00 74 2D 48 8B CA 41 83 F8 10 "
    "74 0A 48 83 C4 20 5B E9 ?? ?? ?? ?? 48 8B 15 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 4B 08 "
    "48 89 41 08 48 83 C4 20 5B C3 48 8B 51 08 49 8B C9 48 83 C2 10 48 83 C4 20 5B E9 ?? "
    "?? ?? ?? ";
}
namespace starbasetemplates {
// CStarbaseDefensesTabController::Upgrade (GOG PDB), the defenses tab's Upgrade button: builds a
// CDefensivePlatformsUpgradeDesignCommand (0x28 bytes: vtable, CCommand fields +0x8..+0x18, country
// id +0x20, starbase id +0x24) on the stack, checks IsValid, then copies it to the heap and posts it.
// Operands: game state +24, GetLocalObserved +29, vtable +72, IsValid +116, allocator +130, post +184.
inline constexpr char kPlatformUpgrade[] =
    "48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 83 EC 50 48 8B D9 48 8B 0D ?? ?? ?? ?? E8 ?? ?? "
    "?? ?? C7 44 24 28 FF FF FF FF 33 F6 89 74 24 2C C7 44 24 30 00 00 FF FF 66 89 74 24 34 40 88 "
    "74 24 36 89 74 24 38 4C 8D 35 ?? ?? ?? ?? 4C 89 74 24 20 48 85 C0 BF FF FF FF FF 74 03 8B 78 "
    "20 89 7C 24 40 8B 9B ?? ?? ?? ?? 89 5C 24 44 33 D2 48 8D 4C 24 20 E8 ?? ?? ?? ?? 84 C0 74 41 "
    "B9 28 00 00 00 E8 ?? ?? ?? ?? 48 89 44 24 70 C7 40 08 FF FF FF FF 89 70 0C C7 40 10 00 00 FF "
    "FF 66 C7 40 14 00 00 C6 40 16 00 89 70 18 4C 89 30 89 78 20 89 58 24 33 D2 48 8B C8 E8 ?? ?? "
    "?? ??";
}
namespace starbasetemplates {
// Template editor (re/starbase_templates.md, "Template editor").
// CStarbase::CalcFreeModuleSlots and CalcFreeBuildingSlots (GOG PDB): the cached capacity minus
// the valid entries of the completed array. Two matches; the array operands tell them apart.
// Operands: array data +3, array count +12, cached capacity +18.
inline constexpr char kFreeSlots[] =
    "48 8B 99 ?? ?? 00 00 33 FF 48 63 81 ?? ?? 00 00 8B A9 ?? ?? 00 00 48 8D 34 C3 48 3B DE 74 19 "
    "48 8B 0B 48 8B 01 FF 50 08 84 C0 74 03 48 FF C7 48 83 C3 08 48 3B DE 75 E7";
// CStarbaseLevelType::GetModuleSlotType and GetBuildingSlotType (GOG PDB): index against the
// count of _ModuleSlots / _BuildingSlots (CPdxArray<CString>), then its data. Two matches.
// Operands: array count +8, array data +28.
inline constexpr char kLevelSlotType[] =
    "45 85 C0 78 25 44 3B 81 ?? ?? 00 00 7D 1C 49 63 C0 48 8D 14 40 48 C1 E2 04 48 03 91 ?? ?? 00 "
    "00 48 8B CB E8";
// CStarbaseComponent<T>::GetName(CString* out, const CStarbase*) (GOG PDB; one body for modules
// and buildings): the triggered _Name (+0xb0, count +0xc4) with a starbase, else the
// localisation of "sm_" + key.
inline constexpr char kComponentName[] =
    "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 4C 89 74 24 20 55 48 8D AC 24 90 FE FF FF 48 81 "
    "EC 70 02 00 00 49 8B F0 48 8B FA 48 8B D9 45 33 F6 44 89 74 24 30 4D 85 C0 0F 84 ?? ?? ?? ?? "
    "44 39 B1 C4 00 00 00 0F 84 ?? ?? ?? ?? 48 8D 4D 00 E8 ?? ?? ?? ?? 90 8B 46 08 48 C7 45 08 00 "
    "00 02 00 89 45 10 4C 89 75 14 4C 89 75 1C 48 8D 8B B0 00 00 00";
// Inside it: the key (+0x10) and the "sm_" literal (operand +7).
inline constexpr char kComponentNamePrefix[] = "4C 8D 41 10 48 8D 15 ?? ?? ?? ?? 48 8D 4C 24 68 E8";
inline constexpr size_t kComponentNameSize = 0x2e3;
// CStarbaseComponent<T>::ReadMember (GOG PDB), where it stores `starbase_type = <word>` into
// _StarbaseType (EBuildingStarbaseType): six stores of 0, 4, 1, 5, 3, 2 by the word's token. The
// four tokens after the first compare are consecutive and give 0-3, in the order the game's
// documentation lists the words: starbase, orbital_ring, waystation, arkship; 4 is "all", which
// the constructor also sets; 5 is any other word.
// Operands: the field's offset at +4, +20, +36, +65, +81 and +97.
inline constexpr char kComponentTypeRead[] =
    "33 DB 89 9E ?? ?? 00 00 E9 ?? ?? ?? ?? BB 04 00 00 00 89 9E ?? ?? 00 00 E9 ?? ?? ?? ?? BB 01 00 "
    "00 00 89 9E ?? ?? 00 00 E9 ?? ?? ?? ?? 81 E9 ?? ?? 00 00 74 25 83 F9 01 74 10 BB 05 00 00 00 89 "
    "9E ?? ?? 00 00 E9 ?? ?? ?? ?? BB 03 00 00 00 89 9E ?? ?? 00 00 E9 ?? ?? ?? ?? BB 02 00 00 00 89 "
    "9E ?? ?? 00 00";
}
