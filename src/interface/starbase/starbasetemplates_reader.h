#pragma once
#include "starbasetemplates_native.h"
#include "starbasetemplates.h"
#include "starbasetemplates_editor.h"
#include "../../performance/natsort.h"
#include "../../core/game.h"
#include <stdexcept>
#include <set>

namespace starbasetemplates {
inline bool CopyMemoryChecked(void* destination, const void* source, size_t size) {
    __try {
        memcpy(destination, source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
template <class T> T ReadField(const void* p, size_t offset = 0) {
    T result{};
    if (!p || !CopyMemoryChecked(&result, static_cast<const uint8_t*>(p) + offset, sizeof(result)))
        throw std::runtime_error("Cannot read starbase state");
    return result;
}
inline uint8_t* Reference(uint8_t** database, uint32_t id, size_t idOffset = 8) {
    auto* db = ReadField<uint8_t*>(database);
    if (!db || id == UINT32_MAX)
        return nullptr;
    const auto index = id & 0xffffff;
    if (index >= ReadField<uint32_t>(db, 0x20))
        return nullptr;
    auto* entries = ReadField<uint8_t*>(db, 0x18);
    auto* object = ReadField<uint8_t*>(entries, 8 + size_t(index) * 16);
    return object && ReadField<uint32_t>(object, idOffset) == id ? object : nullptr;
}
inline bool DefinitionValid(uint8_t* object) {
    __try {
        return object && reinterpret_cast<bool (*)(void*)>((*reinterpret_cast<uintptr_t**>(object))[1])(object);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
inline std::string DefinitionKey(uint8_t* object) {
    if (!DefinitionValid(object))
        return {};
    const auto str = ReadField<game::String>(object, 0x10);
    if (!str.size || str.size > 256 || str.cap < str.size)
        throw std::runtime_error("Invalid definition key");
    std::string value(str.size, '\0');
    if (!CopyMemoryChecked(value.data(), str.data(), str.size))
        throw std::runtime_error("Cannot read definition key");
    return value;
}
inline uint8_t* CallOwner(uint8_t* function, uint8_t* base) {
    __try {
        return reinterpret_cast<uint8_t* (*)(uint8_t*)>(function)(base);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}
inline bool CallOccupation(uint8_t* function, uint8_t* base) {
    __try {
        return reinterpret_cast<bool (*)(uint8_t*, void*)>(function)(base, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}
// CStarbaseComponent<T>::GetName with no starbase: the localised "sm_<key>".
inline bool CallComponentName(uint8_t* function, uint8_t* definition, game::String* out) {
    __try {
        reinterpret_cast<game::String* (*)(uint8_t*, game::String*, void*)>(function)(definition, out, nullptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
struct NativeReader {
    SelectionLocations selection;
    ConstructionLocations construction;
    StateLocations fields;
    // Template editor; `editorLocated` false leaves slot counts to what the draft holds.
    EditorLocations editor;
    bool editorLocated = false;
    void (*gameFree)(void*) = nullptr; // frees a CString buffer the game allocated

    // The chain from `first` on, with each level's listed slots.
    std::vector<LevelInfo> Levels(const std::string& first) const {
        std::vector<LevelInfo> levels;
        std::set<uint8_t*> seen;
        auto* level = FindDefinition(Kind::Upgrade, first);
        while (DefinitionValid(level)) {
            if (levels.size() >= 256 || !seen.insert(level).second)
                throw std::runtime_error("Cyclic starbase level chain");
            LevelInfo info;
            info.key = DefinitionKey(level);
            if (editorLocated) {
                const int modules = ReadField<int>(level, editor.levelModuleSlots);
                const int buildings = ReadField<int>(level, editor.levelBuildingSlots);
                if (modules < 0 || modules > 256 || buildings < 0 || buildings > 256)
                    throw std::runtime_error("Invalid level slot count");
                info.modules = (std::min)(modules, kEditorMaxSlots);
                info.buildings = (std::min)(buildings, kEditorMaxSlots);
            }
            levels.push_back(std::move(info));
            level = ReadField<uint8_t*>(level, fields.nextLevel);
        }
        return levels;
    }
    // The starbase's current module and building capacity, modifiers included.
    bool Capacity(uint8_t* base, int& modules, int& buildings) const {
        if (!editorLocated || !base)
            return false;
        modules = ReadField<int>(base, editor.moduleCapacity);
        buildings = ReadField<int>(base, editor.buildingCapacity);
        return modules >= 0 && modules <= 256 && buildings >= 0 && buildings <= 256;
    }
    std::string ComponentName(uint8_t* definition, const std::string& key) const {
        if (!editor.componentName || !gameFree)
            return {};
        game::String s{};
        s.cap = 0xF;
        if (!CallComponentName(editor.componentName, definition, &s))
            return {};
        std::string text;
        if (s.size && s.size <= 256 && s.cap >= s.size) {
            text.resize(s.size);
            if (!CopyMemoryChecked(text.data(), s.data(), s.size))
                text.clear();
        }
        if (s.ownsHeap())
            gameFree(s.ptr);
        text = natsort::StripMarkup(text);
        return text == "sm_" + key ? std::string() : text; // no localisation: the caller shows the key
    }
    // Calls f(definition) for each definition of a kind the game loaded, until f returns false.
    template <class F> void ForEachDefinition(Kind kind, F&& f) const {
        auto* db = ReadField<uint8_t*>(fields.definitions[static_cast<int>(kind)]);
        if (!db)
            return;
        const int count = ReadField<int>(db, fields.definitionCount);
        if (count < 0 || count > 100000)
            throw std::runtime_error("Invalid definition database");
        auto* data = ReadField<uint8_t*>(db, fields.definitionData);
        for (int i = 0; i < count; ++i)
            if (!f(ReadField<uint8_t*>(data, size_t(i) * 8)))
                return;
    }
    // Every module or building definition the game loaded, mods included.
    std::vector<Choice> Choices(Kind kind) const {
        std::vector<Choice> choices;
        if (kind == Kind::Upgrade)
            return choices;
        const TypeMap map = Types();
        ForEachDefinition(kind, [&](uint8_t* def) {
            const auto key = DefinitionKey(def);
            if (!key.empty())
                choices.push_back({key, ComponentName(def, key),
                                   editor.componentType ? map.Of(ReadField<int>(def, editor.componentType)) : -1});
            return true;
        });
        return choices;
    }
    // What starbase_type's values mean, learned from the module and building definitions together.
    TypeMap Types() const {
        std::vector<std::string> keys;
        std::vector<int> types;
        if (!editor.componentType) {
            TypeMap none;
            none.note = "starbase_type not located";
            return none;
        }
        for (Kind kind : {Kind::Module, Kind::Building}) {
            if (!fields.definitions[static_cast<int>(kind)])
                continue;
            ForEachDefinition(kind, [&](uint8_t* def) {
                const auto key = DefinitionKey(def);
                if (!key.empty()) {
                    keys.push_back(key);
                    types.push_back(ReadField<int>(def, editor.componentType));
                }
                return true;
            });
        }
        return LearnTypes(keys, types);
    }
    // Each station kind's level chain.
    KindChains Kinds() const {
        KindChains chains;
        for (int kind = 0; kind < kStationKinds; ++kind)
            chains[kind] = Chain(FindDefinition(Kind::Upgrade, kStationKindInfo[kind].firstLevel));
        return chains;
    }

    uint8_t* FindDefinition(Kind kind, const std::string& key) const {
        uint8_t* found = nullptr;
        ForEachDefinition(kind, [&](uint8_t* def) {
            if (DefinitionKey(def) == key)
                found = def;
            return !found;
        });
        return found;
    }
    std::vector<std::string> Chain(uint8_t* level) const {
        std::vector<std::string> chain;
        std::set<uint8_t*> seen;
        while (DefinitionValid(level)) {
            if (chain.size() >= 256 || !seen.insert(level).second)
                throw std::runtime_error("Cyclic starbase level chain");
            chain.push_back(DefinitionKey(level));
            level = ReadField<uint8_t*>(level, fields.nextLevel);
        }
        return chain;
    }
    std::vector<std::string> Components(uint8_t* base, size_t dataOffset, size_t countOffset) const {
        const int count = ReadField<int>(base, countOffset);
        if (count < 0 || count > 256)
            throw std::runtime_error("Invalid starbase slot count");
        auto* data = ReadField<uint8_t*>(base, dataOffset);
        std::vector<std::string> result;
        for (int i = 0; i < count; ++i)
            result.push_back(DefinitionKey(ReadField<uint8_t*>(data, size_t(i) * 8)));
        return result;
    }
    State Snapshot(uint32_t id, const std::string& targetLevel = {}) const {
        State s;
        s.id = id;
        auto* base = Reference(selection.starbaseDatabase, id, selection.objectReference);
        if (!base) {
            s.valid = false;
            return s;
        }
        auto* owner = CallOwner(fields.owner, base);
        if (!owner) {
            s.valid = false;
            return s;
        }
        s.owner = ReadField<uint32_t>(owner, 0x20);
        s.occupied = CallOccupation(fields.occupation, base);
        s.built.level = DefinitionKey(ReadField<uint8_t*>(base, fields.level));
        s.built.modules = Components(base, fields.modules, fields.moduleCount);
        s.built.buildings = Components(base, fields.buildings, fields.buildingCount);
        s.successors = Chain(ReadField<uint8_t*>(base, fields.level));
        if (!targetLevel.empty()) {
            for (const auto& level : Chain(FindDefinition(Kind::Upgrade, targetLevel)))
                if (level == s.built.level)
                    s.targetLevelSatisfied = true;
        }
        auto* queue = Reference(fields.queues, ReadField<uint32_t>(base, construction.mainQueue));
        if (!queue)
            throw std::runtime_error("Starbase construction queue unavailable");
        const int count = ReadField<int>(queue, fields.queueCount);
        if (count < 0 || count > 4096)
            throw std::runtime_error("Invalid construction queue length");
        auto* data = ReadField<uint8_t*>(queue, fields.queueData);
        for (int i = 0; i < count; ++i) {
            auto* item = Reference(fields.queueItems, ReadField<uint32_t>(data, size_t(i) * 4));
            if (!item)
                throw std::runtime_error("Construction queue item unavailable");
            auto* buildable = ReadField<uint8_t*>(item, fields.buildable);
            const auto* table = ReadField<uintptr_t*>(buildable);
            Kind kind;
            if (table == construction.upgradeVtable)
                kind = Kind::Upgrade;
            else if (table == construction.moduleVtable)
                kind = Kind::Module;
            else if (table == construction.buildingVtable)
                kind = Kind::Building;
            else
                continue;
            if (ReadField<uint32_t>(buildable, 0x10) != id)
                throw std::runtime_error("Unexpected construction target");
            const int slot = kind == Kind::Upgrade ? 0 : ReadField<int>(buildable, 0x14);
            if (slot < 0 || slot > 255)
                throw std::runtime_error("Invalid queued slot");
            s.queue.push_back({kind, static_cast<size_t>(slot), DefinitionKey(ReadField<uint8_t*>(buildable, 8))});
        }
        return s;
    }
};
}
