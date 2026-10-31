# Game engine root build
#
# C++23, C++ modules. Requires CMake >= 3.28 and a modules-capable toolchain
# (Clang 17+, GCC 14+, or MSVC 19.36+) with the Ninja generator.

cmake_minimum_required(VERSION 3.28)

project(GameEngine
    VERSION 0.1.0
    DESCRIPTION "Game engine"
    LANGUAGES CXX C ASM) # C: SheenBidi (the bidi algorithm) builds as C

# MSVC assembles with ml64, not cl, so the plain ASM language above does not apply to it
# (hence "CMP194: MSVC is not an assembler for language ASM" at configure). AngelScript's x64
# native-call trampoline is hand-written assembly and is the only consumer; without MASM its
# CallX64/GetReturnedFloat/GetReturnedDouble go unresolved at link.
if(MSVC)
    enable_language(ASM_MASM)
endif()

# --- Global language settings ------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Position-independent code so the static engine libraries can be linked into
# dynamically-loaded plugin shared objects (Foundation::Runtime plugin host).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Build the engine's library targets as shared (.so/.dll) instead of static.
# Every first-party library goes through util_add_engine_library, which reads
# this option. BUILD_SHARED_LIBS would have no effect, because the library type
# is passed explicitly.
option(ENGINE_SHARED_LIBS "Build engine libraries as shared libraries" OFF)

# A game project's native directory, added to this build as a subdirectory. The game's
# module builds like any engine library (full C++20 module and BMI access through
# util_add_engine_library), so the dev .so and the shipping static library come from the
# same source. Engine.GamePlayer, the shipping player with the game statically linked,
# exists when ENGINE_GAME_NATIVE_TARGET names the game's target. The exporter sets these;
# setting them by hand works too.
set(ENGINE_GAME_NATIVE_DIR "" CACHE PATH "Game project's native source directory (adds it to this build)")
set(ENGINE_GAME_NATIVE_TARGET "" CACHE STRING "The game native target name (enables Engine.GamePlayer)")
if(ENGINE_SHARED_LIBS AND EMSCRIPTEN)
    # Web is always a single self-contained binary: browsers have no runtime plugin
    # loading, Emscripten dynamic linking gives up whole-program optimization and bloats
    # the main module, and web games extend the engine through scripts.
    message(FATAL_ERROR "ENGINE_SHARED_LIBS is not supported for Emscripten - the web "
                        "target is always a static single binary (shared-libraries.md).")
endif()
if(ENGINE_SHARED_LIBS)
    # Binaries and libraries land together in Bin/<cfg>/<plat>; $ORIGIN makes the
    # loader find sibling .so files without LD_LIBRARY_PATH.
    list(APPEND CMAKE_BUILD_RPATH "$ORIGIN")
    set(CMAKE_INSTALL_RPATH "$ORIGIN")
endif()

# Single-config generators default to Debug.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

# --- Output layout: Bin/<Config>/<Platform>-<Compiler> at the repo root -------
# Final artifacts (exes, DLLs/.so, static libs, PDBs) land here instead of being
# scattered through the build tree mirroring the source layout. Object files and
# C++20-module BMIs stay in the build tree; only deliverables move. The compiler
# suffix keeps concurrent toolchains (e.g. clang and gcc, both Debug) from clobbering
# each other. $<CONFIG> resolves for single- and multi-config generators, and stops
# a multi-config generator from appending its own extra per-config subdirectory.
if(WIN32)
    set(_dr_plat "Win64")
elseif(APPLE)
    set(_dr_plat "Mac64")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_dr_plat "Linux64")
else()
    set(_dr_plat "${CMAKE_SYSTEM_NAME}")
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(_dr_comp "GCC")
else()
    set(_dr_comp "${CMAKE_CXX_COMPILER_ID}")   # Clang / MSVC / AppleClang / ...
endif()

# Sanitizer builds get their own output dir, so sanitized libraries never overwrite the
# regular ones in a shared Bin. Set BUILDSYSTEM_OUTPUT_SUFFIX (e.g. "-ASAN") in sanitizer
# configure runs; a -fsanitize build without one gets "-ASAN" below.
set(BUILDSYSTEM_OUTPUT_SUFFIX "" CACHE STRING "Suffix for the Bin output dir (e.g. -ASAN)")
# Shared builds get their own dir the same way, so build/clang and build/clang-shared do
# not overwrite each other's binaries. An explicit BUILDSYSTEM_OUTPUT_SUFFIX still wins.
if(ENGINE_SHARED_LIBS AND BUILDSYSTEM_OUTPUT_SUFFIX STREQUAL "")
    set(BUILDSYSTEM_OUTPUT_SUFFIX "-Shared")
endif()
# A sanitizer build gets its suffix from its flags, without relying on the configure run to
# pass one: sanitized binaries and archives in a shared Bin would replace the plain build's,
# and their newer mtimes keep ninja from relinking.
if(BUILDSYSTEM_OUTPUT_SUFFIX STREQUAL "" AND CMAKE_CXX_FLAGS MATCHES "-fsanitize")
    set(BUILDSYSTEM_OUTPUT_SUFFIX "-ASAN")
endif()
set(BUILDSYSTEM_BIN_DIR "${CMAKE_SOURCE_DIR}/Bin/$<CONFIG>/${_dr_plat}-${_dr_comp}${BUILDSYSTEM_OUTPUT_SUFFIX}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY     "${BUILDSYSTEM_BIN_DIR}")       # exes and Windows DLLs
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY     "${BUILDSYSTEM_BIN_DIR}")       # .so plugins/modules (beside the exe)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY     "${BUILDSYSTEM_BIN_DIR}/lib")   # static .a/.lib and MSVC import libs
set(CMAKE_PDB_OUTPUT_DIRECTORY         "${BUILDSYSTEM_BIN_DIR}")       # MSVC linker PDBs (beside the exe)
# Not set for MSVC: compile PDBs are per-object intermediates, and CMake names them from the
# compiler default (vc<ver>.pdb) rather than the target, so pointing every target at one shared
# directory makes all of them write the same file. Parallel cl.exe jobs then fail with
# "C1041: cannot open program database", and /FS alone does not prevent it. Left unset, each
# target writes into its own build directory. The linker PDBs, the ones worth having beside the
# binaries, come from CMAKE_PDB_OUTPUT_DIRECTORY above.
if(NOT MSVC)
    set(CMAKE_COMPILE_PDB_OUTPUT_DIRECTORY "${BUILDSYSTEM_BIN_DIR}/lib")
endif()

# Reusable helpers (util_copy_runtime_deps, ...).
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Helpers.cmake)
# BUILDSYSTEM_NOTICES_FILE: the third-party notices every shipped executable stages beside it.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/ThirdPartyNotices.cmake)

# Add a module's sibling doctest suite, but only when the module target was built, so the web
# subset and platform-gated backends (Shell.Desktop, ...) skip cleanly and a test never outlives
# its module. The call sites are wrapped once in if(OPTION_BUILD_TESTS).
macro(util_test_suite dir target)
    if(TARGET ${target})
        add_subdirectory(${dir})
    endif()
endmacro()

# --- Options -----------------------------------------------------------------
option(OPTION_BUILD_TESTS "Build unit tests" ON)
option(OPTION_BUILD_SAMPLES "Build sample applications" ON)
option(OPTION_BUILD_TOOLS "Build editor/authoring tooling (the editor libraries + CLI exes)" ON)
option(OPTION_BUILD_DEPRECATED "Build Code/Deprecated (the old UI framework and its UISandbox)" OFF)
# Optional extensions are independently switchable; each exposes an OPTION_HAS_* compile define
# (see the policy block) so C++ can guard usage with #if.
option(OPTION_GUI_LIBFUZZER "Build a libFuzzer executable per GUI fuzz target (clang; configure the tree with -fsanitize=fuzzer-no-link,address)" OFF)
option(OPTION_EXTENSION_IMGUI  "Build the Dear ImGui debug-UI extension (extensions.imgui)" ON)
# Script backends are independently switchable; each defines OPTION_HAS_<backend> (presence macro,
# guard C++ with `#ifdef`) when enabled. At least one must stay on. AngelScript additionally needs
# its vendored lib to have built (reconciled after ThirdParty is added, before the backend subdirs).
option(OPTION_ENABLE_ANGELSCRIPT "Build the AngelScript script backend" ON)
option(OPTION_ENABLE_LUAU        "Build the Luau script backend" ON)
option(OPTION_WARNINGS_AS_ERRORS "Treat warnings as errors" ON)

