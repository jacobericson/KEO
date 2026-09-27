@echo off
REM Builds and runs every injection harness in tools\tests\injections.txt
REM through run_injections.py, in the VS 2010 x64 environment. Arguments pass
REM through (--list FILE).
setlocal
cd /d "%~dp0..\.."
call "C:\Program Files (x86)\Microsoft Visual Studio 10.0\VC\vcvarsall.bat" amd64 >nul
if errorlevel 1 (
    echo ERROR: Could not set up VS 2010 x64 environment.
    exit /b 1
)
python tools\tests\run_injections.py %*
if errorlevel 1 exit /b 1
