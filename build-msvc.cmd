@echo off
setlocal EnableDelayedExpansion
rem Configure + build an MSVC preset (default `msvc`).
rem
rem MSVC needs its environment (cl.exe, the Windows SDK, ml64 for the AngelScript trampoline)
rem on PATH, and a CMake preset cannot establish that - hence this wrapper. Run it from a plain
rem shell; it finds Visual Studio itself.
rem
rem   build-msvc.cmd                                   configure + build everything (Debug)
rem   build-msvc.cmd --target UI                       build just that target
rem   build-msvc.cmd -- -k 0                           keep going after failures (args after -- go to ninja)
rem   build-msvc.cmd --preset msvc-reldbg              a different MSVC preset (RelWithDebInfo)
rem   build-msvc.cmd --preset msvc-reldbg --target Tools.Editor
rem
rem --preset must come FIRST and names a preset from CMakePresets.json: msvc, msvc-shared,
rem msvc-release, msvc-reldbg, msvc-shipping. Symbolized builds (msvc-reldbg) are what makes an
rem assert backtrace resolvable - a plain Release emits no PDB at all.
rem
rem Set BUILD_VS_PATH to an installation root to override the search entirely, e.g.
rem   set "BUILD_VS_PATH=C:\Program Files\Microsoft Visual Studio\18\Community"
rem
rem Remaining arguments are forwarded to the build step; configure always runs (a no-op when
rem nothing changed).

set "REPO=%~dp0"
if "%REPO:~-1%"=="\" set "REPO=%REPO:~0,-1%"

rem --- Arguments ------------------------------------------------------------------------
rem Flat (goto, not nested parentheses) on purpose: `exit /b 1` inside a NESTED block does not
rem propagate its code out of the script - the usage error reported failure and still exited 0.
set "PRESET=msvc"
if /i not "%~1"=="--preset" goto preset_done
if "%~2"=="" goto no_preset_value
set "PRESET=%~2"
shift & shift
:preset_done

