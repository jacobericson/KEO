@echo off
REM Shared clang-cl build prerequisites; deliberately no setlocal (exports
REM settings). Run after vcvarsall amd64 and tools\kenshilib\build_env.bat.
REM
REM Sets:
REM   LLVM_BIN           the LLVM bin folder (default C:\Program Files\LLVM\bin)
REM   CLANG_CL, LLD_LINK the two tools in it
REM   KENSHILIB_HEADERS  the KenshiLib tree whose Include\ is compiled against: the pinned tree
REM                      named by dependency-baseline.json's clang_headers block unless set;
REM                      tools\kenshilib\check_headers_pin.py refuses any other. Libraries always
REM                      come from KENSHILIB (build_env.bat checks it holds KenshiLib.lib).
REM   CLANG_COMMON       the flags every clang unit shares
REM
REM The compile runs inside the VS 2010 environment, so clang-cl takes its
REM system headers from INCLUDE (VS 2010 VC\INCLUDE and Windows SDK 7.0A) and
REM lld-link its libraries from LIB (msvcrt.lib/msvcprt.lib: msvcr100 and
REM msvcp100 at run time). -fms-compatibility-version=16.00 makes _MSC_VER
REM 1600, which those headers and the sources test, and turns off thread-safe
REM statics, which msvcr100 cannot support.
REM
REM /EHa is required, not a preference: under /EHsc clang gives a __try whose
REM body makes no call (a guarded plain load) no exception handler, so the
REM access violation it exists to catch ends the process.
REM
REM The three Boost defines are header-only configuration for Boost 1.60 under
REM clang, reached through KenshiLib's headers (boost/thread): Boost stops
REM redeclaring Win32 prototypes, stops using the type-trait intrinsics that
REM name std::is_assignable (absent from VS 2010's library), and stops using
REM constexpr the VS 2010 library cannot honour. No Boost type changes layout.
REM
REM -mcx16: _InterlockedCompareExchange128 is one cmpxchg16b under cl.exe;
REM clang's default x86-64 target lacks the feature and calls
REM __atomic_compare_exchange_16 instead, which no VS 2010 library provides.
if "%LLVM_BIN%"=="" set "LLVM_BIN=C:\Program Files\LLVM\bin"
set "CLANG_CL=%LLVM_BIN%\clang-cl.exe"
set "LLD_LINK=%LLVM_BIN%\lld-link.exe"
if not exist "%CLANG_CL%" goto :no_llvm
if not exist "%LLD_LINK%" goto :no_llvm
if not defined INCLUDE goto :no_vs2010
if "%KENSHILIB_HEADERS%"=="" for /f "usebackq delims=" %%P in (`python tools\kenshilib\check_headers_pin.py --print-path`) do set "KENSHILIB_HEADERS=%%P"
if "%KENSHILIB_HEADERS%"=="" goto :no_headers
python tools\kenshilib\check_headers_pin.py --headers "%KENSHILIB_HEADERS%"
if errorlevel 1 exit /b 1
if not exist "%KENSHILIB_HEADERS%\Include\kenshi\GameWorld.h" goto :no_headers
set "CLANG_COMMON=-fms-compatibility-version=16.00 -mcx16 /nologo /EHa /O2 /MD /W3 /Z7 /DNDEBUG /DWIN32_LEAN_AND_MEAN /DBOOST_ALL_NO_LIB /DBOOST_ERROR_CODE_HEADER_ONLY /DBOOST_SYSTEM_NO_DEPRECATED /DBOOST_USE_WINDOWS_H /DBOOST_TT_DISABLE_INTRINSICS /DBOOST_NO_CXX11_CONSTEXPR"
exit /b 0

:no_llvm
echo ERROR: clang-cl.exe or lld-link.exe not found in LLVM_BIN=%LLVM_BIN%
exit /b 1
:no_vs2010
echo ERROR: INCLUDE is empty. Run VS 2010 vcvarsall amd64 first, or clang-cl finds a later Visual Studio's headers.
exit /b 1
:no_headers
echo ERROR: KENSHILIB_HEADERS=%KENSHILIB_HEADERS% has no Include\kenshi\GameWorld.h
exit /b 1