# A web (Emscripten) build ships a game, not the authoring toolchain: force the tooling group off
# up front, before the policy defines below capture it. The GUI (foundation.gui) stays, since a web
# game has the engine's GUI (Engine.GUI) as a desktop one does.
if(EMSCRIPTEN)
    set(OPTION_BUILD_TOOLS OFF)
    # OPTION_EXTENSION_IMGUI stays available on web: Dear ImGui and extensions.imgui compile for
    # wasm (the renderer is pure RHI, the subsystem consumes the shell's input), so the debug
    # panel works in the browser too (WebScene's tweak UI).

    # One link at a time (Ninja job pool): a wasm link runs wasm-opt, which alone peaks near 5 GB,
    # so -j4 linking four tests at once runs the box out of memory. Compiles keep -j4.
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS wasm_link=1)
    set(CMAKE_JOB_POOL_LINK wasm_link)
endif()

# --- Product binary basenames: single source of truth ------------------------
# A few executables are referenced by name or path from outside their own target, so the name
# cannot live only in the target definition:
#   - the export host template stages the player and reads "<player>.runtime-libs" beside the tool
#     (Editor.Project gets the name below as the BUILTIN_PLAYER_BASENAME compile define)
#   - the web build invokes the native shader-pack cook by path (the wasm build reaches into the
#     desktop Bin dir, so it cannot use $<TARGET_FILE:>); WebScene and TerrainPlayground read this name
# Change a name here and every reference (OUTPUT_NAME, the C++ constant, the sidecar, the cook path)
# follows. Dotted, matching the target names.
set(BUILTIN_PLAYER_BASENAME         "Engine.Player")
set(BUILDSYSTEM_SHADERPACKCOOK_BASENAME "Tools.ShaderPack")
set(USER_DATA_DIR_NAME_VALUE        "gameengine")  # OS user-data folder (settings/projects/templates)
set(TEMPLATE_ID_PREFIX_VALUE        "gameengine")  # export-template id prefix (<prefix>-<platform>-<config>-<ver>)

# Where the desktop build put its executables, as seen from a cross build (wasm). The web
# build shells out to native cook tools by path, and cannot use $<TARGET_FILE:> because those
# targets do not exist in the wasm configure, so it needs the host's Bin dir. Derived from
# CMAKE_HOST_* (CMAKE_SYSTEM_NAME is "Emscripten" here, so the _dr_plat above would be wrong),
# and the exe suffix is the host's, since CMAKE_EXECUTABLE_SUFFIX is ".js" under Emscripten.
# Assumes the desktop build used Clang (the documented dev toolchain); override the cache
# variable for a GCC/MSVC host build or a non-default Bin location.
if(CMAKE_HOST_WIN32)
    set(_dr_host_plat "Win64")
    set(BUILDSYSTEM_HOST_EXE_SUFFIX ".exe")
elseif(CMAKE_HOST_APPLE)
    set(_dr_host_plat "Mac64")
    set(BUILDSYSTEM_HOST_EXE_SUFFIX "")
else()
    set(_dr_host_plat "Linux64")
    set(BUILDSYSTEM_HOST_EXE_SUFFIX "")
endif()
set(BUILDSYSTEM_HOST_PLATFORM "${_dr_host_plat}" CACHE STRING
    "Host platform folder name under Bin/<config>/ (cross builds locate native tools there)")
set(BUILDSYSTEM_HOST_BIN_DIR "${CMAKE_SOURCE_DIR}/Bin/Debug/${BUILDSYSTEM_HOST_PLATFORM}-Clang"
    CACHE PATH "Desktop Bin dir holding the native cook tools a cross (web) build shells out to")

# Enable testing before any subdirectory so add_test() calls made by modules that
# register their own Tests/ (e.g. Runtime) are picked up by ctest.
if(OPTION_BUILD_TESTS)
    enable_testing()
endif()

# --- Engine-wide compile policy ----------------------------------------------
# Exceptions and C++ RTTI are disabled engine-wide.
add_library(policy INTERFACE)
add_library(Foundation::Policy ALIAS policy)

target_compile_features(policy INTERFACE cxx_std_23)

# Shipping flavor (Core/Prelude.h requires exactly one of DEBUG, RELEASE and SHIPPING): an
# optimized build with asserts compiled out and stripped binaries, the config for dists and size
# measurements. The clang-shipping and gcc-shipping presets turn it on (Release, with a
# BUILDSYSTEM_OUTPUT_SUFFIX so it never clobbers a plain Release Bin dir).
option(OPTION_SHIPPING_BUILD "Shipping config: asserts compiled out, binaries stripped" OFF)

