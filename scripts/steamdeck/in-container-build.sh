#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026-Present Robert Campbell

#
# Runs inside the scripts/steamdeck/Dockerfile container (repo bind-mounted at /work). Configures
# a Release build with Clang 21 + GCC 14's libstdc++, links libstdc++/libgcc statically, builds
# everything, then stages runnable Steam Deck bundles and verifies the glibc floor of every shipped
# file. Do not run this on the host; run scripts/build-steamdeck.sh, which builds the image and
# invokes this inside it.
set -euo pipefail

JOBS="${JOBS:-4}"
BUILD="build/steamdeck"                 # separate binary dir so it never mixes with a host build
# The Bin/ deliverables get their own suffix too: without it the container build clobbers (and
# is clobbered by) the host clang-release binaries in Bin/Release/Linux64-Clang.
BIN="Bin/Release/Linux64-Clang-SteamDeck"
GCC14="/usr/lib/gcc/x86_64-linux-gnu/14" # the libstdc++ Clang should use (std::print)
DIST="dist/SteamDeck"
FORMATS="${FORMATS:-spirv}"             # the Deck renders through wgpu-native on Vulkan
MAX_GLIBC="2.35"                        # Ubuntu 22.04 floor; <= Steam Deck host (~2.37)

log() { printf '\n== %s ==\n' "$*"; }

