# Third-party dependencies.

# SDL3 - vendored SOURCE, built as a STATIC library. Desktop binaries carry SDL inside
# the executable: no libSDL3.so / SDL3.dll runtime dependency on Linux or Windows. SDL still dlopens
# the platform windowing/audio backends (X11/Wayland/PipeWire/ALSA) at RUNTIME by design, so those
# stay enabled (a static SDL is still portable). Building from source means SDL is compiled by this
# build's toolchain, so its glibc floor matches the rest of the engine rather than a prebuilt's.
# Set ENGINE_SYSTEM_SDL to use a system find_package(SDL3) instead (distro packaging). Not built for
# Emscripten - the browser shell replaces SDL there.
#
# ENGINE_SDL_TARGET (the link target for the desktop shell + its consumers) is exported to the root
# scope here; empty means no SDL, so the root skips Foundation::Shell.Desktop.
option(ENGINE_SYSTEM_SDL "Use a system SDL3 (find_package) instead of the vendored static build" OFF)
# Clear the cached choice first: a re-configure that flips ENGINE_SYSTEM_SDL (or loses the
# vendored tree) must not leave a stale SDL3::SDL3-static in the cache - the root would then
# add Shell.Desktop against a target that no longer exists (a confusing generate-time error
# instead of the intended clean skip).
unset(ENGINE_SDL_TARGET CACHE)
if(NOT EMSCRIPTEN)
    if(NOT ENGINE_SYSTEM_SDL AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/SDL3/CMakeLists.txt)
        # SDL follows the engine's library type, for the same reason AngelScript does
        # (below): MORE THAN ONE target links it - Foundation::Shell.Desktop and, so it can
        # push synthetic events, Shell.Desktop.Tests. A static archive embedded in both a DLL
        # and the exe is TWO SDL runtimes with two event queues, so events pushed by one are
        # invisible to the other. A static engine link dedups and never sees this; ELF
        # interposition unifies the duplicate symbols and hides it on Linux; PE has neither,
        # so the shell tests fail there first. shared-libraries.md P5/W2.
        if(ENGINE_SHARED_LIBS)
            set(SDL_SHARED ON CACHE BOOL "" FORCE)
            set(SDL_STATIC OFF CACHE BOOL "" FORCE)
        else()
            set(SDL_SHARED OFF CACHE BOOL "" FORCE)   # static only - no runtime .so/.dll
            set(SDL_STATIC ON CACHE BOOL "" FORCE)
        endif()
        set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE) # skip SDL_test, tests, examples, install
        set(SDL_TESTS OFF CACHE BOOL "" FORCE)
        set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
        add_subdirectory(SDL3 EXCLUDE_FROM_ALL)       # compiled only when the shell links it
        if(ENGINE_SHARED_LIBS)
            set(ENGINE_SDL_TARGET SDL3::SDL3 CACHE INTERNAL "SDL link target for the desktop shell")
        else()
            set(ENGINE_SDL_TARGET SDL3::SDL3-static CACHE INTERNAL "SDL link target for the desktop shell")
        endif()
    else()
        find_package(SDL3 QUIET GLOBAL)               # system SDL3 (escape hatch / distro packaging)
        if(SDL3_FOUND)
            set(ENGINE_SDL_TARGET SDL3::SDL3 CACHE INTERNAL "SDL link target for the desktop shell")
        endif()
    endif()
endif()

