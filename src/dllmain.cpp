// stellaris-engine-fixes: a winmm.dll proxy that Stellaris loads from its folder. It fixes slow
// or broken parts of stellaris.exe:
//   - the outliner sorts fleets and megastructures by name, naturally (numbers by value);
//   - mimalloc replaces the game's heap (allocator.h);
//   - faster simulation: fleet automation's intel lookup, empty combat entries skipped, cached
//     megastructure affordability, the AI income target loop counted instead of run, tick pacing;
//   - faster loading: cached file lookups (pathcache.h), incremental token table, no modifier
//     documentation, cheaper deferred-reference cleanup;
//   - optional fixes to the country AI ([ai_fixes]);
//   - the console command sef_bench, which measures how fast game days pass and where the time goes.
// Settings are in stellaris-engine-fixes.ini. Everything in stellaris.exe is found by code patterns
// (locate.h); a feature whose patterns are not found stays off, the others still work.
#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <share.h>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "allocator.h"
#include "game.h"
#include "incometarget.h"
#include "locate.h"
#include "natsort.h"
#include "pathcache.h"
#include "pdxstore.h"


// Exports of the system DLL we stand in for, forwarded to the real one.
#include "exports_winmm.h"

namespace {

// ---------------------------------------------------------------------------
// Logging

FILE* g_log = nullptr;
CRITICAL_SECTION g_logLock;

void Log(const char* fmt, ...) {
    if (!g_log) return;
    EnterCriticalSection(&g_logLock);
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vfprintf(g_log, fmt, args);
    va_end(args);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_logLock);
}

void OpenLog(HMODULE self) {
    InitializeCriticalSection(&g_logLock);
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    char* slash = strrchr(path, '\\');
    if (!slash) return;
    strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), "stellaris-engine-fixes.log");
    g_log = _fsopen(path, "w", _SH_DENYWR);  // others may read it while the game runs
}

// ---------------------------------------------------------------------------
// Timing

LARGE_INTEGER g_qpcFreq;

struct Timing {
    uint64_t passes = 0;
    LONGLONG ticks = 0, maxTicks = 0;

    void Add(LONGLONG t) {
        ++passes;
        ticks += t;
        maxTicks = (std::max)(maxTicks, t);
    }
    double AvgUs() const { return passes ? 1e6 * ticks / g_qpcFreq.QuadPart / passes : 0.0; }
    double MaxUs() const { return 1e6 * maxTicks / g_qpcFreq.QuadPart; }
};

LONGLONG Now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

// Logs both features' counters every 30 s. Called from the outliner hooks (no thread of our own).
void MaybeLogSummary();

// ---------------------------------------------------------------------------
// Game access

uintptr_t g_base = 0;
locate::StringLocations g_str;

// TPdxRef lookup: the object for `id` in database `db`, or nullptr if it is gone.
uint8_t* ResolveRef(uint8_t* db, uint32_t id, size_t idOffset) {
    uint32_t idx = id & 0xFFFFFF;
    if (!db || idx >= *reinterpret_cast<uint32_t*>(db + game::kDbCountOffset)) return nullptr;
    uint8_t* entries = *reinterpret_cast<uint8_t**>(db + game::kDbEntriesOffset);
    uint8_t* obj = *reinterpret_cast<uint8_t**>(entries + 8 + static_cast<size_t>(idx) * 0x10);
    if (!obj || *reinterpret_cast<uint32_t*>(obj + idOffset) != id) return nullptr;
    return obj;
}

// Copies a game string into `out` and frees it. Returns the length copied.
int TakeString(game::String& s, char* out, size_t outSize) {
    size_t n = s.size < outSize - 1 ? s.size : outSize - 1;
    memcpy(out, s.data(), n);
    out[n] = '\0';
    if (s.cap > 0xF && s.kind != 1) reinterpret_cast<game::FreeFn>(g_str.gameFree)(s.ptr);
    return static_cast<int>(n);
}

// Game dates count hours in 360-day years of 30-day months, from an epoch 43,800,000 hours
// (1,825,000 days) before 0.01.01. Found by comparing ships' dates with their construction_date
// in a save.
std::string FormatDate(int32_t d) {
    if (d == INT_MAX) return "no ships";
    constexpr int32_t kEpochHours = 43800000;
    int days = (d - kEpochHours) / 24;
    char buf[32];
    sprintf_s(buf, "%04d.%02d.%02d", days / 360, days % 360 / 30 + 1, days % 30 + 1);
    return buf;
}

// ---------------------------------------------------------------------------
// Hooking

// Replaces the first `stolen` bytes of `target` (position-independent instructions, at least 14
// bytes) with an absolute jump to `hook`. Returns a trampoline that runs them and jumps back,
// i.e. the original function, or null.
uint8_t* InstallJumpHook(uint8_t* target, size_t stolen, void* hook) {
    constexpr size_t kJmpSize = 14;  // jmp qword ptr [rip+0]; dq addr
    if (stolen < kJmpSize || stolen > 32) return nullptr;
    auto tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return nullptr;
    auto writeJump = [](uint8_t* at, uintptr_t to) {
        at[0] = 0xFF;
        at[1] = 0x25;
        memset(at + 2, 0, 4);
        memcpy(at + 6, &to, 8);
    };
    memcpy(tramp, target, stolen);
    writeJump(tramp + stolen, reinterpret_cast<uintptr_t>(target) + stolen);

    uint8_t patch[32];
    memset(patch, 0x90, stolen);
    writeJump(patch, reinterpret_cast<uintptr_t>(hook));
    DWORD old;
    if (!VirtualProtect(target, stolen, PAGE_EXECUTE_READWRITE, &old)) return nullptr;
    memcpy(target, patch, stolen);
    VirtualProtect(target, stolen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, stolen);
    return tramp;
}

// Script storage ("Script storage", below), called from the log, save and tick hooks earlier on.
bool g_storeOn = false;
void StoreEffect(const char* text, uint8_t* scope);
bool StoreTrigger(const char* text, uint8_t* scope);
void StoreFrame();
void StoreSaveStarted(void* manager);
void StoreLoading(const char* path);
void StoreNewGame();

// Points a vtable slot that holds `expected` at `hook`.
bool SwapVtableSlot(uintptr_t* slot, const uint8_t* expected, void* hook) {
    if (*slot != reinterpret_cast<uintptr_t>(expected)) return false;
    DWORD old;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old)) return false;
    *slot = reinterpret_cast<uintptr_t>(hook);
    VirtualProtect(slot, sizeof(*slot), old, &old);
    return true;
}

// ===========================================================================
// Fleets

locate::Locations g_fleet;
game::CompareFn g_origCompare = nullptr;
game::FleetGroupUpdateFn g_origFleetUpdate = nullptr;

// Copies the fleet's display name into `out`. Returns the length, -1 on a fault, or -2 if the
// fleet's name function is not the one the outliner's code calls (unexpected game version).
// Kept free of C++ objects with destructors so it can use SEH.
int FleetNameRaw(uint8_t* fleet, char* out, size_t outSize) {
    __try {
        void* sub = fleet + g_fleet.fleetNameSubobject;
        auto vtbl = *reinterpret_cast<uint8_t***>(sub);
        uint8_t* fn = vtbl[g_fleet.fleetNameVtblOffset / sizeof(void*)];
        if (!locate::IsFleetNameThunk(g_str, fn)) return -2;
        game::String s{};
        s.cap = 0xF;
        reinterpret_cast<game::GetNameFn>(fn)(sub, &s);
        return TakeString(s, out, outSize);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// Oldest construction date among the fleet's ships (INT_MAX if it has none), or -1 on a fault.
int64_t FleetAgeRaw(uint8_t* fleet) {
    __try {
        auto ships = *reinterpret_cast<uint32_t**>(fleet + g_fleet.fleetShipsOffset);
        int count = *reinterpret_cast<int*>(fleet + g_fleet.fleetShipCountOffset);
        int32_t oldest = INT_MAX;
        for (int i = 0; i < count; ++i) {
            uint8_t* ship = ResolveRef(*g_fleet.shipDb, ships[i], g_fleet.shipIdOffset);
            if (!ship) continue;
            int32_t date = *reinterpret_cast<int32_t*>(ship + g_fleet.shipDateOffset);
            if (date < oldest) oldest = date;
        }
        return oldest;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

struct FleetKey {
    std::string name;  // markup stripped
    int32_t oldestShip = INT_MAX;
};

// A game read faulted: fall back to the original order for good. Takes effect at the next
// update, since switching comparators mid-sort would break the sort's ordering guarantees
// (the game's inlined insertion sort relies on them to stay inside the array).
bool g_fleetFailed = false;
std::unordered_map<uint32_t, FleetKey> g_fleetKeys;  // for one outliner update
bool g_inFleetPass = false;                           // inside a fleet group update that uses FleetLess
uint64_t g_fleetKeyBuilds = 0, g_fleetCompares = 0;
std::vector<uint32_t> g_fleetPassIds;
int g_fleetOrdersLogged = 0;
Timing g_fleetTiming;

FleetKey BuildFleetKey(uint32_t id) {
    FleetKey key;
    uint8_t* fleet = ResolveRef(*g_fleet.fleetDb, id, g_fleet.fleetIdOffset);
    if (!fleet) return key;  // vanished fleets sort first, consistently
    char buf[512];
    int n = FleetNameRaw(fleet, buf, sizeof(buf));
    int64_t age = FleetAgeRaw(fleet);
    if (n < 0 || age < 0) {
        Log("%s reading fleet %08X (name=%d age=%lld); fleets fall back to the game's order",
            n == -2 ? "unexpected name function" : "fault", id, n, age);
        g_fleetFailed = true;
        return key;
    }
    key.name = natsort::StripMarkup(std::string_view(buf, n));
    key.oldestShip = static_cast<int32_t>(age);
    ++g_fleetKeyBuilds;
    return key;
}

const FleetKey& FleetKeyFor(uint32_t id) {
    auto it = g_fleetKeys.find(id);
    if (it == g_fleetKeys.end()) it = g_fleetKeys.emplace(id, BuildFleetKey(id)).first;
    return it->second;
}

bool FleetLess(uint32_t a, uint32_t b) {
    const FleetKey& ka = FleetKeyFor(a);
    const FleetKey& kb = FleetKeyFor(b);
    int c = natsort::Compare(ka.name, kb.name);
    if (c != 0) return c < 0;
    return ka.oldestShip < kb.oldestShip;  // equal: false, so the stable sort keeps ID order
}

bool __fastcall CompareHook(void* cmp, uint32_t a, uint32_t b) {
    if (!g_inFleetPass) return g_origCompare(cmp, a, b);
    ++g_fleetCompares;
    if (g_fleetOrdersLogged < 1) {
        g_fleetPassIds.push_back(a);
        g_fleetPassIds.push_back(b);
    }
    return FleetLess(a, b);
}

void LogFleetOrder(void* group) {
    std::sort(g_fleetPassIds.begin(), g_fleetPassIds.end());
    g_fleetPassIds.erase(std::unique(g_fleetPassIds.begin(), g_fleetPassIds.end()), g_fleetPassIds.end());
    std::stable_sort(g_fleetPassIds.begin(), g_fleetPassIds.end(), FleetLess);
    Log("fleet order for group %p (%zu fleets):", group, g_fleetPassIds.size());
    for (uint32_t id : g_fleetPassIds) {
        const FleetKey& k = FleetKeyFor(id);
        Log("  %08X  %-40s oldest ship %s", id, k.name.c_str(), FormatDate(k.oldestShip).c_str());
    }
}

int __fastcall FleetGroupUpdateHook(void* group, bool countOnly) {
    g_fleetKeys.clear();
    g_fleetPassIds.clear();
    uint64_t comparesBefore = g_fleetCompares;
    LONGLONG t0 = Now();
    g_inFleetPass = !g_fleetFailed;
    int result = g_origFleetUpdate(group, countOnly);
    g_inFleetPass = false;
    LONGLONG t1 = Now();

    if (g_fleetCompares > comparesBefore) {
        g_fleetTiming.Add(t1 - t0);
        if (g_fleetOrdersLogged < 1 && !g_fleetFailed) {
            ++g_fleetOrdersLogged;
            LogFleetOrder(group);
        }
    }
    MaybeLogSummary();
    return result;
}

void InstallFleets(const locate::Image& img) {
    std::string error;
    if (!locate::Locate(img, g_fleet, error)) {
        Log("fleets: %s; fleet sorting is off", error.c_str());
        return;
    }
    Log("fleets: fleet db +%llX (id at +0x%zx), ship db +%llX (id at +0x%zx), ships at +0x%zx/+0x%zx, "
        "name via +0x%zx vtable +0x%zx, ship construction date at +0x%zx",
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_fleet.fleetDb) - g_base), g_fleet.fleetIdOffset,
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_fleet.shipDb) - g_base), g_fleet.shipIdOffset,
        g_fleet.fleetShipsOffset, g_fleet.fleetShipCountOffset, g_fleet.fleetNameSubobject,
        g_fleet.fleetNameVtblOffset, g_fleet.shipDateOffset);
    g_origFleetUpdate = reinterpret_cast<game::FleetGroupUpdateFn>(g_fleet.fleetGroupUpdate);
    auto tramp = InstallJumpHook(g_fleet.compare, locate::kCompareStolenBytes, reinterpret_cast<void*>(&CompareHook));
    if (!tramp) {
        Log("fleets: failed to hook SSortFleets::Compare; fleet sorting is off");
        return;
    }
    g_origCompare = reinterpret_cast<game::CompareFn>(tramp);
    if (!SwapVtableSlot(g_fleet.fleetGroupVtblSlot, g_fleet.fleetGroupUpdate,
                        reinterpret_cast<void*>(&FleetGroupUpdateHook))) {
        // The comparator hook alone passes everything through to the original.
        Log("fleets: failed to hook COutlinerGroupFleet::UpdateInternal; fleet sorting is off");
        return;
    }
    Log("fleets: hooks installed");
}

// ===========================================================================
// Megastructures
//
// The group has no sort: UpdateInternal walks the country's megastructure list and calls
// UpdateEntry(entry i, mega) for each one it shows. The hook runs UpdateInternal twice: first
// recording the megastructures it shows (UpdateEntry skipped), then again handing UpdateEntry
// the same megastructures sorted by name. The game decides what is shown both times and
// nothing in the game state is changed.

using MegaEntryUpdateFn = void(__fastcall*)(void* entry, uint8_t* mega);
using TypeGetNameFn = game::String*(__fastcall*)(uint8_t* type, game::String* out);
using BuildStringFn = game::String*(__fastcall*)(uint8_t* name, game::String* out);

locate::MegaLocations g_mega;
game::FleetGroupUpdateFn g_origMegaUpdate = nullptr;
MegaEntryUpdateFn g_origMegaEntry = nullptr;

enum class MegaMode { Off, Record, Replay };
MegaMode g_megaMode = MegaMode::Off;
std::vector<uint8_t*> g_megaShown;   // Record: what the game shows, in its order
std::vector<uint8_t*> g_megaSorted;  // Replay: the same, sorted
size_t g_megaReplayIndex = 0;
bool g_megaFailed = false;
uint64_t g_megaNameBuilds = 0, g_megaMismatches = 0;
int g_megaOrdersLogged = 0;
Timing g_megaTiming;

struct MegaName {
    std::string name;  // markup stripped
    uint32_t typeId = 0;
    ULONGLONG built = 0;
};
std::unordered_map<uint32_t, MegaName> g_megaNames;  // by megastructure id, refreshed every second
constexpr ULONGLONG kMegaNameTtlMs = 1000;

// The megastructure's name as the outliner shows it (its own name, or its type's), or -1 on a fault.
int MegaNameRaw(uint8_t* mega, char* out, size_t outSize) {
    __try {
        game::String s{};
        s.cap = 0xF;
        if (*reinterpret_cast<void**>(mega + g_mega.megaCustomNameFlag) != nullptr) {
            reinterpret_cast<BuildStringFn>(g_str.buildString)(mega + g_mega.megaCustomName, &s);
        } else {
            uint8_t* type = *reinterpret_cast<uint8_t**>(mega + g_mega.megaTypeOffset);
            reinterpret_cast<TypeGetNameFn>(g_mega.typeGetName)(type, &s);
        }
        return TakeString(s, out, outSize);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

const std::string* MegaNameFor(uint8_t* mega, ULONGLONG now) {
    uint32_t id = *reinterpret_cast<uint32_t*>(mega + g_mega.megaIdOffset);
    uint8_t* type = *reinterpret_cast<uint8_t**>(mega + g_mega.megaTypeOffset);
    uint32_t typeId = type ? *reinterpret_cast<uint32_t*>(type + g_mega.typeIdOffset) : 0;
    MegaName& cached = g_megaNames[id];
    if (cached.built == 0 || cached.typeId != typeId || now - cached.built >= kMegaNameTtlMs) {
        char buf[512];
        int n = MegaNameRaw(mega, buf, sizeof(buf));
        if (n < 0) return nullptr;
        cached.name = natsort::StripMarkup(std::string_view(buf, n));
        cached.typeId = typeId;
        cached.built = now;
        ++g_megaNameBuilds;
    }
    return &cached.name;
}

void __fastcall MegaEntryHook(void* entry, uint8_t* mega) {
    switch (g_megaMode) {
        case MegaMode::Record:
            g_megaShown.push_back(mega);
            return;  // the replay updates every entry
        case MegaMode::Replay:
            if (g_megaReplayIndex < g_megaSorted.size()) mega = g_megaSorted[g_megaReplayIndex];
            ++g_megaReplayIndex;
            break;
        case MegaMode::Off:
            break;
    }
    g_origMegaEntry(entry, mega);
}

// Sorts g_megaShown into g_megaSorted. False if a name could not be read.
bool SortMegastructures() {
    ULONGLONG now = GetTickCount64();
    std::vector<std::pair<const std::string*, uint8_t*>> keyed;
    keyed.reserve(g_megaShown.size());
    for (uint8_t* mega : g_megaShown) {
        const std::string* name = MegaNameFor(mega, now);
        if (!name) return false;
        keyed.emplace_back(name, mega);
    }
    std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) {
        return natsort::Compare(*a.first, *b.first) < 0;  // equal names keep the game's order
    });
    g_megaSorted.clear();
    for (auto& k : keyed) g_megaSorted.push_back(k.second);
    return true;
}

int __fastcall MegaGroupUpdateHook(void* group, bool countOnly) {
    if (countOnly || g_megaFailed) return g_origMegaUpdate(group, countOnly);

    LONGLONG t0 = Now();
    g_megaShown.clear();
    g_megaMode = MegaMode::Record;
    int shown = g_origMegaUpdate(group, false);
    g_megaMode = MegaMode::Off;

    if (!SortMegastructures()) {
        Log("fault reading a megastructure name; megastructures fall back to the game's order");
        g_megaFailed = true;
        return g_origMegaUpdate(group, false);
    }

    g_megaReplayIndex = 0;
    g_megaMode = MegaMode::Replay;
    int replayed = g_origMegaUpdate(group, false);
    g_megaMode = MegaMode::Off;

    if (replayed != shown || g_megaReplayIndex != g_megaSorted.size()) {
        // Should not happen within one frame; the entries were still all updated (extra ones
        // with the game's own megastructure), so just count it.
        if (g_megaMismatches++ < 3) {
            Log("megastructures: replay showed %d (%zu entries) but the first run showed %d", replayed,
                g_megaReplayIndex, shown);
        }
    }
    g_megaTiming.Add(Now() - t0);

    if (g_megaOrdersLogged < 1 && !g_megaSorted.empty()) {
        ++g_megaOrdersLogged;
        Log("megastructure order for group %p (%zu shown):", group, g_megaSorted.size());
        ULONGLONG now = GetTickCount64();
        for (uint8_t* mega : g_megaSorted) {
            const std::string* name = MegaNameFor(mega, now);
            Log("  %08X  %s", *reinterpret_cast<uint32_t*>(mega + g_mega.megaIdOffset),
                name ? name->c_str() : "?");
        }
    }
    MaybeLogSummary();
    return replayed;
}

