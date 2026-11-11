// Replaces the host exe's CRT heap with mimalloc (third_party/mimalloc, linked in statically).
//
// The exe's statically linked CRT allocates through five functions (locate.h, CrtHeapLocations);
// each one's entry is overwritten with a jump to a replacement here. This must happen before
// the exe's CRT starts, i.e. in DllMain of a DLL the exe imports: from then on every block the
// exe allocates comes from mimalloc. Blocks the replacements do not recognize as mimalloc's
// (memory another module allocated on the process heap and the exe frees, as the original CRT
// allowed) go to the process heap, as before.
//
// Settings: the [mimalloc] section of stellaris-engine-fixes.ini next to this DLL.
//   enabled=0|1                 replace the heap (default 1)
//   reserve_huge_os_pages=N     reserve N GiB of 1 GiB pages at startup (needs "Lock pages in memory")
//   <option>=<value>            any other mimalloc option, set as MIMALLOC_<OPTION> (unless that
//                               variable is already set) for mimalloc to read when it starts
// mimalloc starts in this DLL's TLS callback, before DllMain, so the ini is read by a TLS callback
// that runs just before it. The variables are removed again once mimalloc has read them: other
// copies of mimalloc in the process (the NVIDIA driver has one) would read them too.
#pragma once
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include <mimalloc.h>
#include "logging.h"

#include "locate.h"

#ifndef SEF_HOST_EXE
#define SEF_HOST_EXE L"stellaris.exe"
#endif

