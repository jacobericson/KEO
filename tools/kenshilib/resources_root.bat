@echo off
REM Sets KENSHI_RESOURCES to the resources folder the build scripts take their
REM default dependencies from; deliberately no setlocal (exports it).
REM
REM The search starts at this checkout's root (two folders above this script)
REM and looks in each of the six folders above it for a resources folder
REM holding KenshiLib\Libraries\KenshiLib\KenshiLib.lib; the nearest wins. A
REM checkout nested below another one, such as a worktree, reaches the same
REM tree as the checkout beside it. With none found it names the resources
REM folder beside this checkout, and the callers then fail on it (their own
REM checks, or the compile that cannot find the headers).
REM tools\kenshilib\resources_root.py makes the same search for the Python tools.
set "KENSHI_RESOURCES="
for %%U in (.. ..\.. ..\..\.. ..\..\..\.. ..\..\..\..\.. ..\..\..\..\..\..) do if not defined KENSHI_RESOURCES if exist "%~dp0..\..\%%U\resources\KenshiLib\Libraries\KenshiLib\KenshiLib.lib" for %%R in ("%~dp0..\..\%%U\resources") do set "KENSHI_RESOURCES=%%~fR"
if not defined KENSHI_RESOURCES for %%R in ("%~dp0..\..\..\resources") do set "KENSHI_RESOURCES=%%~fR"
exit /b 0
