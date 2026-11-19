#include "../src/interface/starbase/starbasetemplates_reader.h"
#include <array>
#include <cstdlib>
#include <cstdio>
using namespace starbasetemplates;
namespace {
void Check(bool ok) {
    if (!ok)
        std::abort();
}
template <class T> void Put(void* object, size_t offset, T value) {
    memcpy(static_cast<uint8_t*>(object) + offset, &value, sizeof(value));
}
bool Yes(void*) {
    return true;
}
uintptr_t definitionTable[2]{0, reinterpret_cast<uintptr_t>(&Yes)};
uintptr_t moduleTable[1]{}, buildingTable[1]{}, upgradeTable[1]{};
struct Definition {
    std::array<uint8_t, 768> data{};
    Definition(const char* key) {
        Put(data.data(), 0, definitionTable);
        game::String s{};
        s.kind = 1;
        s.size = strlen(key);
        s.cap = 15;
        if (s.size <= 15)
            memcpy(s.buf, key, s.size);
        else {
            s.ptr = const_cast<char*>(key);
            s.cap = s.size;
        }
        Put(data.data(), 0x10, s);
    }
};
struct Database {
    std::array<uint8_t, 64> data{};
    std::array<uint8_t, 16> entry{};
    uint8_t* pointer = data.data();
    Database(uint8_t* object) {
        Put(data.data(), 0x18, entry.data());
        Put(data.data(), 0x20, uint32_t(1));
        Put(entry.data(), 8, object);
    }
};
std::array<uint8_t, 48> country{};
uint8_t* Owner(uint8_t*) {
    return country.data();
}
bool Occupied(uint8_t*, void*) {
    return false;
}
// CStarbaseComponent::GetName with no starbase: "Anchorage" for anchorage, else the unlocalised key.
game::String* Name(uint8_t* definition, game::String* out, void*) {
    const std::string text = DefinitionKey(definition) == "anchorage" ? "Anchorage" : "sm_" + DefinitionKey(definition);
    *out = game::String{};
    out->kind = 1;
    out->cap = 15;
    out->size = text.size();
    memcpy(out->buf, text.c_str(), text.size());
    return out;
}
void Free(void*) {
}
}
int main() {
    Definition port("starbase_level_starport"), hold("starbase_level_starhold"), anchor("anchorage");
    Put(port.data.data(), 0x100, hold.data.data());
    std::array<uint8_t, 64> definitions{};
    uint8_t* allLevels[]{port.data.data(), hold.data.data()};
    Put(definitions.data(), 8, allLevels);
    Put(definitions.data(), 16, 2);
    auto* definitionsPtr = definitions.data();
    std::array<uint8_t, 256> base{}, queue{}, item{}, buildable{};
    const uint32_t id = 0x02000000, queueId = 0x03000000, itemId = 0x04000000;
    Put(base.data(), 8, id);
    Put(queue.data(), 8, queueId);
    Put(item.data(), 8, itemId);
    Put(country.data(), 0x20, uint32_t(42));
    Database bases(base.data()), queues(queue.data()), items(item.data());
    uint8_t* modules[]{anchor.data.data(), nullptr};
    uint32_t queued[]{itemId};
    Put(base.data(), 0x30, modules);
    Put(base.data(), 0x38, 2);
    Put(base.data(), 0x48, 0);
    Put(base.data(), 0x50, port.data.data());
    Put(base.data(), 0x58, queueId);
    Put(queue.data(), 0x20, queued);
    Put(queue.data(), 0x2c, 1);
    Put(item.data(), 0x18, buildable.data());
    Put(buildable.data(), 0, moduleTable);
    Put(buildable.data(), 8, anchor.data.data());
    Put(buildable.data(), 0x10, id);
    Put(buildable.data(), 0x14, 1);
    NativeReader r;
    r.selection.starbaseDatabase = &bases.pointer;
    r.selection.objectReference = 8;
    r.construction.mainQueue = 0x58;
    r.construction.moduleVtable = moduleTable;
    r.construction.buildingVtable = buildingTable;
    r.construction.upgradeVtable = upgradeTable;
    auto& f = r.fields;
    f.owner = reinterpret_cast<uint8_t*>(&Owner);
    f.occupation = reinterpret_cast<uint8_t*>(&Occupied);
    f.modules = 0x30;
    f.moduleCount = 0x38;
    f.buildings = 0x40;
    f.buildingCount = 0x48;
    f.level = 0x50;
    f.nextLevel = 0x100;
    f.queues = &queues.pointer;
    f.queueItems = &items.pointer;
    f.queueData = 0x20;
    f.queueCount = 0x2c;
    f.buildable = 0x18;
    f.definitions[0] = &definitionsPtr;
    f.definitionData = 8;
    f.definitionCount = 16;
    auto s = r.Snapshot(id, "starbase_level_starhold");
    Check(s.valid && s.owner == 42 && !s.targetLevelSatisfied);
    Check(s.built.modules == std::vector<std::string>({"anchorage", ""}));
    Check(s.successors.size() == 2 && s.queue.size() == 1 && s.queue[0].slot == 1);
    Check(!r.Snapshot(0x05000000).valid); // Same database slot, wrong generation.
    Put(base.data(), 0x50, hold.data.data());
    Check(r.Snapshot(id, "starbase_level_starport").targetLevelSatisfied);
    Check(!r.FindDefinition(Kind::Upgrade, "missing_mod_level"));
    Put(hold.data.data(), 0x100, port.data.data());
    bool rejected = false;
    try {
        r.Snapshot(id);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    Check(rejected);
    // Template editor: the chain with each level's listed slots, capacity, and definitions.
    Put(hold.data.data(), 0x100, static_cast<uint8_t*>(nullptr));
    Check(r.Levels("starbase_level_starport").size() == 2 && r.Levels("starbase_level_starport")[0].modules == 0);
    int moduleCapacity = 0, buildingCapacity = 0;
    Check(!r.Capacity(base.data(), moduleCapacity, buildingCapacity));
    r.editorLocated = true;
    r.editor.levelModuleSlots = 0xdc;
    r.editor.levelBuildingSlots = 0xf4;
    r.editor.moduleCapacity = 0x60;
    r.editor.buildingCapacity = 0x64;
    Put(port.data.data(), 0xdc, 2);
    Put(port.data.data(), 0xf4, 1);
    Put(hold.data.data(), 0xdc, 40);
    Put(hold.data.data(), 0xf4, 2);
    const auto levels = r.Levels("starbase_level_starport");
    Check(levels.size() == 2 && levels[0].key == "starbase_level_starport" && levels[0].modules == 2 &&
          levels[0].buildings == 1);
    Check(levels[1].modules == kEditorMaxSlots && levels[1].buildings == 2);
    Check(r.Levels("missing_mod_level").empty());
    Put(base.data(), 0x60, 7);
    Put(base.data(), 0x64, 5);
    Check(r.Capacity(base.data(), moduleCapacity, buildingCapacity) && moduleCapacity == 7 && buildingCapacity == 5);
    Put(base.data(), 0x60, -1);
    Check(!r.Capacity(base.data(), moduleCapacity, buildingCapacity));
    Definition yard("shipyard"), ring("orbital_ring_habitation");
    r.editor.componentType = 0x2d8;
    Put(yard.data.data(), 0x2d8, int(kTypeAll));
    Put(anchor.data.data(), 0x2d8, int(kTypeStarbase));
    Put(ring.data.data(), 0x2d8, int(kTypeOrbitalRing));
    std::array<uint8_t, 64> moduleDefinitions{};
    uint8_t* allModules[]{yard.data.data(), anchor.data.data(), ring.data.data()};
    Put(moduleDefinitions.data(), 8, allModules);
    Put(moduleDefinitions.data(), 16, 3);
    auto* modulesPtr = moduleDefinitions.data();
    f.definitions[1] = &modulesPtr;
    auto choices = r.Choices(Kind::Module);
    Check(choices.size() == 3 && choices[0].key == "shipyard" && choices[0].name.empty() &&
          choices[0].type == kTypeAll);
    // Starbase (0) can't be inferred without waystation and arkship examples: listed everywhere.
    Check(choices[1].type == -1 && choices[2].type == kTypeOrbitalRing);
    Check(r.Types().trusted && r.Types().Of(4) == kTypeAll);
    // A value outside the enum: the offset is wrong for this build, so no types at all.
    Put(anchor.data.data(), 0x2d8, 77);
    choices = r.Choices(Kind::Module);
    Check(choices.size() == 3 && choices[0].type == -1 && choices[2].type == -1 && !r.Types().trusted);
    Put(anchor.data.data(), 0x2d8, int(kTypeStarbase));
    r.editor.componentType = 0; // not located
    Check(r.Choices(Kind::Module)[2].type == -1 && !r.Types().trusted);
    r.editor.componentType = 0x2d8;
    r.editor.componentName = reinterpret_cast<uint8_t*>(&Name);
    r.gameFree = &Free;
    choices = r.Choices(Kind::Module);
    Check(choices[1].name == "Anchorage" && choices[0].name.empty()); // "sm_shipyard": no localisation
    Check(r.Choices(Kind::Upgrade).empty());
    // Station kinds: the chain from each kind's first level, empty when the game has none.
    Definition outpost("starbase_level_outpost"), way1("starbase_level_waystation_1"), way2("mod_waystation_2");
    Put(outpost.data.data(), 0x100, port.data.data());
    Put(way1.data.data(), 0x100, way2.data.data());
    uint8_t* kindLevels[]{outpost.data.data(), port.data.data(), hold.data.data(), way1.data.data(), way2.data.data()};
    Put(definitions.data(), 8, kindLevels);
    Put(definitions.data(), 16, 5);
    const auto kinds = r.Kinds();
    Check(kinds[kStationStarbase] ==
          std::vector<std::string>({"starbase_level_outpost", "starbase_level_starport", "starbase_level_starhold"}));
    Check(kinds[kStationWaystation] == std::vector<std::string>({"starbase_level_waystation_1", "mod_waystation_2"}));
    Check(kinds[kStationOrbitalRing].empty() && kinds[kStationCitadel].empty());
    Check(KindOfLevel(kinds, "mod_waystation_2") == kStationWaystation &&
          KindOfLevel(kinds, "starbase_level_starhold") == kStationStarbase);
    std::puts("starbasetemplates_reader_test: passed");
}
