#include "../src/interface/starbase/starbasetemplates_native.h"
#include "../src/interface/starbase/starbasetemplates_ui_patterns.h"
#include "../src/interface/starbase/shipyardreinforce_patterns.h"
#include <cstdio>
int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: starbasetemplates_native_test executable\n");
        return 1;
    }
    HMODULE module = LoadLibraryExA(argv[1], nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!module)
        return 1;
    auto* base = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(module) & ~uintptr_t(3));
    auto image = locate::Describe(base);
    starbasetemplates::SelectionLocations selection;
    std::string error;
    bool ok = starbasetemplates::LocateSelection(image, selection, error);
    starbasetemplates::ConstructionLocations construction;
    ok = ok && starbasetemplates::LocateConstruction(image, construction, error);
    starbasetemplates::StateLocations state;
    ok = ok && starbasetemplates::LocateState(image, state, error);
    starbasetemplates::PlatformUpgradeLocations platforms;
    ok = ok && starbasetemplates::LocatePlatformUpgrade(image, selection, construction, platforms, error);
    if (ok)
        std::printf("platform upgrade vtable=%zx\n", reinterpret_cast<uint8_t*>(platforms.vtable) - base);
    shipyardreinforce::Locations reinforce;
    ok = ok && shipyardreinforce::Locate(image, construction.mainQueue, reinforce, error);
    if (ok) {
        std::printf(
            "reinforce: build locations=%zx ships queue=%zx checkbox factory=%zx set state=%zx (state +%zx) text=%zx\n",
            reinforce.calcBuildLocations - base, reinforce.shipsQueueRef - base, reinforce.checkBoxFactory - base,
            reinforce.checkBoxSetState - base, reinforce.checkBoxState, reinforce.textBoxInstantiate - base);
        ok = reinforce.checkBoxState == 0x118;
    }
    for (const auto* pattern :
         {starbasetemplates::kButtonText, starbasetemplates::kButtonSize, starbasetemplates::kViewUpdate,
          starbasetemplates::kWaystationViewUpdate, starbasetemplates::kIsDesignable,
          starbasetemplates::kSideShowForSlot, starbasetemplates::kSideHide, starbasetemplates::kSideClear,
          starbasetemplates::kTextChange, starbasetemplates::kButtonMoveTo, starbasetemplates::kButtonCentre,
          starbasetemplates::kWindowGetSize, starbasetemplates::kButtonSetAlpha, starbasetemplates::kButtonSetModulate})
        ok = ok && locate::FindOne(image, "starbase UI", pattern, error);
    if (ok) {
        std::printf("arrays=%zx/%zx %zx/%zx level=%zx next=%zx queue=%zx/%zx item=%zx definitions=%zx/%zx\n",
                    state.modules, state.moduleCount, state.buildings, state.buildingCount, state.level,
                    state.nextLevel, state.queueData, state.queueCount, state.buildable, state.definitionData,
                    state.definitionCount);
        ok = state.modules == 0x5e0 && state.moduleCount == 0x5ec && state.buildings == 0x5f8 &&
             state.buildingCount == 0x604 && state.level == 0x890 && state.nextLevel == 0x100 &&
             state.queueData == 0x20 && state.queueCount == 0x2c && state.buildable == 0x18 &&
             state.definitionData == 0x50 && state.definitionCount == 0x5c;
    }
    starbasetemplates::IconLocations icons;
    ok = ok && starbasetemplates::LocateIcons(image, icons, error);
    if (ok) {
        std::printf("icons: apply=%zx keep scale=%zx set scale slot=%zx\n", icons.apply - base, icons.keepScale - base,
                    icons.setScaleSlot);
        ok = icons.setScaleSlot == 0x340; // CButtonStandard::SetScale(float) in the GOG 4.5.2 PDB
    }
    starbasetemplates::EditorLocations editor;
    ok = ok && starbasetemplates::LocateEditor(image, state, editor, error);
    if (ok) {
        std::printf("editor: capacity=%zx/%zx level slots=%zx/%zx name=%zx component type=%zx\n", editor.moduleCapacity,
                    editor.buildingCapacity, editor.levelModuleSlots, editor.levelBuildingSlots,
                    editor.componentName ? editor.componentName - base : size_t(0), editor.componentType);
        // The level's lists sit just before _pNextLevel (PDB); the capacity pair moves with the build.
        // _StarbaseType is 0x2d8 in the 4.5.2 GOG PDB; the others agree.
        ok = editor.levelModuleSlots == 0xdc && editor.levelBuildingSlots == 0xf4 && editor.componentName &&
             editor.componentType == 0x2d8 && editor.moduleCapacity > state.buildingCount &&
             editor.moduleCapacity < 0x4000;
    }
    if (ok) {
        ok = construction.mainQueue == 0x880;
        std::printf("module=%zx building=%zx upgrade=%zx queue=%zx\n", construction.module - base,
                    construction.building - base, construction.upgrade - base, construction.mainQueue);
    }
    if (ok) {
        // PDB-confirmed layout shared by these tested builds. Runtime uses decoded offsets.
        ok = selection.selectedReference == 0xa8 && selection.objectReference == 8;
        bool code = false, db = false, mappedState = false;
        for (const auto& section : image.code)
            code |= selection.observedCountry >= section.begin && selection.observedCountry < section.end;
        for (const auto& section : image.data) {
            auto* d = reinterpret_cast<uint8_t*>(selection.starbaseDatabase);
            auto* s = reinterpret_cast<uint8_t*>(selection.gameState);
            db |= d >= section.begin && d + 8 <= section.end;
            mappedState |= s >= section.begin && s + 8 <= section.end;
        }
        ok &= code && db && mappedState;
        std::printf("selected=%zx id=%zx database=%zx state=%zx country=%zx\n", selection.selectedReference,
                    selection.objectReference, reinterpret_cast<uint8_t*>(selection.starbaseDatabase) - base,
                    reinterpret_cast<uint8_t*>(selection.gameState) - base, selection.observedCountry - base);
    }
    FreeLibrary(module);
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", error.c_str());
    return ok ? 0 : 1;
}
