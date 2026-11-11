// stellaris-engine-fixes: proxy DLL that sorts two outliner groups of Stellaris by name, naturally
// (numbers by value):
//   fleets           same-name fleets by the oldest construction date among their ships,
//                    then the game's order (fleet ID)
//   megastructures   same-name megastructures in the game's order (when they were acquired)
// and gives the game mimalloc as its heap (allocator.h, settings in stellaris-engine-fixes.ini), and adds the
// console command sef_bench, which measures how fast game days pass and where the time goes.
// Everything in stellaris.exe is found by code patterns (locate.h); a feature whose patterns
// are not found stays off, the others still work.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <share.h>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "allocator.h"
#include "aibossthreat.h"
#include "aifleettrace.h"
#include "aiwarguard.h"
#include "game.h"
#include "incometarget.h"
#include "locate.h"
#include "logging.h"
#include "mpchecksum.h"
#include "mpstore.h"
#include "natsort.h"
#include "pathcache.h"
#include "pdxstore.h"
#include "resourceoverflow.h"
#include "fixedpointoverflow.h"
#include "fleetoverflow.h"
#include "resourcearithmetic.h"
#include "resourceledger.h"
#include "numericdisplay.h"
#include "diplomacyoverflow.h"
#include "fleetstats.h"
#include "countrytotals.h"
#include "forecastoverflow.h"
#include "incomeoverflow.h"
#include "diploscores.h"
#include "shipformula.h"
#include "shipstats.h"
#include "componentmath.h"
#include "shipdamage.h"
#include "designcache.h"
#include "diplovotes.h"
#include "incomelifetime.h"
#include "diplodisplay.h"
#include "federationsums.h"
#include "uishortcuts.h"

#pragma comment(lib, "user32.lib")

// Exports of the system DLL we stand in for, forwarded to the real one.
#include "exports_winmm.h"

namespace {

// ---------------------------------------------------------------------------
// Logging

FILE* g_log = nullptr;
CRITICAL_SECTION g_logLock;

void Log(LogLevel level, const char* component, const char* fmt, ...) {
    if (!g_log) return;
    DWORD lastError = GetLastError();
    EnterCriticalSection(&g_logLock);
    SYSTEMTIME t;
    GetSystemTime(&t);
    va_list args;
    va_start(args, fmt);
    WriteLogRecord(g_log, t, level, component, fmt, args);
    va_end(args);
    fflush(g_log);
    LeaveCriticalSection(&g_logLock);
    SetLastError(lastError);
}

void OpenLog(HMODULE self) {
    InitializeCriticalSection(&g_logLock);
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return;
    constexpr wchar_t filename[] = L"stellaris-engine-fixes.log";
    size_t remaining = MAX_PATH - (slash + 1 - path);
    if (remaining < _countof(filename)) return;
    wcscpy_s(slash + 1, remaining, filename);
    g_log = _wfsopen(path, L"w", _SH_DENYWR);  // others may read it while the game runs
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

// The outliner hooks and the summary thread both check the 30-second stats interval.
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
        Log(LogLevel::Error, "fleets", "%s reading fleet %08X (name=%d age=%lld); fleets fall back to the game's order",
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
    Log(LogLevel::Debug, "fleets", "fleet order for group %p (%zu fleets):", group, g_fleetPassIds.size());
    for (uint32_t id : g_fleetPassIds) {
        const FleetKey& k = FleetKeyFor(id);
        Log(LogLevel::Debug, "fleets", "  %08X  %-40s oldest ship %s", id, k.name.c_str(), FormatDate(k.oldestShip).c_str());
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
        Log(LogLevel::Error, "fleets", "%s; fleet sorting is off", error.c_str());
        return;
    }
    Log(LogLevel::Debug, "fleets", "fleet db +%llX (id at +0x%zx), ship db +%llX (id at +0x%zx), ships at +0x%zx/+0x%zx, "
        "name via +0x%zx vtable +0x%zx, ship construction date at +0x%zx",
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_fleet.fleetDb) - g_base), g_fleet.fleetIdOffset,
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_fleet.shipDb) - g_base), g_fleet.shipIdOffset,
        g_fleet.fleetShipsOffset, g_fleet.fleetShipCountOffset, g_fleet.fleetNameSubobject,
        g_fleet.fleetNameVtblOffset, g_fleet.shipDateOffset);
    g_origFleetUpdate = reinterpret_cast<game::FleetGroupUpdateFn>(g_fleet.fleetGroupUpdate);
    auto tramp = InstallJumpHook(g_fleet.compare, locate::kCompareStolenBytes, reinterpret_cast<void*>(&CompareHook));
    if (!tramp) {
        Log(LogLevel::Error, "fleets", "failed to hook SSortFleets::Compare; fleet sorting is off");
        return;
    }
    g_origCompare = reinterpret_cast<game::CompareFn>(tramp);
    if (!SwapVtableSlot(g_fleet.fleetGroupVtblSlot, g_fleet.fleetGroupUpdate,
                        reinterpret_cast<void*>(&FleetGroupUpdateHook))) {
        // The comparator hook alone passes everything through to the original.
        Log(LogLevel::Error, "fleets", "failed to hook COutlinerGroupFleet::UpdateInternal; fleet sorting is off");
        return;
    }
    Log(LogLevel::Info, "fleets", "hooks installed");
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
        Log(LogLevel::Error, "megastructures", "fault reading a megastructure name; megastructures fall back to the game's order");
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
            Log(LogLevel::Error, "megastructures", "replay showed %d (%zu entries) but the first run showed %d", replayed,
                g_megaReplayIndex, shown);
        }
    }
    g_megaTiming.Add(Now() - t0);

    if (g_megaOrdersLogged < 1 && !g_megaSorted.empty()) {
        ++g_megaOrdersLogged;
        Log(LogLevel::Debug, "megastructures", "megastructure order for group %p (%zu shown):", group, g_megaSorted.size());
        ULONGLONG now = GetTickCount64();
        for (uint8_t* mega : g_megaSorted) {
            const std::string* name = MegaNameFor(mega, now);
            Log(LogLevel::Debug, "megastructures", "  %08X  %s", *reinterpret_cast<uint32_t*>(mega + g_mega.megaIdOffset),
                name ? name->c_str() : "?");
        }
    }
    MaybeLogSummary();
    return replayed;
}

void InstallMegastructures(const locate::Image& img) {
    std::string error;
    if (!locate::LocateMega(img, g_mega, error)) {
        Log(LogLevel::Error, "megastructures", "%s; megastructure sorting is off", error.c_str());
        return;
    }
    Log(LogLevel::Debug, "megastructures", "db +%llX (id at +0x%zx), type at +0x%zx (id at +0x%zx), own name at +0x%zx if +0x%zx is set",
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_mega.megaDb) - g_base), g_mega.megaIdOffset,
        g_mega.megaTypeOffset, g_mega.typeIdOffset, g_mega.megaCustomName, g_mega.megaCustomNameFlag);
    g_origMegaUpdate = reinterpret_cast<game::FleetGroupUpdateFn>(g_mega.groupUpdate);
    auto tramp = InstallJumpHook(g_mega.entryUpdate, locate::kMegaEntryStolenBytes,
                                 reinterpret_cast<void*>(&MegaEntryHook));
    if (!tramp) {
        Log(LogLevel::Error, "megastructures", "failed to hook UpdateEntry; megastructure sorting is off");
        return;
    }
    g_origMegaEntry = reinterpret_cast<MegaEntryUpdateFn>(tramp);  // with mode Off the hook passes through
    if (!SwapVtableSlot(g_mega.groupVtblSlot, g_mega.groupUpdate, reinterpret_cast<void*>(&MegaGroupUpdateHook))) {
        Log(LogLevel::Error, "megastructures", "failed to hook COutlinerGroupMegastructure::UpdateInternal; megastructure sorting is off");
        return;
    }
    Log(LogLevel::Info, "megastructures", "hooks installed");
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
        if (!g_intelFailed.exchange(true)) Log(LogLevel::Error, "fleet_intel", "fault in the fast lookup; it is off from now on");
        g_intelOn = false;
        return g_origIntel(country, system, owner);
    }
    g_intelFast.fetch_add(1, std::memory_order_relaxed);
    if (!g_intelVerify) return static_cast<uint32_t>(fast);
    uint64_t game = g_origIntel(country, system, owner);
    g_intelCompared.fetch_add(1, std::memory_order_relaxed);
    if (static_cast<int32_t>(game) != fast && g_intelMismatches.fetch_add(1) < 20) {
        Log(LogLevel::Error, "fleet_intel", "MISMATCH: country %p system %d owner %08X: game %d, fast %d", country, system, owner,
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
        Log(LogLevel::Info, "fleet_intel", "off (fast_intel_lookup=0)");
        return;
    }
    std::string error;
    if (!locate::LocateIntel(img, g_intel, error)) {
        Log(LogLevel::Error, "fleet_intel", "%s; the fast lookup is off", error.c_str());
        return;
    }
    Log(LogLevel::Debug, "fleet_intel", "lookup +%llX, search +%llX; relations +0x%zx/+0x%zx, systems +0x%zx/+0x%zx, owner intel +0x%zx",
        static_cast<unsigned long long>(g_intel.lookup - reinterpret_cast<uint8_t*>(g_base)),
        static_cast<unsigned long long>(g_intel.search - reinterpret_cast<uint8_t*>(g_base)), g_intel.relData,
        g_intel.relCount, g_intel.sysData, g_intel.sysCount, g_intel.ownerIntel);
    auto lookup = InstallJumpHook(g_intel.lookup, locate::kIntelLookupStolenBytes, reinterpret_cast<void*>(&IntelHook));
    if (!lookup) {
        Log(LogLevel::Error, "fleet_intel", "failed to hook the lookup; the fast lookup is off");
        return;
    }
    g_origIntel = reinterpret_cast<game::IntelFn>(lookup);  // passes through until g_intelOn
    auto search = InstallJumpHook(g_intel.search, locate::kSearchStolenBytes, reinterpret_cast<void*>(&SearchHook));
    if (!search) {
        Log(LogLevel::Error, "fleet_intel", "failed to hook the search; the fast lookup is off");
        return;
    }
    g_origSearch = reinterpret_cast<game::SearchFn>(search);
    g_intelOn = true;
    Log(LogLevel::Info, "fleet_intel", "hooks installed; fast lookup on%s", g_intelVerify ? ", verifying every lookup against the game's" : "");
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
        Log(LogLevel::Info, "combat_map", "off (skip_empty_combat_entries=0)");
        return;
    }
    std::string error;
    if (!locate::LocateCombat(img, g_combat, error)) {
        Log(LogLevel::Error, "combat_map", "%s; off", error.c_str());
        return;
    }
    Log(LogLevel::Debug, "combat_map", "insert +%llX, count +%llX; entry counts +0x%zx..+0x%zx, field +0x%zx",
        static_cast<unsigned long long>(g_combat.insert - reinterpret_cast<uint8_t*>(g_base)),
        static_cast<unsigned long long>(reinterpret_cast<uint8_t*>(g_combat.count) - reinterpret_cast<uint8_t*>(g_base)),
        g_combat.countFirst, g_combat.countLast, g_combat.tail);
    g_combatIds.reserve(256);
    auto tramp = InstallJumpHook(g_combat.insert, locate::kCombatInsertStolenBytes,
                                 reinterpret_cast<void*>(&CombatInsertHook));
    if (!tramp) {
        Log(LogLevel::Error, "combat_map", "failed to hook the insert; off");
        return;
    }
    g_origCombatInsert = reinterpret_cast<game::CombatInsertFn>(tramp);
    Log(LogLevel::Info, "combat_map", "hook installed; empty entries are not inserted");
}

void LogCombatStats() {
    if (!g_origCombatInsert) return;
    Log(LogLevel::Stats, "combat_map", "inserts=%llu empty_skipped=%llu skipped_pct=%.1f", g_combatInserts, g_combatSkipped,
        g_combatInserts ? 100.0 * g_combatSkipped / g_combatInserts : 0.0);
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
                Log(LogLevel::Error, "mega_afford", "CanAfford MISMATCH: type %p country %p: game %d, cached %d", type, country, game,
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
        Log(LogLevel::Info, "mega_afford", "off (mega_afford_cache=0)");
        return;
    }
    g_affordVerify = !ini.empty() && GetPrivateProfileIntA("speed", "mega_afford_verify", 0, ini.c_str()) != 0;
    std::string error;
    if (!locate::LocateMegaAfford(img, g_afford, error)) {
        Log(LogLevel::Error, "mega_afford", "%s; off", error.c_str());
        return;
    }
    Log(LogLevel::Debug, "mega_afford", "CanAfford +%llX, FindBestPositionForMegaStructure +%llX",
        static_cast<unsigned long long>(g_afford.canAfford - reinterpret_cast<uint8_t*>(g_base)),
        static_cast<unsigned long long>(g_afford.findBest - reinterpret_cast<uint8_t*>(g_base)));
    // CanAfford first: until FindBestPosition is hooked, t_affordDepth stays 0 and it passes through.
    auto afford = InstallJumpHook(g_afford.canAfford, locate::kCanAffordStolenBytes,
                                  reinterpret_cast<void*>(&CanAffordHook));
    if (!afford) {
        Log(LogLevel::Error, "mega_afford", "failed to hook CanAfford; off");
        return;
    }
    g_origCanAfford = reinterpret_cast<game::CanAffordFn>(afford);
    auto find = InstallJumpHook(g_afford.findBest, locate::kFindBestPositionStolenBytes,
                                reinterpret_cast<void*>(&FindBestHook));
    if (!find) {
        Log(LogLevel::Error, "mega_afford", "failed to hook FindBestPositionForMegaStructure; off");
        return;
    }
    g_origFindBest = reinterpret_cast<game::FindBestPositionFn>(find);
    Log(LogLevel::Info, "mega_afford", "hooks installed; CanAfford cached per search%s",
        g_affordVerify ? ", verifying every cached answer against the game's" : "");
}

void LogAffordStats() {
    if (!g_origFindBest) return;
    uint64_t calls = g_affordCalls.load(), cached = g_affordCached.load();
    Log(LogLevel::Stats, "mega_afford", "searches=%llu calls=%llu cached=%llu cached_pct=%.1f verify=%d mismatches=%llu",
        g_affordScopes.load(), calls, cached, calls ? 100.0 * cached / calls : 0.0, g_affordVerify, g_affordMismatches.load());
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
            Log(LogLevel::Error, "token_strings", "MISMATCH: token %d: hook \"%s\", game \"%.*s\"", i, mine[i].c_str(),
                static_cast<int>(strings[i].size), strings[i].data());
        }
    }
}

void InstallLexer(HMODULE self, const locate::Image& img) {
    std::string ini = IniPath(self);
    if (!ini.empty() && GetPrivateProfileIntA("speed", "fast_token_rebuild", 1, ini.c_str()) == 0) {
        Log(LogLevel::Info, "token_strings", "off (fast_token_rebuild=0)");
        return;
    }
    g_lexerVerify = !ini.empty() && GetPrivateProfileIntA("speed", "fast_token_rebuild_verify", 0, ini.c_str()) != 0;
    std::string error;
    if (!locate::LocateLexer(img, g_lexer, error)) {
        Log(LogLevel::Error, "token_strings", "%s; off", error.c_str());
        return;
    }
    Log(LogLevel::Debug, "token_strings", "RebuildLookup +%llX; table +0x%zx (data +0x%zx, size +0x%zx), wanted +0x%zx, dynamic +0x%zx/+0x%zx",
        static_cast<unsigned long long>(g_lexer.rebuild - reinterpret_cast<uint8_t*>(g_base)), g_lexer.array, g_lexer.data,
        g_lexer.count, g_lexer.wanted, g_lexer.dynData, g_lexer.dynCount);
    auto tramp = InstallJumpHook(g_lexer.rebuild, locate::kRebuildLookupStolenBytes,
                                 reinterpret_cast<void*>(&RebuildLookupHook));
    if (!tramp) {
        Log(LogLevel::Error, "token_strings", "failed to hook RebuildLookup; off");
        return;
    }
    g_origRebuild = reinterpret_cast<game::RebuildLookupFn>(tramp);
    Log(LogLevel::Info, "token_strings", "hook installed%s", g_lexerVerify ? "; verifying every 16th call against the game's" : "");
}

void LogLexerStats() {
    if (!g_origRebuild) return;
    Log(LogLevel::Stats, "token_strings", "full_rebuilds=%llu incremental_rebuilds=%llu tokens_added=%llu "
        "verify=%d compared=%llu mismatches=%llu", g_lexerFull.load(), g_lexerIncremental.load(), g_lexerNew.load(),
        g_lexerVerify, g_lexerCompared.load(), g_lexerMismatches.load());
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
        Log(LogLevel::Info, "modifier_docs", "written as usual (skip_modifier_docs=0)");
        return;
    }
    locate::ModifierDocsLocations docs;
    std::string error;
    if (!locate::LocateModifierDocs(img, docs, error)) {
        Log(LogLevel::Error, "modifier_docs", "%s; written as usual", error.c_str());
        return;
    }
    if (!InstallJumpHook(docs.logDefinitions, locate::kLogDefinitionsStolenBytes, reinterpret_cast<void*>(&LogDefinitionsHook))) {
        Log(LogLevel::Error, "modifier_docs", "failed to hook LogDefinitions; written as usual");
        return;
    }
    Log(LogLevel::Info, "modifier_docs", "LogDefinitions +%llX skipped; modifiers.log stays empty",
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
                    Log(LogLevel::Error, "deferred_refs", "MISMATCH, owner %p has an entry the set doesn't know", owner);
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
        Log(LogLevel::Info, "deferred_refs", "off (fast_deferred_unregister=0)");
        return;
    }
    g_deferredVerify = !ini.empty() && GetPrivateProfileIntA("speed", "fast_deferred_unregister_verify", 0, ini.c_str()) != 0;
    std::string error;
    if (!locate::LocateDeferredRefs(img, g_deferred, error)) {
        Log(LogLevel::Error, "deferred_refs", "%s; off", error.c_str());
        return;
    }
    Log(LogLevel::Debug, "deferred_refs", "Register_Internal +%llX, Unregister +%llX; list +0x%zx/+0x%zx, entries 0x%zx bytes, owner +0x%zx",
        static_cast<unsigned long long>(g_deferred.registerRef - reinterpret_cast<uint8_t*>(g_base)),
        static_cast<unsigned long long>(g_deferred.unregister - reinterpret_cast<uint8_t*>(g_base)), g_deferred.data,
        g_deferred.count, g_deferred.entrySize, g_deferred.owner);
    g_deferredOwners = new std::unordered_set<const void*>();
    // Nothing has registered yet at this point, so the set starts complete. Register first: with
    // only Unregister hooked, an owner it hasn't seen would wrongly be skipped.
    auto reg = InstallJumpHook(g_deferred.registerRef, locate::kDeferredRegisterStolenBytes,
                               reinterpret_cast<void*>(&DeferredRegisterHook));
    if (!reg) {
        Log(LogLevel::Error, "deferred_refs", "failed to hook Register_Internal; off");
        return;
    }
    g_origDeferredRegister = reinterpret_cast<game::DeferredRegisterFn>(reg);
    auto unreg = InstallJumpHook(g_deferred.unregister, locate::kDeferredUnregisterStolenBytes,
                                 reinterpret_cast<void*>(&DeferredUnregisterHook));
    if (!unreg) {
        Log(LogLevel::Error, "deferred_refs", "failed to hook Unregister; off");
        return;
    }
    g_origDeferredUnregister = reinterpret_cast<game::DeferredUnregisterFn>(unreg);
    Log(LogLevel::Info, "deferred_refs", "hooks installed%s", g_deferredVerify ? ", verifying every skip against the list" : "");
}

void LogDeferredStats() {
    if (!g_origDeferredUnregister) return;
    uint64_t calls = g_deferredCalls.load(), skipped = g_deferredSkipped.load();
    Log(LogLevel::Stats, "deferred_refs", "registered=%llu unregisters=%llu skipped=%llu skipped_pct=%.1f "
        "verify=%d mismatches=%llu", g_deferredRegistered.load(), calls, skipped, calls ? 100.0 * skipped / calls : 0.0,
        g_deferredVerify, g_deferredMismatches.load());
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

std::string LogPath(LPCWSTR path) {
    int size = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
    if (!size) return "[unavailable path]";
    std::string text(size, '\0');
    if (!WideCharToMultiByte(CP_UTF8, 0, path, -1, text.data(), size, nullptr, nullptr)) {
        return "[unavailable path]";
    }
    text.pop_back();  // The size returned by Windows includes the terminator.
    return text;
}

void CompareFileAnswer(LPCWSTR path, DWORD cached, bool found, DWORD real) {
    g_fileCompared.fetch_add(1, std::memory_order_relaxed);
    if ((found || real != cached) && g_fileMismatches.fetch_add(1) < 20) {
        Log(LogLevel::Error, "file_lookups", "MISMATCH: cache says error %lu, file system says %s %lu: %s",
            cached, found ? "found" : "error", found ? 0 : real, LogPath(path).c_str());
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
        Log(LogLevel::Info, "file_lookups", "off (fast_file_lookups=0)");
        return;
    }
    g_filesVerify = !ini.empty() && GetPrivateProfileIntA("speed", "fast_file_lookups_verify", 0, ini.c_str()) != 0;
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        Log(LogLevel::Error, "file_lookups", "no exe path; off");
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
        Log(LogLevel::Error, "file_lookups", "CreateFileW isn't imported; off");
        return;
    }
    if (!HookKernel32Import("GetFileAttributesExW", reinterpret_cast<void*>(&GetAttributesHook), reinterpret_cast<void**>(&g_origGetAttributes))) {
        Log(LogLevel::Warn, "file_lookups", "GetFileAttributesExW isn't imported; only opens are answered");
    }
    g_filesOn = true;
    Log(LogLevel::Info, "file_lookups", "import hooks installed (%d of 5 calls that change folders); game folder %s%s",
        writerHooks, LogPath(exe).c_str(),
        g_filesVerify ? "; verifying every answer against the file system" : "");
}

void LogFileLookupStats() {
    if (!g_filesOn) return;
    uint64_t lookups = g_fileLookups.load();
    uint64_t answered = g_filesVerify ? g_fileCompared.load() : g_fileAnswered.load();
    Log(LogLevel::Stats, "file_lookups", "lookups=%llu answered_missing=%llu answered_missing_pct=%.1f folder_listings=%llu "
        "verify=%d mismatches=%llu", lookups, answered, lookups ? 100.0 * answered / lookups : 0.0, g_files.Listings(),
        g_filesVerify, g_fileMismatches.load());
}

