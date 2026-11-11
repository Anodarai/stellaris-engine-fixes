// Game types stellaris-engine-fixes uses. Where things are in stellaris.exe is found by locate.h;
// see re/fleet_outliner_sort.md for how each of them was found.
#pragma once
#include <cstddef>
#include <cstdint>

namespace game {

// Paradox CString as laid out in the MSVC build (0x30 bytes).
struct String {
    uint8_t kind;  // 1 = not heap-owned
    uint8_t pad[7];
    uint64_t aux;
    union {
        char buf[16];
        char* ptr;
    };
    size_t size;
    size_t cap;  // 0xF when the text is stored inline

    const char* data() const { return cap > 0xF ? ptr : buf; }
};
static_assert(sizeof(String) == 0x30, "game::String layout");

using CompareFn = bool(__fastcall*)(void* cmp, uint32_t fleetIdA, uint32_t fleetIdB);
using FleetGroupUpdateFn = int(__fastcall*)(void* group, bool countOnly);
using GetNameFn = String*(__fastcall*)(void* sub, String* out);
using FreeFn = void(__fastcall*)(void* p);

// The console (re/simulation_speed.md).
// A command's result, CConsoleCmd::SResult: success (the line is drawn white, else red) and the
// text, whose lines become console lines.
struct ConsoleResult {
    bool ok;
    uint8_t pad[7];
    String text;
};
static_assert(sizeof(ConsoleResult) == 0x38, "game::ConsoleResult layout");
using ExecuteFn = ConsoleResult*(__fastcall*)(void* manager, ConsoleResult* out, const uint8_t* args);
using RunCommandFn = void(__fastcall*)(void* console, const String* line);
// CPdxArray<CString>, Execute's arguments (the command's name first).
constexpr size_t kArrayDataOffset = 0x8;    // String*
constexpr size_t kArrayCountOffset = 0x14;  // int

// The console's `effect` command, used by script storage.
using EffectCommandFn = ConsoleResult*(__fastcall*)(ConsoleResult* out, const uint8_t* args);

// The tick loop.
using CheckTickFn = uint64_t(__fastcall*)(uint8_t* idler, float frameSeconds);
using TurnTickFn = void(__fastcall*)(uint8_t* state, void* checksums);
using RenderFrameFn = void(__fastcall*)(void* a, void* b, void* c, void* d);  // takes nothing it uses

// Fleet automation's pathfinding.
using IntelFn = uint64_t(__fastcall*)(uint8_t* country, int32_t system, uint32_t owner);  // int in eax
using SearchFn = uint64_t(__fastcall*)(void* a, void* b, void* c, void* d, void* e, void* f);
using RelationCheckFn = bool(__fastcall*)(uint8_t* relation);
using HashFindFn = uint64_t(__fastcall*)(uint8_t* table, int32_t key);  // int in eax
using CountryCheckFn = bool(__fastcall*)(uint8_t* country);

// MicroUpdate's combat participants map. The insert works on the static map and ignores its
// first argument; it moves *value in, or sets inserted = false if the id is already there.
struct InsertResult {
    uint8_t* entry;
    bool inserted;
};
using CombatInsertFn = InsertResult*(__fastcall*)(void* unused, InsertResult* out, uint32_t hash, const uint32_t* id,
                                                  uint8_t* value);

// The AI's megastructure placement. CanAfford(type, country, CString* why); FindBestPosition is a
// member function returning a struct: (minister, out, type, fleet, systems set, seed, best score).
using CanAffordFn = bool(__fastcall*)(const uint8_t* type, const uint8_t* country, void* why);
using FindBestPositionFn = void*(__fastcall*)(void* a, void* b, void* c, void* d, void* e, void* f, void* g);

// The deferred database references. Register_Internal takes (resolver, owner, key, database, callback).
using DeferredRegisterFn = void(__fastcall*)(uint8_t* resolver, const void* owner, void* a, void* b, void* c);
using DeferredUnregisterFn = void(__fastcall*)(uint8_t* resolver, const void* owner);

// The token strings' table.
using LexerInstanceFn = uint8_t*(__fastcall*)();
using RebuildLookupFn = void(__fastcall*)();
using SetSizeFn = void(__fastcall*)(void* array, int size);
using AssignFn = void*(__fastcall*)(String* string, const char* text, size_t length);

// TPdxRef databases (the same for every object type).
constexpr size_t kDbCountOffset = 0x20;    // uint32 entry count
constexpr size_t kDbEntriesOffset = 0x18;  // entries: 16 bytes each, object pointer at +8

// TPdxRef lookup, as the game inlines it: the object for `id` in database `db` (an object's own
// id at `idOffset`), or nullptr if it is gone. Ids reuse slots, hence the id check.
inline uint8_t* ResolveRef(uint8_t* db, uint32_t id, size_t idOffset) {
    uint32_t index = id & 0xFFFFFF;
    if (!db || index >= *reinterpret_cast<uint32_t*>(db + kDbCountOffset)) return nullptr;
    uint8_t* entries = *reinterpret_cast<uint8_t**>(db + kDbEntriesOffset);
    uint8_t* object = *reinterpret_cast<uint8_t**>(entries + 8 + static_cast<size_t>(index) * 16);
    if (!object || *reinterpret_cast<uint32_t*>(object + idOffset) != id) return nullptr;
    return object;
}

// The same, with the type's null instance for a missing object, as most of the game's code does.
inline uint8_t* ResolveRefOr(uint8_t* db, uint32_t id, size_t idOffset, uint8_t* nullObject) {
    uint8_t* object = ResolveRef(db, id, idOffset);
    return object ? object : nullObject;
}

}  // namespace game