void InstallMegastructures(const locate::Image& img) {
    std::string error;
    if (!locate::LocateMega(img, g_mega, error)) {
        Log("megastructures: %s; megastructure sorting is off", error.c_str());
        return;
    }
    Log("megastructures: db +%llX (id at +0x%zx), type at +0x%zx (id at +0x%zx), own name at +0x%zx if +0x%zx is set",
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_mega.megaDb) - g_base), g_mega.megaIdOffset,
        g_mega.megaTypeOffset, g_mega.typeIdOffset, g_mega.megaCustomName, g_mega.megaCustomNameFlag);
    g_origMegaUpdate = reinterpret_cast<game::FleetGroupUpdateFn>(g_mega.groupUpdate);
    auto tramp = InstallJumpHook(g_mega.entryUpdate, locate::kMegaEntryStolenBytes,
                                 reinterpret_cast<void*>(&MegaEntryHook));
    if (!tramp) {
        Log("megastructures: failed to hook UpdateEntry; megastructure sorting is off");
        return;
    }
    g_origMegaEntry = reinterpret_cast<MegaEntryUpdateFn>(tramp);  // with mode Off the hook passes through
    if (!SwapVtableSlot(g_mega.groupVtblSlot, g_mega.groupUpdate, reinterpret_cast<void*>(&MegaGroupUpdateHook))) {
        Log("megastructures: failed to hook COutlinerGroupMegastructure::UpdateInternal; megastructure sorting is off");
        return;
    }
    Log("megastructures: hooks installed");
}

// stellaris-engine-fixes.ini next to this DLL, or empty.
std::string IniPath(HMODULE self) {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(self, path, MAX_PATH);
    char* slash = n && n < MAX_PATH ? strrchr(path, '\\') : nullptr;
    if (!slash) return std::string();
    strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), "stellaris-engine-fixes.ini");
    return path;
}

// ===========================================================================
// Fleet automation's intel lookup
//
// Fleet automation finds its targets with a shortest-path search over every system. For each
// system it expands, the game walks the fleets there and asks, for each, the searching country's
// intel on the fleet's owner in that system (`lookup`). That function scans the country's
// relations for the owner and then, without shared intel, the country's per-system list for the
// system: two linear scans per call, a large part of automation's time.
//
// While a search runs nothing changes the game state (it only reads), so during one search
// call, on its thread, the answers are computed once:
//   - per (country, owner): the relation's verdict on shared intel (the same virtual check and
//     flag the game reads);
//   - per country: whether it has intel of its own (the same field and function);
//   - per (country, system): the largest value of the system's entry, from one pass over the list
//     (the first entry for a system wins, as in the game's scan).
// Shared intel is still read through the game's own hash lookup. The caches are dropped when the
// search returns. Outside a search the game's function runs unchanged.
//
// [speed] fast_intel_lookup=0 turns this off; fast_intel_verify=1 also runs the game's function
// for every lookup, returns the game's answer, and logs any difference.

locate::IntelLocations g_intel;
game::IntelFn g_origIntel = nullptr;
game::SearchFn g_origSearch = nullptr;
std::atomic<bool> g_intelOn{false};
bool g_intelVerify = false;
std::atomic<uint64_t> g_intelSearches{0}, g_intelFast{0}, g_intelCompared{0}, g_intelMismatches{0};
std::atomic<bool> g_intelFailed{false};

struct IntelCache {
    uint8_t* country = nullptr;
    std::unordered_map<uint32_t, bool> shared;  // owner id -> shared intel
    int ownIntel = -1;                          // -1 not known yet; 0 none (the game returns 0); 1 has some
    bool systemsBuilt = false;
    std::unordered_map<int32_t, int32_t> systems;  // system id -> largest sub-entry value (at least 0)
};
thread_local int t_searchDepth = 0;
thread_local std::vector<IntelCache> t_intelCaches;  // one per country seen in the current search
thread_local size_t t_intelUsed = 0;

IntelCache& IntelCacheFor(uint8_t* country) {
    for (size_t i = 0; i < t_intelUsed; ++i) {
        if (t_intelCaches[i].country == country) return t_intelCaches[i];
    }
    if (t_intelUsed == t_intelCaches.size()) t_intelCaches.emplace_back();
    IntelCache& c = t_intelCaches[t_intelUsed++];
    c.country = country;
    c.shared.clear();
    c.ownIntel = -1;
    c.systemsBuilt = false;
    c.systems.clear();
    return c;
}

template <typename T>
T At(const uint8_t* p, size_t offset) {
    T v;
    memcpy(&v, p + offset, sizeof(T));
    return v;
}

// The game's lookup, from the caches. Reads only what the game's function reads.
int32_t FastIntel(uint8_t* country, int32_t system, uint32_t owner) {
    const locate::IntelLocations& L = g_intel;
    IntelCache& c = IntelCacheFor(country);
    bool shared;
    auto known = c.shared.find(owner);
    if (known != c.shared.end()) {
        shared = known->second;
    } else {
        uint8_t* relation;
        if (owner == At<uint32_t>(At<uint8_t*>(country, L.selfObject), L.selfId)) {
            relation = L.selfRelation;
        } else {
            relation = *L.defaultRelation;
            uint8_t* entry = At<uint8_t*>(country, L.relData);
            int count = At<int32_t>(country, L.relCount);
            for (int i = 0; i < count; ++i, entry += 0x70) {
                if (At<uint32_t>(entry, 0) == owner) {
                    relation = entry + L.relObject;
                    break;
                }
            }
        }
        auto check = At<game::RelationCheckFn>(At<uint8_t*>(relation, 0), L.relVirtual);
        shared = check(relation) && ((At<uint32_t>(relation, L.relFlags) >> L.relSharedBit) & 1) != 0;
        c.shared.emplace(owner, shared);
    }
    if (shared) {
        uint8_t* ownerCountry = ResolveRef(*L.countryDb, owner, L.countryId);
        if (!ownerCountry) ownerCountry = *L.nullCountry;
        return static_cast<int32_t>(reinterpret_cast<game::HashFindFn>(L.hashFind)(ownerCountry + L.ownerIntel, system));
    }
    if (c.ownIntel < 0) {
        c.ownIntel = At<int32_t>(country, L.noOwnIntel) == 0 &&
                             !reinterpret_cast<game::CountryCheckFn>(L.ownIntelExpired)(country)
                         ? 1
                         : 0;
    }
    if (c.ownIntel == 0) return 0;
    if (!c.systemsBuilt) {
        uint8_t* entry = At<uint8_t*>(country, L.sysData);
        int count = At<int32_t>(country, L.sysCount);
        c.systems.reserve(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i, entry += 0x28) {
            int32_t key = At<int32_t>(entry, L.sysKey);
            if (c.systems.count(key)) continue;  // the game's scan stops at the first
            int32_t best = 0;
            uint8_t* sub = At<uint8_t*>(entry, L.subData);
            int subs = At<int32_t>(entry, L.subCount);
            for (int j = 0; j < subs; ++j, sub += 0x18) best = (std::max)(best, At<int32_t>(sub, L.subValue));
            c.systems.emplace(key, best);
        }
        c.systemsBuilt = true;
    }
    auto found = c.systems.find(system);
    return found == c.systems.end() ? 0 : found->second;
}