rem This wrapper exists only to establish the MSVC environment, so refuse presets that do not
rem want it rather than silently configuring a clang/gcc tree inside a vcvars shell.
if /i not "%PRESET:~0,4%"=="msvc" (
    echo [build-msvc] '%PRESET%' is not an MSVC preset. Use msvc, msvc-shared, msvc-release,
    echo [build-msvc] msvc-reldbg or msvc-shipping - or call cmake --preset directly.
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

set "BEST_PATH="
set "BEST_VER="

rem --- Candidate installs ---------------------------------------------------------------
rem Two sources, because neither is sufficient alone: vswhere is the supported API but has been
rem observed here NOT to report a side-by-side VS 2026 (18.x) install that is present and
rem working, while a bare directory scan cannot tell which of several installs is newest.
rem Collect from both, then pick by MSVC TOOLSET version (14.51 > 14.44) rather than by
rem edition/year - the directory names use different schemes ("18" vs "2022") and do not sort.
if defined BUILD_VS_PATH (
    call :consider "%BUILD_VS_PATH%"
) else (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if exist "!VSWHERE!" (
        for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -all -prerelease -products * ^
                -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 ^
                -property installationPath 2^>nul`) do call :consider "%%i"
    )
    for %%R in ("%ProgramFiles%\Microsoft Visual Studio" "%ProgramFiles(x86)%\Microsoft Visual Studio") do (
        if exist "%%~R" for /d %%Y in ("%%~R\*") do (
            for /d %%E in ("%%~Y\*") do call :consider "%%~E"
        )
    )
)

if not defined BEST_PATH (
    echo [build-msvc] No Visual Studio install with the C++ x64 toolset was found.
    echo [build-msvc] Install the "Desktop development with C++" workload, or set
    echo [build-msvc]   BUILD_VS_PATH=^<installation root^>
    exit /b 1
)

rem --- Enter the MSVC environment -------------------------------------------------------
echo [build-msvc] Visual Studio: %BEST_PATH%
echo [build-msvc] MSVC toolset : %BEST_VER%
call "%BEST_PATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo [build-msvc] vcvars64.bat failed.
    exit /b 1
)

rem --- Configure ------------------------------------------------------------------------
rem The preset asks for "cl", which CMake resolves from PATH and then PINS in the cache. If the
rem selected toolchain differs from the cached one the build tree must be regenerated, or the
rem old compiler keeps being used - so detect that and wipe the cache.
rem Compare with FORWARD slashes: CMake stores CMAKE_CXX_COMPILER with '/', while BEST_PATH came
rem from the filesystem with '\'. Matching the raw path never hits, which silently wiped the cache
rem and forced a full rebuild on every single run.
rem Every preset in CMakePresets.json uses binaryDir build/<preset name>, so the build tree is
rem derivable from the name - keep that true if a preset is ever added with a different dir.
set "BEST_FWD=%BEST_PATH:\=/%"
set "CACHE=%REPO%\build\%PRESET%\CMakeCache.txt"
if exist "%CACHE%" (
    findstr /c:"CMAKE_CXX_COMPILER:STRING=" "%CACHE%" | findstr /i /c:"%BEST_FWD%" >nul
    if errorlevel 1 (
        echo [build-msvc] Cached compiler differs from the selected toolchain - reconfiguring.
        del /q "%CACHE%"
    )
)

echo [build-msvc] Preset       : %PRESET%
echo [build-msvc] Configuring...
cmake --preset %PRESET% -S "%REPO%"
if errorlevel 1 (
    echo [build-msvc] Configure FAILED.
    exit /b 1
)

rem --- Build ----------------------------------------------------------------------------
echo [build-msvc] Building...
cmake --build --preset %PRESET%%BUILDARGS%
if errorlevel 1 (
    echo [build-msvc] Build FAILED.
    exit /b 1
)

rem Bin\<config>\Win64-MSVC<suffix>, per preset. Hardcoded because the config and the output
rem suffix both come from the preset's cache variables; unknown presets just report the tree.
set "OUTDIR="
if /i "%PRESET%"=="msvc"          set "OUTDIR=Bin\Debug\Win64-MSVC"
if /i "%PRESET%"=="msvc-shared"   set "OUTDIR=Bin\Debug\Win64-MSVC-Shared"
if /i "%PRESET%"=="msvc-release"  set "OUTDIR=Bin\Release\Win64-MSVC"
if /i "%PRESET%"=="msvc-reldbg"   set "OUTDIR=Bin\RelWithDebInfo\Win64-MSVC"
if /i "%PRESET%"=="msvc-shipping" set "OUTDIR=Bin\Release\Win64-MSVC-Shipping"
if defined OUTDIR (
    echo [build-msvc] OK - binaries in %OUTDIR%
) else (
    echo [build-msvc] OK - build tree: build\%PRESET%
)
endlocal
exit /b 0

rem --- usage errors (kept out of the main flow so `exit /b` is never nested) --------------
:no_preset_value
echo [build-msvc] --preset needs a preset name, e.g. --preset msvc-reldbg.
exit /b 1

rem --- :consider <installation root> -----------------------------------------------------
rem Keeps the candidate with the highest MSVC toolset version. Toolset strings are 14.NN.BBBBB
rem with fixed-width components, so a plain string comparison orders them correctly.
:consider
set "CAND=%~1"
if not exist "%CAND%\VC\Auxiliary\Build\vcvars64.bat" goto :eof
set "VERFILE=%CAND%\VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt"
if not exist "%VERFILE%" goto :eof
set "CANDVER="
for /f "usebackq tokens=* delims= " %%v in ("%VERFILE%") do if not defined CANDVER set "CANDVER=%%v"
if not defined CANDVER goto :eof
if not defined BEST_VER (
    set "BEST_VER=%CANDVER%"
    set "BEST_PATH=%CAND%"
    goto :eof
)
if "%CANDVER%" GTR "%BEST_VER%" (
    set "BEST_VER=%CANDVER%"
    set "BEST_PATH=%CAND%"
)
goto :eof