# --- version comparison helpers -------------------------------------------------------------
glibc_floor() { objdump -T "$1" 2>/dev/null | grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sed 's/GLIBC_//' | sort -V | tail -1; }
# ver_le A B  -> true if A <= B
ver_le() { [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | head -1)" = "$1" ]; }

# --- 1. configure: Clang 21 + GCC 14 libstdc++, static C++ runtime ---------------------------
log "Configure ($BUILD): Clang 21 + GCC 14 libstdc++, static libstdc++/libgcc"
cmake -S . -B "$BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang-21 \
    -DCMAKE_CXX_COMPILER=clang++-21 \
    -DCMAKE_ASM_COMPILER=clang++-21 \
    -DCMAKE_CXX_FLAGS="--gcc-install-dir=$GCC14" \
    -DCMAKE_EXE_LINKER_FLAGS="--gcc-install-dir=$GCC14 -static-libstdc++ -static-libgcc" \
    -DCMAKE_SHARED_LINKER_FLAGS="--gcc-install-dir=$GCC14 -static-libstdc++ -static-libgcc" \
    -DBUILDSYSTEM_OUTPUT_SUFFIX=-SteamDeck

# --- 2. build everything ---------------------------------------------------------------------
log "Build all targets (-j$JOBS)"
cmake --build "$BUILD" -j"$JOBS"

# --- 3. cook the engine shader pack ----------------------------------------------------------
# The pack is platform-independent SPIR-V, but DXC (the compiler it cooks with) needs glibc 2.38,
# which this 22.04 container does not have, so build-steamdeck.sh cooks it on the host into
# build/steamdeck-shaders.dpak, and this stages that. Cooked here only when that is absent.
if [ -f "build/steamdeck-shaders.dpak" ]; then
    log "Shaders: the host-cooked build/steamdeck-shaders.dpak"
    cp "build/steamdeck-shaders.dpak" "/tmp/shaders.dpak"
else
    log "Cook shaders.dpak ($FORMATS)"
    "$BIN/Tools.ShaderPack" "Data/Engine/Shaders" "/tmp/shaders.dpak" $FORMATS
fi

# --- 4. stage runnable bundles ---------------------------------------------------------------
# SDL3 is linked statically (vendored source), so there is no libSDL3.so to bundle: it lives inside
# the executables. wgpu is a runtime .so both need. DXC is for the editor only (shader authoring)
# and its glibc floor (2.38) exceeds the Deck host's, so it is bundled with the editor but flagged
# in the glibc check below; the editor still launches in pack mode without it.
WGPU="ThirdParty/WgpuNative/lib/linux-x86_64/libwgpu_native.so"
DXC1="ThirdParty/DXC/lib/linux-x86_64/libdxcompiler.so"
DXC2="ThirdParty/DXC/lib/linux-x86_64/libdxil.so"

rm -rf "$DIST"
mkdir -p "$DIST/Editor/Data" "$DIST/Player"

log "Stage $DIST/Editor"
cp "$BIN/Tools.Editor" "$DIST/Editor/"
cp "$WGPU" "$DIST/Editor/"
cp "$DXC1" "$DXC2" "$DIST/Editor/"           # editor cooks shaders (see glibc note below)
# The data the editor reads, as the desktop distribution stages it: Engine, Editor (less the
# sandbox's mock), Branding and Templates; the pack into Data/Engine/Shaders, the one
# path the runtime opens, with the marker that makes the editor start from it.
for folder in Engine Editor Branding Templates; do
    cp -r "Data/$folder" "$DIST/Editor/Data/$folder"
done
rm -rf "$DIST/Editor/Data/Editor/Mock"
cp "/tmp/shaders.dpak" "$DIST/Editor/Data/Engine/Shaders/shaders.dpak"
echo "An editor distribution: start from shaders.dpak; the sources are for cooking." \
    > "$DIST/Editor/Data/Engine/Shaders/.pack-first"
cp "Data/.dataroot" "$DIST/Editor/Data/.dataroot"
strip "$DIST/Editor/Tools.Editor" 2>/dev/null || true

log "Stage $DIST/Player"
cp "$BIN/Engine.Player" "$DIST/Player/"
cp "$WGPU" "$DIST/Player/"
cp "/tmp/shaders.dpak" "$DIST/Player/shaders.dpak"
strip "$DIST/Player/Engine.Player" 2>/dev/null || true

cat > "$DIST/README.txt" <<EOF
Steam Deck bundles (built for glibc <= $MAX_GLIBC, libstdc++ linked statically).

Editor/   ./Tools.Editor    - the authoring tool (runs in shader-pack mode).
Player/   ./Engine.Player   - the game runtime; point it at a project or a cooked dist.

SDL3 is linked statically (inside the binary). libwgpu_native.so ships beside each binary and is
found via the executable's \$ORIGIN rpath. Requires GPU drivers with Vulkan (the Deck has them).

NOTE: libdxcompiler.so (DXC, the editor's shader COMPILER) needs glibc 2.38, which exceeds the
Deck host - so on-device shader authoring/recompiling will not work. The editor still launches
and renders from the bundled shaders.dpak. Cook shaders on a desktop instead.
EOF

# --- 5. verify glibc floors ------------------------------------------------------------------
# Every shipped ELF except DXC must be <= MAX_GLIBC. DXC is allowed to exceed (editor-only, warned).
log "Verify glibc floors (must be <= $MAX_GLIBC; DXC exempt)"
fail=0
while IFS= read -r f; do
    head -c4 "$f" | grep -q $'\x7fELF' || continue
    base="$(basename "$f")"
    fl="$(glibc_floor "$f")"; [ -n "$fl" ] || fl="none(static)"
    if [ "$base" = "libdxcompiler.so" ] || [ "$base" = "libdxil.so" ]; then
        printf '  %-24s %-14s (editor-only, exempt)\n' "$base" "$fl"
        continue
    fi
    if [ "$fl" = "none(static)" ] || ver_le "$fl" "$MAX_GLIBC"; then
        printf '  %-24s %-14s OK\n' "$base" "$fl"
    else
        printf '  %-24s %-14s TOO NEW (> %s)\n' "$base" "$fl" "$MAX_GLIBC"; fail=1
    fi
done < <(find "$DIST" -type f)

if [ "$fail" -ne 0 ]; then
    echo "!! one or more shipped binaries exceed the glibc floor - NOT Deck-safe" >&2
    exit 1
fi

log "done -> $DIST (Editor + Player), all within glibc $MAX_GLIBC"
