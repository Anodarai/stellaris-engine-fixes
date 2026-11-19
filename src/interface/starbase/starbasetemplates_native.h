#pragma once
#include "../../core/locate.h"
#include "../uishortcuts.h"
#include "starbasetemplates_patterns.h"

namespace starbasetemplates {
struct ConstructionLocations {
    uint8_t* module = nullptr;
    uint8_t* building = nullptr;
    uint8_t* upgrade = nullptr;
    uint8_t* allocate = nullptr;
    uint8_t* post = nullptr;
    uintptr_t* commandVtable = nullptr;
    uintptr_t* moduleVtable = nullptr;
    uintptr_t* buildingVtable = nullptr;
    uintptr_t* upgradeVtable = nullptr;
    size_t mainQueue = 0;
};
inline bool LocateConstruction(const locate::Image& image, ConstructionLocations& out, std::string& error) {
    ConstructionLocations found;
    auto* moduleFinished = locate::FindOne(image, "module completion", kModuleFinished, error);
    auto* buildingFinished = locate::FindOne(image, "building completion", kBuildingFinished, error);
    found.upgrade = locate::FindOne(image, "starbase upgrade command", kUpgradeCommand, error);
    found.post = locate::FindOne(image, "starbase command posting", uishortcuts::kPostCommand, error);
    if (!moduleFinished || !buildingFinished || !found.upgrade || !found.post)
        return false;
    found.upgradeVtable = reinterpret_cast<uintptr_t*>(locate::RelTarget(found.upgrade + 0xb3, found.upgrade + 0xb7));
    found.commandVtable = reinterpret_cast<uintptr_t*>(locate::RelTarget(found.upgrade + 0xd0, found.upgrade + 0xd4));
    found.allocate = locate::RelTarget(found.upgrade + 0xa7, found.upgrade + 0xab);
    found.mainQueue = locate::Disp32(found.upgrade + 0x90);
    size_t matches = 0;
    const auto pattern = locate::Parse(kComponentCommand);
    for (const auto& section : image.code) {
        for (auto* p : locate::FindAll(section.begin, section.end, pattern, 3)) {
            ++matches;
            auto* table = reinterpret_cast<uintptr_t*>(locate::RelTarget(p + 0xdd, p + 0xe1));
            auto* completion = uishortcuts::SlotTarget(image, table + 4);
            if (locate::RelTarget(p + 0xe7, p + 0xeb) != reinterpret_cast<uint8_t*>(found.commandVtable) ||
                locate::RelTarget(p + 0xac, p + 0xb0) != found.allocate) {
                error = "starbase command constructor cross-check failed";
                return false;
            }
            if (completion == moduleFinished && !found.module) {
                found.module = p;
                found.moduleVtable = table;
            } else if (completion == buildingFinished && !found.building) {
                found.building = p;
                found.buildingVtable = table;
            } else {
                error = "unrecognized starbase component constructor";
                return false;
            }
        }
    }
    if (matches != 2 || !found.module || !found.building || !uishortcuts::SlotTarget(image, found.upgradeVtable) ||
        !uishortcuts::SlotTarget(image, found.commandVtable)) {
        error = "incomplete starbase construction locations";
        return false;
    }
    out = found;
    return true;
}
// Discovered from CStarbaseView::UpdateSpecialized. All displacements are decoded
// from the executable; none of these addresses are persisted in template data.
struct SelectionLocations {
    uint8_t** starbaseDatabase = nullptr;
    uint8_t** gameState = nullptr;
    uint8_t* observedCountry = nullptr;
    size_t selectedReference = 0;
    size_t objectReference = 0;
};
struct StateLocations {
    uint8_t* owner = nullptr;
    uint8_t* occupation = nullptr;
    uint8_t** queues = nullptr;
    uint8_t** queueItems = nullptr;
    uint8_t** definitions[3]{}; // upgrade, module, building
    size_t modules = 0, moduleCount = 0, buildings = 0, buildingCount = 0;
    size_t level = 0, nextLevel = 0;
    size_t queueData = 0, queueCount = 0, buildable = 0;
    size_t definitionData = 0, definitionCount = 0;
};
inline bool LocateState(const locate::Image& image, StateLocations& out, std::string& error) {
    StateLocations s;
    s.owner = locate::FindOne(image, "starbase owner", kOwner, error);
    s.occupation = locate::FindOne(image, "starbase occupation", kOccupation, error);
    auto* walk = locate::FindOne(image, "starbase upgrade queue traversal", kQueueWalk, error);
    auto* upgrade = locate::FindOne(image, "starbase upgrade validation", kCanUpgrade, error);
    auto* module = locate::FindOne(image, "module deserializer", kReadModule, error);
    auto* building = locate::FindOne(image, "building deserializer", kReadBuilding, error);
    auto* level = locate::FindOne(image, "level deserializer", kReadLevel, error);
    if (!s.owner || !s.occupation || !walk || !upgrade || !module || !building || !level)
        return false;
    s.queues = reinterpret_cast<uint8_t**>(locate::RelTarget(walk + 0xa1, walk + 0xa5));
    s.queueItems = reinterpret_cast<uint8_t**>(locate::RelTarget(walk + 0xf3, walk + 0xf7));
    s.queueData = walk[0xff];
    s.queueCount = walk[0xd8];
    s.buildable = walk[0x137];
    s.level = locate::Disp32(upgrade + 0x16a);
    s.nextLevel = locate::Disp32(upgrade + 0x17a);
    s.definitions[0] = reinterpret_cast<uint8_t**>(locate::RelTarget(level + 0x2b, level + 0x2f));
    s.definitions[1] = reinterpret_cast<uint8_t**>(locate::RelTarget(module + 0x3d, module + 0x41));
    s.definitions[2] = reinterpret_cast<uint8_t**>(locate::RelTarget(building + 0x3d, building + 0x41));
    auto* lookup = locate::RelTarget(module + 0x42, module + 0x46);
    auto* array = uishortcuts::InFunction(lookup, 0x100, "41 8B 77 ?? 85 F6 7E 57 4D 8B 67 ?? 48 83 7D FF 10");
    auto* m = locate::FindOne(
        image, "completed starbase modules",
        "4D 63 46 14 44 3B 87 ?? ?? ?? ?? 7C 09 4C 8B 2D ?? ?? ?? ?? EB 0B 48 8B 87 ?? ?? ?? ?? 4E 8B 2C C0", error);
    auto* b = locate::FindOne(
        image, "completed starbase buildings",
        "4C 63 46 14 44 3B 83 ?? ?? ?? ?? 7C 09 48 8B 2D ?? ?? ?? ?? EB 0B 48 8B 83 ?? ?? ?? ?? 4A 8B 2C C0", error);
    if (!array || !m || !b) {
        error = "starbase state array discovery failed: " + error;
        return false;
    }
    s.definitionCount = array[3];
    s.definitionData = array[11];
    s.moduleCount = locate::Disp32(m + 7);
    s.modules = locate::Disp32(m + 25);
    s.buildingCount = locate::Disp32(b + 7);
    s.buildings = locate::Disp32(b + 25);
    out = s;
    return true;
}
inline bool LocateSelection(const locate::Image& image, SelectionLocations& result, std::string& error) {
    constexpr char pattern[] = "48 8B 15 ?? ?? ?? ?? 48 85 D2 74 2D 44 8B 81 ?? ?? ?? ?? "
                               "41 8B C0 25 FF FF FF 00 3B 42 20 73 19 8B C8 48 03 C9 48 8B 42 18 "
                               "48 8B 74 C8 08 48 85 F6 74 06 44 39 46 08 74 07 "
                               "48 8B 35 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ??";
    auto* p = locate::FindOne(image, "starbase view selection", pattern, error);
    if (!p)
        return false;
    SelectionLocations found;
    found.starbaseDatabase = reinterpret_cast<uint8_t**>(locate::RelTarget(p + 3, p + 7));
    found.selectedReference = locate::Disp32(p + 15);
    found.objectReference = p[54];
    found.gameState = reinterpret_cast<uint8_t**>(locate::RelTarget(p + 67, p + 71));
    found.observedCountry = locate::RelTarget(p + 72, p + 76);
    if (found.selectedReference > 0x10000 || found.objectReference > 0x80) {
        error = "invalid starbase selection offsets";
        return false;
    }
    result = found;
    return true;
}
// Template editor: where a starbase keeps its module and building capacity (the sum of the
// starbase_module_capacity_add / starbase_building_capacity_add modifiers), how many slots a
// level type lists, and the game's name for a module or building.
struct EditorLocations {
    size_t moduleCapacity = 0, buildingCapacity = 0;     // CStarbase, int
    size_t levelModuleSlots = 0, levelBuildingSlots = 0; // CStarbaseLevelType, int counts
    uint8_t* componentName = nullptr;                    // optional: names fall back to keys
    size_t componentType = 0;                            // optional: _StarbaseType; 0 = no kind filter
};
inline bool LocateEditor(const locate::Image& image, const StateLocations& state, EditorLocations& out,
                         std::string& error) {
    EditorLocations found;
    size_t free = 0, slots = 0;
    size_t levelCount[2]{}, levelData[2]{};
    const auto freePattern = locate::Parse(kFreeSlots), slotPattern = locate::Parse(kLevelSlotType);
    for (const auto& section : image.code) {
        for (auto* p : locate::FindAll(section.begin, section.end, freePattern, 3)) {
            ++free;
            const size_t data = locate::Disp32(p + 3), count = locate::Disp32(p + 12),
                         capacity = locate::Disp32(p + 18);
            if (data == state.modules && count == state.moduleCount)
                found.moduleCapacity = capacity;
            else if (data == state.buildings && count == state.buildingCount)
                found.buildingCapacity = capacity;
            else {
                error = "unrecognized starbase free slot count";
                return false;
            }
        }
        for (auto* p : locate::FindAll(section.begin, section.end, slotPattern, 3)) {
            if (slots < 2) {
                levelCount[slots] = locate::Disp32(p + 8);
                levelData[slots] = locate::Disp32(p + 28);
            }
            ++slots;
        }
    }
    if (free != 2 || !found.moduleCapacity || found.buildingCapacity != found.moduleCapacity + 4) {
        error = "starbase capacity fields not found";
        return false;
    }
    if (slots != 2) {
        error = "starbase level slot lists not found";
        return false;
    }
    const int module = levelData[0] < levelData[1] ? 0 : 1, building = 1 - module;
    // CPdxArray: data +8, count +0x14. The lists are adjacent, followed by the ship size and the
    // next level, whose offset the upgrade validation gave.
    if (levelCount[module] != levelData[module] + 0xc || levelCount[building] != levelData[building] + 0xc ||
        levelData[building] != levelData[module] + 0x18 || levelData[building] + 0x18 != state.nextLevel) {
        error = "starbase level slot list cross-check failed";
        return false;
    }
    found.levelModuleSlots = levelCount[module];
    found.levelBuildingSlots = levelCount[building];
    std::string nameError;
    if (auto* name = locate::FindOne(image, "starbase component name", kComponentName, nameError)) {
        auto* prefix = uishortcuts::InFunction(name, kComponentNameSize, kComponentNamePrefix);
        if (prefix && memcmp(locate::RelTarget(prefix + 7, prefix + 11), "sm_", 4) == 0)
            found.componentName = name;
    }
    std::string typeError;
    if (auto* type = locate::FindOne(image, "starbase component type", kComponentTypeRead, typeError)) {
        const size_t offset = locate::Disp32(type + 4);
        bool same = offset >= 0x40 && offset < 0x4000;
        for (size_t at : {size_t(20), size_t(36), size_t(65), size_t(81), size_t(97)})
            same = same && locate::Disp32(type + at) == offset;
        if (same)
            found.componentType = offset;
    }
    out = found;
    return true;
}
// The defenses tab's Upgrade button command. Its constructor is inlined, so the field layout
// is the one the pattern writes; the call targets must agree with the starbase locations.
struct PlatformUpgradeLocations {
    uintptr_t* vtable = nullptr;
    static constexpr size_t kSize = 0x28, kCountry = 0x20, kStarbase = 0x24;
};
inline bool LocatePlatformUpgrade(const locate::Image& image, const SelectionLocations& selection,
                                  const ConstructionLocations& construction, PlatformUpgradeLocations& out,
                                  std::string& error) {
    auto* p = locate::FindOne(image, "defense platform upgrade", kPlatformUpgrade, error);
    if (!p)
        return false;
    PlatformUpgradeLocations found;
    found.vtable = reinterpret_cast<uintptr_t*>(locate::RelTarget(p + 72, p + 76));
    auto* valid = locate::RelTarget(p + 116, p + 120);
    if (reinterpret_cast<uint8_t**>(locate::RelTarget(p + 24, p + 28)) != selection.gameState ||
        locate::RelTarget(p + 29, p + 33) != selection.observedCountry ||
        locate::RelTarget(p + 130, p + 134) != construction.allocate ||
        locate::RelTarget(p + 184, p + 188) != construction.post || !uishortcuts::SlotTarget(image, found.vtable) ||
        uishortcuts::SlotTarget(image, found.vtable + 8) != valid) {
        error = "defense platform upgrade cross-check failed";
        return false;
    }
    out = found;
    return true;
}
} // namespace starbasetemplates
