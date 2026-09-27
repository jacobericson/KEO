@echo off
REM The clang checking tier: a clang-cl syntax pass (/Zs) over every optimizer
REM source with the DEV and the PROD defines and every profiler source, against
REM the pinned KenshiLib header tree. Diagnostics only: nothing is linked.
REM
REM Usage: tools\build\clang_check.bat [update-baseline]
REM   update-baseline rewrites tools\build\clang_warnings_baseline.txt from this
REM   run instead of checking against it.
REM TEST_CLANG_EXTRA=<source> (test only) adds one source to the DEV pass first; its warnings
REM count toward DEV's classes, and a baseline write refuses while its log exists.
REM Output: build\clang_check\ (the .rsp files and dev.log, prod.log, prof.log)
REM and compile_commands.json at the repository root. Exit 0 when every source
REM parses and no warning class is above the baseline; 1 otherwise.

setlocal enabledelayedexpansion
cd /d "%~dp0..\.."

set "INCLUDE="
set "LIB="
set "LIBPATH="
call "C:\Program Files (x86)\Microsoft Visual Studio 10.0\VC\vcvarsall.bat" amd64 >nul
if errorlevel 1 (
    echo ERROR: Could not set up VS 2010 x64 environment.
    exit /b 1
)
call tools\kenshilib\build_env.bat
if errorlevel 1 exit /b 1
call tools\build\clang_env.bat
if errorlevel 1 exit /b 1

set "CC_DIR=build\clang_check"
if not exist "%CC_DIR%" mkdir "%CC_DIR%"
python tools\build\compile_commands.py --rsp-dir "%CC_DIR%" --json compile_commands.json
if errorlevel 1 exit /b 1

if exist "%CC_DIR%\extra.log" del /q "%CC_DIR%\extra.log"
if defined TEST_CLANG_EXTRA (
    > "%CC_DIR%\extra.txt" echo %TEST_CLANG_EXTRA%
    python tools\build\clang_compile.py --syntax-only --clang "%CLANG_CL%" --flags "%CC_DIR%\dev.rsp" --sources "%CC_DIR%\extra.txt" --log "%CC_DIR%\extra.log"
    if errorlevel 1 goto :failed
)
python tools\build\clang_compile.py --syntax-only --clang "%CLANG_CL%" --flags "%CC_DIR%\dev.rsp" --sources tools\build\coresrc.txt --log "%CC_DIR%\dev.log" --ehsc src\gui\settings_panel.cpp --ehsc src\render\gpu_upload.cpp
if errorlevel 1 goto :failed
python tools\build\clang_compile.py --syntax-only --clang "%CLANG_CL%" --flags "%CC_DIR%\prod.rsp" --sources tools\build\coresrc.txt --log "%CC_DIR%\prod.log" --ehsc src\gui\settings_panel.cpp --ehsc src\render\gpu_upload.cpp
if errorlevel 1 goto :failed
python tools\build\clang_compile.py --syntax-only --clang "%CLANG_CL%" --flags "%CC_DIR%\prof.rsp" --sources tools\build\profsrc.txt --log "%CC_DIR%\prof.log"
if errorlevel 1 goto :failed

if /i "%~1"=="update-baseline" (
    python tools\build\clang_warnings.py write --logdir "%CC_DIR%" --baseline tools\build\clang_warnings_baseline.txt
    exit /b !errorlevel!
)
python tools\build\clang_warnings.py check --logdir "%CC_DIR%" --baseline tools\build\clang_warnings_baseline.txt
exit /b %errorlevel%

:failed
echo clang_check: FAILED ^(a source did not parse; see %CC_DIR%\*.log^)
exit /b 1
