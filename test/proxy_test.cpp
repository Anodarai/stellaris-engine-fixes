// Loads the proxy in a non-Stellaris process and calls a forwarded export of each.
// Expect: the DLLs load, do nothing (host is not stellaris.exe), and forwarding works.
#include <windows.h>

#include <cstdio>

static bool Check(const wchar_t* path, const char* exportName, bool (*call)(FARPROC)) {
    HMODULE dll = LoadLibraryW(path);
    if (!dll) {
        wprintf(L"FAIL: LoadLibrary(%ls) error %lu\n", path, GetLastError());
        return false;
    }
    FARPROC fn = GetProcAddress(dll, exportName);
    bool ok = fn && call(fn);
    wprintf(L"%ls: %ls %hs\n", ok ? L"PASS" : L"FAIL", path, exportName);
    return ok;
}

int main() {
    bool ok = Check(L"build\\winmm\\winmm.dll", "timeGetTime", [](FARPROC fn) {
        auto now = reinterpret_cast<DWORD(WINAPI*)()>(fn);
        return now() > 0;
    });
    return ok ? 0 : 1;
}