# DXC - vendored headers + pre-built runtime binaries (HLSL -> SPIR-V/DXIL).
# Headers are an INTERFACE target; the runtime library is loaded at run time
# (dlopen / LoadLibrary), so we only publish its path + an rpath for dev builds.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/DXC/include/dxc/dxcapi.h)
    add_library(dxc_headers INTERFACE)
    add_library(ThirdParty::DXCHeaders ALIAS dxc_headers)
    # SYSTEM so the vendored headers (DXC's WinAdapter shim has unused-parameter
    # paths under NDEBUG) are exempt from our -Wall -Wextra -Werror.
    target_include_directories(dxc_headers
        SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/DXC/include)

    # BUILDSYSTEM_DXC_LIB (cache): the absolute path to the DXC runtime library, so apps that ship
    # (Engine.Player) can STAGE it beside the executable as a sidecar - DXC is loaded at runtime
    # (renderer compiles HLSL on demand), so a dist that lacks it renders nothing on any machine
    # but this one (where the compiled-in BUILDSYSTEM_DXC_PATH rpath still resolves into the tree).
    if(WIN32)
        set(_dxc_lib_dir "${CMAKE_CURRENT_SOURCE_DIR}/DXC/lib/win-x64")
        if(EXISTS "${_dxc_lib_dir}/dxcompiler.dll")
            target_compile_definitions(dxc_headers
                INTERFACE BUILDSYSTEM_DXC_PATH="${_dxc_lib_dir}/dxcompiler.dll")
            set(BUILDSYSTEM_DXC_LIB "${_dxc_lib_dir}/dxcompiler.dll" CACHE INTERNAL
                "DXC runtime library to stage beside shipping apps")
            message(STATUS "DXC runtime: ${_dxc_lib_dir}")
        endif()
    elseif(UNIX AND NOT APPLE)
        set(_dxc_lib_dir "${CMAKE_CURRENT_SOURCE_DIR}/DXC/lib/linux-x86_64")
        if(EXISTS "${_dxc_lib_dir}/libdxcompiler.so")
            target_compile_definitions(dxc_headers
                INTERFACE BUILDSYSTEM_DXC_PATH="${_dxc_lib_dir}/libdxcompiler.so")
            set(BUILDSYSTEM_DXC_LIB "${_dxc_lib_dir}/libdxcompiler.so" CACHE INTERNAL
                "DXC runtime library to stage beside shipping apps")
            list(APPEND CMAKE_BUILD_RPATH "${_dxc_lib_dir}")
            set(CMAKE_BUILD_RPATH "${CMAKE_BUILD_RPATH}" PARENT_SCOPE)
            message(STATUS "DXC runtime: ${_dxc_lib_dir}")
        endif()
    endif()
else()
    message(STATUS "ThirdParty/DXC not present - foundation.shaders will be skipped.")
endif()

# wgpu-native - vendored standard webgpu.h header + pre-built runtime binaries
# (gfx-rs/wgpu-native, see WgpuNative/VERSION). The desktop WebGPU RHI backend is
# written against webgpu.h and loads wgpu_native at RUN TIME (dlopen / LoadLibrary),
# the same runtime-sidecar pattern as DXC: headers are an INTERFACE target; only the
# library's path (+ a dev-build rpath) is published. Browser builds link the
# emscripten-provided implementation instead - no sidecar there.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/WgpuNative/include/webgpu/webgpu.h)
    add_library(wgpu_headers INTERFACE)
    add_library(ThirdParty::WgpuHeaders ALIAS wgpu_headers)
    target_include_directories(wgpu_headers
        SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/WgpuNative/include)

    if(WIN32)
        set(_wgpu_lib_dir "${CMAKE_CURRENT_SOURCE_DIR}/WgpuNative/lib/win-x64")
        if(EXISTS "${_wgpu_lib_dir}/wgpu_native.dll")
            target_compile_definitions(wgpu_headers
                INTERFACE BUILDSYSTEM_WGPU_PATH="${_wgpu_lib_dir}/wgpu_native.dll")
            set(BUILDSYSTEM_WGPU_LIB "${_wgpu_lib_dir}/wgpu_native.dll" CACHE INTERNAL
                "wgpu-native runtime library to stage beside shipping apps")
            message(STATUS "wgpu-native runtime: ${_wgpu_lib_dir}")
        endif()
    elseif(UNIX AND NOT APPLE AND NOT EMSCRIPTEN)
        set(_wgpu_lib_dir "${CMAKE_CURRENT_SOURCE_DIR}/WgpuNative/lib/linux-x86_64")
        if(EXISTS "${_wgpu_lib_dir}/libwgpu_native.so")
            target_compile_definitions(wgpu_headers
                INTERFACE BUILDSYSTEM_WGPU_PATH="${_wgpu_lib_dir}/libwgpu_native.so")
            set(BUILDSYSTEM_WGPU_LIB "${_wgpu_lib_dir}/libwgpu_native.so" CACHE INTERNAL
                "wgpu-native runtime library to stage beside shipping apps")
            list(APPEND CMAKE_BUILD_RPATH "${_wgpu_lib_dir}")
            set(CMAKE_BUILD_RPATH "${CMAKE_BUILD_RPATH}" PARENT_SCOPE)
            message(STATUS "wgpu-native runtime: ${_wgpu_lib_dir}")
        endif()
    endif()
else()
    message(STATUS "ThirdParty/WgpuNative not present - foundation.rhi.webgpu will be skipped.")
endif()

