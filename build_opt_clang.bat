@echo off
REM Build the KenshiZoneOpt.dll DEV+PROD pair with clang-cl and lld-link
REM against the VS 2010 contract (VS 2010 headers, msvcr100/msvcp100, Windows
REM SDK 7.0A, all from vcvarsall amd64). Additive: the shipped pair is still
REM build_opt_step4.bat's MSVC build, and nothing here writes its folders.
REM
REM Usage:
REM   build_opt_clang.bat
REM       Headers come from KENSHILIB_HEADERS, else the pinned tree named by
REM       dependency-baseline.json's clang_headers block (tools\build\clang_env.bat);
REM       tools\kenshilib\check_headers_pin.py refuses any other tree. Libraries
REM       always come from KENSHILIB, which must hold
REM       Libraries\KenshiLib\KenshiLib.lib (build_env.bat refuses a tree
REM       without it), so a header-only tree goes in KENSHILIB_HEADERS, never
REM       in KENSHILIB.
REM
REM   ZONEHAND_STEP: from the environment, else 3 (as build_opt_step4.bat).
REM   LLVM_BIN: the LLVM bin folder, else C:\Program Files\LLVM\bin.
REM   BUILD_MP: clang-cl processes at once, else the logical core count.
REM
REM Output: build\KenshiZoneOpt_clang_dev\   (DEV: /DZONEOPT_DEBUG; clang-cl /O2, lld-link without /OPT)
REM         build\KenshiZoneOpt_clang_prod\  (PROD: clang-cl /O2 /Gy, lld-link /OPT:REF /OPT:ICF)
REM   each with KenshiZoneOpt.dll, KenshiZoneOpt.pdb, RE_Kenshi.json and the INI.
REM   Objects and compile.log (every compiler message): build\obj_clang_dev\,
REM   build\obj_clang_prod\. Flags and the reasons for them: tools\build\clang_env.bat.
REM
REM Success prints, as its last lines:
REM   CLANG build OK: ZONEHAND_STEP=<n> headers=<KENSHILIB_HEADERS>
REM     DEV:  build\KenshiZoneOpt_clang_dev\
REM     PROD: build\KenshiZoneOpt_clang_prod\
REM A failure prints the failing step, then "CLANG FAILED at <folder>", and exits 1.

setlocal enabledelayedexpansion
cd /d "%~dp0"

REM vcvarsall prepends to INCLUDE and LIB, so a later Visual Studio's headers
REM and libraries inherited from a developer prompt would sit behind 2010's,
REM and clang would take any header 2010 lacks (<atomic>, <thread>) from them.
set "INCLUDE="
set "LIB="
set "LIBPATH="
call "C:\Program Files (x86)\Microsoft Visual Studio 10.0\VC\vcvarsall.bat" amd64
if errorlevel 1 (
    echo ERROR: Could not set up VS 2010 x64 environment.
    exit /b 1
)

call tools\kenshilib\build_env.bat
if errorlevel 1 (
    echo        KENSHILIB must be a full KenshiLib tree, libraries included; put a header-only tree in KENSHILIB_HEADERS.
    exit /b 1
)
call tools\build\clang_env.bat
if errorlevel 1 exit /b 1

if not "%~1"=="" (
    echo ERROR: build_opt_clang.bat takes no positional argument -- "%~1" was given.
    exit /b 1
)

REM Every other gate is folded in config.h; a leftover setting would be
REM silently ignored, so it is refused, as build_opt_step4.bat does.
set "CLANG_FOLDED_GATES=PATHFIND_STEP NMCACHE_STEP ISLAND_STEP ZONELIFE_STEP PRELOAD_STEP PATHPOOL_STEP NMPRUNE_STEP NMNBRSEED_STEP NMFIX_STEP GATE_STEP MISSPAR_STEP NMADJ_STEP KLIB_STEP KLIB_MEMBERS"
for %%G in (%CLANG_FOLDED_GATES%) do (
    if defined %%G (
        echo ERROR: %%G is set in the environment, but that gate is folded and no longer read.
        echo        Clear it ^(set %%G=^) before building, or the build would silently ignore it.
        exit /b 1
    )
)

if "%ZONEHAND_STEP%"=="" set ZONEHAND_STEP=3
if not "%ZONEHAND_STEP%"=="0" if not "%ZONEHAND_STEP%"=="1" if not "%ZONEHAND_STEP%"=="2" if not "%ZONEHAND_STEP%"=="3" (
    echo ERROR: ZONEHAND_STEP=%ZONEHAND_STEP% is out of range ^(0-3^).
    exit /b 1
)
set "CLANG_DEFINES=/DZONEHAND_STEP=%ZONEHAND_STEP%"

"%CLANG_CL%" --version | findstr /b /c:"clang version"
echo Headers: %KENSHILIB_HEADERS%
echo Libraries: %KENSHILIB%

set "CLANG_DEV_OUT=build\KenshiZoneOpt_clang_dev"
set "CLANG_PROD_OUT=build\KenshiZoneOpt_clang_prod"

call tools\build\variant_clang.bat "%CLANG_DEV_OUT%" "build\obj_clang_dev" "%CLANG_DEFINES% /DZONEOPT_DEBUG" ""
if errorlevel 1 (
    echo CLANG FAILED at %CLANG_DEV_OUT%
    exit /b 1
)
call tools\build\variant_clang.bat "%CLANG_PROD_OUT%" "build\obj_clang_prod" "%CLANG_DEFINES%" ""
if errorlevel 1 (
    echo CLANG FAILED at %CLANG_PROD_OUT%
    exit /b 1
)

echo.
echo CLANG build OK: ZONEHAND_STEP=%ZONEHAND_STEP% headers=%KENSHILIB_HEADERS%
echo   DEV:  %CLANG_DEV_OUT%\
echo   PROD: %CLANG_PROD_OUT%\
endlocal
exit /b 0