void LogIntelStats() {
    if (!g_origSearch) return;
    Log(LogLevel::Stats, "fleet_intel", "searches=%llu fast_lookups=%llu faulted=%d verify=%d compared=%llu mismatches=%llu",
        g_intelSearches.load(), g_intelFast.load(), g_intelFailed.load(), g_intelVerify, g_intelCompared.load(),
        g_intelMismatches.load());
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
    Log(LogLevel::Stats, "sef_bench", "wall_s=%.3f active_s=%.3f paused_s=%.3f speed=%d turn_time_s=%.3f "
        "frame_smoothing=%d speed_changed=%d smoothing_changed=%d in_tick_frame_ms=%d skipped_frames=%llu "
        "batch_ticks=%d batches=%llu intel_on=%d intel_verify=%d intel_searches=%llu intel_fast_lookups=%llu "
        "combat_skip_on=%d combat_inserts=%llu combat_skipped=%llu days=%llu ticks=%llu days_per_s=%.3f ms_per_day=%.3f "
        "first_date=%s last_date=%s simulation_s=%.3f in_tick_s=%.3f in_tick_frames=%llu in_tick_frame_s=%.3f "
        "between_tick_s=%.3f between_tick_frames=%llu between_command_frames=%llu between_command_frame_s=%.3f "
        "longest_tick_ms=%.3f longest_tick_date=%s",
        wall, active, paused, r.speed + 1, turnTime, smoothing, r.speedChanged, smoothing != r.smoothingAtStart,
        g_pacing.inTickFrameMs, r.skippedFrames, g_pacing.batchTicks, r.batches, g_intelOn.load(), g_intelVerify,
        g_intelSearches.load() - r.intelSearches, g_intelFast.load() - r.intelFast, g_combatWanted,
        g_combatInserts - r.combatInserts, g_combatSkipped - r.combatSkipped, r.days, r.ticks,
        days / active, r.days ? 1e3 * active / days : 0.0, r.days ? FormatDate(r.firstDate).c_str() : "none",
        r.days ? FormatDate(r.lastDate).c_str() : "none", sim, inTicks, r.insideFrames, inside, rest, r.frames,
        r.betweenFrames, between, 1e3 * Seconds(r.longestTick),
        r.ticks ? FormatDate(r.longestTickDate).c_str() : "none");
    if (!r.days) Log(LogLevel::Warn, "sef_bench", "no full game day elapsed");
    if (r.speedChanged || smoothing != r.smoothingAtStart) {
        Log(LogLevel::Warn, "sef_bench", "game speed or frame smoothing changed during the run");
    }
    g_benchReport = out;
}

// Runs one of stellaris-engine-fixes' own commands so its reply reaches the console, without recording it
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

// ---- Script storage save hooks ----

locate::SaveLocations g_saves;
using StartSaveFn = uint32_t(__fastcall*)(void* manager, const game::String* path, bool a, bool b, bool c);
using GameStateLoadFn = void(__fastcall*)(void* state, void* file, const game::String* path);
using NewGameFn = void(__fastcall*)(void* state);
StartSaveFn g_origStartSave = nullptr;
GameStateLoadFn g_origLoad = nullptr;
NewGameFn g_origNewGame = nullptr;

int32_t CopyGameString(const game::String* text, char* out, int32_t size);

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
    if (result == 0) StoreSaveStarted(manager);  // 0: the save has started
    return result;
}

void __fastcall LoadHook(void* state, void* file, const game::String* path) {
    char text[1024];
    bool read = CopySavePath(path, text, sizeof(text));
    if (read) StoreLoading(text);  // before the load, which may already run script
    g_origLoad(state, file, path);
}

void __fastcall NewGameHook(void* state) {
    StoreNewGame();  // before on_game_start runs
    g_origNewGame(state);
}

// Hook saves, loads and new games for the save store.
bool InstallSaves(const locate::Image& img) {
    std::string error;
    if (!locate::LocateSaves(img, g_saves, error)) {
        Log(LogLevel::Error, "save_hooks", "%s; save storage is off", error.c_str());
        return false;
    }
    // Install save tracking after the load and new-game hooks.
    auto newGame = InstallJumpHook(g_saves.newGame, locate::kNewGameStartedStolenBytes, reinterpret_cast<void*>(&NewGameHook));
    auto load = newGame ? InstallJumpHook(g_saves.load, locate::kGameStateLoadStolenBytes, reinterpret_cast<void*>(&LoadHook))
                        : nullptr;
    if (load) g_origLoad = reinterpret_cast<GameStateLoadFn>(load);
    if (newGame) g_origNewGame = reinterpret_cast<NewGameFn>(newGame);
    auto save = load ? InstallJumpHook(g_saves.startSave, locate::kStartSaveToPathStolenBytes,
                                       reinterpret_cast<void*>(&StartSaveHook))
                     : nullptr;
    if (!save) {
        Log(LogLevel::Error, "save_hooks", "failed to hook saving and loading; save storage is off");
        return false;
    }
    g_origStartSave = reinterpret_cast<StartSaveFn>(save);
    Log(LogLevel::Debug, "save_hooks", "hooks: StartSaveToPath +%llX, CGameState::Load +%llX, OnNewGameStarted +%llX",
        static_cast<unsigned long long>(g_saves.startSave - reinterpret_cast<uint8_t*>(g_base)),
        static_cast<unsigned long long>(g_saves.load - reinterpret_cast<uint8_t*>(g_base)),
        static_cast<unsigned long long>(g_saves.newGame - reinterpret_cast<uint8_t*>(g_base)));
    return true;
}

// Copies a CString's text into `out` (NUL-terminated, cut to fit); returns its full length.
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

// ---- Script storage log hooks ----

locate::LogScriptLocations g_storeLogs;
using LogEffectFn = void(__fastcall*)(uint8_t* effect, uint8_t* scope);
using LogTriggerFn = bool(__fastcall*)(uint8_t* trigger, uint8_t* scope);
LogEffectFn g_origLogEffect = nullptr;
LogTriggerFn g_origLogTrigger = nullptr;