# Naga + Tint - vendored COOK-TIME executables for the WGSL shader cook. naga-cli
# (SPIR-V -> WGSL, version-matched to WgpuNative's naga) is the translator; tint is
# the Chrome/Dawn conformance oracle run over its output. NOT linked and NOT loaded
# at run time - the cook shells out to them (core::RunProcess). An INTERFACE target
# carries the binary paths as compile defs so the WgslTranslator can find them; a
# missing per-platform binary just leaves the define empty (translator reports it).
add_library(shader_cook_tools INTERFACE)
add_library(ThirdParty::ShaderCookTools ALIAS shader_cook_tools)
# Keyed on the HOST, not the target: these run on the cook machine, so a cross build
# (wasm) still needs the host's binaries - plain WIN32/APPLE are false under Emscripten
# and would select the Linux ones on a Windows host.
if(CMAKE_HOST_WIN32)
    set(_naga_exe "${CMAKE_CURRENT_SOURCE_DIR}/Naga/bin/win-x64/naga.exe")
    set(_tint_exe "${CMAKE_CURRENT_SOURCE_DIR}/Tint/bin/win-x64/tint.exe")
elseif(CMAKE_HOST_APPLE)
    set(_naga_exe "${CMAKE_CURRENT_SOURCE_DIR}/Naga/bin/mac-arm64/naga")
    set(_tint_exe "${CMAKE_CURRENT_SOURCE_DIR}/Tint/bin/mac-arm64/tint")
else()
    set(_naga_exe "${CMAKE_CURRENT_SOURCE_DIR}/Naga/bin/linux-x86_64/naga")
    set(_tint_exe "${CMAKE_CURRENT_SOURCE_DIR}/Tint/bin/linux-x86_64/tint")
endif()
if(EXISTS "${_naga_exe}")
    target_compile_definitions(shader_cook_tools INTERFACE BUILDSYSTEM_NAGA_PATH="${_naga_exe}")
    message(STATUS "naga-cli (WGSL cook): ${_naga_exe}")
else()
    message(STATUS "naga-cli not present for this host - WGSL cook will report it missing.")
endif()
if(EXISTS "${_tint_exe}")
    target_compile_definitions(shader_cook_tools INTERFACE BUILDSYSTEM_TINT_PATH="${_tint_exe}")
    message(STATUS "tint (WGSL conformance): ${_tint_exe}")
else()
    message(STATUS "tint not present for this host - WGSL cook will FAIL unless validateWgsl=false is passed explicitly.")
endif()

# stb / cgltf / ufbx - vendored header(+source) libraries (copied, not submodules).
# Header include-dir INTERFACE targets; the *_impl translation units that define
# the implementations live in the engine libraries that use them.
add_library(stb INTERFACE)
add_library(ThirdParty::Stb ALIAS stb)
target_include_directories(stb SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/stb)

add_library(cgltf INTERFACE)
add_library(ThirdParty::Cgltf ALIAS cgltf)
target_include_directories(cgltf SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/cgltf)

add_library(ufbx INTERFACE)
add_library(ThirdParty::Ufbx ALIAS ufbx)
target_include_directories(ufbx SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/ufbx)

# miniaudio - vendored single-header audio library (github.com/mackron/miniaudio v0.11.25,
# public domain / MIT-0). Header include-dir INTERFACE target; the MINIAUDIO_IMPLEMENTATION
# translation unit lives in foundation.audio. Ogg Vorbis decoding is provided by
# ThirdParty/stb/stb_vorbis.c (public domain), compiled inside the same implementation TU.
add_library(miniaudio INTERFACE)
add_library(ThirdParty::Miniaudio ALIAS miniaudio)
target_include_directories(miniaudio SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/miniaudio)

# doctest - vendored single-header test framework (github.com/doctest/doctest v2.4.11, MIT).
# A copy of the source (not a submodule / not FetchContent); tests link doctest::doctest.
add_library(doctest INTERFACE)
add_library(doctest::doctest ALIAS doctest)
target_include_directories(doctest SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/doctest)

# Dear ImGui - vendored (copied from upstream, not a submodule). Compiled as a static library;
# the include dir is PUBLIC + SYSTEM so consumers (the extensions.imgui lib + apps) can
# #include "imgui.h" and call ImGui:: directly. Does NOT link policy, so the vendored
# sources build without our -Werror. No stock backends - The engine has its own RHI-based renderer.
# Builds for Emscripten too (portable C++; the debug panel works in the browser).
add_library(imgui STATIC
    imgui/imgui.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/imgui_demo.cpp)
add_library(ThirdParty::ImGui ALIAS imgui)
target_include_directories(imgui SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/imgui)
# OUTPUT_NAME, not just the target name: the engine extension target is `Imgui`, and every
# static lib lands in the same Bin/<cfg>/<plat>/lib. On Windows that directory is
# case-INSENSITIVE, so `imgui.lib` and `Imgui.lib` are one file and whichever archived last
# silently replaced the other - the executable then linked an Imgui.lib holding only the
# extension's objects and every ImGui:: symbol came up undefined. Case-sensitive filesystems
# hide this entirely, so it does not reproduce on Linux. Renaming the VENDORED side costs
# nothing: consumers refer to ThirdParty::ImGui, never to the file.
set_target_properties(imgui PROPERTIES POSITION_INDEPENDENT_CODE ON
                                       OUTPUT_NAME thirdparty_imgui)

