#!/usr/bin/env bash
# Configure + build a GCC preset on Linux (default `gcc`).
#
#   ./build-gcc.sh                                      configure + build everything (Debug)
#   ./build-gcc.sh --target UI                          build just that target
#   ./build-gcc.sh -- -k 0                              keep going after failures (-- goes to ninja)
#   ./build-gcc.sh --preset gcc-release                 a different GCC preset
#   ./build-gcc.sh --preset gcc-release --target Tools.Editor
#
# --preset must come FIRST: gcc, gcc-release, gcc-shipping. (There is no gcc-shared or
# gcc-reldbg preset today - Clang is the daily driver and owns those lanes.)
#
# WHY THIS WRAPPER EXISTS: the preset asks for plain "g++", but this tree needs GCC 15+
# (README) for its C++23 modules support, and distros still default to older. So pick the
# NEWEST versioned g++ installed, verify it clears the floor, and pin C/CXX explicitly - the
# same override CI applies by hand. ASM is deliberately left alone: CMake drives it from the C
# compiler for GNU, which is what the gcc CI lane does.
#
# Set BUILD_GCC_CXX to a specific binary to override the search, e.g.
#   BUILD_GCC_CXX=g++-15 ./build-gcc.sh
set -euo pipefail

REPO="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
GCC_FLOOR=15

# Linux only, and enforced rather than assumed. Under a Windows bash (git-bash/MSYS) every path
# here comes out POSIX-shaped (/c/Program Files/...) while CMake's cache holds C:/Program
# Files/... - so the "cached compiler differs" guard below can never match, and the script
# silently wipes a perfectly good build tree's cache on every run. GCC has no Windows lane here.
case "$(uname -s 2>/dev/null)" in
    Linux) ;;
    MINGW*|MSYS*|CYGWIN*)
        echo "[build-gcc] This is the Linux wrapper; there is no GCC lane on Windows." >&2
        echo "[build-gcc] Use build-msvc.cmd or build-clang.cmd." >&2
        exit 1
        ;;
    *)
        echo "[build-gcc] Unsupported platform '$(uname -s 2>/dev/null)' - this wrapper targets Linux." >&2
        exit 1
        ;;
esac

# --- Arguments ---------------------------------------------------------------------------
PRESET="gcc"
if [ "${1-}" = "--preset" ]; then
    if [ -z "${2-}" ]; then
        echo "[build-gcc] --preset needs a preset name, e.g. --preset gcc-release." >&2
        exit 1
    fi
    PRESET="$2"
    shift 2
fi

case "$PRESET" in
    gcc*) ;;
    *)
        echo "[build-gcc] '$PRESET' is not a GCC preset. Use gcc, gcc-release or gcc-shipping" >&2
        echo "[build-gcc] - or call cmake --preset directly." >&2
        exit 1
        ;;
esac

# --- Pick the compiler -------------------------------------------------------------------
# Walk versioned names newest-first, then fall back to an unsuffixed g++ that clears the floor.
# -dumpversion is the version of the binary itself, so this never trusts the name.
major_of() { "$1" -dumpversion 2>/dev/null | cut -d. -f1; }

CXX_BIN=""
if [ -n "${BUILD_GCC_CXX-}" ]; then
    if ! command -v "$BUILD_GCC_CXX" >/dev/null 2>&1; then
        echo "[build-gcc] BUILD_GCC_CXX='$BUILD_GCC_CXX' is not executable / not on PATH." >&2
        exit 1
    fi
    CXX_BIN="$BUILD_GCC_CXX"
else
    for v in $(seq 25 -1 "$GCC_FLOOR"); do
        if command -v "g++-$v" >/dev/null 2>&1; then CXX_BIN="g++-$v"; break; fi
    done
    if [ -z "$CXX_BIN" ] && command -v g++ >/dev/null 2>&1; then
        m="$(major_of g++)"
        if [ -n "$m" ] && [ "$m" -ge "$GCC_FLOOR" ] 2>/dev/null; then CXX_BIN="g++"; fi
    fi
fi

if [ -z "$CXX_BIN" ]; then
    echo "[build-gcc] No g++ >= $GCC_FLOOR found. Install one, or set" >&2
    echo "[build-gcc]   BUILD_GCC_CXX=<g++ binary>" >&2
    exit 1
fi

# Verify whatever we landed on actually clears the floor, including an explicit override.
MAJOR="$(major_of "$CXX_BIN" || true)"
if [ -z "$MAJOR" ] || ! [ "$MAJOR" -ge "$GCC_FLOOR" ] 2>/dev/null; then
    echo "[build-gcc] '$CXX_BIN' reports version '${MAJOR:-unknown}'; this tree needs GCC >= $GCC_FLOOR." >&2
    exit 1
fi

# The matching C driver: g++-15 -> gcc-15, g++ -> gcc.
C_BIN="gcc${CXX_BIN#g++}"
if ! command -v "$C_BIN" >/dev/null 2>&1; then
    echo "[build-gcc] Found '$CXX_BIN' but no matching '$C_BIN' (needed for vendored C sources)." >&2
    exit 1
fi

CXX_PATH="$(command -v "$CXX_BIN")"
echo "[build-gcc] Preset   : $PRESET"
echo "[build-gcc] Compiler : $CXX_PATH (GCC $MAJOR)"

# --- Configure ---------------------------------------------------------------------------
# Every preset in CMakePresets.json uses binaryDir build/<preset name>, so the build tree is
# derivable from the name - keep that true if a preset is ever added with a different dir.
# A cached compiler that differs from the selected one must not keep being used, so wipe the
# cache when they disagree (the compiler is pinned in the cache on first configure).
CACHE="$REPO/build/$PRESET/CMakeCache.txt"
if [ -f "$CACHE" ]; then
    if ! grep -q "^CMAKE_CXX_COMPILER:[^=]*=$CXX_PATH\$" "$CACHE"; then
        echo "[build-gcc] Cached compiler differs from the selected one - reconfiguring."
        rm -f "$CACHE"
    fi
fi

echo "[build-gcc] Configuring..."
cmake --preset "$PRESET" -S "$REPO" \
    -DCMAKE_C_COMPILER="$C_BIN" \
    -DCMAKE_CXX_COMPILER="$CXX_PATH"

echo "[build-gcc] Building..."
cmake --build --preset "$PRESET" "$@"

# Read the output dir back out of the cache rather than mapping preset -> path here: the config
# and the suffix are both cache variables, so this cannot drift from CMakePresets.json.
CFG="$(sed -n 's/^CMAKE_BUILD_TYPE:[^=]*=//p' "$CACHE" | head -1)"
SFX="$(sed -n 's/^BUILDSYSTEM_OUTPUT_SUFFIX:[^=]*=//p' "$CACHE" | head -1)"
if [ -n "$CFG" ]; then
    echo "[build-gcc] OK - binaries in Bin/$CFG/Linux64-GCC$SFX"
else
    echo "[build-gcc] OK - build tree: build/$PRESET"
fi
