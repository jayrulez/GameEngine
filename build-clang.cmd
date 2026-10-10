@echo off
setlocal EnableDelayedExpansion
rem Configure + build a Clang preset on Windows (default `clang`).
rem
rem   build-clang.cmd                                      configure + build everything (Debug)
rem   build-clang.cmd --target UI                          build just that target
rem   build-clang.cmd -- -k 0                              keep going after failures (args after -- go to ninja)
rem   build-clang.cmd --preset clang-reldbg                a different Clang preset
rem   build-clang.cmd --preset clang-reldbg --target Tools.Editor
rem
rem --preset must come FIRST: clang, clang-shared, clang-reldbg, clang-release, clang-shipping.
rem
rem WHY THIS WRAPPER EXISTS: the preset asks for plain "clang++", and on a dev box that is
rem ambiguous - a standalone LLVM, a Swift toolchain and Visual Studio's own bundled copy can all
rem be on PATH, so which compiler builds the tree depends on PATH order. This pins one explicitly
rem (C, CXX and ASM - ASM is driven by the C++ driver, as CI does) so the choice is visible in the
rem log and stable across shells.
rem
rem Clang targets the MSVC ABI here and finds the MSVC toolchain + Windows SDK itself, so a
rem Developer Command Prompt is NOT required (unlike build-msvc.cmd). Run it from one anyway if
rem you need to pin which Visual Studio install clang builds against.
rem
rem Set BUILD_CLANG_PATH to an LLVM root (or the directory holding clang++.exe) to override the
rem search, e.g.  set "BUILD_CLANG_PATH=C:\Program Files\LLVM"

set "REPO=%~dp0"
if "%REPO:~-1%"=="\" set "REPO=%REPO:~0,-1%"

rem --- Arguments ------------------------------------------------------------------------
rem Flat (goto, not nested parentheses) on purpose: `exit /b 1` inside a NESTED block does not
rem propagate its code out of the script - a usage error would report failure and still exit 0.
set "PRESET=clang"
if /i not "%~1"=="--preset" goto preset_done
if "%~2"=="" goto no_preset_value
set "PRESET=%~2"
shift & shift
:preset_done

if /i not "%PRESET:~0,5%"=="clang" (
    echo [build-clang] '%PRESET%' is not a Clang preset. Use clang, clang-shared, clang-reldbg,
    echo [build-clang] clang-release or clang-shipping - or call cmake --preset directly.
    exit /b 1
)

rem %* ignores `shift`, so the tail to forward has to be rebuilt by hand. %1 (not %~1) keeps
rem each argument's own quoting intact.
set "BUILDARGS="
:collect_args
if "%~1"=="" goto args_done
set "BUILDARGS=%BUILDARGS% %1"
shift
goto collect_args
:args_done

rem --- Pick the compiler ----------------------------------------------------------------
set "CLANGXX="
if defined BUILD_CLANG_PATH (
    if exist "%BUILD_CLANG_PATH%\bin\clang++.exe" set "CLANGXX=%BUILD_CLANG_PATH%\bin\clang++.exe"
    if exist "%BUILD_CLANG_PATH%\clang++.exe" set "CLANGXX=%BUILD_CLANG_PATH%\clang++.exe"
    if not defined CLANGXX (
        echo [build-clang] BUILD_CLANG_PATH is set but no clang++.exe was found under it:
        echo [build-clang]   %BUILD_CLANG_PATH%
        exit /b 1
    )
)
rem A standalone LLVM install first: it is the daily driver and the newest of the candidates.
if not defined CLANGXX if exist "%ProgramFiles%\LLVM\bin\clang++.exe" set "CLANGXX=%ProgramFiles%\LLVM\bin\clang++.exe"
rem Otherwise whatever PATH resolves - reported below so an unexpected pick is obvious.
if not defined CLANGXX for /f "delims=" %%i in ('where clang++ 2^>nul') do if not defined CLANGXX set "CLANGXX=%%i"

if not defined CLANGXX (
    echo [build-clang] No clang++.exe found. Install LLVM ^(https://llvm.org/^) or set
    echo [build-clang]   BUILD_CLANG_PATH=^<LLVM root^>
    exit /b 1
)

rem The matching C driver sits beside the C++ one; SDL3 and other vendored C sources need it.
set "CLANGC=%CLANGXX:clang++.exe=clang.exe%"
if not exist "%CLANGC%" (
    echo [build-clang] Found "%CLANGXX%" but no clang.exe beside it ^(needed for vendored C^).
    exit /b 1
)

rem CMake stores paths with forward slashes; match that so the cache comparison below can hit.
set "CXX_FWD=%CLANGXX:\=/%"
set "CC_FWD=%CLANGC:\=/%"

echo [build-clang] Preset   : %PRESET%
echo [build-clang] Compiler : %CLANGXX%
rem usebackq (backquoted command, not a single-quoted string): the compiler path contains spaces,
rem and the plain for /f '...' form hands cmd `C:\Program` as the command. First line only - that
rem is the "clang version N" banner.
set "CLANGVER="
for /f "usebackq tokens=*" %%v in (`"%CLANGXX%" --version 2^>nul`) do if not defined CLANGVER set "CLANGVER=%%v"
if defined CLANGVER echo [build-clang] Version  : %CLANGVER%

rem --- Configure ------------------------------------------------------------------------
rem Every preset in CMakePresets.json uses binaryDir build/<preset name>, so the build tree is
rem derivable from the name - keep that true if a preset is ever added with a different dir.
rem A cached compiler that differs from the selected one must not keep being used, so wipe the
rem cache when they disagree (the compiler is pinned in the cache on first configure).
set "CACHE=%REPO%\build\%PRESET%\CMakeCache.txt"
if exist "%CACHE%" (
    findstr /c:"CMAKE_CXX_COMPILER:" "%CACHE%" | findstr /i /c:"%CXX_FWD%" >nul
    if errorlevel 1 (
        echo [build-clang] Cached compiler differs from the selected one - reconfiguring.
        del /q "%CACHE%"
    )
)

echo [build-clang] Configuring...
cmake --preset %PRESET% -S "%REPO%" -DCMAKE_C_COMPILER="%CC_FWD%" -DCMAKE_CXX_COMPILER="%CXX_FWD%" -DCMAKE_ASM_COMPILER="%CXX_FWD%"
if errorlevel 1 (
    echo [build-clang] Configure FAILED.
    exit /b 1
)

rem --- Build ----------------------------------------------------------------------------
echo [build-clang] Building...
cmake --build --preset %PRESET%%BUILDARGS%
if errorlevel 1 (
    echo [build-clang] Build FAILED.
    exit /b 1
)

rem Read the output dir back out of the cache rather than mapping preset -> path here: the
rem config and the suffix are both cache variables, so this cannot drift from CMakePresets.json.
set "CFG="
set "SFX="
for /f "tokens=2 delims==" %%v in ('findstr /b /c:"CMAKE_BUILD_TYPE:" "%CACHE%" 2^>nul') do set "CFG=%%v"
for /f "tokens=2 delims==" %%v in ('findstr /b /c:"BUILDSYSTEM_OUTPUT_SUFFIX:" "%CACHE%" 2^>nul') do set "SFX=%%v"
if defined CFG (
    echo [build-clang] OK - binaries in Bin\%CFG%\Win64-Clang%SFX%
) else (
    echo [build-clang] OK - build tree: build\%PRESET%
)
endlocal
exit /b 0

rem --- usage errors (kept out of the main flow so `exit /b` is never nested) --------------
:no_preset_value
echo [build-clang] --preset needs a preset name, e.g. --preset clang-reldbg.
exit /b 1
