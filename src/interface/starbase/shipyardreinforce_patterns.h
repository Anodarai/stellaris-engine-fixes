// Locations for the shipyard reinforcement checkbox (src/shipyardreinforce.h). Each pattern matches
// once in the Steam and GOG 4.5.2, 4.5.3 and 4.5.4 executables (the queue getter twice: it has a
// twin for the main queue, told apart by the starbase field it reads).
#pragma once
#include "../../core/locate.h"
#include "../uishortcuts.h"

namespace shipyardreinforce {
// CFleetTemplateManager::CalcBuildLocations(this, CPdxArray<pair<int, const CConstructionQueue*>>& out,
// const CPdxArray<SPerQueueCacheData>& queues, const CFleet*, const CPdxArray<CShipDesignImplementation>&)
// (GOG PDB): picks the shipyards a reinforcement may build at from the country's queues. Of `queues`
// it reads only the data pointer (+8) and the count (+0x14, entries of 16 bytes: `shl rax,4`). The
// first three instructions spill r9, r8 and rdx (15 bytes, no relative operands).
inline constexpr char kCalcBuildLocations[] =
    "4C 89 4C 24 20 4C 89 44 24 18 48 89 54 24 10 48 89 4C 24 08 55 53 56 57 41 54 41 55 41 56 41 57 "
    "B8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 2B E0 48 8D 6C 24 30 0F 29 B5 ?? ?? ?? ?? 4D 8B F9 4D 8B F0 4C 8B E2 "
    "48 8B F1 49 63 58 14 48 8B C3 48 C1 E0 04 4C 8D 50 0F 4C 3B D0 77 0A 49 BA F0 FF FF FF FF FF FF 0F 49 83 E2 F0";
inline constexpr size_t kCalcBuildLocationsStolen = 15;
inline constexpr size_t kQueuesData = 8, kQueuesCount = 0x14, kQueuesHeader = 0x18;

// CStarbase::GetShipsBuildQueueRef(TPdxRef<CConstructionQueue>* out) (GOG PDB): the station ship's own
// queue (+0x78) for a station whose design says so, otherwise the starbase's _ShipsBuildQueue (+0x82).
// GetMainBuildQueueRef is the same code reading _MainBuildQueue, four bytes earlier.
inline constexpr char kShipsQueueRef[] =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B FA 48 8B F1 48 8B 15 ?? ?? ?? ?? 48 85 D2 74 2D "
    "44 8B 81 ?? ?? ?? ?? 41 8B C0 25 FF FF FF 00 3B 42 20 73 19 8B C8 48 8B 42 18 48 03 C9 48 8B 5C C8 08 "
    "48 85 DB 74 06 44 39 43 ?? 74 07 48 8B 1D ?? ?? ?? ?? 48 8B 03 48 8B CB FF 50 20 84 C0 74 1E "
    "48 8B 83 ?? ?? ?? ?? 8B 88 ?? ?? ?? ?? 48 C1 E9 12 F6 C1 01 74 08 8B 83 ?? ?? ?? ?? EB 06 "
    "8B 86 ?? ?? ?? ?? 48 8B 5C 24 30 48 8B 74 24 38 89 07 48 8B C7 48 83 C4 20 5F C3";
inline constexpr size_t kShipsQueueField = 0x82;

// CButtonType::InstansiateCheckBox(CGuiGraphics&, CGui&, C2dObject*, CVector2<int>) (GOG PDB; the
// button type's vftable +0x70, beside InstantiateStandardButton at +0x68). It places the checkbox at the
// type's _Position (+0x530/+0x534) for its _Orientation (+0x544).
inline constexpr char kCheckBoxFactory[] =
    "48 89 5C 24 20 4C 89 44 24 18 55 56 57 41 54 41 55 41 56 41 57 48 81 EC ?? ?? ?? ?? 0F 29 B4 24 ?? ?? ?? ?? "
    "4D 8B E9 4C 8B E2 4C 8B F9 66 0F 6E 72 38 0F 5B F6 F3 0F 59 B1 38 05 00 00 0F 57 C0 0F 2F C6 76 0A "
    "F3 0F 5C 35 ?? ?? ?? ?? EB 08 F3 0F 58 35 ?? ?? ?? ?? B9 80 02 00 00 E8 ?? ?? ?? ?? 4C 8B F0 "
    "48 89 84 24 ?? ?? ?? ?? 41 8B AF 44 05 00 00";
inline constexpr char kCheckBoxFactoryPosition[] = "41 03 97 34 05 00 00 41 8B 8F 30 05 00 00";
inline constexpr size_t kCheckBoxFactorySlot = 0x70;
inline constexpr size_t kButtonTypePosition = 0x530, kButtonTypeOrientation = 0x544, kButtonTypeCentred = 0x64b;

// CCheckBox::SetCheckedWithoutNotify(bool) (GOG PDB): stores _state (+0x118: 0 unchecked, 1 checked),
// then tail-calls ActivateCurrentState, which picks the sprite frame for it.
inline constexpr char kCheckBoxSetState[] = "0F B6 C2 89 81 ?? ?? ?? ?? E9";
inline constexpr char kCheckBoxActivate[] =
    "40 53 48 83 EC 30 80 B9 5A 02 00 00 00 48 8B D9 75 1D 48 8B 89 28 01 00 00 48 8B 01 FF 90 C0 02 00 00";

// CInstantTextBoxType::Instantiate(CGui&, C2dObject*, CVector2<int>) (GOG PDB). Its literal offsets pin
// the type's _Position (+0x1f8); _nMaxWidth (+0x1ea) and _Orientation (+0x210) are from the same PDB
// type. CGui::CreateInstantTextBox (CGui vftable +0x80) calls it at +0x26, then AddGuiObject (+0x98).
inline constexpr char kTextBoxInstantiate[] =
    "48 89 5C 24 18 4C 89 4C 24 20 55 56 57 48 81 EC ?? ?? ?? ?? 49 8B E8 48 8B F2 48 8B F9 48 8B 82 08 01 00 00 "
    "66 0F 6E 40 38 0F 5B C0 0F 28 C8 F3 0F 59 89 00 02 00 00 0F 57 DB F3 0F 10 15 ?? ?? ?? ?? 0F 2F D9 76 06 "
    "F3 0F 5C CA EB 04 F3 0F 58 CA F3 0F 2C C9 F3 0F 59 87 F0 01 00 00 0F 2F D8 76 06 F3 0F 5C C2 EB 04 "
    "F3 0F 58 C2 F3 0F 2C C0 8B 9F F4 01 00 00 3B C3 0F 4C D8 03 8F FC 01 00 00 8B 87 F8 01 00 00";
inline constexpr size_t kTextTypePosition = 0x1f8, kTextTypeMaxWidth = 0x1ea, kTextTypeOrientation = 0x210;
inline constexpr size_t kGuiCreateTextBoxSlot = 0x80, kGuiCreateTextBoxCall = 0x26;

// CContainerWindow's child arrays (GOG PDB): one per kind, beside a parallel array of names.
inline constexpr size_t kWindowCheckBoxes = 0x780, kWindowCheckBoxNames = 0x798;
inline constexpr size_t kWindowTextBoxes = 0x6f0, kWindowTextBoxNames = 0x708;
// CContainerWindowType's lists of child names: _InstantTextBoxes (+0x1b0) and _CheckBoxes (+0x240).
inline constexpr size_t kWindowTypeTextBoxes = 0x1b0, kWindowTypeCheckBoxes = 0x240;
// CCheckBox: the observable (+0xe0) takes observers like a button's (vftable +8); clicks arrive at the
// observer's slot 1, after the box has toggled itself.
inline constexpr size_t kCheckBoxObservable = 0xe0;

struct Locations {
    uint8_t* calcBuildLocations = nullptr;
    uint8_t* shipsQueueRef = nullptr;
    uint8_t* checkBoxFactory = nullptr;
    uint8_t* checkBoxSetState = nullptr;
    uint8_t* textBoxInstantiate = nullptr;
    size_t checkBoxState = 0;
};

// `mainQueue` is CStarbase::_MainBuildQueue as the starbase upgrade command reads it; the ships
// queue is the next field.
inline bool Locate(const locate::Image& image, size_t mainQueue, Locations& out, std::string& error) {
    Locations found;
    found.calcBuildLocations =
        locate::FindOne(image, "CFleetTemplateManager::CalcBuildLocations", kCalcBuildLocations, error);
    found.checkBoxFactory = locate::FindOne(image, "CButtonType::InstansiateCheckBox", kCheckBoxFactory, error);
    found.checkBoxSetState = locate::FindOne(image, "CCheckBox::SetCheckedWithoutNotify", kCheckBoxSetState, error);
    auto* activate = locate::FindOne(image, "CCheckBox::ActivateCurrentState", kCheckBoxActivate, error);
    found.textBoxInstantiate = locate::FindOne(image, "CInstantTextBoxType::Instantiate", kTextBoxInstantiate, error);
    if (!found.calcBuildLocations || !found.checkBoxFactory || !found.checkBoxSetState || !activate ||
        !found.textBoxInstantiate)
        return false;
    if (!uishortcuts::InFunction(found.checkBoxFactory, 0x140, kCheckBoxFactoryPosition)) {
        error = "checkbox position fields not found";
        return false;
    }
    found.checkBoxState = static_cast<size_t>(locate::Disp32(found.checkBoxSetState + 5));
    if (locate::RelTarget(found.checkBoxSetState + 10, found.checkBoxSetState + 14) != activate ||
        found.checkBoxState < 0x100 || found.checkBoxState > 0x200) {
        error = "CCheckBox::SetCheckedWithoutNotify cross-check failed";
        return false;
    }
    const auto pattern = locate::Parse(kShipsQueueRef);
    size_t matches = 0;
    for (const auto& section : image.code) {
        for (auto* p : locate::FindAll(section.begin, section.end, pattern, 3)) {
            ++matches;
            if (static_cast<size_t>(locate::Disp32(p + kShipsQueueField)) == mainQueue + 4) {
                if (found.shipsQueueRef) {
                    error = "two ships queue getters";
                    return false;
                }
                found.shipsQueueRef = p;
            }
        }
    }
    if (matches != 2 || !found.shipsQueueRef) {
        error = "CStarbase::GetShipsBuildQueueRef not found (" + std::to_string(matches) + " candidates)";
        return false;
    }
    out = found;
    return true;
}
} // namespace shipyardreinforce
