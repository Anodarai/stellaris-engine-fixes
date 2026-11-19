// Ship designer functions and fields for the ship design library (src/shipdesignlibrary.h). Names and
// layouts are from the GOG 4.5.2 PDB; each pattern matches once in the Steam and GOG 4.5.2, 4.5.3 and
// 4.5.4 executables. Evidence: re/ship_design_library.md.
#pragma once
#include "../../core/locate.h"
#include "../uishortcuts.h"

namespace shipdesignlibrary {
// CSelectorItemShipSize::OnClick, the hull picker's "new design of this size" branch:
//   CShipDesign d;  d._pGfxCulture (+0x200) = item+0x140;  d.CreateGrowthStages(item+0x148 size);
//   working = d (operator=);  ~d;
//   LoadDesign(designer, working, g_pIdler+0x160 ? 3 : designer->_DesignOwner (+0xb0), false);
//   then every view in designer+0x1d60..+0x1d88 is hidden (vftable +0xe8).
inline constexpr char kNewDesign[] =
    "48 8D 4C 24 60 E8 ?? ?? ?? ?? C7 85 ?? ?? ?? ?? 01 00 00 00 48 8B 83 40 01 00 00 48 89 85 ?? ?? ?? ?? "
    "48 8B 93 48 01 00 00 48 8D 4C 24 60 E8 ?? ?? ?? ?? 48 8D 54 24 60 48 8D 8D ?? ?? ?? ?? E8 ?? ?? ?? ?? 90 "
    "48 8D 4C 24 60 E8 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 83 B8 60 01 00 00 00 41 B8 03 00 00 00 75 07 "
    "44 8B 86 B0 00 00 00 45 33 C9 48 8D 95 ?? ?? ?? ?? 48 8B CE E8 ?? ?? ?? ?? 48 8D 9E 60 1D 00 00 48 8D BE 88 1D 00 00";
inline constexpr size_t kNewDesignCtor = 6, kNewDesignGrowth = 0x2f, kNewDesignDtor = 0x4b, kNewDesignIdler = 0x52,
                        kNewDesignLoad = 0x7a;
// The tail of CShipDesignerBase::LoadDesign: for a new design (id +0x10 == -1) it shows the role
// picker with SetRoleSelectionShown(designer, true). Picking a role there generates a whole design
// for that role, so after loading a library design the DLL hides the picker again.
inline constexpr char kLoadRolePicker[] = "41 83 7F 10 FF 75 0A B2 01 48 8B CE E8";
inline constexpr size_t kLoadDesignLength = 0x300;

// CShipDesignerBase::ComponentIsAllowedOnSlot(designer, const CComponentTemplate*, const CComponentSlot*,
// CString* reason): slot size and type, then NShipDesignUtil::CanBuildComponent(component,
// designer->_DesignOwner, AccessCurrentDesign(designer), slot, reason), which tests technology,
// potential/possible triggers and limits and writes the game's own reason.
inline constexpr char kComponentAllowed[] =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 41 0F B6 80 94 00 00 00 49 8B E9 49 8B D8 "
    "48 8B FA 48 8B F1 3A 82 E0 01 00 00 74 04 3C 0B 75 14 41 0F B6 80 95 00 00 00 3A 82 E1 01 00 00 74 08 "
    "3C 04 74 04 32 C0 EB 32 48 8B 05 ?? ?? ?? ?? 83 B8 60 01 00 00 00 75 20 E8 ?? ?? ?? ?? "
    "8B 96 B0 00 00 00 4C 8B CB 4C 8B C0 48 89 6C 24 20 48 8B CF E8";
inline constexpr size_t kAllowedIdler = 0x4f, kAllowedStage = 0x5d, kAllowedCanBuild = 118;

// CShipDesignerBase::SetComponentOnSlot(designer, int stage, const CComponentTemplate*, const
// CComponentSlot*, int sectionSlot): SDataUnderModification::GetGrowthStage(designer+0xd8, stage) (rdx passes
// through), then the stage's section whose slot hash (+0x38) is the
// ship size's _SectionSlots[sectionSlot] hash (+0x868 array, data +0x870, 0x50 each, hash +0x30), then
// CShipDesignSection::SetComponentOnSlot(section, component, slot), CShipGrowthStage::UpdateResources
// and CBaseDesignImplementation::RecalcSpecialValues(designer+0xd8), and plays a sound. The DLL calls
// the three itself, without the sound per component.
inline constexpr char kSetComponent[] =
    "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 41 56 41 57 48 8B EC 48 83 EC 50 4D 8B F9 49 8B F8 "
    "48 8B D9 48 81 C1 D8 00 00 00 E8 ?? ?? ?? ?? 48 8B F0 4C 8B 50 08 48 63 55 40 48 8D 0C 92 48 C1 E1 04 "
    "49 03 8A 70 08 00 00 45 33 C0 48 63 50 24 85 D2 0F 8E ?? ?? ?? ?? 4C 8B 58 18 44 8B 51 30 4C 8B CA "
    "33 D2 49 8B CB 0F 1F 80 00 00 00 00 48 8B 01 44 39 50 38 74 14 41 FF C0 48 FF C2 48 83 C1 08 49 3B D1 "
    "7C E8 E9 ?? ?? ?? ?? 41 83 F8 FF 0F 84 ?? ?? ?? ?? 49 63 C0 4D 8B C7 48 8B D7 49 8B 0C C3 "
    "E8 ?? ?? ?? ?? 48 8B CE E8 ?? ?? ?? ?? 48 8D 8B D8 00 00 00 E8 ?? ?? ?? ?? 48 8B 87 48 08 00 00";
inline constexpr size_t kSetComponentStage = 0x2c, kSetComponentSection = 0xa5, kSetComponentResources = 0xad,
                        kSetComponentRecalc = 0xb9;

// CShipDesigner::SetSectionTemplateOnSlot(designer, CShipGrowthStage&, const CSectionTemplate*, const
// SSectionSlotID&): a new, empty CShipDesignSection (operator new(0x60)) replaces the stage's section
// on that slot or is appended; then a sound, UpdateResources, RecalcSpecialValues and the designer's
// outdated UI segments (+0xbf0) = 6.
inline constexpr char kSetSection[] =
    "48 89 5C 24 18 48 89 4C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 70 4D 8B E9 49 8B D8 "
    "4C 8B E1 4C 8D 72 10 B9 60 00 00 00 E8 ?? ?? ?? ?? 48 8B F0 48 89 45 ?? 48 8D 05 ?? ?? ?? ?? 48 89 06 "
    "48 8D 4E 08 49 8B D5 E8 ?? ?? ?? ?? 48 89 5E 40 48 8D 05 ?? ?? ?? ?? 48 89 46 48";

// CPopupMessageOK::CPopupMessageOK(this, const CString& title, const CString& text): the window
// "ok_popup_window". The object is 0x358 bytes; it is shown with CGameIdler::AddUpdateable.
inline constexpr char kPopupOk[] =
    "48 8B C4 48 89 58 10 48 89 70 18 48 89 48 08 57 48 83 EC 60 49 8B F8 48 8B F2 48 8B D9 C7 40 C8 00 00 00 00 "
    "48 C7 40 D0 00 00 00 00 48 C7 40 F0 0F 00 00 00 48 C7 40 E8 0F 00 00 00 F2 0F 10 05 ?? ?? ?? ?? "
    "F2 0F 11 40 D8 8B 05 ?? ?? ?? ?? 89 44 24 48 0F B7 05";
inline constexpr size_t kPopupSize = 0x358;

// CSectionTemplate::HasPrerequisites(const CCountry*) (the inlined CTechnologyPrereqNodeAnd::IsValidFor
// over _Prereqs +0x218: nodes +0x228, count +0x234). The designer offers a section only when this holds.
inline constexpr char kSectionPrereqs[] =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 50 48 8B F2 48 8B C1 83 B9 34 02 00 00 00 74 ?? 33 C9 "
    "89 4C 24 20 0F 57 C0 F3 0F 7F 44 24 28 66 0F 6F 05 ?? ?? ?? ?? F3 0F 7F 44 24 40 88 4C 24 30 "
    "48 8B 98 28 02 00 00 48 63 80 34 02 00 00";

// CShipDesign::CreateGrowthStages(design, const CShipSize*): reads TGameDatabase<CShipSizeDatabase>::
// _pInstance (data +0x50, count +0x5c). Must be the function kNewDesign calls.
inline constexpr char kCreateGrowthStages[] =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 8B 82 C8 00 00 00 48 8B DA 48 C1 E8 13 48 8B F1 A8 01 74 ?? "
    "48 8B 3D ?? ?? ?? ?? 4C 63 57 5C";
inline constexpr size_t kGrowthSizeDb = 0x26;

// CSectionInstanceEntry::ChangeSection (+0x23 into the function): GetGrowthStage(designer+0xd8, entry's stage),
// then the loop over CShipDesignTemplatesDatabase::_pInstance's _SectionTemplates (data +8, count +0x14).
inline constexpr char kChangeSection[] =
    "48 8B 59 78 8B 91 ?? ?? ?? ?? 48 81 C3 D8 00 00 00 89 93 10 0B 00 00 48 8B 03 48 8B CB FF 50 10 8B 48 2C "
    "FF C9 8B 83 10 0B 00 00 85 C0 79 0E C7 83 10 0B 00 00 00 00 00 00 33 C0 EB 0C 3B C1 7E 08 89 8B 10 0B 00 00 "
    "8B C1 89 83 2C 05 00 00 49 8B 4C 24 78 48 81 C1 D8 00 00 00 41 8B 94 24 ?? ?? ?? ?? E8 ?? ?? ?? ?? "
    "4C 8B 68 08 48 8D 05 ?? ?? ?? ?? 48 89 45 ?? 48 C7 45 ?? 00 00 00 00 48 C7 45 ?? 00 00 00 00 33 DB "
    "48 8B 05 ?? ?? ?? ?? 4C 8B 78 08 48 63 40 14";
inline constexpr size_t kChangeSectionStage = 0x64, kChangeSectionTemplates = 0x8c;

// Fields (GOG PDB, CShipDesignerBase and the design types; Steam and GOG share them).
inline constexpr size_t kDesignerType = 0xa4;        // _ShipDesignerType: 0 ships, 1 creatures
inline constexpr size_t kDesignerOwner = 0xb0;       // _DesignOwner (in kNewDesign and kComponentAllowed)
inline constexpr size_t kDesignerGfxCulture = 0xb8;  // _pGfxCulture (item+0x140 is copied from it)
inline constexpr size_t kDesignerData = 0xd8;        // _DataUnderModification; vftable +0x10 returns the CShipDesign
inline constexpr size_t kDesignerStageIndex = 0x604; // _BeingModified._GrowthStage
inline constexpr size_t kDesignerOutdated = 0xbf0;   // _OutdatedSegments
inline constexpr size_t kDesignerViewsBegin = 0x1d60, kDesignerViewsEnd = 0x1d88, kViewHideSlot = 0xe8;
inline constexpr size_t kIdlerAllComponents = 0x160;
inline constexpr size_t kDesignSize = 0x208, kDesignId = 0x10, kDesignStages = 0x20, kDesignStageCount = 0x2c;
inline constexpr size_t kDesignNameKey = 0x40, kDesignNameLiteral = 0x70, kDesignName = 0x38;
inline constexpr size_t kDesignRole = 0x130, kDesignGfxCulture = 0x200, kStageSize = 0x110;
inline constexpr size_t kStageShipSize = 8, kStageSections = 0x18, kStageSectionCount = 0x24;
inline constexpr size_t kStageRequired = 0x30, kStageRequiredCount = 0x3c;
inline constexpr size_t kSectionSlotKey = 0x8, kSectionSlotHash = 0x38, kSectionTemplate = 0x40;
inline constexpr size_t kSectionComponents = 0x50, kSectionComponentCount = 0x5c, kComponentEntrySize = 0x20;
inline constexpr size_t kEntrySlot = 0x8, kEntryTemplate = 0x10;
inline constexpr size_t kComponentSlotKey = 0x8, kComponentSlotSize = 0x98;
inline constexpr size_t kComponentKey = 0x1a0, kComponentSet = 0x848;
inline constexpr size_t kSectionKey = 0x138, kSectionSlots = 0x178, kSectionSlotCount = 0x184;
inline constexpr size_t kSectionSizes = 0x1e8, kSectionSizeCount = 0x1f4, kSectionAllSizes = 0x1f8;
inline constexpr size_t kSectionFits = 0x208, kSectionFitCount = 0x214, kSlotIdSize = 0x38, kSlotIdHash = 0x30;
inline constexpr size_t kShipSizeKey = 0x10, kShipSizeSlots = 0x870, kShipSizeSlotCount = 0x87c,
                        kShipSizeSlotStride = 0x50;
inline constexpr size_t kShipSizeRequiredSets = 0x8c8, kShipSizeRequiredSetCount = 0x8d4;
inline constexpr size_t kShipSizeDbData = 0x50, kShipSizeDbCount = 0x5c;
inline constexpr size_t kSetKey = 0x8, kSetComponents = 0xb8, kSetComponentCount = 0xc4;
// CShipDesignTemplatesDatabase: _SectionTemplates, _ComponentSets, _Weapon/_Utility/_StrikeCraft
// component templates; each a CPdxArray (data +8, count +0x14).
inline constexpr size_t kTemplatesSections = 0x0, kTemplatesSets = 0x18;
inline constexpr size_t kTemplatesComponentArrays[] = {0x30, 0x48, 0x60};

struct Locations {
    uint8_t* designCtor = nullptr; // CShipDesign::CShipDesign(this)
    uint8_t* designDtor = nullptr; // CShipDesign::~CShipDesign(this)
    uint8_t* createGrowthStages = nullptr;
    uint8_t* loadDesign = nullptr;          // CShipDesignerBase::LoadDesign(designer, const CShipDesign&, owner, bool)
    uint8_t* rolePicker = nullptr;          // CShipDesignerBase::SetRoleSelectionShown(designer, bool)
    uint8_t** idler = nullptr;              // &g_pCurrentIdler
    uint8_t* allowed = nullptr;             // CShipDesignerBase::ComponentIsAllowedOnSlot
    uint8_t* canBuild = nullptr;            // NShipDesignUtil::CanBuildComponent
    uint8_t* growthStage = nullptr;         // SDataUnderModification::GetGrowthStage(designer+0xd8, int index)
    uint8_t* sectionSetComponent = nullptr; // CShipDesignSection::SetComponentOnSlot
    uint8_t* updateResources = nullptr;     // CShipGrowthStage::UpdateResources
    uint8_t* recalc = nullptr;              // CBaseDesignImplementation::RecalcSpecialValues
    uint8_t* setSection = nullptr;          // CShipDesigner::SetSectionTemplateOnSlot
    uint8_t* popupOk = nullptr;             // CPopupMessageOK::CPopupMessageOK
    uint8_t* sectionPrereqs = nullptr;      // CSectionTemplate::HasPrerequisites
    uint8_t** shipSizes = nullptr;          // &TGameDatabase<CShipSizeDatabase>::_pInstance
    uint8_t** templates = nullptr;          // &CShipDesignTemplatesDatabase::_pInstance
};

inline bool Locate(const locate::Image& image, Locations& out, std::string& error) {
    using locate::RelTarget;
    auto call = [](uint8_t* p, size_t at) { return RelTarget(p + at, p + at + 4); };
    auto global = [](uint8_t* p, size_t at) { return reinterpret_cast<uint8_t**>(RelTarget(p + at, p + at + 4)); };
    Locations f;
    auto* newDesign = locate::FindOne(image, "CSelectorItemShipSize::OnClick new design", kNewDesign, error);
    auto* allowed =
        newDesign ? locate::FindOne(image, "CShipDesignerBase::ComponentIsAllowedOnSlot", kComponentAllowed, error)
                  : nullptr;
    auto* setComponent =
        allowed ? locate::FindOne(image, "CShipDesignerBase::SetComponentOnSlot", kSetComponent, error) : nullptr;
    f.setSection =
        setComponent ? locate::FindOne(image, "CShipDesigner::SetSectionTemplateOnSlot", kSetSection, error) : nullptr;
    f.popupOk = f.setSection ? locate::FindOne(image, "CPopupMessageOK::CPopupMessageOK", kPopupOk, error) : nullptr;
    f.sectionPrereqs =
        f.popupOk ? locate::FindOne(image, "CSectionTemplate::HasPrerequisites", kSectionPrereqs, error) : nullptr;
    auto* growth = f.sectionPrereqs
                       ? locate::FindOne(image, "CShipDesign::CreateGrowthStages", kCreateGrowthStages, error)
                       : nullptr;
    auto* change =
        growth ? locate::FindOne(image, "CSectionInstanceEntry::ChangeSection", kChangeSection, error) : nullptr;
    if (!change)
        return false;
    f.designCtor = call(newDesign, kNewDesignCtor);
    f.createGrowthStages = call(newDesign, kNewDesignGrowth);
    f.designDtor = call(newDesign, kNewDesignDtor);
    f.idler = global(newDesign, kNewDesignIdler);
    f.loadDesign = call(newDesign, kNewDesignLoad);
    f.allowed = allowed;
    f.canBuild = call(allowed, kAllowedCanBuild);
    f.growthStage = call(setComponent, kSetComponentStage);
    f.sectionSetComponent = call(setComponent, kSetComponentSection);
    f.updateResources = call(setComponent, kSetComponentResources);
    f.recalc = call(setComponent, kSetComponentRecalc);
    f.shipSizes = global(growth, kGrowthSizeDb);
    f.templates = global(change, kChangeSectionTemplates);
    auto* role = uishortcuts::InFunction(f.loadDesign, kLoadDesignLength, kLoadRolePicker);
    if (!role) {
        error = "CShipDesignerBase::LoadDesign role picker call not found";
        return false;
    }
    f.rolePicker = call(role, 13);
    // Cross-checks: the same idler, the same stage getter, the growth function that OnClick calls.
    if (f.createGrowthStages != growth || global(allowed, kAllowedIdler) != f.idler ||
        call(change, kChangeSectionStage) != f.growthStage) {
        error = "ship designer cross-checks failed";
        return false;
    }
    for (auto* p : {f.designCtor, f.designDtor, f.loadDesign, f.rolePicker, f.canBuild, f.growthStage,
                    f.sectionSetComponent, f.updateResources, f.recalc}) {
        if (!uishortcuts::InCode(image, p, 16)) {
            error = "ship designer call target outside code";
            return false;
        }
    }
    out = f;
    return true;
}
} // namespace shipdesignlibrary
