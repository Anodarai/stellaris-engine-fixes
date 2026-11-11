// Statically imports the same system DLLs as stellaris.exe, then reports which VERSION.dll
// the loader picked. Run from build\ next to the proxy version.dll.
#include <winsock2.h>
#include <windows.h>
#include <d3d11.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <gl/GL.h>
#include <imm.h>
#include <setupapi.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <wininet.h>

#include <cstdio>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "wininet.lib")

// Referencing one function per DLL keeps every import in the import table.
volatile const void* g_keep[] = {
    &SetupDiGetClassDevsW, &D3DCompile,    &D3D11CreateDevice, &CreateDXGIFactory, &Direct3DCreate9,
    &glGetError,           &GetDC,         &CreateSolidBrush,  &ShellExecuteW,     &CoInitialize,
    &SysAllocString,       &RegCloseKey,   &BCryptGenRandom,   &timeGetTime,       &WSAStartup,
    &PathFileExistsW,      &ImmGetContext, &GetFileVersionInfoSizeW, &InternetOpenW,
};

int main() {
    const wchar_t* names[] = {L"version.dll", L"winmm.dll"};
    for (const wchar_t* name : names) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(GetModuleHandleW(name), path, MAX_PATH);
        wprintf(L"%ls loaded from: %ls\n", name, path);
    }
    wprintf(L"(%zu imports kept)\n", sizeof(g_keep) / sizeof(g_keep[0]));
    return 0;
}
