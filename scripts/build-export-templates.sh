#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026-Present Robert Campbell

#
# build-export-templates.sh - build export templates (prebuilt runtime bundles) for the
# supported platforms, so `Tools.Export` and the editor can stamp shippable dists against them.
#
# A template is the Engine.Player runtime for one (platform, config), its runtime sidecars and a
# template.xml manifest. It is neither game content nor a toolchain: it is the prebuilt runtime
# you ship against.
#
# Per platform this builds Engine.Player (and the native Tools.Export once), then runs
#   Tools.Export --template create <Bin/<Config>/<Platform>-<Compiler>>
# which reads the build's runtime-libs manifest, copies the player and sidecars, and writes
# template.xml.
#
# The Linux template is built RelWithDebInfo (the clang-reldbg preset, the tree the editor
# distribution builds too, so a release builds the engine once): the player is optimised, and its
# debug info is split off before packaging: the template's player is stripped (an export copies it
# as it is, so every game would carry it otherwise) and Engine.Player.debug goes to
# <OUT>/linux64-release-symbols, to keep with the build. The template's config, and so its id, is
# RelWithDebInfo (gameengine-linux64-relwithdebinfo-<version>).
#
# Windows is not built here (no toolchain on Linux); use scripts/build-export-templates.ps1 on a
# Windows agent for the Win64 template.
#
# The web templates are two: web-release (the wasm-shipping preset: smallest, no profiler), which
# the sample games' Web presets and the published demos use, and web-profile (the wasm-release
# preset: Release with the profiler, its capture and its overlay compiled in, the overlay off by
# default), to profile a game in a browser. Both land in Bin/Release/..., so the profiling one
# is given its own id.
#
#   Usage:  scripts/build-export-templates.sh [linux|web|all]      (default: all)
#   Env:    JOBS=N     build parallelism (default 4; higher OOMs the engine build)
#           OUT=<dir>  write self-contained bundles under <dir> (for zip/distribution);
#                      unset = --install into the local templates root (usable immediately)
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

JOBS="${JOBS:-4}"
OUT="${OUT:-}"
WHICH="${1:-all}"