# bc7enc - vendored BCn texture encoder (github.com/richgel999/bc7enc_rdo; permissive license,
# attribution requested). BC1/3/4/5 via rgbcx, BC7 via bc7enc, BC7 decode via bc7decomp (round-trip
# PSNR tests). Only the encoder/decoder core is vendored - no ISPC path, RDO, CLI, lodepng, or the
# committed ispc.exe. Cook/import-time only (Texture.Compression); never linked at runtime. Does NOT
# link policy (vendored code builds without our -Werror; SYSTEM include suppresses header warnings).
add_library(bc7enc STATIC
    bc7enc/bc7enc.cpp
    bc7enc/rgbcx.cpp
    bc7enc/bc7decomp.cpp)
add_library(ThirdParty::Bc7enc ALIAS bc7enc)
# The block encoders run at full optimisation in EVERY configuration (not MSVC: /O2 fights the
# Debug runtime checks): a Debug editor cooks textures too, and at -O0 a 4k BC7 encode took
# 84 s of one core (2026-09-23). Nothing here is ever stepped through.
if(NOT MSVC)
    target_compile_options(bc7enc PRIVATE $<$<CONFIG:Debug>:-O2>)
endif()

# bcdec - vendored single-header BCn DECODER (github.com/iOrange/bcdec, MIT/Unlicense). Implemented
# ONCE in Foundation::Image.DDS (the DDS level decoder); the compression tests include it a second
# time as the reference the engine's BC6H encoder is checked against. Header-only interface target.
add_library(bcdec INTERFACE)
add_library(ThirdParty::Bcdec ALIAS bcdec)
target_include_directories(bcdec SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/bcdec)
target_include_directories(bc7enc SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/bc7enc)
# Distinct archive name (case-insensitive-FS collision lesson, per imgui above).
set_target_properties(bc7enc PROPERTIES POSITION_INDEPENDENT_CODE ON
                                        OUTPUT_NAME thirdparty_bc7enc)

# meshoptimizer - vendored mesh optimization/simplification core (github.com/zeux/meshoptimizer,
# MIT; commit + trim manifest in meshoptimizer/README.md). Index/vertex reorder (vcache/overdraw/
# vfetch), simplification (auto-LOD chains), and the analyzers the cook tests log. Cook/import-time
# only (Geometry.Pipeline + the model importer); never linked by the runtime. Vendored code builds
# without our -Werror; SYSTEM include suppresses header warnings.
add_library(meshoptimizer STATIC
    meshoptimizer/allocator.cpp
    meshoptimizer/indexgenerator.cpp
    meshoptimizer/indexanalyzer.cpp
    meshoptimizer/rasterizer.cpp
    meshoptimizer/vcacheoptimizer.cpp
    meshoptimizer/overdrawoptimizer.cpp
    meshoptimizer/vfetchoptimizer.cpp
    meshoptimizer/simplifier.cpp)
add_library(ThirdParty::MeshOptimizer ALIAS meshoptimizer)
target_include_directories(meshoptimizer SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/meshoptimizer)
set_target_properties(meshoptimizer PROPERTIES POSITION_INDEPENDENT_CODE ON
                                               OUTPUT_NAME thirdparty_meshoptimizer)

# HarfBuzz - vendored text shaper (github.com/harfbuzz/harfbuzz 14.5.1, Old MIT; trim manifest in
# harfbuzz/README.md). Built from its own amalgamation with no FreeType/glib/ICU: its built-in
# Unicode data and OpenType shaping. Linked only by Fonts.HarfBuzz, one implementation of our
# ITextShaper (HarfBuzz is never used directly). Optimized in Debug too, as bc7enc is: UI text
# shapes every frame something changes. SYSTEM include suppresses its header warnings.
add_library(harfbuzz STATIC harfbuzz/src/harfbuzz.cc)
add_library(ThirdParty::HarfBuzz ALIAS harfbuzz)
if(NOT MSVC)
    target_compile_options(harfbuzz PRIVATE $<$<CONFIG:Debug>:-O2>)
endif()
target_include_directories(harfbuzz SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/harfbuzz/src)
set_target_properties(harfbuzz PROPERTIES POSITION_INDEPENDENT_CODE ON
                                          OUTPUT_NAME thirdparty_harfbuzz)

