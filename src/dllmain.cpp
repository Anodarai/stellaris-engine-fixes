// stellaris-engine-fixes: a winmm.dll proxy that fixes and speeds up parts of Stellaris from inside
// the game's process. This file is the runtime: it reads stellaris-engine-fixes.ini, finds the
// game's code and installs every feature's hooks from DllMain, before the game's CRT starts.
//   outliner         fleets and megastructures sorted by name, numbers by value (outliner_runtime.inl,
//                    natsort.h)
//   heap             mimalloc in place of the CRT heap (allocator.h)
//   [speed]          simulation and load-time savings (simulation_runtime.inl, loadtime_runtime.inl),
//                    tick pacing and the console command sef_bench (bench_runtime.inl)
//   [ai_fixes]       gameplay fixes for the country AI (aifixes_runtime.inl, re/ai.md)
//   [numeric_fixes]  wide arithmetic for huge numbers (numericfixes_runtime.inl and the economy/,
//                    fleets/ and diplomacy/ parts it includes)
//   [interface]      trade amounts, the planet queue shortcut, Contacts, view navigation, text selection
//                    (interface_runtime.inl, interface_navigation.inl); starbase templates, the
//                    shipyard checkbox and the ship design library (their *_runtime.inl files)
//   [script_storage] stores for Paradox script (scriptstorage_runtime.inl, pdxstore.h, mpstore.h)
//   [multiplayer]    the result-changing settings in the game's checksum (mpchecksum_runtime.inl,
//                    mpchecksum.h)
// This file keeps the shared helpers (logging, game strings, hooking, settings), includes each
// feature's runtime in the order they depend on, and runs the installers.
// Everything in stellaris.exe is found by code patterns (locate.h and each feature's header); a
// feature whose patterns are not found stays off, the others still work.
#include <windows.h>
#include <psapi.h>

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

#include "performance/allocator.h"
#include "ai/aibossthreat.h"
#include "ai/aifleettrace.h"
#include "ai/aiwarguard.h"
#include "core/game.h"
#include "core/devstats.h"
#include "ai/ai_patterns.h"
#include "ai/incometarget.h"
#include "core/locate.h"
#include "core/logging.h"
#include "storage/mpchecksum.h"
#include "storage/mpstore.h"
#include "performance/natsort.h"
#include "performance/outliner_patterns.h"
#include "performance/pathcache.h"
#include "performance/speed_patterns.h"
#include "performance/tick_patterns.h"
#include "storage/pdxstore.h"
#include "storage/storage_patterns.h"
#include "numeric/economy/resourceoverflow.h"
#include "numeric/fixedpointoverflow.h"
#include "numeric/fleets/fleetoverflow.h"
#include "numeric/economy/resourcearithmetic.h"
#include "numeric/economy/resourceledger.h"
#include "numeric/numericdisplay.h"
#include "numeric/diplomacy/diplomacyoverflow.h"
#include "numeric/fleets/fleetstats.h"
#include "numeric/diplomacy/countrytotals.h"
#include "numeric/economy/forecastoverflow.h"
#include "numeric/economy/incomeoverflow.h"
#include "numeric/diplomacy/diploscores.h"
#include "numeric/fleets/shipformula.h"
#include "numeric/fleets/shipstats.h"
#include "numeric/fleets/componentmath.h"
#include "numeric/fleets/shipdamage.h"
#include "numeric/fleets/designcache.h"
#include "numeric/diplomacy/diplovotes.h"
#include "numeric/economy/incomelifetime.h"
#include "numeric/diplomacy/diplodisplay.h"
#include "numeric/diplomacy/federationsums.h"
#include "interface/uishortcuts.h"
#include "interface/textselect.h"
#include "interface/uinavigation_runtime.h"
#include "interface/starbase/starbasetemplates_reader.h"
#include "interface/starbase/starbasetemplates_commands.h"
#include "interface/starbase/starbasetemplates_ui_patterns.h"
#include "interface/starbase/starbasetemplates_panel.h"
#include "interface/starbase/starbaseautoupgrade.h"
#include "interface/starbase/shipyardreinforce.h"
#include "interface/starbase/shipyardreinforce_patterns.h"
#include "interface/ship_design/shipdesignlibrary.h"
#include "interface/ship_design/shipdesignlibrary_patterns.h"

#pragma comment(lib, "user32.lib")

