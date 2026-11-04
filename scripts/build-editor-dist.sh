#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026-Present Robert Campbell

#
# build-editor-dist.sh - assemble a portable, downloadable editor distribution for Linux.
#
# The result is an unzip-and-run folder: the Tools.Editor executable and the CLI tools (Tools.Cook,
# Tools.Export, Tools.Mcp), the engine's shared libraries they run on, their runtime sidecars (DXC,
# since the editor cooks and recompiles shaders), and the Data root (Engine, Editor, Branding,
# Templates and the .dataroot marker) beside the exe. Data/Engine/Shaders holds the cooked engine
# shader pack (shaders.dpak, the path the ShaderSystemHost opens), the shader sources (an export
# cooks the game's pack from them) and the .pack-first marker, so this editor starts from the pack
# and not by compiling the sources. FindDataRoot() discovers Data/ beside the exe, and $ORIGIN on
# the exe's RUNPATH finds the sidecars, so the folder relocates to any machine.
#
# Built RelWithDebInfo with the engine as shared libraries (the clang-reldbg-shared preset), so the
# editor and the tools load a project's own plugin packages built against the same engine:
# optimised, asserts kept (a development tool; an assert names the bug), and the debug info split
# off into <dist>-symbols.tar.gz (<binary>.debug for each executable and library, linked from the
# stripped one by .gnu_debuglink), so a crash in a shipped editor can be resolved against the build
# that made it.
#
# Not bundled: Vulkan (a system dependency: the target needs GPU drivers and the Vulkan loader,
# the same as the build dependencies).
#
# Windows is not built here (no toolchain on Linux); use scripts/build-editor-dist.ps1 on a
# Windows agent for the Win64 editor dist.
#
#   Usage:  scripts/build-editor-dist.sh
#   Env:    JOBS=N     build parallelism (default 4; higher OOMs the modules build)
#           OUT=<dir>  the dist folder (default: dist/Editor-Linux64)
#           FORMATS=".." shader-pack formats (default: spirv, the Linux/Vulkan backend)
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

JOBS="${JOBS:-4}"
FORMATS="${FORMATS:-spirv}"
PRESET="clang-reldbg-shared"                 # RelWithDebInfo, shared engine: $ORIGIN rpath, the libraries beside the editor
BUILD="build/$PRESET"
BIN="Bin/RelWithDebInfo/Linux64-Clang-Shared" # Bin/<Config>/<Platform>-<Compiler><suffix>, from the preset
TOOLS=(Tools.Editor Tools.Cook Tools.Export Tools.Mcp) # what the dist runs

# Version stamp: the one source of truth is project(VERSION) in the root CMakeLists (the same
# value the binary reports with --version). It is folded into the dist folder and archive names so
# a download identifies itself (Editor-0.1.0-Linux64). Callers pass OUT as the label
# (Editor-Linux64); the version is inserted before the platform suffix.
VERSION="$(grep -oP '^\s*VERSION\s+\K[0-9]+\.[0-9]+\.[0-9]+' CMakeLists.txt | head -1)"
VERSION="${VERSION:-0.0.0}"
RAW="${OUT:-dist/Editor-Linux64}"
RAW_DIR="$(dirname "$RAW")"
RAW_NAME="$(basename "$RAW")"                # e.g. Editor-Linux64
DIST="$RAW_DIR/${RAW_NAME%-*}-${VERSION}-${RAW_NAME##*-}"  # e.g. dist/Editor-0.1.0-Linux64

log() { printf '\n== %s ==\n' "$*"; }

# 1. Build the editor, the CLI tools and the shader-pack cooker (the preset: clang, RelWithDebInfo, shared).
log "Building ${TOOLS[*]} + Tools.ShaderPack ($PRESET)"
if [[ ! -f "$BUILD/CMakeCache.txt" ]]; then
    # The preset pins clang (the Bin/ folder is named for CMAKE_CXX_COMPILER_ID, so a default GCC
    # configure would build into Linux64-GCC and every cp below would fail). CXX_COMPILER env
    # overrides it for runners that install a versioned clang (clang++-21).
    CXX_BIN="${CXX_COMPILER:-clang++}"
    C_BIN="${C_COMPILER:-clang}"
    cmake --preset "$PRESET" -DCMAKE_C_COMPILER="$C_BIN" -DCMAKE_CXX_COMPILER="$CXX_BIN" \
        -DCMAKE_ASM_COMPILER="$CXX_BIN"
fi
cmake --build "$BUILD" --target "${TOOLS[@]}" Tools.ShaderPack -j"$JOBS"

# 2. Fresh dist tree.
log "Staging into $DIST"
SYMBOLS="${DIST}-symbols"
rm -rf "$DIST" "$SYMBOLS"
mkdir -p "$DIST/Data" "$SYMBOLS"