// Copy the command following the storage prefix, or leave the log call to the game.
bool ReadStoreCommand(const uint8_t* object, size_t textOffset, char* buf, size_t size) {
    constexpr char prefix[] = "@sef:";
    constexpr size_t prefixSize = sizeof(prefix) - 1;
    __try {
        const auto* text = reinterpret_cast<const game::String*>(object + textOffset);
        if (text->size < prefixSize || text->size >= size || memcmp(text->data(), prefix, prefixSize) != 0)
            return false;
        size_t length = text->size - prefixSize;
        memcpy(buf, text->data() + prefixSize, length);
        buf[length] = '\0';
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __fastcall LogEffectHook(uint8_t* effect, uint8_t* scope) {
    char buf[1024];
    if (g_storeOn && ReadStoreCommand(effect, g_storeLogs.logEffectText, buf, sizeof(buf))) return StoreEffect(buf, scope);
    g_origLogEffect(effect, scope);
}

bool __fastcall LogTriggerHook(uint8_t* trigger, uint8_t* scope) {
    char buf[1024];
    if (g_storeOn && ReadStoreCommand(trigger, g_storeLogs.logTriggerText, buf, sizeof(buf))) return StoreTrigger(buf, scope);
    return g_origLogTrigger(trigger, scope);
}

bool HookLogs() {
    uint8_t* e = InstallJumpHook(g_storeLogs.logEffect, locate::kLogScriptStolenBytes, reinterpret_cast<void*>(&LogEffectHook));
    if (!e) return false;
    g_origLogEffect = reinterpret_cast<LogEffectFn>(e);
    // If the trigger hook fails, the effect hook still forwards to the game.
    uint8_t* t = InstallJumpHook(g_storeLogs.logTrigger, locate::kLogScriptStolenBytes, reinterpret_cast<void*>(&LogTriggerHook));
    if (!t) return false;
    g_origLogTrigger = reinterpret_cast<LogTriggerFn>(t);
    return true;
}

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
            Log(LogLevel::Info, "sef_bench", "skip empty combat entries %s", g_combatWanted ? "on" : "off");
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
    Log(LogLevel::Info, "sef_bench", "started, %.0f s", seconds);
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
        Log(LogLevel::Error, "ticks", "%s; tick pacing and the sef_bench command are off", error.c_str());
        return;
    }
    ReadPacingSettings(self);
    Log(LogLevel::Debug, "ticks", "idler speed +0x%zx, paused +0x%zx, multiplayer +0x%zx; game state tick +0x%zx, date +0x%zx; console lock flag +0x%zx",
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
            Log(LogLevel::Error, "ticks", "failed to hook %s; tick pacing and the sef_bench command are off", h.what);
            g_pacing = PacingSettings{0, false};
            return;
        }
        *h.orig = tramp;
    }
    g_minFrameGap = g_qpcFreq.QuadPart * g_pacing.inTickFrameMs / 1000;
    Log(LogLevel::Info, "ticks", "hooks installed; frames inside ticks %s%d ms, tick batching at the fastest speed %s; console command sef_bench",
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
//   build_plan_skip_unaffordable  2.16  the weekly walk passes over an item it can't order, instead of stopping
//   colonizer_idle_days=N  2.17  a colony ship idle for N days no longer stops another being built
//   constructor_idle_days=N  2.18  a construction ship without work for N days looks further away
//   survey_blacklist_monthly  2.15  systems a science ship couldn't survey are tried again after a month, not a year

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

// Keep AddResource's whole-unit quotient in int64 (issue #3; re/numeric_overflow.md).
void InstallResourceRounding(HMODULE self, const locate::Image& img) {
    std::string ini = IniPath(self);
    if (ini.empty() || GetPrivateProfileIntA("numeric_fixes", "resource_amount_rounding", 0, ini.c_str()) == 0) {
        Log(LogLevel::Info, "resource_rounding", "off (resource_amount_rounding=0)");
        return;
    }
    std::string error;
    uint8_t* at = resourceoverflow::Locate(img, error);
    if (!at) {
        Log(LogLevel::Error, "resource_rounding", "%s; off", error.c_str());
        return;
    }
    uint8_t patched[resourceoverflow::kSize];
    if (!resourceoverflow::Prepare(at, patched) || !WriteCode(at, patched, sizeof(patched))) {
        Log(LogLevel::Error, "resource_rounding", "could not patch both conversions; off");
        return;
    }
    Log(LogLevel::Info, "resource_rounding", "int64 whole units at +%llX (stockpile and related situations)",
        static_cast<unsigned long long>(at - img.base));
}

bool g_fixedConversionInstalled=false;
bool InstallFixedPointConversion(HMODULE self, const locate::Image& img, bool required=false) {
    if(g_fixedConversionInstalled)return true;
    std::string ini = IniPath(self);
    if (!required && (ini.empty() || GetPrivateProfileIntA("numeric_fixes", "fixed_point_conversion", 0, ini.c_str()) == 0)) {
        Log(LogLevel::Info, "fixed_point_conversion", "off (fixed_point_conversion=0)");
        return false;
    }
    fixedpointoverflow::Locations loc{};
    std::string error;
    if (!fixedpointoverflow::Locate(img, loc, error)) {
        Log(LogLevel::Error, "fixed_point_conversion", "%s; off", error.c_str());
        return false;
    }
    auto function = fixedpointoverflow::FunctionPatch();
    auto inlined = fixedpointoverflow::InlinePatch();
    uint8_t* sites[] = {loc.function, loc.fleetWeight, loc.techWeight};
    const uint8_t* patches[] = {function.data(), inlined.data(), inlined.data()};
    size_t sizes[] = {function.size(), inlined.size(), inlined.size()};
    DWORD previous[3];
    int protectedCount = 0;
    for (; protectedCount < 3; ++protectedCount) {
        if (VirtualProtect(sites[protectedCount], sizes[protectedCount], PAGE_EXECUTE_READWRITE,
                           &previous[protectedCount])) continue;
        break;
    }
    // Acquire write access to all three ranges before changing any code.
    if (protectedCount == 3) {
        for (int i = 0; i < 3; ++i) {
            memcpy(sites[i], patches[i], sizes[i]);
            FlushInstructionCache(GetCurrentProcess(), sites[i], sizes[i]);
        }
    }
    for (int i = protectedCount - 1; i >= 0; --i) {
        DWORD ignored;
        VirtualProtect(sites[i], sizes[i], previous[i], &ignored);
    }
    if (protectedCount != 3) {
        Log(LogLevel::Error, "fixed_point_conversion", "cannot make all three ranges writable; off");
        return false;
    }
    Log(LogLevel::Info, "fixed_point_conversion", "checked int64 output at +%llX, fleet diplomatic weight +%llX, tech weight +%llX",
        static_cast<unsigned long long>(loc.function-img.base),
        static_cast<unsigned long long>(loc.fleetWeight-img.base),
        static_cast<unsigned long long>(loc.techWeight-img.base));
    g_fixedConversionInstalled=true;
    return true;
}

fleetoverflow::Locations g_fleetPower;
fleetoverflow::PowerFn g_originalFleetPower = nullptr;
fleetoverflow::HealthFn g_originalHealthPower = nullptr;
std::atomic<bool> g_fleetPowerFailed{false};

int64_t* __fastcall FleetPowerHook(uint8_t* fleet, int64_t* out, int mode, bool cached) {
    if (!g_fleetPowerFailed.load(std::memory_order_relaxed)) {
        __try {
            int64_t result;
            if (fleetoverflow::Compute(g_fleetPower, fleet, mode, cached, false, result)) {
                *out = result;
                return out;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (!g_fleetPowerFailed.exchange(true)) Log(LogLevel::Error, "fleet_power", "invalid state or memory fault; off from next call");
    }
    return g_originalFleetPower(fleet, out, mode, cached);
}

int64_t* __fastcall HealthPowerHook(uint8_t* fleet, int64_t* out) {
    if (!g_fleetPowerFailed.load(std::memory_order_relaxed)) {
        __try {
            int64_t result;
            if (fleetoverflow::Compute(g_fleetPower, fleet, 0, true, true, result)) {
                *out = result;
                return out;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (!g_fleetPowerFailed.exchange(true)) Log(LogLevel::Error, "fleet_power", "invalid state or memory fault; off from next call");
    }
    return g_originalHealthPower(fleet, out);
}

void InstallFleetPowerArithmetic(HMODULE self, const locate::Image& img) {
    std::string ini = IniPath(self);
    if (ini.empty() || GetPrivateProfileIntA("numeric_fixes", "fleet_power_arithmetic", 0, ini.c_str()) == 0) {
        Log(LogLevel::Info, "fleet_power", "off (fleet_power_arithmetic=0)");
        return;
    }
    std::string error;
    if (!fleetoverflow::Locate(img, g_fleetPower, error)) {
        Log(LogLevel::Error, "fleet_power", "%s; off", error.c_str());
        return;
    }
    auto power = InstallJumpHook(g_fleetPower.power, fleetoverflow::kPowerStolen, reinterpret_cast<void*>(&FleetPowerHook));
    if (!power) { Log(LogLevel::Error, "fleet_power", "cannot install aggregate hook; off"); return; }
    g_originalFleetPower = reinterpret_cast<fleetoverflow::PowerFn>(power);
    auto health = InstallJumpHook(g_fleetPower.healthPower, fleetoverflow::kHealthStolen, reinterpret_cast<void*>(&HealthPowerHook));
    if (!health) {
        // The existing aggregate trampoline remains valid even if restoring code fails.
        g_fleetPowerFailed.store(true);
        bool restored = WriteCode(g_fleetPower.power, power, fleetoverflow::kPowerStolen);
        Log(LogLevel::Error, "fleet_power", "cannot install health hook; off (aggregate restored=%d)", restored ? 1 : 0);
        return;
    }
    g_originalHealthPower = reinterpret_cast<fleetoverflow::HealthFn>(health);
    Log(LogLevel::Info, "fleet_power", "wide sums, ratios and products at +%llX / +%llX; ship power production unchanged",
        static_cast<unsigned long long>(g_fleetPower.power-img.base),
        static_cast<unsigned long long>(g_fleetPower.healthPower-img.base));
}

resourcearithmetic::Locations g_resourceArithmetic;
resourcearithmetic::MultiplyFn g_originalResourceMultiply = nullptr;
std::atomic<bool> g_resourceMultiplyFailed{false};
void __fastcall ResourceMultiplyHook(uint8_t* table, const uint8_t* resource, const int64_t* multiplier) {
    if (!g_resourceMultiplyFailed.load(std::memory_order_relaxed)) {
        __try {
            resourcearithmetic::Multiply(g_resourceArithmetic, table, resource, multiplier);
            return;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (!g_resourceMultiplyFailed.exchange(true)) Log(LogLevel::Error, "resource_multiplication", "memory fault; off from next call");
    }
    g_originalResourceMultiply(table, resource, multiplier);
}

void InstallResourceMultiplication(HMODULE self, const locate::Image& img) {
    std::string ini = IniPath(self);
    if (ini.empty() || GetPrivateProfileIntA("numeric_fixes", "resource_multiplication", 0, ini.c_str()) == 0) {
        Log(LogLevel::Info, "resource_multiplication", "off (resource_multiplication=0)");
        return;
    }
    std::string error;
    if (!resourcearithmetic::Locate(img, g_resourceArithmetic, error)) {
        Log(LogLevel::Error, "resource_multiplication", "%s; off", error.c_str()); return;
    }
    auto original = InstallJumpHook(g_resourceArithmetic.multiply, resourcearithmetic::kStolen,
                                    reinterpret_cast<void*>(&ResourceMultiplyHook));
    if (!original) { Log(LogLevel::Error, "resource_multiplication", "cannot install hook; off"); return; }
    g_originalResourceMultiply = reinterpret_cast<resourcearithmetic::MultiplyFn>(original);
    Log(LogLevel::Info, "resource_multiplication", "wide signed product at +%llX (CResourceTable::MultResource only)",
        static_cast<unsigned long long>(g_resourceArithmetic.multiply-img.base));
}

resourceledger::Locations g_resourceLedger;
resourceledger::ChangeFn g_ledgerOriginalAdd=nullptr, g_ledgerOriginalRemove=nullptr;
resourceledger::ConvertFn g_ledgerOriginalConvert=nullptr;
std::atomic<bool> g_ledgerFailed{false};
int LedgerFault(const char* operation, EXCEPTION_POINTERS* fault, const resourceledger::ChangeProgress& progress,
                void* economy, void* converter, void* table, void* resource, int64_t amount) {
    if (g_ledgerFailed.exchange(true)) return EXCEPTION_EXECUTE_HANDLER;

    const auto& record = *fault->ExceptionRecord;
    MEMORY_BASIC_INFORMATION region{};
    char modulePath[MAX_PATH]{};
    const char* module = "unknown";
    uintptr_t offset = 0;
    if (VirtualQuery(record.ExceptionAddress, &region, sizeof(region)) && region.AllocationBase) {
        offset = reinterpret_cast<uintptr_t>(record.ExceptionAddress) - reinterpret_cast<uintptr_t>(region.AllocationBase);
        DWORD length = GetModuleFileNameA(static_cast<HMODULE>(region.AllocationBase), modulePath, MAX_PATH);
        if (length && length < MAX_PATH) {
            const char* slash = strrchr(modulePath, '\\');
            module = slash ? slash + 1 : modulePath;
        }
    }
    Log(LogLevel::Error, "stockpile", "fault: operation=%s stage=%s code=%08lX instruction=%p (%s+0x%llX) "
        "access=%llu address=%p economy=%p converter=%p table=%p resource=%p amount=%lld effects_started=%d",
        operation, progress.stage, record.ExceptionCode, record.ExceptionAddress, module,
        static_cast<unsigned long long>(offset),
        record.NumberParameters > 0 ? static_cast<unsigned long long>(record.ExceptionInformation[0]) : 0,
        record.NumberParameters > 1 ? reinterpret_cast<void*>(record.ExceptionInformation[1]) : nullptr,
        economy, converter, table, resource, static_cast<long long>(amount), int(progress.effectsStarted));
    Log(LogLevel::Error, "stockpile", "single-resource hooks off from next call (no replay after side effects)");
    return EXCEPTION_EXECUTE_HANDLER;
}
void __fastcall LedgerAdd(uint8_t* economy,uint8_t* resource,int64_t amount) {
    resourceledger::ChangeProgress progress{};
    if(!g_ledgerFailed.load()) {
        __try { if (resourceledger::Change(g_resourceLedger,economy,resource,amount,false,progress)) return; }
        __except(LedgerFault("add",GetExceptionInformation(),progress,economy,nullptr,nullptr,resource,amount)) {}
    }
    if(!progress.effectsStarted) g_ledgerOriginalAdd(economy,resource,amount);
}
void __fastcall LedgerRemove(uint8_t* economy,uint8_t* resource,int64_t amount) {
    resourceledger::ChangeProgress progress{};
    if(!g_ledgerFailed.load()) {
        __try { if (resourceledger::Change(g_resourceLedger,economy,resource,amount,true,progress)) return; }
        __except(LedgerFault("remove",GetExceptionInformation(),progress,economy,nullptr,nullptr,resource,amount)) {}
    }
    if(!progress.effectsStarted) g_ledgerOriginalRemove(economy,resource,amount);
}
// False, with nothing changed, when the resource's slot is outside the table (left to the game).
bool LedgerConvertCopy(uint8_t* converter,void* table,uint8_t* resource,int64_t amount,resourceledger::ChangeProgress& progress) {
    progress.stage = "copy conversion table";
    int count=resourceledger::Count(table);
    if(!resourceledger::InTable(g_resourceLedger,resource,count)) return false;
    int64_t* destination=resourceledger::Values(table);
    std::vector<int64_t> values(destination,destination+count);
    resourceledger::Table copy(values.data(),count,g_resourceLedger.allocatorVtable);
    progress.stage = "calculate conversion";
    resourceledger::Convert(g_resourceLedger,converter,&copy,resource,amount);
    progress.stage = "write conversion table";
    progress.effectsStarted = true;
    memcpy(destination,values.data(),size_t(count)*sizeof(int64_t));
    return true;
}
void __fastcall LedgerConvert(uint8_t* converter,void* table,uint8_t* resource,int64_t amount) {
    resourceledger::ChangeProgress progress{};
    if(!g_ledgerFailed.load()) {
        __try { if (LedgerConvertCopy(converter,table,resource,amount,progress)) return; }
        __except(LedgerFault("convert",GetExceptionInformation(),progress,nullptr,converter,table,resource,amount)) {}
    }
    if(!progress.effectsStarted) g_ledgerOriginalConvert(converter,table,resource,amount);
}
void InstallResourceLedger(HMODULE self,const locate::Image& img) {
    auto ini=IniPath(self);
    if(ini.empty() || !GetPrivateProfileIntA("numeric_fixes","stockpile_arithmetic",0,ini.c_str())) {
        Log(LogLevel::Info, "stockpile", "off (stockpile_arithmetic=0)"); return;
    }
    std::string error;
    if(!resourceledger::Locate(img,g_resourceLedger,error)) { Log(LogLevel::Error, "stockpile", "%s; off",error.c_str());return; }
    std::vector<uint8_t> batchJumps[2],batchOriginals[2];
    for(int i=0;i<2;++i){auto& b=g_resourceLedger.batches[i];auto stub=AllocNear(b.at,1024);
        if(!stub || !numericpatch::Prepare(b,stub,1024,batchJumps[i])){Log(LogLevel::Error, "stockpile", "cannot prepare batch hooks; off");return;}
        batchOriginals[i].assign(b.at,b.at+b.stolen);}
    uint8_t* sites[]={g_resourceLedger.add,g_resourceLedger.remove,g_resourceLedger.convert};
    void* hooks[]={reinterpret_cast<void*>(&LedgerAdd),reinterpret_cast<void*>(&LedgerRemove),reinterpret_cast<void*>(&LedgerConvert)};
    uint8_t* originals[3]{};
    for(int i=0;i<3;++i) {
        originals[i]=InstallJumpHook(sites[i],resourceledger::kStolen,hooks[i]);
        if(!originals[i]) {
            g_ledgerFailed.store(true);
            for(int j=0;j<i;++j) WriteCode(sites[j],originals[j],resourceledger::kStolen);
            Log(LogLevel::Error, "stockpile", "hook installation failed; off");return;
        }
        if(i==0) g_ledgerOriginalAdd=reinterpret_cast<resourceledger::ChangeFn>(originals[i]);
        if(i==1) g_ledgerOriginalRemove=reinterpret_cast<resourceledger::ChangeFn>(originals[i]);
        if(i==2) g_ledgerOriginalConvert=reinterpret_cast<resourceledger::ConvertFn>(originals[i]);
    }
    for(int i=0;i<2;++i)if(!WriteCode(g_resourceLedger.batches[i].at,batchJumps[i].data(),batchJumps[i].size())){
        for(int j=0;j<i;++j)WriteCode(g_resourceLedger.batches[j].at,batchOriginals[j].data(),batchOriginals[j].size());
        for(int j=0;j<3;++j)WriteCode(sites[j],originals[j],resourceledger::kStolen);
        Log(LogLevel::Error, "stockpile", "batch installation failed; off");return;}
    Log(LogLevel::Info, "stockpile", "checked single/batch add/remove/conversion; native capacity and situation handling retained");
}

numericdisplay::Locations g_numericDisplay;
numericdisplay::FormatFn g_originalMilitaryFormat=nullptr;
std::atomic<bool> g_numericDisplayFailed{false};
void* __fastcall MilitaryFormat64(void* out,int64_t value) {
    if(!g_numericDisplayFailed.load()) {
        __try { return numericdisplay::Format(g_numericDisplay,g_originalMilitaryFormat,out,value); }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            if(!g_numericDisplayFailed.exchange(true)) Log(LogLevel::Error, "military_power_display", "memory fault; native formatter from next call");
        }
    }
    return g_originalMilitaryFormat(out,value);
}
void InstallNumericDisplay(HMODULE self,const locate::Image& img) {
    auto ini=IniPath(self);
    if(ini.empty() || !GetPrivateProfileIntA("numeric_fixes","military_power_display",0,ini.c_str())) {
        Log(LogLevel::Info, "military_power_display", "off (military_power_display=0)");return;
    }
    std::string error;
    if(!numericdisplay::Locate(img,g_numericDisplay,error)) {Log(LogLevel::Error, "military_power_display", "%s; off",error.c_str());return;}
    std::vector<uint8_t> jumps[8],originals[8];
    for(size_t i=0;i<8;++i) {
        auto& b=g_numericDisplay.rounds[i];
        auto stub=AllocNear(b.at,1024);
        if(!stub || !numericpatch::Prepare(b,stub,1024,jumps[i])) {Log(LogLevel::Error, "military_power_display", "cannot prepare rounding stubs; off");return;}
        originals[i].assign(b.at,b.at+b.stolen);
    }
    size_t installed=0;
    for(;installed<8;++installed) if(!WriteCode(g_numericDisplay.rounds[installed].at,jumps[installed].data(),jumps[installed].size())) break;
    uint8_t* original=installed==8 ? InstallJumpHook(g_numericDisplay.format,15,reinterpret_cast<void*>(&MilitaryFormat64)) : nullptr;
    if(!original) {
        for(size_t i=0;i<installed;++i) WriteCode(g_numericDisplay.rounds[i].at,originals[i].data(),originals[i].size());
        Log(LogLevel::Error, "military_power_display", "installation failed; off");return;
    }
    g_originalMilitaryFormat=reinterpret_cast<numericdisplay::FormatFn>(original);
    Log(LogLevel::Info, "military_power_display", "64-bit SI localization; six overflow-safe caller roundings; boss/color wrappers retained");
}

diplomacyoverflow::Locations g_diplomacyOverflow;
bool g_diplomacyArithmeticInstalled=false;
// The parts of diplomatic_weight_arithmetic installed after the base weights (display, votes,
// federation sums, scores, population weight) go in only with them, never as a partial feature.
bool DiplomacyPartWanted(HMODULE self,const char* component){
    auto ini=IniPath(self);
    if(ini.empty() || !GetPrivateProfileIntA("numeric_fixes","diplomatic_weight_arithmetic",0,ini.c_str()))return false;
    if(g_diplomacyArithmeticInstalled)return true;
    Log(LogLevel::Error, component, "off: the base diplomatic weight arithmetic did not install");
    return false;
}
void InstallDiplomacyArithmetic(HMODULE self,const locate::Image& img) {
    auto ini=IniPath(self);
    if(ini.empty() || !GetPrivateProfileIntA("numeric_fixes","diplomatic_weight_arithmetic",0,ini.c_str())) {
        Log(LogLevel::Info, "diplomatic_arithmetic", "off (diplomatic_weight_arithmetic=0)");return;
    }
    std::string error;
    if(!diplomacyoverflow::Locate(img,g_diplomacyOverflow,error)) {Log(LogLevel::Error, "diplomatic_arithmetic", "%s; off",error.c_str());return;}
    constexpr size_t count=diplomacyoverflow::kBlockCount;
    std::vector<uint8_t> jumps[count],originals[count];
    for(size_t i=0;i<count;++i) {
        auto& b=g_diplomacyOverflow.blocks[i];auto stub=AllocNear(b.at,1024);
        if(!stub || !numericpatch::Prepare(b,stub,1024,jumps[i])) {Log(LogLevel::Error, "diplomatic_arithmetic", "cannot prepare stubs; off");return;}
        originals[i].assign(b.at,b.at+b.stolen);
    }
    // Conversion is part of this feature: otherwise a wrapped input reaches the new products.
    if(!InstallFixedPointConversion(self,img,true)) {Log(LogLevel::Error, "diplomatic_arithmetic", "conversion prerequisite failed; off");return;}
    size_t installed=0;
    for(;installed<count;++installed) if(!WriteCode(g_diplomacyOverflow.blocks[installed].at,jumps[installed].data(),jumps[installed].size()))break;
    if(installed!=count) {
        for(size_t i=0;i<installed;++i)WriteCode(g_diplomacyOverflow.blocks[i].at,originals[i].data(),originals[i].size());
        Log(LogLevel::Error, "diplomatic_arithmetic", "installation failed; off (fixed-point conversion remains enabled)");return;
    }
    g_diplomacyArithmeticInstalled=true;
    Log(LogLevel::Info, "diplomatic_arithmetic", "19 wide products; exact component/modifier totals; economic conversion and borrowing boundaries");
}

fleetstats::Locations g_fleetStats;
fleetstats::HullFn g_originalHull[2]{};
fleetstats::PointsFn g_originalPoints[2]{};
std::atomic<bool> g_fleetStatsFailed{false};
void FleetStatsFault(){if(!g_fleetStatsFailed.exchange(true))Log(LogLevel::Error, "fleet_stats", "invalid state or memory fault; off from next call");}
int64_t* FleetHullValue(uint8_t* fleet,int64_t* out,bool maximum){
    if(!g_fleetStatsFailed.load()){
        __try{int64_t a,b;if(fleetstats::Hull(g_fleetStats,fleet,a,b)){*out=maximum?b:a;return out;}}
        __except(EXCEPTION_EXECUTE_HANDLER){}
        FleetStatsFault();
    }
    return g_originalHull[maximum?1:0](fleet,out);
}
int64_t* __fastcall FleetHull(uint8_t* fleet,int64_t* out){return FleetHullValue(fleet,out,false);}
int64_t* __fastcall FleetMaxHull(uint8_t* fleet,int64_t* out){return FleetHullValue(fleet,out,true);}
void FleetPointsValue(uint8_t* fleet,int64_t* current,int64_t* maximum,int type){
    bool writing=false;
    if(!g_fleetStatsFailed.load()){
        __try{int64_t a=*current,b=*maximum;
            if(fleetstats::Points(g_fleetStats,fleet,type,a,b,current==maximum)){
                writing=true;*current=a;*maximum=b;return;
            }
        }__except(EXCEPTION_EXECUTE_HANDLER){}
        FleetStatsFault();
    }
    if(!writing)g_originalPoints[type](fleet,current,maximum);
}
void __fastcall FleetArmor(uint8_t* fleet,int64_t* current,int64_t* maximum){FleetPointsValue(fleet,current,maximum,0);}
void __fastcall FleetShield(uint8_t* fleet,int64_t* current,int64_t* maximum){FleetPointsValue(fleet,current,maximum,1);}
void InstallFleetStats(HMODULE self,const locate::Image& img){
    auto ini=IniPath(self);
    if(ini.empty() || !GetPrivateProfileIntA("numeric_fixes","fleet_stat_arithmetic",0,ini.c_str())){Log(LogLevel::Info, "fleet_stats", "off (fleet_stat_arithmetic=0)");return;}
    std::string error;if(!fleetstats::Locate(img,g_fleetStats,error)){Log(LogLevel::Error, "fleet_stats", "%s; off",error.c_str());return;}
    void* hooks[]={reinterpret_cast<void*>(&FleetHull),reinterpret_cast<void*>(&FleetMaxHull),reinterpret_cast<void*>(&FleetArmor),reinterpret_cast<void*>(&FleetShield)};
    uint8_t* original[4]{};
    for(int i=0;i<4;++i){
        original[i]=InstallJumpHook(g_fleetStats.functions[i],fleetstats::kStolen[i],hooks[i]);
        if(!original[i]){
            g_fleetStatsFailed=true;
            for(int j=0;j<i;++j)WriteCode(g_fleetStats.functions[j],original[j],fleetstats::kStolen[j]);
            Log(LogLevel::Error, "fleet_stats", "installation failed; off");return;
        }
        if(i<2)g_originalHull[i]=reinterpret_cast<fleetstats::HullFn>(original[i]);else g_originalPoints[i-2]=reinterpret_cast<fleetstats::PointsFn>(original[i]);
    }
    Log(LogLevel::Info, "fleet_stats", "wide hull/armor/shield totals; common scaling preserves extreme current/maximum ratios");
}

diploscores::Locations g_diploScores;
shipformula::Locations g_shipFormula;
shipstats::Locations g_shipStats;
componentmath::Locations g_componentMath;
designcache::Locations g_designCache;
std::atomic<bool> g_designCacheFailed{false};
void DisableDesignCache(){
    for(auto& block:g_designCache.blocks)block.failed=true;
    if(!g_designCacheFailed.exchange(true))Log(LogLevel::Error, "design_cache", "calculation fault; off from next call");
}
using DesignCalcFn=void(__fastcall*)(uint8_t*,uint8_t*);
using DesignAddFn=void(__fastcall*)(uint8_t*,int64_t*,uint32_t,void*,int);
using DesignComponentFn=void(__fastcall*)(uint8_t*,uint8_t*,int);
using DesignRecalcFn=void(__fastcall*)(uint8_t*);
using DesignModifierFn=void(__fastcall*)(uint8_t*,uint8_t*,uint32_t,void*,int);
bool CalculateDesignModifiers(uint8_t* base,uint8_t* mods,uint32_t kind,void* name,int count,bool& writing){
    try {designcache::AddModifiers(g_designCache,base,mods,kind,count,[&](uint8_t* stat,int64_t value,int mode){
        writing=true;reinterpret_cast<DesignAddFn>(g_designCache.originals[1])(stat,&value,kind,name,mode);});return true;}catch(...){return false;}
}
void __fastcall DesignAddModifiers(uint8_t* base,uint8_t* mods,uint32_t kind,void* name,int count){
    bool writing=false;
    if(!g_designCacheFailed.load()){
        __try{if(CalculateDesignModifiers(base,mods,kind,name,count,writing))return;}__except(EXCEPTION_EXECUTE_HANDLER){}
        DisableDesignCache();
    }
    if(!writing)reinterpret_cast<DesignModifierFn>(g_designCache.originals[5])(base,mods,kind,name,count);
}
void CalculateDesignStats(uint8_t* stats,uint8_t* record,bool& started){
    designcache::Pass pass(stats,record);designcache::Scope scope(pass);started=true;
    reinterpret_cast<DesignCalcFn>(g_designCache.originals[0])(stats,record);
}
bool TryCalculateDesignStats(uint8_t* stats,uint8_t* record,bool& started){try{CalculateDesignStats(stats,record,started);return true;}catch(...){return false;}}
void __fastcall DesignCalcStats(uint8_t* stats,uint8_t* record){
    bool started=false;
    auto previous=designcache::current;auto previousComponent=designcache::component;auto previousSpecial=designcache::special;
    if(!g_designCacheFailed.load()){
        __try {if(TryCalculateDesignStats(stats,record,started))return;}
        __except(EXCEPTION_EXECUTE_HANDLER){}
        designcache::current=previous;designcache::component=previousComponent;designcache::special=previousSpecial;
        DisableDesignCache();
    }
    if(!started)reinterpret_cast<DesignCalcFn>(g_designCache.originals[0])(stats,record);
}
bool CaptureDesignValue(uint8_t* stat,int64_t value,int mode){try{designcache::Add(g_designCache,stat,value,mode);return true;}catch(...){return false;}}
void __fastcall DesignAddValue(uint8_t* stat,int64_t* amount,uint32_t kind,void* name,int mode){
    if(!g_designCacheFailed.load()){
        bool ok=false;__try{ok=CaptureDesignValue(stat,*amount,mode);}__except(EXCEPTION_EXECUTE_HANDLER){}
        if(!ok)DisableDesignCache();
    }
    reinterpret_cast<DesignAddFn>(g_designCache.originals[1])(stat,amount,kind,name,mode);
}
void __fastcall DesignAddComponent(uint8_t* stats,uint8_t* component,int count){
    // A single-use override replaces the first damage contribution with its exact
    // component result. Later flat-damage modifiers remain separate contributions.
    designcache::Component value{(component[0x1e1]&0xfd)==0?stats+0xc0:nullptr,component};designcache::ComponentScope scope(value);
    reinterpret_cast<DesignComponentFn>(g_designCache.originals[2])(stats,component,count);
}
void CalculateDesignSpecial(uint8_t* design,bool& started){
    designcache::SpecialPass pass{design};designcache::SpecialScope scope(pass);started=true;
    reinterpret_cast<DesignRecalcFn>(g_designCache.originals[3])(design);
    if(!g_designCacheFailed.load())designcache::PublishSpecial(pass);
}
bool TryCalculateDesignSpecial(uint8_t* design,bool& started){try{CalculateDesignSpecial(design,started);return true;}catch(...){return false;}}
void __fastcall DesignRecalc(uint8_t* design){
    bool started=false;
    auto previous=designcache::current;auto previousComponent=designcache::component;auto previousSpecial=designcache::special;
    if(!g_designCacheFailed.load()){
        __try {if(TryCalculateDesignSpecial(design,started))return;}
        __except(EXCEPTION_EXECUTE_HANDLER){}
        designcache::current=previous;designcache::component=previousComponent;designcache::special=previousSpecial;
        DisableDesignCache();
    }
    if(!started)reinterpret_cast<DesignRecalcFn>(g_designCache.originals[3])(design);
}
bool CalculateGroupedSources(uint8_t* stat,uint8_t* output,bool& started){
    try{designcache::GroupPass pass{output};designcache::GroupScope scope(pass);started=true;
        reinterpret_cast<DesignCalcFn>(g_designCache.originals[6])(stat,output);return true;
    }catch(...){return false;}
}
void __fastcall DesignGroupedSources(uint8_t* stat,uint8_t* output){
    bool started=false;auto previous=designcache::group;
    if(!g_designCacheFailed.load()){
        __try{if(CalculateGroupedSources(stat,output,started))return;}__except(EXCEPTION_EXECUTE_HANDLER){}
        designcache::group=previous;DisableDesignCache();
    }
    if(!started)reinterpret_cast<DesignCalcFn>(g_designCache.originals[6])(stat,output);
}
void InstallDesignCache(HMODULE self,const locate::Image& img){
    auto ini=IniPath(self);if(ini.empty())return;
    if(!GetPrivateProfileIntA("numeric_fixes","fleet_stat_arithmetic",0,ini.c_str()) &&
       !GetPrivateProfileIntA("numeric_fixes","fleet_power_arithmetic",0,ini.c_str()))return;
    std::string error;if(!designcache::Locate(img,g_designCache,error)){Log(LogLevel::Error, "design_cache", "%s; off",error.c_str());return;}
    std::vector<uint8_t> jumps[6],originals[6];
    for(int i=0;i<6;++i){auto& b=g_designCache.blocks[i];auto stub=AllocNear(b.at,1024);
        if(!stub || !numericpatch::Prepare(b,stub,1024,jumps[i])){Log(LogLevel::Error, "design_cache", "cannot prepare stubs; off");return;}
        originals[i].assign(b.at,b.at+b.stolen);}
    void* hooks[]={reinterpret_cast<void*>(&DesignCalcStats),reinterpret_cast<void*>(&DesignAddValue),reinterpret_cast<void*>(&DesignAddComponent),reinterpret_cast<void*>(&DesignRecalc),reinterpret_cast<void*>(&DesignAddModifiers),reinterpret_cast<void*>(&DesignGroupedSources)};
    const int entries[]={0,1,2,3,5,6};
    for(int i=0;i<6;++i){int entry=entries[i];auto original=InstallJumpHook(g_designCache.functions[entry],designcache::kStolen[entry],hooks[i]);
        if(!original){DisableDesignCache();for(int j=0;j<i;++j){int e=entries[j];WriteCode(g_designCache.functions[e],g_designCache.originals[e],designcache::kStolen[e]);}return;}
        g_designCache.originals[entry]=original;}
    for(int i=0;i<6;++i)if(!WriteCode(g_designCache.blocks[i].at,jumps[i].data(),jumps[i].size())){
        DisableDesignCache();for(int j=0;j<i;++j)WriteCode(g_designCache.blocks[j].at,originals[j].data(),originals[j].size());
        for(int j:entries)WriteCode(g_designCache.functions[j],g_designCache.originals[j],designcache::kStolen[j]);return;}
    Log(LogLevel::Info, "design_cache", "wide rebuild-scoped stat terms, component totals and excess-power ratios");
}
shipdamage::Locations g_shipDamage;
std::atomic<bool> g_shipDamageFailed{false};
int ShipNumericFault(const char* feature,int kind,EXCEPTION_POINTERS* fault,void* subject,void* tooltip){
    const auto& record=*fault->ExceptionRecord;
    Log(LogLevel::Error, feature, "fault detail: kind=%d code=%08lX instruction=%p subject=%p tooltip=%p operation=%llu address=%p",
        kind,record.ExceptionCode,record.ExceptionAddress,subject,tooltip,
        record.NumberParameters>0?static_cast<unsigned long long>(record.ExceptionInformation[0]):0,
        record.NumberParameters>1?reinterpret_cast<void*>(record.ExceptionInformation[1]):nullptr);
    return EXCEPTION_EXECUTE_HANDLER;
}
bool CalculateShipDamage(int kind,int64_t* out,uint8_t* design,uint8_t* modifiers,uint8_t* ship,bool weighted,void* tooltip,int id,bool& writing){
    try {
        auto value=(kind==0?shipdamage::Damage(g_shipDamage,design,modifiers,ship,weighted):
            kind==1?shipdamage::Adjustments(g_shipDamage,design,modifiers,weighted):shipdamage::Penalty(g_shipDamage,ship,id)).Clamp();
        // The result first: a fault in the native tooltip formatter must not leave *out unwritten.
        writing=true;*out=value;
        if(tooltip){int64_t ignored;
            if(kind==0)reinterpret_cast<shipdamage::MainFn>(g_shipDamage.originals[0])(&ignored,design,modifiers,ship,weighted,tooltip);
            else reinterpret_cast<shipdamage::AdjustFn>(g_shipDamage.originals[1])(design,&ignored,modifiers,weighted,tooltip);}
        return true;
    }catch(const std::exception& e){Log(LogLevel::Error, "ship_damage", "calculation failed: kind=%d %s",kind,e.what());return false;}
    catch(...){return false;}
}
int64_t* ShipDamageValue(int kind,int64_t* out,uint8_t* design,uint8_t* modifiers,uint8_t* ship,bool weighted,void* tooltip,int id){
    bool writing=false;
    if(!g_shipDamageFailed.load()){
        __try {if(CalculateShipDamage(kind,out,design,modifiers,ship,weighted,tooltip,id,writing))return out;}
        __except(ShipNumericFault("ship_damage",kind,GetExceptionInformation(),design?design:ship,tooltip)){}
        if(!g_shipDamageFailed.exchange(true))Log(LogLevel::Error, "ship_damage", "memory/formatting fault; off from next call");
    }
    if(writing)return out;
    if(kind==0)return reinterpret_cast<shipdamage::MainFn>(g_shipDamage.originals[0])(out,design,modifiers,ship,weighted,tooltip);
    if(kind==1)return reinterpret_cast<shipdamage::AdjustFn>(g_shipDamage.originals[1])(design,out,modifiers,weighted,tooltip);
    return reinterpret_cast<shipdamage::PenaltyFn>(g_shipDamage.originals[2])(out,ship,id);
}
int64_t* __fastcall ShipDamage(int64_t* o,uint8_t* d,uint8_t* m,uint8_t* s,bool w,void* t){return ShipDamageValue(0,o,d,m,s,w,t,0);}
int64_t* __fastcall ShipComponentAdjustments(uint8_t* d,int64_t* o,uint8_t* m,bool w,void* t){return ShipDamageValue(1,o,d,m,nullptr,w,t,0);}
int64_t* __fastcall ShipDamagePenalty(int64_t* o,uint8_t* s,int id){return ShipDamageValue(2,o,nullptr,nullptr,s,false,nullptr,id);}
void InstallShipDamage(HMODULE self,const locate::Image& img){
    auto ini=IniPath(self);if(ini.empty())return;
    if(!GetPrivateProfileIntA("numeric_fixes","fleet_stat_arithmetic",0,ini.c_str()) &&
       !GetPrivateProfileIntA("numeric_fixes","fleet_power_arithmetic",0,ini.c_str()))return;
    std::string error;if(!shipdamage::Locate(img,g_shipDamage,error)){Log(LogLevel::Error, "ship_damage", "%s; off",error.c_str());return;}
    void* hooks[]={reinterpret_cast<void*>(&ShipDamage),reinterpret_cast<void*>(&ShipComponentAdjustments),reinterpret_cast<void*>(&ShipDamagePenalty)};
    for(int i=0;i<3;++i){auto original=InstallJumpHook(g_shipDamage.functions[i],shipdamage::kStolen[i],hooks[i]);
        if(!original){g_shipDamageFailed=true;
            for(int j=0;j<i;++j)WriteCode(g_shipDamage.functions[j],g_shipDamage.originals[j],shipdamage::kStolen[j]);
            Log(LogLevel::Error, "ship_damage", "cannot install; off");return;}
        g_shipDamage.originals[i]=original;}
    Log(LogLevel::Info, "ship_damage", "exact modifier chains, component adjustments and damaged-hull penalties");
}
void InstallComponentDamage(HMODULE self,const locate::Image& img){
    auto ini=IniPath(self);if(ini.empty())return;
    if(!GetPrivateProfileIntA("numeric_fixes","fleet_stat_arithmetic",0,ini.c_str()) &&
       !GetPrivateProfileIntA("numeric_fixes","fleet_power_arithmetic",0,ini.c_str()))return;
    std::string error;if(!componentmath::Locate(img,g_componentMath,error)){Log(LogLevel::Error, "component_damage", "%s; off",error.c_str());return;}
    std::vector<uint8_t> jumps[2],originals[2];
    for(int i=0;i<2;++i){auto& b=g_componentMath.blocks[i];auto stub=AllocNear(b.at,1024);
        if(!stub || !numericpatch::Prepare(b,stub,1024,jumps[i])){Log(LogLevel::Error, "component_damage", "cannot prepare stubs; off");return;}
        originals[i].assign(b.at,b.at+b.stolen);}
    for(int i=0;i<2;++i)if(!WriteCode(g_componentMath.blocks[i].at,jumps[i].data(),jumps[i].size())){
        for(int j=0;j<i;++j)WriteCode(g_componentMath.blocks[j].at,originals[j].data(),originals[j].size());
        Log(LogLevel::Error, "component_damage", "cannot install; off");return;}
    Log(LogLevel::Info, "component_damage", "exact weapon/strike-craft damage, range means, counts and military weights");
}
std::atomic<bool> g_shipStatsFailed{false};
bool CalculateMaximumShipStat(int kind,int64_t* out,uint8_t* design,uint8_t* modifiers,uint8_t* fleet,void* tooltip,bool& writing){
    try {
        int64_t value=shipstats::Maximum(g_shipStats,kind,design,modifiers,fleet).Clamp();
        // Native formatting lists the unchanged base and individual modifiers. Retain its
        // localization/ownership path for the corrected final result, which is written first
        // so that a fault in the native formatter cannot leave *out unwritten.
        writing=true;*out=value;
        if(tooltip){int64_t ignored;g_shipStats.originals[kind](&ignored,design,modifiers,fleet,tooltip);}
        return true;
    }catch(const std::exception& e){Log(LogLevel::Error, "ship_stats", "calculation failed: kind=%d %s",kind,e.what());return false;}
    catch(...){return false;}
}
int64_t* MaximumShipStat(int kind,int64_t* out,uint8_t* design,uint8_t* modifiers,uint8_t* fleet,void* tooltip){
    bool writing=false;
    if(!g_shipStatsFailed.load()){
        __try {if(CalculateMaximumShipStat(kind,out,design,modifiers,fleet,tooltip,writing))return out;}
        __except(ShipNumericFault("ship_stats",kind,GetExceptionInformation(),design,tooltip)){}
        if(!g_shipStatsFailed.exchange(true))Log(LogLevel::Error, "ship_stats", "memory/formatting fault; off from next call");
    }
    if(!writing)return g_shipStats.originals[kind](out,design,modifiers,fleet,tooltip);
    return out;
}
int64_t* __fastcall MaximumShipHull(int64_t* o,uint8_t* d,uint8_t* m,uint8_t* f,void* t){return MaximumShipStat(0,o,d,m,f,t);}
int64_t* __fastcall MaximumShipArmor(int64_t* o,uint8_t* d,uint8_t* m,uint8_t* f,void* t){return MaximumShipStat(1,o,d,m,f,t);}
int64_t* __fastcall MaximumShipShield(int64_t* o,uint8_t* d,uint8_t* m,uint8_t* f,void* t){return MaximumShipStat(2,o,d,m,f,t);}
void InstallShipStats(HMODULE self,const locate::Image& img){
    auto ini=IniPath(self);
    if(ini.empty() || !GetPrivateProfileIntA("numeric_fixes","fleet_stat_arithmetic",0,ini.c_str()))return;
    std::string error;if(!shipstats::Locate(img,g_shipStats,error)){Log(LogLevel::Error, "ship_stats", "%s; off",error.c_str());return;}
    shipstats::MaximumFn hooks[]={MaximumShipHull,MaximumShipArmor,MaximumShipShield};
    for(int i=0;i<3;++i){auto original=InstallJumpHook(g_shipStats.functions[i],shipstats::kStolen[i],reinterpret_cast<void*>(hooks[i]));
        if(!original){g_shipStatsFailed=true;
            for(int j=0;j<i;++j)WriteCode(g_shipStats.functions[j],reinterpret_cast<void*>(g_shipStats.originals[j]),shipstats::kStolen[j]);
            Log(LogLevel::Error, "ship_stats", "cannot install; off");return;}
        g_shipStats.originals[i]=reinterpret_cast<shipstats::MaximumFn>(original);}
    Log(LogLevel::Info, "ship_stats", "exact hull/armor/shield modifiers, bioship growth and final products");
}
void InstallShipFormula(HMODULE self,const locate::Image& img){
    auto ini=IniPath(self);
    if(ini.empty() || !GetPrivateProfileIntA("numeric_fixes","fleet_power_arithmetic",0,ini.c_str()))return;
    std::string error;if(!shipformula::Locate(img,g_shipFormula,error)){Log(LogLevel::Error, "ship_power", "%s; off",error.c_str());return;}
    auto& block=g_shipFormula.block;auto stub=AllocNear(block.at,1024);std::vector<uint8_t> jump;
    if(!stub || !numericpatch::Prepare(block,stub,1024,jump) || !WriteCode(block.at,jump.data(),jump.size())){
        Log(LogLevel::Error, "ship_power", "cannot install; off");return;}
    Log(LogLevel::Info, "ship_power", "wide Q7 arithmetic; integer interval exponent fallback on native overflow");
}
incomelifetime::Locations g_incomeProduction;
std::atomic<bool> g_incomeProductionFailed{false};
using IncomePassFn=void(__fastcall*)();
using IncomeDrainFn=void(__fastcall*)(uint8_t*,uint8_t*);
using IncomeReadFn=int64_t*(__fastcall*)(uint8_t*,int64_t*,uint8_t*,void*);
template<int Kind>void __fastcall IncomeProductionPass(){
    auto& l=g_incomeProduction;
    if(!l.depth && g_incomeProductionFailed.load()){
        for(auto& b:l.boundaries)b.failed=true;for(auto& b:l.producers.blocks)b.failed=true;
    }
    ++l.depth;
    reinterpret_cast<IncomePassFn>(l.originals[Kind])();
    if(--l.depth==0 && l.pass.Active() && !l.pass.End())
        Log(LogLevel::Error, "income_production", "joined pass retained an unrepresentable undrained value");
}
bool DrainIncome(uint8_t* source,uint8_t* destination,bool categorized,bool& writing){
    try{auto plan=incomeatomic::Plan(g_incomeProduction.tables,source,destination,categorized);
        incomeatomic::Drain(g_incomeProduction.pass,plan,writing);return true;}catch(...){return false;}
}
template<int Kind>void __fastcall IncomeProductionDrain(uint8_t* source,uint8_t* destination){
    if(g_incomeProductionFailed.load() && !g_incomeProduction.pass.Active()){
        reinterpret_cast<IncomeDrainFn>(g_incomeProduction.originals[2+Kind])(source,destination);return;}
    bool writing=false;
    __try{if(DrainIncome(source,destination,Kind!=0,writing))return;}__except(EXCEPTION_EXECUTE_HANDLER){}
    g_incomeProductionFailed=true;
    if(!writing)reinterpret_cast<IncomeDrainFn>(g_incomeProduction.originals[2+Kind])(source,destination);
}
bool ReadIncome(uint8_t* source,int64_t* output,uint8_t* resource,void* tooltip,bool& writing){
    try{auto value=(tooltip?incomeatomic::FormattedResource(g_incomeProduction.tables,g_incomeProduction.pass,source,resource,static_cast<uint8_t*>(tooltip)):
            incomeatomic::ReadResource(g_incomeProduction.pass,source,resource)).Clamp();
        // The result first: a fault in the native tooltip formatter must not leave *output unwritten.
        writing=true;*output=value;
        if(tooltip){int64_t ignored;reinterpret_cast<IncomeReadFn>(g_incomeProduction.originals[4])(source,&ignored,resource,tooltip);}
        return true;}catch(...){return false;}
}
int64_t* __fastcall IncomeProductionRead(uint8_t* source,int64_t* output,uint8_t* resource,void* tooltip){
    if(g_incomeProductionFailed.load() && !g_incomeProduction.pass.Active())
        return reinterpret_cast<IncomeReadFn>(g_incomeProduction.originals[4])(source,output,resource,tooltip);
    bool writing=false;
    __try{if(ReadIncome(source,output,resource,tooltip,writing))return output;}__except(EXCEPTION_EXECUTE_HANDLER){}
    g_incomeProductionFailed=true;
    if(!writing)return reinterpret_cast<IncomeReadFn>(g_incomeProduction.originals[4])(source,output,resource,tooltip);
    return output;
}
void InstallIncomeProduction(HMODULE self,const locate::Image& image){
    auto ini=IniPath(self);if(ini.empty() || !GetPrivateProfileIntA("numeric_fixes","income_production_arithmetic",0,ini.c_str()))return;
    auto& l=g_incomeProduction;std::string error;
    if(!incomelifetime::Locate(image,l,error)){Log(LogLevel::Error, "income_production", "%s; off",error.c_str());return;}
    std::vector<numericpatch::Block*> blocks;for(auto& b:l.producers.blocks)blocks.push_back(&b);for(auto& b:l.boundaries)blocks.push_back(&b);
    std::vector<std::vector<uint8_t>> jumps(blocks.size()),originals(blocks.size());
    for(size_t i=0;i<blocks.size();++i){auto& b=*blocks[i];auto stub=AllocNear(b.at,1024);
        if(!stub || !numericpatch::Prepare(b,stub,1024,jumps[i])){Log(LogLevel::Error, "income_production", "cannot prepare stubs; off");return;}
        originals[i].assign(b.at,b.at+b.stolen);}
    uint8_t* entries[]={l.tables.roots[0],l.tables.roots[1],l.tables.drain,l.tables.categorizedDrain,l.tables.readResource};
    size_t stolen[]={14,18,14,14,14};
    void* hooks[]={reinterpret_cast<void*>(&IncomeProductionPass<0>),reinterpret_cast<void*>(&IncomeProductionPass<1>),
        reinterpret_cast<void*>(&IncomeProductionDrain<0>),reinterpret_cast<void*>(&IncomeProductionDrain<1>),reinterpret_cast<void*>(&IncomeProductionRead)};
    for(size_t i=0;i<5;++i){l.originals[i]=InstallJumpHook(entries[i],stolen[i],hooks[i]);
        if(!l.originals[i]){for(size_t j=0;j<i;++j)WriteCode(entries[j],l.originals[j],stolen[j]);Log(LogLevel::Error, "income_production", "entry installation failed; off");return;}}
    for(size_t i=0;i<blocks.size();++i)if(!WriteCode(blocks[i]->at,jumps[i].data(),jumps[i].size())){
        for(size_t j=0;j<i;++j)WriteCode(blocks[j]->at,originals[j].data(),originals[j].size());
        for(size_t j=0;j<5;++j)WriteCode(entries[j],l.originals[j],stolen[j]);Log(LogLevel::Error, "income_production", "atomic installation failed; off");return;}
    Log(LogLevel::Info, "income_production", "116 atomic additions, reads, stores, compare/subtract and drains within joined pass lifetimes");
}
diplodisplay::Locations g_diploDisplay;
void InstallDiplomaticDisplay(HMODULE self,const locate::Image& image){
    if(!DiplomacyPartWanted(self,"diplomatic_display"))return;
    std::string error;if(!g_diplomacyOverflow.functions[0] || !g_diplomacyOverflow.functions[1] ||
        !diplodisplay::Locate(image,g_diploDisplay,error,g_diplomacyOverflow.functions)){Log(LogLevel::Error, "diplomatic_display", "%s; off",error.c_str());return;}
    std::vector<uint8_t> jumps[diplodisplay::kCount],originals[diplodisplay::kCount];
    for(size_t i=0;i<diplodisplay::kCount;++i){auto& b=g_diploDisplay.blocks[i];auto stub=AllocNear(b.at,1024);
        if(!stub || !numericpatch::Prepare(b,stub,1024,jumps[i])){Log(LogLevel::Error, "diplomatic_display", "cannot prepare stubs; off");return;}
        originals[i].assign(b.at,b.at+b.stolen);}
    for(size_t i=0;i<diplodisplay::kCount;++i)if(!WriteCode(g_diploDisplay.blocks[i].at,jumps[i].data(),jumps[i].size())){
        for(size_t j=0;j<i;++j)WriteCode(g_diploDisplay.blocks[j].at,originals[j].data(),originals[j].size());
        Log(LogLevel::Error, "diplomatic_display", "installation failed; off");return;}
    Log(LogLevel::Info, "diplomatic_display", "wide localized base, component, modifier, borrowed and federation values");
}
diplovotes::Locations g_diploVotes;
federationsums::Locations g_federationSums;
void InstallFederationSums(HMODULE self,const locate::Image& image){
    if(!DiplomacyPartWanted(self,"federation_sums"))return;
    std::string error;if(!federationsums::Locate(image,g_federationSums,error)){Log(LogLevel::Error, "federation_sums", "%s; off",error.c_str());return;}
    std::vector<uint8_t> jumps[federationsums::kCount],originals[federationsums::kCount];
    for(size_t i=0;i<federationsums::kCount;++i){auto& b=g_federationSums.blocks[i];auto stub=AllocNear(b.at,1024);
        if(!stub || !numericpatch::Prepare(b,stub,1024,jumps[i])){Log(LogLevel::Error, "federation_sums", "cannot prepare stubs; off");return;}
        originals[i].assign(b.at,b.at+b.stolen);}
    for(size_t i=0;i<federationsums::kCount;++i)if(!WriteCode(g_federationSums.blocks[i].at,jumps[i].data(),jumps[i].size())){
        for(size_t j=0;j<i;++j)WriteCode(g_federationSums.blocks[j].at,originals[j].data(),originals[j].size());
        Log(LogLevel::Error, "federation_sums", "installation failed; off");return;}
    Log(LogLevel::Info, "federation_sums", "exact AI acceptance, predictions and law/type views");
}
void InstallDiplomaticVotes(HMODULE self,const locate::Image& img){
    if(!DiplomacyPartWanted(self,"diplomatic_votes"))return;
    std::string error;if(!diplovotes::Locate(img,g_diploVotes,error)){Log(LogLevel::Error, "diplomatic_votes", "%s; off",error.c_str());return;}
    std::vector<uint8_t> jumps[7],originals[7];
    for(size_t i=0;i<7;++i){auto& b=g_diploVotes.blocks[i];auto stub=AllocNear(b.at,1024);
        if(!stub || !numericpatch::Prepare(b,stub,1024,jumps[i])){Log(LogLevel::Error, "diplomatic_votes", "cannot prepare stubs; off");return;}
        originals[i].assign(b.at,b.at+b.stolen);}
    for(size_t i=0;i<7;++i)if(!WriteCode(g_diploVotes.blocks[i].at,jumps[i].data(),jumps[i].size())){
        for(size_t j=0;j<i;++j)WriteCode(g_diploVotes.blocks[j].at,originals[j].data(),originals[j].size());
        Log(LogLevel::Error, "diplomatic_votes", "installation failed; off");return;}
    Log(LogLevel::Info, "diplomatic_votes", "wide resolution/federation votes and coupled community totals");
}
void InstallDiplomaticScores(HMODULE self,const locate::Image& img){
    if(!DiplomacyPartWanted(self,"diplomatic_scores"))return;
    std::string error;if(!diploscores::Locate(img,g_diploScores,error)){Log(LogLevel::Error, "diplomatic_scores", "%s; off",error.c_str());return;}
    std::vector<uint8_t> jumps[diploscores::kBlockCount],originals[diploscores::kBlockCount];
    for(size_t i=0;i<diploscores::kBlockCount;++i){auto& b=g_diploScores.blocks[i];auto stub=AllocNear(b.at,1024);
        if(!stub || !numericpatch::Prepare(b,stub,1024,jumps[i])){Log(LogLevel::Error, "diplomatic_scores", "cannot prepare stubs; off");return;}
        originals[i].assign(b.at,b.at+b.stolen);}
    for(size_t i=0;i<diploscores::kBlockCount;++i)if(!WriteCode(g_diploScores.blocks[i].at,jumps[i].data(),jumps[i].size())){
        for(size_t j=0;j<i;++j)WriteCode(g_diploScores.blocks[j].at,originals[j].data(),originals[j].size());
        Log(LogLevel::Error, "diplomatic_scores", "installation failed; off");return;}
    Log(LogLevel::Info, "diplomatic_scores", "exact technology, economy, job weights and individual/total favor loans");
}

countrytotals::Locations g_countryTotals;
void InstallCountryTotals(HMODULE self,const locate::Image& img){
    auto ini=IniPath(self);if(ini.empty())return;
    bool fleet=GetPrivateProfileIntA("numeric_fixes","fleet_power_arithmetic",0,ini.c_str())!=0;
    bool population=DiplomacyPartWanted(self,"country_totals");
    if(!fleet && !population)return;
    std::string error;if(!countrytotals::Locate(img,g_countryTotals,error)){Log(LogLevel::Error, "country_totals", "%s; off",error.c_str());return;}
    std::vector<uint8_t> jumps[3],originals[3];bool enabled[]={fleet,fleet,population};
    for(int i=0;i<3;++i)if(enabled[i]){auto& b=g_countryTotals.blocks[i];auto stub=AllocNear(b.at,1024);
        if(!stub || !numericpatch::Prepare(b,stub,1024,jumps[i])){Log(LogLevel::Error, "country_totals", "cannot prepare stubs; off");return;}
        originals[i].assign(b.at,b.at+b.stolen);}
    for(int i=0;i<3;++i)if(enabled[i] && !WriteCode(g_countryTotals.blocks[i].at,jumps[i].data(),jumps[i].size())){
        for(int j=0;j<i;++j)if(enabled[j])WriteCode(g_countryTotals.blocks[j].at,originals[j].data(),originals[j].size());
        Log(LogLevel::Error, "country_totals", "installation failed; off");return;}
    Log(LogLevel::Info, "country_totals", "wide fleet totals=%d; wide population weight=%d",fleet,population);
}

forecastoverflow::Locations g_forecastOverflow;
incomeoverflow::Locations g_incomeArithmetic;
incomeoverflow::CalculateFn g_originalIncomeCalculation=nullptr;
std::atomic<bool> g_incomeArithmeticFailed{false};
bool g_incomeArithmeticInstalled=false;
bool CalculateIncomeExact(uint8_t* plan,incomeoverflow::Table* out,void* scope,const incomeoverflow::Table* target,int mode,bool& writing){
    try{incomeoverflow::Calculate(g_incomeArithmetic,plan,out,scope,target,mode,writing);return true;}
    catch(const std::exception& e){Log(LogLevel::Error, "income_target_arithmetic", "%s",e.what());return false;}
}
incomeoverflow::Table* __fastcall IncomeCalculationHook(uint8_t* plan,incomeoverflow::Table* out,void* scope,const incomeoverflow::Table* target,int mode){
    bool writing=false;
    if(!g_incomeArithmeticFailed.load()){
        __try{if(CalculateIncomeExact(plan,out,scope,target,mode,writing))return out;}
        __except(EXCEPTION_EXECUTE_HANDLER){}
        if(!g_incomeArithmeticFailed.exchange(true))Log(LogLevel::Error, "income_target_arithmetic", "invalid state or memory fault; off from next call");
    }
    // Never allocate/write a second result over a partially constructed native table.
    return writing?out:g_originalIncomeCalculation(plan,out,scope,target,mode);
}
void InstallIncomeArithmetic(HMODULE self,const locate::Image& img){
    auto ini=IniPath(self);if(ini.empty() || !GetPrivateProfileIntA("numeric_fixes","income_target_arithmetic",0,ini.c_str()))return;
    std::string error;if(!incomeoverflow::Locate(img,g_incomeArithmetic,error)){Log(LogLevel::Error, "income_target_arithmetic", "%s; off",error.c_str());return;}
    auto original=InstallJumpHook(g_incomeArithmetic.calculate,incomeoverflow::kStolen,reinterpret_cast<void*>(&IncomeCalculationHook));
    if(!original){Log(LogLevel::Error, "income_target_arithmetic", "installation failed; off");return;}
    g_originalIncomeCalculation=reinterpret_cast<incomeoverflow::CalculateFn>(original);g_incomeArithmeticInstalled=true;
    Log(LogLevel::Info, "income_target_arithmetic", "exact recursive totals/counts; unreachable scaling targets stop after one round");
}
void InstallForecastArithmetic(HMODULE self,const locate::Image& img){
    auto ini=IniPath(self);if(ini.empty() || !GetPrivateProfileIntA("numeric_fixes","income_forecast_arithmetic",0,ini.c_str()))return;
    std::string error;if(!forecastoverflow::Locate(img,g_forecastOverflow,error)){Log(LogLevel::Error, "income_forecast", "%s; off",error.c_str());return;}
    std::vector<uint8_t> jumps[3],originals[3];
    for(int i=0;i<3;++i){auto& b=g_forecastOverflow.blocks[i];auto stub=AllocNear(b.at,1024);
        if(!stub || !numericpatch::Prepare(b,stub,1024,jumps[i])){Log(LogLevel::Error, "income_forecast", "cannot prepare stubs; off");return;}
        originals[i].assign(b.at,b.at+b.stolen);}
    for(int i=0;i<3;++i)if(!WriteCode(g_forecastOverflow.blocks[i].at,jumps[i].data(),jumps[i].size())){
        for(int j=0;j<i;++j)WriteCode(g_forecastOverflow.blocks[j].at,originals[j].data(),originals[j].size());
        Log(LogLevel::Error, "income_forecast", "installation failed; off");return;}
    Log(LogLevel::Info, "income_forecast", "wide income + production - upkeep; checked target differences");
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
        Log(LogLevel::Error, "ai_build_plan", "fault while reading a plan; the game's behaviour from now on");
    }
    if (!g_planFailed && array && count > 0) {
        if (spill.size() < static_cast<size_t>(count)) spill.resize(count);
        auto items = *reinterpret_cast<PlanItem**>(array + g_ai.arrayData);
        int moved = DeferBlockedItems(items, count, spill.data());
        if (moved < 0) {
            g_planFailed = true;
            Log(LogLevel::Error, "ai_build_plan", "fault while sorting a plan; the game's behaviour from now on");
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
        Log(LogLevel::Error, "ai_build_plan", "no memory near the game's code; off");
        return;
    }
    uint8_t* tramp = InstallJumpHook(g_ai.planWalk, locate::kPlanWalkStolenBytes, reinterpret_cast<void*>(&PlanWalkHook));
    if (!tramp) {
        Log(LogLevel::Warn, "ai_build_plan", "failed to hook the walk; only the unordered items are kept");
        return;
    }
    g_origPlanWalk = reinterpret_cast<PlanWalkFn>(tramp);
    Log(LogLevel::Info, "ai_build_plan", "on; the plan lasts until the AI's next month, blocked items wait at its end");
}

// ---- 2.16 Items the walk can't order ---------------------------------------------------------
//
// The walk checks each item (colony usable, nothing queued, affordable, ...) and stops at the
// first that fails; whatever is below it waits, for as long as the heaviest item can't be paid.
// The fix points the failed check's jump at the loop's continue, so the walk goes on to the next
// item. A failed item stays in the plan (build_plan_keep keeps it for the next week).

void InstallPlanSkipFix() {
    uint8_t* jump = g_ai.planStopJump;
    int32_t rel;
    if (jump[0] != 0x0F || jump[1] != 0x84 || !Rel32(jump + 6, g_ai.planContinue, &rel) || !WriteCode(jump + 2, &rel, 4)) {
        Log(LogLevel::Error, "ai_build_plan_skip", "failed to patch; off");
        return;
    }
    Log(LogLevel::Info, "ai_build_plan_skip", "on; the weekly walk passes over items it can't order");
}

// ---- 2.15 The survey blacklist ---------------------------------------------------------------
//
// A system a science ship can't survey (hostile fleet seen, no clear path, ...) goes on a list
// that MonthlyUpdate empties only in the month country id % 12. The fix drops that test, so the
// list is emptied every AI month and each listed system is tested again once a month.

void InstallSurveyBlacklistFix() {
    const uint8_t nop2[2] = {0x66, 0x90};
    uint8_t* jump = g_ai.blacklistJump;
    if (jump[0] != 0x75 || jump[1] != 0x04 || !WriteCode(jump, nop2, sizeof(nop2))) {
        Log(LogLevel::Error, "ai_survey_blacklist", "failed to patch; off");
        return;
    }
    Log(LogLevel::Info, "ai_survey_blacklist", "on; emptied every AI month instead of once a year");
}

// ---- 2.17, 2.18 Ships without work -----------------------------------------------------------
//
// How long a fleet has been seen without work, by fleet id. AI updates run on worker threads.

struct IdleSpan {
    int64_t first, last;  // days
};
struct IdleTracker {
    std::mutex lock;
    std::unordered_map<uint32_t, IdleSpan> spans;
    // Notes the fleet as idle today and returns the days it has been. A fleet not seen for more
    // than `gap` days (it had work, or its id belongs to a new fleet), or seen in the future (a
    // load), starts again.
    int64_t Seen(uint32_t id, int64_t day, int64_t gap) {
        std::lock_guard<std::mutex> guard(lock);
        if (spans.size() > 4096) {
            for (auto it = spans.begin(); it != spans.end();) {
                if (day < it->second.last || day - it->second.last > gap) {
                    it = spans.erase(it);
                } else {
                    ++it;
                }
            }
        }
        auto [it, added] = spans.try_emplace(id, IdleSpan{day, day});
        IdleSpan& span = it->second;
        if (day < span.last || day - span.last > gap) span.first = day;
        span.last = day;
        return day - span.first;
    }
    void Forget(uint32_t id) {
        std::lock_guard<std::mutex> guard(lock);
        spans.erase(id);
    }
};

// The fleet's id and today's day number; false on a fault or before a game is loaded.
bool ReadFleetAndDay(uint8_t* fleet, uint32_t* id, int64_t* day) {
    __try {
        uint8_t* state = g_bench.gameState ? *g_bench.gameState : nullptr;
        if (!state || !fleet) return false;
        *day = *reinterpret_cast<int32_t*>(state + g_bench.stateDate) / 24;
        *id = *reinterpret_cast<uint32_t*>(fleet + g_fleet.fleetIdOffset);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 2.17 BuildColonizer builds a colony ship only when no colony ship is idle. A ship that can't
// reach any target stays idle for good and stops every later one, also one from a shipyard that
// could reach a target. With the fix an idle colony fleet counts as idle only for its first N
// days. The game's other limit stays: ships plus queued ships below the number of targets.

IdleTracker g_colonizerIdle;
int g_colonizerIdleDays = 0;
constexpr int64_t kColonizerGap = 45;  // days between sightings that end an idle spell
std::atomic<uint64_t> g_colonizerSeen{0}, g_colonizerPassed{0};

// The ships the idle count gets for this fleet: the game's number, or 0 once idle for long.
uint32_t __fastcall ColonizerIdleShips(uint8_t* fleet) {
    uint32_t ships = *reinterpret_cast<uint32_t*>(fleet + g_ai.colonizerShipCount);
    uint32_t id;
    int64_t day;
    if (!ReadFleetAndDay(fleet, &id, &day)) return ships;
    g_colonizerSeen.fetch_add(1, std::memory_order_relaxed);
    if (g_colonizerIdle.Seen(id, day, kColonizerGap) < g_colonizerIdleDays) return ships;
    g_colonizerPassed.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

void InstallColonizerFix(int days) {
    if (!g_bench.gameState || !g_fleet.fleetIdOffset) {
        Log(LogLevel::Error, "ai_colony_ships", "the game date or the fleet id wasn't found; off");
        return;
    }
    uint8_t* at = g_ai.colonizerIdle;
    Code c;
    c.Put({0x51, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53});  // push rcx, r9, r10, r11
    c.Put({0x48, 0x89, 0xF9});                          // mov rcx,rdi
    c.Put({0x48, 0x83, 0xEC, 0x20});                    // sub rsp,20h
    c.Put({0x48, 0xB8}).Put64(reinterpret_cast<uint64_t>(&ColonizerIdleShips));  // mov rax,fn
    c.Put({0xFF, 0xD0});                                // call rax
    c.Put({0x48, 0x83, 0xC4, 0x20});                    // add rsp,20h
    c.Put({0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x59});  // pop r11, r10, r9, rcx
    c.Put({0x44, 0x03, 0xF8});                          // add r15d,eax
    c.Put({0xE9}).Put32(0);                             // jmp back
    uint8_t* stub = AllocNear(at, c.n);
    uint8_t patch[locate::kColonizerIdleBytes] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90};
    int32_t back, to;
    if (!stub || !Rel32(stub + c.n, at + sizeof(patch), &back) || !Rel32(at + 5, stub, &to)) {
        Log(LogLevel::Error, "ai_colony_ships", "no memory near the game's code; off");
        return;
    }
    memcpy(c.bytes + c.n - 4, &back, 4);
    memcpy(stub, c.bytes, c.n);
    FlushInstructionCache(GetCurrentProcess(), stub, c.n);
    memcpy(patch + 1, &to, 4);
    g_colonizerIdleDays = days;
    if (!WriteCode(at, patch, sizeof(patch))) {
        Log(LogLevel::Error, "ai_colony_ships", "failed to patch; off");
        return;
    }
    Log(LogLevel::Info, "ai_colony_ships", "on; a colony ship idle for %d days no longer stops another being built", days);
}

// 2.18 An idle construction ship asks BuildStation for work: a special project's station, a mining
// or research station, an outpost. Both searches take CONSTRUCTION_SHIP_DISTANCE_PENALTY per jump
// from the ship off a target's score, so far targets fall below zero and the ship stays idle next
// to them. With the fix, a ship BuildStation found nothing for during N days searches with half
// the penalty, and from 2N days with none, until it has work again. Only this ship's search
// changes: the factor is per thread, and the two loads of the define read it.

using BuildStationFn = uint64_t(__fastcall*)(uint8_t* minister, uint8_t* fleet, void* a3, uint64_t a4, void* a5);
BuildStationFn g_origBuildStation = nullptr;
IdleTracker g_constructorIdle;
int g_constructorIdleDays = 0;
constexpr int64_t kConstructorGap = 30;
thread_local int t_penaltyQuarters = 4;
std::atomic<uint64_t> g_stationCalls{0}, g_stationWidened{0}, g_stationWidenedFound{0};

int64_t __fastcall StationPenalty() {
    int64_t penalty = *g_ai.stationPenalty;
    int quarters = t_penaltyQuarters;
    return quarters >= 4 ? penalty : penalty * quarters / 4;
}

uint64_t __fastcall BuildStationHook(uint8_t* minister, uint8_t* fleet, void* a3, uint64_t a4, void* a5) {
    uint32_t id;
    int64_t day;
    if (!ReadFleetAndDay(fleet, &id, &day)) return g_origBuildStation(minister, fleet, a3, a4, a5);
    g_stationCalls.fetch_add(1, std::memory_order_relaxed);
    int64_t idle = g_constructorIdle.Seen(id, day, kConstructorGap);
    int quarters = idle >= 2 * int64_t(g_constructorIdleDays) ? 0 : idle >= g_constructorIdleDays ? 2 : 4;
    t_penaltyQuarters = quarters;
    uint64_t found = g_origBuildStation(minister, fleet, a3, a4, a5);
    t_penaltyQuarters = 4;
    if (quarters < 4) {
        g_stationWidened.fetch_add(1, std::memory_order_relaxed);
        if (found & 0xFF) g_stationWidenedFound.fetch_add(1, std::memory_order_relaxed);
    } else if (found & 0xFF) {
        g_constructorIdle.Forget(id);  // it found work at the game's range
    }
    return found;
}

// Replaces a 7-byte load of the define with a call (and a 2-byte nop). The stub returns the scaled
// penalty in the register the game loaded (`slot`: where the stub's pushes keep that register)
// and leaves everything else as it was.
bool PatchPenaltyLoad(uint8_t* at, uint8_t rex, uint8_t modrm, uint8_t slot) {
    if (at[0] != rex || at[1] != 0x8B || at[2] != modrm) return false;
    Code c;
    c.Put({0x9C, 0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53});  // pushfq; push rax, rcx, rdx, r8-r11
    c.Put({0x48, 0x83, 0xEC, 0x28});                                                  // sub rsp,28h
    c.Put({0x48, 0xB8}).Put64(reinterpret_cast<uint64_t>(&StationPenalty));           // mov rax,fn
    c.Put({0xFF, 0xD0});                                                              // call rax
    c.Put({0x48, 0x83, 0xC4, 0x28});                                                  // add rsp,28h
    c.Put({0x48, 0x89, 0x44, 0x24, slot});                                            // mov [rsp+slot],rax
    c.Put({0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59, 0x58, 0x9D, 0xC3});  // pops; popfq; ret
    uint8_t* stub = PlaceStub(c, at);
    uint8_t patch[locate::kPenaltyLoadBytes] = {0xE8, 0, 0, 0, 0, 0x66, 0x90};
    int32_t rel;
    if (!stub || !Rel32(at + 5, stub, &rel)) return false;
    memcpy(patch + 1, &rel, 4);
    return WriteCode(at, patch, sizeof(patch));
}

void InstallStationRangeFix(int days) {
    if (!g_bench.gameState || !g_fleet.fleetIdOffset) {
        Log(LogLevel::Error, "ai_construction_ships", "the game date or the fleet id wasn't found; off");
        return;
    }
    g_constructorIdleDays = days;
    // The loads first: until BuildStation is hooked they return the game's value.
    if (!PatchPenaltyLoad(g_ai.penaltyLoadOutpost, 0x4C, 0x15, 0x08) ||   // mov r10,[rip+define]
        !PatchPenaltyLoad(g_ai.penaltyLoadStation, 0x4C, 0x05, 0x18)) {   // mov r8,[rip+define]
        Log(LogLevel::Error, "ai_construction_ships", "failed to patch the distance penalty; off");
        return;
    }
    uint8_t* tramp = InstallJumpHook(g_ai.buildStation, locate::kBuildStationStolenBytes,
                                     reinterpret_cast<void*>(&BuildStationHook));
    if (!tramp) {
        Log(LogLevel::Error, "ai_construction_ships", "failed to hook BuildStation; off");
        return;
    }
    g_origBuildStation = reinterpret_cast<BuildStationFn>(tramp);
    Log(LogLevel::Info, "ai_construction_ships", "on; without work for %d days a ship searches with half the distance penalty, "
        "from %d days with none", days, 2 * days);
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
        Log(LogLevel::Error, "ai_opportunist", "no memory for the stub; off");
        return;
    }
    Code jump;
    jump.JumpTo(stub);
    uint8_t patch[locate::kOpportunistStolenBytes];
    memset(patch, 0x90, sizeof(patch));
    memcpy(patch, jump.bytes, jump.n);
    if (!WriteCode(g_ai.opportunist, patch, sizeof(patch))) {
        Log(LogLevel::Error, "ai_opportunist", "failed to patch; off");
        return;
    }
    Log(LogLevel::Info, "ai_opportunist", "on; opportunist AIs count the enemies of a target at war");
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
        Log(LogLevel::Error, "ai_war_distance", "failed to patch; off");
        return;
    }
    g_aiDefinesWanted = true;
    Log(LogLevel::Info, "ai_war_distance", "on; the malus counts the jumps above AI_WAR_DECLARATION_MALUS_DISTANCE");
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
        Log(LogLevel::Error, "ai_truce_divisor", "no memory near the game's code; off");
        return;
    }
    *value = divisor;
    if (!WriteCode(g_ai.truceOperand, &rel, 4)) {
        Log(LogLevel::Error, "ai_truce_divisor", "failed to patch; off");
        return;
    }
    g_aiDefinesWanted = true;
    Log(LogLevel::Info, "ai_truce_divisor", "on; a truce divides war appetite by %d instead of AI_AGGRESSIVENESS_BOXED_IN_MULT",
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
        Log(LogLevel::Error, "ai_expansion_lists", "failed to patch; off");
        return;
    }
    Log(LogLevel::Info, "ai_expansion_lists", "on; rebuilt every month");
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
            Log(LogLevel::Info, "ai_random", "first AI day after start or load: %u AIs continue a saved stream, %u start "
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
        Log(LogLevel::Error, "ai_schedule", "fault; off from now on");
    }
    return g_origAiUpdate(core);
}

void InstallAiSchedule(bool schedule, bool random) {
    if (!g_bench.gameState) {
        Log(LogLevel::Error, "ai_schedule", "the game date wasn't found (see the ticks line); off");
        return;
    }
    g_aiSchedule = schedule;
    g_aiRandomCheck = random;
    uint8_t* tramp = InstallJumpHook(g_ai.aiUpdate, locate::kAiUpdateStolenBytes, reinterpret_cast<void*>(&AiUpdateHook));
    if (!tramp) {
        Log(LogLevel::Error, "ai_schedule", "failed to hook CAICore::Update; off");
        g_aiSchedule = g_aiRandomCheck = false;
        return;
    }
    g_origAiUpdate = reinterpret_cast<AiUpdateFn>(tramp);
    if (schedule) Log(LogLevel::Info, "ai_schedule", "on; weekly, monthly and yearly updates on days where (id + day) %% 7, 30, 360 is 0");
}

void InstallSavedRandom() {
    const uint8_t nop6[6] = {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00};
    for (uint8_t* jump : g_ai.savedRandomJumps) {
        if (jump[0] != 0x0F || jump[1] != 0x84 || !WriteCode(jump, nop6, sizeof(nop6))) {
            Log(LogLevel::Warn, "ai_random", "failed to patch; a load may have restored part of it");
            return;
        }
    }
    Log(LogLevel::Info, "ai_random", "on; a loaded AI keeps its saved random_seed and random_count (-loadrandomseed is %s)",
        *g_ai.useSavedRandom ? "on" : "off");
}

void LogAiFixStats() {
    // The defines are read after stellaris-engine-fixes starts; report them once they're there.
    if (g_aiDefinesWanted && g_ai.boxedInMult && *g_ai.boxedInMult != 0) {
        g_aiDefinesWanted = false;
        if (g_ai.malusDistance && g_ai.malus) {
            Log(LogLevel::Info, "ai_defines", "AI_WAR_DECLARATION_MALUS_DISTANCE %d, malus %.2f per jump; "
                "AI_AGGRESSIVENESS_BOXED_IN_MULT %d", *g_ai.malusDistance, *g_ai.malus / 100000.0, *g_ai.boxedInMult);
        }
    }
    if (g_origPlanWalk) {
        Log(LogLevel::Stats, "ai_build_plan", "walks=%llu blocked_deferred=%llu ordered=%llu kept=%llu faulted=%d",
            g_planWalks.load(), g_planDeferred.load(), g_planOrdered.load(), g_planKept.load(), g_planFailed);
    }
    if (uint64_t seen = g_colonizerSeen.load()) {
        Log(LogLevel::Stats, "ai_colony_ships", "idle_seen=%llu long_idle_skipped=%llu", seen, g_colonizerPassed.load());
    }
    if (uint64_t calls = g_stationCalls.load()) {
        Log(LogLevel::Stats, "ai_construction_ships", "searches=%llu widened=%llu widened_found_work=%llu",
            calls, g_stationWidened.load(), g_stationWidenedFound.load());
    }
    if (uint64_t calls = g_opportunistCalls.load()) {
        Log(LogLevel::Stats, "ai_opportunist", "checks=%llu enemies_counted=%llu faulted=%d",
            calls, g_opportunistAdded.load(), g_opportunistFailed);
    }
}

constexpr uint32_t kNoAiTraceId = UINT32_MAX;
uint32_t g_aiTraceCountry = kNoAiTraceId;
uint32_t g_aiTraceSystem = kNoAiTraceId;
int32_t g_aiTraceSystemFleetArray = 0;
int32_t g_aiTraceAiFlags = 0;

using AiTraceScanFn = void (*)(const uint8_t*, const uint8_t*, bool);
using AiTraceCollectFn = void (*)(const uint8_t*, uint8_t*);
using AiTraceCountryFn = const uint8_t* (*)(const uint8_t*);
using AiTraceArmedConflictFn = bool (*)(const uint8_t*);
using AiTraceWantedFn = int64_t* (*)(const uint8_t*, int64_t*, int, const uint8_t*, void*);
using AiTraceAssignFn = int64_t* (*)(const uint8_t*, int64_t*, const uint8_t*, const uint8_t*, int, bool);
using AiTraceAddFn = int (*)(const uint8_t*, const void*, uint32_t, int, void*, void*, bool);
AiTraceScanFn g_aiTraceScan = nullptr;
AiTraceCollectFn g_aiTraceCollect = nullptr;
AiTraceCountryFn g_aiTraceCountryFor = nullptr;
AiTraceArmedConflictFn g_aiTraceArmedConflict = nullptr;
AiTraceWantedFn g_aiTraceWanted = nullptr;
AiTraceAssignFn g_aiTraceAssign = nullptr;
AiTraceAddFn g_aiTraceAdd = nullptr;

template<typename T>
T AiTraceRead(const uint8_t* object, size_t offset, T fallback) {
    if (!object) return fallback;
    __try {
        return *reinterpret_cast<const T*>(object + offset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return fallback;
    }
}

bool AiTraceMatchingGroup(const uint8_t* group) {
    return AiTraceRead(group, 0x18, kNoAiTraceId) == g_aiTraceSystem &&
           AiTraceRead(group, 0x58, kNoAiTraceId) == g_aiTraceCountry;
}

void AiTraceLogScan(const uint8_t* minister, const uint8_t* system, bool local) {
    __try {
        const auto country = g_aiTraceCountryFor(minister);
        if (!country || *reinterpret_cast<const uint32_t*>(country + 0x20) != g_aiTraceCountry) return;
        const auto aiModule = *reinterpret_cast<const uint8_t* const*>(minister + 8);
        const auto moduleVtable = *reinterpret_cast<const uintptr_t* const*>(aiModule);
        using GetAi = const uint8_t* (*)(const uint8_t*);
        const auto ai = reinterpret_cast<GetAi>(moduleVtable[0x28 / 8])(aiModule);
        const auto aiVtable = *reinterpret_cast<const uintptr_t* const*>(ai);
        using GetStrategies = const uint8_t* (*)(const uint8_t*, int);
        const auto strategies = reinterpret_cast<GetStrategies>(aiVtable[0x20 / 8])(ai, 14);
        const auto entries = *reinterpret_cast<const uint8_t* const* const*>(strategies + 8);
        const int count = *reinterpret_cast<const int*>(strategies + 0x14);
        const auto ids = *reinterpret_cast<const uint32_t* const*>(system + g_aiTraceSystemFleetArray);
        const int fleetCount = *reinterpret_cast<const int*>(system + g_aiTraceSystemFleetArray + 12);
        const bool preparingWar = (*reinterpret_cast<const uint32_t*>(ai + g_aiTraceAiFlags) & 4) != 0;
        Log(LogLevel::Debug, "ai_fleet_trace", "scan local=%d prepare_war=%d armed_conflict_with_allies=%d fleets_present=%d",
            local, preparingWar, g_aiTraceArmedConflict(country), fleetCount);
        for (int i = 0; i < count; ++i) {
            const auto id = *reinterpret_cast<const uint32_t*>(entries[i] + 8);
            for (int j = 0; j < fleetCount; ++j) {
                if (ids[j] != id) continue;
                Log(LogLevel::Debug, "ai_fleet_trace", "hostile target=%u in strategy list", id);
                break;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log(LogLevel::Warn, "ai_fleet_trace", "could not read the hostile strategy");
    }
}

void AiTraceScanHook(const uint8_t* minister, const uint8_t* system, bool local) {
    g_aiTraceScan(minister, system, local);
    if (AiTraceRead(system, 8, kNoAiTraceId) == g_aiTraceSystem) AiTraceLogScan(minister, system, local);
}

void AiTraceCollectHook(const uint8_t* minister, uint8_t* available) {
    g_aiTraceCollect(minister, available);
    if (AiTraceRead(g_aiTraceCountryFor(minister), 0x20, kNoAiTraceId) == g_aiTraceCountry) {
        Log(LogLevel::Debug, "ai_fleet_trace", "collected %d available fleets", AiTraceRead(available, 0x14, -1));
    }
}

int64_t* AiTraceWantedHook(const uint8_t* minister, int64_t* power, int mission,
                          const uint8_t* target, void* explanation) {
    auto result = g_aiTraceWanted(minister, power, mission, target, explanation);
    if (AiTraceRead(target, 0x18, kNoAiTraceId) != g_aiTraceSystem ||
        AiTraceRead(g_aiTraceCountryFor(minister), 0x20, kNoAiTraceId) != g_aiTraceCountry) return result;
    Log(LogLevel::Debug, "ai_fleet_trace", "target=%u mission=%d required_power=%.2f",
        AiTraceRead(target, 0x10, kNoAiTraceId), mission, *power / 32768.0);
    return result;
}

int64_t* AiTraceAssignHook(const uint8_t* minister, int64_t* power, const uint8_t* available,
                          const uint8_t* group, int maxDistance, bool armedConflict) {
    const bool trace = AiTraceMatchingGroup(group);
    if (trace) {
        Log(LogLevel::Debug, "ai_fleet_trace", "assign target=%u mission=%u available_fleets=%d max_distance=%d",
            AiTraceRead(group, 0x10, kNoAiTraceId), AiTraceRead(group, 0x30, kNoAiTraceId),
            AiTraceRead(available, 0x14, -1), maxDistance);
    }
    auto result = g_aiTraceAssign(minister, power, available, group, maxDistance, armedConflict);
    if (trace) {
        Log(LogLevel::Debug, "ai_fleet_trace", "assigned_power=%.2f group_fleets=%d",
            *power / 32768.0, AiTraceRead(group, 0x54, -1));
    }
    return result;
}

int AiTraceAddHook(const uint8_t* group, const void* policy, uint32_t fleet, int maxDistance,
                   void* cache, void* powerCache, bool armedConflict) {
    const bool trace = AiTraceMatchingGroup(group);
    const int result = g_aiTraceAdd(group, policy, fleet, maxDistance, cache, powerCache, armedConflict);
    if (trace) {
        static constexpr const char* reasons[] = {"accepted", "invalid fleet", "invalid target system",
            "failed combat readiness policy", "incompatible fleet", "already in group",
            "outside assignment distance", "route or grouping failed"};
        const char* reason = result >= 0 && result < static_cast<int>(std::size(reasons)) ?
            reasons[result] : "unknown";
        Log(LogLevel::Debug, "ai_fleet_trace", "fleet=%u target=%u add_result=%d (%s)",
            fleet, AiTraceRead(group, 0x10, kNoAiTraceId), result, reason);
    }
    return result;
}

void InstallAiFleetTrace(HMODULE self, const locate::Image& image) {
    const auto ini = IniPath(self);
    if (ini.empty()) return;
    g_aiTraceCountry = GetPrivateProfileIntA("diagnostics", "ai_fleet_country", -1, ini.c_str());
    g_aiTraceSystem = GetPrivateProfileIntA("diagnostics", "ai_fleet_system", -1, ini.c_str());
    if (g_aiTraceCountry == kNoAiTraceId || g_aiTraceSystem == kNoAiTraceId) return;

    aifleettrace::Locations found{};
    if (!aifleettrace::Locate(image, found)) {
        Log(LogLevel::Error, "ai_fleet_trace", "could not locate the military planner");
        return;
    }
    g_aiTraceCountryFor = reinterpret_cast<AiTraceCountryFn>(found.country);
    g_aiTraceArmedConflict = reinterpret_cast<AiTraceArmedConflictFn>(found.armedConflict);
    g_aiTraceSystemFleetArray = found.systemFleetArray;
    g_aiTraceAiFlags = found.aiFlags;
    g_aiTraceScan = reinterpret_cast<AiTraceScanFn>(
        InstallJumpHook(found.scan, aifleettrace::kScanStolen, reinterpret_cast<void*>(&AiTraceScanHook)));
    g_aiTraceCollect = reinterpret_cast<AiTraceCollectFn>(
        InstallJumpHook(found.collect, aifleettrace::kCollectStolen, reinterpret_cast<void*>(&AiTraceCollectHook)));
    g_aiTraceWanted = reinterpret_cast<AiTraceWantedFn>(
        InstallJumpHook(found.wanted, aifleettrace::kWantedStolen, reinterpret_cast<void*>(&AiTraceWantedHook)));
    g_aiTraceAssign = reinterpret_cast<AiTraceAssignFn>(
        InstallJumpHook(found.assign, aifleettrace::kAssignStolen, reinterpret_cast<void*>(&AiTraceAssignHook)));
    g_aiTraceAdd = reinterpret_cast<AiTraceAddFn>(
        InstallJumpHook(found.add, aifleettrace::kAddStolen, reinterpret_cast<void*>(&AiTraceAddHook)));
    Log(LogLevel::Debug, "ai_fleet_trace", "country=%u system=%u hooks scan=%d collect=%d wanted=%d assign=%d add=%d",
        g_aiTraceCountry, g_aiTraceSystem, g_aiTraceScan != nullptr, g_aiTraceCollect != nullptr,
        g_aiTraceWanted != nullptr, g_aiTraceAssign != nullptr, g_aiTraceAdd != nullptr);
}

aiwarguard::Locations g_aiWarGuard;
using WarScoreFn = int64_t* (*)(void*, int64_t*, const uint8_t*, bool*, const uint8_t*, void*);
WarScoreFn g_origWarScore = nullptr;

const uint8_t* WarPlanDefender(const uint8_t* relation) {
    __try {
        const auto id = *reinterpret_cast<const uint32_t*>(relation + g_aiWarGuard.relationCountry);
        return ResolveRef(*g_aiWarGuard.countryDb, id, g_aiWarGuard.countryId);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// PrepareWar and CheckShouldContinuePreparingWar both score the same way. A zero score
// rejects a new target or lets the existing weekly check post its normal cancellation.
int64_t* WarPlanScoreHook(void* minister, int64_t* score, const uint8_t* relation, bool* truce,
                          const uint8_t* attacker, void* warData) {
    auto result = g_origWarScore(minister, score, relation, truce, attacker, warData);
    if (*score <= 0) return result;
    const auto defender = WarPlanDefender(relation);
    if (!defender) return result;

    using CreateAction = void* (*)(uint32_t);
    using InitAction = void (*)(void*, const uint8_t*, const uint8_t*);
    using IsPossible = bool (*)(void*, void*);
    using DeleteAction = void (*)(void*, unsigned);
    auto action = reinterpret_cast<CreateAction>(g_aiWarGuard.createEmptyAction)(aiwarguard::kDeclareWar);
    if (!action) return result;
    reinterpret_cast<InitAction>(g_aiWarGuard.initAction)(action, attacker, defender);
    const bool possible = reinterpret_cast<IsPossible>(g_aiWarGuard.isPossible)(action, nullptr);
    reinterpret_cast<DeleteAction>(g_aiWarGuard.deleteAction)(action, 1);
    if (!possible) *score = 0;
    return result;
}

void InstallAiWarGuard(const locate::Image& img) {
    std::string error;
    if (!aiwarguard::Locate(img, g_aiWarGuard, error)) {
        Log(LogLevel::Error, "ai_war_plan", "%s; off", error.c_str());
        return;
    }

    auto refresh = g_aiWarGuard.refresh;
    const auto code = aiwarguard::RefreshStub(g_aiWarGuard, refresh + aiwarguard::kRefreshStolen);
    auto stub = VirtualAlloc(nullptr, code.size(), MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!stub) {
        Log(LogLevel::Error, "ai_war_plan", "cannot allocate refresh stub; off");
        return;
    }
    memcpy(stub, code.data(), code.size());
    FlushInstructionCache(GetCurrentProcess(), stub, code.size());

    uint8_t original[aiwarguard::kRefreshStolen];
    memcpy(original, refresh, sizeof(original));
    Code patch;
    patch.JumpTo(stub).Put({0x90});
    if (!WriteCode(refresh, patch.bytes, patch.n)) {
        VirtualFree(stub, 0, MEM_RELEASE);
        Log(LogLevel::Error, "ai_war_plan", "cannot install refresh; off");
        return;
    }
    g_origWarScore = reinterpret_cast<WarScoreFn>(
        InstallJumpHook(g_aiWarGuard.score, aiwarguard::kScoreStolen, reinterpret_cast<void*>(&WarPlanScoreHook)));
    if (!g_origWarScore) {
        if (WriteCode(refresh, original, sizeof(original))) VirtualFree(stub, 0, MEM_RELEASE);
        else Log(LogLevel::Error, "ai_war_plan", "could not restore the refresh patch");
        Log(LogLevel::Error, "ai_war_plan", "cannot install war score hook; off");
        return;
    }
    Log(LogLevel::Info, "ai_war_plan", "declaration eligibility checked and cancelled plans request a military refresh");
}

void InstallAiBossThreat(const locate::Image& img) {
    aibossthreat::Locations found;
    std::string error;
    if (!aibossthreat::Locate(img, found, error)) {
        Log(LogLevel::Error, "ai_boss_threat_floor", "%s; off", error.c_str());
        return;
    }
    const auto code = aibossthreat::ThreatStub(found);
    // AllocNear hands out pieces of one shared block: a piece is left unused, never freed.
    auto stub = AllocNear(found.site, code.size());
    int32_t jump;
    if (!stub || !Rel32(found.site + 5, stub, &jump)) {
        Log(LogLevel::Error, "ai_boss_threat_floor", "cannot allocate nearby stub; off");
        return;
    }
    memcpy(stub, code.data(), code.size());
    FlushInstructionCache(GetCurrentProcess(), stub, code.size());
    Code patch;
    patch.Put({0xE9}).Put32(static_cast<uint32_t>(jump)).Put({0x90, 0x90, 0x90});
    if (!WriteCode(found.site, patch.bytes, patch.n)) {
        Log(LogLevel::Error, "ai_boss_threat_floor", "cannot install estimate patch; off");
        return;
    }
    Log(LogLevel::Info, "ai_boss_threat_floor", "hostile bosses count for at least their cached fleet power");
}

void InstallAiFixes(HMODULE self, const locate::Image& img) {
    std::string ini = IniPath(self);
    bool plan = AiIni(ini, "build_plan_keep") != 0;
    bool opportunist = AiIni(ini, "opportunist_wars") != 0;
    bool distance = AiIni(ini, "war_distance_slope") != 0;
    bool warGuard = AiIni(ini, "war_plan_check") != 0;
    bool bossThreat = AiIni(ini, "boss_threat_floor") != 0;
    int truce = AiIni(ini, "truce_war_divisor");
    bool expansion = AiIni(ini, "expansion_refresh_monthly") != 0;
    bool schedule = AiIni(ini, "ai_schedule_from_date") != 0;
    bool random = AiIni(ini, "ai_random_from_save") != 0;
    bool skip = AiIni(ini, "build_plan_skip_unaffordable") != 0;
    int colonizer = AiIni(ini, "colonizer_idle_days");
    int constructor = AiIni(ini, "constructor_idle_days");
    bool blacklist = AiIni(ini, "survey_blacklist_monthly") != 0;
    if (!plan && !opportunist && !distance && !warGuard && !bossThreat && truce <= 0 && !expansion &&
        !schedule && !random && !skip && colonizer <= 0 && constructor <= 0 && !blacklist) {
        Log(LogLevel::Info, "ai_fixes", "all off ([ai_fixes] in stellaris-engine-fixes.ini)");
        return;
    }
    std::string error;
    auto find = [&](const char* what, bool (*locateFn)(const locate::Image&, locate::AiFixLocations&, std::string&)) {
        if (locateFn(img, g_ai, error)) return true;
        Log(LogLevel::Error, "ai_fixes", "AI %s: %s; off", what, error.c_str());
        return false;
    };
    if (plan && find("build plan", locate::LocateBuildPlan)) InstallBuildPlanFix();
    if (skip && find("build plan skip", locate::LocatePlanSkip)) InstallPlanSkipFix();
    if (colonizer > 0 && find("colony ships", locate::LocateColonizerIdle)) InstallColonizerFix(colonizer);
    if (constructor > 0 && find("construction ships", locate::LocateStationRange)) InstallStationRangeFix(constructor);
    if (blacklist && find("survey blacklist", locate::LocateSurveyBlacklist)) InstallSurveyBlacklistFix();
    if (opportunist && find("opportunist", locate::LocateOpportunist)) InstallOpportunistFix();
    if (distance && find("war distance", locate::LocateWarDistance)) InstallWarDistanceFix();
    if (warGuard) InstallAiWarGuard(img);
    if (bossThreat) InstallAiBossThreat(img);
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
        for (int i = 0; i < n; ++i) {
            if (income::Difference(tot[i], l.t0[i], l.r[i])) continue;
            --depth;
            g_incomeBails.fetch_add(1, std::memory_order_relaxed);
            return round;
        }
    } else {
        // Verifying: rounds after the first must have added the same.
        for (int i = 0; i < n; ++i) {
            int64_t expected;
            if (!income::Total(l.t0[i], l.r[i], round, expected)) {
                --depth;
                g_incomeBails.fetch_add(1, std::memory_order_relaxed);
                return round;
            }
            if (tot[i] == expected) continue;
            if (g_incomeMismatches.fetch_add(1) < 20) {
                Log(LogLevel::Error, "ai_income_targets", "MISMATCH: round %d, resource %d: total %lld, expected %lld", round, i,
                    static_cast<long long>(tot[i]), static_cast<long long>(expected));
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
    if (k == static_cast<int64_t>(round) + 1) return round;  // this round is the last anyway
    // Compute every result before changing the table. No partial update on a failed check.
    int64_t next[kIncomeMaxResources];
    for (int i = 0; i < n; ++i) {
        if (income::Total(l.t0[i], l.r[i], k - 1, next[i])) continue;
        g_incomeBails.fetch_add(1, std::memory_order_relaxed);
        return round;
    }
    memcpy(tot, next, n * sizeof(int64_t));
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
    if(g_incomeArithmeticInstalled){Log(LogLevel::Info, "ai_income_targets", "covered by exact income_target_arithmetic");return;}
    std::string ini = IniPath(self);
    if (!ini.empty() && GetPrivateProfileIntA("speed", "fast_income_target", 1, ini.c_str()) == 0) {
        Log(LogLevel::Info, "ai_income_targets", "off (fast_income_target=0)");
        return;
    }
    g_incomeVerify = !ini.empty() && GetPrivateProfileIntA("speed", "income_target_verify", 0, ini.c_str()) != 0;
    std::string error;
    if (!locate::LocateIncomeTarget(img, g_income, error)) {
        Log(LogLevel::Error, "ai_income_targets", "%s; off", error.c_str());
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
        Log(LogLevel::Error, "ai_income_targets", "no memory near the game's code; off");
        return;
    }
    g_incomeOn = true;
    Log(LogLevel::Info, "ai_income_targets", "CalculateIncomeTarget's scaling loop +%llX, CalcScalingSubplans +%llX; rounds after the "
        "first are counted, not run%s",
        static_cast<unsigned long long>(g_income.loop - reinterpret_cast<uint8_t*>(g_base)),
        static_cast<unsigned long long>(g_income.calcScaling - reinterpret_cast<uint8_t*>(g_base)),
        g_incomeVerify ? " (verifying rounds 2 and 3)" : "");
}

void LogIncomeStats() {
    if (!g_incomeOn) return;
    Log(LogLevel::Stats, "ai_income_targets", "loops_past_round_one=%llu shortened=%llu rounds_skipped=%llu "
        "max_rounds_skipped=%llu native_loops=%llu faulted=%d verify=%d mismatches=%llu",
        g_incomeLoops.load(), g_incomeSkips.load(), g_incomeRoundsSkipped.load(), g_incomeMaxRounds.load(),
        g_incomeBails.load(), g_incomeFailed, g_incomeVerify, g_incomeMismatches.load());
}

// ===========================================================================
// Script storage (re/script_storage.md)
//
// Paradox script keeps data outside the game without C#, through log = "@sef:..." effects and
// triggers: numbers from variables, and text recorded from log lines, which `run` executes later.
// Two stores, global (shared by every game, sefdata\global.txt) and save (sefdata\saves\<folder>\
// <name>.txt, written once a save has succeeded, read when that save loads, empty for a new game).
// In multiplayer each machine has its own files, so storage is off there unless multiplayer=1,
// which has the host send its stores to every player ("Script storage in multiplayer", below).

locate::VariableLocations g_vars;
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
    Log(LogLevel::Error, "script_storage", "%s", buf);
}

bool StoreMultiplayer() {
    __try {
        const uint8_t* session = *g_mp.session;
        return session && session[g_mp.flag] != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}

// ---- Script storage in multiplayer (mpstore.h) ----
// With [script_storage] multiplayer=1 the host sends its stores to every player as chat commands
// the chat never shows. The host's stores are its real ones; a client has copies, which no file
// ever sees. Until this machine has the snapshot of the session it is in, storage is refused.

mpstore::Locations g_mpStore;
bool g_mpStoreOn = false;
pdxstore::Store g_mpGlobal, g_mpSave;                     // a client's copies; under g_storeLock
std::atomic<const uint8_t*> g_mpSyncedSession{nullptr};  // the session whose snapshot this machine executed
mpstore::Assembler g_mpAssembler;                         // under g_storeLock
void(__fastcall* g_origChatExecute)(void*) = nullptr;
void(__fastcall* g_origStartSend)(void*, void*, bool) = nullptr;
void(__fastcall* g_origStartSync)(void*) = nullptr;
void(__fastcall* g_origSessionInit)(void*, void*, int, int) = nullptr;

enum class MpRole { Single, Host, Client };

// The session commands go to, as the game finds it (the idler's application's AccessSession);
// null without one. No C++ objects here, for __try.
uint8_t* MpSession() {
    if (!g_mpStoreOn) return nullptr;
    __try {
        uint8_t* idler = *g_mpStore.idler;
        if (!idler) return nullptr;
        uint8_t* app = *reinterpret_cast<uint8_t**>(idler + g_mpStore.idlerApp);
        if (!app) return nullptr;
        using AccessFn = uint8_t*(__fastcall*)(uint8_t*);
        return (*reinterpret_cast<AccessFn*>(*reinterpret_cast<uint8_t**>(app) + g_mpStore.appSessionSlot))(app);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool MpReadSession(const uint8_t* session, bool* host, int* type) {
    *host = false;
    *type = -1;
    if (!session) return false;
    __try {
        *host = session[g_mpStore.hostFlag] != 0;
        *type = *reinterpret_cast<const int*>(session + g_mpStore.sessionType);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// What this machine is in the running game. Without the session it counts as a client, which has
// no data.
MpRole MpRoleNow() {
    if (!StoreMultiplayer()) return MpRole::Single;
    bool host = false;
    int type = 0;
    MpReadSession(MpSession(), &host, &type);
    return host ? MpRole::Host : MpRole::Client;
}

// Whether script may use the stores now: always in single player, and in multiplayer once this
// machine has executed the snapshot of the session it is in, including the host.
bool MpStoreReady(MpRole* role) {
    *role = MpRoleNow();
    if (*role == MpRole::Single) return true;
    const uint8_t* synced = g_mpSyncedSession.load();
    return synced && MpSession() == synced;
}

// Posts one chat command with `text` to every player. No C++ objects here, for __try.
bool MpPostText(uint8_t* session, const char* text) {
    game::String s = BorrowString(text);
    __try {
        auto* command = reinterpret_cast<void*(__fastcall*)(size_t)>(g_mpStore.operatorNew)(mpstore::kChatCommandSize);
        if (!command) return false;
        using CtorFn = void*(__fastcall*)(void*, const game::String*, int, int, int, bool, int);
        reinterpret_cast<CtorFn>(g_mpStore.chatCtor)(command, &s, 0, 0, 0, true, -1);
        bool posted = reinterpret_cast<bool(__fastcall*)(void*, void*)>(g_mpStore.post)(session, command);
        if (!posted) {  // still ours: the scalar deleting destructor, the vtable's first slot
            using DeleteFn = void*(__fastcall*)(void*, unsigned);
            (*reinterpret_cast<DeleteFn**>(command))[0](command, 1);
        }
        return posted;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

pdxstore::Store& StoreFor(pdxstore::Which which);

// The host sends both stores to every player. Main thread.
bool MpPostSnapshot(uint8_t* session, const char* when, bool newGame = false) {
    std::string snapshot;
    size_t globalKeys = 0, saveKeys = 0;
    {
        std::lock_guard<std::mutex> lock(g_storeLock);
        StoreFor(pdxstore::Which::Global);  // reads the file the first time
        snapshot = mpstore::MakeSnapshot(g_globalStore, newGame ? pdxstore::Store{} : g_saveStore);
        globalKeys = g_globalStore.Count();
        saveKeys = newGame ? 0 : g_saveStore.Count();
    }
    if (snapshot.size() > mpstore::kMaxSnapshot) {
        Log(LogLevel::Warn, "script_storage", "the stores are %zu bytes, more than the %zu sent in multiplayer; posting storage disable to all players",
            snapshot.size(), mpstore::kMaxSnapshot);
        return MpPostText(session, mpstore::kDisabled);
    }
    std::vector<std::string> messages = mpstore::MakeMessages(snapshot);
    for (const std::string& m : messages) {
        if (!MpPostText(session, m.c_str())) {
            Log(LogLevel::Error, "script_storage", "the session refused a store command (%s)", when);
            return false;
        }
    }
    Log(LogLevel::Info, "script_storage", "sent the stores to the players (%s): %zu global and %zu save keys, %zu bytes in %zu chat commands",
        when, globalKeys, saveKeys, snapshot.size(), messages.size());
    return true;
}

// Only the host gets here (CGameSetup::Start), when the lobby starts the game.
void __fastcall MpStartSyncHook(void* setup) {
    uint8_t* session = MpSession();
    bool host = false;
    int type = 0;
    MpReadSession(session, &host, &type);
    if (session && host && type == mpstore::kLobbyHostSession) {
        if (!MpPostSnapshot(session, "the lobby starts the game", true)) {
            Log(LogLevel::Error, "script_storage", "game start blocked; retry after fixing the session");
            return;
        }
    } else {
        Log(LogLevel::Warn, "script_storage", "a game starts and the stores are not sent (session type %d, host %d)", type, host ? 1 : 0);
    }
    g_origStartSync(setup);
}

// The host starts to send its game state to a player: a hot join, or a loaded save.
bool MpIsSaveTransfer(const void* handler, const uint8_t* session) {
    __try {
        auto* p = static_cast<const uint8_t*>(handler);
        auto* name = reinterpret_cast<const game::String*>(p + g_mpStore.transferName);
        return *reinterpret_cast<uint8_t* const*>(p + g_mpStore.transferSession) == session &&
               name->size == sizeof("HotJoinData") - 1 &&
               memcmp(name->data(), "HotJoinData", sizeof("HotJoinData") - 1) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The game is running: only ask for a snapshot here (mpstore::kResync). MpTakeMessage takes it
// where the request executes, so script that runs before then keeps its writes.
void __fastcall MpStartSendHook(void* handler, void* blob, bool reset) {
    uint8_t* session = MpSession();
    bool host = false;
    int type = 0;
    MpReadSession(session, &host, &type);
    if (session && host && StoreMultiplayer() && MpIsSaveTransfer(handler, session) &&
        !MpPostText(session, mpstore::kResync)) {
        Log(LogLevel::Error, "script_storage", "the session refused a store command (a player receives the game); "
            "transfer blocked, retry after fixing the session");
        return;
    }
    g_origStartSend(handler, blob, reset);
}

void __fastcall MpSessionInitHook(void* session, void* server, int basicType, int type) {
    if (!g_mpStoreOn) return g_origSessionInit(session, server, basicType, type);
    {
        std::lock_guard<std::mutex> lock(g_storeLock);
        g_mpSyncedSession.store(nullptr);
        g_mpAssembler.Reset();
        g_mpGlobal = pdxstore::Store{};
        g_mpSave = pdxstore::Store{};
        g_recording = StoreRecording{};
        g_storeRuns.clear();
    }
    g_origSessionInit(session, server, basicType, type);
}

// A chat text that begins with mpstore::kPrefix: a piece of the host's stores.
void MpTakeMessage(const std::string& text) {
    uint8_t* session = MpSession();
    bool host = false;
    int type = 0;
    if (!MpReadSession(session, &host, &type)) return;
    std::string snapshot, error;
    mpstore::Assembler::Result result = mpstore::Assembler::Result::NotOurs;
    bool resync = text == mpstore::kResync;
    {
        std::lock_guard<std::mutex> lock(g_storeLock);
        // The ordered command, not the local send, revokes readiness on every peer.
        g_mpSyncedSession.store(nullptr);
        if (text == mpstore::kDisabled) {
            g_mpAssembler.Reset();
            Log(LogLevel::Warn, "script_storage", "host disabled storage for this multiplayer game (snapshot too large)");
            return;
        }
        if (resync) {
            g_mpAssembler.Reset();
        } else {
            result = g_mpAssembler.Feed(text, &snapshot, &error);
        }
    }
    if (resync) {
        // Every machine is at this boundary with the same stores and refuses storage from here until
        // the snapshot's last piece executes, so the snapshot taken now loses no write. Posting while
        // a command executes is what the AI does during the tick command.
        if (host && !MpPostSnapshot(session, "a player receives the game")) {
            StoreProblem("multiplayer: the host could not send its stores; storage is off in this game until they are sent again");
        }
        return;
    }
    if (result == mpstore::Assembler::Result::Bad) {
        StoreProblem("multiplayer: %s; the host's stores were not taken", error.c_str());
        return;
    }
    if (result != mpstore::Assembler::Result::Complete) return;
    pdxstore::Store global, save;
    if (!mpstore::ReadSnapshot(snapshot, global, save, error)) {
        g_mpSyncedSession.store(nullptr);
        StoreProblem("multiplayer: the host's stores: %s; storage is off in this game", error.c_str());
        return;
    }
    size_t globalKeys = global.Count(), saveKeys = save.Count();
    {
        std::lock_guard<std::mutex> lock(g_storeLock);
        if (host) {
            // Apply at exactly the same command boundary as clients. Preserve a pending disk write.
            // Nothing was written since the snapshot was taken (kResync) or no game was running.
            bool dirty = g_globalStore.Dirty() || g_globalStore.Values() != global.Values();
            g_globalStore = std::move(global);
            if (dirty) g_globalStore.MarkDirty();
            g_saveStore = std::move(save);
        } else {
            g_mpGlobal = std::move(global);
            g_mpSave = std::move(save);
        }
    }
    g_mpSyncedSession.store(session);
    Log(LogLevel::Info, "script_storage", "took the host's stores: %zu global and %zu save keys, %zu bytes", globalKeys, saveKeys,
        snapshot.size());
}

// The command's text, or -1. No C++ objects here, for __try.
int32_t MpCopyChatText(const void* command, char* out, int32_t size) {
    __try {
        auto* text = reinterpret_cast<const game::String*>(static_cast<const uint8_t*>(command) + g_mpStore.chatText);
        return CopyGameString(text, out, size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

bool MpIsHostCommand(const void* command) {
    __try {
        return *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(command) + g_mpStore.commandSender) ==
               g_mpStore.initialHostId;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// CChatCommand::Execute, on every machine, in the order the session gives commands.
void __fastcall MpChatExecuteHook(void* command) {
    char text[mpstore::kMaxMessage + 2];
    int32_t length = MpCopyChatText(command, text, sizeof(text));
    if (length < 0 || !mpstore::IsMessage(text, strlen(text))) return g_origChatExecute(command);
    if (!MpIsHostCommand(command)) return;  // an ordinary client's chat must never replace the host's stores
    if (length > mpstore::kMaxMessage) {
        MpTakeMessage(std::string(mpstore::kPrefix) + "invalid-length");
        return;
    }
    MpTakeMessage(std::string(text, static_cast<size_t>(length)));
}

// The caller holds g_storeLock.
pdxstore::Store& StoreFor(pdxstore::Which which) {
    if (g_mpStoreOn && MpRoleNow() == MpRole::Client) return which == pdxstore::Which::Save ? g_mpSave : g_mpGlobal;
    if (which == pdxstore::Which::Save) return g_saveStore;
    if (!g_globalLoaded) {
        g_globalLoaded = true;
        std::string error;
        if (!pdxstore::ReadStore(g_storeDir + L"\\global.txt", g_globalStore, error)) {
            Log(LogLevel::Warn, "script_storage", "sefdata\\global.txt: %s; the global store starts empty (the file is kept until it is "
                "next written)", error.c_str());
        } else {
            Log(LogLevel::Info, "script_storage", "global store: %zu keys", g_globalStore.Count());
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
    MpRole role = MpRole::Single;
    if (!MpStoreReady(&role)) {
        StoreProblem("log = \"@sef:%s\": off in this multiplayer game (%s)", text,
                     g_mpStoreOn ? "the host sent no stores" : "[script_storage] multiplayer=0");
        return;
    }
    if (role != MpRole::Single && c.op == pdxstore::Op::Run) {
        StoreProblem("log = \"@sef:%s\": run is off in multiplayer", text);
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
        Log(LogLevel::Info, "script_storage", "recorded %s.%s: %d lines kept, %d without script dropped", StoreName(r.store), r.key.c_str(),
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
    MpRole role = MpRole::Single;
    if (!MpStoreReady(&role)) return false;  // the same answer on every machine
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
    if (g_recording.text.size() > pdxstore::kMaxValue) {
        ++g_recording.dropped;  // already too long to store: record_end reports it
    } else if (pdxstore::KeepRecordedLine(buf, &kept)) {
        g_recording.text += kept;
        g_recording.text += '\n';
        ++g_recording.lines;
    } else {
        ++g_recording.dropped;
    }
    return result;
}

// Runs `text` with the console's effect command. No C++ objects here, for __try.
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
    if (runs.empty()) return;
    if (!*g_bench.gameState) {
        StoreProblem("run: %zu queued with no game running; dropped", runs.size());
        return;
    }
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
            Log(LogLevel::Info, "script_storage", "ran %s.%s (%zu bytes)%s", name, runs[i].key.c_str(), texts[i].size(),
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
    bool tracked = g_origZipAndWrite && MpRoleNow() != MpRole::Client &&
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
        Log(LogLevel::Info, "script_storage", "save store written for %s: %zu keys", save.key.c_str(), count);
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
    if (g_mpStoreOn && MpRoleNow() == MpRole::Client) return; // the host supplies the save store
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
        Log(LogLevel::Info, "script_storage", "save store read for %s: %zu keys", key.c_str(), g_saveStore.Count());
    }
}

void StoreNewGame() {
    if (!g_storeOn) return;
    std::lock_guard<std::mutex> lock(g_storeLock);
    g_saveStore.Clear();
    g_saveStore.SetClean();
    g_mpSave.Clear();  // a client's copy: every machine starts the new game with an empty save store
    g_mpSave.SetClean();
}

// At exit, if the main thread isn't in the middle of a change.
void StoreExit() {
    if (!g_storeOn || !g_storeLock.try_lock()) return;
    WriteGlobalStore();
    g_storeLock.unlock();
}

void LogStoreStats() {
    if (!g_storeOn) return;
    uint64_t sets = g_storeSets.load(), gets = g_storeGets.load(), tests = g_storeTests.load(), rec = g_storeRecorded.load(),
             ran = g_storeRan.load(), errors = g_storeErrors.load();
    if (!sets && !gets && !tests && !rec && !ran && !errors) return;
    Log(LogLevel::Stats, "script_storage", "sets=%llu gets=%llu has=%llu recorded=%llu runs=%llu problems=%llu",
        sets, gets, tests, rec, ran, errors);
}

bool RedirectCall(uint8_t* call, const void* hook) {
    Code code;
    code.JumpTo(hook);
    uint8_t* stub = PlaceStub(code, call);
    int32_t rel = 0;
    return stub && call[0] == 0xE8 && Rel32(call + 5, stub, &rel) && WriteCode(call + 1, &rel, sizeof(rel));
}

using ChangeTradeAmountFn = void(*)(void*, int, bool);
using TradeUiAction = void(*)(void*);
using TradeUiDestroy = void*(*)(void*, unsigned);
using TradeDialogSetTitle = void(*)(void*, const game::String*);

// CTextInputDialog copies the 32-byte Boost function buffer when the vtable's low bit is set.
// The invoker needs no captured state and the tagged vtable skips manager calls on destruction.
struct TradeTextCallback {
    uintptr_t taggedVtable;
    uint64_t storage[4]{};
};
static_assert(sizeof(TradeTextCallback) == 40 && offsetof(TradeTextCallback, storage) == 8);

uishortcuts::TradeLocations g_tradeUi;
uishortcuts::TradeControlLocations g_tradeControls;
uishortcuts::MarketAmountBackgroundLocations g_marketAmountBackground;
bool g_tradeControlsReady = false;

enum class TradeAmountKind { LumpSum, EmpireMonthly, MarketMonthly };
struct MarketTradeContext {
    const void* resource = nullptr;
    int mode = 0;
    int id = 0;
};
struct TradeAmountEdit {
    uint8_t* dialog = nullptr;
    void* row = nullptr;
    TradeAmountKind kind = TradeAmountKind::LumpSum;
    MarketTradeContext market;
    bool invalid = false;
};
TradeAmountEdit g_tradeEdit;
struct TradeRowLabels {
    void* container = nullptr;
    void* lump = nullptr;
    void* monthly = nullptr;
};
std::unordered_map<void*, TradeRowLabels> g_tradeLabels;
uint8_t* g_marketTradeView = nullptr;
using TradeInputFn = void(*)(void*, uint8_t*, void*);
using MarketAmountClickFn = void(*)(uint8_t*, void*);
TradeInputFn g_tradeTextInput = nullptr;
TradeInputFn g_tradeSpriteInput = nullptr;
MarketAmountClickFn g_marketAmountClick = nullptr;

game::String TradeDialogString(const char* text) {
    game::String result{};
    result.size = strlen(text);
    if (result.size <= 15) {
        result.cap = 15;
        memcpy(result.buf, text, result.size + 1);
    } else {
        result.kind = 1;
        result.ptr = const_cast<char*>(text);
        result.cap = result.size;
    }
    return result;
}

bool ParseTradeDialogText(const game::String* text, int& amount) {
    __try {
        return text->size < 32 && uishortcuts::ParseTradeAmount({text->data(), text->size}, amount);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* TradeGuiChild(void* container, size_t slot, const game::String* name) {
    using FindChild = void*(*)(void*, const game::String*);
    __try {
        if (!container) return nullptr;
        auto* vtable = *static_cast<uint8_t**>(container);
        return (*reinterpret_cast<FindChild*>(vtable + slot))(container, name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void* TradeGuiPointer(const void* object, size_t offset) {
    __try {
        return *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(object) + offset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool ReadMarketTrade(uint8_t* view, MarketTradeContext& context) {
    __try {
        if (!view || !reinterpret_cast<bool(*)(void*)>(g_tradeControls.marketShown)(view)) return false;
        context.resource = *reinterpret_cast<void**>(view + g_tradeControls.marketResource);
        context.mode = *reinterpret_cast<int*>(view + g_tradeControls.marketMode);
        context.id = *reinterpret_cast<int*>(view + g_tradeControls.marketId);
        return context.resource != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void SetMarketTradeAmount(uint8_t* view, const MarketTradeContext& expected, int amount) {
    MarketTradeContext current;
    if (view != g_marketTradeView || !ReadMarketTrade(view, current) || current.resource != expected.resource ||
        current.mode != expected.mode || current.id != expected.id) return;
    __try {
        // OnAmountChange writes this nonnegative draft field. Update and AddTrade still
        // handle the display, eligibility checks and eventual submission.
        *reinterpret_cast<int*>(view + g_tradeControls.marketAmount) = amount;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_marketTradeView = nullptr;
    }
}

void AcceptTradeAmount(void*, const game::String* text) {
    if (!g_tradeEdit.row) return;
    int amount;
    if (!ParseTradeDialogText(text, amount)) {
        g_tradeEdit.invalid = true;
        return;
    }
    auto* row = g_tradeEdit.row;
    g_tradeEdit.row = nullptr;
    if (g_tradeEdit.kind == TradeAmountKind::MarketMonthly) {
        SetMarketTradeAmount(static_cast<uint8_t*>(row), g_tradeEdit.market, amount);
        return;
    }
    reinterpret_cast<ChangeTradeAmountFn>(g_tradeUi.changeValue)(row, amount, g_tradeEdit.kind == TradeAmountKind::EmpireMonthly);
    reinterpret_cast<TradeUiAction>(g_tradeUi.refreshRow)(row);
}

void FinishTradeDialog(uint8_t* dialog, bool finished) {
    __try {
        dialog[g_tradeUi.dialogFinishedOffset] = finished;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_tradeEdit = {};
    }
}

void TradeDialogAcceptHook(uint8_t* dialog) {
    reinterpret_cast<TradeUiAction>(g_tradeUi.dialogAccept)(dialog);
    if (dialog == g_tradeEdit.dialog && g_tradeEdit.invalid) {
        g_tradeEdit.invalid = false;
        FinishTradeDialog(dialog, false);
        if (dialog == g_tradeEdit.dialog) {
            auto title = TradeDialogString("Enter a whole number");
            reinterpret_cast<TradeDialogSetTitle>(g_tradeUi.setTitle)(dialog, &title);
        }
    }
}

void* TradeDialogDestroyHook(void* dialog, unsigned flags) {
    if (dialog == g_tradeEdit.dialog) g_tradeEdit = {};
    return reinterpret_cast<TradeUiDestroy>(g_tradeUi.dialogDestroy)(dialog, flags);
}

void CancelTradeAmountEdit(void* row) {
    if (!g_tradeEdit.dialog || row != g_tradeEdit.row) return;
    g_tradeEdit.row = nullptr;
    FinishTradeDialog(g_tradeEdit.dialog, true);
}

void TradeRowClearHook(void* row) {
    CancelTradeAmountEdit(row);
    g_tradeLabels.erase(row);
    reinterpret_cast<TradeUiAction>(g_tradeUi.rowClear)(row);
}

void* TradeRowDestroyHook(uint8_t* subobject, unsigned flags) {
    auto* row = subobject - g_tradeUi.rowDestroyThisOffset;
    CancelTradeAmountEdit(row);
    g_tradeLabels.erase(row);
    return reinterpret_cast<TradeUiDestroy>(g_tradeUi.rowDestroy)(subobject, flags);
}

void* TradeDialogIdler() {
    __try {
        return *g_tradeUi.idler;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool OpenTradeAmountDialog(void* row, TradeAmountKind kind, const MarketTradeContext& market = {}) {
    if (g_tradeEdit.dialog) return false;
    auto* idler = TradeDialogIdler();
    if (!idler) return false;
    using Allocate = void*(*)(size_t);
    using Construct = uint8_t*(*)(void*, const game::String*, const game::String*, const TradeTextCallback*);
    using AddUpdateable = int(*)(void*, void*);
    static const struct {
        uintptr_t manager;
        void (*invoke)(void*, const game::String*);
    } callbackVtable{0, &AcceptTradeAmount};

    auto title = TradeDialogString(kind == TradeAmountKind::LumpSum ? "Trade amount" : "Monthly trade amount");
    auto initialText = TradeDialogString("");
    TradeTextCallback callback{reinterpret_cast<uintptr_t>(&callbackVtable) | 1};
    auto* storage = reinterpret_cast<Allocate>(g_tradeUi.allocate)(g_tradeUi.dialogSize);
    auto* dialog = reinterpret_cast<Construct>(g_tradeUi.dialogConstructor)(storage, &title, &initialText, &callback);
    g_tradeEdit = {dialog, row, kind, market};
    reinterpret_cast<AddUpdateable>(g_tradeUi.addUpdateable)(idler, dialog);
    if (g_tradeEdit.dialog != dialog) return false;
    if (g_tradeControlsReady) {
        auto editName = TradeDialogString("editbox");
        auto* container = TradeGuiPointer(dialog, g_tradeControls.dialogRoot);
        auto* edit = TradeGuiChild(container, g_tradeControls.findEditBox, &editName);
        if (edit) reinterpret_cast<TradeUiAction>(g_tradeControls.giveFocus)(edit);
    }
    return true;
}

void ChangeTradeAmountHook(void* row, int amount, bool monthly) {
    if (GetKeyState(VK_MENU) < 0) {
        OpenTradeAmountDialog(row, monthly ? TradeAmountKind::EmpireMonthly : TradeAmountKind::LumpSum);
        return;
    }
    reinterpret_cast<ChangeTradeAmountFn>(g_tradeUi.changeValue)(row, amount, monthly);
}

void TradeRowRefreshHook(void* row) {
    reinterpret_cast<TradeUiAction>(g_tradeUi.refreshRow)(row);
    if (!g_tradeControlsReady) return;
    auto* container = TradeGuiPointer(row, g_tradeControls.rowRoot);
    auto existing = g_tradeLabels.find(row);
    if (!container || (existing != g_tradeLabels.end() && existing->second.container == container)) return;
    auto lump = TradeDialogString("value");
    auto monthly = TradeDialogString("value_monthly");
    g_tradeLabels[row] = {container, TradeGuiChild(container, g_tradeControls.findTextBox, &lump),
        TradeGuiChild(container, g_tradeControls.findTextBox, &monthly)};
}

void MarketTradeUpdateHook(uint8_t* view) {
    reinterpret_cast<TradeUiAction>(g_tradeControls.marketUpdate)(view);
    g_marketTradeView = view;
}

void ForgetMarketTradeView(uint8_t* view) {
    if (g_marketTradeView == view) g_marketTradeView = nullptr;
    CancelTradeAmountEdit(view);
}

void MarketTradeReloadHook(uint8_t* view) {
    ForgetMarketTradeView(view);
    reinterpret_cast<TradeUiAction>(g_tradeControls.marketReload)(view);
}

void* MarketTradeDestroyHook(uint8_t* view, unsigned flags) {
    ForgetMarketTradeView(view);
    return reinterpret_cast<TradeUiDestroy>(g_tradeControls.marketDestroy)(view, flags);
}

bool OpenMarketAmount(uint8_t* view) {
    MarketTradeContext market;
    if (!ReadMarketTrade(view, market)) return false;
    g_marketTradeView = view;
    return OpenTradeAmountDialog(view, TradeAmountKind::MarketMonthly, market);
}

void MarketAmountClickHook(uint8_t* view, void* button) {
    if (g_tradeControlsReady && button && GetKeyState(VK_MENU) < 0 &&
        (button == TradeGuiPointer(view, g_tradeControls.marketDecrease) ||
         button == TradeGuiPointer(view, g_tradeControls.marketIncrease))) {
        OpenMarketAmount(view);
        return;
    }
    g_marketAmountClick(view, button);
}

bool IsTradeAmountClick(const uint8_t* event) {
    __try {
        return !event[g_tradeControls.eventTaken] &&
            *reinterpret_cast<const int*>(event + g_tradeControls.eventType) == 0 &&  // mouse input
            *reinterpret_cast<const int*>(event + g_tradeControls.mouseAction) == 4 &&  // release
            *reinterpret_cast<const int*>(event + g_tradeControls.mouseButton) == 1;    // left button
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool OpenTradeAmountFromLabel(void* label) {
    if (g_marketTradeView && label == TradeGuiPointer(g_marketTradeView, g_tradeControls.marketLabel)) {
        return OpenMarketAmount(g_marketTradeView);
    }
    for (const auto& [row, labels] : g_tradeLabels) {
        if (label == labels.lump || label == labels.monthly) {
            // A reload can replace the row's GUI before the next refresh.
            if (labels.container != TradeGuiPointer(row, g_tradeControls.rowRoot)) return false;
            return OpenTradeAmountDialog(row, label == labels.lump ? TradeAmountKind::LumpSum : TradeAmountKind::EmpireMonthly);
        }
    }
    return false;
}

void TradeTextInputHook(void* label, uint8_t* event, void* gui) {
    if (g_tradeControlsReady && IsTradeAmountClick(event) && GetKeyState(VK_MENU) < 0 && OpenTradeAmountFromLabel(label)) {
        event[g_tradeControls.eventTaken] = 1;
        return;
    }
    g_tradeTextInput(label, event, gui);
}

bool IsMarketAmountBackground(void* sprite, uint8_t* view) {
    using FindChild = void*(*)(void*, const game::String*);
    const auto amount = TradeDialogString("editable_amount");
    const auto background = TradeDialogString("value_bg");
    __try {
        auto* root = TradeGuiPointer(view, g_marketAmountBackground.marketRoot);
        if (!root) return false;
        auto* window = reinterpret_cast<FindChild>(g_marketAmountBackground.findWindow)(root, &amount);
        return window && sprite == reinterpret_cast<FindChild>(g_marketAmountBackground.findIcon)(window, &background);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void TradeSpriteInputHook(void* sprite, uint8_t* event, void* gui) {
    // The market amount's background can receive clicks outside the rendered text.
    auto* view = g_marketTradeView;
    if (view && IsTradeAmountClick(event) && GetKeyState(VK_MENU) < 0 &&
        IsMarketAmountBackground(sprite, view) && OpenMarketAmount(view)) {
        event[g_tradeControls.eventTaken] = 1;
        return;
    }
    g_tradeSpriteInput(sprite, event, gui);
}

bool InstallTradeControls(const locate::Image& img, std::string& error) {
    if (!uishortcuts::LocateTradeControls(img, g_tradeUi, g_tradeControls, error)) return false;
    auto* market = g_tradeControls.marketVtable;
    if (!SwapVtableSlot(g_tradeUi.rowRefreshSlot, g_tradeUi.refreshRow, reinterpret_cast<void*>(&TradeRowRefreshHook)) ||
        !SwapVtableSlot(market, g_tradeControls.marketDestroy, reinterpret_cast<void*>(&MarketTradeDestroyHook)) ||
        !SwapVtableSlot(market + 2, g_tradeControls.marketReload, reinterpret_cast<void*>(&MarketTradeReloadHook)) ||
        !SwapVtableSlot(market + 4, g_tradeControls.marketUpdate, reinterpret_cast<void*>(&MarketTradeUpdateHook))) {
        error = "could not hook trade view lifetimes";
        return false;
    }
    g_marketAmountClick = reinterpret_cast<MarketAmountClickFn>(
        InstallJumpHook(g_tradeControls.marketClick, 16, reinterpret_cast<void*>(&MarketAmountClickHook)));
    g_tradeTextInput = reinterpret_cast<TradeInputFn>(
        InstallJumpHook(g_tradeControls.textInput, 16, reinterpret_cast<void*>(&TradeTextInputHook)));
    if (!g_marketAmountClick || !g_tradeTextInput) {
        error = "could not hook the market amount button or trade amount label";
        return false;
    }
    g_tradeControlsReady = true;
    return true;
}

void InstallTradeAmount(HMODULE self, const locate::Image& img) {
    auto ini = IniPath(self);
    if (!ini.empty() && GetPrivateProfileIntA("interface", "trade_amount_entry", 1, ini.c_str()) == 0) return;
    std::string error;
    if (!uishortcuts::LocateTrade(img, g_tradeUi, error)) {
        Log(LogLevel::Error, "trade_amount", "%s", error.c_str());
        return;
    }
    if (!SwapVtableSlot(g_tradeUi.dialogAcceptSlot, g_tradeUi.dialogAccept, reinterpret_cast<void*>(&TradeDialogAcceptHook)) ||
        !SwapVtableSlot(g_tradeUi.dialogDestroySlot, g_tradeUi.dialogDestroy, reinterpret_cast<void*>(&TradeDialogDestroyHook)) ||
        !SwapVtableSlot(g_tradeUi.rowClearSlot, g_tradeUi.rowClear, reinterpret_cast<void*>(&TradeRowClearHook)) ||
        !SwapVtableSlot(g_tradeUi.rowDestroySlot, g_tradeUi.rowDestroy, reinterpret_cast<void*>(&TradeRowDestroyHook)) ||
        !RedirectCall(g_tradeUi.call, reinterpret_cast<const void*>(&ChangeTradeAmountHook))) {
        Log(LogLevel::Error, "trade_amount", "could not install the native text dialog hooks");
        return;
    }
    Log(LogLevel::Info, "trade_amount", "installed native text dialog");
    if (!InstallTradeControls(img, error)) {
        Log(LogLevel::Warn, "trade_amount", "%s", error.c_str());
        return;
    }
    Log(LogLevel::Info, "trade_amount", "installed market, label clicks and keyboard focus");
    if (!uishortcuts::LocateMarketAmountBackground(img, g_tradeControls, g_marketAmountBackground, error)) {
        Log(LogLevel::Warn, "trade_amount", "%s", error.c_str());
        return;
    }
    g_tradeSpriteInput = reinterpret_cast<TradeInputFn>(
        InstallJumpHook(g_marketAmountBackground.spriteInput, 16, reinterpret_cast<void*>(&TradeSpriteInputHook)));
    if (g_tradeSpriteInput) Log(LogLevel::Info, "trade_amount", "installed market amount background clicks");
    else Log(LogLevel::Warn, "trade_amount", "could not hook market amount background clicks");
}

using UiPostCommandFn = void(*)(void*, bool);
using UiExecuteCommandFn = void(*)(void*);
using UiDestroyFn = void*(*)(void*, unsigned);
using UiQueueInsertFn = void*(*)(uint8_t*, int, const uint32_t*);
using UiCreateCommandFn = void*(*)(const void*, int);

UiPostCommandFn g_uiPostCommand = nullptr;
locate::MultiplayerLocations g_uiMultiplayer;
uishortcuts::QueueLocations g_queueUi;
UiExecuteCommandFn g_queueExecute = nullptr;
UiDestroyFn g_queueDestroy = nullptr;
UiQueueInsertFn g_queueInsert = nullptr;
UiCreateCommandFn g_queueCreate = nullptr;
std::mutex g_queueCommandsLock;
std::unordered_set<const void*> g_queueCommands;
thread_local bool g_queuePostingFirst = false;
thread_local bool g_queueFirst = false;

bool UiLocalSession() {
    __try {
        const auto* session = *g_uiMultiplayer.session;
        return session && session[g_uiMultiplayer.flag] == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool IsQueueCommand(const void* command) {
    __try {
        return command && *static_cast<uintptr_t* const*>(command) == g_queueUi.vtable;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool TakeQueueCommand(const void* command) {
    std::lock_guard<std::mutex> lock(g_queueCommandsLock);
    return g_queueCommands.erase(command) != 0;
}

void UiPostCommandHook(void* command, bool validate) {
    bool previous = g_queuePostingFirst;
    g_queuePostingFirst = IsQueueCommand(command) && GetKeyState(VK_CONTROL) < 0 && UiLocalSession();
    g_uiPostCommand(command, validate);
    g_queuePostingFirst = previous;
}

void* QueueCreateCommandHook(const void* bytes, int size) {
    // The single-player server serializes the posted command and queues a fresh copy.
    // Keep the input modifier on that copy locally, outside the command's serialized fields.
    void* command = g_queueCreate(bytes, size);
    if (g_queuePostingFirst && IsQueueCommand(command)) {
        std::lock_guard<std::mutex> lock(g_queueCommandsLock);
        g_queueCommands.insert(command);
    }
    return command;
}

void QueueExecuteHook(void* command) {
    bool previous = g_queueFirst;
    g_queueFirst = TakeQueueCommand(command) && UiLocalSession();
    g_queueExecute(command);
    g_queueFirst = previous;
}

void* QueueDestroyHook(void* command, unsigned flags) {
    TakeQueueCommand(command);
    return g_queueDestroy(command, flags);
}

bool IsPlanetQueue(const uint8_t* array) {
    constexpr uint8_t kPlanetQueue = 1;  // EConstructionQueueTypeEnum::Planet
    __try {
        return array[g_queueUi.arrayToType] == kPlanetQueue;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* QueueInsertHook(uint8_t* array, int index, const uint32_t* item) {
    if (g_queueFirst) {
        g_queueFirst = false;
        if (IsPlanetQueue(array)) index = 0;
    }
    return g_queueInsert(array, index, item);
}

bool InstallQueueShortcut(const locate::Image& img, std::string& error) {
    if (!uishortcuts::LocateQueue(img, g_queueUi, error)) return false;
    g_queueExecute = reinterpret_cast<UiExecuteCommandFn>(g_queueUi.execute);
    g_queueDestroy = reinterpret_cast<UiDestroyFn>(g_queueUi.destroy);
    g_queueInsert = reinterpret_cast<UiQueueInsertFn>(g_queueUi.insert);
    g_queueCreate = reinterpret_cast<UiCreateCommandFn>(g_queueUi.create);
    if (!RedirectCall(g_queueUi.insertCall, reinterpret_cast<const void*>(&QueueInsertHook)) ||
        !RedirectCall(g_queueUi.createCall, reinterpret_cast<const void*>(&QueueCreateCommandHook)) ||
        !SwapVtableSlot(g_queueUi.vtable, g_queueUi.destroy, reinterpret_cast<void*>(&QueueDestroyHook)) ||
        !SwapVtableSlot(g_queueUi.vtable + 9, g_queueUi.execute, reinterpret_cast<void*>(&QueueExecuteHook))) {
        error = "could not install the construction queue hooks";
        return false;
    }
    return true;
}

void InstallUiCommandShortcuts(HMODULE self, const locate::Image& img) {
    auto ini = IniPath(self);
    bool queue = ini.empty() || GetPrivateProfileIntA("interface", "planet_queue_first", 1, ini.c_str()) != 0;
    if (!queue) return;

    std::string error;
    if (!locate::LocateMultiplayer(img, g_uiMultiplayer, error)) {
        Log(LogLevel::Error, "command_shortcuts", "%s", error.c_str());
        return;
    }
    auto* post = locate::FindOne(img, "PostCommandToSession", uishortcuts::kPostCommand, error);
    if (!post) {
        Log(LogLevel::Error, "command_shortcuts", "%s", error.c_str());
        return;
    }
    if (!InstallQueueShortcut(img, error)) {
        Log(LogLevel::Warn, "planet_queue", "%s", error.c_str());
        return;
    }
    auto* trampoline = InstallJumpHook(post, 15, reinterpret_cast<void*>(&UiPostCommandHook));
    if (!trampoline) {
        Log(LogLevel::Error, "command_shortcuts", "could not hook command posting");
        return;
    }
    g_uiPostCommand = reinterpret_cast<UiPostCommandFn>(trampoline);
    Log(LogLevel::Info, "planet_queue", "installed for single player");
}

bool InstallStoreSaveWrite(const locate::Image& img) {
    std::string error;
    if (!locate::LocateSaveWrite(img, g_saves, g_saveWrite, error)) {
        Log(LogLevel::Error, "script_storage", "%s", error.c_str());
        return false;
    }
    g_origDestroySaveArchive = reinterpret_cast<decltype(g_origDestroySaveArchive)>(g_saveWrite.destroyArchive);
    g_origCloseSaveFile = reinterpret_cast<decltype(g_origCloseSaveFile)>(g_saveWrite.closeFile);
    g_origCloudSaveWrite = reinterpret_cast<decltype(g_origCloudSaveWrite)>(g_saveWrite.cloudWrite);
    g_origSaveAddFile = reinterpret_cast<decltype(g_origSaveAddFile)>(g_saveWrite.addFile);
    if (!RedirectCall(g_saveWrite.closeCall, reinterpret_cast<const void*>(&StoreCloseSaveFileHook)) ||
        !RedirectCall(g_saveWrite.cloudCall, reinterpret_cast<const void*>(&StoreCloudSaveWriteHook))) return false;
    for (uint8_t* call : g_saveWrite.archiveDestroyCalls) {
        if (!RedirectCall(call, reinterpret_cast<const void*>(&StoreDestroySaveArchiveHook))) return false;
    }
    for (uint8_t* call : g_saveWrite.addFileCalls) {
        if (!RedirectCall(call, reinterpret_cast<const void*>(&StoreSaveAddFileHook))) return false;
    }
    // Until this last hook installs, the call redirects only forward to the game.
    auto* zip = InstallJumpHook(g_saveWrite.zipAndWrite, locate::kZipAndWriteStolenBytes,
                                reinterpret_cast<void*>(&StoreZipAndWriteHook));
    g_origZipAndWrite = reinterpret_cast<decltype(g_origZipAndWrite)>(zip);
    return zip != nullptr;
}

// [script_storage] multiplayer=1: the host's stores go to every player (mpstore.h).
void InstallMpStore(const std::string& ini, const locate::Image& img) {
    if (ini.empty() || GetPrivateProfileIntA("script_storage", "multiplayer", 0, ini.c_str()) == 0) {
        Log(LogLevel::Info, "script_storage", "off in multiplayer (multiplayer=0)");
        return;
    }
    std::string error;
    if (!mpstore::Locate(img, g_mpStore, error)) {
        Log(LogLevel::Error, "script_storage", "%s; off in multiplayer although multiplayer=1", error.c_str());
        return;
    }
    auto init = InstallJumpHook(g_mpStore.sessionInit, mpstore::kSessionInitStolenBytes,
                               reinterpret_cast<void*>(&MpSessionInitHook));
    if (!init) {
        Log(LogLevel::Error, "script_storage", "failed to hook CSession::Init; off in multiplayer although multiplayer=1");
        return;
    }
    g_origSessionInit = reinterpret_cast<decltype(g_origSessionInit)>(init);
    // The two jump hooks only forward to the game until g_mpStoreOn is set.
    auto sync = InstallJumpHook(g_mpStore.startSync, mpstore::kStartSyncStolenBytes, reinterpret_cast<void*>(&MpStartSyncHook));
    if (!sync) {
        Log(LogLevel::Error, "script_storage", "failed to hook CGameSetup::StartSyncPlayers; off in multiplayer although multiplayer=1");
        return;
    }
    g_origStartSync = reinterpret_cast<decltype(g_origStartSync)>(sync);
    auto send = InstallJumpHook(g_mpStore.startSend, mpstore::kStartSendStolenBytes, reinterpret_cast<void*>(&MpStartSendHook));
    if (!send) {
        Log(LogLevel::Error, "script_storage", "failed to hook CLargeFileHandler::StartSend; off in multiplayer although multiplayer=1");
        return;
    }
    g_origStartSend = reinterpret_cast<decltype(g_origStartSend)>(send);
    g_origChatExecute = reinterpret_cast<decltype(g_origChatExecute)>(g_mpStore.chatExecute);
    if (!SwapVtableSlot(g_mpStore.chatExecuteSlot, g_mpStore.chatExecute, reinterpret_cast<void*>(&MpChatExecuteHook))) {
        Log(LogLevel::Error, "script_storage", "failed to hook CChatCommand::Execute; off in multiplayer although multiplayer=1");
        return;
    }
    g_mpStoreOn = true;
    auto rva = [](const void* p) { return static_cast<unsigned long long>(static_cast<const uint8_t*>(p) - reinterpret_cast<uint8_t*>(g_base)); };
    Log(LogLevel::Info, "script_storage", "on in multiplayer, the host sends its stores; chat command +%llX, Execute +%llX, Post +%llX, "
        "StartSend +%llX, StartSyncPlayers +%llX, operator new +%llX, host flag +0x%zx",
        rva(g_mpStore.chatCtor), rva(g_mpStore.chatExecute), rva(g_mpStore.post), rva(g_mpStore.startSend),
        rva(g_mpStore.startSync), rva(g_mpStore.operatorNew), g_mpStore.hostFlag);
}

void InstallScriptStorage(HMODULE self, const locate::Image& img) {
    std::string ini = IniPath(self);
    if (!ini.empty() && GetPrivateProfileIntA("script_storage", "enabled", 1, ini.c_str()) == 0) {
        Log(LogLevel::Info, "script_storage", "off (enabled=0)");
        return;
    }
    std::string error;
    locate::LexerLocations lexer;
    if (!locate::LocateVariables(img, g_vars, error) || !locate::LocateLexer(img, lexer, error) ||
        !locate::LocateMultiplayer(img, g_mp, error) ||
        !locate::LocateLogScript(img, g_vars.effectName, g_vars.triggerName, g_storeLogs, error)) {
        Log(LogLevel::Error, "script_storage", "%s; off", error.c_str());
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
        Log(LogLevel::Error, "script_storage", "no room for the log text stub; off");
        return;
    }
    g_origProcessString = reinterpret_cast<ProcessStringFn>(g_storeLogs.processString);
    if (!HookLogs()) {
        Log(LogLevel::Error, "script_storage", "failed to hook CLogEffect or CLogTrigger; off");
        return;
    }
    if (!WriteCode(call + 1, &rel, sizeof(rel))) {
        Log(LogLevel::Error, "script_storage", "failed to redirect the log text call; off");
        return;
    }
    if (!InstallSaves(img) || !InstallStoreSaveWrite(img)) {
        Log(LogLevel::Warn, "script_storage", "save persistence is off; save hooks could not be installed");
    }
    uint8_t* readExecute = nullptr;
    if (!g_bench.gameState || !g_origCheckTick) {
        Log(LogLevel::Warn, "script_storage", "no tick hook; run is off");
    } else if (!locate::LocateEffectCommand(img, g_bench.gameState, &g_storeEffectCommand, &readExecute, error)) {
        g_storeEffectCommand = nullptr;
        Log(LogLevel::Warn, "script_storage", "%s; run is off", error.c_str());
    }
    g_storeOn = true;
    Log(LogLevel::Info, "script_storage", "on; log = \"@sef:...\"; variables: access +%llX, get +%llX, set +%llX, map +0x%zx (value +0x%zx); "
        "log text call +%llX; multiplayer flag +0x%zx; data in sefdata\\",
        static_cast<unsigned long long>(g_vars.access - reinterpret_cast<uint8_t*>(g_base)),
        static_cast<unsigned long long>(g_vars.get - reinterpret_cast<uint8_t*>(g_base)),
        static_cast<unsigned long long>(g_vars.set - reinterpret_cast<uint8_t*>(g_base)), g_vars.map, g_vars.entryValue,
        static_cast<unsigned long long>(call - reinterpret_cast<uint8_t*>(g_base)), g_mp.flag);
    InstallMpStore(ini, img);
}

// ===========================================================================

std::atomic<ULONGLONG> g_lastSummary{0};

void MaybeLogSummary() {
    ULONGLONG now = GetTickCount64();
    ULONGLONG last = g_lastSummary.load(std::memory_order_relaxed);
    if (now - last < 30000 || !g_lastSummary.compare_exchange_strong(last, now)) return;
    Log(LogLevel::Stats, "fleets", "sorts=%llu compares=%llu keys=%llu avg_us=%.1f max_us=%.1f faulted=%d",
        g_fleetTiming.passes, g_fleetCompares, g_fleetKeyBuilds, g_fleetTiming.AvgUs(), g_fleetTiming.MaxUs(),
        g_fleetFailed);
    Log(LogLevel::Stats, "megastructures", "sorts=%llu names=%llu avg_us=%.1f max_us=%.1f faulted=%d",
        g_megaTiming.passes, g_megaNameBuilds, g_megaTiming.AvgUs(), g_megaTiming.MaxUs(), g_megaFailed);
    if (auto heap = alloc::Stats(); !heap.empty()) Log(LogLevel::Stats, "allocator", "%s", heap.c_str());
    LogIntelStats();
    LogCombatStats();
    LogAffordStats();
    LogIncomeStats();
    LogLexerStats();
    LogDeferredStats();
    LogFileLookupStats();
    LogStoreStats();
    LogAiFixStats();
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

// ---- Multiplayer: result-changing settings in the game's checksum (mpchecksum.h) ----

mpchecksum::Locations g_mpChecksum;
std::string g_mpText;  // names the settings that are on; never empty once the hook is in
using ChecksumFinalizeFn = void(__fastcall*)(uint8_t* checksum);
using Md5UpdateFn = void(__fastcall*)(uint8_t* md5, const unsigned char* bytes, uint32_t count);
ChecksumFinalizeFn g_origChecksumFinalize = nullptr;

// Before the game finishes a checksum of its files: the settings' text goes in after the files.
void __fastcall ChecksumFinalizeHook(uint8_t* checksum) {
    reinterpret_cast<Md5UpdateFn>(g_mpChecksum.md5Update)(checksum + g_mpChecksum.md5,
                                                         reinterpret_cast<const unsigned char*>(g_mpText.data()),
                                                         static_cast<uint32_t>(g_mpText.size()));
    Log(LogLevel::Info, "multiplayer", "the game's checksum now includes %s", g_mpText.c_str());
    g_origChecksumFinalize(checksum);
}

void InstallMpChecksum(HMODULE self, const locate::Image& img) {
    std::string ini = IniPath(self);
    if (ini.empty()) return;  // without an ini every result-changing setting is off
    std::vector<mpchecksum::Setting> settings;
    std::vector<char> entries(32768);
    for (const char* section : {"numeric_fixes", "ai_fixes"}) {
        DWORD n = GetPrivateProfileSectionA(section, entries.data(), static_cast<DWORD>(entries.size()), ini.c_str());
        if (n >= entries.size() - 2) {
            Log(LogLevel::Error, "multiplayer", "[%s] is too long to read; its settings are not in the game's checksum", section);
            continue;
        }
        mpchecksum::AddSection(section, entries.data(), settings);
    }
    // Storage in multiplayer only works when every player has it: the host's stores are sent as chat commands.
    mpchecksum::AddScriptStorage(GetPrivateProfileIntA("script_storage", "enabled", 1, ini.c_str()) != 0,
                                GetPrivateProfileIntA("script_storage", "multiplayer", 0, ini.c_str()) != 0, settings);
    g_mpText = mpchecksum::Fingerprint(settings);
    if (g_mpText.empty()) {
        Log(LogLevel::Info, "multiplayer", "no setting that changes the game's results is on; the game's checksum is its own");
        return;
    }
    if (GetPrivateProfileIntA("multiplayer", "settings_in_checksum", 1, ini.c_str()) == 0) {
        Log(LogLevel::Warn, "multiplayer", "settings_in_checksum=0; the game's checksum is its own although these are on: %s. "
            "Players whose settings differ can join each other and may go out of sync",
            g_mpText.c_str());
        return;
    }
    std::string error;
    if (!mpchecksum::Locate(img, g_mpChecksum, error)) {
        Log(LogLevel::Error, "multiplayer", "%s; the game's checksum is its own although these are on: %s", error.c_str(), g_mpText.c_str());
        return;
    }
    auto tramp = InstallJumpHook(g_mpChecksum.finalize, mpchecksum::kFinalizeStolenBytes,
                                 reinterpret_cast<void*>(&ChecksumFinalizeHook));
    if (!tramp) {
        Log(LogLevel::Error, "multiplayer", "failed to hook CChecksum::Finalize; the game's checksum is its own although these are on: %s",
            g_mpText.c_str());
        return;
    }
    g_origChecksumFinalize = reinterpret_cast<ChecksumFinalizeFn>(tramp);
    Log(LogLevel::Info, "multiplayer", "Finalize +%llX, MD5::update +%llX, MD5 +0x%zx; the game's checksum will include the settings that are on",
        static_cast<unsigned long long>(g_mpChecksum.finalize - reinterpret_cast<uint8_t*>(g_base)),
        static_cast<unsigned long long>(g_mpChecksum.md5Update - reinterpret_cast<uint8_t*>(g_base)), g_mpChecksum.md5);
}

void Install(HMODULE self) {
    QueryPerformanceFrequency(&g_qpcFreq);
    Log(LogLevel::Info, "startup", "stellaris-engine-fixes loaded, stellaris.exe base=%p", reinterpret_cast<void*>(g_base));
    LONGLONG t0 = Now();
    locate::Image img = locate::Describe(reinterpret_cast<uint8_t*>(g_base));
    alloc::Install(img, &Log);  // first: before the game's CRT allocates anything
    std::string error;
    if (!locate::LocateStrings(img, g_str, error)) {
        Log(LogLevel::Error, "startup", "%s; this stellaris.exe differs from the versions stellaris-engine-fixes knows, nothing is hooked", error.c_str());
        return;
    }
    InstallFleets(img);
    InstallMegastructures(img);
    InstallBench(self, img);
    InstallAiFixes(self, img);  // after InstallBench: the AI schedule reads the date it finds
    InstallAiFleetTrace(self, img);
    InstallScriptStorage(self, img);
    InstallIntel(self, img);
    InstallCombat(self, img);
    InstallMegaAfford(self, img);
    InstallIncomeProduction(self, img);
    InstallIncomeArithmetic(self, img);
    InstallIncomeTarget(self, img);
    InstallResourceLedger(self, img);
    InstallResourceRounding(self, img);
    InstallDiplomacyArithmetic(self, img);
    InstallDiplomaticDisplay(self, img);
    InstallFixedPointConversion(self, img);
    InstallFleetStats(self, img);
    InstallFleetPowerArithmetic(self, img);
    InstallResourceMultiplication(self, img);
    InstallNumericDisplay(self, img);
    InstallCountryTotals(self, img);
    InstallDiplomaticVotes(self, img);
    InstallFederationSums(self, img);
    InstallDiplomaticScores(self, img);
    InstallShipFormula(self, img);
    InstallDesignCache(self, img);
    InstallShipDamage(self, img);
    InstallShipStats(self, img);
    InstallComponentDamage(self, img);
    InstallForecastArithmetic(self, img);
    InstallLexer(self, img);
    InstallModifierDocs(self, img);
    InstallDeferredRefs(self, img);
    InstallFileLookups(self);
    InstallMpChecksum(self, img);
    InstallTradeAmount(self, img);
    InstallUiCommandShortcuts(self, img);
    Log(LogLevel::Stats, "startup", "setup_ms=%.0f", 1e3 * (Now() - t0) / g_qpcFreq.QuadPart);
    g_lastSummary = GetTickCount64();
    HANDLE thread = CreateThread(nullptr, 0, &SummaryThread, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
    else Log(LogLevel::Warn, "startup", "could not start the periodic stats thread (Windows error %lu)", GetLastError());
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
