// Statically imports VERSION.dll (like stellaris.exe) and reports which copy the loader picked.
// Run from build\ next to the proxy version.dll.
#include <windows.h>

#include <cstdio>

#pragma comment(lib, "version.lib")

int main() {
    DWORD handle = 0;
    GetFileVersionInfoSizeW(L"C:\\Windows\\System32\\kernel32.dll", &handle);  // forces the import
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(GetModuleHandleW(L"version.dll"), path, MAX_PATH);
    wprintf(L"VERSION.dll loaded from: %ls\n", path);
    return 0;
}
