@echo off
REM Build the DEV+PROD pair handed out for in-game sessions
REM (the only session build until V9). The PLAYER STUCK retry is gone; PLAYER
REM STUCK itself stays a diagnostic.
REM
REM Usage: build_opt_step4.bat
REM   ZONEHAND_STEP: from the environment, else 3. Step 1 holds cells without
REM   adopting them, which blocks the game's own lease. (Per-cycle, Set B and
REM   private-lease measurement; config.h's compiled-in default is 0).
REM   STEP4_SUFFIX: optional, from the environment. Appended to the output and
REM   object folder names before _dev/_prod, so a second pair can sit beside
REM   the first (STEP4_SUFFIX=_base -> build\KEO_step4_base_dev\).
REM   It must not contain "_dev" or "_prod" (the _dev suffix selects DEV flags).
REM   STEP4_VARIANTS: optional, from the environment. Empty or "dev prod"
REM   builds both folders; "dev" builds the DEV folder only, and the PROD line
REM   below then reads "(not built)". Any other value is refused.
REM   The success line below always prints every effective gate value (never
REM   empty), so a log proves what was built.
REM
REM Output: build\KEO_step4<SUFFIX>_dev\   (DEV: verbose logging;
REM                                                  /O2 /GL + /LTCG, no /Gy /OPT)
REM         build\KEO_step4<SUFFIX>_prod\  (PROD: reduced logging;
REM                                                  /GL /Gy + /LTCG /OPT:REF /OPT:ICF)
REM   where <SUFFIX> is STEP4_SUFFIX.
REM
REM The variants build at the same time through tools\build\run_variants.py:
REM one cl process per source, then each variant's link through
REM tools\build\variant.bat. BUILD_JOBS=1 builds them one after the other;
REM BUILD_MP sets how many processes run at once. Each variant's full output
REM is in build\obj_step4<SUFFIX>_dev\build.log / ..._prod\build.log; a failure
REM prints that log's tail and "STEP4 FAILED at <folder>", and exits 1.

setlocal enabledelayedexpansion
cd /d "%~dp0"

call "C:\Program Files (x86)\Microsoft Visual Studio 10.0\VC\vcvarsall.bat" amd64
if errorlevel 1 (
    echo ERROR: Could not set up VS 2010 x64 environment.
    exit /b 1
)

REM Allow an already-set KENSHILIB (e.g. a worktree elsewhere) to override the default.
call tools\kenshilib\build_env.bat
if errorlevel 1 exit /b 1

REM No positional argument: every folded gate is fixed, so there is nothing
REM left for one to select. An old invocation naming a step number (the
REM ladder-era ISLAND_STEP argument, for example) fails loudly instead of
REM silently building the folded values.
if not "%~1"=="" (
    echo ERROR: build_opt_step4.bat takes no positional argument -- "%~1" was given.
    echo        Every step gate but ZONEHAND_STEP is folded; there is nothing left to select.
    exit /b 1
)

REM Every gate this script used to accept is folded to a fixed value in
REM config.h. A leftover "set NMFIX_STEP=8" or "set KLIB_STEP=2" from an old
REM session would otherwise build the same folded DLL with no warning that
REM the variable did nothing. Refuse instead.
set "STEP4_FOLDED_GATES=PATHFIND_STEP NMCACHE_STEP ISLAND_STEP ZONELIFE_STEP PRELOAD_STEP PATHPOOL_STEP NMPRUNE_STEP NMNBRSEED_STEP NMFIX_STEP GATE_STEP MISSPAR_STEP NMADJ_STEP KLIB_STEP KLIB_MEMBERS"
for %%G in (%STEP4_FOLDED_GATES%) do (
    if defined %%G (
        echo ERROR: %%G is set in the environment, but that gate is folded and no longer read.
        echo        Clear it ^(set %%G=^) before building, or the build would silently ignore it.
        exit /b 1
    )
)

REM Optional folder suffix. Refused when it would change the DEV/PROD choice
REM made by :build from the "_dev" in the output folder name.
if defined STEP4_SUFFIX if not "!STEP4_SUFFIX:_dev=!"=="!STEP4_SUFFIX!" goto :badsuffix
if defined STEP4_SUFFIX if not "!STEP4_SUFFIX:_prod=!"=="!STEP4_SUFFIX!" goto :badsuffix

REM Zone handoff staging gate. Step 1 is the loading-cycle / Set B /
REM private-lease measurement (no behaviour change).
if "%ZONEHAND_STEP%"=="" set ZONEHAND_STEP=3
if not "%ZONEHAND_STEP%"=="0" if not "%ZONEHAND_STEP%"=="1" if not "%ZONEHAND_STEP%"=="2" if not "%ZONEHAND_STEP%"=="3" (
    echo ERROR: ZONEHAND_STEP=%ZONEHAND_STEP% is out of range ^(0-3^).
    exit /b 1
)

set "STEP4_DEFINES=/DZONEHAND_STEP=%ZONEHAND_STEP%"

set "STEP4_FOLDER_SUFFIX=%STEP4_SUFFIX%"
set "STEP4_DEV_OUT=build\KEO_step4%STEP4_FOLDER_SUFFIX%_dev"
set "STEP4_PROD_OUT=build\KEO_step4%STEP4_FOLDER_SUFFIX%_prod"

REM Which variants: both unless STEP4_VARIANTS=dev. Exact values only, so a
REM misspelt or space-padded value is refused rather than read as "both".
if not "%STEP4_VARIANTS%"=="" if not "%STEP4_VARIANTS%"=="dev" if not "%STEP4_VARIANTS%"=="dev prod" (
    echo ERROR: STEP4_VARIANTS="%STEP4_VARIANTS%" must be empty, "dev" or "dev prod".
    exit /b 1
)
set "STEP4_PROD_ARG=--variant "%STEP4_PROD_OUT%" "build\obj_step4%STEP4_FOLDER_SUFFIX%_prod" "" """
set "STEP4_PROD_SHOW=%STEP4_PROD_OUT%\"
if "%STEP4_VARIANTS%"=="dev" set "STEP4_PROD_ARG="
if "%STEP4_VARIANTS%"=="dev" set "STEP4_PROD_SHOW=(not built)"

REM Both variants compile with "%STEP4_DEFINES%" plus their own
REM extra define (DEV: /DKEO_DEBUG), as before. DEV/PROD flags follow the
REM "_dev" suffix on the output folder (tools\build\run_variants.py). On failure
REM run_variants.py prints the failing log's tail and "STEP4 FAILED at <folder>".
python tools\build\run_variants.py --fail-prefix "STEP4 FAILED at" --defines "%STEP4_DEFINES%" ^
    --variant "%STEP4_DEV_OUT%" "build\obj_step4%STEP4_FOLDER_SUFFIX%_dev" "/DKEO_DEBUG" "" ^
    %STEP4_PROD_ARG%
if errorlevel 1 exit /b 1

echo.
echo STEP4 build OK: ZONEHAND_STEP=%ZONEHAND_STEP%
echo   DEV:  %STEP4_DEV_OUT%\
echo   PROD: %STEP4_PROD_SHOW%
endlocal
exit /b 0

:badsuffix
echo ERROR: STEP4_SUFFIX="%STEP4_SUFFIX%" must not contain "_dev" or "_prod" (the _dev suffix selects the DEV flags)
exit /b 1
