#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026-Present Robert Campbell

#
# build-editor-dist.sh - assemble a portable, downloadable EDITOR distribution for Linux.
#
# The result is an unzip-and-run folder: the Tools.Editor executable, its runtime sidecars
# (DXC - the editor cooks/recompiles shaders), the Data root (Assets + the .dataroot marker)
# beside the exe. Data/Shaders holds the cooked engine shader pack (shaders.dpak, the path the
# ShaderSystemHost opens), the shader SOURCES (an export cooks the game's pack from them) and the
# .pack-first marker, so this editor starts from the pack and not by compiling the sources.
# FindDataRoot() discovers Data/ beside the exe, and $ORIGIN on the exe's RUNPATH finds the
# sidecars - so the folder relocates to any machine.
#
# Built RelWithDebInfo (the clang-reldbg preset): optimised, asserts kept (a development tool; an
# assert names the bug), and the debug info split off into <dist>-symbols.tar.gz
# (Tools.Editor.debug, linked from the stripped exe by .gnu_debuglink), so a crash in a shipped
# editor can be resolved against the build that made it.
#
# NOT bundled: Vulkan (a system dependency - the target needs GPU drivers + the Vulkan loader,
# same as the build deps in README.md).
#
# Windows is not built here (no toolchain on Linux) - use scripts/build-editor-dist.ps1 on a
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
PRESET="clang-reldbg"                        # RelWithDebInfo: $ORIGIN rpath + DXC beside the editor
BUILD="build/$PRESET"
BIN="Bin/RelWithDebInfo/Linux64-Clang"       # Bin/<Config>/<Platform>-<Compiler>, from the preset

# Version stamp: the SINGLE source of truth is project(VERSION) in the root CMakeLists (the same
# value the binary reports via --version). Folded into the dist folder + archive name so a
# download self-identifies (Editor-0.1.0-Linux64). Callers pass OUT as the LABEL (Editor-Linux64);
# the version is inserted before the platform suffix.
VERSION="$(grep -oP '^\s*VERSION\s+\K[0-9]+\.[0-9]+\.[0-9]+' CMakeLists.txt | head -1)"
VERSION="${VERSION:-0.0.0}"
RAW="${OUT:-dist/Editor-Linux64}"
RAW_DIR="$(dirname "$RAW")"
RAW_NAME="$(basename "$RAW")"                # e.g. Editor-Linux64
DIST="$RAW_DIR/${RAW_NAME%-*}-${VERSION}-${RAW_NAME##*-}"  # e.g. dist/Editor-0.1.0-Linux64

log() { printf '\n== %s ==\n' "$*"; }

# 1. Build the editor + the shader-pack cooker (the preset: clang, RelWithDebInfo).
log "Building Tools.Editor + Tools.ShaderPack ($PRESET)"
if [[ ! -f "$BUILD/CMakeCache.txt" ]]; then
    # The preset pins clang (the Bin/ folder is named for CMAKE_CXX_COMPILER_ID, so a default GCC
    # configure would build into Linux64-GCC and every cp below would fail). CXX_COMPILER env
    # overrides it for runners that install a versioned clang (clang++-21).
    CXX_BIN="${CXX_COMPILER:-clang++}"
    C_BIN="${C_COMPILER:-clang}"
    cmake --preset "$PRESET" -DCMAKE_C_COMPILER="$C_BIN" -DCMAKE_CXX_COMPILER="$CXX_BIN" \
        -DCMAKE_ASM_COMPILER="$CXX_BIN"
fi
cmake --build "$BUILD" --target Tools.Editor Tools.ShaderPack -j"$JOBS"

# 2. Fresh dist tree.
log "Staging into $DIST"
SYMBOLS="${DIST}-symbols"
rm -rf "$DIST" "$SYMBOLS"
mkdir -p "$DIST/Data" "$SYMBOLS"

# 3. The editor exe + its runtime sidecars (the .runtime-libs manifest lists DXC on Linux).
cp "$BIN/Tools.Editor" "$DIST/"
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
# The debug info to the symbols folder, the exe stripped and linked to it by name.
objcopy --only-keep-debug "$DIST/Tools.Editor" "$SYMBOLS/Tools.Editor.debug"
strip --strip-debug "$DIST/Tools.Editor"
objcopy --add-gnu-debuglink="$SYMBOLS/Tools.Editor.debug" "$DIST/Tools.Editor"

# 4. Data/Shaders: the pack INTO the dist's data root (Data/Shaders/shaders.dpak is the one path
#    the runtime opens; a pack beside the exe is never found); the sources beside it, for an
#    export to cook a game's pack from; and the marker that makes this editor start from the pack.
log "Cooking Data/Shaders/shaders.dpak ($FORMATS)"
mkdir -p "$DIST/Data/Shaders"
"$BIN/Tools.ShaderPack" "Data/Shaders" "$DIST/Data/Shaders/shaders.dpak" $FORMATS
SHADERS_OUT="$(cd "$DIST/Data/Shaders" && pwd)"
( cd Data/Shaders && find . -type f \( -name '*.hlsl' -o -name '*.hlsli' \) -print0 |
    xargs -0 -I{} cp --parents {} "$SHADERS_OUT/" )
echo "An editor distribution: start from shaders.dpak; the sources are for cooking." \
    > "$DIST/Data/Shaders/.pack-first"

# 5. Stage the rest of the Data root: Assets + the marker. Skip Data/Output (build output).
cp -r "Data/Assets" "$DIST/Data/Assets"
cp "Data/.dataroot" "$DIST/Data/.dataroot"

# 6. A short run note next to the binary.
cat > "$DIST/README.txt" <<'EOF'
Editor (Linux x86_64)

Run:  ./Tools.Editor

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
