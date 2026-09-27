@echo off
REM Build KenshiZoneProfiler.dll using VS 2010 x64 compiler directly
REM Run this from any command prompt - it sets up the environment itself.
REM Output: build\KenshiZoneProfiler\  (ready to copy into Kenshi's mods folder)

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
set OUTDIR=build\KenshiZoneProfiler
set OBJDIR=build\obj

REM --- Create output directories ---
if not exist "%OBJDIR%" mkdir "%OBJDIR%"
if not exist "%OUTDIR%" mkdir "%OUTDIR%"

REM --- cl /MP: compile the sources in parallel cl processes. It only schedules
REM the compile; the DLL is byte-identical apart from the link timestamps.
REM BUILD_MP=0 turns it off, BUILD_MP=<n> sets the process count. ---
set "B_MP=/MP"
if "%BUILD_MP%"=="0" set "B_MP="
if not "%BUILD_MP%"=="" if not "%BUILD_MP%"=="0" set "B_MP=/MP%BUILD_MP%"

REM --- Sources: tools\build\profsrc.txt, one path per line, in link order ---
if not exist tools\build\profsrc.txt (
    echo ERROR: tools\build\profsrc.txt is missing.
    exit /b 1
)
python tools\build\check_coresrc.py --profsrc
if errorlevel 1 exit /b 1
set "SRC_RSP=%OBJDIR%\sources.rsp"
set "OBJS_RSP=%OBJDIR%\objs.rsp"
> "%SRC_RSP%" (
    for /f "usebackq eol=# delims=" %%F in ("tools\build\profsrc.txt") do echo "%%F"
)
> "%OBJS_RSP%" (
    for /f "usebackq eol=# delims=" %%F in ("tools\build\profsrc.txt") do echo "%OBJDIR%\%%~nF.obj"
)

REM --- Compile ---
cl /nologo /EHsc /O2 /GL /MD /W3 /DNDEBUG /DWIN32_LEAN_AND_MEAN /DBOOST_ALL_NO_LIB /DBOOST_ERROR_CODE_HEADER_ONLY /DBOOST_SYSTEM_NO_DEPRECATED %B_MP% ^
   /I"%KENSHILIB%\Include" /I"%KENSHILIB%\Include\ogre" /I"%BOOST_ROOT%" /Isrc ^
   /c @"%SRC_RSP%" ^
   /Fo%OBJDIR%\

if errorlevel 1 (
    echo.
    echo COMPILE FAILED
    exit /b 1
)

REM --- Link ---
REM /OPT:NOICF keeps the per-probe wrapper templates (CallSiteProbe.cpp) and the
REM per-class listener thunks (AuditListeners.cpp) distinct.
REM /MAP writes build\obj\KenshiZoneProfiler.map, for tools\build\verify_layout.py.
link /nologo /DLL /LTCG /OPT:REF /OPT:NOICF /MACHINE:X64 /SUBSYSTEM:CONSOLE ^
     /LIBPATH:"%KENSHILIB%\Libraries\KenshiLib" ^
     KenshiLib.lib user32.lib ^
     @"%OBJS_RSP%" ^
     /OUT:"%OUTDIR%\KenshiZoneProfiler.dll" ^
     /IMPLIB:"%OBJDIR%\KenshiZoneProfiler.lib" ^
     /MAP:"%OBJDIR%\KenshiZoneProfiler.map"

if errorlevel 1 (
    echo.
    echo LINK FAILED
    exit /b 1
)

REM --- Create RE_Kenshi.json if missing ---
python tools\kenshilib\check_imports.py "%OUTDIR%\KenshiZoneProfiler.dll"
if errorlevel 1 exit /b 1

if not exist "%OUTDIR%\RE_Kenshi.json" (
    echo {"Plugins": ["KenshiZoneProfiler.dll"]} > "%OUTDIR%\RE_Kenshi.json"
)

REM --- Copy the default frame-audit INI if missing (never overwrite local edits) ---
if not exist "%OUTDIR%\KenshiZoneProfiler.ini" (
    copy /y profiler\KenshiZoneProfiler.ini "%OUTDIR%\KenshiZoneProfiler.ini" >nul
    if errorlevel 1 (
        echo INI COPY FAILED: profiler\KenshiZoneProfiler.ini
        exit /b 1
    )
)

echo.
echo BUILD SUCCEEDED
echo Output: %OUTDIR%\
echo Copy that folder into Kenshi's mods\ directory to install.

endlocal