// Exports of the system DLL we stand in for, forwarded to the real one.
#include "core/exports_winmm.h"

namespace {

// ---------------------------------------------------------------------------
// Logging

FILE* g_log = nullptr;
CRITICAL_SECTION g_logLock;

void Log(LogLevel level, const char* component, const char* fmt, ...) {
    if (!g_log)
        return;
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
    if (n == 0 || n >= MAX_PATH)
        return;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash)
        return;
    constexpr wchar_t filename[] = L"stellaris-engine-fixes.log";
    size_t remaining = MAX_PATH - (slash + 1 - path);
    if (remaining < _countof(filename))
        return;
    wcscpy_s(slash + 1, remaining, filename);
    g_log = _wfsopen(path, L"w", _SH_DENYWR); // others may read it while the game runs
}

// ---------------------------------------------------------------------------
// Timing

LARGE_INTEGER g_qpcFreq;

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

// An address in stellaris.exe as an offset from its base, for the log.
unsigned long long Rva(const void* p) {
    return static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(p) - g_base);
}
locate::StringLocations g_str;

using game::ResolveRef;

// A value of type T at `p + offset`, read without alignment or aliasing assumptions.
template <typename T> T At(const uint8_t* p, size_t offset) {
    T v;
    memcpy(&v, p + offset, sizeof(T));
    return v;
}

// Frees a game string's heap buffer, if it owns one, and leaves it empty.
void FreeGameString(game::String& s) {
    if (s.ownsHeap() && g_str.gameFree)
        reinterpret_cast<game::FreeFn>(g_str.gameFree)(s.ptr);
    memset(&s, 0, sizeof(s));
    s.cap = 0xF;
}

// Copies a game string into `out` and frees it. Returns the length copied.
int TakeString(game::String& s, char* out, size_t outSize) {
    size_t n = s.size < outSize - 1 ? s.size : outSize - 1;
    memcpy(out, s.data(), n);
    out[n] = '\0';
    FreeGameString(s);
    return static_cast<int>(n);
}

// Copies a CString's text into `out` (NUL-terminated, cut to fit); returns its full length.
int32_t CopyGameString(const game::String* text, char* out, int32_t size) {
    size_t length = text->size;
    if (length > 0x7FFFFFFF)
        length = 0;
    if (size > 0) {
        size_t n = length < static_cast<size_t>(size - 1) ? length : static_cast<size_t>(size - 1);
        memcpy(out, text->data(), n);
        out[n] = '\0';
    }
    return static_cast<int32_t>(length);
}

// The game's path, cut to fit; false on a fault. No C++ objects here, for __try.
bool CopySavePath(const game::String* path, char* out, int32_t size) {
    out[0] = '\0';
    __try {
        if (path)
            CopyGameString(path, out, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = '\0';
        return false;
    }
}

// Game dates count hours in 360-day years of 30-day months, from an epoch 43,800,000 hours
// (1,825,000 days) before 0.01.01. Found by comparing ships' dates with their construction_date
// in a save.
std::string FormatDate(int32_t d) {
    if (d == INT_MAX)
        return "no ships";
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
    constexpr size_t kJmpSize = 14; // jmp qword ptr [rip+0]; dq addr
    if (stolen < kJmpSize || stolen > 32)
        return nullptr;
    auto tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp)
        return nullptr;
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
    if (!VirtualProtect(target, stolen, PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        return nullptr;
    }
    memcpy(target, patch, stolen);
    VirtualProtect(target, stolen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, stolen);
    return tramp;
}

// Called once a frame from the tick hook (CheckTickHook); defined later, in
// scriptstorage_runtime.inl, starbasetemplates_runtime.inl and shipdesignlibrary_runtime.inl.
void StoreFrame();
void StarbaseTemplateFrame();
void ShipDesignLibraryFrame();

// Points a vtable slot that holds `expected` at `hook`.
bool SwapVtableSlot(uintptr_t* slot, const uint8_t* expected, void* hook) {
    if (*slot != reinterpret_cast<uintptr_t>(expected))
        return false;
    DWORD old;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old))
        return false;
    *slot = reinterpret_cast<uintptr_t>(hook);
    VirtualProtect(slot, sizeof(*slot), old, &old);
    return true;
}

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
    const uintptr_t range = 0x70000000; // well inside rel32's reach from anywhere in the image
    for (uintptr_t delta = gran; delta < range; delta += gran) {
        for (int side = 0; side < 2; ++side) {
            uintptr_t at = side ? center + delta : center - delta;
            if (side == 0 && center < delta)
                continue;
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(reinterpret_cast<void*>(at), &mbi, sizeof(mbi)) || mbi.State != MEM_FREE)
                continue;
            void* p =
                VirtualAlloc(reinterpret_cast<void*>(at), kBlock, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (!p)
                continue;
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
    if (d < INT32_MIN || d > INT32_MAX)
        return false;
    *out = static_cast<int32_t>(d);
    return true;
}

// Overwrites game code.
bool WriteCode(uint8_t* at, const void* bytes, size_t n) {
    DWORD old;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old))
        return false;
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
        for (uint8_t x : b)
            bytes[n++] = x;
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
    Code& JumpTo(const void* to) { // jmp qword [rip+0]; dq to
        Put({0xFF, 0x25, 0, 0, 0, 0});
        return Put64(reinterpret_cast<uint64_t>(to));
    }
};