// FastIntel behind SEH: false on a fault.
bool FastIntelGuarded(uint8_t* country, int32_t system, uint32_t owner, int32_t* out) {
    __try {
        *out = FastIntel(country, system, owner);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint64_t __fastcall IntelHook(uint8_t* country, int32_t system, uint32_t owner) {
    if (t_searchDepth == 0 || !g_intelOn) return g_origIntel(country, system, owner);
    int32_t fast = 0;
    if (!FastIntelGuarded(country, system, owner, &fast)) {
        if (!g_intelFailed.exchange(true)) Log("fleet intel: fault in the fast lookup; it is off from now on");
        g_intelOn = false;
        return g_origIntel(country, system, owner);
    }
    g_intelFast.fetch_add(1, std::memory_order_relaxed);
    if (!g_intelVerify) return static_cast<uint32_t>(fast);
    uint64_t game = g_origIntel(country, system, owner);
    g_intelCompared.fetch_add(1, std::memory_order_relaxed);
    if (static_cast<int32_t>(game) != fast && g_intelMismatches.fetch_add(1) < 20) {
        Log("fleet intel MISMATCH: country %p system %d owner %08X: game %d, fast %d", country, system, owner,
            static_cast<int32_t>(game), fast);
    }
    return game;
}

// Scope for the caches: one search call on this thread.
struct SearchScope {
    SearchScope() { ++t_searchDepth; }
    ~SearchScope() {
        if (--t_searchDepth == 0) t_intelUsed = 0;
    }
};

uint64_t __fastcall SearchHook(void* a, void* b, void* c, void* d, void* e, void* f) {
    if (!g_intelOn) return g_origSearch(a, b, c, d, e, f);
    g_intelSearches.fetch_add(1, std::memory_order_relaxed);
    SearchScope scope;
    return g_origSearch(a, b, c, d, e, f);
}

void InstallIntel(HMODULE self, const locate::Image& img) {
    std::string ini = IniPath(self);
    bool enabled = ini.empty() || GetPrivateProfileIntA("speed", "fast_intel_lookup", 1, ini.c_str()) != 0;
    g_intelVerify = !ini.empty() && GetPrivateProfileIntA("speed", "fast_intel_verify", 0, ini.c_str()) != 0;
    if (!enabled) {
        Log("fleet intel: off (fast_intel_lookup=0)");
        return;
    }
    std::string error;
    if (!locate::LocateIntel(img, g_intel, error)) {
        Log("fleet intel: %s; the fast lookup is off", error.c_str());
        return;
    }
    Log("fleet intel: lookup +%llX, search +%llX; relations +0x%zx/+0x%zx, systems +0x%zx/+0x%zx, owner intel +0x%zx",
        static_cast<unsigned long long>(g_intel.lookup - reinterpret_cast<uint8_t*>(g_base)),
        static_cast<unsigned long long>(g_intel.search - reinterpret_cast<uint8_t*>(g_base)), g_intel.relData,
        g_intel.relCount, g_intel.sysData, g_intel.sysCount, g_intel.ownerIntel);
    auto lookup = InstallJumpHook(g_intel.lookup, locate::kIntelLookupStolenBytes, reinterpret_cast<void*>(&IntelHook));
    if (!lookup) {
        Log("fleet intel: failed to hook the lookup; the fast lookup is off");
        return;
    }
    g_origIntel = reinterpret_cast<game::IntelFn>(lookup);  // passes through until g_intelOn
    auto search = InstallJumpHook(g_intel.search, locate::kSearchStolenBytes, reinterpret_cast<void*>(&SearchHook));
    if (!search) {
        Log("fleet intel: failed to hook the search; the fast lookup is off");
        return;
    }
    g_origSearch = reinterpret_cast<game::SearchFn>(search);
    g_intelOn = true;
    Log("fleet intel: hooks installed; fast lookup on%s", g_intelVerify ? ", verifying every lookup against the game's" : "");
}

// ---- Combat participants map ----
//
// Every tick, CGameState::MicroUpdate empties a static map of fleet id -> SCombatParticipants and
// inserts an entry for every fleet, including the many not in combat, whose entry is empty (seven
// empty arrays and a zero 16-bit field). Both readers, MicroUpdate's second fleet loop and the
// parallel ship update, look a fleet up and use an empty SCombatParticipants when it is missing.
// So an empty entry and no entry give the same result, and the hook skips inserting empty ones:
// fewer entries to insert, destroy, copy and probe, every tick.
//
// The caller only reads the result's `inserted` flag (if false it moves the value into the entry
// found), so a skipped insert reports inserted = true. An id already inserted since the map was
// last empty goes to the game's insert, so a repeated id behaves exactly as before.
//
// [speed] skip_empty_combat_entries=0 turns this off. With it on, `sef_bench combat off|on` switches
// the skipping at run time, for A/B runs of one save; the switch takes effect when the map is next
// empty (the next tick).

locate::CombatLocations g_combat;
game::CombatInsertFn g_origCombatInsert = nullptr;
uint64_t g_combatInserts = 0, g_combatSkipped = 0;
std::vector<uint32_t> g_combatIds;  // ids passed to the game's insert since the map was last empty
bool g_combatWanted = true;         // set by `sef_bench combat`
bool g_combatSkipping = true;       // g_combatWanted, taken when the map is empty

bool IsEmptyParticipants(const uint8_t* value) {
    for (size_t at = g_combat.countFirst; at <= g_combat.countLast; at += 0x18) {
        if (At<int32_t>(value, at) != 0) return false;
    }
    return At<uint16_t>(value, g_combat.tail) == 0;
}

game::InsertResult* __fastcall CombatInsertHook(void* unused, game::InsertResult* out, uint32_t hash,
                                                const uint32_t* id, uint8_t* value) {
    ++g_combatInserts;  // MicroUpdate's serial part: main thread only
    if (*g_combat.count == 0) {
        g_combatIds.clear();
        g_combatSkipping = g_combatWanted;
    }
    if (!g_combatSkipping) return g_origCombatInsert(unused, out, hash, id, value);
    if (IsEmptyParticipants(value) &&
        std::find(g_combatIds.begin(), g_combatIds.end(), *id) == g_combatIds.end()) {
        ++g_combatSkipped;
        out->entry = nullptr;
        out->inserted = true;
        return out;
    }
    g_combatIds.push_back(*id);
    return g_origCombatInsert(unused, out, hash, id, value);
}

void InstallCombat(HMODULE self, const locate::Image& img) {
    std::string ini = IniPath(self);
    if (!ini.empty() && GetPrivateProfileIntA("speed", "skip_empty_combat_entries", 1, ini.c_str()) == 0) {
        Log("combat map: off (skip_empty_combat_entries=0)");
        return;
    }
    std::string error;
    if (!locate::LocateCombat(img, g_combat, error)) {
        Log("combat map: %s; off", error.c_str());
        return;
    }
    Log("combat map: insert +%llX, count +%llX; entry counts +0x%zx..+0x%zx, field +0x%zx",
        static_cast<unsigned long long>(g_combat.insert - reinterpret_cast<uint8_t*>(g_base)),
        static_cast<unsigned long long>(reinterpret_cast<uint8_t*>(g_combat.count) - reinterpret_cast<uint8_t*>(g_base)),
        g_combat.countFirst, g_combat.countLast, g_combat.tail);
    g_combatIds.reserve(256);
    auto tramp = InstallJumpHook(g_combat.insert, locate::kCombatInsertStolenBytes,
                                 reinterpret_cast<void*>(&CombatInsertHook));
    if (!tramp) {
        Log("combat map: failed to hook the insert; off");
        return;
    }
    g_origCombatInsert = reinterpret_cast<game::CombatInsertFn>(tramp);
    Log("combat map: hook installed; empty entries are not inserted");
}

std::string CombatSummary() {
    if (!g_origCombatInsert) return std::string();
    char buf[160];
    sprintf_s(buf, "combat map: %llu inserts, %llu empty skipped (%.1f%%)", g_combatInserts, g_combatSkipped,
              g_combatInserts ? 100.0 * g_combatSkipped / g_combatInserts : 0.0);
    return buf;
}

// ---- AI megastructure placement ----
//
// In a country's weekly AI update, CAIInteriorMinister::BuildMegaStructure calls
// FindBestPositionForMegaStructure for a megastructure type and a construction ship. That scores
// every system the ship can reach, and each score calls CMegaStructureType::CanQueue, which ends in
// CanAfford(type, country): the type's cost for the country, from its modifiers, checked against
// the country's resources. The system plays no part in it, and nothing in the scoring loop changes
// the country (the AI only posts commands, and runs with the game state read-only). So within one
// FindBestPosition call, on its thread, the answer for a (type, country) is computed once.
// Calls that ask for the reason text (a CString*) always run the game's function.
//
// The weekly update of the largest AI empire is what the main thread waits for at the end of the
// parallel AI update; this placement search is a large part of it (re/simulation_speed.md).
//
// [speed] mega_afford_cache=0 turns this off; mega_afford_verify=1 also runs the game's function
// on every cached answer and logs any difference.

locate::MegaAffordLocations g_afford;
game::CanAffordFn g_origCanAfford = nullptr;
game::FindBestPositionFn g_origFindBest = nullptr;
bool g_affordVerify = false;
std::atomic<uint64_t> g_affordScopes{0}, g_affordCalls{0}, g_affordCached{0}, g_affordMismatches{0};

struct AffordEntry {
    const uint8_t* type;
    const uint8_t* country;
    bool result;
};
thread_local int t_affordDepth = 0;
thread_local std::vector<AffordEntry> t_affordCache;

bool __fastcall CanAffordHook(const uint8_t* type, const uint8_t* country, void* why) {
    if (t_affordDepth == 0 || why != nullptr) return g_origCanAfford(type, country, why);
    g_affordCalls.fetch_add(1, std::memory_order_relaxed);
    for (const AffordEntry& e : t_affordCache) {
        if (e.type != type || e.country != country) continue;
        g_affordCached.fetch_add(1, std::memory_order_relaxed);
        if (g_affordVerify) {
            bool game = g_origCanAfford(type, country, why);
            if (game != e.result && g_affordMismatches.fetch_add(1) < 20) {
                Log("megastructure CanAfford MISMATCH: type %p country %p: game %d, cached %d", type, country, game,
                    e.result);
            }
            return game;
        }
        return e.result;
    }
    bool result = g_origCanAfford(type, country, why);
    t_affordCache.push_back({type, country, result});
    return result;
}

// Scope for the cache: one FindBestPosition call on this thread.
struct AffordScope {
    AffordScope() { ++t_affordDepth; }
    ~AffordScope() {
        if (--t_affordDepth == 0) t_affordCache.clear();
    }
};

void* __fastcall FindBestHook(void* a, void* b, void* c, void* d, void* e, void* f, void* g) {
    g_affordScopes.fetch_add(1, std::memory_order_relaxed);
    AffordScope scope;
    return g_origFindBest(a, b, c, d, e, f, g);
}

void InstallMegaAfford(HMODULE self, const locate::Image& img) {
    std::string ini = IniPath(self);
    if (!ini.empty() && GetPrivateProfileIntA("speed", "mega_afford_cache", 1, ini.c_str()) == 0) {
        Log("megastructure placement: off (mega_afford_cache=0)");
        return;
    }
    g_affordVerify = !ini.empty() && GetPrivateProfileIntA("speed", "mega_afford_verify", 0, ini.c_str()) != 0;
    std::string error;
    if (!locate::LocateMegaAfford(img, g_afford, error)) {
        Log("megastructure placement: %s; off", error.c_str());
        return;
    }
    Log("megastructure placement: CanAfford +%llX, FindBestPositionForMegaStructure +%llX",
        static_cast<unsigned long long>(g_afford.canAfford - reinterpret_cast<uint8_t*>(g_base)),
        static_cast<unsigned long long>(g_afford.findBest - reinterpret_cast<uint8_t*>(g_base)));
    // CanAfford first: until FindBestPosition is hooked, t_affordDepth stays 0 and it passes through.
    auto afford = InstallJumpHook(g_afford.canAfford, locate::kCanAffordStolenBytes,
                                  reinterpret_cast<void*>(&CanAffordHook));
    if (!afford) {
        Log("megastructure placement: failed to hook CanAfford; off");
        return;
    }
    g_origCanAfford = reinterpret_cast<game::CanAffordFn>(afford);
    auto find = InstallJumpHook(g_afford.findBest, locate::kFindBestPositionStolenBytes,
                                reinterpret_cast<void*>(&FindBestHook));
    if (!find) {
        Log("megastructure placement: failed to hook FindBestPositionForMegaStructure; off");
        return;
    }
    g_origFindBest = reinterpret_cast<game::FindBestPositionFn>(find);
    Log("megastructure placement: hooks installed; CanAfford cached per search%s",
        g_affordVerify ? ", verifying every cached answer against the game's" : "");
}

std::string AffordSummary() {
    if (!g_origFindBest) return std::string();
    char buf[200];
    uint64_t calls = g_affordCalls.load(), cached = g_affordCached.load();
    sprintf_s(buf, "megastructure placement: %llu searches, %llu CanAfford calls, %llu cached (%.1f%%)",
              g_affordScopes.load(), calls, cached, calls ? 100.0 * cached / calls : 0.0);
    std::string out = buf;
    if (g_affordVerify) {
        sprintf_s(buf, "; verified, %llu mismatches", g_affordMismatches.load());
        out += buf;
    }
    return out;
}

// ---- Token strings ----
//
// CStaticLexer keeps a table of every token's text. GetString, and code that inlines it, calls
// RebuildLookup whenever the table's size differs from the token count, and RebuildLookup empties
// and refills the whole table. Script parsing adds dynamic tokens and asks for strings in turn, so
// each new token costs a pass over all of them (re/load_time.md).
//
// Tokens are only added at the end, and a token's text doesn't change, so once the table exists the
// hook grows it and fills in the new tokens. A table that should shrink goes to the game's function.
//
// [speed] fast_token_rebuild=0 turns this off; fast_token_rebuild_verify=1 also runs the game's
// function on every 16th call and logs any string that differs.

locate::LexerLocations g_lexer;
game::RebuildLookupFn g_origRebuild = nullptr;
bool g_lexerVerify = false;
std::atomic<uint64_t> g_lexerFull{0}, g_lexerIncremental{0}, g_lexerNew{0}, g_lexerCompared{0}, g_lexerMismatches{0};

void __fastcall RebuildLookupHook() {
    auto instance = reinterpret_cast<game::LexerInstanceFn>(g_lexer.instance);
    uint8_t* lexer = instance();
    int have = At<int32_t>(lexer, g_lexer.count);
    int wanted = At<int32_t>(lexer, g_lexer.wanted);
    if (have <= 0 || wanted < have) {
        g_lexerFull.fetch_add(1, std::memory_order_relaxed);
        g_origRebuild();
        return;
    }
    reinterpret_cast<game::SetSizeFn>(g_lexer.setSize)(lexer + g_lexer.array, wanted);
    auto assign = reinterpret_cast<game::AssignFn>(g_lexer.assign);
    auto strings = At<game::String*>(lexer, g_lexer.data);
    auto tokens = At<uint8_t**>(lexer, g_lexer.dynData);
    for (int i = At<int32_t>(lexer, g_lexer.dynCount) - 1; i >= 0; --i) {
        int id = At<int32_t>(tokens[i], 0);
        if (id < have) break;
        const char* name = At<const char*>(tokens[i], g_lexer.tokenName);
        assign(&strings[id], name, strlen(name));
        g_lexerNew.fetch_add(1, std::memory_order_relaxed);
    }
    uint64_t call = g_lexerIncremental.fetch_add(1, std::memory_order_relaxed);
    if (!g_lexerVerify || call % 16 != 0) return;
    std::vector<std::string> mine;
    mine.reserve(static_cast<size_t>(wanted));
    for (int i = 0; i < wanted; ++i) mine.emplace_back(strings[i].data(), strings[i].size);
    g_origRebuild();
    strings = At<game::String*>(lexer, g_lexer.data);
    g_lexerCompared.fetch_add(1, std::memory_order_relaxed);
    for (int i = 0; i < wanted; ++i) {
        if (mine[i].size() == strings[i].size && memcmp(mine[i].data(), strings[i].data(), mine[i].size()) == 0) continue;
        if (g_lexerMismatches.fetch_add(1) < 20) {
            Log("token strings MISMATCH: token %d: hook \"%s\", game \"%.*s\"", i, mine[i].c_str(),
                static_cast<int>(strings[i].size), strings[i].data());
        }
    }
}

void InstallLexer(HMODULE self, const locate::Image& img) {
    std::string ini = IniPath(self);
    if (!ini.empty() && GetPrivateProfileIntA("speed", "fast_token_rebuild", 1, ini.c_str()) == 0) {
        Log("token strings: off (fast_token_rebuild=0)");
        return;
    }
    g_lexerVerify = !ini.empty() && GetPrivateProfileIntA("speed", "fast_token_rebuild_verify", 0, ini.c_str()) != 0;
    std::string error;
    if (!locate::LocateLexer(img, g_lexer, error)) {
        Log("token strings: %s; off", error.c_str());
        return;
    }
    Log("token strings: RebuildLookup +%llX; table +0x%zx (data +0x%zx, size +0x%zx), wanted +0x%zx, dynamic +0x%zx/+0x%zx",
        static_cast<unsigned long long>(g_lexer.rebuild - reinterpret_cast<uint8_t*>(g_base)), g_lexer.array, g_lexer.data,
        g_lexer.count, g_lexer.wanted, g_lexer.dynData, g_lexer.dynCount);
    auto tramp = InstallJumpHook(g_lexer.rebuild, locate::kRebuildLookupStolenBytes,
                                 reinterpret_cast<void*>(&RebuildLookupHook));
    if (!tramp) {
        Log("token strings: failed to hook RebuildLookup; off");
        return;
    }
    g_origRebuild = reinterpret_cast<game::RebuildLookupFn>(tramp);
    Log("token strings: hook installed%s", g_lexerVerify ? "; verifying every 16th call against the game's" : "");
}

std::string LexerSummary() {
    if (!g_origRebuild) return std::string();
    char buf[200];
    sprintf_s(buf, "token strings: %llu full rebuilds, %llu incremental (%llu tokens added)", g_lexerFull.load(),
              g_lexerIncremental.load(), g_lexerNew.load());
    std::string out = buf;
    if (g_lexerVerify) {
        sprintf_s(buf, "; verified %llu, %llu mismatches", g_lexerCompared.load(), g_lexerMismatches.load());
        out += buf;
    }
    return out;
}

// ---- Modifier documentation ----
//
// CModifier::LogDefinitions builds the text of logs/script_documentation/modifiers.log, one very
// long message, and logs it: seconds of the load, and nothing in the game reads it. The file is
// a reference for modders.
//
// [speed] skip_modifier_docs=0 writes it as usual.

void __fastcall LogDefinitionsHook() {}

void InstallModifierDocs(HMODULE self, const locate::Image& img) {
    std::string ini = IniPath(self);
    if (!ini.empty() && GetPrivateProfileIntA("speed", "skip_modifier_docs", 1, ini.c_str()) == 0) {
        Log("modifier documentation: written as usual (skip_modifier_docs=0)");
        return;
    }
    locate::ModifierDocsLocations docs;
    std::string error;
    if (!locate::LocateModifierDocs(img, docs, error)) {
        Log("modifier documentation: %s; written as usual", error.c_str());
        return;
    }
    if (!InstallJumpHook(docs.logDefinitions, locate::kLogDefinitionsStolenBytes, reinterpret_cast<void*>(&LogDefinitionsHook))) {
        Log("modifier documentation: failed to hook LogDefinitions; written as usual");
        return;
    }
    Log("modifier documentation: LogDefinitions +%llX skipped; modifiers.log stays empty",
        static_cast<unsigned long long>(docs.logDefinitions - reinterpret_cast<uint8_t*>(g_base)));
}

// ---- Deferred references ----
//
// While the databases load, every object that destroys a CGlobalDeferredDatabaseObject scans the
// whole list of deferred references for entries it owns (Unregister), and nearly all own none
// (re/load_time.md). Only Register_Internal adds to the list, so an owner it never saw has no
// entries, and Unregister would do nothing for it.
//
// The set of owners only has to contain every owner in the list; an owner in it needlessly costs
// the game's scan. Register_Internal adds after the game's function, and the set is emptied only
// when, under the lock, the list is empty: an entry added meanwhile can't be lost.

locate::DeferredRefLocations g_deferred;
game::DeferredRegisterFn g_origDeferredRegister = nullptr;
game::DeferredUnregisterFn g_origDeferredUnregister = nullptr;
SRWLOCK g_deferredLock = SRWLOCK_INIT;
std::unordered_set<const void*>* g_deferredOwners = nullptr;  // never freed: objects die in the exe's exit
bool g_deferredVerify = false;
std::atomic<uint64_t> g_deferredRegistered{0};
std::atomic<uint64_t> g_deferredCalls{0};
std::atomic<uint64_t> g_deferredSkipped{0};
std::atomic<uint64_t> g_deferredMismatches{0};

void __fastcall DeferredRegisterHook(uint8_t* resolver, const void* owner, void* a, void* b, void* c) {
    g_origDeferredRegister(resolver, owner, a, b, c);
    g_deferredRegistered.fetch_add(1, std::memory_order_relaxed);
    AcquireSRWLockExclusive(&g_deferredLock);
    g_deferredOwners->insert(owner);
    ReleaseSRWLockExclusive(&g_deferredLock);
}

int DeferredCount(const uint8_t* resolver) {
    return *reinterpret_cast<const int32_t*>(resolver + g_deferred.count);
}

void __fastcall DeferredUnregisterHook(uint8_t* resolver, const void* owner) {
    g_deferredCalls.fetch_add(1, std::memory_order_relaxed);
    if (DeferredCount(resolver) <= 0) {
        g_deferredSkipped.fetch_add(1, std::memory_order_relaxed);
        AcquireSRWLockShared(&g_deferredLock);
        bool stale = !g_deferredOwners->empty();
        ReleaseSRWLockShared(&g_deferredLock);
        if (stale) {
            AcquireSRWLockExclusive(&g_deferredLock);
            if (DeferredCount(resolver) <= 0) g_deferredOwners->clear();
            ReleaseSRWLockExclusive(&g_deferredLock);
        }
        return;
    }
    AcquireSRWLockShared(&g_deferredLock);
    bool known = g_deferredOwners->count(owner) != 0;
    ReleaseSRWLockShared(&g_deferredLock);
    if (known) {
        g_origDeferredUnregister(resolver, owner);
        return;
    }
    if (g_deferredVerify) {
        const uint8_t* data = *reinterpret_cast<uint8_t* const*>(resolver + g_deferred.data);
        for (int i = DeferredCount(resolver) - 1; i >= 0; --i) {
            if (*reinterpret_cast<const void* const*>(data + i * g_deferred.entrySize + g_deferred.owner) == owner) {
                if (g_deferredMismatches.fetch_add(1, std::memory_order_relaxed) == 0) {
                    Log("deferred references: MISMATCH, owner %p has an entry the set doesn't know", owner);
                }
                g_origDeferredUnregister(resolver, owner);
                return;
            }
        }
    }
    g_deferredSkipped.fetch_add(1, std::memory_order_relaxed);
}

void InstallDeferredRefs(HMODULE self, const locate::Image& img) {
    std::string ini = IniPath(self);
    if (!ini.empty() && GetPrivateProfileIntA("speed", "fast_deferred_unregister", 1, ini.c_str()) == 0) {
        Log("deferred references: off (fast_deferred_unregister=0)");
        return;
    }
    g_deferredVerify = !ini.empty() && GetPrivateProfileIntA("speed", "fast_deferred_unregister_verify", 0, ini.c_str()) != 0;
    std::string error;
    if (!locate::LocateDeferredRefs(img, g_deferred, error)) {
        Log("deferred references: %s; off", error.c_str());
        return;
    }
    Log("deferred references: Register_Internal +%llX, Unregister +%llX; list +0x%zx/+0x%zx, entries 0x%zx bytes, owner +0x%zx",
        static_cast<unsigned long long>(g_deferred.registerRef - reinterpret_cast<uint8_t*>(g_base)),
        static_cast<unsigned long long>(g_deferred.unregister - reinterpret_cast<uint8_t*>(g_base)), g_deferred.data,
        g_deferred.count, g_deferred.entrySize, g_deferred.owner);
    g_deferredOwners = new std::unordered_set<const void*>();
    // Nothing has registered yet at this point, so the set starts complete. Register first: with
    // only Unregister hooked, an owner it hasn't seen would wrongly be skipped.
    auto reg = InstallJumpHook(g_deferred.registerRef, locate::kDeferredRegisterStolenBytes,
                               reinterpret_cast<void*>(&DeferredRegisterHook));
    if (!reg) {
        Log("deferred references: failed to hook Register_Internal; off");
        return;
    }
    g_origDeferredRegister = reinterpret_cast<game::DeferredRegisterFn>(reg);
    auto unreg = InstallJumpHook(g_deferred.unregister, locate::kDeferredUnregisterStolenBytes,
                                 reinterpret_cast<void*>(&DeferredUnregisterHook));
    if (!unreg) {
        Log("deferred references: failed to hook Unregister; off");
        return;
    }
    g_origDeferredUnregister = reinterpret_cast<game::DeferredUnregisterFn>(unreg);
    Log("deferred references: hooks installed%s", g_deferredVerify ? ", verifying every skip against the list" : "");
}

std::string DeferredSummary() {
    if (!g_origDeferredUnregister) return std::string();
    char buf[200];
    uint64_t calls = g_deferredCalls.load(), skipped = g_deferredSkipped.load();
    sprintf_s(buf, "deferred references: %llu registered, %llu unregisters, %llu skipped (%.1f%%)",
              static_cast<unsigned long long>(g_deferredRegistered.load()), static_cast<unsigned long long>(calls),
              static_cast<unsigned long long>(skipped), calls ? 100.0 * skipped / calls : 0.0);
    std::string out = buf;
    if (g_deferredVerify) {
        sprintf_s(buf, ", %llu mismatches", static_cast<unsigned long long>(g_deferredMismatches.load()));
        out += buf;
    }
    return out;
}

// ---- File lookups ----
//
// The game's virtual file system (PhysFS) tries each mounted folder in turn, so a load makes over a
// million CreateFileW and GetFileAttributesExW calls that fail, nearly all because the folder doesn't
// exist in that mod (re/load_time.md). pathcache.h answers those from folder listings. The hooks
// sit in the exe's import table, so they need no patterns and see only the game's own calls.
//
// Only the game folder, workshop mods and the user folder's mod folder are answered: the rest of the
// user folder changes as the game runs. Calls from the exe that may change a folder drop its listing.
//
// [speed] fast_file_lookups=0 turns this off; fast_file_lookups_verify=1 also asks the file system
// for every answer, returns what it says, and logs any difference.

pathcache::Cache g_files;
bool g_filesVerify = false;
bool g_filesOn = false;
std::atomic<uint64_t> g_fileLookups{0}, g_fileAnswered{0}, g_fileCompared{0}, g_fileMismatches{0};

using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using GetFileAttributesExWFn = BOOL(WINAPI*)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
using CreateDirectoryWFn = BOOL(WINAPI*)(LPCWSTR, LPSECURITY_ATTRIBUTES);
using PathOnlyFn = BOOL(WINAPI*)(LPCWSTR);
using MoveFileExWFn = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, DWORD);
using MoveFileWFn = BOOL(WINAPI*)(LPCWSTR, LPCWSTR);
CreateFileWFn g_origCreateFileW = nullptr;
GetFileAttributesExWFn g_origGetAttributes = nullptr;
CreateDirectoryWFn g_origCreateDirectoryW = nullptr;
PathOnlyFn g_origDeleteFileW = nullptr, g_origRemoveDirectoryW = nullptr;
MoveFileExWFn g_origMoveFileExW = nullptr;
MoveFileWFn g_origMoveFileW = nullptr;

// The cache's error for a path, or 0 to ask the file system.
DWORD MissingError(LPCWSTR path) {
    g_fileLookups.fetch_add(1, std::memory_order_relaxed);
    switch (g_files.Query(path)) {
        case pathcache::Answer::MissingFile: return ERROR_FILE_NOT_FOUND;
        case pathcache::Answer::MissingPath: return ERROR_PATH_NOT_FOUND;
        default: return 0;
    }
}

void CompareFileAnswer(LPCWSTR path, DWORD cached, bool found, DWORD real) {
    g_fileCompared.fetch_add(1, std::memory_order_relaxed);
    if ((found || real != cached) && g_fileMismatches.fetch_add(1) < 20) {
        Log("file lookup MISMATCH: cache says error %lu, file system says %s %lu: %ls", cached, found ? "found" : "error",
            found ? 0 : real, path);
    }
}

HANDLE WINAPI CreateFileWHook(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security, DWORD disposition,
                              DWORD flags, HANDLE templateFile) {
    constexpr DWORD kWrites = GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE;
    if (!path) return g_origCreateFileW(path, access, share, security, disposition, flags, templateFile);
    if (disposition != OPEN_EXISTING || (access & kWrites)) {
        HANDLE h = g_origCreateFileW(path, access, share, security, disposition, flags, templateFile);
        DWORD error = GetLastError();
        g_files.Invalidate(path);
        SetLastError(error);
        return h;
    }
    DWORD cached = MissingError(path);
    if (cached == 0) return g_origCreateFileW(path, access, share, security, disposition, flags, templateFile);
    if (g_filesVerify) {
        HANDLE h = g_origCreateFileW(path, access, share, security, disposition, flags, templateFile);
        DWORD error = GetLastError();
        CompareFileAnswer(path, cached, h != INVALID_HANDLE_VALUE, error);
        SetLastError(error);
        return h;
    }
    g_fileAnswered.fetch_add(1, std::memory_order_relaxed);
    SetLastError(cached);
    return INVALID_HANDLE_VALUE;
}

BOOL WINAPI GetAttributesHook(LPCWSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID out) {
    if (!path) return g_origGetAttributes(path, level, out);
    DWORD cached = MissingError(path);
    if (cached == 0) return g_origGetAttributes(path, level, out);
    if (g_filesVerify) {
        BOOL ok = g_origGetAttributes(path, level, out);
        DWORD error = GetLastError();
        CompareFileAnswer(path, cached, ok != FALSE, error);
        SetLastError(error);
        return ok;
    }
    g_fileAnswered.fetch_add(1, std::memory_order_relaxed);
    SetLastError(cached);
    return FALSE;
}

// The calls that change a folder: run them, then forget the listings of the paths they touched.
void InvalidateKeepingError(LPCWSTR a, LPCWSTR b = nullptr) {
    DWORD error = GetLastError();
    if (a) g_files.Invalidate(a);
    if (b) g_files.Invalidate(b);
    SetLastError(error);
}
BOOL WINAPI CreateDirectoryWHook(LPCWSTR path, LPSECURITY_ATTRIBUTES security) {
    BOOL r = g_origCreateDirectoryW(path, security);
    InvalidateKeepingError(path);
    return r;
}
BOOL WINAPI DeleteFileWHook(LPCWSTR path) {
    BOOL r = g_origDeleteFileW(path);
    InvalidateKeepingError(path);
    return r;
}
BOOL WINAPI RemoveDirectoryWHook(LPCWSTR path) {
    BOOL r = g_origRemoveDirectoryW(path);
    InvalidateKeepingError(path);
    return r;
}
BOOL WINAPI MoveFileExWHook(LPCWSTR from, LPCWSTR to, DWORD flags) {
    BOOL r = g_origMoveFileExW(from, to, flags);
    InvalidateKeepingError(from, to);
    return r;
}
BOOL WINAPI MoveFileWHook(LPCWSTR from, LPCWSTR to) {
    BOOL r = g_origMoveFileW(from, to);
    InvalidateKeepingError(from, to);
    return r;
}

// Points the exe's import of a KERNEL32 function at `hook`. False if the exe doesn't import it.
bool HookKernel32Import(const char* function, void* hook, void** original) {
    auto base = reinterpret_cast<uint8_t*>(g_base);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    for (auto d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
        if (_stricmp(reinterpret_cast<const char*>(base + d->Name), "KERNEL32.dll") != 0 || !d->OriginalFirstThunk) continue;
        auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->OriginalFirstThunk);
        auto slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(byName->Name, function) != 0) continue;
            DWORD old;
            if (!VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) return false;
            *original = reinterpret_cast<void*>(slots->u1.Function);
            slots->u1.Function = reinterpret_cast<ULONGLONG>(hook);
            VirtualProtect(&slots->u1.Function, sizeof(void*), old, &old);
            return true;
        }
    }
    return false;
}

