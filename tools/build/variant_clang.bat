@echo off
REM Builds ONE KEO.dll variant with clang-cl and lld-link against the
REM VS 2010 contract: compile, link, KenshiLib import check, CRT import check,
REM SEH handler check, RE_Kenshi.json, INI. The clang counterpart of
REM tools\build\variant.bat, called by build_opt_clang.bat once per variant.
REM
REM Usage (from the repository root, after vcvarsall amd64,
REM tools\kenshilib\build_env.bat and tools\build\clang_env.bat):
REM   tools\build\variant_clang.bat <OUTDIR> <OBJDIR> "<DEFINES>" "<LABEL>"
REM     DEFINES  every define after the shared set in CLANG_COMMON (gate
REM              values, KEO_DEBUG), exactly as the calling script
REM              computed them.
REM     LABEL    name used in the progress lines ("" = OUTDIR).
REM
REM DEV or PROD is decided from the "_dev" suffix on OUTDIR, as in variant.bat:
REM   PROD = clang-cl /O2 /Gy, lld-link /OPT:REF /OPT:ICF;
REM   DEV  = clang-cl /O2,     lld-link with no /OPT option.
REM Both compile with /Z7 and link with /DEBUG, so the PDB sits beside the DLL.
REM
REM No /GL or LTO. Under clang, &Class::method for a KenshiLib export resolves
REM to an import thunk the linker makes inside our own image, and
REM KenshiLib::GetRealAddress refuses an address in the caller's image;
REM KlibRealAddress (src\game\klib_bindings.cpp) follows the thunk first, which is
REM what keeps that startup check passing without whole-program optimization.
REM
REM The two sources holding the only catch (...) blocks compile with /EHsc, the
REM exception model they have in the MSVC build: under /EHa a catch (...) also
REM catches access violations, which would then no longer end the process.
REM Every other unit must build /EHa: tools\build\clang_compile.py refuses the
REM build otherwise, and refuses /EHsc for a unit whose preprocessed text
REM holds a __try.
REM
REM After the link, tools\build\check_seh_handlers.py confirms that every
REM __try function carries its exception handler in the DLL. PROD leaves out
REM the __try functions under #ifdef KEO_DEBUG, so it names them with
REM --allow-absent; any other absent one fails the build.
REM
REM The source list is tools\build\coresrc.txt, read straight into the compile
REM driver and into the link response file, never into a cmd variable.
REM
REM Exit code 0 on success, 1 on any failure (the last lines say which step).

setlocal enabledelayedexpansion
set "B_OUTDIR=%~1"
set "B_OBJDIR=%~2"
set "B_DEFINES=%~3"
set "B_LABEL=%~4"
if "%B_LABEL%"=="" set "B_LABEL=%B_OUTDIR%"
if "%B_OBJDIR%"=="" (
    echo ERROR: usage: tools\build\variant_clang.bat OUTDIR OBJDIR "DEFINES" "LABEL"
    exit /b 1
)
if "%CLANG_COMMON%"=="" (
    echo ERROR: run tools\build\clang_env.bat first
    exit /b 1
)

if not exist "%B_OBJDIR%" mkdir "%B_OBJDIR%"
if not exist "%B_OUTDIR%" mkdir "%B_OUTDIR%"

set "B_ISDEV=NO"
if not "%B_OUTDIR:_dev=%"=="%B_OUTDIR%" set "B_ISDEV=YES"
if "%B_ISDEV%"=="YES" (
    set "B_OPTFLAGS="
    set "B_LINKOPT="
    set "B_FLAVOUR=DEV, folder ends in _dev: clang-cl /O2 without /Gy; lld-link without /OPT:REF /OPT:ICF"
) else (
    set "B_OPTFLAGS=/Gy"
    set "B_LINKOPT=/OPT:REF /OPT:ICF"
    set "B_FLAVOUR=PROD: clang-cl /O2 /Gy; lld-link /OPT:REF /OPT:ICF"
)

