#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026-Present Robert Campbell

#
# build-model-viewer-dist.sh - assemble a portable, downloadable MODEL VIEWER for Linux
# (Documentation/Specs/model-viewer.md, P5), as build-editor-dist.sh does the editor.
#
# The result is an unzip-and-run folder: the Tools.ModelViewer executable, its runtime sidecars,
# and the Data root it reads beside it: Engine (the shader sources and the cooked pack, the GUI's
# font), Editor (the editor's sheets, fonts and icons, which the viewer's look is made of), ModelViewer
# (its window's markup and sheet) and the .dataroot marker. FindDataRoot() discovers Data/ beside
# the exe, and $ORIGIN on the exe's RUNPATH finds the sidecars, so the folder relocates anywhere.
#
# Built RelWithDebInfo (the clang-reldbg preset), the debug info split off into
# <dist>-symbols.tar.gz (Tools.ModelViewer.debug, linked from the stripped exe by .gnu_debuglink).
#
# NOT bundled: Vulkan (a system dependency: GPU drivers and the Vulkan loader).
#
#   Usage:  scripts/build-model-viewer-dist.sh
#   Env:    JOBS=N     build parallelism (default 4; higher OOMs the modules build)
#           OUT=<dir>  the dist folder (default: dist/ModelViewer-Linux64)
#           FORMATS=".." shader-pack formats (default: spirv, the Linux/Vulkan backend)
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

JOBS="${JOBS:-4}"
FORMATS="${FORMATS:-spirv}"
PRESET="clang-reldbg"
BUILD="build/$PRESET"
BIN="Bin/RelWithDebInfo/Linux64-Clang"

# The version from project(VERSION) in the root CMakeLists, folded into the folder and archive
# names (ModelViewer-0.1.0-Linux64), as the editor's dist does.
VERSION="$(grep -oP '^\s*VERSION\s+\K[0-9]+\.[0-9]+\.[0-9]+' CMakeLists.txt | head -1)"
VERSION="${VERSION:-0.0.0}"
RAW="${OUT:-dist/ModelViewer-Linux64}"
RAW_DIR="$(dirname "$RAW")"
RAW_NAME="$(basename "$RAW")"
DIST="$RAW_DIR/${RAW_NAME%-*}-${VERSION}-${RAW_NAME##*-}"

log() { printf '\n== %s ==\n' "$*"; }

# 1. Build the viewer and the shader-pack cooker.
log "Building Tools.ModelViewer + Tools.ShaderPack ($PRESET)"
if [[ ! -f "$BUILD/CMakeCache.txt" ]]; then
    CXX_BIN="${CXX_COMPILER:-clang++}"
    C_BIN="${C_COMPILER:-clang}"
    cmake --preset "$PRESET" -DCMAKE_C_COMPILER="$C_BIN" -DCMAKE_CXX_COMPILER="$CXX_BIN" \
        -DCMAKE_ASM_COMPILER="$CXX_BIN"
fi
cmake --build "$BUILD" --target Tools.ModelViewer Tools.ShaderPack -j"$JOBS"

# 2. Fresh dist tree.
log "Staging into $DIST"
SYMBOLS="${DIST}-symbols"
rm -rf "$DIST" "$SYMBOLS"
mkdir -p "$DIST/Data" "$SYMBOLS"

# 3. The exe and its runtime sidecars (the .runtime-libs manifest), the debug info split off.
cp "$BIN/Tools.ModelViewer" "$DIST/"
if [[ -f "$BIN/Tools.ModelViewer.runtime-libs" ]]; then
    while IFS= read -r lib; do
        [[ -n "$lib" ]] || continue
        if [[ -f "$BIN/$lib" ]]; then
            cp "$BIN/$lib" "$DIST/"
        else
            echo "!! sidecar listed but not found in $BIN: $lib" >&2
        fi
    done < "$BIN/Tools.ModelViewer.runtime-libs"
fi
objcopy --only-keep-debug "$DIST/Tools.ModelViewer" "$SYMBOLS/Tools.ModelViewer.debug"
strip --strip-debug "$DIST/Tools.ModelViewer"
objcopy --add-gnu-debuglink="$SYMBOLS/Tools.ModelViewer.debug" "$DIST/Tools.ModelViewer"

# 4. The data it reads, whole folders: Engine, Editor (its sheets, fonts with their licences, and
#    icons; not the GUI sandbox's mock) and ModelViewer, and the marker.
cp -r "Data/Engine" "$DIST/Data/Engine"
cp -r "Data/Editor" "$DIST/Data/Editor"
rm -rf "$DIST/Data/Editor/Mock"
cp -r "Data/ModelViewer" "$DIST/Data/ModelViewer"
cp "Data/.dataroot" "$DIST/Data/.dataroot"
# The agent's guide beside the exe, where --mcp and --headless look first (docs://ModelViewerGuide.md).
cp "Documentation/Shipping/ModelViewerGuide.md" "$DIST/"

# 5. The shader pack into the dist's data root, and the marker that makes it start from the pack.
log "Cooking Data/Engine/Shaders/shaders.dpak ($FORMATS)"
"$BIN/Tools.ShaderPack" "Data/Engine/Shaders" "$DIST/Data/Engine/Shaders/shaders.dpak" $FORMATS
echo "A distribution: start from shaders.dpak; the sources are for cooking." \
    > "$DIST/Data/Engine/Shaders/.pack-first"

# 6. A short run note next to the binary.
cat > "$DIST/README.txt" <<'EOF'
Model Viewer (Linux x86_64)

Look at glTF, GLB, FBX and OBJ files without a project, play their animations,
browse a folder of models as thumbnails, and shortlist the ones you want.

Run:  ./Tools.ModelViewer [model files...] [--rest] [--skeleton] [--mcp]
      ./Tools.ModelViewer --headless

  --rest       open models in their rest pose, no clip playing
  --skeleton   start with the bones drawn
  --mcp        serve the library to an agent over MCP (HTTP, port 7406;
               --mcp-port <port> for another), the token in mcp-token
               under ~/.local/share/gameengine
  --headless   serve the library over MCP on stdio, no window

An agent's manual is ModelViewerGuide.md, beside the executable.

Drop model files on the window or use Open... to open more; the Browse tab
shows the models under a folder; the Recent button reopens what you had.
Preferences (the gear) say where screenshots go and how they are named,
the view's defaults, the thumbnail size and the cache. The viewer keeps its
cache, thumbnails, settings and library under
~/.local/share/gameengine/model-viewer.

Requires GPU drivers with Vulkan support and the Vulkan loader installed
(e.g. on Ubuntu:  sudo apt install libvulkan1 mesa-vulkan-drivers).

Everything else ships in this folder and is found relative to the
executable: the folder can be moved anywhere.
EOF

# 7. Package.
TARBALL="${DIST}.tar.gz"
SYMBOLS_TARBALL="${SYMBOLS}.tar.gz"
log "Packaging $TARBALL and $SYMBOLS_TARBALL"
tar -czf "$TARBALL" -C "$(dirname "$DIST")" "$(basename "$DIST")"
tar -czf "$SYMBOLS_TARBALL" -C "$(dirname "$SYMBOLS")" "$(basename "$SYMBOLS")"

log "done"
echo "model viewer dist folder: $DIST"
echo "model viewer archive:     $TARBALL"
echo "model viewer symbols:     $SYMBOLS_TARBALL (keep it with the build; not for download)"
echo "smoke test on THIS machine:  ( cd '$DIST' && ./Tools.ModelViewer --screenshot shot.png --screenshot-exit )"
