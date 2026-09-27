@echo off
REM Host unit-test entry point: suites are data (tools\tests\suites.txt), run
REM by tools\tests\run_suites.py (compile then run, in parallel, capped at
REM TEST_JOBS). No game or KenshiLib headers; only cl and the VS 2010 x64
REM environment are required.
REM
REM check_test_guards.py is run AFTER the cd below (it used to run before it,
REM by relative path, so it only worked when invoked from the repo root) and
REM checks that every tools\tests\*_units.cpp is covered by suites.txt (or
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

python tools\tests\run_suites.py
if errorlevel 1 exit /b 1

REM The Python unit tests (tools\tests\py_tests.txt) have no C++ suite of
REM their own; run them here so a broken one fails this gate too.
python tools\tests\run_py_tests.py
if errorlevel 1 exit /b 1
