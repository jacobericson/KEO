@echo off
REM Shared build prerequisite checks; deliberately no setlocal (exports settings).
REM An unset KENSHILIB defaults to the KenshiLib tree in the resources folder
REM resources_root.bat finds up to six folders above the checkout root.
if "%KENSHILIB%"=="" (
    call "%~dp0resources_root.bat"
    if errorlevel 1 exit /b 1
)
if "%KENSHILIB%"=="" set "KENSHILIB=%KENSHI_RESOURCES%\KenshiLib"
REM Boost sits beside resources in the usual checkout; honour an explicit override.
if "%BOOST_ROOT%"=="" set "BOOST_ROOT=%KENSHILIB%\..\..\boost_1_60_0"
if not exist "%KENSHILIB%\Include\kenshi\GameWorld.h" (
    echo ERROR: KENSHILIB must point to the KenshiLib 0.5.1 headers and artifacts.
    exit /b 1
)
if not exist "%KENSHILIB%\Libraries\KenshiLib\KenshiLib.lib" (
    echo ERROR: KenshiLib.lib missing under KENSHILIB.
    exit /b 1
)
if not exist "%BOOST_ROOT%\boost\version.hpp" (
    echo ERROR: Set BOOST_ROOT to the Boost 1.60.0 header directory.
    exit /b 1
)
findstr /c:"#define BOOST_VERSION 106000" "%BOOST_ROOT%\boost\version.hpp" >nul
if errorlevel 1 (
    echo ERROR: BOOST_ROOT must contain Boost 1.60.0.
    exit /b 1
)
python --version >nul 2>&1
if errorlevel 1 (
    echo ERROR: Python 3 is required for the KenshiLib import compatibility check.
    exit /b 1
)
exit /b 0
