@echo off
REM Host unit-test entry point. tools\tests\test_gate.py runs the guards
REM (check_test_guards.py), the C++ suites (run_suites.py over suites.txt) and
REM the Python tests (run_py_tests.py) as three concurrent processes, then prints
REM their output in that order. No game or KenshiLib headers; only cl and the
REM VS 2010 x64 environment are required. Arguments go to test_gate.py.
REM
REM The cd makes the relative paths work from any caller's directory. No failure
REM path is a parenthesized block: an exit /b inside one, with commands after
REM it, ends the script but leaves cmd /c returning 0. echo leaves errorlevel
REM as it was, so the second test still sees vcvarsall's.
setlocal
cd /d "%~dp0..\.."
if errorlevel 1 exit /b 1
call "C:\Program Files (x86)\Microsoft Visual Studio 10.0\VC\vcvarsall.bat" amd64 >nul
if errorlevel 1 echo ERROR: Could not set up VS 2010 x64 environment.
if errorlevel 1 exit /b 1

python tools\tests\test_gate.py %*
if errorlevel 1 exit /b 1
if not errorlevel 0 exit /b 1
