@echo off
REM Host unit-test entry point: suites are data (tools\tests\suites.txt), run
REM by tools\tests\run_suites.py (compile then run, in parallel, capped at
REM TEST_JOBS). No game or KenshiLib headers; only cl and the VS 2010 x64
REM environment are required.
REM
REM check_test_guards.py runs AFTER the cd below (its path is relative to
REM the repo root, so the cd makes it work from any caller's directory) and checks
REM that every tools\tests\*_units.cpp is covered by suites.txt (or
REM explicitly excluded) and that run_suites.py actually fails a run when a
REM suite fails.
setlocal
cd /d "%~dp0..\.."
call "C:\Program Files (x86)\Microsoft Visual Studio 10.0\VC\vcvarsall.bat" amd64 >nul
if errorlevel 1 (
    echo ERROR: Could not set up VS 2010 x64 environment.
    exit /b 1
)

python tools\tests\check_test_guards.py
if errorlevel 1 exit /b 1

REM A private suite list beside this file runs merged with suites.txt; the
REM public export carries neither it nor the suites it names. The merge is not
REM a parenthesized block: an exit /b inside one, with commands after it, ends
REM the script but leaves cmd /c returning 0.
set SUITES=tools\tests\suites.txt
if not exist "%~dp0suites_private.txt" goto run_suites
if not exist build\tests mkdir build\tests
if errorlevel 1 exit /b 1
copy /y tools\tests\suites.txt build\tests\suites_merged.txt >nul
if errorlevel 1 exit /b 1
echo.>> build\tests\suites_merged.txt
type "%~dp0suites_private.txt" >> build\tests\suites_merged.txt
if errorlevel 1 exit /b 1
set SUITES=build\tests\suites_merged.txt
echo build_tests: merging the private suite list suites_private.txt
:run_suites
python tools\tests\run_suites.py --suites %SUITES%
if errorlevel 1 exit /b 1

REM The Python unit tests (tools\tests\py_tests.txt) have no C++ suite of
REM their own; run them here so a broken one fails this gate too.
python tools\tests\run_py_tests.py
if errorlevel 1 exit /b 1