uint8_t* PlaceStub(const Code& code, const uint8_t* nearTo) {
    uint8_t* stub =
        nearTo ? AllocNear(nearTo, code.n)
               : static_cast<uint8_t*>(VirtualAlloc(nullptr, code.n, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (stub) {
        memcpy(stub, code.bytes, code.n);
        FlushInstructionCache(GetCurrentProcess(), stub, code.n);
    }
    return stub;
}

// Points the call instruction at `call` (E8 rel32) at `hook`, through a stub near it.
bool RedirectCall(uint8_t* call, const void* hook) {
    Code code;
    code.JumpTo(hook);
    uint8_t* stub = PlaceStub(code, call);
    int32_t rel = 0;
    return stub && call[0] == 0xE8 && Rel32(call + 5, stub, &rel) && WriteCode(call + 1, &rel, sizeof(rel));
}

// ---------------------------------------------------------------------------
// Settings

// stellaris-engine-fixes.ini next to this DLL, or empty.
std::string IniPath(HMODULE self) {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(self, path, MAX_PATH);
    char* slash = n && n < MAX_PATH ? strrchr(path, '\\') : nullptr;
    if (!slash)
        return std::string();
    strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), "stellaris-engine-fixes.ini");
    return path;
}

// A setting's number from stellaris-engine-fixes.ini; `fallback` when the file or the key is missing.
int IniInt(HMODULE self, const char* section, const char* key, int fallback) {
    std::string ini = IniPath(self);
    if (ini.empty())
        return fallback;
    return static_cast<int>(GetPrivateProfileIntA(section, key, fallback, ini.c_str()));
}

// Whether a setting is on (not 0).
bool IniOn(HMODULE self, const char* section, const char* key, bool fallback) {
    return IniInt(self, section, key, fallback ? 1 : 0) != 0;
}

// The same for a feature's own switch: when it is off, the log says so under `component`.
bool IniOnLogged(HMODULE self, const char* section, const char* key, bool fallback, const char* component) {
    if (IniOn(self, section, key, fallback))
        return true;
    Log(LogLevel::Info, component, "off (%s=0)", key);
    return false;
}

// The outliner: fleets and megastructures sorted by name.
#include "performance/outliner_runtime.inl"

// [speed]: savings inside the simulation, then while the game loads.
#include "performance/simulation_runtime.inl"
#include "performance/loadtime_runtime.inl"

// Tick pacing and the console command sef_bench.
#include "performance/bench_runtime.inl"

// The numeric fixes ([numeric_fixes] in stellaris-engine-fixes.ini; re/numeric_overflow.md).
#include "numeric/numericfixes_runtime.inl"

// The AI fixes ([ai_fixes]; re/ai.md), the AI fleet trace and the income target shortcut.
#include "ai/aifixes_runtime.inl"

// Script storage: the "@sef:" commands, the stores and their multiplayer copies.
#include "storage/scriptstorage_runtime.inl"

// The interface additions ([interface]): trade amounts, the planet queue shortcut, Contacts.
#include "interface/interface_runtime.inl"

// Ctrl+A and mouse selection in the game's edit boxes.
#include "interface/textselect_runtime.inl"

// Starbase templates, auto-upgrade and the shipyard checkbox (re/starbase_templates.md), then
// the ship design library (re/ship_design_library.md).
#include "interface/starbase/starbasetemplates_runtime.inl"
#include "interface/ship_design/shipdesignlibrary_runtime.inl"