namespace alloc {

// --- Settings, read before mimalloc starts ---------------------------------------------------
// Runs in a TLS callback before the CRT of this DLL is initialized: kernel32 only, static storage.

// All zero by default: g_settings is zero-initialized storage, valid before any initializer runs.
struct Settings {
    bool hostMatches;  // the process is SEF_HOST_EXE
    bool fileFound;
    bool disabled;
    long hugePages;
    int setCount, skippedCount, badCount;
    wchar_t set[24][64];      // variables this DLL set (removed after mimalloc read them)
    wchar_t skipped[24][64];  // already set in the environment, left alone
};
Settings g_settings;

inline bool IsSpace(wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r'; }

inline bool KeyIs(const wchar_t* key, const wchar_t* want) {
    for (; *key && *want; ++key, ++want) {
        wchar_t c = (*key >= L'A' && *key <= L'Z') ? static_cast<wchar_t>(*key + 32) : *key;
        if (c != *want) return false;  // `want` is lower case
    }
    return *key == *want;
}

inline long ParseLong(const wchar_t* s) {
    long v = 0;
    for (; *s >= L'0' && *s <= L'9'; ++s) v = v * 10 + (*s - L'0');
    return v;
}

// One `key=value` line of the [mimalloc] section (both trimmed, NUL-terminated).
inline void ApplySetting(Settings& st, const wchar_t* key, const wchar_t* value) {
    if (KeyIs(key, L"enabled")) {
        st.disabled = ParseLong(value) == 0;
        return;
    }
    if (KeyIs(key, L"reserve_huge_os_pages")) {
        st.hugePages = ParseLong(value);
        return;
    }
    wchar_t name[64] = L"MIMALLOC_";
    int n = 9;
    for (const wchar_t* k = key; *k; ++k) {
        wchar_t c = *k;
        bool ok = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'_';
        if (!ok || n >= 63) {
            ++st.badCount;
            return;
        }
        name[n++] = (c >= L'a' && c <= L'z') ? static_cast<wchar_t>(c - 32) : c;
    }
    name[n] = 0;
    bool already = GetEnvironmentVariableW(name, nullptr, 0) != 0;
    wchar_t(*list)[64] = already ? st.skipped : st.set;
    int& count = already ? st.skippedCount : st.setCount;
    if (count >= 24) return;
    if (!already && !SetEnvironmentVariableW(name, value)) return;
    lstrcpynW(list[count++], name, 64);
}

inline void ReadSettings(HMODULE self) {
    Settings& st = g_settings;
    static wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    const wchar_t* exe = path;
    for (const wchar_t* p = path; *p; ++p) {
        if (*p == L'\\') exe = p + 1;
    }
    if (lstrcmpiW(exe, SEF_HOST_EXE) != 0) return;
    st.hostMatches = true;

    n = GetModuleFileNameW(self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    wchar_t* slash = path;
    for (wchar_t* p = path; *p; ++p) {
        if (*p == L'\\') slash = p + 1;
    }
    if (lstrcpynW(slash, L"stellaris-engine-fixes.ini", MAX_PATH - static_cast<int>(slash - path)) == nullptr) return;

    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    static char raw[16384];
    DWORD got = 0;
    BOOL ok = ReadFile(f, raw, sizeof(raw) - 1, &got, nullptr);
    CloseHandle(f);
    if (!ok) return;
    st.fileFound = true;

    // Line by line; settings are ASCII, values are copied as bytes.
    static wchar_t line[512];
    bool inSection = false;
    for (DWORD i = 0; i < got;) {
        int len = 0;
        for (; i < got && raw[i] != '\n'; ++i) {
            if (len < 511) line[len++] = static_cast<unsigned char>(raw[i]);
        }
        ++i;
        line[len] = 0;
        wchar_t* s = line;
        while (IsSpace(*s)) ++s;
        wchar_t* end = s + lstrlenW(s);
        while (end > s && IsSpace(end[-1])) *--end = 0;
        if (*s == 0 || *s == L';' || *s == L'#') continue;
        if (*s == L'[') {
            inSection = KeyIs(s, L"[mimalloc]");
            continue;
        }
        if (!inSection) continue;
        wchar_t* eq = s;
        while (*eq && *eq != L'=') ++eq;
        if (*eq != L'=') {
            ++st.badCount;
            continue;
        }
        wchar_t* keyEnd = eq;
        while (keyEnd > s && IsSpace(keyEnd[-1])) --keyEnd;
        *keyEnd = 0;
        wchar_t* value = eq + 1;
        while (IsSpace(*value)) ++value;
        ApplySetting(st, s, value);
    }
    if (st.disabled) {  // off: mimalloc stays unused, so its options do not matter
        for (int k = 0; k < st.setCount; ++k) SetEnvironmentVariableW(st.set[k], nullptr);
        st.setCount = 0;
    }
}

inline void NTAPI ReadSettingsCallback(PVOID module, DWORD reason, PVOID) {
    if (reason == DLL_PROCESS_ATTACH) ReadSettings(static_cast<HMODULE>(module));
}

}  // namespace alloc

// TLS callbacks run in the order of their sections' names: .CRT$XLAA sorts after the CRT's
// start marker (.CRT$XLA) and before mimalloc's process attach (.CRT$XLB).
extern "C" {
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:sef_read_settings")
#pragma const_seg(".CRT$XLAA")
extern const PIMAGE_TLS_CALLBACK sef_read_settings[];
const PIMAGE_TLS_CALLBACK sef_read_settings[] = {&alloc::ReadSettingsCallback};
#pragma const_seg()
}

namespace alloc {

// --- Replacements ----------------------------------------------------------------------------

std::atomic<uint64_t> g_foreignFrees{0}, g_foreignReallocs{0}, g_foreignSizes{0};

inline bool Owned(void* p) { return mi_is_in_heap_region(p); }

void* __cdecl Malloc(size_t size) { return mi_malloc(size); }

void* __cdecl Calloc(size_t count, size_t size) { return mi_calloc(count, size); }

void __cdecl Free(void* p) {
    if (!p) return;
    if (Owned(p)) {
        mi_free(p);
    } else {
        ++g_foreignFrees;
        HeapFree(GetProcessHeap(), 0, p);
    }
}

// As _realloc_base: a null block allocates, size 0 frees and returns null.
void* __cdecl Realloc(void* p, size_t size) {
    if (!p) return mi_malloc(size);
    if (size == 0) {
        Free(p);
        return nullptr;
    }
    if (Owned(p)) return mi_realloc(p, size);
    ++g_foreignReallocs;
    return HeapReAlloc(GetProcessHeap(), 0, p, size);
}

size_t __cdecl Msize(void* p) {
    if (!p) return static_cast<size_t>(-1);
    if (Owned(p)) return mi_usable_size(p);
    ++g_foreignSizes;
    return HeapSize(GetProcessHeap(), 0, p);
}

// --- Install ---------------------------------------------------------------------------------


struct Status {
    bool installed = false;
    long hugePagesReserved = 0;
    locate::CrtHeapLocations crt;
};
Status g_status;

// Overwrites the first 14 bytes of `target` with `jmp [rip+0]; dq to`.
inline bool WriteJump(uint8_t* target, void* to) {
    uint8_t patch[14] = {0xFF, 0x25, 0, 0, 0, 0};
    uintptr_t dest = reinterpret_cast<uintptr_t>(to);
    memcpy(patch + 6, &dest, 8);
    DWORD old;
    if (!VirtualProtect(target, sizeof(patch), PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(target, patch, sizeof(patch));
    VirtualProtect(target, sizeof(patch), old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(patch));
    return true;
}

LogFn g_log = nullptr;

inline void MimallocOutput(const char* msg, void*) {
    if (!g_log) return;
    // The live callback receives the prefix and message separately. Startup
    // diagnostics are replayed together in a single buffer.
    static thread_local LogLevel pending = LogLevel::Debug;
    constexpr std::string_view prefix = "mimalloc: ";
    std::string_view remaining(msg);
    while (!remaining.empty()) {
        size_t newline = remaining.find('\n');
        auto line = remaining.substr(0, newline);
        remaining.remove_prefix(newline == remaining.npos ? remaining.size() : newline + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        LogLevel level = pending;
        pending = LogLevel::Debug;
        if (line.substr(0, prefix.size()) == prefix) {
            line.remove_prefix(prefix.size());
            level = LogLevel::Debug;
            if (line.substr(0, 7) == "error: ") {
                line.remove_prefix(7);
                level = LogLevel::Error;
            } else if (line.substr(0, 9) == "warning: ") {
                line.remove_prefix(9);
                level = LogLevel::Warn;
            } else if (line.substr(0, 17) == "assertion failed:") {
                level = LogLevel::Error;
            }
            // Worker prefixes may end in "thread 0x...: ".
            if (newline == remaining.npos &&
                (line.empty() || (line.substr(0, 9) == "thread 0x" && line.substr(line.size() - 2) == ": "))) {
                pending = level;
                continue;
            }
        }
        if (!line.empty()) g_log(level, "mimalloc", "%.*s", static_cast<int>(line.size()), line.data());
    }
}

inline std::string Narrow(const wchar_t* w) {
    std::string s;
    for (; *w; ++w) s.push_back(static_cast<char>(*w));
    return s;
}

// Removes the variables ReadSettings set, once mimalloc has read them.
inline void ClearEnvironment() {
    for (int i = 0; i < g_settings.setCount; ++i) SetEnvironmentVariableW(g_settings.set[i], nullptr);
}

// Call from DllMain, before the exe's CRT starts. Logs what it does through `log`.
inline bool Install(const locate::Image& img, LogFn log) {
    g_log = log;
    mi_register_output(&MimallocOutput, nullptr);  // also prints what mimalloc said while starting
    const Settings& st = g_settings;
    if (!st.hostMatches) {
        log(LogLevel::Debug, "allocator", "host does not match %s", Narrow(SEF_HOST_EXE).c_str());
        return false;
    }
    std::string set, skipped;
    for (int i = 0; i < st.setCount; ++i) set += " " + Narrow(st.set[i]);
    for (int i = 0; i < st.skippedCount; ++i) skipped += " " + Narrow(st.skipped[i]);
    log(st.badCount ? LogLevel::Warn : LogLevel::Info, "allocator",
        "stellaris-engine-fixes.ini %s; mimalloc %s, reserve_huge_os_pages=%ld%s%s%s%s%s", st.fileFound ? "read" : "not found",
        st.disabled ? "off" : "on", st.hugePages, set.empty() ? "" : "; set", set.c_str(),
        skipped.empty() ? "" : "; already in the environment, kept:", skipped.c_str(),
        st.badCount ? "; some lines were not understood" : "");
    ClearEnvironment();
    if (st.disabled) return false;

    std::string error;
    if (!locate::LocateCrtHeap(img, g_status.crt, error)) {
        log(LogLevel::Error, "allocator", "%s; the game keeps its own heap", error.c_str());
        return false;
    }
    if (*g_status.crt.heap != nullptr) {
        log(LogLevel::Error, "allocator", "the game's CRT has already started; the game keeps its own heap");
        return false;
    }
    if (st.hugePages > 0) {
        // A full reservation or a part of one both return 0, so count what was committed.
        size_t before = 0, after = 0;
        LONGLONG t0 = GetTickCount64();
        mi_process_info(nullptr, nullptr, nullptr, nullptr, nullptr, &before, nullptr, nullptr);
        int err = mi_reserve_huge_os_pages_interleave(static_cast<size_t>(st.hugePages), 0,
                                                      static_cast<size_t>(st.hugePages) * 500);
        mi_process_info(nullptr, nullptr, nullptr, nullptr, nullptr, &after, nullptr, nullptr);
        g_status.hugePagesReserved = static_cast<long>((after - before + (size_t(1) << 29)) >> 30);
        log(LogLevel::Stats, "allocator", "huge_pages_requested_gib=%ld huge_pages_reserved_gib=%ld reservation_ms=%llu",
            st.hugePages, g_status.hugePagesReserved, GetTickCount64() - t0);
        if (err != 0 || g_status.hugePagesReserved < st.hugePages) {
            log(LogLevel::Warn, "allocator", "reserved %ld of %ld GiB of huge pages (mimalloc result %d)",
                g_status.hugePagesReserved, st.hugePages, err);
        }
    }
    const locate::CrtHeapLocations& c = g_status.crt;
    struct {
        uint8_t* at;
        void* to;
    } hooks[] = {{c.malloc, reinterpret_cast<void*>(&Malloc)},   {c.calloc, reinterpret_cast<void*>(&Calloc)},
                 {c.realloc, reinterpret_cast<void*>(&Realloc)}, {c.free, reinterpret_cast<void*>(&Free)},
                 {c.msize, reinterpret_cast<void*>(&Msize)}};
    for (auto& h : hooks) {
        if (!WriteJump(h.at, h.to)) {
            // Nothing has been allocated yet, so the functions already patched are harmless only
            // if every one is: stop the process from starting half-switched.
            log(LogLevel::Error, "allocator", "failed to patch %p (error %lu); exiting", h.at, GetLastError());
            ExitProcess(1);
        }
    }
    g_status.installed = true;
    log(LogLevel::Info, "allocator", "mimalloc %d.%d.%d replaces the game's heap", MI_MALLOC_VERSION / 10000,
        MI_MALLOC_VERSION / 100 % 100, MI_MALLOC_VERSION % 100);
    return true;
}

// One summary line, or empty when not installed.
inline std::string Stats() {
    if (!g_status.installed) return {};
    size_t elapsed, user, sys, rss, peakRss, commit, peakCommit, faults;
    mi_process_info(&elapsed, &user, &sys, &rss, &peakRss, &commit, &peakCommit, &faults);
    char buf[320];
    sprintf_s(buf, "commit_mib=%zu peak_commit_mib=%zu working_set_mib=%zu peak_working_set_mib=%zu huge_pages_gib=%ld "
              "foreign_frees=%llu foreign_reallocs=%llu foreign_sizes=%llu",
              commit >> 20, peakCommit >> 20, rss >> 20, peakRss >> 20, g_status.hugePagesReserved,
              static_cast<unsigned long long>(g_foreignFrees.load()),
              static_cast<unsigned long long>(g_foreignReallocs.load()),
              static_cast<unsigned long long>(g_foreignSizes.load()));
    return buf;
}

}  // namespace alloc