void InstallFileLookups(HMODULE self) {
    std::string ini = IniPath(self);
    if (!ini.empty() && GetPrivateProfileIntA("speed", "fast_file_lookups", 1, ini.c_str()) == 0) {
        Log("file lookups: off (fast_file_lookups=0)");
        return;
    }
    g_filesVerify = !ini.empty() && GetPrivateProfileIntA("speed", "fast_file_lookups_verify", 0, ini.c_str()) != 0;
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        Log("file lookups: no exe path; off");
        return;
    }
    if (wchar_t* slash = wcsrchr(exe, L'\\')) *slash = 0;
    g_files.AddRoot(exe);
    g_files.AddRootMarker(L"\\workshop\\content\\281990\\");
    g_files.AddRootMarker(L"\\paradox interactive\\stellaris\\mod\\");
    // The calls that change folders first: nothing is answered until the two lookups are hooked.
    struct Import {
        const char* name;
        void* hook;
        void** original;
    };
    const Import writers[] = {
        {"CreateDirectoryW", reinterpret_cast<void*>(&CreateDirectoryWHook), reinterpret_cast<void**>(&g_origCreateDirectoryW)},
        {"DeleteFileW", reinterpret_cast<void*>(&DeleteFileWHook), reinterpret_cast<void**>(&g_origDeleteFileW)},
        {"RemoveDirectoryW", reinterpret_cast<void*>(&RemoveDirectoryWHook), reinterpret_cast<void**>(&g_origRemoveDirectoryW)},
        {"MoveFileExW", reinterpret_cast<void*>(&MoveFileExWHook), reinterpret_cast<void**>(&g_origMoveFileExW)},
        {"MoveFileW", reinterpret_cast<void*>(&MoveFileWHook), reinterpret_cast<void**>(&g_origMoveFileW)},
    };
    int writerHooks = 0;
    for (const Import& i : writers) writerHooks += HookKernel32Import(i.name, i.hook, i.original);
    // CreateFileW must be in place for the cache to hear of the game's writes: without it, nothing
    // is answered, and GetFileAttributesExW isn't touched.
    if (!HookKernel32Import("CreateFileW", reinterpret_cast<void*>(&CreateFileWHook), reinterpret_cast<void**>(&g_origCreateFileW))) {
        Log("file lookups: CreateFileW isn't imported; off");
        return;
    }
    if (!HookKernel32Import("GetFileAttributesExW", reinterpret_cast<void*>(&GetAttributesHook), reinterpret_cast<void**>(&g_origGetAttributes))) {
        Log("file lookups: GetFileAttributesExW isn't imported; only opens are answered");
    }
    g_filesOn = true;
    Log("file lookups: import hooks installed (%d of 5 calls that change folders); game folder %ls%s", writerHooks, exe,
        g_filesVerify ? "; verifying every answer against the file system" : "");
}

std::string FileLookupSummary() {
    if (!g_filesOn) return std::string();
    char buf[220];
    uint64_t lookups = g_fileLookups.load();
    uint64_t answered = g_filesVerify ? g_fileCompared.load() : g_fileAnswered.load();
    sprintf_s(buf, "file lookups: %llu, %llu answered as missing (%.1f%%), %llu folder listings", lookups, answered,
              lookups ? 100.0 * answered / lookups : 0.0, g_files.Listings());
    std::string out = buf;
    if (g_filesVerify) {
        sprintf_s(buf, "; verified, %llu mismatches", g_fileMismatches.load());
        out += buf;
    }
    return out;
}

std::string IntelSummary() {
    if (!g_origSearch) return std::string();
    char buf[200];
    sprintf_s(buf, "fleet intel: %llu searches, %llu fast lookups%s", g_intelSearches.load(), g_intelFast.load(),
              g_intelFailed ? " (OFF after a fault)" : "");
    std::string out = buf;
    if (g_intelVerify) {
        sprintf_s(buf, "; verified %llu, %llu mismatches", g_intelCompared.load(), g_intelMismatches.load());
        out += buf;
    }
    return out;
}

// ===========================================================================
// Tick pacing and the benchmark
//
// The game simulates and draws on one thread. Two settings ([speed] in stellaris-engine-fixes.ini) give the
// simulation more of it, without changing what it computes:
//   in_tick_frame_ms  frames the game draws from inside a tick (RenderFrame, "frame smoothing")
//                     are drawn at most once per this many ms; the game draws one whenever a
//                     refresh interval has passed (about 11 ms at a 90 Hz cap).
//   batch_ticks       at the fastest speed, CheckTick queues the ticks up to the next AI tick
//                     (tick 4) or day tick (tick 10) per frame instead of NUM_COMMANDS_PER_TURN (2):
//                     2 frames per day instead of 5. Commands the AI posts on its tick, and those
//                     posted on the day tick, still run before the next tick, as with the game's 2.
//
// The console command `sef_bench [seconds]` measures for that much wall time (default 60): the
// game ticks and days that pass, and where the main thread's time goes: inside ticks
// (HandleTurnTick), in frames drawn from inside a tick, and between ticks (the normal frames: UI,
// rendering, the frame limiter). `sef_bench stop` ends a run early and `sef_bench show` prints the
// last results. A run that ends by itself prints its results by running `sef_bench show`, the way
// the game's own commands run another command (OnExecute_UnchartedSpace). `sef_bench combat
// off|on` switches skip_empty_combat_entries (above) for A/B runs.
// See re/simulation_speed.md.

struct PacingSettings {
    int inTickFrameMs = 33;  // 0: the game's own rate
    bool batchTicks = true;
};
PacingSettings g_pacing;
LONGLONG g_minFrameGap = 0;  // in_tick_frame_ms in QPC ticks
LONGLONG g_lastFrame = 0;    // when the last frame (normal or drawn inside a tick) started

locate::TickLocations g_bench;
game::ExecuteFn g_origExecute = nullptr;
game::CheckTickFn g_origCheckTick = nullptr;
game::TurnTickFn g_origTurnTick = nullptr;
game::RenderFrameFn g_origRenderFrame = nullptr;
DWORD g_mainThread = 0;

int g_renderDepth = 0;  // RenderFrame calls in progress: a CheckTick inside one is a nested frame
bool g_inTurnTick = false;

struct BenchRun {
    bool active = false;
    double seconds = 0;
    LONGLONG start = 0, deadline = 0, lastFrame = 0;
    int32_t speed = -1;  // the game speed index at the run's first frame
    bool speedChanged = false;
    bool smoothingAtStart = false;
    uint64_t ticks = 0, days = 0;
    uint64_t frames = 0;                           // normal frames, not paused
    uint64_t insideFrames = 0, betweenFrames = 0;  // drawn by RenderFrame inside a tick, or between commands
    uint64_t skippedFrames = 0;                    // RenderFrame calls skipped by in_tick_frame_ms
    uint64_t batches = 0;                          // CheckTick calls that queued a batch of ticks
    uint64_t intelSearches = 0, intelFast = 0;     // the fleet intel counters at the start
    uint64_t combatInserts = 0, combatSkipped = 0; // the combat map counters at the start
    LONGLONG tickTime = 0, insideTime = 0, betweenTime = 0, pausedTime = 0, longestTick = 0;
    int32_t firstDate = 0, lastDate = 0, longestTickDate = 0;
    bool haveDate = false;
};
BenchRun g_run;
std::string g_benchReport;  // the last run's results
bool g_benchPrintPending = false;

double Seconds(LONGLONG t) { return static_cast<double>(t) / g_qpcFreq.QuadPart; }

