// allocator.h in a DLL that alloc_host.exe imports: it replaces alloc_host.exe's CRT heap the
// way winmm.dll replaces stellaris.exe's, and exports what alloc_host checks.
#define SEF_HOST_EXE L"alloc_host.exe"
#include <cstdarg>

#include "../src/allocator.h"

static std::string g_text;

static void TestLog(LogLevel level, const char* component, const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    g_text += LogLevelName(level);
    g_text += ' ';
    g_text += component;
    g_text += " | ";
    g_text += buf;
    g_text += '\n';
}

extern "C" __declspec(dllexport) const char* AllocTestLog() { return g_text.c_str(); }
extern "C" __declspec(dllexport) bool AllocTestInstalled() { return alloc::g_status.installed; }
extern "C" __declspec(dllexport) bool AllocTestOwned(const void* p) { return mi_is_in_heap_region(p); }
extern "C" __declspec(dllexport) long AllocTestPurgeDelay() { return mi_option_get(mi_option_purge_delay); }
extern "C" __declspec(dllexport) unsigned long long AllocTestForeign() {
    return alloc::g_foreignFrees + alloc::g_foreignReallocs + alloc::g_foreignSizes;
}
extern "C" __declspec(dllexport) const char* AllocTestSummary() {
    static std::string s;
    s = alloc::Stats();
    return s.c_str();
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        locate::Image img = locate::Describe(reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr)));
        alloc::Install(img, &TestLog);
    }
    return TRUE;
}
