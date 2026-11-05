@echo off
rem Builds build\winmm\winmm.dll (install it with stellaris-engine-fixes.ini into the Stellaris
rem folder) with MSVC x64, plus the test programs.
setlocal
rem Visual Studio (Community, Professional, Enterprise or Build Tools) with the C++ x64 tools.
set "VS_DIR="
for /f "usebackq delims=" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_DIR=%%I"
if not defined VS_DIR (
  echo Visual Studio with the C++ x64 tools was not found.
  exit /b 1
)
call "%VS_DIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul || exit /b 1
cd /d "%~dp0"
for %%p in (winmm alloctest) do (
  if not exist build\%%p mkdir build\%%p
)
set MIINC=/I third_party\mimalloc\include
rem mimalloc, as one unit (its own warning level; built as C++ as its MSVC build does).
cl /nologo /std:c++17 /O2 /MT /TP /DNDEBUG %MIINC% /c third_party\mimalloc\src\static.c /Fobuild\mimalloc.obj || exit /b 1
set CLFLAGS=/nologo /std:c++17 /O2 /MT /EHsc /W4 %MIINC%
set LINKFLAGS=/NOLOGO /DEBUG /INCREMENTAL:NO
cl %CLFLAGS% /LD src\dllmain.cpp build\mimalloc.obj /Fobuild\winmm\ /Fdbuild\winmm\ /Fe:build\winmm\winmm.dll /link %LINKFLAGS% /PDB:build\winmm\winmm.pdb advapi32.lib || exit /b 1
copy /y stellaris-engine-fixes.ini build\winmm\stellaris-engine-fixes.ini >nul || exit /b 1
for %%t in (proxy_test natsort_test locate_test pathcache_test income_test) do (
  cl %CLFLAGS% test\%%t.cpp /Fobuild\ /Fe:build\%%t.exe /link /NOLOGO || exit /b 1
)
rem The allocator test: alloc_host.exe imports alloc_testdll.dll, which replaces its heap.
cl %CLFLAGS% /LD test\alloc_testdll.cpp build\mimalloc.obj /Fobuild\alloctest\ /Fe:build\alloctest\alloc_testdll.dll /link %LINKFLAGS% advapi32.lib || exit /b 1
cl %CLFLAGS% test\alloc_host.cpp /Fobuild\alloctest\ /Fe:build\alloctest\alloc_host.exe /link /NOLOGO build\alloctest\alloc_testdll.lib || exit /b 1
copy /y test\alloc_test.ini build\alloctest\stellaris-engine-fixes.ini >nul || exit /b 1
