@echo off
rem Builds with MSVC x64:
rem   build\winmm\winmm.dll      the mod; install this one, with stellaris-engine-fixes.ini
rem   the test programs          run them all with python tools\validate_builds.py
rem Usage:
rem   build.bat       release: no statistics are collected or logged (src\core\devstats.h)
rem   build.bat dev   dev build: counters, timings and the 30-second summaries in the log
setlocal
set STATS=
if /i "%~1"=="dev" set STATS=/DSEF_DEV_STATS
if not "%~1"=="" if /i not "%~1"=="dev" if /i not "%~1"=="release" (echo usage: build.bat [dev^|release] & exit /b 1)
rem Visual Studio (Community, Professional, Enterprise or Build Tools) with the C++ x64 tools.
set "VS_DIR="
for /f "usebackq delims=" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_DIR=%%I"
if not defined VS_DIR (
  echo Visual Studio with the C++ x64 tools was not found.
  exit /b 1
)
call "%VS_DIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul || exit /b 1
cd /d "%~dp0"
for %%p in (winmm alloctest numeric-install-test mpstore-runtime-test starbase-install-test) do (
  if not exist build\%%p mkdir build\%%p
)
set MIINC=/I third_party\mimalloc\include
rem mimalloc, as one unit (its own warning level; built as C++ as its MSVC build does).
cl /nologo /std:c++17 /O2 /MT /TP /DNDEBUG %MIINC% /c third_party\mimalloc\src\static.c /Fobuild\mimalloc.obj || exit /b 1
set CLFLAGS=/nologo /std:c++17 /O2 /MT /EHsc /W4 %STATS% %MIINC%
set LINKFLAGS=/NOLOGO /DEBUG /INCREMENTAL:NO
cl %CLFLAGS% /LD src\dllmain.cpp build\mimalloc.obj /Fobuild\winmm\ /Fdbuild\winmm\ /Fe:build\winmm\winmm.dll /link %LINKFLAGS% /PDB:build\winmm\winmm.pdb advapi32.lib || exit /b 1
copy /y stellaris-engine-fixes.ini build\winmm\stellaris-engine-fixes.ini >nul || exit /b 1
rem The standalone tests. static_import_test is a manual loader probe and is not built; the three
rem tests that link mimalloc are built below.
for %%t in (test\*_test.cpp) do (
  if /i not "%%~nt"=="static_import_test" if /i not "%%~nt"=="numeric_install_test" if /i not "%%~nt"=="mpstore_runtime_test" if /i not "%%~nt"=="starbasetemplates_install_test" (
    cl %CLFLAGS% "%%t" /Fobuild\ /Fe:build\%%~nt.exe /link /NOLOGO || exit /b 1
  )
)
rem The tests that install hooks into their own process; they link mimalloc as the DLL does.
cl %CLFLAGS% test\numeric_install_test.cpp build\mimalloc.obj /Fobuild\numeric-install-test\ /Fe:build\numeric-install-test\numeric_install_test.exe /link /NOLOGO advapi32.lib || exit /b 1
cl %CLFLAGS% test\mpstore_runtime_test.cpp build\mimalloc.obj /Fobuild\mpstore-runtime-test\ /Fe:build\mpstore-runtime-test\mpstore_runtime_test.exe /link /NOLOGO advapi32.lib || exit /b 1
cl %CLFLAGS% test\starbasetemplates_install_test.cpp build\mimalloc.obj /Fobuild\starbase-install-test\ /Fe:build\starbase-install-test\starbasetemplates_install_test.exe /link /NOLOGO advapi32.lib || exit /b 1
rem The allocator test: alloc_host.exe imports alloc_testdll.dll, which replaces its heap.
cl %CLFLAGS% /LD test\alloc_testdll.cpp build\mimalloc.obj /Fobuild\alloctest\ /Fe:build\alloctest\alloc_testdll.dll /link %LINKFLAGS% advapi32.lib || exit /b 1
cl %CLFLAGS% test\alloc_host.cpp /Fobuild\alloctest\ /Fe:build\alloctest\alloc_host.exe /link /NOLOGO build\alloctest\alloc_testdll.lib || exit /b 1
copy /y test\alloc_test.ini build\alloctest\stellaris-engine-fixes.ini >nul || exit /b 1
