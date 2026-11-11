// Loads the WinMM proxy in a non-Stellaris process and checks a forwarded export.
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

// Map the game without loading it or resolving its own dependencies. Check the
// actual WinMM import names and ordinals against the built proxy's exports.
static bool CheckGameImports(const char* path) {
    HMODULE game = LoadLibraryExA(path, nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!game) {
        printf("FAIL: cannot map %s (error %lu)\n", path, GetLastError());
        return false;
    }
    auto base = reinterpret_cast<const BYTE*>(reinterpret_cast<ULONG_PTR>(game) & ~ULONG_PTR(3));
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        puts("FAIL: expected an x64 Windows executable");
        return false;
    }

    HMODULE proxy = LoadLibraryW(L"build\\winmm\\winmm.dll");
    if (!proxy) {
        printf("FAIL: cannot load the built WinMM proxy (error %lu)\n", GetLastError());
        return false;
    }
    DWORD imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!imports) {
        puts("FAIL: no static import table");
        return false;
    }

    unsigned checked = 0;
    bool ok = true;
    auto descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + imports);
    for (; descriptor->Name; ++descriptor) {
        if (lstrcmpiA(reinterpret_cast<const char*>(base + descriptor->Name), "winmm.dll") != 0) continue;
        DWORD lookup = descriptor->OriginalFirstThunk ? descriptor->OriginalFirstThunk : descriptor->FirstThunk;
        auto thunk = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + lookup);
        for (; thunk->u1.AddressOfData; ++thunk) {
            ++checked;
            ULONGLONG entry = thunk->u1.AddressOfData;
            if (IMAGE_SNAP_BY_ORDINAL64(entry)) {
                WORD ordinal = static_cast<WORD>(IMAGE_ORDINAL64(entry));
                if (GetProcAddress(proxy, MAKEINTRESOURCEA(ordinal))) continue;
                printf("FAIL: WinMM ordinal %u does not resolve through the proxy\n", unsigned(ordinal));
            } else {
                auto name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + entry)->Name;
                if (GetProcAddress(proxy, name)) continue;
                printf("FAIL: WinMM import %s does not resolve through the proxy\n", name);
            }
            ok = false;
        }
    }
    if (!checked) {
        puts("FAIL: no static WinMM imports");
        return false;
    }
    printf("%s: checked %u WinMM imports\n", ok ? "PASS" : "FAIL", checked);
    return ok;
}

int main(int argc, char** argv) {
    if (argc == 2) return CheckGameImports(argv[1]) ? 0 : 1;
    if (argc != 1) {
        puts("usage: proxy_test [<stellaris.exe>]");
        return 2;
    }
    bool ok = Check(L"build\\winmm\\winmm.dll", "timeGetTime", [](FARPROC fn) {
        auto now = reinterpret_cast<DWORD(WINAPI*)()>(fn);
        return now() > 0;
    });
    return ok ? 0 : 1;
}