# 3. The executables, the engine's shared libraries they load (those ldd resolves inside the build's
#    Bin folder, through their $ORIGIN rpath), and the editor's runtime sidecars (the .runtime-libs
#    manifest lists DXC on Linux).
BIN_ABS="$(cd "$BIN" && pwd)"
for tool in "${TOOLS[@]}"; do
    cp "$BIN/$tool" "$DIST/"
    while IFS= read -r lib; do
        [[ -n "$lib" && ! -f "$DIST/$(basename "$lib")" ]] && cp "$lib" "$DIST/"
    done < <(ldd "$BIN/$tool" | awk -v bin="$BIN_ABS/" 'index($3, bin) == 1 { print $3 }')
done
if [[ -f "$BIN/Tools.Editor.runtime-libs" ]]; then
    while IFS= read -r lib; do
        [[ -n "$lib" ]] || continue
        if [[ -f "$BIN/$lib" ]]; then
            cp "$BIN/$lib" "$DIST/"
        else
            echo "!! sidecar listed but not found in $BIN: $lib" >&2
        fi
    done < "$BIN/Tools.Editor.runtime-libs"
fi
# Copy DXC directly as well, in case the manifest omits it (the editor cooks shaders, so it needs it).
[[ -f "$BIN/libdxcompiler.so" && ! -f "$DIST/libdxcompiler.so" ]] && cp "$BIN/libdxcompiler.so" "$DIST/"
# The debug info to the symbols folder, each executable and engine library stripped and linked to its
# own by name (DXC ships as vendored, untouched).
for binary in "${TOOLS[@]/#/$DIST/}" "$DIST"/lib*.so; do
    [[ -f "$binary" ]] || continue
    name="$(basename "$binary")"
    [[ "$name" == libdxcompiler.so || "$name" == libdxil.so ]] && continue
    objcopy --only-keep-debug "$binary" "$SYMBOLS/$name.debug"
    strip --strip-debug "$binary"
    objcopy --add-gnu-debuglink="$SYMBOLS/$name.debug" "$binary"
done

# 4. The data the editor reads, whole folders: Engine (what the runtime
#    reads: the shader sources an export cooks a game's pack from, the GUI's font), Editor (its
#    sheets, page markup, icons, fonts and code languages; without its frame the editor runs with no
#    interface), Branding (the splash, the logo and the window icons) and Templates (what a new
#    project starts with), and the marker. Not Samples, Tests, SampleProjects or Output, and not
#    the GUISandbox's design mock under Editor.
cp -r "Data/Engine" "$DIST/Data/Engine"
cp -r "Data/Editor" "$DIST/Data/Editor"
cp -r "Data/Branding" "$DIST/Data/Branding"
cp -r "Data/Templates" "$DIST/Data/Templates"
rm -rf "$DIST/Data/Editor/Mock"
cp "Data/.dataroot" "$DIST/Data/.dataroot"

# 5. Data/Engine/Shaders: the pack into the dist's data root (Data/Engine/Shaders/shaders.dpak is the
#    one path the runtime opens; a pack beside the exe is never found), and the marker that makes
#    this editor start from the pack (the sources beside it came with Data/Engine).
log "Cooking Data/Engine/Shaders/shaders.dpak ($FORMATS)"
"$BIN/Tools.ShaderPack" "Data/Engine/Shaders" "$DIST/Data/Engine/Shaders/shaders.dpak" $FORMATS
echo "An editor distribution: start from shaders.dpak; the sources are for cooking." \
    > "$DIST/Data/Engine/Shaders/.pack-first"

# 6. A short run note next to the binary.
cat > "$DIST/README.txt" <<'EOF'
Editor (Linux x86_64)

Run:  ./Tools.Editor

The command-line tools beside it run on the same engine: ./Tools.Cook <project> cooks,
./Tools.Export <project> exports, and ./Tools.Mcp serves the engine's MCP tools over stdio.

Requires GPU drivers with Vulkan support and the Vulkan loader installed
(e.g. on Ubuntu:  sudo apt install libvulkan1 mesa-vulkan-drivers).

Everything else (fonts, assets, shader pack and sources, DXC) ships in this folder
and is found relative to the executable - the folder can be moved anywhere.
EOF

# 7. Package.
TARBALL="${DIST}.tar.gz"
SYMBOLS_TARBALL="${SYMBOLS}.tar.gz"
log "Packaging $TARBALL and $SYMBOLS_TARBALL"
tar -czf "$TARBALL" -C "$(dirname "$DIST")" "$(basename "$DIST")"
tar -czf "$SYMBOLS_TARBALL" -C "$(dirname "$SYMBOLS")" "$(basename "$SYMBOLS")"

log "done"
echo "editor dist folder: $DIST"
echo "editor dist archive: $TARBALL"
echo "editor symbols:      $SYMBOLS_TARBALL (keep it with the build; not for download)"
echo "smoke test on THIS machine:  ( cd '$DIST' && ./Tools.Editor --exit-after 3 )"
