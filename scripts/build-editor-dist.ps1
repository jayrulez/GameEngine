# SPDX-License-Identifier: MIT
# Copyright (c) 2026-Present Robert Campbell

<#
.SYNOPSIS
  Assemble a portable, downloadable EDITOR distribution for Windows (x64).

.DESCRIPTION
  The Windows counterpart of build-editor-dist.sh. Produces an unzip-and-run folder:
  Tools.Editor.exe + its runtime DLLs (dxcompiler.dll, SDL3.dll - staged by the build's
  util_copy_runtime_deps into Bin\...\ and listed in Tools.Editor.runtime-libs), and the Data root
  (Assets + .dataroot) beside the exe. Data\Shaders holds the cooked shader pack (shaders.dpak, the
  path the runtime opens), the shader SOURCES (an export cooks the game's pack from them) and the
  .pack-first marker, so this editor starts from the pack and not by compiling the sources.
  FindDataRoot() discovers Data\ beside the exe; Windows searches the exe directory for bare DLLs.

  Built RelWithDebInfo through a preset: optimised, asserts kept (it is a development tool, and an
  assert names the bug), and symbols - the PDBs go into a separate <dist>-symbols.zip, so a crash
  in a shipped editor can be resolved against the build that made it. The preset follows
  -Compiler, and so does the Bin\RelWithDebInfo\Win64-<Compiler> folder copied from: the build and
  the copy cannot disagree.

  NOT bundled: the Vulkan/DX12 system runtime (the target machine's GPU drivers).

  Run from a Developer PowerShell / VS dev environment so cl or clang++ and ninja are on PATH.

.PARAMETER Out
  The dist folder label (default: dist\Editor-Win64; the version goes before the platform).
.PARAMETER Jobs
  Build parallelism (default 4; higher can OOM the modules build).
.PARAMETER Compiler
  MSVC (the msvc-reldbg preset) or Clang (clang-reldbg: clang++ on the MSVC ABI). Default MSVC.
.PARAMETER Formats
  Shader-pack formats (default "dxil spirv": the editor on Windows runs DX12 or Vulkan).
#>
param(
    [string]$Out = "dist\Editor-Win64",
    [int]$Jobs = 4,
    [ValidateSet("MSVC", "Clang")]
    [string]$Compiler = "MSVC",
    [string]$Formats = "dxil spirv"
)
$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
Set-Location $Root

# Version stamp: the SINGLE source of truth is project(VERSION) in the root CMakeLists (the same
# value the binary reports via --version). Folded into the dist folder + zip name so a download
# self-identifies (Editor-0.1.0-Win64). $Out is the LABEL; the version goes before the platform.
$cmake = Get-Content "CMakeLists.txt" -Raw
$Version = if ($cmake -match 'project\(\s*GameEngine\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') { $Matches[1] } else { "0.0.0" }
$rawDir  = Split-Path -Parent $Out
$rawName = Split-Path -Leaf   $Out           # e.g. Editor-Win64
$prefix  = $rawName.Substring(0, $rawName.LastIndexOf('-'))
$suffix  = $rawName.Substring($rawName.LastIndexOf('-') + 1)
$Out     = Join-Path $rawDir "$prefix-$Version-$suffix"   # e.g. dist\Editor-0.1.0-Win64

# One choice names the preset, its build tree and the Bin folder it writes (CMakeLists.txt:
# Bin/<Config>/<Platform>-<CMAKE_CXX_COMPILER_ID>).
$Preset = if ($Compiler -eq "MSVC") { "msvc-reldbg" } else { "clang-reldbg" }
$Build  = "build\$Preset"
$Bin    = "Bin\RelWithDebInfo\Win64-$Compiler"

Write-Host "== Building Tools.Editor + Tools.ShaderPack ($Preset) =="
if (-not (Test-Path (Join-Path $Build "CMakeCache.txt"))) {
    cmake --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "configuring the $Preset preset failed" }
}
cmake --build $Build --target Tools.Editor Tools.ShaderPack -j $Jobs
if ($LASTEXITCODE -ne 0) { throw "building $Build failed" }
if (-not (Test-Path (Join-Path $Bin "Tools.Editor.exe"))) {
    throw "$Bin\Tools.Editor.exe not found - the $Preset build writes somewhere else?"
}

Write-Host "== Staging into $Out =="
$Symbols = "$Out-symbols"
foreach ($dir in @($Out, $Symbols)) {
    if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
}
New-Item -ItemType Directory -Force -Path (Join-Path $Out "Data") | Out-Null
New-Item -ItemType Directory -Force -Path $Symbols | Out-Null

# Editor exe + its runtime DLLs (from the .runtime-libs manifest); each one's PDB, when the build
# made one, to the symbols folder.
$staged = @("Tools.Editor.exe")
Copy-Item (Join-Path $Bin "Tools.Editor.exe") $Out
$manifest = Join-Path $Bin "Tools.Editor.runtime-libs"
if (Test-Path $manifest) {
    Get-Content $manifest | Where-Object { $_ -ne "" } | ForEach-Object {
        $src = Join-Path $Bin $_
        if (Test-Path $src) { Copy-Item $src $Out; $staged += $_ } else { Write-Warning "sidecar missing: $_" }
    }
}
foreach ($file in $staged) {
    $pdb = Join-Path $Bin ([IO.Path]::ChangeExtension($file, ".pdb"))
    if (Test-Path $pdb) { Copy-Item $pdb $Symbols }
}
if (-not (Test-Path (Join-Path $Symbols "Tools.Editor.pdb"))) {
    Write-Warning "no Tools.Editor.pdb in $Bin - a crash in this dist cannot be resolved"
}

# Data\Shaders: the pack goes INTO the dist's data root (Data\Shaders\shaders.dpak is the one path
# the runtime opens), cooked for both Windows backends; the sources beside it, for an export to cook
# a game's pack from; and the marker that makes this editor start from the pack.
Write-Host "== Cooking Data\Shaders\shaders.dpak ($Formats) =="
$Shaders = Join-Path $Out "Data\Shaders"
New-Item -ItemType Directory -Force -Path $Shaders | Out-Null
& (Join-Path $Bin "Tools.ShaderPack.exe") "Data\Shaders" (Join-Path $Shaders "shaders.dpak") $Formats.Split(" ")
if ($LASTEXITCODE -ne 0) { throw "cooking the shader pack failed" }
Get-ChildItem "Data\Shaders" -Recurse -File -Include *.hlsl, *.hlsli | ForEach-Object {
    $relative = $_.FullName.Substring((Resolve-Path "Data\Shaders").Path.Length + 1)
    $target = Join-Path $Shaders $relative
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
    Copy-Item $_.FullName $target
}
Set-Content (Join-Path $Shaders ".pack-first") "An editor distribution: start from shaders.dpak; the sources are for cooking."

Copy-Item -Recurse "Data\Assets"   (Join-Path $Out "Data\Assets")
Copy-Item          "Data\.dataroot" (Join-Path $Out "Data\.dataroot")

@"
Editor (Windows x64)

Run:  Tools.Editor.exe

Requires GPU drivers with Vulkan or DX12 support. Everything else (fonts, assets,
shader pack and sources, dxcompiler.dll) ships in this folder and is found relative to the exe.
"@ | Set-Content (Join-Path $Out "README.txt")

$Zip = "$Out.zip"
$SymbolsZip = "$Symbols.zip"
Write-Host "== Packaging $Zip and $SymbolsZip =="
foreach ($file in @($Zip, $SymbolsZip)) {
    if (Test-Path $file) { Remove-Item -Force $file }
}
Compress-Archive -Path $Out -DestinationPath $Zip
Compress-Archive -Path $Symbols -DestinationPath $SymbolsZip

Write-Host "editor dist folder:   $Out"
Write-Host "editor dist archive:  $Zip"
Write-Host "editor symbols:       $SymbolsZip (keep it with the build; not for download)"