LINUX_PRESET="clang-reldbg"            # RelWithDebInfo, shared with build-editor-dist.sh
LINUX_BUILD="build/$LINUX_PRESET"
LINUX_BIN="Bin/RelWithDebInfo/Linux64-Clang"
WEB_BUILD="build/wasm-shipping"        # the wasm-shipping preset: emscripten, Release, shipping
WEB_BIN="Bin/Release/Emscripten-Clang-Shipping" # the preset's output suffix (-Shipping)
WEB_PROFILE_BUILD="build/wasm-release"  # the wasm-release preset: emscripten, Release, profiler in
WEB_PROFILE_BIN="Bin/Release/Emscripten-Clang"
VERSION="$(sed -n 's/^[[:space:]]*VERSION[[:space:]]\+\([0-9][0-9.]*\).*/\1/p' CMakeLists.txt | head -1)"
PREFIX="$(sed -n 's/^set(TEMPLATE_ID_PREFIX_VALUE[[:space:]]*"\([^"]*\)".*/\1/p' CMakeLists.txt | head -1)"

# The native exporter packages every platform's bundle (it runs on the host, pointing at the
# target's Bin dir, including the wasm one, which it synthesizes into a "Web" template).
EXPORTER="$LINUX_BIN/Tools.Export"

log() { printf '\n== %s ==\n' "$*"; }

create_template() { # <config-dir> <out-subdir-tag> [Tools.Export --id/--name/... flags]
    local cfg="$1"
    local tag="$2"
    shift 2
    if [[ ! -x "$EXPORTER" ]]; then
        echo "!! $EXPORTER not built yet (the Linux step builds it)" >&2
        return 1
    fi
    if [[ -n "$OUT" ]]; then
        # --out writes the bundle flat into the given folder, so each platform needs its own
        # subfolder or a multi-platform run would clobber one template.xml with the next.
        local dest="$OUT/$tag"
        mkdir -p "$dest"
        "$EXPORTER" --template create "$cfg" --out "$dest" "$@"
    else
        "$EXPORTER" --template create "$cfg" --install "$@"   # --install lands each under its <id>
    fi
}

build_linux() {
    log "Linux (Release) template"
    if [[ ! -f "$LINUX_BUILD/CMakeCache.txt" ]]; then
        echo ">> configuring $LINUX_BUILD (the $LINUX_PRESET preset)"
        # The preset pins clang (the Bin/ dir is compiler-derived); CXX_COMPILER overrides it
        # for a versioned clang (see build-editor-dist.sh).
        CXX_BIN="${CXX_COMPILER:-clang++}"
        C_BIN="${C_COMPILER:-clang}"
        cmake --preset "$LINUX_PRESET" -DCMAKE_C_COMPILER="$C_BIN" -DCMAKE_CXX_COMPILER="$CXX_BIN" \
            -DCMAKE_ASM_COMPILER="$CXX_BIN"
    fi
    cmake --build "$LINUX_BUILD" --target Engine.Player Tools.Export -j"$JOBS"

    # Package a stripped copy: the player and its sidecars in a staging folder shaped like the Bin
    # one (the template reads its config and compiler from the path), the debug info split off.
    local stage="build/tmp/template-stage/$LINUX_BIN"
    local symbols="${OUT:-build/tmp/template-stage}/linux64-release-symbols"
    rm -rf "build/tmp/template-stage" "$symbols"
    mkdir -p "$stage" "$symbols"
    cp "$LINUX_BIN/Engine.Player" "$stage/"
    if [[ -f "$LINUX_BIN/Engine.Player.runtime-libs" ]]; then
        cp "$LINUX_BIN/Engine.Player.runtime-libs" "$stage/"
        while IFS= read -r lib; do
            if [[ -n "$lib" && -f "$LINUX_BIN/$lib" ]]; then
                cp "$LINUX_BIN/$lib" "$stage/"
            fi
        done < "$LINUX_BIN/Engine.Player.runtime-libs"
    fi
    objcopy --only-keep-debug "$stage/Engine.Player" "$symbols/Engine.Player.debug"
    strip --strip-debug "$stage/Engine.Player"
    objcopy --add-gnu-debuglink="$symbols/Engine.Player.debug" "$stage/Engine.Player"
    create_template "$stage" "linux64-release"
    echo "player symbols: $symbols/Engine.Player.debug (keep it with the build)"
}

build_web() {
    log "Web (Release) template"
    if ! command -v emcc >/dev/null 2>&1; then
        if [[ -f "$HOME/emsdk/emsdk_env.sh" ]]; then
            # shellcheck disable=SC1091
            source "$HOME/emsdk/emsdk_env.sh" >/dev/null
        fi
    fi
    if ! command -v emcc >/dev/null 2>&1; then
        echo "!! emcc not on PATH (and ~/emsdk/emsdk_env.sh not found) - skipping web template" >&2
        return 0
    fi
    if [[ ! -f "$WEB_BUILD/CMakeCache.txt" ]]; then
        echo ">> configuring $WEB_BUILD (the wasm-shipping preset)"
        cmake --preset wasm-shipping
    fi
    cmake --build "$WEB_BUILD" --target Engine.Player -j"$JOBS"
    # Packaged into a "Web" template by the native exporter (built in the Linux step).
    create_template "$WEB_BIN" "web-release"

    log "Web (profiling) template"
    if [[ ! -f "$WEB_PROFILE_BUILD/CMakeCache.txt" ]]; then
        echo ">> configuring $WEB_PROFILE_BUILD (the wasm-release preset)"
        cmake --preset wasm-release
    fi
    cmake --build "$WEB_PROFILE_BUILD" --target Engine.Player -j"$JOBS"
    [[ -n "$VERSION" && -n "$PREFIX" ]] || { echo "!! no project VERSION or template id prefix in CMakeLists.txt" >&2; return 1; }
    create_template "$WEB_PROFILE_BIN" "web-profile" \
        --id "$PREFIX-web-profile-$VERSION" \
        --name "Web Profile $VERSION" \
        --notes "Web Release with the profiler compiled in: its capture and its overlay (off by default)."
}

case "$WHICH" in
    linux) build_linux ;;
    web)   build_linux; build_web ;;   # web needs the native exporter, so build Linux first
    all)   build_linux; build_web ;;
    *) echo "usage: $0 [linux|web|all]   (env: JOBS=N OUT=<dir>)"; exit 2 ;;
esac

log "done"
if [[ -n "$OUT" ]]; then
    echo "templates written under: $OUT/<platform>/   (zip each platform folder to distribute)"
else
    echo "templates installed into the local templates root (Tools.Export --template list to verify)"
fi