target_compile_definitions(policy INTERFACE
    $<$<CONFIG:Debug>:BUILD_DEBUG=1>
    $<$<AND:$<CONFIG:Release,RelWithDebInfo,MinSizeRel>,$<NOT:$<BOOL:${OPTION_SHIPPING_BUILD}>>>:BUILD_RELEASE=1>
    $<$<BOOL:${OPTION_SHIPPING_BUILD}>:BUILD_SHIPPING=1>
    # Drives ENGINE_EXPORT_DATA (Core/Prelude.h). It is global rather than per target because
    # one BMI is shared by the library that defines an entity and every consumer, so the
    # annotation must expand the same way everywhere.
    $<$<BOOL:${ENGINE_SHARED_LIBS}>:BUILDSYSTEM_SHARED_LIBS=1>
    # The profiler (CPU scopes, GPU timings, the overlay) is in every build but Shipping, Release
    # included, since Release is where timings mean something; in Shipping it compiles to nothing.
    $<$<NOT:$<BOOL:${OPTION_SHIPPING_BUILD}>>:BUILD_PROFILING=1>
    # Build identity as strings, matching the Bin/<Config>/<Platform>-<Compiler> layout: the export
    # host template stamps itself with the config/compiler that built the running tool.
    BUILD_CONFIG=\"$<CONFIG>\"
    BUILD_COMPILER=\"${_dr_comp}\"
    # Optional-module presence flags (0 or 1); guard their use in C++ with, for example,
    # `#if OPTION_HAS_EXTENSION_IMGUI`.
    OPTION_HAS_EXTENSION_IMGUI=$<BOOL:${OPTION_EXTENSION_IMGUI}>
    USER_DATA_DIR_NAME=u8\"${USER_DATA_DIR_NAME_VALUE}\"
    TEMPLATE_ID_PREFIX=u8\"${TEMPLATE_ID_PREFIX_VALUE}\")

target_link_options(policy INTERFACE
    # Shipping: strip symbol tables at link, so the binary's size is what a dist ships.
    $<$<AND:$<BOOL:${OPTION_SHIPPING_BUILD}>,$<CXX_COMPILER_ID:Clang,GNU>>:-s>)

# MSVC: CMake seeds CMAKE_CXX_FLAGS with "/EHsc". Appending /EHs-c- below does win, but cl
# reports every override ("D9025: overriding '/EHs' with '/EHs-'") once per source file, which
# buries real diagnostics. Remove the default instead; the policy flag below is the one place
# exception behaviour is set.
if(MSVC)
    string(REGEX REPLACE "[-/]EH[acsr-]+" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(STRIP "${CMAKE_CXX_FLAGS}" CMAKE_CXX_FLAGS)
endif()

target_compile_options(policy INTERFACE
    $<$<CXX_COMPILER_ID:Clang,GNU>:-fno-exceptions;-fno-rtti>
    # Within one target, parallel cl.exe jobs write the same compile PDB (the compiler's default
    # vc<ver>.pdb) and can fail with "C1041: cannot open program database". /FS funnels those
    # writes through mspdbsrv, as the diagnostic recommends.
    $<$<CXX_COMPILER_ID:MSVC>:/FS>
    # The tree is UTF-8 and uses u8"..." and non-ASCII character literals (e.g. U'●').
    # Without /utf-8 cl decodes sources in the system ANSI codepage and silently mis-reads
    # them: a 3-byte UTF-8 scalar in a char32_t literal becomes "C2015: too many characters
    # in constant". This does not show up as C4819, which only fires when a character cannot
    # be represented at all, so the absence of C4819 does not mean /utf-8 is unneeded.
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    # GCC modules: eager gcm loading. The default lazy loader intermittently fails to merge
    # template pendings across module imports ("failed to read compiled module cluster" /
    # "failed to load pendings for ..."), reproducibly in Tools.Cook's import mix on GCC 15.2.
    $<$<CXX_COMPILER_ID:GNU>:-fno-module-lazy>
    $<$<CXX_COMPILER_ID:MSVC>:/EHs-c-;/GR->)

# Warnings
target_compile_options(policy INTERFACE
    $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall;-Wextra;-Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>)

if(OPTION_WARNINGS_AS_ERRORS)
    target_compile_options(policy INTERFACE
        $<$<CXX_COMPILER_ID:Clang,GNU>:-Werror>
        # /wd4530: "C++ exception handler used, but unwind semantics are not enabled". We disable
        # exceptions on purpose (/EHs-c- above) while still using the STL, so cl emits this from
        # its own headers, never from Code/. It restates that choice rather than finding anything,
        # and under /WX it would fail otherwise clean targets.
        $<$<CXX_COMPILER_ID:MSVC>:/WX;/wd4530>)
    # Clang on Windows emits a spurious "argument unused during compilation: '-c'"
    # during the CMake module-dependency scan step. Suppress just that diagnostic
    # so -Werror doesn't kill the build.
    # Clang 22+ warns that __COUNTER__ is a "C2y extension" when used in C++ mode;
    # doctest (and other widely-used macros) rely on it, so suppress the diagnostic.
    target_compile_options(policy INTERFACE
        $<$<CXX_COMPILER_ID:Clang>:-Wno-error=unused-command-line-argument;-Wno-c2y-extensions>)
    # em++ re-invokes clang++ for the module precompile and does not forward
    # -Wno-error=unused-command-line-argument to that inner driver, so the same spurious
    # "'-c' unused" diagnostic is fatal again on the wasm build. Suppress it outright here
    # (it is never actionable: CMake, not us, passes both --precompile and -c).
    if(EMSCRIPTEN)
        target_compile_options(policy INTERFACE -Wno-unused-command-line-argument)
    endif()
endif()

# --- Include roots -----------------------------------------------------------
# Role-grouped layout: each collection lives at Code/<Role>/<Module> and exposes its public headers
# by collection-relative path ("Core/Prelude.h", "RHI/...", ...). The role dirs below are the global
# include roots, so a header resolves by that path regardless of which role owns it (moving a module
# between roles never touches an #include). Code/ is also a root for shared headers (TestMain.h).
include_directories(
    ${CMAKE_SOURCE_DIR}/Code            # shared headers (TestMain.h) and any cross-role prefixes
    ${CMAKE_SOURCE_DIR}/Code/Foundation
    ${CMAKE_SOURCE_DIR}/Code/Engine
    ${CMAKE_SOURCE_DIR}/Code/Editor
    ${CMAKE_SOURCE_DIR}/Code/Tools
    ${CMAKE_SOURCE_DIR}/Code/Extensions)

# --- Targets -----------------------------------------------------------------
# One dependency-ordered list for every platform. The order must stay a valid topological order,
# because some libs probe siblings at configure time with if(TARGET ...) (Graphics probes the RHI
# backends). Web builds the same list: the
# tooling group it cannot run is forced off for EMSCRIPTEN above, so it skips in place. The
# web-only samples and the web return() sit at the tail.
if(EMSCRIPTEN)
    # Emscripten defaults are sized for tiny demos: 64KB stack (engine code overflows it) and a
    # fixed 16MB heap. Use a 2MB stack and a growable heap; link-time options applied to every target below.
    add_link_options(-sSTACK_SIZE=2097152 -sALLOW_MEMORY_GROWTH=1)
endif()

add_subdirectory(ThirdParty)
add_subdirectory(Code/Foundation/Core)
add_subdirectory(Code/Foundation/Settings)  # typed/versioned settings store over Core (runtime and editor)
add_subdirectory(Code/Foundation/Profiler)  # CPU scope profiler over Core (instrumented across the engine)
add_subdirectory(Code/Foundation/Messaging) # native name-keyed deferred EventBus over Core (run-scope messaging)
add_subdirectory(Code/Foundation/Run)       # a run (one running game) and its manager, over Core and Messaging; runtime and scenes depend on it
add_subdirectory(Code/Foundation/Lod) # LOD selection math leaf over Core (mesh chains and terrain chunks share it)
add_subdirectory(Code/Foundation/I18N) # text in several languages: string tables, a resolver, plurals (leaf over Core)
add_subdirectory(Code/Foundation/Spline) # authorable 3D spline math leaf over Core (component/editor/consumers share it)
add_subdirectory(Code/Foundation/Net)       # real-time game networking: the wire format, INetTransport and a loopback simulator (over Core)
add_subdirectory(Code/Foundation/Net.Replication)  # StateReplication foundation: NetworkId and the reflection-driven field codec (over Net and Core)
add_subdirectory(Code/Foundation/Http)      # small HTTP/1.1 client, server and SSE over Core/System TCP (the MCP transport; WebSocket upgrades build on it)
add_subdirectory(Code/Foundation/Xml)       # DOM XML parser and writer over Core
    add_subdirectory(Code/Foundation/Xml.Serialization)
add_subdirectory(Code/Foundation/Json)      # hand-rolled JSON DOM (MCP wire and game data); RTTI-reflected for script
add_subdirectory(Code/Foundation/Profiler.Capture) # profile captures outside the process: Chrome trace export (over Profiler and Json)
add_subdirectory(Code/Foundation/Mcp)       # JSON-RPC 2.0 and the MCP protocol (tool/resource registry, stdio); over Json
add_subdirectory(Code/Foundation/Mcp.Http)  # MCP streamable-HTTP binding (bearer token and SSE channel); over Http and Mcp
add_subdirectory(Code/Foundation/Mcp.Reflection)  # MCP reflection tools (type_list/type_info) over Mcp and Core
add_subdirectory(Code/Foundation/VFS)       # byte access over Core stream primitives
    add_subdirectory(Code/Foundation/VFS.Pak)
add_subdirectory(Code/Engine/Engine.Project)   # project manifest/layout (runtime-side, editor-free)
add_subdirectory(Code/Foundation/Content)   # content database over Core and VFS
add_subdirectory(Code/Foundation/Resource)  # resource manager over content
add_subdirectory(Code/Foundation/RHI)   # depends only on Core (foundational sibling of Runtime)
# --- RHI backends (Null/Validation always; GPU backends by platform and availability) ---
add_subdirectory(Code/Foundation/RHI.Null)
add_subdirectory(Code/Foundation/RHI.Validation)
add_subdirectory(Code/Foundation/RHI.TestSupport) # offscreen-render test support (RHI and Core only)
find_package(Vulkan QUIET GLOBAL)
if(Vulkan_FOUND)
    add_subdirectory(Code/Foundation/RHI.Vulkan)
else()
    message(STATUS "Vulkan not found - skipping Foundation::RHI.Vulkan")
endif()
if(WIN32)
    add_subdirectory(Code/Foundation/RHI.DX12)
endif()
if(EMSCRIPTEN OR TARGET ThirdParty::WgpuHeaders)
    add_subdirectory(Code/Foundation/RHI.WebGPU)
else()
    message(STATUS "Skipping Foundation::RHI.WebGPU (no wgpu headers)")
endif()
add_subdirectory(Code/Foundation/RenderGraph)  # render graph over RHI
add_subdirectory(Code/Foundation/Shaders)
add_subdirectory(Code/Foundation/Shaders.System)
add_subdirectory(Code/Foundation/Shaders.Resource)
add_subdirectory(Code/Foundation/Image)
add_subdirectory(Code/Foundation/Image.DDS)  # DDS container and BCn decode (bcdec)
add_subdirectory(Code/Foundation/Image.IO)
add_subdirectory(Code/Foundation/RHI.Capture) # backbuffer to PNG and the --screenshot flags (over RHI and Image)
add_subdirectory(Code/Foundation/Image.Resource)
add_subdirectory(Code/Foundation/Model)
    add_subdirectory(Code/Foundation/Model.IO)
    add_subdirectory(Code/Foundation/Model.GLTF)
    add_subdirectory(Code/Foundation/Model.FBX)
add_subdirectory(Code/Foundation/Geometry)  # engine runtime mesh format (StaticMesh/SkinnedMesh and primitives)
add_subdirectory(Code/Foundation/Geometry.Resource)
add_subdirectory(Code/Foundation/Heightfield) # pure heightfield grid and sampling math; shared by render/physics/nav
add_subdirectory(Code/Foundation/Heightfield.Resource) # heightfield as a cooked, referenceable resource (Ref<Heightfield>)
add_subdirectory(Code/Foundation/Terrain) # terrain model over a heightfield: chunk grid, quadtree, LOD and splat (no RHI)
add_subdirectory(Code/Foundation/Terrain.Resource) # cooked terrain resource: heightfield, splatmap and layer refs (Ref<Terrain>)
add_subdirectory(Code/Foundation/Vegetation.Resource) # painted vegetation mask resource: density planes, brush cores, cooked source and factory
add_subdirectory(Code/Foundation/Vegetation) # terrain vegetation model: layer params, deterministic per-chunk scatter and fade (no RHI)
add_subdirectory(Code/Foundation/Animation) # skeletal-animation foundation (Core only)
add_subdirectory(Code/Foundation/PropertyAnimation) # reflected property-curve animation (Core only; not skeletal)
add_subdirectory(Code/Foundation/PropertyAnimation.Resource) # cooked property-clip resource and its loader
add_subdirectory(Code/Foundation/Animation.Resource)
add_subdirectory(Code/Foundation/Particles) # CPU particle runtime (Core only)
add_subdirectory(Code/Foundation/Particles.Resource)
add_subdirectory(Code/Foundation/Physics)   # Jolt-backed rigid-body core (world/bodies/queries/events; JPH never public)
add_subdirectory(Code/Foundation/Physics.Resource)  # cooked collision shapes and physical materials
add_subdirectory(Code/Foundation/Navigation) # Recast/Detour navmesh core (bake, query, crowd; rc*/dt* never public)
add_subdirectory(Code/Foundation/Navigation.Resource) # cooked navmesh-zone resource and factory
add_subdirectory(Code/Foundation/Audio)     # miniaudio-backed audio engine core (voices/buses/handles; ma_* never public)
add_subdirectory(Code/Foundation/Audio.Resource)    # cooked audio clips and factory
add_subdirectory(Code/Foundation/Scene)     # scene / ECS foundation (entities, transforms, components, SceneSubsystem)
add_subdirectory(Code/Engine/Engine.Scene)
add_subdirectory(Code/Foundation/Scene.Resource)
add_subdirectory(Code/Engine/Engine.Domain) # what one engine domain declares (DomainModule) and the composition built from a list of them
add_subdirectory(Code/Foundation/Render.Api) # light scene-rendering interface (ISceneRenderer), so tools drive rendering without linking the renderer
add_subdirectory(Code/Foundation/Render)    # the renderer: scene-to-render extraction and the GPU passes
add_subdirectory(Code/Engine/Engine.Render)
add_subdirectory(Code/Engine/Engine.Animation) # skeletal-animation scene integration (links RenderSubsystem; after Render)
add_subdirectory(Code/Engine/Engine.Model) # a cooked model put into a scene as entities (Sandbox, the model viewer)
add_subdirectory(Code/Engine/Engine.Particles) # particle scene/render integration (a renderer and a provider registered with Render; after Render)
add_subdirectory(Code/Engine/Engine.Physics) # physics scene integration (components, per-scene Jolt world, interpolation; after Render)
add_subdirectory(Code/Engine/Engine.Terrain) # terrain scene integration (TerrainComponent)
add_subdirectory(Code/Engine/Engine.Vegetation) # terrain vegetation: a per-layer component and the provider of per-chunk instanced sets (VegetationSubsystem)
add_subdirectory(Code/Engine/Engine.Spline) # authorable 3D spline scene component (data-only; math in Foundation::Spline)
add_subdirectory(Code/Engine/Engine.Navigation) # navmesh scene integration (zone/agent components and a per-scene Detour crowd)
add_subdirectory(Code/Engine/Engine.Audio)   # audio scene integration (components, per-scene voice group, listener sync; after Render)
add_subdirectory(Code/Foundation/Fonts)     # font types/interfaces over Core and Image
add_subdirectory(Code/Foundation/Fonts.Coverage)
add_subdirectory(Code/Foundation/Fonts.TrueType)
add_subdirectory(Code/Foundation/Fonts.HarfBuzz) # HarfBuzz behind ITextShaper (ligatures, joining, marks)
add_subdirectory(Code/Foundation/Fonts.Bidi) # the bidi algorithm (SheenBidi) behind IBidiResolver
add_subdirectory(Code/Foundation/Fonts.Coverage.Baker)
add_subdirectory(Code/Foundation/Fonts.DistanceField)
add_subdirectory(Code/Foundation/Fonts.Resource)
if(TARGET ThirdParty::MsdfgenCore)  # distance-field baker (bakes TTF faces at editor time); needs the vendored msdfgen
    add_subdirectory(Code/Foundation/Fonts.DistanceField.Baker)
endif()
add_subdirectory(Code/Foundation/Texture)   # texture descriptors, texture data and format helpers
add_subdirectory(Code/Foundation/Texture.Resource)
add_subdirectory(Code/Foundation/Materials) # data-driven material model (PipelineConfig and the bind-group-inferring MaterialSystem)
add_subdirectory(Code/Foundation/Materials.PipelineCache)
add_subdirectory(Code/Foundation/Materials.Resource)
add_subdirectory(Code/Foundation/Model.Resource) # cooked model manifest and runtime composite (runtime side; no tooling deps)
add_subdirectory(Code/Foundation/VG)        # 2D vector-graphics core over Core and Image
add_subdirectory(Code/Foundation/VG.SVG)
add_subdirectory(Code/Foundation/VG.Resource) # cooked vector images (validated SVG; renderer- and UI-free records)
add_subdirectory(Code/Foundation/VG.Renderer)
add_subdirectory(Code/Foundation/GUI)   # the GUI framework (derived from eepp)
add_subdirectory(Code/Foundation/Script)
# --- Script backends: independently switchable via OPTION_ENABLE_* (declared up top) -----------
# Both OPTION_ENABLE_ANGELSCRIPT/LUAU flags are defined in the option block above. Only AngelScript
# needs reconciling here: its vendored lib is conditional (it may not build on a target), so a
# requested but absent AngelScript is switched off. Luau is vendored unconditionally. Then enforce
# at least one backend and publish the OPTION_HAS_<backend> presence defines (C++ guards usage
# with #ifdef).
if(OPTION_ENABLE_ANGELSCRIPT AND NOT TARGET ThirdParty::AngelScript)
    message(STATUS "AngelScript backend requested but its vendored lib did not build - disabling OPTION_ENABLE_ANGELSCRIPT.")
    set(OPTION_ENABLE_ANGELSCRIPT OFF)
endif()
if(NOT OPTION_ENABLE_ANGELSCRIPT AND NOT OPTION_ENABLE_LUAU)
    message(FATAL_ERROR "All script backends are disabled; enable at least one of OPTION_ENABLE_ANGELSCRIPT / OPTION_ENABLE_LUAU.")
endif()
# Presence macros, defined only when enabled, so C++ guards read `#ifdef OPTION_HAS_ANGELSCRIPT`.
target_compile_definitions(policy INTERFACE
    $<$<BOOL:${OPTION_ENABLE_ANGELSCRIPT}>:OPTION_HAS_ANGELSCRIPT>
    $<$<BOOL:${OPTION_ENABLE_LUAU}>:OPTION_HAS_LUAU>)

# Target-or-empty aliases for the optional backend libs, so a consumer shared by both backends
# (composition roots, multi-backend test suites) lists ${SCRIPTLIB_*} in target_link_libraries and
# it resolves to the real target when the backend is on, or to nothing when it is off, with no
# if() at each site.
# (The per-backend subdirs are gated whole, so they keep their explicit names.)
set(SCRIPTLIB_ANGELSCRIPT "")
set(SCRIPTLIB_LUAU "")
set(SCRIPTLIB_ANGELSCRIPT_PIPELINE "")
set(SCRIPTLIB_LUAU_PIPELINE "")
if(OPTION_ENABLE_ANGELSCRIPT)
    set(SCRIPTLIB_ANGELSCRIPT Foundation::Script.AngelScript)
    set(SCRIPTLIB_ANGELSCRIPT_PIPELINE Pipeline::Script.AngelScript)
endif()
if(OPTION_ENABLE_LUAU)
    set(SCRIPTLIB_LUAU Foundation::Script.Luau)
    set(SCRIPTLIB_LUAU_PIPELINE Pipeline::Script.Luau)
endif()

if(OPTION_ENABLE_LUAU)
    add_subdirectory(Code/Foundation/Script.Luau)
endif()
if(OPTION_ENABLE_ANGELSCRIPT)
    add_subdirectory(Code/Foundation/Script.AngelScript)
endif()
add_subdirectory(Code/Foundation/Script.Resource)   # cooked script classes and factory (source and harvested metadata)
add_subdirectory(Code/Foundation/Script.Facades)    # curated behavior facades (Entity/Log/Time/Random), shared by the cook VM and the runtime
add_subdirectory(Code/Foundation/Mcp.Script)        # MCP scripting tools (script_api) over Mcp, Script and Script.Facades
add_subdirectory(Code/Foundation/Net.WebSocket)     # native WebSocket accept path (RFC 6455 over Http) and a hybrid UDP/WebSocket socket, so browser clients join native hosts
add_subdirectory(Code/Foundation/Net.Tls)           # TLS interfaces: a session as a byte engine, the backend interface, the system's root certificates
add_subdirectory(Code/Foundation/Net.Tls.MbedTLS)   # the Mbed TLS backend (the only module linking ThirdParty::MbedTLS)
add_subdirectory(Code/Foundation/Http.Client)       # the HTTP client interface and the native HTTP/1.1 client over plain and TLS streams
add_subdirectory(Code/Foundation/Net.Manager)       # per-instance networked endpoint (NetworkManager) and the Net script facade (over Net and Script)
add_subdirectory(Code/Foundation/Http.Script)       # the Http script facade over a run's HTTP client
add_subdirectory(Code/Engine/Engine.Net)     # once-per-context NetworkSubsystem (injects NetworkComponentManager into scenes)
add_subdirectory(Code/Foundation/Shell)     # raw platform/OS service (IShell: window/input/events) over Core; below Runtime
# --- Shell backends (Null always; browser shell on emscripten, SDL3 desktop otherwise) ---
add_subdirectory(Code/Foundation/Shell.Null)
if(EMSCRIPTEN)
    add_subdirectory(Code/Foundation/Shell.Web)
else()
    # SDL3 is resolved in ThirdParty (vendored and static by default; ENGINE_SDL_TARGET set there,
    # SDL3::SDL3-static or system SDL3::SDL3). Consumers link ${ENGINE_SDL_TARGET}.
    if(ENGINE_SDL_TARGET)
        add_subdirectory(Code/Foundation/Shell.Desktop)
    else()
        message(STATUS "SDL3 unavailable - skipping Foundation::Shell.Desktop")
    endif()
endif()
add_subdirectory(Code/Foundation/Input)     # action-mapping layer (InputMap, ActionRuntime, the asset and its cooked form) over Core and Shell
add_subdirectory(Code/Foundation/Input.Resource)
add_subdirectory(Code/Foundation/GUI.Shell)  # platform input bridge (Shell InputEvents -> GUI EventDispatcher)
add_subdirectory(Code/Foundation/GUI.Inspector)  # the GUI's inspector
add_subdirectory(Code/Foundation/GUI.Testing)  # test helpers for GUIs
add_subdirectory(Code/Foundation/GUI.VFS)   # VFS-backed IResourceProvider (background-image: url())
add_subdirectory(Code/Foundation/Graphics)  # GraphicsDevice/RenderWindow render host over RHI and Shell; below Runtime
add_subdirectory(Code/Foundation/Runtime)
add_subdirectory(Code/Foundation/Runtime.Client)
# The GUI's satellite libraries, over the graphics and runtime hosts.
add_subdirectory(Code/Foundation/GUI.Hosting)   # the host: windows, roots, renderers, input, offscreen pictures
add_subdirectory(Code/Foundation/GUI.Viewport)  # 3D content in a node, with its input gated
add_subdirectory(Code/Foundation/GUI.Docking)   # docking, floating panels as OS windows
add_subdirectory(Code/Foundation/GUI.Toolkit)   # editor widgets
add_subdirectory(Code/Foundation/GUI.Code)      # the code editor
add_subdirectory(Code/Foundation/GUI.Gamekit)   # a game's screens
add_subdirectory(Code/Foundation/Profiler.GUI)  # the profiler's views on the GUI (the editor's Profiler page)
add_subdirectory(Code/Foundation/GUI.Resources) # cooked documents and themes
if(OPTION_BUILD_TOOLS)
    add_subdirectory(Code/Pipeline/GUI.Pipeline) # cooking the GUI's documents and themes (tools only)
endif()
add_subdirectory(Code/Engine/Engine.GameInstance)
# DefaultApplication, the IApplication that registers the standard engine subsystems; it links the
# subsystem libraries above, Net.Manager among them.
add_subdirectory(Code/Engine/Engine.DefaultApp)
# --- Runtime runner backend (browser rAF loop vs blocking desktop loop) ---
if(EMSCRIPTEN)
    add_subdirectory(Code/Foundation/Runtime.Web)
else()
    add_subdirectory(Code/Foundation/Runtime.Desktop)
endif()
add_subdirectory(Code/Engine/Engine.Input)  # input runtime hookup (the tick and the device provider) over Runtime
add_subdirectory(Code/Engine/Engine.Script) # entity behaviors (ScriptComponent and the run's single gameplay script context) over Runtime
add_subdirectory(Code/Engine/Engine.Integration) # cross-subsystem composition helpers (the bridge from physics contacts to scripts) over Physics and Script
add_subdirectory(Code/Engine/Engine.GUI) # the engine's GUI on foundation.gui
add_subdirectory(Code/Engine/Engine.Composition) # the composition root: the one list of domains and the facets built from it on demand (scene, reflection, resource types, factories, facades)
if(OPTION_EXTENSION_IMGUI)
    add_subdirectory(Code/Extensions/Imgui) # Dear ImGui debug UI (RHI renderer and subsystem) over Runtime
endif()
add_subdirectory(Code/Engine/Engine.Player)        # Generic game runner


# --- Authoring tooling: the editor libraries and the CLI executables that assemble them.
# Gated by OPTION_BUILD_TOOLS (forced off for web builds) so the whole editor and cook surface
# drops cleanly. ---
if(OPTION_BUILD_TOOLS)
    add_subdirectory(Code/Pipeline/Pipeline.Core)  # asset-pipeline authoring base: Asset and builders (tooling)
    add_subdirectory(Code/Pipeline/Pipeline.Importer) # file-import framework (IFileImporter and its registry, headless)
    add_subdirectory(Code/Pipeline/Pipeline.Cook)  # incremental cook driver and pipeline DB (tooling, headless)
    add_subdirectory(Code/Editor/Editor.Project) # the headless project half: project/cook/export/jobs/log (tooling; hosts without an editor stand on it)
    add_subdirectory(Code/Editor/Editor.Core) # the editor-facing half: context/commands/pages/selection/actions over Editor.Project (tooling)
    add_subdirectory(Code/Editor/Editor.Mcp) # the MCP tools over a headless project (project_*, asset_uses, scene_validate, ...)
    add_subdirectory(Code/Editor/Editor.Preview.Viewport) # the preview viewport (no editor core: tools link it too)
    add_subdirectory(Code/Editor/Editor.Preview) # the thumbnail stage, the preview viewport re-exported
    add_subdirectory(Code/Editor/Editor.App)
    add_subdirectory(Code/Pipeline/Physics.Pipeline)    # collision/material assets and builders (tooling)
    add_subdirectory(Code/Pipeline/Render.Pipeline)     # render profile assets and builders (tooling)
    add_subdirectory(Code/Pipeline/Navigation.Pipeline) # navmesh-zone asset and builder (tooling)
    add_subdirectory(Code/Pipeline/Audio.Pipeline)      # audio clip asset/builder/importer (tooling)
    add_subdirectory(Code/Pipeline/VG.Pipeline)     # vector image asset, validating builder and .svg importer (tooling)
    add_subdirectory(Code/Pipeline/ModelImporter) # top-level model import: model IR -> cooked geometry/material/texture/animation resources
    add_subdirectory(Code/Pipeline/Script.Pipeline)  # script class asset/builder/importer (cook; backend-neutral)
    # Per-backend cooks, each gated on its OPTION_ENABLE_* flag.
    if(OPTION_ENABLE_ANGELSCRIPT)
        add_subdirectory(Code/Pipeline/Script.AngelScript.Pipeline)  # AngelScript cook service
    endif()
    if(OPTION_ENABLE_LUAU)
        add_subdirectory(Code/Pipeline/Script.Luau.Pipeline)  # Luau cook service
    endif()
    add_subdirectory(Code/Editor/Editor.Spline) # interactive spline viewport tool (editor exe only)
    add_subdirectory(Code/Editor/Editor.Terrain) # the terrain's scene-view brushes and their panels, the terrain page
    add_subdirectory(Code/Editor/Editor.Vegetation) # the vegetation's scene-view brushes and their panels, the vegetation mask page
    add_subdirectory(Code/Editor/Editor.Particles) # the particle effect page
    add_subdirectory(Code/Editor/Editor.Animation) # the animation pages: clip, skeleton, graph
    add_subdirectory(Code/Editor/Editor.PropertyAnimation) # property animation in the scene: its panel under the viewport
    add_subdirectory(Code/Editor/Editor.ViewportTools) # viewport tool-mode framework (IViewportTool, its manager, the provider registry)
    add_subdirectory(Code/Editor/Editor.Camera) # shared free-fly viewport camera (lean, foundation-only) for all editor pages
    add_subdirectory(Code/Editor/Editor.Navigation) # navmesh bake tooling (collects geometry, bakes it with Recast, writes the zone asset)

    add_subdirectory(Code/Pipeline/Animation.Pipeline)
    add_subdirectory(Code/Pipeline/PropertyAnimation.Pipeline)
    add_subdirectory(Code/Pipeline/Geometry.Pipeline)
    add_subdirectory(Code/Pipeline/Heightfield.Pipeline)
    add_subdirectory(Code/Pipeline/Terrain.Pipeline)
    add_subdirectory(Code/Pipeline/Vegetation.Pipeline)
    add_subdirectory(Code/Pipeline/Image.Pipeline)
    add_subdirectory(Code/Pipeline/Input.Pipeline)
    add_subdirectory(Code/Pipeline/Materials.Pipeline)
    add_subdirectory(Code/Pipeline/Particles.Pipeline)
    add_subdirectory(Code/Pipeline/Scene.Pipeline) # scene and prefab creators (links Engine::Render for the sun)
    add_subdirectory(Code/Pipeline/Shaders.Pipeline)
    add_subdirectory(Code/Pipeline/Texture.Compression) # BC encoder and its policy table
    add_subdirectory(Code/Pipeline/Texture.Pipeline)
    if(TARGET Foundation::Fonts.DistanceField.Baker)
        add_subdirectory(Code/Pipeline/Fonts.Pipeline)
    endif()

    # The pipeline's composition root: links every pipeline module above and exposes the single
    # RegisterAllBuilders/RegisterAllImporters/RegisterPipelineTypes set every host consumes. It
    # links all the pipeline modules, the fonts block included, so it comes after them.
    add_subdirectory(Code/Pipeline/Pipeline.Registration)

    add_subdirectory(Code/Editor/Editor.Scene) # the scene page: viewport, hierarchy, inspector, tools
    add_subdirectory(Code/Editor/Editor.Play) # play in editor: the Game tab
    add_subdirectory(Code/Editor/Editor.Generic) # the fallback asset page
    add_subdirectory(Code/Editor/Editor.Image) # the image pages
    add_subdirectory(Code/Editor/Editor.Heightfield)
    add_subdirectory(Code/Editor/Editor.Texture)
    if(TARGET Pipeline::Fonts)
        add_subdirectory(Code/Editor/Editor.Fonts)
        add_subdirectory(Code/Editor/Editor.Physics) # the collision shape page
    endif()
    add_subdirectory(Code/Editor/Editor.Geometry) # the mesh viewer
    add_subdirectory(Code/Editor/Editor.Materials) # the material page
    add_subdirectory(Code/Editor/Editor.Render) # the settings profile page
    add_subdirectory(Code/Editor/Editor.Audio) # the audio pages
    add_subdirectory(Code/Editor/Editor.Input) # the input map page
    add_subdirectory(Code/Editor/Editor.Script) # the script page
    add_subdirectory(Code/Editor/Editor.Shaders) # the shader page
    add_subdirectory(Code/Editor/Editor.GUI) # the GUI document and sheet pages
    add_subdirectory(Code/Tools/Tools.Editor)        # the editor executable

    add_subdirectory(Code/Tools/Tools.Cook)          # Tools.Cook CLI cooker (headless)
    add_subdirectory(Code/Tools/Tools.Mcp)          # headless MCP stdio server (agent access)
    add_subdirectory(Code/Tools/Tools.Export)        # Export dist packager
    add_subdirectory(Code/Tools/Tools.ShaderPack) # ShaderPack: cook shaders.dpak (WGSL for web)
    add_subdirectory(Code/Tools/ModelViewer)       # the model viewer's headless half (open, cache, numbers)
    add_subdirectory(Code/Tools/Tools.ModelViewer) # the standalone model viewer
endif()

# --- Tests: every module's sibling doctest suite (guarded by its module target so the
# web subset and platform-gated backends skip when the module was not built) ---
if(OPTION_BUILD_TESTS)
    util_test_suite(Code/Pipeline/Animation.Pipeline.Tests              Pipeline::Animation)
    util_test_suite(Code/Pipeline/PropertyAnimation.Pipeline.Tests       Pipeline::PropertyAnimation)
    util_test_suite(Code/Pipeline/Audio.Pipeline.Tests                  Pipeline::Audio)
    util_test_suite(Code/Engine/Engine.DefaultApp.Tests             Engine::DefaultApp)
    util_test_suite(Code/Editor/Editor.App.Tests                    Editor::App)
    util_test_suite(Code/Editor/Editor.Animation.Tests              Editor::Animation)
    util_test_suite(Code/Editor/Editor.Materials.Tests              Editor::Materials)
    util_test_suite(Code/Editor/Editor.Geometry.Tests               Editor::Geometry)
    util_test_suite(Code/Editor/Editor.Particles.Tests              Editor::Particles)
    util_test_suite(Code/Editor/Editor.Render.Tests                 Editor::Render)
    util_test_suite(Code/Editor/Editor.Play.Tests                   Editor::Play)
    util_test_suite(Code/Editor/Editor.Audio.Tests                  Editor::Audio)
    util_test_suite(Code/Pipeline/Pipeline.Cook.Tests                   Pipeline::Cook)
    util_test_suite(Code/Pipeline/Pipeline.Importer.Tests               Pipeline::Importer)
    util_test_suite(Code/Pipeline/Pipeline.Registration.Tests           Pipeline::Registration)
    util_test_suite(Code/Editor/Editor.Core.Tests                   Editor::Core)
    util_test_suite(Code/Editor/Editor.Generic.Tests                Editor::Generic)
    util_test_suite(Code/Editor/Editor.Image.Tests                  Editor::Image)
    util_test_suite(Code/Editor/Editor.Heightfield.Tests            Editor::Heightfield)
    util_test_suite(Code/Editor/Editor.Fonts.Tests                  Editor::Fonts)
    util_test_suite(Code/Editor/Editor.Terrain.Tests                Editor::Terrain)
    util_test_suite(Code/Editor/Editor.Vegetation.Tests             Editor::Vegetation)
    util_test_suite(Code/Editor/Editor.Scene.Tests                  Editor::Scene)
    util_test_suite(Code/Editor/Editor.Navigation.Tests             Editor::Navigation)
    util_test_suite(Code/Editor/Editor.ViewportTools.Tests          Editor::ViewportTools)
    util_test_suite(Code/Editor/Editor.Camera.Tests                 Editor::Camera)
    util_test_suite(Code/Editor/Editor.PropertyAnimation.Tests      Editor::PropertyAnimation)
    util_test_suite(Code/Editor/Editor.Script.Tests                 Editor::Script)
    util_test_suite(Code/Editor/Editor.Shaders.Tests                Editor::Shaders)
    util_test_suite(Code/Editor/Editor.Tests                        Pipeline::Core)
    util_test_suite(Code/Editor/Editor.Texture.Tests                Editor::Texture)
    util_test_suite(Code/Editor/Editor.GUI.Tests                    Editor::GUI)
    util_test_suite(Code/Editor/Editor.Input.Tests                  Editor::Input)
    util_test_suite(Code/Editor/Editor.Physics.Tests                Editor::Physics)
    util_test_suite(Code/Editor/Editor.Spline.Tests                 Editor::Spline)
    util_test_suite(Code/Pipeline/Geometry.Pipeline.Tests               Pipeline::Geometry)
    util_test_suite(Code/Pipeline/Heightfield.Pipeline.Tests            Pipeline::Heightfield)
    util_test_suite(Code/Pipeline/Terrain.Pipeline.Tests                Pipeline::Terrain)
    util_test_suite(Code/Pipeline/Vegetation.Pipeline.Tests             Pipeline::Vegetation)
    util_test_suite(Code/Pipeline/Image.Pipeline.Tests                  Pipeline::Image)
    util_test_suite(Code/Pipeline/Input.Pipeline.Tests                  Pipeline::Input)
    util_test_suite(Code/Pipeline/Materials.Pipeline.Tests              Pipeline::Materials)
    util_test_suite(Code/Pipeline/ModelImporter.Tests                 Pipeline::ModelImporter)
    util_test_suite(Code/Pipeline/Particles.Pipeline.Tests              Pipeline::Particles)
    util_test_suite(Code/Pipeline/Physics.Pipeline.Tests                Pipeline::Physics)
    util_test_suite(Code/Pipeline/Render.Pipeline.Tests                 Pipeline::Render)
    util_test_suite(Code/Pipeline/Navigation.Pipeline.Tests             Pipeline::Navigation)
    util_test_suite(Code/Pipeline/Script.AngelScript.Pipeline.Tests     Pipeline::Script.AngelScript)
    util_test_suite(Code/Pipeline/Scene.Pipeline.Tests                  Pipeline::Scene)
    util_test_suite(Code/Pipeline/Script.Pipeline.Tests                 Pipeline::Script)
    util_test_suite(Code/Pipeline/Script.Luau.Pipeline.Tests            Pipeline::Script.Luau)
    util_test_suite(Code/Pipeline/Shaders.Pipeline.Tests                Pipeline::Shaders)
    util_test_suite(Code/Pipeline/Texture.Compression.Tests            Pipeline::Texture.Compression)
    util_test_suite(Code/Pipeline/Texture.Pipeline.Tests                Pipeline::Texture)
    util_test_suite(Code/Pipeline/Fonts.Pipeline.Tests                  Pipeline::Fonts)
    util_test_suite(Code/Pipeline/VG.Pipeline.Tests                     Pipeline::VG)
    util_test_suite(Code/Engine/Engine.Animation.Tests              Engine::Animation)
    util_test_suite(Code/Tools/ModelViewer.Tests                    Tools::ModelViewer)
    util_test_suite(Code/Engine/Engine.Audio.Tests                  Engine::Audio)
    # Monolithic: its one test file exercises both backends, so it builds only when both
    # are on (a subset config drops it; the default all-on CI config runs it).
    if(OPTION_ENABLE_ANGELSCRIPT AND OPTION_ENABLE_LUAU)
        util_test_suite(Code/Engine/Engine.GameInstance.Tests Engine::GameInstance)
    endif()
    util_test_suite(Code/Engine/Engine.Composition.Tests Engine::Composition)
    util_test_suite(Code/Engine/Engine.Integration.Tests            Engine::Integration)
    util_test_suite(Code/Engine/Engine.Net.Tests                    Engine::Net)
    util_test_suite(Code/Engine/Engine.Physics.Tests                Engine::Physics)
    util_test_suite(Code/Engine/Engine.Navigation.Tests             Engine::Navigation)
    util_test_suite(Code/Engine/Engine.Particles.Tests              Engine::Particles)
    util_test_suite(Code/Engine/Engine.Terrain.Tests                Engine::Terrain)
    util_test_suite(Code/Engine/Engine.Vegetation.Tests             Engine::Vegetation)
    util_test_suite(Code/Engine/Engine.Input.Tests                  Engine::Input)
    util_test_suite(Code/Engine/Engine.Render.Tests                 Engine::Render)
    util_test_suite(Code/Engine/Engine.Scene.Tests                  Engine::Scene)
    # Monolithic: one test file exercises both backends, so it builds only when both are on.
    if(OPTION_ENABLE_ANGELSCRIPT AND OPTION_ENABLE_LUAU)
        util_test_suite(Code/Engine/Engine.Script.Tests Engine::Script)
    endif()
    util_test_suite(Code/Foundation/GUI.Shell.Tests               Foundation::GUI.Shell)
    util_test_suite(Code/Foundation/GUI.Inspector.Tests           Foundation::GUI.Inspector)
    util_test_suite(Code/Foundation/GUI.Testing.Tests             Foundation::GUI.Testing)
    util_test_suite(Code/Foundation/GUI.Fuzz.Tests                Foundation::GUI.Testing)
    util_test_suite(Code/Foundation/GUI.Tests                     Foundation::GUI)
    util_test_suite(Code/Foundation/GUI.VFS.Tests                 Foundation::GUI.VFS)
    util_test_suite(Code/Foundation/GUI.Hosting.Tests             Foundation::GUI.Hosting)
    util_test_suite(Code/Foundation/GUI.Viewport.Tests            Foundation::GUI.Viewport)
    util_test_suite(Code/Foundation/GUI.Docking.Tests             Foundation::GUI.Docking)
    util_test_suite(Code/Foundation/GUI.Toolkit.Tests             Foundation::GUI.Toolkit)
    util_test_suite(Code/Foundation/GUI.Code.Tests                Foundation::GUI.Code)
    util_test_suite(Code/Foundation/GUI.Gamekit.Tests             Foundation::GUI.Gamekit)
    util_test_suite(Code/Foundation/Profiler.GUI.Tests            Foundation::Profiler.GUI)
    util_test_suite(Code/Foundation/GUI.Resources.Tests           Foundation::GUI.Resources)
    if(OPTION_BUILD_TOOLS)
        util_test_suite(Code/Pipeline/GUI.Pipeline.Tests        Pipeline::GUI)
    endif()
    util_test_suite(Code/Engine/Engine.GUI.Tests                    Engine::GUI)
    util_test_suite(Code/Foundation/Animation.Resource.Tests        Foundation::Animation.Resource)
    util_test_suite(Code/Foundation/Animation.Tests                 Foundation::Animation)
    util_test_suite(Code/Foundation/PropertyAnimation.Tests         Foundation::PropertyAnimation)
    util_test_suite(Code/Foundation/PropertyAnimation.Resource.Tests Foundation::PropertyAnimation.Resource)
    util_test_suite(Code/Foundation/Audio.Tests                     Foundation::Audio)
    util_test_suite(Code/Foundation/Content.Tests                   Foundation::Content)
    util_test_suite(Code/Foundation/Core.Tests                      Foundation::Core)
    util_test_suite(Code/Foundation/Fonts.Coverage.Tests               Foundation::Fonts.Coverage)
    util_test_suite(Code/Foundation/Fonts.DistanceField.Baker.Tests Foundation::Fonts.DistanceField.Baker)
    util_test_suite(Code/Foundation/Fonts.DistanceField.Tests       Foundation::Fonts.DistanceField)
    util_test_suite(Code/Foundation/Fonts.Coverage.Baker.Tests            Foundation::Fonts.Coverage.Baker)
    util_test_suite(Code/Foundation/Fonts.TrueType.Tests                 Foundation::Fonts.TrueType)
    if(TARGET Foundation::Fonts.DistanceField.Baker) # its bake test needs the baker, which the web build lacks
        util_test_suite(Code/Foundation/Fonts.HarfBuzz.Tests             Foundation::Fonts.HarfBuzz)
    endif()
    util_test_suite(Code/Foundation/Fonts.Tests                     Foundation::Fonts)
    util_test_suite(Code/Foundation/Geometry.Resource.Tests         Foundation::Geometry.Resource)
    util_test_suite(Code/Foundation/Geometry.Tests                  Foundation::Geometry)
    util_test_suite(Code/Foundation/Heightfield.Tests               Foundation::Heightfield)
    util_test_suite(Code/Foundation/Heightfield.Resource.Tests      Foundation::Heightfield.Resource)
    util_test_suite(Code/Foundation/Terrain.Tests                   Foundation::Terrain)
    util_test_suite(Code/Foundation/Terrain.Resource.Tests          Foundation::Terrain.Resource)
    util_test_suite(Code/Foundation/Vegetation.Tests                Foundation::Vegetation)
    util_test_suite(Code/Foundation/Vegetation.Resource.Tests       Foundation::Vegetation.Resource)
    util_test_suite(Code/Foundation/Graphics.Tests                  Foundation::Graphics)
    util_test_suite(Code/Foundation/Image.Tests                     Foundation::Image)
    util_test_suite(Code/Foundation/Image.DDS.Tests                 Foundation::Image.DDS)
    util_test_suite(Code/Foundation/Input.Tests                     Foundation::Input)
    util_test_suite(Code/Foundation/Materials.PipelineCache.Tests   Foundation::Materials.PipelineCache)
    util_test_suite(Code/Foundation/Materials.Resource.Tests        Foundation::Materials.Resource)
    util_test_suite(Code/Foundation/Materials.Tests                 Foundation::Materials)
    util_test_suite(Code/Foundation/Model.Tests                     Foundation::Model)
    util_test_suite(Code/Foundation/Model.GLTF.Tests                Foundation::Model.GLTF)
    util_test_suite(Code/Foundation/Model.FBX.Tests                 Foundation::Model.FBX)
    util_test_suite(Code/Foundation/Net.Manager.Tests Foundation::Net.Manager)  # the suite's CMakeLists gates its AngelScript facade test file
    util_test_suite(Code/Foundation/Net.Replication.Tests           Foundation::Net.Replication)
    util_test_suite(Code/Foundation/Net.Tests                       Foundation::Net)
    util_test_suite(Code/Foundation/Net.WebSocket.Tests             Foundation::Net.WebSocket)
    util_test_suite(Code/Foundation/Net.Tls.MbedTLS.Tests           Foundation::Net.Tls.MbedTLS)
    util_test_suite(Code/Foundation/Http.Client.Tests               Foundation::Http.Client)
    util_test_suite(Code/Foundation/Http.Tests                      Foundation::Http)
    util_test_suite(Code/Foundation/Particles.Resource.Tests        Foundation::Particles.Resource)
    util_test_suite(Code/Foundation/Particles.Tests                 Foundation::Particles)
    util_test_suite(Code/Foundation/Physics.Tests                   Foundation::Physics)
    util_test_suite(Code/Foundation/Navigation.Tests                Foundation::Navigation)
    util_test_suite(Code/Foundation/Profiler.Tests                  Foundation::Profiler)
    util_test_suite(Code/Foundation/Profiler.Capture.Tests          Foundation::Profiler.Capture)
    util_test_suite(Code/Foundation/RHI.Null.Tests                  Foundation::RHI.Null)
    util_test_suite(Code/Foundation/RHI.Validation.Tests            Foundation::RHI.Validation)
    util_test_suite(Code/Foundation/RHI.Capture.Tests               Foundation::RHI.Capture)
    if(TARGET Foundation::RHI.Vulkan)
        util_test_suite(Code/Foundation/RHI.Vulkan.Tests            Foundation::RHI.Vulkan)
    endif()
    if(NOT EMSCRIPTEN)  # spins up a real device; no headless web path yet
        util_test_suite(Code/Foundation/RHI.WebGPU.Tests Foundation::RHI.WebGPU)
        if(TARGET Foundation::RHI.Vulkan AND TARGET Foundation::RHI.WebGPU)
            # Cross-backend pixel probes (need Vulkan and WebGPU compiled; DX12 added on top where it
            # exists; GPUs checked at runtime).
            add_subdirectory(Code/Foundation/Render.Backend.Tests)
            add_subdirectory(Code/Foundation/VG.Backend.Tests)
            add_subdirectory(Code/Engine/Engine.Terrain.Backend.Tests)
            add_subdirectory(Code/Engine/Engine.Vegetation.Backend.Tests)
        endif()
    endif()
    util_test_suite(Code/Foundation/Render.Tests                    Foundation::Render)
    util_test_suite(Code/Foundation/RenderGraph.Tests               Foundation::RenderGraph)
    util_test_suite(Code/Foundation/Resource.Tests                  Foundation::Resource)
    util_test_suite(Code/Foundation/Runtime.Client.Tests            Foundation::Runtime.Client)
    util_test_suite(Code/Foundation/Runtime.Tests                   Foundation::Runtime)
    util_test_suite(Code/Foundation/Scene.Resource.Tests            Foundation::Scene.Resource)
    util_test_suite(Code/Foundation/Messaging.Tests                 Foundation::Messaging)
    util_test_suite(Code/Foundation/Run.Tests                       Foundation::Run)
    util_test_suite(Code/Foundation/Lod.Tests                       Foundation::Lod)
    util_test_suite(Code/Foundation/I18N.Tests                      Foundation::I18N)
    util_test_suite(Code/Foundation/Spline.Tests                    Foundation::Spline)
    util_test_suite(Code/Engine/Engine.Spline.Tests                 Engine::Spline)
    util_test_suite(Code/Foundation/Scene.Tests                     Foundation::Scene)
    util_test_suite(Code/Foundation/Script.AngelScript.Tests        Foundation::Script.AngelScript)
    util_test_suite(Code/Foundation/Script.Resource.Tests           Foundation::Script.Resource)
    util_test_suite(Code/Foundation/Script.Tests                    Foundation::Script)
    util_test_suite(Code/Foundation/Script.Luau.Tests               Foundation::Script.Luau)
    util_test_suite(Code/Foundation/Settings.Tests                  Foundation::Settings)
    util_test_suite(Code/Foundation/Shaders.Resource.Tests          Foundation::Shaders.Resource)
    util_test_suite(Code/Foundation/Shaders.System.Tests            Foundation::Shaders.System)
    util_test_suite(Code/Foundation/Shaders.Tests                   Foundation::Shaders)
    util_test_suite(Code/Foundation/Shell.Desktop.Tests             Foundation::Shell.Desktop)
    util_test_suite(Code/Foundation/Shell.Null.Tests                Foundation::Shell.Null)
    util_test_suite(Code/Foundation/Texture.Resource.Tests          Foundation::Texture.Resource)
    util_test_suite(Code/Foundation/Fonts.Resource.Tests            Foundation::Fonts.Resource)
    util_test_suite(Code/Foundation/Texture.Tests                   Foundation::Texture)
    util_test_suite(Code/Foundation/VFS.Pak.Tests                   Foundation::VFS.Pak)
    util_test_suite(Code/Foundation/VFS.Tests                       Foundation::VFS)
    util_test_suite(Code/Foundation/VG.Renderer.Tests               Foundation::VG.Renderer)
    util_test_suite(Code/Foundation/VG.SVG.Tests                    Foundation::VG.SVG)
    util_test_suite(Code/Foundation/VG.Resource.Tests               Foundation::VG.Resource)
    util_test_suite(Code/Foundation/VG.Tests                        Foundation::VG)
    util_test_suite(Code/Foundation/Xml.Serialization.Tests         Foundation::Xml.Serialization)
    util_test_suite(Code/Foundation/Xml.Tests                       Foundation::Xml)
    util_test_suite(Code/Foundation/Json.Tests                     Foundation::Json)
    util_test_suite(Code/Foundation/Mcp.Tests                      Foundation::Mcp)
    util_test_suite(Code/Foundation/Mcp.Reflection.Tests           Foundation::Mcp.Reflection)
    util_test_suite(Code/Foundation/Mcp.Script.Tests               Foundation::Mcp.Script)
    util_test_suite(Code/Foundation/Mcp.Http.Tests                 Foundation::Mcp.Http)

    # Integration: flow suites that cross collections. The Integration. prefix lets a fast loop run
    # the module suites alone; a full run before review or a push includes these.
    # JSON from scripts: the test file runs on AngelScript and Luau, so build it when either is on.
    if(OPTION_ENABLE_ANGELSCRIPT OR OPTION_ENABLE_LUAU)
        util_test_suite(Code/Integration/Integration.Script            Foundation::Script)
    endif()
    # Subsystem script facades (Net.*, Audio.*) on AngelScript and Luau, kept here so the subsystem
    # unit suites stay backend-neutral. Each backend leg is #ifdef-guarded; build if either is on.
    if(OPTION_ENABLE_ANGELSCRIPT OR OPTION_ENABLE_LUAU)
        util_test_suite(Code/Integration/Integration.ScriptFacades     Engine::Audio)
    endif()
    # The GUI (foundation.gui) and its satellites from scripts, the engine's GUI among them.
    if(OPTION_ENABLE_ANGELSCRIPT OR OPTION_ENABLE_LUAU)
        util_test_suite(Code/Integration/Integration.GuiScript         Foundation::GUI.Gamekit)
    endif()
    util_test_suite(Code/Integration/Integration.Mcp                Editor::Mcp)  # golden flow: create, open and inspect a project
    # Engine composition against pipeline registration: every factory description's cooked form has a builder.
    util_test_suite(Code/Integration/Integration.Composition        Pipeline::Registration)
    # Animation and physics in a running scene: root motion walking a character, which still collides.
    util_test_suite(Code/Integration/Integration.Runtime            Engine::Animation)
    # The BC texture encoder against the renderer: upload and sample block-compressed textures on real GPUs.
    util_test_suite(Code/Integration/Integration.TextureCompression Pipeline::Texture.Compression)
    # A surface shader in the renderer: the built-in PBR as a surface shader draws as forward does, on real GPUs.
    util_test_suite(Code/Integration/Integration.Shaders            Pipeline::Shaders)
    # Every engine shader block against its C++ mirror, member by member, from the SPIR-V DXC compiles.
    util_test_suite(Code/Integration/Integration.ShaderBlocks       Foundation::Render)
endif()

# Web-only samples, then stop: the rest of Code/Samples is desktop-only. Honor
# OPTION_BUILD_SAMPLES (both wasm presets default it off): a samples-off wasm build must
# neither build the samples nor inherit WebScene's need for the native shader-cook tool.
if(EMSCRIPTEN)
    if(OPTION_BUILD_SAMPLES)
        add_subdirectory(Code/Samples/WebTriangle)  # WebShell, the web runner and the WebGPU factory; builds WebTriangle.html
        add_subdirectory(Code/Samples/WebScene)     # DefaultApplication and a lit PBR scene; builds WebScene.html
        if(OPTION_EXTENSION_IMGUI)
            add_subdirectory(Code/Samples/TerrainPlayground) # a heightfield built in code, drawn by engine.terrain; builds TerrainPlayground.html
        endif()
    endif()
    # Game native code: the wasm build hosts the game too, and the web shipping player
    # compiles the module in (browsers have no dlopen). Mirrors the non-Emscripten block
    # at the end of the file, which this branch return()s before reaching.
    if(ENGINE_GAME_NATIVE_DIR)
        add_subdirectory(${ENGINE_GAME_NATIVE_DIR} game-native)
    endif()
    return()
endif()

if(OPTION_BUILD_SAMPLES)
    add_subdirectory(Code/Samples)
endif()

# The old UI framework (foundation.ui) and its sandbox, on their way out:
# nothing outside Code/Deprecated links them, and they build only when asked for.
if(OPTION_BUILD_DEPRECATED)
    add_subdirectory(Code/Deprecated/UI)
endif()

# Game native code: the game's native directory joins the build after every engine
# target exists, so it can link any of them.
if(ENGINE_GAME_NATIVE_DIR)
    add_subdirectory(${ENGINE_GAME_NATIVE_DIR} game-native)
endif()
