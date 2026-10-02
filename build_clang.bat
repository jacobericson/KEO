@echo off
REM Build KEOProfiler.dll with clang-cl and lld-link against the VS 2010
REM contract. The clang counterpart of build.bat: same sources, defines and
REM link options, plus the clang flags from tools\build\clang_env.bat. Additive:
REM build.bat's MSVC profiler is untouched.
REM
REM Usage:
REM   build_clang.bat
REM       Headers come from KENSHILIB_HEADERS, else the pinned tree named by
REM       dependency-baseline.json's clang_headers block (tools\build\clang_env.bat);
REM       tools\kenshilib\check_headers_pin.py refuses any other tree. Libraries
REM       always come from KENSHILIB.
REM
REM   LLVM_BIN and BUILD_MP as in build_opt_clang.bat.
REM
REM Output: build\KEOProfiler_clang\ (KEOProfiler.dll and .pdb,
REM RE_Kenshi.json, the frame-audit INI); objects and compile.log in
REM build\obj_clang_profiler\.
REM
REM After the link: the KenshiLib and CRT import gates, then
REM tools\build\check_seh_handlers.py over the profiler's sources, so a
REM __try without its exception handler fails the build.
REM
REM Success prints "CLANG PROFILER BUILD SUCCEEDED" and the output folder; a
REM failure names the failing step and exits 1.

setlocal enabledelayedexpansion
cd /d "%~dp0"

REM vcvarsall prepends to INCLUDE and LIB; start from empty ones so nothing
REM from a later Visual Studio sits behind 2010's (see build_opt_clang.bat).
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

set "OUTDIR=build\KEOProfiler_clang"
set "OBJDIR=build\obj_clang_profiler"
if not exist "%OBJDIR%" mkdir "%OBJDIR%"
if not exist "%OUTDIR%" mkdir "%OUTDIR%"

REM The profiler's sources, in link order.
set "SRC_LIST=tools\build\profsrc.txt"
python tools\build\check_coresrc.py --profsrc
if errorlevel 1 exit /b 1
set "OBJS_RSP=%OBJDIR%\objs.rsp"
> "%OBJS_RSP%" (
    for /f "usebackq eol=# delims=" %%F in ("%SRC_LIST%") do echo "%OBJDIR%\%%~nF.obj"
)
set "FLAGS_RSP=%OBJDIR%\clang_flags.rsp"
> "%FLAGS_RSP%" (
    echo !CLANG_COMMON!
    echo /I"!KENSHILIB_HEADERS!\Include"
    echo /I"!KENSHILIB_HEADERS!\Include\ogre"
    echo /I"!BOOST_ROOT!"
    echo /Isrc
)

"%CLANG_CL%" --version | findstr /b /c:"clang version"
echo Headers: %KENSHILIB_HEADERS%\Include
echo Libraries: %KENSHILIB%\Libraries
echo Flags ^(clang_flags.rsp^):
type "%FLAGS_RSP%"
python tools\build\clang_compile.py --clang "%CLANG_CL%" --flags "%FLAGS_RSP%" --sources "%SRC_LIST%" --objdir "%OBJDIR%" --log "%OBJDIR%\compile.log"
if errorlevel 1 (
    echo.
    echo CLANG PROFILER COMPILE FAILED
    exit /b 1
)

REM /OPT:NOICF keeps the per-probe wrapper templates (CallSiteProbe.cpp) and the
REM per-class listener thunks (AuditListeners.cpp) distinct.
set "PDB=%OUTDIR%\KEOProfiler.pdb"
if exist "%PDB%" del /q "%PDB%"
"%LLD_LINK%" /nologo /DLL /OPT:REF /OPT:NOICF /MACHINE:X64 /SUBSYSTEM:CONSOLE /DEBUG /PDB:"%PDB%" ^
     /LIBPATH:"%KENSHILIB%\Libraries\KenshiLib" ^
     KenshiLib.lib user32.lib @"%OBJS_RSP%" ^
     /OUT:"%OUTDIR%\KEOProfiler.dll" ^
     /IMPLIB:"%OBJDIR%\KEOProfiler.lib"
if errorlevel 1 goto :link_failed
if not exist "%PDB%" goto :link_failed

python tools\kenshilib\check_imports.py "%OUTDIR%\KEOProfiler.dll"
if errorlevel 1 (
    echo.
    echo CLANG PROFILER IMPORT CHECK FAILED
    exit /b 1
)
python tools\build\check_crt_imports.py "%OUTDIR%\KEOProfiler.dll"
if errorlevel 1 (
    echo.
    echo CLANG PROFILER CRT IMPORT CHECK FAILED
    exit /b 1
)
python tools\build\check_seh_handlers.py "%OUTDIR%\KEOProfiler.dll" --sources "%SRC_LIST%"
if errorlevel 1 (
    echo.
    echo CLANG PROFILER SEH HANDLER CHECK FAILED
    exit /b 1
)

REM RE_Kenshi.json lists exactly the DLL this folder holds. A json that differs (one naming
REM another DLL, or two DLLs) is kept as RE_Kenshi.json.old and rewritten.
REM A failed backup is named in the WARNING.
set "JSON=%OUTDIR%\RE_Kenshi.json"
set "JSON_WANT=%OBJDIR%\RE_Kenshi.json.want"
echo {"Plugins": ["KEOProfiler.dll"]} > "%JSON_WANT%"
if not exist "%JSON%" (
    copy /y "%JSON_WANT%" "%JSON%" >nul
    goto :json_check
)
fc /b "%JSON_WANT%" "%JSON%" >nul 2>&1
if errorlevel 1 (
    copy /y "%JSON%" "%JSON%.old" >nul
    if errorlevel 1 (
        copy /y "%JSON_WANT%" "%JSON%" >nul
        echo CLANG PROFILER WARNING: RE_Kenshi.json did not list exactly KEOProfiler.dll; rewritten, but the previous copy could NOT be saved: RE_Kenshi.json.old could not be written
    ) else (
        copy /y "%JSON_WANT%" "%JSON%" >nul
        echo CLANG PROFILER WARNING: RE_Kenshi.json did not list exactly KEOProfiler.dll; rewritten, previous copy saved as RE_Kenshi.json.old
    )
)
:json_check
fc /b "%JSON_WANT%" "%JSON%" >nul 2>&1
if errorlevel 1 (
    echo CLANG PROFILER RE_Kenshi.json WRITE FAILED
    exit /b 1
)

REM Copy the default frame-audit INI if missing (never overwrite local edits).
if not exist "%OUTDIR%\KEOProfiler.ini" (
    copy /y profiler\KEOProfiler.ini "%OUTDIR%\KEOProfiler.ini" >nul
    if errorlevel 1 (
        echo CLANG PROFILER INI COPY FAILED: profiler\KEOProfiler.ini
        exit /b 1
    )
)

echo.
echo CLANG PROFILER BUILD SUCCEEDED
echo Output: %OUTDIR%\
endlocal
exit /b 0

:link_failed
echo.
echo CLANG PROFILER LINK FAILED
exit /b 1
