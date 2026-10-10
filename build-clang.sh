#!/usr/bin/env bash
# Configure + build a Clang preset on Linux (default `clang`).
#
#   ./build-clang.sh                                      configure + build everything (Debug)
#   ./build-clang.sh --target UI                          build just that target
#   ./build-clang.sh -- -k 0                              keep going after failures (-- goes to ninja)
#   ./build-clang.sh --preset clang-reldbg                a different Clang preset
#   ./build-clang.sh --preset clang-reldbg --target Tools.Editor
#
# --preset must come FIRST: clang, clang-shared, clang-reldbg, clang-release, clang-shipping.
#
# WHY THIS WRAPPER EXISTS: the preset asks for plain "clang++", but a distro's default clang is
# routinely too old for this tree (README: Clang 17+; Ubuntu 24.04 still ships 18, whose C++23
# modules support is weaker and which rejects flags we pass under -Werror). So pick the NEWEST
# versioned clang++ installed, verify it clears the floor, and pin C/CXX/ASM explicitly - the
# same override CI applies by hand. ASM is driven by the C++ driver; the vendored C sources
# (SDL3 and friends) need the matching C driver.
#
# Set BUILD_CLANG_CXX to a specific binary to override the search, e.g.
#   BUILD_CLANG_CXX=clang++-21 ./build-clang.sh
set -euo pipefail

REPO="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CLANG_FLOOR=17

# Linux only, and enforced rather than assumed. Under a Windows bash (git-bash/MSYS) every path
# here comes out POSIX-shaped (/c/Program Files/...) while CMake's cache holds C:/Program
# Files/...\ - so the "cached compiler differs" guard below can never match, and the script
# silently wipes a perfectly good build tree's cache on every run. Use build-clang.cmd there.
case "$(uname -s 2>/dev/null)" in
    Linux) ;;
    MINGW*|MSYS*|CYGWIN*)
        echo "[build-clang] This is the Linux wrapper. On Windows use: build-clang.cmd" >&2
        exit 1
        ;;
    *)
        echo "[build-clang] Unsupported platform '$(uname -s 2>/dev/null)' - this wrapper targets Linux." >&2
        exit 1
        ;;
esac

# --- Arguments ---------------------------------------------------------------------------
PRESET="clang"
if [ "${1-}" = "--preset" ]; then
    if [ -z "${2-}" ]; then
        echo "[build-clang] --preset needs a preset name, e.g. --preset clang-reldbg." >&2
        exit 1
    fi
    PRESET="$2"
    shift 2
fi

case "$PRESET" in
    clang*) ;;
    *)
        echo "[build-clang] '$PRESET' is not a Clang preset. Use clang, clang-shared," >&2
        echo "[build-clang] clang-reldbg, clang-release or clang-shipping - or call cmake directly." >&2
        exit 1
        ;;
esac

# --- Pick the compiler -------------------------------------------------------------------
# Walk versioned names newest-first, then fall back to an unsuffixed clang++ that clears the
# floor. -dumpversion is the version of the binary itself, so this never trusts the name.
major_of() { "$1" -dumpversion 2>/dev/null | cut -d. -f1; }

CXX_BIN=""
if [ -n "${BUILD_CLANG_CXX-}" ]; then
    if ! command -v "$BUILD_CLANG_CXX" >/dev/null 2>&1; then
        echo "[build-clang] BUILD_CLANG_CXX='$BUILD_CLANG_CXX' is not executable / not on PATH." >&2
        exit 1
    fi
    CXX_BIN="$BUILD_CLANG_CXX"
else
    for v in $(seq 30 -1 "$CLANG_FLOOR"); do
        if command -v "clang++-$v" >/dev/null 2>&1; then CXX_BIN="clang++-$v"; break; fi
    done
    if [ -z "$CXX_BIN" ] && command -v clang++ >/dev/null 2>&1; then
        m="$(major_of clang++)"
        if [ -n "$m" ] && [ "$m" -ge "$CLANG_FLOOR" ] 2>/dev/null; then CXX_BIN="clang++"; fi
    fi
fi

if [ -z "$CXX_BIN" ]; then
    echo "[build-clang] No clang++ >= $CLANG_FLOOR found. Install one (apt.llvm.org), or set" >&2
    echo "[build-clang]   BUILD_CLANG_CXX=<clang++ binary>" >&2
    exit 1
fi

# Verify whatever we landed on actually clears the floor, including an explicit override.
MAJOR="$(major_of "$CXX_BIN" || true)"
if [ -z "$MAJOR" ] || ! [ "$MAJOR" -ge "$CLANG_FLOOR" ] 2>/dev/null; then
    echo "[build-clang] '$CXX_BIN' reports version '${MAJOR:-unknown}'; this tree needs Clang >= $CLANG_FLOOR." >&2
    exit 1
fi

# The matching C driver: clang++-21 -> clang-21, clang++ -> clang.
C_BIN="clang${CXX_BIN#clang++}"
if ! command -v "$C_BIN" >/dev/null 2>&1; then
    echo "[build-clang] Found '$CXX_BIN' but no matching '$C_BIN' (needed for vendored C sources)." >&2
    exit 1
fi

CXX_PATH="$(command -v "$CXX_BIN")"
echo "[build-clang] Preset   : $PRESET"
echo "[build-clang] Compiler : $CXX_PATH (Clang $MAJOR)"

# --- Configure ---------------------------------------------------------------------------
# Every preset in CMakePresets.json uses binaryDir build/<preset name>, so the build tree is
# derivable from the name - keep that true if a preset is ever added with a different dir.
# A cached compiler that differs from the selected one must not keep being used, so wipe the
# cache when they disagree (the compiler is pinned in the cache on first configure).
CACHE="$REPO/build/$PRESET/CMakeCache.txt"
if [ -f "$CACHE" ]; then
    if ! grep -q "^CMAKE_CXX_COMPILER:[^=]*=$CXX_PATH\$" "$CACHE"; then
        echo "[build-clang] Cached compiler differs from the selected one - reconfiguring."
        rm -f "$CACHE"
    fi
fi

echo "[build-clang] Configuring..."
cmake --preset "$PRESET" -S "$REPO" \
    -DCMAKE_C_COMPILER="$C_BIN" \
    -DCMAKE_CXX_COMPILER="$CXX_PATH" \
    -DCMAKE_ASM_COMPILER="$CXX_PATH"

echo "[build-clang] Building..."
cmake --build --preset "$PRESET" "$@"

# Read the output dir back out of the cache rather than mapping preset -> path here: the config
# and the suffix are both cache variables, so this cannot drift from CMakePresets.json.
CFG="$(sed -n 's/^CMAKE_BUILD_TYPE:[^=]*=//p' "$CACHE" | head -1)"
SFX="$(sed -n 's/^BUILDSYSTEM_OUTPUT_SUFFIX:[^=]*=//p' "$CACHE" | head -1)"
if [ -n "$CFG" ]; then
    echo "[build-clang] OK - binaries in Bin/$CFG/Linux64-Clang$SFX"
else
    echo "[build-clang] OK - build tree: build/$PRESET"
fi
