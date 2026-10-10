# scripts/

Repo helper scripts.

## build-export-templates.{sh,ps1} - build export templates

An **export template** is a portable, prebuilt RUNTIME bundle for one `(platform, config)`: the
`Engine.Player` executable + that build's runtime sidecars + a `template.xml` manifest. It is the
runtime you ship against - never game content, never editor code, never a toolchain. A project's
export preset references a template and adds the game half (cooked content + naming). Full design:
[`Documentation/Systems/export-templates.md`](../Documentation/Systems/export-templates.md).

These scripts build the player for each platform and run `Tools.Export --template create <buildDir>`
to package it. The published desktop templates are **RelWithDebInfo**, built from the tree the editor
distribution builds (`clang-reldbg`, `msvc-reldbg`), so a release builds the engine once: the
player is optimised, and its debug info goes to a `<platform>-release-symbols` folder beside the
template (the Linux player is stripped before packaging; the Windows PDB never enters the template).

### Linux + Web (from a Linux host)

```sh
scripts/build-export-templates.sh [linux|web|all]     # default: all
#   JOBS=N     build parallelism (default 4; higher OOMs the engine build)
#   OUT=<dir>  write each platform's bundle under <dir>/<platform>/ (to zip); unset = --install locally
```

- **Linux** builds `Engine.Player` + the native `Tools.Export` through the `clang-reldbg`
  preset (`build/clang-reldbg`, `Bin/RelWithDebInfo/Linux64-Clang`) and packages a stripped copy.
- **Web** builds `Engine.Player` in `build/wasm-shipping` (emscripten; needs `emcc` on PATH or
  `~/emsdk/emsdk_env.sh`) and the SAME native exporter synthesizes a `Web` template from the
  `.html`/`.js`/`.wasm` bundle. The wasm build dir must be emscripten-configured first (the script
  prints the one-time `emcmake cmake ...` line if it is missing).

### Windows (on a Windows agent)

```powershell
pwsh scripts/build-export-templates.ps1 [-Out <dir>] [-Jobs <n>] [-Compiler MSVC|Clang]
```

`-Compiler` picks the preset, `msvc-reldbg` (the default) or `clang-reldbg` (clang++ on the MSVC
ABI), and the `Bin/RelWithDebInfo/Win64-<Compiler>` folder it packages follows from the same choice, so
the build and the package cannot disagree. Run it from a Developer PowerShell / VS dev environment
so `cl` or `clang++` and `ninja` are on PATH, and confirm the Windows runtime sidecars (the DXC
runtime `dxcompiler.dll`, wgpu-native) stage into the output; the script's header lists the
specific things to confirm on Windows.

### Verify

```sh
Bin/RelWithDebInfo/Linux64-Clang/Tools.Export --template list
```

## build-editor-dist.{sh,ps1} - package the editor for download

Assemble a portable, unzip-and-run **editor** distribution: `Tools.Editor` + its runtime
sidecars (DXC - the editor cooks/recompiles shaders) + the `Data` root (`Assets` + the `.dataroot`
marker) beside the exe. `Data/Shaders` holds the cooked pack (`shaders.dpak`, the path the runtime
opens through the data mount), the shader sources (an export cooks the game's pack from them) and
the `.pack-first` marker, which makes the distributed editor start from the pack rather than
compiling the sources (a dev tree, without the marker, keeps compiling them, with hot reload).

Built **RelWithDebInfo** (`clang-reldbg`, `msvc-reldbg`): optimised, asserts kept (a development
tool; an assert names the bug), and the symbols split into a `<dist>-symbols` archive beside the
download (`Tools.Editor.debug` on Linux, the PDBs on Windows). Keep that archive with the build: a
crash in a shipped editor resolves against it, and only against the build that made it.
`FindDataRoot()` discovers `Data/` beside the executable and `$ORIGIN` on the RUNPATH finds the
sidecars, so the folder relocates to any machine. This is distinct from the export TEMPLATES
above: those package the game RUNTIME (`Engine.Player`); this packages the AUTHORING TOOL.

Not bundled: the Vulkan/GPU system runtime (the target machine's drivers + loader).

### Linux (from a Linux host)

```sh
scripts/build-editor-dist.sh
#   JOBS=N       build parallelism (default 4; higher OOMs the modules build)
#   OUT=<dir>    the dist folder (default: dist/Editor-Linux64)
#   FORMATS=".." shader-pack formats (default: spirv)
#   CXX_COMPILER / C_COMPILER   a versioned clang for the preset (clang++-21)
```

`OUT` is the LABEL; the version from `project(VERSION)` in the root CMakeLists is inserted before
the platform suffix, so the default produces `dist/Editor-<version>-Linux64/` and its `.tar.gz`
(e.g. `Editor-0.1.0-Linux64.tar.gz`) - the same version the binary reports via `--version`. Smoke
test on the build machine: `( cd dist/Editor-<version>-Linux64 && ./Tools.Editor --exit-after 3 )`.
The conclusive check is to unzip on a machine with **no source tree and no dev toolchain** and
confirm it launches.

### Windows (on a Windows agent)

```powershell
pwsh scripts/build-editor-dist.ps1 [-Out <dir>] [-Jobs <n>] [-Compiler MSVC|Clang] [-Formats "dxil spirv"]
```

`-Compiler` picks `msvc-reldbg` (the default) or `clang-reldbg`, and the
`Bin/RelWithDebInfo/Win64-<Compiler>` folder copied from follows from it. The pack is cooked for
both Windows backends by default (DXIL for DX12, SPIR-V for Vulkan). Run from a Developer
PowerShell so `cl` or `clang++` and `ninja` are on PATH; confirm `dxcompiler.dll`/`SDL3.dll` stage
and that `Tools.Editor.pdb` lands in the symbols archive.