bool ReadInt(const uint8_t* p, int32_t* out) {
    __try {
        *out = *reinterpret_cast<const int32_t*>(p);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Sets a flag for the duration of a call, also when an exception passes through.
struct FlagScope {
    bool& flag;
    explicit FlagScope(bool& f) : flag(f) { flag = true; }
    ~FlagScope() { flag = false; }
};
struct DepthScope {
    int& depth;
    explicit DepthScope(int& d) : depth(d) { ++depth; }
    ~DepthScope() { --depth; }
};

void __fastcall TurnTickHook(uint8_t* state, void* checksums) {
    if (GetCurrentThreadId() != g_mainThread || g_inTurnTick) {  // neither happens in the game
        g_origTurnTick(state, checksums);
        return;
    }
    bool measure = g_run.active;
    int32_t before = 0;
    bool dated = measure && ReadInt(state + g_bench.stateDate, &before);
    LONGLONG t0 = measure ? Now() : 0;
    {
        FlagScope inTick(g_inTurnTick);
        g_origTurnTick(state, checksums);
    }
    if (!measure || !g_run.active) return;  // `sef_bench stop` can run in a frame drawn during the tick
    LONGLONG t = Now() - t0;
    ++g_run.ticks;
    g_run.tickTime += t;
    int32_t after = 0;
    if (dated && ReadInt(state + g_bench.stateDate, &after)) {
        if (!g_run.haveDate) {
            g_run.firstDate = before;
            g_run.haveDate = true;
        }
        g_run.lastDate = after;
        if (after > before) g_run.days += (after - before) / 24;
        if (t > g_run.longestTick) {
            g_run.longestTick = t;
            g_run.longestTickDate = after;
        }
    }
}

void __fastcall RenderFrameHook(void* a, void* b, void* c, void* d) {
    if (GetCurrentThreadId() != g_mainThread) {
        g_origRenderFrame(a, b, c, d);
        return;
    }
    LONGLONG t0 = Now();
    if (g_renderDepth == 0) {
        // Skipping the frame is what the game does when its own check says no frame is due yet.
        if (g_minFrameGap > 0 && t0 - g_lastFrame < g_minFrameGap) {
            if (g_run.active) ++g_run.skippedFrames;
            return;
        }
        g_lastFrame = t0;
    }
    bool measure = g_run.active && g_renderDepth == 0;
    {
        DepthScope depth(g_renderDepth);
        g_origRenderFrame(a, b, c, d);
    }
    if (!measure || !g_run.active) return;
    LONGLONG t = Now() - t0;
    if (g_inTurnTick) {
        ++g_run.insideFrames;
        g_run.insideTime += t;
    } else {
        ++g_run.betweenFrames;
        g_run.betweenTime += t;
    }
}

void AddLine(std::string& out, const char* fmt, ...) {
    char line[512];
    va_list args;
    va_start(args, fmt);
    vsprintf_s(line, fmt, args);
    va_end(args);
    Log("%s", line);
    if (!out.empty()) out += '\n';
    out += line;
}

// Ends the run and writes its results to g_benchReport and the log.
void FinishBench(LONGLONG now) {
    BenchRun& r = g_run;
    r.active = false;
    double wall = Seconds(now - r.start);
    double paused = (std::min)(Seconds(r.pausedTime), wall);
    double active = (std::max)(wall - paused, 1e-6);
    double inTicks = Seconds(r.tickTime), inside = Seconds(r.insideTime), between = Seconds(r.betweenTime);
    double sim = (std::max)(inTicks - inside, 0.0);
    double rest = (std::max)(active - inTicks, 0.0);    // between ticks, frames drawn between commands included
    double normal = (std::max)(rest - between, 0.0);  // the normal frames
    auto pct = [&](double s) { return 100.0 * s / active; };
    double days = static_cast<double>(r.days);
    bool smoothing = *g_bench.frameSmoothing != 0;
    float turnTime = r.speed >= 0 && r.speed < 8 ? g_bench.speedTurnTimes[r.speed] : -1.0f;

    std::string out;
    char pausedNote[64] = "";
    if (paused >= 0.05) sprintf_s(pausedNote, " (%.1f s paused, not counted)", paused);
    char speedNote[48] = "";
    if (turnTime == 0.0f) {
        strcpy_s(speedNote, " (no wait between ticks)");
    } else if (turnTime > 0.0f) {
        sprintf_s(speedNote, " (%.3f s between batches of ticks)", turnTime);
    }
    AddLine(out, "sef_bench: %.1f s%s, game speed %d%s, frame smoothing %s", active, pausedNote, r.speed + 1,
            speedNote, smoothing ? "on" : "off");
    char throttleNote[96] = "the game's rate";
    if (g_pacing.inTickFrameMs > 0) {
        sprintf_s(throttleNote, "at most one per %d ms (%llu skipped)", g_pacing.inTickFrameMs, r.skippedFrames);
    }
    char intelNote[96] = "off";
    if (g_intelOn) {
        sprintf_s(intelNote, "on%s (%llu searches, %llu lookups)", g_intelVerify ? ", verifying" : "",
                  g_intelSearches.load() - r.intelSearches, g_intelFast.load() - r.intelFast);
    }
    AddLine(out, "stellaris-engine-fixes: frames inside ticks %s; tick batching %s (%llu batches); fast fleet intel %s", throttleNote,
            g_pacing.batchTicks ? "on" : "off", r.batches, intelNote);
    if (g_origCombatInsert) {
        AddLine(out, "stellaris-engine-fixes: skip empty combat entries %s (%llu inserts, %llu skipped)",
                g_combatWanted ? "on" : "off", g_combatInserts - r.combatInserts, g_combatSkipped - r.combatSkipped);
    }
    if (r.days == 0) {
        AddLine(out, "%llu ticks, no full day passed: was the game paused?", r.ticks);
    } else {
        AddLine(out, "%s to %s: %llu days, %llu ticks; %.2f days per second, %.0f ms per day",
                FormatDate(r.firstDate).c_str(), FormatDate(r.lastDate).c_str(), r.days, r.ticks, days / active,
                1e3 * active / days);
    }
    AddLine(out, "in ticks %.1f s (%.0f%%): simulation %.1f s (%.0f%%), %llu frames drawn inside ticks %.1f s (%.0f%%, %.1f ms each)",
            inTicks, pct(inTicks), sim, pct(sim), r.insideFrames, inside, pct(inside),
            r.insideFrames ? 1e3 * inside / r.insideFrames : 0.0);
    char betweenNote[96] = "";
    if (r.betweenFrames) {
        sprintf_s(betweenNote, ", plus %llu frames drawn between commands %.1f s", r.betweenFrames, between);
    }
    AddLine(out, "between ticks %.1f s (%.0f%%): %llu frames, %.1f ms each, %.1f ticks per frame%s", rest, pct(rest),
            r.frames, r.frames ? 1e3 * normal / r.frames : 0.0, r.frames ? static_cast<double>(r.ticks) / r.frames : 0.0,
            betweenNote);
    if (r.days) {
        AddLine(out, "per day: simulation %.0f ms + frames inside ticks %.0f ms + between ticks %.0f ms; %.1f frames per day",
                1e3 * sim / days, 1e3 * inside / days, 1e3 * rest / days,
                static_cast<double>(r.frames + r.insideFrames + r.betweenFrames) / days);
    }
    if (r.ticks) {
        AddLine(out, "longest tick %.0f ms (%s)", 1e3 * Seconds(r.longestTick), FormatDate(r.longestTickDate).c_str());
    }
    if (r.speedChanged) AddLine(out, "the game speed changed during the run");
    if (smoothing != r.smoothingAtStart) AddLine(out, "frame smoothing was switched during the run");
    g_benchReport = out;
}

// Runs one of our own commands so its reply reaches the console, without recording it
// in the console's history or console_history.txt.
template <size_t N>
void RunOwnCommand(const char (&text)[N]) {
    static_assert(N <= 16, "fits the inline buffer");
    uint8_t* console = *g_bench.console;
    if (!console) return;
    game::String line{};
    memcpy(line.buf, text, N);
    line.size = N - 1;
    line.cap = 0xF;
    uint8_t history = *g_bench.addToHistory;
    *g_bench.addToHistory = 0;
    reinterpret_cast<game::RunCommandFn>(g_bench.runCommand)(console, &line);
    *g_bench.addToHistory = history;
}

void PrintBenchReport() { RunOwnCommand("sef_bench show"); }

// Once per normal frame (not for frames drawn from inside the simulation).
void BenchFrame(uint8_t* idler) {
    if (g_run.active) {
        LONGLONG now = Now();
        int32_t speed = -1;
        ReadInt(idler + g_bench.idlerSpeed, &speed);
        if (idler[g_bench.idlerPaused]) {
            g_run.pausedTime += now - g_run.lastFrame;
        } else {
            ++g_run.frames;
        }
        if (g_run.speed < 0) {
            g_run.speed = speed;
        } else if (speed != g_run.speed) {
            g_run.speedChanged = true;
        }
        g_run.lastFrame = now;
        if (now >= g_run.deadline) {
            FinishBench(now);
            g_benchPrintPending = true;
        }
    }
    if (g_benchPrintPending) {
        g_benchPrintPending = false;
        PrintBenchReport();
    }
}

// The ticks to queue this frame with batch_ticks, or 0 to leave the game's count: at the fastest
// speed in single player, the ticks up to and including the next AI tick (4) or day tick (10).
int TickBatch(uint8_t* idler) {
    if (!g_pacing.batchTicks || idler[g_bench.idlerMultiplayer] || *g_bench.ticksPerTurn != 1) return 0;
    int32_t speed = -1, tick = -1;
    if (!ReadInt(idler + g_bench.idlerSpeed, &speed) || speed < 0 || speed > 15) return 0;
    if (g_bench.speedTurnTimes[speed] != 0.0f) return 0;  // only the fastest speed has no wait
    uint8_t* state = *g_bench.gameState;
    if (!state || !ReadInt(state + g_bench.stateTick, &tick) || tick < 0 || tick > 9) return 0;
    return tick < 4 ? 4 - tick : 10 - tick;  // the tick count is 1..10 after a tick's increment
}

uint64_t __fastcall CheckTickHook(uint8_t* idler, float frameSeconds) {
    if (g_renderDepth != 0 || GetCurrentThreadId() != g_mainThread) return g_origCheckTick(idler, frameSeconds);
    g_lastFrame = Now();
    BenchFrame(idler);
    StoreFrame();
    int batch = TickBatch(idler);
    if (batch == 0) return g_origCheckTick(idler, frameSeconds);
    // CheckTick queues max(min(NUM_COMMANDS_PER_TURN, 10), g_nTicksPerTurn) ticks; the define goes
    // back to its value right after, so nothing else sees the change.
    int32_t defined = *g_bench.commandsPerTurn;
    *g_bench.commandsPerTurn = batch;
    uint64_t queued = g_origCheckTick(idler, frameSeconds);
    *g_bench.commandsPerTurn = defined;
    if ((queued & 0xFF) && g_run.active) ++g_run.batches;
    return queued;
}

// The reply to `sef_bench <arg>`; false makes the console draw it in red.
bool BenchCommand(const std::string& arg, std::string& reply) {
    if (_stricmp(arg.c_str(), "show") == 0) {
        reply = g_benchReport.empty() ? "sef_bench: no results yet" : g_benchReport;
        return !g_benchReport.empty();
    }
    if (_strnicmp(arg.c_str(), "combat", 6) == 0) {
        std::string value = arg.size() > 6 && arg[6] == ' ' ? arg.substr(7) : std::string();
        if (!g_origCombatInsert) {
            reply = "sef_bench: skipping empty combat entries is not installed (see stellaris-engine-fixes.ini)";
            return false;
        }
        if (_stricmp(value.c_str(), "on") == 0 || _stricmp(value.c_str(), "off") == 0) {
            g_combatWanted = _stricmp(value.c_str(), "on") == 0;
            Log("sef_bench: skip empty combat entries %s", g_combatWanted ? "on" : "off");
        } else if (!value.empty()) {
            reply = "usage: sef_bench combat [on | off]";
            return false;
        }
        reply = std::string("sef_bench: skip empty combat entries ") + (g_combatWanted ? "on" : "off") +
                (g_run.active ? " (a run is in progress: its results mix both)" : "");
        return true;
    }
    if (_stricmp(arg.c_str(), "stop") == 0) {
        if (!g_run.active) {
            reply = "sef_bench: not running";
            return false;
        }
        FinishBench(Now());
        reply = g_benchReport;
        return true;
    }
    if (g_run.active) {
        char buf[160];
        sprintf_s(buf, "sef_bench: running, %.0f s left, %llu days so far (sef_bench stop ends it now)",
                  (std::max)(0.0, Seconds(g_run.deadline - Now())), g_run.days);
        reply = buf;
        return true;
    }
    double seconds = 60;
    if (!arg.empty()) {
        char* end = nullptr;
        seconds = strtod(arg.c_str(), &end);
        if (*end != '\0' || seconds < 5 || seconds > 3600) {
            reply = "usage: sef_bench [seconds, 5 to 3600, default 60] | sef_bench stop | sef_bench show | "
                    "sef_bench combat [on | off]";
            return false;
        }
    }
    g_run = BenchRun{};
    g_run.active = true;
    g_run.seconds = seconds;
    g_run.start = g_run.lastFrame = Now();
    g_run.deadline = g_run.start + static_cast<LONGLONG>(seconds * g_qpcFreq.QuadPart);
    g_run.smoothingAtStart = *g_bench.frameSmoothing != 0;
    g_run.intelSearches = g_intelSearches.load();
    g_run.intelFast = g_intelFast.load();
    g_run.combatInserts = g_combatInserts;
    g_run.combatSkipped = g_combatSkipped;
    Log("sef_bench: started, %.0f s", seconds);
    char buf[200];
    sprintf_s(buf, "sef_bench: measuring for %.0f s; the results are printed here when it ends. "
                   "Leave the game running at the speed to measure.", seconds);
    reply = buf;
    return true;
}

bool SameText(const game::String& s, const char* text) {
    size_t n = strlen(text);
    return s.size == n && _strnicmp(s.data(), text, n) == 0;
}

game::ConsoleResult* Reply(game::ConsoleResult* out, bool ok, const std::string& text) {
    memset(out, 0, sizeof(*out));
    out->ok = ok;
    game::String& s = out->text;
    if (text.size() <= 0xF) {
        memcpy(s.buf, text.data(), text.size());
        s.cap = 0xF;
    } else {
        // Kind 1: the game never frees it. Not freed here either: a few hundred bytes per command.
        char* p = new char[text.size() + 1];
        memcpy(p, text.c_str(), text.size() + 1);
        s.kind = 1;
        s.ptr = p;
        s.cap = text.size();
    }
    s.size = text.size();
    return out;
}

game::ConsoleResult* __fastcall ExecuteHook(void* manager, game::ConsoleResult* out, const uint8_t* args) {
    auto items = *reinterpret_cast<game::String* const*>(args + game::kArrayDataOffset);
    int count = *reinterpret_cast<const int*>(args + game::kArrayCountOffset);
    if (count < 1 || !items) return g_origExecute(manager, out, args);
    if (!SameText(items[0], "sef_bench")) return g_origExecute(manager, out, args);
    std::string arg;
    for (int i = 1; i < count; ++i) {
        if (i > 1) arg += ' ';
        arg.append(items[i].data(), items[i].size);
    }
    std::string reply;
    if (static_cast<uint8_t*>(manager)[g_bench.consoleLockedFlag]) {
        return Reply(out, false, "sef_bench: not available in multiplayer or ironman");
    }
    bool ok = BenchCommand(arg, reply);
    return Reply(out, ok, reply);
}

// [speed] of stellaris-engine-fixes.ini next to this DLL; the defaults when it is missing.
void ReadPacingSettings(HMODULE self) {
    std::string ini = IniPath(self);
    if (ini.empty()) return;
    const char* path = ini.c_str();
    int ms = static_cast<int>(GetPrivateProfileIntA("speed", "in_tick_frame_ms", g_pacing.inTickFrameMs, path));
    g_pacing.inTickFrameMs = (std::max)(0, (std::min)(ms, 1000));
    g_pacing.batchTicks = GetPrivateProfileIntA("speed", "batch_ticks", g_pacing.batchTicks ? 1 : 0, path) != 0;
}

void InstallBench(HMODULE self, const locate::Image& img) {
    std::string error;
    if (!locate::LocateTicks(img, g_bench, error)) {
        Log("ticks: %s; tick pacing and the sef_bench command are off", error.c_str());
        return;
    }
    ReadPacingSettings(self);
    Log("ticks: idler speed +0x%zx, paused +0x%zx, multiplayer +0x%zx; game state tick +0x%zx, date +0x%zx; console lock flag +0x%zx",
        g_bench.idlerSpeed, g_bench.idlerPaused, g_bench.idlerMultiplayer, g_bench.stateTick, g_bench.stateDate,
        g_bench.consoleLockedFlag);
    g_mainThread = GetCurrentThreadId();  // DllMain runs on the game's main thread
    struct Hook {
        const char* what;
        uint8_t* target;
        size_t stolen;
        void* hook;
        void** orig;
    } hooks[] = {
        // The console command last: until it is in place nothing starts a run.
        {"RenderFrame", g_bench.renderFrame, locate::kRenderFrameStolenBytes, reinterpret_cast<void*>(&RenderFrameHook),
         reinterpret_cast<void**>(&g_origRenderFrame)},
        {"HandleTurnTick", g_bench.turnTick, locate::kTurnTickStolenBytes, reinterpret_cast<void*>(&TurnTickHook),
         reinterpret_cast<void**>(&g_origTurnTick)},
        {"CheckTick", g_bench.checkTick, locate::kCheckTickStolenBytes, reinterpret_cast<void*>(&CheckTickHook),
         reinterpret_cast<void**>(&g_origCheckTick)},
        {"CConsoleCmdManager::Execute", g_bench.execute, locate::kConsoleExecuteStolenBytes,
         reinterpret_cast<void*>(&ExecuteHook), reinterpret_cast<void**>(&g_origExecute)},
    };
    for (Hook& h : hooks) {
        uint8_t* tramp = InstallJumpHook(h.target, h.stolen, h.hook);
        if (!tramp) {
            // The hooks already in place pass everything through with pacing off.
            Log("ticks: failed to hook %s; tick pacing and the sef_bench command are off", h.what);
            g_pacing = PacingSettings{0, false};
            return;
        }
        *h.orig = tramp;
    }
    g_minFrameGap = g_qpcFreq.QuadPart * g_pacing.inTickFrameMs / 1000;
    Log("ticks: hooks installed; frames inside ticks %s%d ms, tick batching at the fastest speed %s; console command sef_bench",
        g_pacing.inTickFrameMs ? "at most one per " : "at the game's rate, ", g_pacing.inTickFrameMs,
        g_pacing.batchTicks ? "on" : "off");
}

// ===========================================================================
// The country AI's gameplay fixes (re/ai.md, section 2; [ai_fixes] in stellaris-engine-fixes.ini)
//
// Each fix changes how AI empires play, so each is off unless its setting is 1. None changes the
// save format. They run on the AI's side only, so in multiplayer every player needs the same ones
// (untested there).
//
//   build_plan_keep        2.1  the monthly build plan lasts the month instead of one week
//   opportunist_wars       2.3  "opportunist" counts the target's wars, not our own
//   war_distance_slope     2.2  the war score's distance malus per jump above the malus distance
//   truce_war_divisor=N    2.4  a truce divides war appetite by N, not by AI_AGGRESSIVENESS_BOXED_IN_MULT
//   expansion_refresh_monthly  2.5  the colonize/expansion lists are rebuilt every month, all game
//   ai_schedule_from_date  2.8/2.9  weekly, monthly and yearly AI updates on fixed days of the calendar
//   ai_random_from_save    2.9  a loaded AI continues its saved random stream

locate::AiFixLocations g_ai;
bool g_aiDefinesWanted = false;  // log the defines the war distance and truce fixes use, once loaded

// Executable memory within 2 GB of `nearTo`, for code and data that the game's rel32 operands
// reach. Reserved 64 KB at a time; this hands out pieces of one block.
uint8_t* AllocNear(const uint8_t* nearTo, size_t size) {
    static uint8_t* block = nullptr;
    static size_t used = 0;
    constexpr size_t kBlock = 0x10000;
    size = (size + 15) & ~size_t(15);
    if (block && used + size <= kBlock) {
        uint8_t* p = block + used;
        used += size;
        return p;
    }
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity;
    const uintptr_t center = reinterpret_cast<uintptr_t>(nearTo) & ~(gran - 1);
    const uintptr_t range = 0x70000000;  // well inside rel32's reach from anywhere in the image
    for (uintptr_t delta = gran; delta < range; delta += gran) {
        for (int side = 0; side < 2; ++side) {
            uintptr_t at = side ? center + delta : center - delta;
            if (side == 0 && center < delta) continue;
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(reinterpret_cast<void*>(at), &mbi, sizeof(mbi)) || mbi.State != MEM_FREE) continue;
            void* p = VirtualAlloc(reinterpret_cast<void*>(at), kBlock, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (!p) continue;
            block = static_cast<uint8_t*>(p);
            used = size;
            return block;
        }
    }
    return nullptr;
}

// rel32 from the end of an instruction at `next` to `target`, if it fits.
bool Rel32(const uint8_t* next, const void* target, int32_t* out) {
    ptrdiff_t d = static_cast<const uint8_t*>(target) - next;
    if (d < INT32_MIN || d > INT32_MAX) return false;
    *out = static_cast<int32_t>(d);
    return true;
}

// Overwrites game code.
bool WriteCode(uint8_t* at, const void* bytes, size_t n) {
    DWORD old;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(at, bytes, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
    return true;
}

// Machine code, written into a nearTo or far stub.
struct Code {
    uint8_t bytes[128];
    size_t n = 0;
    Code& Put(std::initializer_list<uint8_t> b) {
        for (uint8_t x : b) bytes[n++] = x;
        return *this;
    }
    Code& Put32(uint32_t v) {
        memcpy(bytes + n, &v, 4);
        n += 4;
        return *this;
    }
    Code& Put64(uint64_t v) {
        memcpy(bytes + n, &v, 8);
        n += 8;
        return *this;
    }
    Code& JumpTo(const void* to) {  // jmp qword [rip+0]; dq to
        Put({0xFF, 0x25, 0, 0, 0, 0});
        return Put64(reinterpret_cast<uint64_t>(to));
    }
};

uint8_t* PlaceStub(const Code& code, const uint8_t* nearTo) {
    uint8_t* stub = nearTo ? AllocNear(nearTo, code.n)
                         : static_cast<uint8_t*>(VirtualAlloc(nullptr, code.n, MEM_COMMIT | MEM_RESERVE,
                                                              PAGE_EXECUTE_READWRITE));
    if (stub) {
        memcpy(stub, code.bytes, code.n);
        FlushInstructionCache(GetCurrentProcess(), stub, code.n);
    }
    return stub;
}

int AiIni(const std::string& ini, const char* key) {
    return ini.empty() ? 0 : static_cast<int>(GetPrivateProfileIntA("ai_fixes", key, 0, ini.c_str()));
}

// ---- 2.1 The build plan --------------------------------------------------------------------
//
// The walk (BuildAndUpgradeDistrictsAndBuildings, weekly) orders items from the top of the plan
// until one can't be placed, then the game deletes the whole plan; it is made again only on the
// AI's month. With the fix:
// - before the walk, items whose colony is unusable or already builds a district or building go to
//   the end of the plan, in their order. The walk would stop at them; now it gets past them and
//   stops at them only at the end. They stay for later weeks.
// - after the walk, the items it ordered (moved out, so null) are dropped and the rest is kept,
//   instead of deleting it. The monthly CreateBuildPlan clears the plan before making a new one.
// The plan's weights are a month old at most, and every item is checked again before it is ordered.
// With the plan's debug log on, its explanation lines are numbered by the old order.

struct PlanItem {
    uint8_t* buildable;
    int64_t weight;
};
using PlanWalkFn = void(__fastcall*)(uint8_t* minister);
using PlanClearFn = void(__fastcall*)(uint8_t* array);
using ResolveColonyFn = uint8_t*(__fastcall*)(uint8_t* buildable);
using ColonyCheckFn = bool(__fastcall*)(uint8_t* sub);
using HasQueuedFn = bool(__fastcall*)(uint8_t* colony);

PlanWalkFn g_origPlanWalk = nullptr;
bool g_planFailed = false;  // a fault: back to the game's behaviour from the next call
std::atomic<uint64_t> g_planWalks{0}, g_planDeferred{0}, g_planOrdered{0}, g_planKept{0};

bool PlanItemBlocked(uint8_t* buildable) {
    if (buildable == *g_ai.nullBuildable) return true;
    uint8_t* colony = reinterpret_cast<ResolveColonyFn>(g_ai.resolveColony)(buildable);
    if (!colony) return true;
    uint8_t* sub = colony + g_ai.colonyCheckObject;
    auto check = *reinterpret_cast<ColonyCheckFn*>(*reinterpret_cast<uint8_t**>(sub) + g_ai.colonyCheckSlot);
    if (!check(sub)) return true;
    return reinterpret_cast<HasQueuedFn>(g_ai.hasQueued)(colony);
}

// Moves the blocked items to the end, keeping the order of both groups. `spill` holds `count`.
// Returns how many moved, or -1 on a fault.
int DeferBlockedItems(PlanItem* items, int count, PlanItem* spill) {
    __try {
        int kept = 0, spilled = 0;
        for (int i = 0; i < count; ++i) {
            PlanItem item = items[i];
            if (item.buildable && PlanItemBlocked(item.buildable)) {
                spill[spilled++] = item;
            } else {
                items[kept++] = item;
            }
        }
        memcpy(items + kept, spill, sizeof(PlanItem) * spilled);
        return spilled;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// The minister's plan and its size, or false on a fault.
bool ReadPlan(uint8_t* minister, uint8_t** array, int* count) {
    __try {
        *array = nullptr;
        *count = 0;
        uint8_t* strategy = *reinterpret_cast<uint8_t**>(minister + g_ai.ministerStrategy);
        if (strategy) {
            *array = strategy + g_ai.planArray;
            *count = *reinterpret_cast<int32_t*>(*array + g_ai.arrayCount);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __fastcall PlanWalkHook(uint8_t* minister) {
    thread_local std::vector<PlanItem> spill;
    uint8_t* array = nullptr;
    int count = 0;
    if (!g_planFailed && !ReadPlan(minister, &array, &count)) {
        g_planFailed = true;
        Log("AI build plan: fault while reading a plan; the game's behaviour from now on");
    }
    if (!g_planFailed && array && count > 0) {
        if (spill.size() < static_cast<size_t>(count)) spill.resize(count);
        auto items = *reinterpret_cast<PlanItem**>(array + g_ai.arrayData);
        int moved = DeferBlockedItems(items, count, spill.data());
        if (moved < 0) {
            g_planFailed = true;
            Log("AI build plan: fault while sorting a plan; the game's behaviour from now on");
        } else {
            g_planWalks.fetch_add(1, std::memory_order_relaxed);
            g_planDeferred.fetch_add(moved, std::memory_order_relaxed);
        }
    }
    g_origPlanWalk(minister);
}

// Replaces the walk's Clear: drops what the walk ordered, keeps the rest.
bool KeepUnorderedItems(uint8_t* array) {
    __try {
        auto items = *reinterpret_cast<PlanItem**>(array + g_ai.arrayData);
        auto count = reinterpret_cast<int32_t*>(array + g_ai.arrayCount);
        uint8_t* null = *g_ai.nullBuildable;
        int kept = 0;
        for (int i = 0; i < *count; ++i) {
            if (items[i].buildable && items[i].buildable != null) items[kept++] = items[i];
        }
        g_planOrdered.fetch_add(*count - kept, std::memory_order_relaxed);
        g_planKept.fetch_add(kept, std::memory_order_relaxed);
        *count = kept;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __fastcall PlanClearHook(uint8_t* array) {
    if (!g_planFailed && KeepUnorderedItems(array)) return;
    g_planFailed = true;
    reinterpret_cast<PlanClearFn>(g_ai.planClear)(array);
}

void InstallBuildPlanFix() {
    // The call first: until the walk is hooked too, it only keeps what the walk didn't order.
    Code jump;
    jump.JumpTo(reinterpret_cast<void*>(&PlanClearHook));
    uint8_t* stub = PlaceStub(jump, g_ai.planClearCall);
    int32_t rel;
    if (!stub || !Rel32(g_ai.planClearCall + 5, stub, &rel) || !WriteCode(g_ai.planClearCall + 1, &rel, 4)) {
        Log("AI build plan: no memory near the game's code; off");
        return;
    }
    uint8_t* tramp = InstallJumpHook(g_ai.planWalk, locate::kPlanWalkStolenBytes, reinterpret_cast<void*>(&PlanWalkHook));
    if (!tramp) {
        Log("AI build plan: failed to hook the walk; only the unordered items are kept");
        return;
    }
    g_origPlanWalk = reinterpret_cast<PlanWalkFn>(tramp);
    Log("AI build plan: on; the plan lasts until the AI's next month, blocked items wait at its end");
}

// ---- 2.3 The opportunist pass ------------------------------------------------------------------
//
// IsReadyForWarWith compares our side's fleet power with the target's. With the opportunist trait
// and a target at war, the game adds half the power of one side of each of OUR wars to us; at
// peace, which PrepareWar requires, that is nothing. The fix adds half the power of the target's
// enemies in each of THE TARGET's wars (except us): they are busy fighting it.

int64_t HalfPower(const uint8_t* country) {
    // The game's rounding: (power << 15) / 65536, towards zero.
    int64_t v = static_cast<int64_t>(static_cast<uint64_t>(*reinterpret_cast<const int64_t*>(country + g_ai.countryPower)) << 15);
    return (v + (v < 0 ? 0xFFFF : 0)) >> 16;
}

bool g_opportunistFailed = false;
std::atomic<uint64_t> g_opportunistCalls{0}, g_opportunistAdded{0};

int64_t __fastcall OpportunistPower(uint8_t* our, uint8_t* target) {
    if (g_opportunistFailed) return 0;
    __try {
        const uint8_t* personality = *reinterpret_cast<uint8_t**>(our + g_ai.countryPersonality);
        int wars = *reinterpret_cast<int32_t*>(target + g_ai.countryWarCount);
        if (!personality || !personality[g_ai.personalityOpportunist] || wars <= 0) return 0;
        g_opportunistCalls.fetch_add(1, std::memory_order_relaxed);
        const uint32_t* warIds = *reinterpret_cast<uint32_t**>(target + g_ai.countryWars);
        uint32_t targetId = *reinterpret_cast<uint32_t*>(target + g_ai.countryId);
        uint32_t ourId = *reinterpret_cast<uint32_t*>(our + g_ai.countryId);
        int64_t sum = 0;
        for (int w = 0; w < wars; ++w) {
            uint8_t* war = ResolveRef(*g_ai.warDb, warIds[w], g_ai.warId);
            if (!war) continue;
            auto members = [&](size_t side, int* n) {
                *n = *reinterpret_cast<int32_t*>(war + side + 0x14);
                return *reinterpret_cast<uint8_t**>(war + side + 8);
            };
            int n;
            uint8_t* attackers = members(g_ai.warAttackers, &n);
            bool targetAttacks = false;
            for (int i = 0; i < n && !targetAttacks; ++i) {
                targetAttacks = *reinterpret_cast<uint32_t*>(attackers + i * g_ai.memberStride + g_ai.memberId) == targetId;
            }
            uint8_t* enemies = members(targetAttacks ? g_ai.warDefenders : g_ai.warAttackers, &n);
            for (int i = 0; i < n; ++i) {
                uint32_t id = *reinterpret_cast<uint32_t*>(enemies + i * g_ai.memberStride + g_ai.memberId);
                if (id == ourId) continue;
                uint8_t* country = ResolveRef(*g_ai.countryDb, id, g_ai.countryId);
                sum += HalfPower(country ? country : *g_ai.nullCountry);
            }
        }
        if (sum) g_opportunistAdded.fetch_add(1, std::memory_order_relaxed);
        return sum;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_opportunistFailed = true;
        return 0;
    }
}

void InstallOpportunistFix() {
    // Replaces the pass: rdi = OpportunistPower(r14, [rbp+target]), the registers the game reads
    // after the join as it leaves them, then on to the join.
    Code c;
    c.Put({0x4C, 0x89, 0xF1});                                // mov rcx, r14
    c.Put({0x48, 0x8B, 0x95}).Put32(static_cast<uint32_t>(g_ai.targetSlot));  // mov rdx, [rbp+target]
    c.Put({0x48, 0x83, 0xEC, 0x20});                          // sub rsp, 20h (the body keeps rsp 16-aligned)
    c.Put({0x48, 0xB8}).Put64(reinterpret_cast<uint64_t>(&OpportunistPower));  // mov rax, fn
    c.Put({0xFF, 0xD0});                                      // call rax
    c.Put({0x48, 0x83, 0xC4, 0x20});                          // add rsp, 20h
    c.Put({0x48, 0x89, 0xC7});                                // mov rdi, rax
    c.Put({0x4C, 0x8B, 0x9D}).Put32(static_cast<uint32_t>(g_ai.targetSlot));  // mov r11, [rbp+target]
    c.Put({0x49, 0xB9}).Put64(reinterpret_cast<uint64_t>(g_ai.countryDb));    // mov r9, &countryDb
    c.Put({0x4D, 0x8B, 0x09});                                // mov r9, [r9]
    c.Put({0x49, 0xBA}).Put64(reinterpret_cast<uint64_t>(g_ai.nullWar));      // mov r10, &nullWar
    c.Put({0x4D, 0x8B, 0x12});                                // mov r10, [r10]
    c.JumpTo(g_ai.opportunistJoin);
    uint8_t* stub = PlaceStub(c, nullptr);
    if (!stub) {
        Log("AI opportunist: no memory for the stub; off");
        return;
    }
    Code jump;
    jump.JumpTo(stub);
    uint8_t patch[locate::kOpportunistStolenBytes];
    memset(patch, 0x90, sizeof(patch));
    memcpy(patch, jump.bytes, jump.n);
    if (!WriteCode(g_ai.opportunist, patch, sizeof(patch))) {
        Log("AI opportunist: failed to patch; off");
        return;
    }
    Log("AI opportunist: on; opportunist AIs count the enemies of a target at war");
}

// ---- 2.2 The war distance malus ----------------------------------------------------------------
//
// factor = 1 - d * MALUS, for d > MALUS_DISTANCE, at least MINIMUM_SCORE: already the minimum one
// jump past the distance. The define's comment says "per jump above the malus distance", so the
// rewrite multiplies d - MALUS_DISTANCE instead:
//   mov eax,ebp; sub eax,[DISTANCE]; jle skip; imul rax,[MALUS]; nop    (19 bytes, as before)

void InstallWarDistanceFix() {
    uint8_t* p = g_ai.warDistance;
    Code c;
    int32_t relDistance = 0, relMalus = 0;
    ptrdiff_t skip = g_ai.malusSkip - (p + 10);
    bool fits = Rel32(p + 8, g_ai.malusDistance, &relDistance) && Rel32(p + 18, g_ai.malus, &relMalus) &&
                skip >= -128 && skip <= 127;
    c.Put({0x89, 0xE8});                                                  // mov eax, ebp
    c.Put({0x2B, 0x05}).Put32(static_cast<uint32_t>(relDistance));        // sub eax, [MALUS_DISTANCE]
    c.Put({0x7E, static_cast<uint8_t>(skip)});                            // jle skip
    c.Put({0x48, 0x0F, 0xAF, 0x05}).Put32(static_cast<uint32_t>(relMalus));  // imul rax, [MALUS]
    c.Put({0x90});
    if (!fits || c.n != locate::kWarDistanceBytes || !WriteCode(p, c.bytes, c.n)) {
        Log("AI war distance: failed to patch; off");
        return;
    }
    g_aiDefinesWanted = true;
    Log("AI war distance: on; the malus counts the jumps above AI_WAR_DECLARATION_MALUS_DISTANCE");
}

// ---- 2.4 The truce divisor ---------------------------------------------------------------------
//
// PrepareWar divides its chance by AI_AGGRESSIVENESS_BOXED_IN_MULT when any contact has a truce
// with us. The division's operand now points at a value of its own. (A truce only with the
// candidate wouldn't mean anything: a target under truce already scores 0.)

void InstallTruceFix(int divisor) {
    auto value = reinterpret_cast<int32_t*>(AllocNear(g_ai.truceOperand, sizeof(int32_t)));
    int32_t rel;
    if (!value || !Rel32(g_ai.truceOperand + 4, value, &rel)) {
        Log("AI truce divisor: no memory near the game's code; off");
        return;
    }
    *value = divisor;
    if (!WriteCode(g_ai.truceOperand, &rel, 4)) {
        Log("AI truce divisor: failed to patch; off");
        return;
    }
    g_aiDefinesWanted = true;
    Log("AI truce divisor: on; a truce divides war appetite by %d instead of AI_AGGRESSIVENESS_BOXED_IN_MULT",
        divisor);
}

// ---- 2.5 The expansion lists ---------------------------------------------------------------------
//
// The colonize data (colonize, explore, stations, outposts, expand borders, terraform) is rebuilt
// every month during the first AI_BUDGETING_START_YEARS years, then once a year per country. The
// year check's jl becomes a jmp: monthly for the whole game.

void InstallExpansionFix() {
    const uint8_t jmp = 0xEB;
    if (*g_ai.expansionYears != 0x7C || !WriteCode(g_ai.expansionYears, &jmp, 1)) {
        Log("AI expansion lists: failed to patch; off");
        return;
    }
    Log("AI expansion lists: on; rebuilt every month");
}

// ---- 2.8/2.9 The AI's schedule and random stream --------------------------------------------------
//
// Each AI counts days to its weekly, monthly and yearly update from id % 7, % 30, % 360 when its
// CAICore is created, also after every load; residues 0 and 1 fall on the same day. With
// ai_schedule_from_date, before each daily update the counters are set so that the update falls
// on days where (id + day) % 7 (30, 360) == 0: the same days before and after a load, spread evenly.
//
// The AI's own (unsynced) random generator is saved, but read back only with -loadrandomseed,
// which also restores the game's synchronized one. ai_random_from_save drops the check for the
// AI's generator only. The other project's notes saw a restored AI behave as if restarted; the
// summary says which AIs, on the first day after a load, continue their saved stream.

using AiUpdateFn = uint64_t(__fastcall*)(uint8_t* core);
AiUpdateFn g_origAiUpdate = nullptr;
bool g_aiSchedule = false, g_aiRandomCheck = false;
std::atomic<int64_t> g_aiLastDay{-1}, g_aiCheckDay{-1};
std::atomic<uint32_t> g_aiFromSave{0}, g_aiRestarted{0};

void ScheduleAiDay(uint8_t* core, int64_t day) {
    uint64_t id = *reinterpret_cast<uint32_t*>(core + g_ai.coreId);
    // The game decrements, then updates when the counter is <= 0: 1 fires today, 2 doesn't.
    *reinterpret_cast<int32_t*>(core + g_ai.weekCounter) = (id + day) % 7 == 0 ? 1 : 2;
    *reinterpret_cast<int32_t*>(core + g_ai.monthCounter) = (id + day) % 30 == 0 ? 1 : 2;
    *reinterpret_cast<int32_t*>(core + g_ai.yearCounter) = (id + day) % 360 == 0 ? 1 : 2;
}

void CheckAiRandom(uint8_t* core, int64_t day) {
    int64_t last = g_aiLastDay.exchange(day);
    if (last != day) {
        if (last >= 0 && day == g_aiCheckDay.load() + 1) {
            Log("AI random on the first AI day after the start or load: %u AIs continue a saved stream, %u start "
                "at their id", g_aiFromSave.load(), g_aiRestarted.load());
        }
        if (last < 0 || day < last || day > last + 1) {  // a load, or a new game
            g_aiFromSave = 0;
            g_aiRestarted = 0;
            g_aiCheckDay = day;
        }
    }
    if (day != g_aiCheckDay.load()) return;
    uint32_t id = *reinterpret_cast<uint32_t*>(core + g_ai.coreId);
    bool restarted = *reinterpret_cast<uint32_t*>(core + g_ai.randomPosition) == id &&
                     *reinterpret_cast<uint32_t*>(core + g_ai.randomSeed) == id;
    (restarted ? g_aiRestarted : g_aiFromSave).fetch_add(1, std::memory_order_relaxed);
}

bool g_aiUpdateFailed = false;

bool BeforeAiUpdate(uint8_t* core) {
    __try {
        uint8_t* state = g_bench.gameState ? *g_bench.gameState : nullptr;
        if (!state) return true;
        int64_t day = *reinterpret_cast<int32_t*>(state + g_bench.stateDate) / 24;
        if (g_aiRandomCheck) CheckAiRandom(core, day);
        if (g_aiSchedule) ScheduleAiDay(core, day);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint64_t __fastcall AiUpdateHook(uint8_t* core) {
    if (!g_aiUpdateFailed && !BeforeAiUpdate(core)) {
        g_aiUpdateFailed = true;
        Log("AI schedule: fault; off from now on");
    }
    return g_origAiUpdate(core);
}

void InstallAiSchedule(bool schedule, bool random) {
    if (!g_bench.gameState) {
        Log("AI schedule: the game date wasn't found (see the ticks line); off");
        return;
    }
    g_aiSchedule = schedule;
    g_aiRandomCheck = random;
    uint8_t* tramp = InstallJumpHook(g_ai.aiUpdate, locate::kAiUpdateStolenBytes, reinterpret_cast<void*>(&AiUpdateHook));
    if (!tramp) {
        Log("AI schedule: failed to hook CAICore::Update; off");
        g_aiSchedule = g_aiRandomCheck = false;
        return;
    }
    g_origAiUpdate = reinterpret_cast<AiUpdateFn>(tramp);
    if (schedule) Log("AI schedule: on; weekly, monthly and yearly updates on days where (id + day) %% 7, 30, 360 is 0");
}

void InstallSavedRandom() {
    const uint8_t nop6[6] = {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00};
    for (uint8_t* jump : g_ai.savedRandomJumps) {
        if (jump[0] != 0x0F || jump[1] != 0x84 || !WriteCode(jump, nop6, sizeof(nop6))) {
            Log("AI random: failed to patch; a load may have restored part of it");
            return;
        }
    }
    Log("AI random: on; a loaded AI keeps its saved random_seed and random_count (-loadrandomseed is %s)",
        *g_ai.useSavedRandom ? "on" : "off");
}

std::string AiFixSummary() {
    std::string out;
    char buf[200];
    // The defines are read after stellaris-engine-fixes starts; report them once they're there.
    if (g_aiDefinesWanted && g_ai.boxedInMult && *g_ai.boxedInMult != 0) {
        g_aiDefinesWanted = false;
        if (g_ai.malusDistance && g_ai.malus) {
            Log("AI defines: AI_WAR_DECLARATION_MALUS_DISTANCE %d, malus %.2f per jump; "
                "AI_AGGRESSIVENESS_BOXED_IN_MULT %d", *g_ai.malusDistance, *g_ai.malus / 100000.0, *g_ai.boxedInMult);
        }
    }
    if (g_origPlanWalk) {
        sprintf_s(buf, "AI build plan: %llu walks, %llu blocked items moved back, %llu ordered, %llu kept%s",
                  g_planWalks.load(), g_planDeferred.load(), g_planOrdered.load(), g_planKept.load(),
                  g_planFailed ? " (off after a fault)" : "");
        out += buf;
    }
    if (g_opportunistCalls.load()) {
        sprintf_s(buf, "%sAI opportunist: %llu checks against a target at war, %llu with enemies counted%s",
                  out.empty() ? "" : "; ", g_opportunistCalls.load(), g_opportunistAdded.load(),
                  g_opportunistFailed ? " (off after a fault)" : "");
        out += buf;
    }
    return out;
}

void InstallAiFixes(HMODULE self, const locate::Image& img) {
    std::string ini = IniPath(self);
    bool plan = AiIni(ini, "build_plan_keep") != 0;
    bool opportunist = AiIni(ini, "opportunist_wars") != 0;
    bool distance = AiIni(ini, "war_distance_slope") != 0;
    int truce = AiIni(ini, "truce_war_divisor");
    bool expansion = AiIni(ini, "expansion_refresh_monthly") != 0;
    bool schedule = AiIni(ini, "ai_schedule_from_date") != 0;
    bool random = AiIni(ini, "ai_random_from_save") != 0;
    if (!plan && !opportunist && !distance && truce <= 0 && !expansion && !schedule && !random) {
        Log("AI fixes: all off ([ai_fixes] in stellaris-engine-fixes.ini)");
        return;
    }
    std::string error;
    auto find = [&](const char* what, bool (*locateFn)(const locate::Image&, locate::AiFixLocations&, std::string&)) {
        if (locateFn(img, g_ai, error)) return true;
        Log("AI %s: %s; off", what, error.c_str());
        return false;
    };
    if (plan && find("build plan", locate::LocateBuildPlan)) InstallBuildPlanFix();
    if (opportunist && find("opportunist", locate::LocateOpportunist)) InstallOpportunistFix();
    if (distance && find("war distance", locate::LocateWarDistance)) InstallWarDistanceFix();
    if (truce > 0 && find("truce divisor", locate::LocateTruce)) InstallTruceFix(truce);
    if (expansion && find("expansion lists", locate::LocateExpansionRefresh)) InstallExpansionFix();
    if ((schedule || random) && find("schedule", locate::LocateAiSchedule)) InstallAiSchedule(schedule, random);
    if (random && find("random", locate::LocateSavedRandom)) InstallSavedRandom();
}

// ---- The AI's income targets ----
//
// CAIEconomicPlan::CalculateIncomeTarget (re/ai.md section 6) adds the income of the plan's
// scaling subplans to the total, round after round, until some resource's total passes the target
// the forecast set, and counts the rounds (r13d; later rounds also scale the optional subplans).
// There is no limit: a scaling subplan that adds 5 of a resource whose target is 6 billion needs
// a billion rounds, half an hour of the main thread at 700,000 rounds a second, every AI month.
//
// Every round is the same: the same subplans, scope and target, so the same income R is added.
// After round 1 the total is T0 + R, and the round count k that the game would reach is the first
// at which T0 + k * R passes the target in some resource. When round 2 starts, the stub below
// sets the total to T0 + (k - 1) * R and the counter to k - 1; round 2 then adds R and counts
// itself, and the game's own check ends the loop at k, with the total and count the game's
// rounds would have given.
//
// [speed] fast_income_target=0 turns this off; income_target_verify=1 also lets rounds 2 and 3
// run and checks they add the same R before skipping the rest.

locate::IncomeTargetLocations g_income;
bool g_incomeOn = false;
bool g_incomeVerify = false;
bool g_incomeFailed = false;
std::atomic<uint64_t> g_incomeLoops{0}, g_incomeSkips{0}, g_incomeBails{0}, g_incomeMismatches{0};
std::atomic<uint64_t> g_incomeRoundsSkipped{0}, g_incomeMaxRounds{0};

constexpr int kIncomeMaxResources = 64;
struct IncomeLoop {
    uintptr_t frame;  // the CalculateIncomeTarget activation's rsp
    int count;
    int64_t t0[kIncomeMaxResources];  // the total before round 1
    int64_t r[kIncomeMaxResources];   // what round 1 added
};
// The loops in progress on this thread, outermost first (CalculateIncomeTarget recurses into
// subplans, and the AI runs on several threads).
thread_local IncomeLoop t_incomeLoops[16];
thread_local int t_incomeDepth = 0;

// A CPdxArray<int64> behind a CFixedResourceTable*: data at +0, count at +0xc (the loop's reads).
int64_t* ResourceData(const uint8_t* table, int* count) {
    const uint8_t* inner = *reinterpret_cast<uint8_t* const*>(table);
    *count = *reinterpret_cast<const int*>(inner + 0xc);
    return *reinterpret_cast<int64_t* const*>(inner);
}

int IncomeRound(uint8_t* total, uint8_t* target, int round, uintptr_t frame) {
    IncomeLoop* loops = t_incomeLoops;
    int& depth = t_incomeDepth;
    // Loops below this frame have returned; a loop at this frame starting over is a new one.
    while (depth > 0 && (loops[depth - 1].frame < frame || (round == 0 && loops[depth - 1].frame == frame))) --depth;
    int n = 0, nt = 0;
    int64_t* tot = ResourceData(total, &n);
    const int64_t* tgt = ResourceData(target, &nt);
    if (round == 0) {
        if (depth == _countof(t_incomeLoops) || n != nt || n <= 0 || n > kIncomeMaxResources) return round;
        IncomeLoop& l = loops[depth++];
        l.frame = frame;
        l.count = n;
        memcpy(l.t0, tot, n * sizeof(int64_t));
        return round;
    }
    if (depth == 0 || loops[depth - 1].frame != frame) return round;
    IncomeLoop& l = loops[depth - 1];
    if (l.count != n || n != nt) {
        --depth;
        return round;
    }
    if (round == 1) {
        g_incomeLoops.fetch_add(1, std::memory_order_relaxed);
        for (int i = 0; i < n; ++i) l.r[i] = tot[i] - l.t0[i];
    } else {
        // Verifying: rounds after the first must have added the same.
        for (int i = 0; i < n; ++i) {
            if (tot[i] == l.t0[i] + round * l.r[i]) continue;
            if (g_incomeMismatches.fetch_add(1) < 20) {
                Log("AI income target MISMATCH: round %d, resource %d: total %lld, expected %lld", round, i,
                    static_cast<long long>(tot[i]), static_cast<long long>(l.t0[i] + round * l.r[i]));
            }
            --depth;
            return round;
        }
    }
    if (g_incomeVerify && round < 3) return round;
    --depth;
    int64_t k = income::Rounds(l.t0, l.r, tgt, n, round);
    if (k < 0) {
        g_incomeBails.fetch_add(1, std::memory_order_relaxed);
        return round;
    }
    if (k == round + 1) return round;  // this round is the last anyway
    for (int i = 0; i < n; ++i) tot[i] = l.t0[i] + (k - 1) * l.r[i];
    g_incomeSkips.fetch_add(1, std::memory_order_relaxed);
    g_incomeRoundsSkipped.fetch_add(static_cast<uint64_t>(k - 1 - round), std::memory_order_relaxed);
    uint64_t max = g_incomeMaxRounds.load(std::memory_order_relaxed);
    while (static_cast<uint64_t>(k) > max && !g_incomeMaxRounds.compare_exchange_weak(max, static_cast<uint64_t>(k))) {
    }
    return static_cast<int>(k - 1);
}

// Called by the stub with the caller's rbx (total), r12 (target), r13d (rounds so far) and rsp;
// returns the new r13d.
int __fastcall IncomeRoundHook(uint8_t* total, uint8_t* target, int round, uintptr_t frame) {
    if (g_incomeFailed) return round;
    __try {
        return IncomeRound(total, target, round, frame);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_incomeFailed = true;
        return round;
    }
}

void InstallIncomeTarget(HMODULE self, const locate::Image& img) {
    std::string ini = IniPath(self);
    if (!ini.empty() && GetPrivateProfileIntA("speed", "fast_income_target", 1, ini.c_str()) == 0) {
        Log("AI income targets: off (fast_income_target=0)");
        return;
    }
    g_incomeVerify = !ini.empty() && GetPrivateProfileIntA("speed", "income_target_verify", 0, ini.c_str()) != 0;
    std::string error;
    if (!locate::LocateIncomeTarget(img, g_income, error)) {
        Log("AI income targets: %s; off", error.c_str());
        return;
    }
    // The redirected call lands here: rsp is 8 below a 16-byte boundary, as at any call.
    Code c;
    c.Put({0x51, 0x52, 0x41, 0x50});              // push rcx / push rdx / push r8 (CalcScalingSubplans' arguments)
    c.Put({0x48, 0x83, 0xEC, 0x20});              // sub rsp, 20h (now 16-aligned)
    c.Put({0x48, 0x89, 0xD9});                    // mov rcx, rbx  (total)
    c.Put({0x4C, 0x89, 0xE2});                    // mov rdx, r12  (target)
    c.Put({0x45, 0x89, 0xE8});                    // mov r8d, r13d (rounds)
    c.Put({0x4C, 0x8D, 0x4C, 0x24, 0x40});        // lea r9, [rsp+40h] (the caller's rsp)
    c.Put({0x48, 0xB8}).Put64(reinterpret_cast<uint64_t>(&IncomeRoundHook));  // mov rax, hook
    c.Put({0xFF, 0xD0});                          // call rax
    c.Put({0x41, 0x89, 0xC5});                    // mov r13d, eax
    c.Put({0x48, 0x83, 0xC4, 0x20});              // add rsp, 20h
    c.Put({0x41, 0x58, 0x5A, 0x59});              // pop r8 / pop rdx / pop rcx
    c.JumpTo(g_income.calcScaling);               // on to CalcScalingSubplans, which returns to the game
    uint8_t* stub = PlaceStub(c, g_income.scalingCall);
    int32_t rel;
    if (!stub || !Rel32(g_income.scalingCall + 5, stub, &rel) || !WriteCode(g_income.scalingCall + 1, &rel, 4)) {
        Log("AI income targets: no memory near the game's code; off");
        return;
    }
    g_incomeOn = true;
    Log("AI income targets: CalculateIncomeTarget's scaling loop +%llX, CalcScalingSubplans +%llX; rounds after the "
        "first are counted, not run%s",
        static_cast<unsigned long long>(g_income.loop - reinterpret_cast<uint8_t*>(g_base)),
        static_cast<unsigned long long>(g_income.calcScaling - reinterpret_cast<uint8_t*>(g_base)),
        g_incomeVerify ? " (verifying rounds 2 and 3)" : "");
}

std::string IncomeSummary() {
    if (!g_incomeOn) return std::string();
    char buf[260];
    sprintf_s(buf, "AI income targets: %llu loops past round 1, %llu shortened (%llu rounds skipped, most %llu), "
              "%llu left to the game%s",
              g_incomeLoops.load(), g_incomeSkips.load(), g_incomeRoundsSkipped.load(), g_incomeMaxRounds.load(),
              g_incomeBails.load(), g_incomeFailed ? " (off after a fault)" : "");
    std::string out = buf;
    if (g_incomeVerify) {
        sprintf_s(buf, "; verified, %llu mismatches", g_incomeMismatches.load());
        out += buf;
    }
    return out;
}

// ===========================================================================
// Script storage: the hooks it needs (log effect and trigger, saves and loads)

// A CString's text into `out` (NUL-terminated, cut to fit); returns its full length.
int32_t CopyGameString(const game::String* text, char* out, int32_t size) {
    size_t length = text->size;
    if (length > 0x7FFFFFFF) length = 0;
    if (size > 0) {
        size_t n = length < static_cast<size_t>(size - 1) ? length : static_cast<size_t>(size - 1);
        memcpy(out, text->data(), n);
        out[n] = '\0';
    }
    return static_cast<int32_t>(length);
}

// A CString over `text` that nobody frees: inline when it fits, else kind 1 pointing at `text`,
// which must outlive it.
game::String BorrowString(const char* text) {
    size_t size = strlen(text);
    game::String s{};
    if (size <= 0xF) {
        memcpy(s.buf, text, size);
        s.cap = 0xF;
    } else {
        s.kind = 1;
        s.ptr = const_cast<char*>(text);
        s.cap = size;
    }
    s.size = size;
    return s;
}

// ---- The log effect and trigger: "@sef:..." ----

using LogEffectFn = void(__fastcall*)(uint8_t* effect, uint8_t* scope);
using LogTriggerFn = bool(__fastcall*)(uint8_t* trigger, uint8_t* scope);
LogEffectFn g_origLogEffect = nullptr;
LogTriggerFn g_origLogTrigger = nullptr;
size_t g_logEffectText = 0, g_logTriggerText = 0;

// A log effect's or trigger's text after "@sef:", in `buf`; false when it doesn't start so.
bool StoreCall(const uint8_t* object, size_t textOffset, char* buf, size_t size) {
    __try {
        const auto* text = reinterpret_cast<const game::String*>(object + textOffset);
        if (text->size < 5 || text->size >= size || memcmp(text->data(), "@sef:", 5) != 0) return false;
        memcpy(buf, text->data() + 5, text->size - 5);
        buf[text->size - 5] = '\0';
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __fastcall LogEffectHook(uint8_t* effect, uint8_t* scope) {
    char buf[1024];
    if (g_storeOn && StoreCall(effect, g_logEffectText, buf, sizeof(buf))) return StoreEffect(buf, scope);
    g_origLogEffect(effect, scope);
}

bool __fastcall LogTriggerHook(uint8_t* trigger, uint8_t* scope) {
    char buf[1024];
    if (g_storeOn && StoreCall(trigger, g_logTriggerText, buf, sizeof(buf))) return StoreTrigger(buf, scope);
    return g_origLogTrigger(trigger, scope);
}

bool HookLogs(uint8_t* effect, size_t effectText, uint8_t* trigger, size_t triggerText) {
    g_logEffectText = effectText;  // before the hooks can run
    g_logTriggerText = triggerText;
    uint8_t* e = InstallJumpHook(effect, locate::kLogScriptStolenBytes, reinterpret_cast<void*>(&LogEffectHook));
    if (!e) return false;
    g_origLogEffect = reinterpret_cast<LogEffectFn>(e);
    // Without the trigger's hook the effect's stays: it passes everything on while storage is off.
    uint8_t* t = InstallJumpHook(trigger, locate::kLogScriptStolenBytes, reinterpret_cast<void*>(&LogTriggerHook));
    if (!t) return false;
    g_origLogTrigger = reinterpret_cast<LogTriggerFn>(t);
    return true;
}

// ---- Saves, loads and new games, for the save store ----

locate::SaveLocations g_saves;
using StartSaveFn = uint32_t(__fastcall*)(void* manager, const game::String* path, bool a, bool b, bool c);
using GameStateLoadFn = void(__fastcall*)(void* state, void* file, const game::String* path);
using NewGameFn = void(__fastcall*)(void* state);
StartSaveFn g_origStartSave = nullptr;
GameStateLoadFn g_origLoad = nullptr;
NewGameFn g_origNewGame = nullptr;

// The game's path, cut to fit; false on a fault. No C++ objects here, for __try.
bool CopySavePath(const game::String* path, char* out, int32_t size) {
    out[0] = '\0';
    __try {
        if (path) CopyGameString(path, out, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = '\0';
        return false;
    }
}

uint32_t __fastcall StartSaveHook(void* manager, const game::String* path, bool a, bool b, bool c) {
    uint32_t result = g_origStartSave(manager, path, a, b, c);
    if (result == 0) StoreSaveStarted(manager);  // 0: the save was accepted; the store reads its normalized path
    return result;
}

void __fastcall LoadHook(void* state, void* file, const game::String* path) {
    char text[1024];
    if (CopySavePath(path, text, sizeof(text))) StoreLoading(text);  // before the load, which may already run script
    g_origLoad(state, file, path);
}

void __fastcall NewGameHook(void* state) {
    StoreNewGame();  // before on_game_start runs
    g_origNewGame(state);
}

bool InstallSaves(const locate::Image& img) {
    std::string error;
    if (!locate::LocateSaves(img, nullptr, g_saves, error)) {
        Log("script storage: %s", error.c_str());
        return false;
    }
    auto newGame = InstallJumpHook(g_saves.newGame, locate::kNewGameStartedStolenBytes, reinterpret_cast<void*>(&NewGameHook));
    if (newGame) g_origNewGame = reinterpret_cast<NewGameFn>(newGame);
    auto load = newGame ? InstallJumpHook(g_saves.load, locate::kGameStateLoadStolenBytes, reinterpret_cast<void*>(&LoadHook))
                        : nullptr;
    if (load) g_origLoad = reinterpret_cast<GameStateLoadFn>(load);
    auto save = load ? InstallJumpHook(g_saves.startSave, locate::kStartSaveToPathStolenBytes,
                                       reinterpret_cast<void*>(&StartSaveHook))
                     : nullptr;
    if (!save) {
        Log("script storage: failed to hook saving and loading");
        return false;
    }
    g_origStartSave = reinterpret_cast<StartSaveFn>(save);
    Log("script storage: save hooks: StartSaveToPath +%llX, CGameState::Load +%llX, OnNewGameStarted +%llX",
        static_cast<unsigned long long>(g_saves.startSave - reinterpret_cast<uint8_t*>(g_base)),
        static_cast<unsigned long long>(g_saves.load - reinterpret_cast<uint8_t*>(g_base)),
        static_cast<unsigned long long>(g_saves.newGame - reinterpret_cast<uint8_t*>(g_base)));
    return true;
}

// ===========================================================================
// Script storage (README, "Script storage")
//
// Paradox script keeps data outside the game without C#, through log = "@sef:..." effects and
// triggers: numbers from variables, and text recorded from log lines, which `run` executes later.
// Two stores, global (shared by every game, sefdata\global.txt) and save (sefdata\saves\<folder>\
// <name>.txt, written when the game saves, read when that save loads, empty for a new game).
// Off in multiplayer: each machine has its own files, so they would go out of sync.

locate::VariableLocations g_vars;
locate::LogScriptLocations g_storeLogs;
locate::MultiplayerLocations g_mp;
uint8_t* g_storeAssign = nullptr;  // CString::assign(string, text, length)
uint8_t* g_storeEffectCommand = nullptr;  // OnExecute_Effect, for `run`; null: run is off
std::wstring g_storeDir;  // sefdata\ in the game folder

std::mutex g_storeLock;  // the stores, the recording and the run queue; effects may run on worker threads
pdxstore::Store g_globalStore, g_saveStore;
bool g_globalLoaded = false;
ULONGLONG g_globalWritten = 0;

struct PendingStoreSave {
    std::string key;
    pdxstore::Store snapshot;
};
std::map<void*, PendingStoreSave> g_pendingStoreSaves;
locate::SaveWriteLocations g_saveWrite;
void(__fastcall* g_origZipAndWrite)(void*, const game::String*) = nullptr;
void(__fastcall* g_origDestroySaveArchive)(void*) = nullptr;
int(__fastcall* g_origCloseSaveFile)(void*) = nullptr;
bool(__fastcall* g_origCloudSaveWrite)(void*, const void*, uint32_t) = nullptr;
bool(__fastcall* g_origSaveAddFile)(void*, void*, const game::String*, int, int) = nullptr;

struct SaveWriteStatus;
thread_local SaveWriteStatus* g_saveWriteStatus = nullptr;

struct SaveWriteStatus {
    explicit SaveWriteStatus(bool tracked) : previous(g_saveWriteStatus) {
        g_saveWriteStatus = tracked ? this : nullptr;
    }
    ~SaveWriteStatus() { g_saveWriteStatus = previous; }

    SaveWriteStatus* const previous;
    bool archiveOk = true;
    bool closingArchive = false;
    bool written = false;
};

struct StoreRecording {
    bool active = false;
    DWORD thread = 0;
    pdxstore::Which store = pdxstore::Which::Global;
    std::string key, text;
    int lines = 0, dropped = 0;
};
StoreRecording g_recording;
struct StoreRun {
    pdxstore::Which store;
    std::string key;
};
std::vector<StoreRun> g_storeRuns;

std::atomic<uint64_t> g_storeSets{0}, g_storeGets{0}, g_storeTests{0}, g_storeRecorded{0}, g_storeRan{0}, g_storeErrors{0};

using ProcessStringFn = game::String*(__fastcall*)(void* text, game::String* out, void* in, void* a, void* b, void* c);
using AccessVariablesFn = uint8_t*(__fastcall*)(uint8_t* scope, const game::String* name);
using FindVariableFn = void*(__fastcall*)(uint8_t* map, uint8_t** entry, const game::String* key);
using SetVariableFn = void(__fastcall*)(uint8_t* variables, const game::String* name, int64_t value);
ProcessStringFn g_origProcessString = nullptr;

const char* StoreName(pdxstore::Which which) { return which == pdxstore::Which::Global ? "global" : "save"; }

// Logs at most 100 problems a session, so a script that errs every day doesn't fill the log.
void StoreProblem(const char* fmt, ...) {
    if (g_storeErrors.fetch_add(1) >= 100) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Log("script storage: %s", buf);
}

bool StoreMultiplayer() {
    __try {
        const uint8_t* session = *g_mp.session;
        return session && session[g_mp.flag] != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}

// The caller holds g_storeLock.
pdxstore::Store& StoreFor(pdxstore::Which which) {
    if (which == pdxstore::Which::Save) return g_saveStore;
    if (!g_globalLoaded) {
        g_globalLoaded = true;
        std::string error;
        if (!pdxstore::ReadStore(g_storeDir + L"\\global.txt", g_globalStore, error)) {
            Log("script storage: sefdata\\global.txt: %s; the global store starts empty (the file is kept until it is "
                "next written)", error.c_str());
        } else {
            Log("script storage: global store: %zu keys", g_globalStore.Count());
        }
    }
    return g_globalStore;
}

// The caller holds g_storeLock.
void WriteGlobalStore() {
    if (!g_globalLoaded || !g_globalStore.Dirty()) return;
    g_globalWritten = GetTickCount64();
    if (!pdxstore::WriteStore(g_storeDir + L"\\global.txt", g_globalStore)) {
        StoreProblem("cannot write sefdata\\global.txt (error %lu)", GetLastError());
    }
}

// A CString the game owns, holding `text` (zeroed first, as the game's out-parameters are). No C++
// objects here, for __try.
bool MakeGameString(const char* text, size_t size, game::String* s) {
    memset(s, 0, sizeof(*s));
    s->cap = 0xF;
    __try {
        reinterpret_cast<game::AssignFn>(g_storeAssign)(s, text, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void FreeGameString(game::String* s) {
    __try {
        if (s->cap > 0xF && s->kind != 1) reinterpret_cast<game::FreeFn>(g_str.gameFree)(s->ptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    memset(s, 0, sizeof(*s));
}

// The scope's variable, as check_variable reads it: 1 set, 0 unset, -1 on a fault.
int ReadVariable(uint8_t* scope, const game::String* name, int64_t* value) {
    __try {
        uint8_t* variables = reinterpret_cast<AccessVariablesFn>(g_vars.get)(scope, name);
        if (!variables) return 0;
        uint8_t* entry = nullptr;
        reinterpret_cast<FindVariableFn>(g_vars.find)(variables + g_vars.map, &entry, name);
        if (!entry || entry[g_vars.entryEmpty] == 0xFF) return 0;
        *value = *reinterpret_cast<const int64_t*>(entry + g_vars.entryValue);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// Sets the scope's variable, as set_variable does: 1 done, 0 when the scope has no variables, -1 on a fault.
int WriteVariable(uint8_t* scope, const game::String* name, int64_t value) {
    __try {
        uint8_t* variables = reinterpret_cast<AccessVariablesFn>(g_vars.access)(scope, name);
        if (!variables) return 0;
        reinterpret_cast<SetVariableFn>(g_vars.set)(variables, name, value);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// "@sef:" from a log effect: everything but `has`.
void StoreEffect(const char* text, uint8_t* scope) {
    pdxstore::Command c;
    std::string error;
    if (!pdxstore::ParseCommand(text, c, error)) {
        StoreProblem("log = \"@sef:%s\": %s", text, error.c_str());
        return;
    }
    if (StoreMultiplayer()) {
        StoreProblem("log = \"@sef:%s\": off in multiplayer", text);
        return;
    }
    if (c.op == pdxstore::Op::Has) {
        StoreProblem("log = \"@sef:%s\": has is a trigger; use it in a limit or trigger block", text);
        return;
    }
    std::unique_lock<std::mutex> lock(g_storeLock);
    switch (c.op) {
    case pdxstore::Op::Set: {
        int64_t value = c.value;
        int got = 1;
        if (!c.number) {
            lock.unlock();  // game code: no lock held while it runs
            game::String name;
            if (!MakeGameString(c.variable.data(), c.variable.size(), &name)) {
                StoreProblem("@sef:%s: the game faulted making the name", text);
                return;
            }
            got = ReadVariable(scope, &name, &value);
            FreeGameString(&name);
            lock.lock();
            if (got < 0) {
                StoreProblem("@sef:%s: the game faulted reading the variable", text);
                return;
            }
        }
        pdxstore::Store& store = StoreFor(c.store);
        if (got == 0) {
            store.Remove(c.key);  // an unset variable leaves the key unset
        } else if (!store.Set(c.key, pdxstore::FormatFixed(value))) {
            StoreProblem("@sef:%s: the %s store is full", text, StoreName(c.store));
        }
        g_storeSets.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    case pdxstore::Op::Get: {
        const std::string* stored = StoreFor(c.store).Get(c.key);
        if (!stored) return;  // nothing stored: the variable stays as it is
        int64_t value = 0;
        if (!pdxstore::ParseFixed(*stored, &value)) {
            StoreProblem("@sef:%s: the %s value of %s is text, not a number", text, StoreName(c.store), c.key.c_str());
            return;
        }
        lock.unlock();
        game::String name;
        if (!MakeGameString(c.variable.data(), c.variable.size(), &name)) {
            StoreProblem("@sef:%s: the game faulted making the name", text);
            return;
        }
        int done = WriteVariable(scope, &name, value);
        FreeGameString(&name);
        if (done < 0) StoreProblem("@sef:%s: the game faulted setting the variable", text);
        if (done == 0) StoreProblem("@sef:%s: this scope has no variables", text);
        g_storeGets.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    case pdxstore::Op::Clear:
        StoreFor(c.store).Remove(c.key);
        return;
    case pdxstore::Op::Record:
        if (g_recording.active) {
            StoreProblem("@sef:%s: already recording %s; record_end first", text, g_recording.key.c_str());
            return;
        }
        g_recording = StoreRecording();
        g_recording.active = true;
        g_recording.thread = GetCurrentThreadId();
        g_recording.store = c.store;
        g_recording.key = c.key;
        return;
    case pdxstore::Op::RecordEnd: {
        if (!g_recording.active) {
            StoreProblem("@sef:record_end with no recording");
            return;
        }
        if (g_recording.thread != GetCurrentThreadId()) {
            StoreProblem("@sef:record_end from a different thread than record");
            return;
        }
        StoreRecording r = std::move(g_recording);
        g_recording = StoreRecording();
        if (!StoreFor(r.store).Set(r.key, r.text)) {
            StoreProblem("record %s.%s: too long for the store", StoreName(r.store), r.key.c_str());
            return;
        }
        g_storeRecorded.fetch_add(1, std::memory_order_relaxed);
        Log("script storage: recorded %s.%s: %d lines kept, %d without script dropped", StoreName(r.store), r.key.c_str(),
            r.lines, r.dropped);
        return;
    }
    case pdxstore::Op::Run:
        if (!g_storeEffectCommand) {
            StoreProblem("@sef:%s: run is not available (the console's effect command was not found)", text);
            return;
        }
        g_storeRuns.push_back({c.store, c.key});
        return;
    default:
        return;
    }
}

// "@sef:" from a log trigger: only `has`.
bool StoreTrigger(const char* text, uint8_t* /*scope*/) {
    pdxstore::Command c;
    std::string error;
    if (!pdxstore::ParseCommand(text, c, error)) {
        StoreProblem("log = \"@sef:%s\": %s", text, error.c_str());
        return false;
    }
    if (c.op != pdxstore::Op::Has) {
        StoreProblem("log = \"@sef:%s\" in a trigger: only has works there", text);
        return false;
    }
    if (StoreMultiplayer()) return false;  // the same answer on every machine
    std::lock_guard<std::mutex> lock(g_storeLock);
    g_storeTests.fetch_add(1, std::memory_order_relaxed);
    return StoreFor(c.store).Get(c.key) != nullptr;
}

// Copies the text ProcessString built. No C++ objects here, for __try.
int32_t CopyLogText(const game::String* text, char* out, int32_t size) {
    __try {
        return CopyGameString(text, out, size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// Replaces the log effect's call to CTextBase::ProcessString: the game's text, with \[...] filled in,
// added to the recording while one is open on this thread.
game::String* __fastcall LogTextHook(void* text, game::String* out, void* in, void* a, void* b, void* c) {
    game::String* result = g_origProcessString(text, out, in, a, b, c);
    std::lock_guard<std::mutex> lock(g_storeLock);
    if (!g_recording.active || g_recording.thread != GetCurrentThreadId()) return result;
    char buf[8192];
    int32_t length = CopyLogText(out, buf, sizeof(buf));
    if (length < 0) return result;
    if (length >= static_cast<int32_t>(sizeof(buf))) {
        StoreProblem("record %s: a line of %d bytes was cut to %zu", g_recording.key.c_str(), length, sizeof(buf) - 1);
    }
    std::string kept;
    if (pdxstore::KeepRecordedLine(buf, &kept)) {
        g_recording.text += kept;
        g_recording.text += '\n';
        ++g_recording.lines;
    } else {
        ++g_recording.dropped;
    }
    return result;
}

// Runs `text` with the console's effect command, as the console's `effect` does. No C++ objects here, for __try.
bool RunStoredEffect(const char* text, game::ConsoleResult* result) {
    game::String line = BorrowString(text);
    uint8_t args[0x18]{};
    game::String* items = &line;
    memcpy(args + game::kArrayDataOffset, &items, sizeof(items));
    *reinterpret_cast<int*>(args + game::kArrayCountOffset) = 1;
    __try {
        reinterpret_cast<game::EffectCommandFn>(g_storeEffectCommand)(result, args);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Once per frame, on the main thread between ticks: runs what `run` queued, ends a recording left
// open, and writes the global store at most once a second.
void StoreFrame() {
    if (!g_storeOn) return;
    std::vector<StoreRun> runs;
    std::vector<std::string> texts;
    {
        std::lock_guard<std::mutex> lock(g_storeLock);
        if (g_recording.active) {
            StoreProblem("record %s.%s: no record_end in the same frame; dropped", StoreName(g_recording.store),
                         g_recording.key.c_str());
            g_recording = StoreRecording();
        }
        runs.swap(g_storeRuns);
        for (const StoreRun& r : runs) {
            const std::string* stored = StoreFor(r.store).Get(r.key);
            texts.push_back(stored ? *stored : std::string());
        }
        if (g_globalStore.Dirty() && GetTickCount64() - g_globalWritten >= 1000) WriteGlobalStore();
    }
    if (runs.empty() || !*g_bench.gameState) return;
    for (size_t i = 0; i < runs.size(); ++i) {
        const char* name = StoreName(runs[i].store);
        if (texts[i].empty()) {
            StoreProblem("run %s.%s: nothing stored", name, runs[i].key.c_str());
            continue;
        }
        if (StoreMultiplayer()) continue;
        game::ConsoleResult result{};
        bool ran = RunStoredEffect(texts[i].c_str(), &result);
        g_storeRan.fetch_add(1, std::memory_order_relaxed);
        if (ran) {
            Log("script storage: ran %s.%s (%zu bytes)%s", name, runs[i].key.c_str(), texts[i].size(),
                result.ok ? "" : "; the game refused it, see error.log");
            FreeGameString(&result.text);
        } else {
            StoreProblem("run %s.%s: the game faulted running it", name, runs[i].key.c_str());
        }
    }
}

// StartSaveToPath finishes any prior write before accepting a new one on this manager.
// Capture the accepted, normalized path before Update (or Ironman exit) starts the write.
void StoreSaveStarted(void* manager) {
    if (!g_storeOn) return;
    char path[1024];
    auto* params = static_cast<const uint8_t*>(manager) + g_saveWrite.paramsOffset;
    bool tracked = g_origZipAndWrite && !StoreMultiplayer() &&
                   CopySavePath(reinterpret_cast<const game::String*>(params), path, sizeof(path));
    std::string key = tracked ? pdxstore::SaveKey(path, true) : std::string();
    std::lock_guard<std::mutex> lock(g_storeLock);
    WriteGlobalStore();
    if (key.empty()) {
        g_pendingStoreSaves.erase(manager);
        return;
    }
    g_pendingStoreSaves.insert_or_assign(manager, PendingStoreSave{std::move(key), g_saveStore});
}

PendingStoreSave TakeStoreSave(void* manager, const game::String* path) {
    char text[1024];
    const std::string key = CopySavePath(path, text, sizeof(text)) ? pdxstore::SaveKey(text, true) : std::string();
    std::lock_guard<std::mutex> lock(g_storeLock);
    auto save = g_pendingStoreSaves.find(manager);
    if (save == g_pendingStoreSaves.end() || save->second.key != key) return {};
    PendingStoreSave pending = std::move(save->second);
    g_pendingStoreSaves.erase(save);
    return pending;
}

// Both the scheduled save task and the synchronous Ironman exit go through this writer.
void __fastcall StoreZipAndWriteHook(void* manager, const game::String* params) {
    PendingStoreSave save = TakeStoreSave(manager, params);
    SaveWriteStatus status(!save.key.empty());
    g_origZipAndWrite(manager, params);
    if (save.key.empty() || !status.written || !status.archiveOk) return;
    std::lock_guard<std::mutex> lock(g_storeLock);
    size_t count = save.snapshot.Count();
    if (!pdxstore::WriteStore(pdxstore::SaveFile(g_storeDir, save.key), save.snapshot)) {
        StoreProblem("cannot write the save store for %s (error %lu)", save.key.c_str(), GetLastError());
    } else if (count) {
        Log("script storage: save store written for %s: %zu keys", save.key.c_str(), count);
    }
}

bool __fastcall StoreSaveAddFileHook(void* archive, void* file, const game::String* name, int method, int level) {
    bool added = g_origSaveAddFile(archive, file, name, method, level);
    if (!added && g_saveWriteStatus) g_saveWriteStatus->archiveOk = false;
    return added;
}

bool __fastcall StoreCloudSaveWriteHook(void* file, const void* data, uint32_t size) {
    bool written = g_origCloudSaveWrite(file, data, size);
    if (g_saveWriteStatus) g_saveWriteStatus->written = written;
    return written;
}

// The two normal local-write paths reach the destructor after WriteData succeeds.
// Only observe PHYSFS_close during those calls. Unwind cleanup calls the original destructor.
void __fastcall StoreDestroySaveArchiveHook(void* archive) {
    SaveWriteStatus* status = g_saveWriteStatus;
    if (status) status->closingArchive = true;
    g_origDestroySaveArchive(archive);
    if (status) status->closingArchive = false;
}

int __fastcall StoreCloseSaveFileHook(void* file) {
    int closed = g_origCloseSaveFile(file);
    if (g_saveWriteStatus && g_saveWriteStatus->closingArchive) g_saveWriteStatus->written = closed != 0;
    return closed;
}

void StoreLoading(const char* path) {
    if (!g_storeOn) return;
    std::string key = pdxstore::SaveKey(path, false);
    std::lock_guard<std::mutex> lock(g_storeLock);
    if (key.empty()) {
        g_saveStore.Clear();
        return;
    }
    std::string error;
    if (!pdxstore::ReadStore(pdxstore::SaveFile(g_storeDir, key), g_saveStore, error)) {
        StoreProblem("the save store for %s: %s; it starts empty", key.c_str(), error.c_str());
    } else if (g_saveStore.Count()) {
        Log("script storage: save store read for %s: %zu keys", key.c_str(), g_saveStore.Count());
    }
}

void StoreNewGame() {
    if (!g_storeOn) return;
    std::lock_guard<std::mutex> lock(g_storeLock);
    g_saveStore.Clear();
    g_saveStore.SetClean();
}

// At exit, if the main thread isn't in the middle of a change.
void StoreExit() {
    if (!g_storeOn || !g_storeLock.try_lock()) return;
    WriteGlobalStore();
    g_storeLock.unlock();
}

std::string StoreSummary() {
    if (!g_storeOn) return std::string();
    uint64_t sets = g_storeSets.load(), gets = g_storeGets.load(), tests = g_storeTests.load(), rec = g_storeRecorded.load(),
             ran = g_storeRan.load(), errors = g_storeErrors.load();
    if (!sets && !gets && !tests && !rec && !ran && !errors) return std::string();
    char buf[200];
    sprintf_s(buf, "script storage: %llu sets, %llu gets, %llu has, %llu recorded, %llu run, %llu problems", sets, gets, tests,
              rec, ran, errors);
    return buf;
}

bool RedirectStoreCall(uint8_t* call, const void* hook) {
    Code code;
    code.JumpTo(hook);
    uint8_t* stub = PlaceStub(code, call);
    int32_t rel = 0;
    return stub && call[0] == 0xE8 && Rel32(call + 5, stub, &rel) && WriteCode(call + 1, &rel, sizeof(rel));
}

bool InstallStoreSaveWrite(const locate::Image& img) {
    std::string error;
    if (!locate::LocateSaveWrite(img, g_saves, g_saveWrite, error)) {
        Log("script storage: %s", error.c_str());
        return false;
    }
    g_origDestroySaveArchive = reinterpret_cast<decltype(g_origDestroySaveArchive)>(g_saveWrite.destroyArchive);
    g_origCloseSaveFile = reinterpret_cast<decltype(g_origCloseSaveFile)>(g_saveWrite.closeFile);
    g_origCloudSaveWrite = reinterpret_cast<decltype(g_origCloudSaveWrite)>(g_saveWrite.cloudWrite);
    g_origSaveAddFile = reinterpret_cast<decltype(g_origSaveAddFile)>(g_saveWrite.addFile);
    if (!RedirectStoreCall(g_saveWrite.closeCall, reinterpret_cast<const void*>(&StoreCloseSaveFileHook)) ||
        !RedirectStoreCall(g_saveWrite.cloudCall, reinterpret_cast<const void*>(&StoreCloudSaveWriteHook))) return false;
    for (uint8_t* call : g_saveWrite.archiveDestroyCalls) {
        if (!RedirectStoreCall(call, reinterpret_cast<const void*>(&StoreDestroySaveArchiveHook))) return false;
    }
    for (uint8_t* call : g_saveWrite.addFileCalls) {
        if (!RedirectStoreCall(call, reinterpret_cast<const void*>(&StoreSaveAddFileHook))) return false;
    }
    // Until this last hook installs, the call redirects only forward to the game.
    auto* zip = InstallJumpHook(g_saveWrite.zipAndWrite, locate::kZipAndWriteStolenBytes,
                                reinterpret_cast<void*>(&StoreZipAndWriteHook));
    g_origZipAndWrite = reinterpret_cast<decltype(g_origZipAndWrite)>(zip);
    return zip != nullptr;
}

void InstallScriptStorage(HMODULE self, const locate::Image& img) {
    std::string ini = IniPath(self);
    if (!ini.empty() && GetPrivateProfileIntA("script_storage", "enabled", 1, ini.c_str()) == 0) {
        Log("script storage: off (enabled=0)");
        return;
    }
    std::string error;
    locate::LexerLocations lexer;
    if (!locate::LocateVariables(img, g_vars, error) || !locate::LocateLexer(img, lexer, error) ||
        !locate::LocateMultiplayer(img, g_mp, error) ||
        !locate::LocateLogScript(img, g_vars.effectName, g_vars.triggerName, g_storeLogs, error)) {
        Log("script storage: %s; off", error.c_str());
        return;
    }
    g_storeAssign = lexer.assign;
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    const wchar_t* slash = n && n < MAX_PATH ? wcsrchr(path, L'\\') : nullptr;
    if (!slash) return;
    g_storeDir = std::wstring(path, static_cast<size_t>(slash - path)) + L"\\sefdata";

    // The text call: a near stub to LogTextHook, then the call's rel32 pointed at it.
    uint8_t* call = g_storeLogs.textCall;
    Code code;
    code.JumpTo(reinterpret_cast<const void*>(&LogTextHook));
    uint8_t* stub = PlaceStub(code, call);
    int32_t rel = 0;
    if (!stub || call[0] != 0xE8 || !Rel32(call + 5, stub, &rel)) {
        Log("script storage: no room for the log text stub; off");
        return;
    }
    g_origProcessString = reinterpret_cast<ProcessStringFn>(g_storeLogs.processString);
    if (!HookLogs(g_storeLogs.logEffect, g_storeLogs.logEffectText, g_storeLogs.logTrigger, g_storeLogs.logTriggerText)) {
        Log("script storage: failed to hook CLogEffect or CLogTrigger; off");
        return;
    }
    if (!WriteCode(call + 1, &rel, sizeof(rel))) {
        Log("script storage: failed to redirect the log text call; off");
        return;
    }
    if (!InstallSaves(img) || !InstallStoreSaveWrite(img)) {
        Log("script storage: save persistence is off; save hooks could not be installed");
    }
    uint8_t* readExecute = nullptr;
    if (!g_bench.gameState || !g_origCheckTick) {
        Log("script storage: no tick hook; run is off");
    } else if (!locate::LocateEffectCommand(img, g_bench.gameState, &g_storeEffectCommand, &readExecute, error)) {
        g_storeEffectCommand = nullptr;
        Log("script storage: %s; run is off", error.c_str());
    }
    g_storeOn = true;
    Log("script storage: on; log = \"@sef:...\"; variables: access +%llX, get +%llX, set +%llX, map +0x%zx (value +0x%zx); "
        "log text call +%llX; multiplayer flag +0x%zx; data in sefdata\\",
        static_cast<unsigned long long>(g_vars.access - reinterpret_cast<uint8_t*>(g_base)),
        static_cast<unsigned long long>(g_vars.get - reinterpret_cast<uint8_t*>(g_base)),
        static_cast<unsigned long long>(g_vars.set - reinterpret_cast<uint8_t*>(g_base)), g_vars.map, g_vars.entryValue,
        static_cast<unsigned long long>(call - reinterpret_cast<uint8_t*>(g_base)), g_mp.flag);
}

// ===========================================================================

std::atomic<ULONGLONG> g_lastSummary{0};

void MaybeLogSummary() {
    ULONGLONG now = GetTickCount64();
    ULONGLONG last = g_lastSummary.load(std::memory_order_relaxed);
    if (now - last < 30000 || !g_lastSummary.compare_exchange_strong(last, now)) return;
    Log("summary: fleets %llu sorts, %llu compares, %llu keys, avg %.1f us, max %.1f us%s | "
        "megastructures %llu sorts, %llu names, avg %.1f us, max %.1f us%s",
        g_fleetTiming.passes, g_fleetCompares, g_fleetKeyBuilds, g_fleetTiming.AvgUs(), g_fleetTiming.MaxUs(),
        g_fleetFailed ? " (OFF after a fault)" : "", g_megaTiming.passes, g_megaNameBuilds, g_megaTiming.AvgUs(),
        g_megaTiming.MaxUs(), g_megaFailed ? " (OFF after a fault)" : "");
    std::string heap = alloc::Summary();
    if (!heap.empty()) Log("%s", heap.c_str());
    std::string intel = IntelSummary();
    if (!intel.empty()) Log("%s", intel.c_str());
    std::string combat = CombatSummary();
    if (!combat.empty()) Log("%s", combat.c_str());
    std::string afford = AffordSummary();
    if (!afford.empty()) Log("%s", afford.c_str());
    std::string income = IncomeSummary();
    if (!income.empty()) Log("%s", income.c_str());
    std::string lexer = LexerSummary();
    if (!lexer.empty()) Log("%s", lexer.c_str());
    std::string deferred = DeferredSummary();
    if (!deferred.empty()) Log("%s", deferred.c_str());
    std::string files = FileLookupSummary();
    if (!files.empty()) Log("%s", files.c_str());
    std::string store = StoreSummary();
    if (!store.empty()) Log("%s", store.c_str());
    std::string ai = AiFixSummary();
    if (!ai.empty()) Log("%s", ai.c_str());
}

// True when the host process is stellaris.exe.
bool IsStellaris() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    return _wcsicmp(name, L"stellaris.exe") == 0;
}

// The hooks that log summaries run only in a game; this covers the load and the main menu.
DWORD WINAPI SummaryThread(void*) {
    for (;;) {
        Sleep(30000);
        MaybeLogSummary();
    }
}

void Install(HMODULE self) {
    QueryPerformanceFrequency(&g_qpcFreq);
    Log("stellaris-engine-fixes loaded, stellaris.exe base=%p", reinterpret_cast<void*>(g_base));
    LONGLONG t0 = Now();
    locate::Image img = locate::Describe(reinterpret_cast<uint8_t*>(g_base));
    alloc::Install(img, &Log);  // first: before the game's CRT allocates anything
    std::string error;
    if (!locate::LocateStrings(img, g_str, error)) {
        Log("%s; this stellaris.exe differs from the versions stellaris-engine-fixes knows, nothing is hooked", error.c_str());
        return;
    }
    InstallFleets(img);
    InstallMegastructures(img);
    InstallBench(self, img);
    InstallScriptStorage(self, img);  // after InstallBench: run needs the tick hook
    InstallAiFixes(self, img);  // after InstallBench: the AI schedule reads the date it finds
    InstallIntel(self, img);
    InstallCombat(self, img);
    InstallMegaAfford(self, img);
    InstallIncomeTarget(self, img);
    InstallLexer(self, img);
    InstallModifierDocs(self, img);
    InstallDeferredRefs(self, img);
    InstallFileLookups(self);
    Log("setup took %.0f ms", 1e3 * (Now() - t0) / g_qpcFreq.QuadPart);
    g_lastSummary = GetTickCount64();
    HANDLE thread = CreateThread(nullptr, 0, &SummaryThread, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if (!IsStellaris()) return TRUE;  // other programs in the game folder: plain proxy
        OpenLog(module);
        Install(module);
    } else if (reason == DLL_PROCESS_DETACH) {
        StoreExit();
    }
    return TRUE;
}