# SheenBidi - vendored Unicode bidirectional algorithm (github.com/Tehreer/SheenBidi v3.0.0,
# Apache-2.0; manifest in sheenbidi/README.md). Built as C from its own unity source. Linked only by
# Fonts.Bidi, behind the engine's own bidi interface (never used directly). Optimized in Debug, as
# HarfBuzz is: a paragraph is resolved whenever its text changes. SYSTEM include suppresses its
# header warnings.
add_library(sheenbidi STATIC sheenbidi/Source/SheenBidi.c)
add_library(ThirdParty::SheenBidi ALIAS sheenbidi)
target_compile_definitions(sheenbidi PRIVATE SB_CONFIG_UNITY)
target_include_directories(sheenbidi SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/sheenbidi/Headers)
target_include_directories(sheenbidi PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/sheenbidi/Source)
if(NOT MSVC)
    target_compile_options(sheenbidi PRIVATE $<$<CONFIG:Debug>:-O2>)
endif()
set_target_properties(sheenbidi PROPERTIES POSITION_INDEPENDENT_CODE ON
                                           OUTPUT_NAME thirdparty_sheenbidi)

# astcenc - vendored ASTC encoder/decoder (github.com/ARM-software/astc-encoder; Apache-2.0). The
# mobile-web compressed family (asset-variants Decision 4). Only the astcenc_*.cpp core is vendored -
# no astcenccli_* front end, no test/bench harness (see astcenc/README.draconic.md). Forced onto the
# scalar "none" ISA path so ONE static lib builds on desktop x86 AND wasm with no per-arch source
# selection - a cook-time encoder is never on a runtime hot path. Cook/import-time only
# (Texture.Compression); never linked at runtime. SYSTEM include suppresses its header warnings.
file(GLOB ASTCENC_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/astcenc/astcenc_*.cpp)
add_library(astcenc STATIC ${ASTCENC_SOURCES})
add_library(ThirdParty::Astcenc ALIAS astcenc)
if(NOT MSVC)
    target_compile_options(astcenc PRIVATE $<$<CONFIG:Debug>:-O2>) # see bc7enc above
endif()
target_include_directories(astcenc SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/astcenc)
target_compile_definitions(astcenc PUBLIC
    ASTCENC_SSE=0 ASTCENC_AVX=0 ASTCENC_POPCNT=0 ASTCENC_F16C=0 ASTCENC_NEON=0 ASTCENC_SVE=0)
set_target_properties(astcenc PROPERTIES POSITION_INDEPENDENT_CODE ON
                                         OUTPUT_NAME thirdparty_astcenc)

# Compiled third-party whose consumers have not yet climbed to web: msdfgen only
# (import-time font tooling; the web build ships no authoring). Jolt/AngelScript/imgui all
# build for wasm and live outside this gate.
if(NOT EMSCRIPTEN)

# msdfgen - multi-channel signed distance field generator (compiled from source).
# Import-time only: used by the font atlas baker, not linked at runtime.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/msdfgen/CMakeLists.txt)
    set(MSDFGEN_CORE_ONLY          ON  CACHE BOOL "" FORCE)
    set(MSDFGEN_BUILD_STANDALONE   OFF CACHE BOOL "" FORCE)
    set(MSDFGEN_USE_OPENMP         OFF CACHE BOOL "" FORCE)
    set(MSDFGEN_USE_SKIA           OFF CACHE BOOL "" FORCE)
    set(MSDFGEN_USE_VCPKG          OFF CACHE BOOL "" FORCE)
    set(MSDFGEN_INSTALL            OFF CACHE BOOL "" FORCE)
    set(MSDFGEN_DYNAMIC_RUNTIME    ON  CACHE BOOL "" FORCE)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    add_subdirectory(msdfgen)
    unset(CMAKE_POLICY_VERSION_MINIMUM)
    add_library(ThirdParty::MsdfgenCore ALIAS msdfgen-core)
endif()

endif() # NOT EMSCRIPTEN (msdfgen gated until a web consumer needs it)

