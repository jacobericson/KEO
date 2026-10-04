@echo off
REM Links ONE KEO.dll variant: link, KenshiLib import check, RE_Kenshi.json,
REM the settings INI. Called by tools\build\run_variants.py (build_opt_step4.bat,
REM and so build_opt.bat) once a variant's objects are compiled and checked,
REM with the output going to <OBJDIR>\build.log. run_variants.py compiles;
REM its cl_args() holds the compile flags.
REM
REM Usage (from the repository root, after vcvarsall amd64 and
REM tools\kenshilib\build_env.bat have set up the environment):
REM   tools\build\variant.bat link <OUTDIR> <OBJDIR> "<LABEL>"
REM     link     the only mode: link every CORESRC object in OBJDIR, import
REM              check, RE_Kenshi.json, KEO.ini.
REM     LABEL    name used in the progress lines ("" = OUTDIR).
REM
REM DEV or PROD is decided from the "_dev" suffix on OUTDIR, as for the compile:
REM   PROD = link /LTCG /OPT:REF /OPT:ICF;
REM   DEV  = link /LTCG (no /OPT:REF /OPT:ICF).
REM Every variant MUST keep /GL + /LTCG: without whole-program optimization
REM &KenshiLib::fn resolves to an import thunk inside our own DLL, and
REM KenshiLib::GetRealAddress() asserts at startup ("address ... in your own
REM module, try enabling whole program optimization"), DEV as much as PROD.
REM
REM tools\build\coresrc.txt is the one source list for every optimizer
REM variant. Its order is the link order, and the link list is built from it,
REM never from %OBJDIR%\*.obj (stale objects from removed source files would
REM otherwise link).
REM
REM Exit code 0 on success, 1 on any failure (the last lines say which step).

setlocal enabledelayedexpansion
set "B_MODE=%~1"
set "B_OUTDIR=%~2"
set "B_OBJDIR=%~3"
set "B_LABEL=%~4"
if "%B_LABEL%"=="" set "B_LABEL=%B_OUTDIR%"
if /i not "%B_MODE%"=="link" (
    echo ERROR: usage: tools\build\variant.bat link OUTDIR OBJDIR "LABEL" -- tools\build\run_variants.py compiles
    exit /b 1
)
if "%B_OBJDIR%"=="" (
    echo ERROR: usage: tools\build\variant.bat link OUTDIR OBJDIR "LABEL"
    exit /b 1
)

if not exist "%B_OBJDIR%" mkdir "%B_OBJDIR%"
if not exist "%B_OUTDIR%" mkdir "%B_OUTDIR%"

set "B_ISDEV=NO"
if not "%B_OUTDIR:_dev=%"=="%B_OUTDIR%" set "B_ISDEV=YES"

if "%B_ISDEV%"=="YES" (
    set "B_LTCG=/LTCG"
    set "B_LINKOPT="
) else (
    set "B_LTCG=/LTCG"
    set "B_LINKOPT=/OPT:REF /OPT:ICF"
)

REM Response file (@file): cmd's delayed-expansion "set" silently truncates
REM around 8,191 characters (confirmed with a 200-entry scratch variable: it
REM ran with no error and the program received only the first ~8,102 of
REM ~11,400 characters). At 171 CORESRC files the object list already sits
REM close to that ceiling (measured 7,824 characters for a 168-file OBJS/LINK
REM line, before three more files were added). The link's object list goes
REM into a one-token-per-line response file instead, read entirely by link
REM with no cmd variable in the middle, so there is no length ceiling below
REM what link accepts.
REM
REM tools\build\coresrc.txt is the one source list for every optimizer
REM variant, one path per line; its order is the link order, and the link
REM list is built from it, never from %OBJDIR%\*.obj (stale objects from
REM removed source files would otherwise link). The response file is read
REM straight from it with "for /f", never assembled into one cmd variable.
set "B_OBJS_RSP=%B_OBJDIR%\objs.rsp"
> "%B_OBJS_RSP%" (
    for /f "usebackq eol=# delims=" %%F in ("tools\build\coresrc.txt") do echo "%B_OBJDIR%\%%~nF.obj"
)

REM The command line is built once and echoed before it runs, so the log
REM shows exactly what was executed.
REM The link also writes KEO.map beside the objects, for tools\build\verify_layout.py.
set "B_LINK=link /nologo /DLL %B_LTCG% %B_LINKOPT% /MACHINE:X64 /SUBSYSTEM:CONSOLE /LIBPATH:"%KENSHILIB%\Libraries\KenshiLib" /LIBPATH:"%KENSHILIB%\Libraries\mygui" KenshiLib.lib MyGUIEngine_x64.lib user32.lib @"%B_OBJS_RSP%" /OUT:"%B_OUTDIR%\KEO.dll" /IMPLIB:"%B_OBJDIR%\KEO.lib" /MAP:"%B_OBJDIR%\KEO.map""

echo LINK: !B_LINK!
echo   objs.rsp: @"%B_OBJS_RSP%"
!B_LINK!
if errorlevel 1 (
    echo.
    echo %B_LABEL% LINK FAILED
    exit /b 1
)

python tools\kenshilib\check_imports.py "%B_OUTDIR%\KEO.dll"
if errorlevel 1 (
    echo.
    echo %B_LABEL% IMPORT CHECK FAILED
    exit /b 1
)

REM RE_Kenshi.json lists exactly the DLL this folder holds. A json that differs (one naming
REM another DLL, or two DLLs) is kept as RE_Kenshi.json.old and rewritten.
REM A failed backup is named in the WARNING.
set "B_JSON=%B_OUTDIR%\RE_Kenshi.json"
set "B_JSON_WANT=%B_OBJDIR%\RE_Kenshi.json.want"
echo {"Plugins": ["KEO.dll"]} > "%B_JSON_WANT%"
if not exist "%B_JSON%" (
    copy /y "%B_JSON_WANT%" "%B_JSON%" >nul
    goto :json_check
)
fc /b "%B_JSON_WANT%" "%B_JSON%" >nul 2>&1
if errorlevel 1 (
    copy /y "%B_JSON%" "%B_JSON%.old" >nul
    if errorlevel 1 (
        copy /y "%B_JSON_WANT%" "%B_JSON%" >nul
        echo %B_LABEL% WARNING: RE_Kenshi.json did not list exactly KEO.dll; rewritten, but the previous copy could NOT be saved: RE_Kenshi.json.old could not be written
    ) else (
        copy /y "%B_JSON_WANT%" "%B_JSON%" >nul
        echo %B_LABEL% WARNING: RE_Kenshi.json did not list exactly KEO.dll; rewritten, previous copy saved as RE_Kenshi.json.old
    )
)
:json_check
fc /b "%B_JSON_WANT%" "%B_JSON%" >nul 2>&1
if errorlevel 1 (
    echo %B_LABEL% RE_Kenshi.json WRITE FAILED
    exit /b 1
)

REM Copy the default settings INI, without clobbering a user's edited copy.
REM KEO.ini.template records the exact template last copied into
REM this output folder. On a rebuild the output is refreshed only when it
REM still matches that record (i.e. untouched since) and the template itself
REM has moved on; an edited output, or a legacy folder with no record, is
REM left alone and the newer template is saved beside it as
REM KEO.ini.new instead, so nothing is silently lost either way.
set "B_INI_OUT=%B_OUTDIR%\KEO.ini"
set "B_INI_STAMP=%B_OUTDIR%\KEO.ini.template"

if not exist "%B_INI_OUT%" (
    copy /y KEO.ini "%B_INI_OUT%" >nul
    copy /y KEO.ini "%B_INI_STAMP%" >nul
    goto :ini_done
)

if not exist "%B_INI_STAMP%" (
    fc /b KEO.ini "%B_INI_OUT%" >nul 2>&1
    if not errorlevel 1 (
        copy /y KEO.ini "%B_INI_STAMP%" >nul
        goto :ini_done
    )
    copy /y KEO.ini "%B_OUTDIR%\KEO.ini.new" >nul
    echo %B_LABEL% WARNING: KEO.ini differs from the template and this folder has no template record ^(legacy folder^); new template saved as KEO.ini.new
    goto :ini_done
)

fc /b "%B_INI_OUT%" "%B_INI_STAMP%" >nul 2>&1
if errorlevel 1 (
    fc /b KEO.ini "%B_INI_STAMP%" >nul 2>&1
    if errorlevel 1 (
        copy /y KEO.ini "%B_OUTDIR%\KEO.ini.new" >nul
        echo %B_LABEL% WARNING: KEO.ini was edited and the template changed; new template saved as KEO.ini.new
    )
    goto :ini_done
)

fc /b KEO.ini "%B_INI_STAMP%" >nul 2>&1
if errorlevel 1 (
    copy /y KEO.ini "%B_INI_OUT%" >nul
    copy /y KEO.ini "%B_INI_STAMP%" >nul
)

:ini_done

echo %B_LABEL% build OK: %B_OUTDIR%\
exit /b 0