// View navigation, and the GUI reload hook that also attaches the starbase and designer controls.
#include "interface/interface_navigation.inl"

#include "interface/starbase/starbasetemplates_install.inl"

// ===========================================================================
// Statistics and start-up

std::atomic<ULONGLONG> g_lastSummary{0};

// The 30-second statistics. Only a dev build (build.bat dev, SEF_DEV_STATS) logs them.
void MaybeLogSummary() {
#ifdef SEF_DEV_STATS
    ULONGLONG now = GetTickCount64();
    ULONGLONG last = g_lastSummary.load(std::memory_order_relaxed);
    if (now - last < 30000 || !g_lastSummary.compare_exchange_strong(last, now))
        return;
    LogOutlinerStats();
    if (auto heap = alloc::Stats(); !heap.empty())
        Log(LogLevel::Stats, "allocator", "%s", heap.c_str());
    LogIntelStats();
    LogCombatStats();
    LogAffordStats();
    LogIncomeStats();
    LogLexerStats();
    LogDeferredStats();
    LogFileLookupStats();
    LogStoreStats();
    LogStockpileStats();
    LogAiFixStats();
    LogStarbasePerf();
#endif
}

// True when the host process is stellaris.exe.
bool IsStellaris() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return false;
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

// The multiplayer checksum: result-changing settings in the game's checksum.
#include "storage/mpchecksum_runtime.inl"

void Install(HMODULE self) {
    QueryPerformanceFrequency(&g_qpcFreq);
    Log(LogLevel::Info, "startup", "stellaris-engine-fixes loaded, stellaris.exe base=%p",
        reinterpret_cast<void*>(g_base));
    LONGLONG t0 = Now();
    locate::Image img = locate::Describe(reinterpret_cast<uint8_t*>(g_base));
    alloc::Install(img, &Log); // first: before the game's CRT allocates anything
    std::string error;
    if (!locate::LocateStrings(img, g_str, error)) {
        Log(LogLevel::Error, "startup",
            "%s; this stellaris.exe differs from the versions stellaris-engine-fixes knows, nothing is hooked",
            error.c_str());
        return;
    }
    // The order is the one the features have always been installed in; the log follows it.
    // Outliner sorting.
    InstallFleets(img);
    InstallMegastructures(img);
    // Tick pacing and sef_bench, then what uses the tick and date fields they find.
    InstallBench(self, img);
    InstallAiFixes(self, img); // after InstallBench: the AI schedule reads the date it finds
    InstallAiFleetTrace(self, img);
    InstallScriptStorage(self, img);
    // [speed] inside the simulation.
    InstallIntel(self, img);
    InstallCombat(self, img);
    InstallMegaAfford(self, img);
    // [numeric_fixes], with the AI's income target shortcut among them.
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
    // [speed] while the game loads.
    InstallLexer(self, img);
    InstallModifierDocs(self, img);
    InstallDeferredRefs(self, img);
    InstallFileLookups(self);
    // [multiplayer]: reads the settings above from the ini, not their results.
    InstallMpChecksum(self, img);
    // [interface].
    InstallContactsScroll(self, img);
    InstallViewNavigation(self, img);
    InstallTradeAmount(self, img);
    InstallTextSelection(self, img);
    InstallStarbaseTemplates(self, img);
    InstallShipDesignLibrary(self, img);
    InstallUiCommandShortcuts(self, img);
    Log(LogLevel::Stats, "startup", "setup_ms=%.0f", 1e3 * (Now() - t0) / g_qpcFreq.QuadPart);
    g_lastSummary = GetTickCount64();
#ifdef SEF_DEV_STATS
    Log(LogLevel::Info, "startup", "dev build: statistics are collected and logged every 30 s");
    HANDLE thread = CreateThread(nullptr, 0, &SummaryThread, nullptr, 0, nullptr);
    if (thread)
        CloseHandle(thread);
    else
        Log(LogLevel::Warn, "startup", "could not start the periodic stats thread (Windows error %lu)", GetLastError());
#endif
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if (!IsStellaris())
            return TRUE; // other programs in the game folder: plain proxy
        OpenLog(module);
        Install(module);
    } else if (reason == DLL_PROCESS_DETACH) {
        StoreExit();
    }
    return TRUE;
}
