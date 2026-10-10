# SPDX-License-Identifier: MIT
# Copyright (c) 2026-Present Robert Campbell

<#
build-export-templates.ps1 - build the WINDOWS export template (a prebuilt runtime bundle:
Engine.Player + its runtime sidecars + template.xml). Sibling of build-export-templates.sh
(which does Linux + Web); this covers the Win64 template that cannot be built on Linux.

A template is NOT game content and NOT a toolchain - it is the prebuilt runtime you ship
against. See Documentation/Systems/export-templates.md.

What it does: configure the RelWithDebInfo preset for -Compiler if its build dir is missing
(msvc-reldbg, or clang-reldbg: clang++ on the MSVC ABI - the tree build-editor-dist.ps1 builds, so a
release builds the engine once), build Engine.Player (+ the native Tools.Export), then run
    Tools.Export.exe --template create Bin\RelWithDebInfo\Win64-<Compiler>
which reads the build's runtime-libs manifest, copies the player + sidecars, writes template.xml.
The player is optimised; its PDB (not part of the template) goes to <Out>\win64-release-symbols, to
keep with the build. The template's config, and so its id, is RelWithDebInfo
(gameengine-win64-relwithdebinfo-<version>).

  Usage:   pwsh scripts/build-export-templates.ps1 [-Out <dir>] [-Jobs <n>] [-Compiler MSVC|Clang]
           -Out <dir>   write a self-contained bundle under <dir> (for zip/distribution);
                        omitted = --install into the local templates root (usable immediately)
           -Jobs <n>    build parallelism (default 4)
           -Compiler    MSVC (default) or Clang: picks the preset, and so the Bin\RelWithDebInfo\Win64-<Compiler>
                        folder packaged - the build and the package cannot disagree

  Run from a Developer PowerShell / VS dev environment so cl or clang++ and ninja are on PATH.
  On Windows, confirm:
    * The Win64 runtime sidecars stage: the DXC runtime (dxcompiler.dll) and wgpu-native are
      dlopen/runtime deps (see export-templates.md and the dxc-runtime-sidecar note). Confirm they
      land in Engine.Player.runtime-libs and beside the exe (rpath is POSIX-only; on Windows it is
      the DLL search path / same directory).
    * Tools.Export.exe --template list shows the new win64 RelWithDebInfo template.
#>
[CmdletBinding()]
param(
    [string]$Out = "",
    [int]$Jobs = 4,
    [ValidateSet("MSVC", "Clang")]
    [string]$Compiler = "MSVC"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot   # repo root (scripts/..)
Set-Location $Root

# One choice names the preset, its build tree and the Bin folder it writes (CMakeLists.txt:
# Bin/<Config>/<Platform>-<CMAKE_CXX_COMPILER_ID>).
$Preset   = if ($Compiler -eq "MSVC") { "msvc-reldbg" } else { "clang-reldbg" }
$BuildDir = "build/$Preset"
$Plat     = "Win64"
$BinDir   = "Bin/RelWithDebInfo/$Plat-$Compiler"
$Exporter = Join-Path $BinDir "Tools.Export.exe"

Write-Host "== Windows ($Compiler, RelWithDebInfo: $Preset) template ==" -ForegroundColor Cyan

# 1) Configure the preset if its build dir does not exist yet (Ninja + a dev environment).
if (-not (Test-Path (Join-Path $BuildDir "CMakeCache.txt"))) {
    Write-Host ">> configuring the $Preset preset"
    & cmake --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "configuring the $Preset preset failed" }
}

# 2) Build the player + the native exporter.
& cmake --build $BuildDir --target Engine.Player Tools.Export -j $Jobs
if ($LASTEXITCODE -ne 0) { throw "building $BuildDir failed" }

# 3) Package the build dir into a template.
if (-not (Test-Path $Exporter)) {
    throw "$Exporter not found - did the $Preset build produce it? (expected under $BinDir)"
}
if ($Out -ne "") {
    # --out writes the bundle FLAT into the folder; use a per-platform subfolder so it never
    # collides with the Linux/Web bundles when they share one -Out root.
    $dest = Join-Path $Out "win64-release"
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    & $Exporter --template create $BinDir --out $dest
    if ($LASTEXITCODE -ne 0) { throw "creating the template failed" }
    Write-Host "template written under: $dest  (zip the folder to distribute)"
    # The player's symbols beside the template, not in it: keep them with the build.
    $symbols = Join-Path $Out "win64-release-symbols"
    New-Item -ItemType Directory -Force -Path $symbols | Out-Null
    $pdb = Join-Path $BinDir "Engine.Player.pdb"
    if (Test-Path $pdb) {
        Copy-Item $pdb $symbols
        Write-Host "player symbols: $symbols"
    } else {
        Write-Warning "no Engine.Player.pdb in $BinDir - a crash in a game built on this template cannot be resolved"
    }
} else {
    & $Exporter --template create $BinDir --install
    if ($LASTEXITCODE -ne 0) { throw "creating the template failed" }
    Write-Host "template installed into the local templates root (Tools.Export.exe --template list to verify)"
}
