@echo off
REM Build the shipped KenshiZoneOpt.dll DEV/PROD pair. This is the same build
REM as build_opt_step4.bat with no argument: that script is now the one
REM source of truth for the shipped defines, so both scripts produce the
REM identical build\KenshiZoneOpt_step4_dev\ / _prod\ folders.
REM
REM The TESTING, ZONEONLY and PATHFIND_STEP=7 FULL variants this script used
REM to also build are retired: only the step-4 DEV/PROD pair ships.

setlocal

cd /d "%~dp0"
call "%~dp0build_opt_step4.bat"
set "RC=%errorlevel%"

endlocal & exit /b %RC%