REM One response file holds every compile flag; the driver adds /c, the source
REM and /Fo per process.
set "B_FLAGS_RSP=%B_OBJDIR%\clang_flags.rsp"
> "%B_FLAGS_RSP%" (
    echo !CLANG_COMMON! !B_OPTFLAGS! !B_DEFINES!
    echo /I"!KENSHILIB_HEADERS!\Include"
    echo /I"!KENSHILIB_HEADERS!\Include\ogre"
    echo /I"!BOOST_ROOT!"
    echo /Isrc
)
set "B_OBJS_RSP=%B_OBJDIR%\objs.rsp"
> "%B_OBJS_RSP%" (
    for /f "usebackq eol=# delims=" %%F in ("tools\build\coresrc.txt") do echo "%B_OBJDIR%\%%~nF.obj"
)

set "B_PDB=%B_OUTDIR%\KEO.pdb"
set "B_LINK="%LLD_LINK%" /nologo /DLL %B_LINKOPT% /MACHINE:X64 /SUBSYSTEM:CONSOLE /DEBUG /PDB:"%B_PDB%" /LIBPATH:"%KENSHILIB%\Libraries\KenshiLib" /LIBPATH:"%KENSHILIB%\Libraries\mygui" KenshiLib.lib MyGUIEngine_x64.lib user32.lib @"%B_OBJS_RSP%" /OUT:"%B_OUTDIR%\KEO.dll" /IMPLIB:"%B_OBJDIR%\KEO.lib""

echo === Building %B_LABEL% with clang-cl ===
echo Flavour: !B_FLAVOUR!
echo Headers: %KENSHILIB_HEADERS%\Include
echo Libraries: %KENSHILIB%\Libraries
echo Flags ^(clang_flags.rsp^):
type "%B_FLAGS_RSP%"
python tools\build\clang_compile.py --clang "%CLANG_CL%" --flags "%B_FLAGS_RSP%" --sources tools\build\coresrc.txt --objdir "%B_OBJDIR%" --log "%B_OBJDIR%\compile.log" --ehsc src\gui\settings_panel.cpp --ehsc src\render\gpu_upload.cpp
if errorlevel 1 goto :compile_failed

REM A stale PDB from an earlier link would satisfy the existence check below.
if exist "%B_PDB%" del /q "%B_PDB%"
echo LINK: !B_LINK!
echo   objs.rsp: @"%B_OBJS_RSP%"
!B_LINK!
if errorlevel 1 goto :link_failed
if not exist "%B_PDB%" goto :link_failed

python tools\kenshilib\check_imports.py "%B_OUTDIR%\KEO.dll"
if errorlevel 1 goto :import_failed
python tools\build\check_crt_imports.py "%B_OUTDIR%\KEO.dll"
if errorlevel 1 goto :crt_failed

set "B_SEH_ALLOW="
if "%B_ISDEV%"=="NO" set "B_SEH_ALLOW=--allow-absent ReadCallFacts --allow-absent ReadCollectionNow --allow-absent ReadConnNodeIndex --allow-absent ReadInstanceFacts --allow-absent ReadMutexWord --allow-absent ReadNodeMapSize --allow-absent ReadPlayerTaskSnap --allow-absent ReadSetFacts --allow-absent ResolveOpposite --allow-absent SafeRead16 --allow-absent ScanClearanceBody --allow-absent ScanCutBody"
python tools\build\check_seh_handlers.py "%B_OUTDIR%\KEO.dll" --sources tools\build\coresrc.txt --expect ReadPointerGuarded --expect ReadU32Guarded --expect ReadGameBytes16 --expect CallOriginalTracked !B_SEH_ALLOW!
if errorlevel 1 goto :seh_failed

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

:compile_failed
echo.
echo %B_LABEL% COMPILE FAILED
exit /b 1
:link_failed
echo.
echo %B_LABEL% LINK FAILED
exit /b 1
:import_failed
echo.
echo %B_LABEL% IMPORT CHECK FAILED
exit /b 1
:crt_failed
echo.
echo %B_LABEL% CRT IMPORT CHECK FAILED
exit /b 1
:seh_failed
echo.
echo %B_LABEL% SEH HANDLER CHECK FAILED
exit /b 1