# Jolt Physics v5.3.0 - vendored BY COPY (Jolt/ source tree + LICENSE; no submodule).
# Built for Emscripten too (JoltPhysics.js is this same C++ compiled to wasm).
# Compiled from source as one static lib with a fixed, engine-chosen configuration
# (deterministic-enough defaults; no Jolt CMake options surface). Include dir is
# PUBLIC + SYSTEM so foundation.physics can #include <Jolt/...>; consumers MUST define
# the same JPH_* macros, so they are PUBLIC compile definitions here. Does not link
# policy (vendored code builds without our -Werror).
file(GLOB_RECURSE util_jolt_sources ${CMAKE_CURRENT_SOURCE_DIR}/JoltPhysics/Jolt/*.cpp)
add_library(jolt STATIC ${util_jolt_sources})
add_library(ThirdParty::Jolt ALIAS jolt)
target_include_directories(jolt SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/JoltPhysics)
target_compile_features(jolt PUBLIC cxx_std_17)
target_compile_definitions(jolt PUBLIC
    $<$<CONFIG:Debug>:JPH_ENABLE_ASSERTS>)
set_target_properties(jolt PROPERTIES POSITION_INDEPENDENT_CODE ON)

# AngelScript 2.39.0 WIP - vendored BY COPY (github.com/anjo76/angelscript, sdk library
# + the scriptstdstring, scriptbuilder and scriptarray add-ons; no submodule). scriptarray
# provides the native `array<T>` type - facades return engine Array<T>, which the backend
# renders as a CScriptArray (script-array-returns.md). Compiled from source as
# one static lib; per-architecture native-call sources self-select via as_config.h #ifdefs,
# so globbing every .cpp is safe. Include dirs are PUBLIC + SYSTEM so foundation.script.angelscript
# can #include <angelscript.h> / <scriptstdstring/scriptstdstring.h> /
# <scriptbuilder/scriptbuilder.h> from its module IMPLEMENTATION unit. Does not link
# policy (vendored code builds without our -Werror). scriptstdstring_utils.cpp is
# deliberately not vendored (needs scriptarray). scriptbuilder parses [metadata] ahead of
# declarations (the AngelScript editor cook harvests behavior properties through it).
# Gated on OPTION_ENABLE_ANGELSCRIPT: a disabled backend never compiles its VM.
if(OPTION_ENABLE_ANGELSCRIPT)
file(GLOB util_angelscript_sources ${CMAKE_CURRENT_SOURCE_DIR}/angelscript/source/*.cpp)
# x64 Windows needs a hand-written native-call trampoline; the glob above only picks up .cpp,
# and AngelScript ships the same routines twice, once per assembler dialect. Clang takes the
# GNU-syntax .S; MSVC takes the MASM .asm (built via enable_language(ASM_MASM) at the root).
# Miss this and CallX64 / GetReturnedFloat / GetReturnedDouble are unresolved at link.
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    list(APPEND util_angelscript_sources
        ${CMAKE_CURRENT_SOURCE_DIR}/angelscript/source/as_callfunc_x64_msvc_clang.S)
elseif(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    list(APPEND util_angelscript_sources
        ${CMAKE_CURRENT_SOURCE_DIR}/angelscript/source/as_callfunc_x64_msvc_asm.asm)
endif()
# TWO engine libraries embed this archive (Foundation/Script.AngelScript and
# Pipeline/Script.AngelScript.Pipeline - an asIScriptEngine* crosses between
# them). Static per-target embedding is fine in a static engine (the final link
# dedups), but under ENGINE_SHARED_LIBS each .so would carry its OWN AngelScript
# runtime (two thread managers, UB on the crossing pointer) - so the archive
# follows the engine's library type (shared-libraries.md P3).
if(ENGINE_SHARED_LIBS)
    set(_angelscript_lib_type SHARED)
else()
    set(_angelscript_lib_type STATIC)
endif()
add_library(angelscript ${_angelscript_lib_type}
    ${util_angelscript_sources}
    ${CMAKE_CURRENT_SOURCE_DIR}/angelscript/add_on/scriptstdstring/scriptstdstring.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/angelscript/add_on/scriptbuilder/scriptbuilder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/angelscript/add_on/scriptarray/scriptarray.cpp)
add_library(ThirdParty::AngelScript ALIAS angelscript)
target_include_directories(angelscript SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/angelscript/include
    ${CMAKE_CURRENT_SOURCE_DIR}/angelscript/add_on)
target_compile_features(angelscript PRIVATE cxx_std_17)
set_target_properties(angelscript PROPERTIES POSITION_INDEPENDENT_CODE ON)
# Windows: nothing leaves a DLL without an export, and a DLL that exports nothing
# produces no import library at all (consumers then fail with LNK1104 "cannot open
# angelscript.lib"). Unlike our own libraries this is a CLASSIC C++ target, not a
# C++20 module, so CMake's built-in bindexplib handles it - the module-attached
# symbol gap that forced our own .def generator does not apply here.
# shared-libraries.md P5/W2.
set_target_properties(angelscript PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
if(NOT EMSCRIPTEN)
    find_package(Threads REQUIRED)
    target_link_libraries(angelscript PUBLIC Threads::Threads)
endif()
endif() # OPTION_ENABLE_ANGELSCRIPT
# On Emscripten as_config.h auto-enables AS_MAX_PORTABILITY (native-call trampolines compile out; the
# backend is all-generic) AND AS_NO_THREADS, so the web build needs no pthread / SharedArrayBuffer and
# no manual define here - matching the single-threaded web v1 (JobSystem inline) model.

# Luau 0.733 - vendored BY COPY, FULL upstream tree (github.com/luau-lang/luau, release
# tag 0.733; MIT + the original Lua license). We define our OWN target over the sources
# rather than add_subdirectory-ing their CMake (house pattern; their build wants options,
# tests, and CLI wiring we do not). Compiled TODAY: VM + Compiler + Ast + Bytecode +
# Common + Config - the interpreter + source->bytecode path. In the tree but NOT compiled
# yet: Analysis (the P5 typechecker for script_validate - own target when that lands),
# CodeGen (JIT - our targets are interpreter-only), CLI/tests/bench/fuzz/tools/extern/
# Inliner/Require. Being vendored in full keeps upstream bumps a plain re-copy.
#
# LUA_USE_LONGJMP=1 is REQUIRED, PUBLIC: the engine builds -fno-exceptions, so Luau's
# error unwinding must be longjmp, never a C++ throw - a script error crossing our frames
# as an exception is a hard abort. PUBLIC so every consumer sees the same luaconf shape.
# BYTECODE COMPAT NOTE: Luau bytecode is NOT stable across versions - the script cook
# fingerprint must include this vendored version (see docs/specs/luau-backend.md).
# Gated on OPTION_ENABLE_LUAU: a disabled backend never compiles its VM.
if(OPTION_ENABLE_LUAU)
file(GLOB util_luau_sources
    ${CMAKE_CURRENT_SOURCE_DIR}/luau/VM/src/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/luau/Compiler/src/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/luau/Ast/src/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/luau/Bytecode/src/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/luau/Common/src/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/luau/Config/src/*.cpp)
add_library(luau STATIC ${util_luau_sources})
add_library(ThirdParty::Luau ALIAS luau)
target_include_directories(luau SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/luau/VM/include
    ${CMAKE_CURRENT_SOURCE_DIR}/luau/Compiler/include
    ${CMAKE_CURRENT_SOURCE_DIR}/luau/Ast/include
    ${CMAKE_CURRENT_SOURCE_DIR}/luau/Bytecode/include
    ${CMAKE_CURRENT_SOURCE_DIR}/luau/Common/include
    ${CMAKE_CURRENT_SOURCE_DIR}/luau/Config/include)
target_include_directories(luau PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/luau/VM/src)
target_compile_definitions(luau PUBLIC LUA_USE_LONGJMP=1)
target_compile_features(luau PRIVATE cxx_std_17)
set_target_properties(luau PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif() # OPTION_ENABLE_LUAU

# recastnavigation v1.6.0 - navmesh bake (Recast) + runtime navmesh/query (Detour) + agent
# crowd/avoidance (DetourCrowd), vendored BY COPY (github.com/recastnavigation/recastnavigation
# tag v1.6.0, zlib; see recastnavigation/README.vendored.md) like Jolt. Compiled from source as
# one static lib. DetourTileCache + DebugUtils are vendored in-tree but NOT compiled (the tile
# cache joins navigation P2; debug draw uses the engine's own draw). Include dirs of the three
# compiled components are PUBLIC + SYSTEM so foundation.navigation can #include <Recast.h> /
# <DetourNavMesh.h> / <DetourNavMeshQuery.h> / <DetourCrowd.h> from its implementation units.
# Pure exception-free C++; built for Emscripten too (JoltPhysics.js precedent). Does not link
# policy (vendored code builds without our -Werror).
file(GLOB util_recast_sources
    ${CMAKE_CURRENT_SOURCE_DIR}/recastnavigation/Recast/Source/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/recastnavigation/Detour/Source/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/recastnavigation/DetourCrowd/Source/*.cpp)
add_library(recast STATIC ${util_recast_sources})
add_library(ThirdParty::Recast ALIAS recast)
target_include_directories(recast SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/recastnavigation/Recast/Include
    ${CMAKE_CURRENT_SOURCE_DIR}/recastnavigation/Detour/Include
    ${CMAKE_CURRENT_SOURCE_DIR}/recastnavigation/DetourCrowd/Include)
target_compile_features(recast PUBLIC cxx_std_17)
set_target_properties(recast PROPERTIES POSITION_INDEPENDENT_CODE ON
                                        OUTPUT_NAME thirdparty_recast)

# Mbed TLS 4.2.0 with its crypto, TF-PSA-Crypto - TLS (1.2 and 1.3) and X.509 for the engine's
# HTTP client and secure WebSockets (wss), vendored BY COPY (github.com/Mbed-TLS/mbedtls release
# mbedtls-4.2.0, Apache-2.0 OR GPL-2.0-or-later, taken under Apache-2.0; see
# mbedtls/README.vendored.md). Compiled from source as one static C library, the source list
# upstream's own CMake configures with its default configuration (no defines: the configuration
# is its headers, include/mbedtls/mbedtls_config.h and tf-psa-crypto/include/psa/crypto_config.h),
# its generated sources shipped in the release (no Python at build time). The public include
# dirs are PUBLIC + SYSTEM (<mbedtls/ssl.h>, <psa/crypto.h>); the rest are private. Windows takes
# ws2_32 (net_sockets) and bcrypt (the entropy source). Built for Emscripten too, though a game in
# a browser speaks TLS through the browser's fetch and WebSocket. Does not link policy (vendored
# code builds without our -Werror).
set(util_mbedtls_dir ${CMAKE_CURRENT_SOURCE_DIR}/mbedtls)
set(util_tfpsa_dir ${util_mbedtls_dir}/tf-psa-crypto)
file(GLOB util_mbedtls_sources
    ${util_mbedtls_dir}/library/*.c
    ${util_tfpsa_dir}/core/*.c
    ${util_tfpsa_dir}/platform/*.c
    ${util_tfpsa_dir}/utilities/*.c
    ${util_tfpsa_dir}/extras/*.c
    ${util_tfpsa_dir}/drivers/builtin/src/*.c
    # Everest as upstream builds it: the joined unit picks the 128-bit Curve25519 where the
    # compiler has the type, the portable one elsewhere (MSVC); the parts never build alone.
    ${util_tfpsa_dir}/drivers/everest/library/Hacl_Curve25519_joined.c
    ${util_tfpsa_dir}/drivers/everest/library/x25519.c
    ${util_tfpsa_dir}/drivers/p256-m/*.c
    ${util_tfpsa_dir}/drivers/p256-m/p256-m/*.c
    ${util_tfpsa_dir}/drivers/pqcp/src/*.c)
add_library(mbedtls STATIC ${util_mbedtls_sources})
add_library(ThirdParty::MbedTLS ALIAS mbedtls)
target_include_directories(mbedtls SYSTEM PUBLIC
    ${util_mbedtls_dir}/include
    ${util_tfpsa_dir}/include
    ${util_tfpsa_dir}/drivers/builtin/include
    ${util_tfpsa_dir}/drivers/everest/include
    ${util_tfpsa_dir}/drivers/pqcp/include)
target_include_directories(mbedtls PRIVATE
    ${util_mbedtls_dir}/library
    ${util_tfpsa_dir}/core
    ${util_tfpsa_dir}/dispatch
    ${util_tfpsa_dir}/extras
    ${util_tfpsa_dir}/platform
    ${util_tfpsa_dir}/utilities
    ${util_tfpsa_dir}/drivers/builtin/src
    ${util_tfpsa_dir}/drivers/p256-m
    ${util_tfpsa_dir}/drivers/p256-m/p256-m
    ${util_tfpsa_dir}/drivers/pqcp/src
    ${util_tfpsa_dir}/drivers/everest/include/tf-psa-crypto/private/everest
    ${util_tfpsa_dir}/drivers/everest/include/tf-psa-crypto/private/everest/kremlib)
if(WIN32)
    target_link_libraries(mbedtls PUBLIC ws2_32 bcrypt)
endif()
# Thread-safe: the crypto layer keeps global state (its random generator, its key store) and the
# engine reaches it from more than one thread (a run's HTTP client on its worker, the game thread's
# wss joins and gateway). Mbed TLS: "You must enable this option if TF-PSA-Crypto runs in a
# multithreaded environment." pthreads where they exist; on Windows the alternate layer, whose
# types are mbedtls-house/threading_alt.h and whose functions the engine's backend installs. The
# web links no Threads package (that would turn on wasm threads); its pthread mutexes are libc's.
target_compile_definitions(mbedtls PUBLIC MBEDTLS_THREADING_C)
if(WIN32)
    target_compile_definitions(mbedtls PUBLIC MBEDTLS_THREADING_ALT)
    target_include_directories(mbedtls SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/mbedtls-house)
else()
    target_compile_definitions(mbedtls PUBLIC MBEDTLS_THREADING_PTHREAD)
    if(NOT EMSCRIPTEN)
        find_package(Threads REQUIRED)
        target_link_libraries(mbedtls PUBLIC Threads::Threads)
    endif()
endif()
set_target_properties(mbedtls PROPERTIES C_STANDARD 99
                                         POSITION_INDEPENDENT_CODE ON
                                         OUTPUT_NAME thirdparty_mbedtls)
