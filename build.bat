@echo off
REM Build KEOProfiler.dll using VS 2010 x64 compiler directly
REM Run this from any command prompt - it sets up the environment itself.
REM Output: build\KEOProfiler\  (copy its contents, RE_Kenshi.json included, over mods\KenshiZoneProfiler\; never into a second mod folder)

setlocal

REM --- Change to script directory ---
cd /d "%~dp0"

REM --- VS 2010 x64 environment setup ---
call "C:\Program Files (x86)\Microsoft Visual Studio 10.0\VC\vcvarsall.bat" amd64
if errorlevel 1 (
    echo ERROR: Could not set up VS 2010 x64 environment.
    exit /b 1
)

REM --- Paths ---
REM Allow an already-set KENSHILIB (e.g. a worktree elsewhere) to override the default.
call tools\kenshilib\build_env.bat
if errorlevel 1 exit /b 1
set OUTDIR=build\KEOProfiler
set OBJDIR=build\obj

REM --- Create output directories ---
if not exist "%OBJDIR%" mkdir "%OBJDIR%"
if not exist "%OUTDIR%" mkdir "%OUTDIR%"

REM --- Sources: tools\build\profsrc.txt, one path per line, in link order ---
if not exist tools\build\profsrc.txt (
    echo ERROR: tools\build\profsrc.txt is missing.
    exit /b 1
)
python tools\build\check_coresrc.py --profsrc
if errorlevel 1 exit /b 1
set "OBJS_RSP=%OBJDIR%\objs.rsp"
> "%OBJS_RSP%" (
    for /f "usebackq eol=# delims=" %%F in ("tools\build\profsrc.txt") do echo "%OBJDIR%\%%~nF.obj"
)

REM --- Compile: one cl process per source (tools\build\run_variants.py, which
REM holds the flags). Every listed object is deleted first and must come back
REM fresh and match objects.json before the link below may run. Its output is
REM in %OBJDIR%\build.log; BUILD_MP sets how many cl processes run at once. ---
python tools\build\run_variants.py --kind prof --sources tools\build\profsrc.txt --compile-only ^
    --fail-prefix "PROFILER COMPILE FAILED at" --variant "%OUTDIR%" "%OBJDIR%" "" ""
if errorlevel 1 (
    echo.
    echo COMPILE FAILED
    exit /b 1
)

REM --- Link ---
REM /OPT:NOICF keeps the per-probe wrapper templates (CallSiteProbe.cpp) and the
REM per-class listener thunks (AuditListeners.cpp) distinct.
REM /MAP writes build\obj\KEOProfiler.map, for tools\build\verify_layout.py.
link /nologo /DLL /LTCG /OPT:REF /OPT:NOICF /MACHINE:X64 /SUBSYSTEM:CONSOLE ^
     /LIBPATH:"%KENSHILIB%\Libraries\KenshiLib" ^
     KenshiLib.lib user32.lib ^
     @"%OBJS_RSP%" ^
     /OUT:"%OUTDIR%\KEOProfiler.dll" ^
     /IMPLIB:"%OBJDIR%\KEOProfiler.lib" ^
     /MAP:"%OBJDIR%\KEOProfiler.map"

if errorlevel 1 (
    echo.
    echo LINK FAILED
    exit /b 1
)

REM --- KenshiLib import check, then RE_Kenshi.json ---
python tools\kenshilib\check_imports.py "%OUTDIR%\KEOProfiler.dll"
if errorlevel 1 exit /b 1

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
        echo PROFILER WARNING: RE_Kenshi.json did not list exactly KEOProfiler.dll; rewritten, but the previous copy could NOT be saved: RE_Kenshi.json.old could not be written
    ) else (
        copy /y "%JSON_WANT%" "%JSON%" >nul
        echo PROFILER WARNING: RE_Kenshi.json did not list exactly KEOProfiler.dll; rewritten, previous copy saved as RE_Kenshi.json.old
    )
)
:json_check
fc /b "%JSON_WANT%" "%JSON%" >nul 2>&1
if errorlevel 1 (
    echo PROFILER RE_Kenshi.json WRITE FAILED
    exit /b 1
)

REM --- Copy the default frame-audit INI if missing (never overwrite local edits) ---
if not exist "%OUTDIR%\KEOProfiler.ini" (
    copy /y profiler\KEOProfiler.ini "%OUTDIR%\KEOProfiler.ini" >nul
    if errorlevel 1 (
        echo INI COPY FAILED: profiler\KEOProfiler.ini
        exit /b 1
    )
)

echo.
echo BUILD SUCCEEDED
echo Output: %OUTDIR%\
echo Copy its contents, RE_Kenshi.json included, over mods\KenshiZoneProfiler\ to install; never into a second mod folder.

endlocal
