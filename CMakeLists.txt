# Game engine root build
#
# C++23, C++ modules. Requires CMake >= 3.28 and a modules-capable toolchain
# (Clang 17+, GCC 14+, or MSVC 19.36+) with the Ninja generator.

cmake_minimum_required(VERSION 3.28)

project(GameEngine
    VERSION 0.1.0
    DESCRIPTION "Game engine"
    LANGUAGES CXX ASM)

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

# Build the engine's library targets as SHARED (.so/.dll) instead of STATIC.
# Every first-party library goes through util_add_engine_library, which reads
# this; the deliberate option (BUILD_SHARED_LIBS would silently no-op against
# hardcoded types). Design + hazard inventory: Documentation/Specs/shared-libraries.md.
option(ENGINE_SHARED_LIBS "Build engine libraries as shared libraries" OFF)

# A game project's native directory, wired into THIS build as a subdirectory
# (game-native-code.md N3): the game's module builds like any engine library (full
# C++20-module/BMI access, util_add_engine_library, both lanes - the dev .so and the
# ship static lib come from the same source), and Engine.GamePlayer (the ship player
# with the game statically linked) materializes when ENGINE_GAME_NATIVE_TARGET names
# the game's target. The exporter drives these; setting them by hand works too.
set(ENGINE_GAME_NATIVE_DIR "" CACHE PATH "Game project's native source directory (adds it to this build)")
set(ENGINE_GAME_NATIVE_TARGET "" CACHE STRING "The game native target name (enables Engine.GamePlayer)")
if(ENGINE_SHARED_LIBS AND EMSCRIPTEN)
    # Web is a single self-contained binary by design: browsers have no runtime
    # plugin loading, Emscripten dynamic linking forfeits whole-program optimization
    # and bloats the main module, and web extensibility flows through scripts.
    message(FATAL_ERROR "ENGINE_SHARED_LIBS is not supported for Emscripten - the web "
                        "target is always a static single binary (shared-libraries.md).")
endif()
if(ENGINE_SHARED_LIBS)
    # Binaries and libraries land together in Bin/<cfg>/<plat>; $ORIGIN makes the
    # loader find sibling .so files without LD_LIBRARY_PATH (the DXC/wgpu sidecar
    # precedent, generalized).
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
# C++20-module BMIs stay in the build tree - only deliverables move. The compiler
# suffix keeps concurrent toolchains (e.g. clang + gcc, both Debug) from clobbering
# each other. $<CONFIG> resolves for single- AND multi-config generators, and stops
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

# Sanitizer builds get their OWN output dir: build/asan sharing Bin with build/clang once
# clobbered the regular libs with sanitized ones (and purging the shared dir cost a user
# project that lived beside the binaries). Set BUILDSYSTEM_OUTPUT_SUFFIX (e.g. "-ASAN") in
# sanitizer configure runs.
set(BUILDSYSTEM_OUTPUT_SUFFIX "" CACHE STRING "Suffix for the Bin output dir (e.g. -ASAN)")
# Shared builds get their own dir the same way (the -ASAN precedent): build/clang and
# build/clang-shared sharing one Bin overwrote each other's binaries. An explicit
# BUILDSYSTEM_OUTPUT_SUFFIX still wins over the default.
if(ENGINE_SHARED_LIBS AND BUILDSYSTEM_OUTPUT_SUFFIX STREQUAL "")
    set(BUILDSYSTEM_OUTPUT_SUFFIX "-Shared")
endif()
# Sanitizer builds too - STRUCTURALLY, not by remembering to pass the suffix: an ASAN
# configure without it once clobbered the plain build's binaries AND archives in the
# shared Bin (mtime-newer outputs even dodge ninja's relink).
if(BUILDSYSTEM_OUTPUT_SUFFIX STREQUAL "" AND CMAKE_CXX_FLAGS MATCHES "-fsanitize")
    set(BUILDSYSTEM_OUTPUT_SUFFIX "-ASAN")
endif()
set(BUILDSYSTEM_BIN_DIR "${CMAKE_SOURCE_DIR}/Bin/$<CONFIG>/${_dr_plat}-${_dr_comp}${BUILDSYSTEM_OUTPUT_SUFFIX}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY     "${BUILDSYSTEM_BIN_DIR}")       # exes + Windows DLLs
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY     "${BUILDSYSTEM_BIN_DIR}")       # .so plugins/modules (beside the exe)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY     "${BUILDSYSTEM_BIN_DIR}/lib")   # static .a/.lib + MSVC import libs
set(CMAKE_PDB_OUTPUT_DIRECTORY         "${BUILDSYSTEM_BIN_DIR}")       # MSVC linker PDBs (beside the exe)
# NOT set for MSVC: compile PDBs are per-OBJECT intermediates, and CMake names them from the
# compiler default (vc<ver>.pdb) rather than the target, so pointing every target at one shared
# directory makes all of them write the same file. Parallel cl.exe jobs then lose the race with
# "C1041: cannot open program database" (49 failures in the first triage build; /FS alone still
# left 14). Left unset, each target gets its own build directory and the collision cannot happen.
# The LINKER PDBs - the ones worth having beside the binaries - are CMAKE_PDB_OUTPUT_DIRECTORY
# above and are unaffected.
if(NOT MSVC)
    set(CMAKE_COMPILE_PDB_OUTPUT_DIRECTORY "${BUILDSYSTEM_BIN_DIR}/lib")
endif()

# Reusable helpers (util_copy_runtime_deps, ...).
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Helpers.cmake)

# Add a module's flat-sibling doctest suite, but only when the module target was actually built -
# so the web subset and platform-gated backends (Shell.Desktop, ...) skip cleanly; a test never
# outlives its module. The whole cluster is wrapped once in if(OPTION_BUILD_TESTS) at the call site.
macro(util_test_suite dir target)
    if(TARGET ${target})
        add_subdirectory(${dir})
    endif()
endmacro()

# --- Options -----------------------------------------------------------------
option(OPTION_BUILD_TESTS "Build unit tests" ON)
option(OPTION_BUILD_SAMPLES "Build sample applications" ON)
option(OPTION_BUILD_TOOLS "Build editor/authoring tooling (the editor libraries + CLI exes)" ON)
# Experimental modules and optional extensions are independently switchable; each exposes a
# OPTION_HAS_* compile define (see the policy block) so C++ can guard usage with #if.
option(OPTION_EXPERIMENTAL_GUI "Build the experimental GUI framework (experimental.gui + shell/vfs)" ON)
option(OPTION_EXTENSION_IMGUI  "Build the Dear ImGui debug-UI extension (extensions.imgui)" ON)
# Script backends are independently switchable; each defines OPTION_HAS_<backend> (presence macro,
# guard C++ with `#ifdef`) when enabled. At least one must stay on. AngelScript additionally needs
# its vendored lib to have built (reconciled after ThirdParty is added, before the backend subdirs).
option(OPTION_ENABLE_ANGELSCRIPT "Build the AngelScript script backend" ON)
option(OPTION_ENABLE_LUAU        "Build the Luau script backend" ON)
option(OPTION_WARNINGS_AS_ERRORS "Treat warnings as errors" ON)

# A web (Emscripten) build ships a game, not the authoring/side toolchain: force the tooling,
# experimental, and extension groups off up front, before the policy defines below capture them.
if(EMSCRIPTEN)
    set(OPTION_BUILD_TOOLS OFF)
    set(OPTION_EXPERIMENTAL_GUI OFF)
    # OPTION_EXTENSION_IMGUI stays available on web: Dear ImGui + extensions.imgui compile for
    # wasm (the renderer is pure RHI, the subsystem consumes the shell's input), so the debug
    # panel works in the browser too (WebScene's tweak UI).
endif()

# --- Product binary basenames: single source of truth ------------------------
# A few executables are referenced by name/path from OUTSIDE their own target, so the name cannot
# live only in the target definition:
#   - the export host-template stages the player and reads "<player>.runtime-libs" beside the tool
#     (Editor.Core gets the name below as the BUILTIN_PLAYER_BASENAME compile define)
#   - the web build invokes the NATIVE shader-pack cook by PATH (cross-config: the wasm build reaches
#     into the desktop Bin dir, so it cannot use $<TARGET_FILE:>) - Player/WebScene read this name
# Change a name here and every reference (OUTPUT_NAME, the C++ constant, the sidecar, the cook path)
# follows. Dotted, matching the target names.
set(BUILTIN_PLAYER_BASENAME         "Engine.Player")
set(BUILDSYSTEM_SHADERPACKCOOK_BASENAME "Tools.ShaderPack")
set(USER_DATA_DIR_NAME_VALUE        "gameengine")  # OS user-data folder (settings/projects/templates)
set(TEMPLATE_ID_PREFIX_VALUE        "gameengine")  # export-template id prefix (<prefix>-<platform>-<config>-<ver>)

# Where the DESKTOP build put its executables, as seen from a CROSS build (wasm). The web
# build shells out to native cook tools by path - it cannot use $<TARGET_FILE:> because those
# targets do not exist in the wasm configure - so it needs the host's Bin dir. Derived from
# CMAKE_HOST_* (CMAKE_SYSTEM_NAME is "Emscripten" here, so the _dr_plat above would be wrong),
# and the exe suffix is the HOST's - CMAKE_EXECUTABLE_SUFFIX is ".js" under Emscripten.
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

# Shipping flavor (Prelude: "define exactly one" of DEBUG/RELEASE/SHIPPING): an optimized
# build with asserts compiled out + stripped binaries - the dist/size-measurement config.
# Presets clang-shipping/gcc-shipping turn it on (Release + BUILDSYSTEM_OUTPUT_SUFFIX so it
# never clobbers a plain Release Bin dir).
option(OPTION_SHIPPING_BUILD "Shipping config: asserts compiled out, binaries stripped" OFF)

target_compile_definitions(policy INTERFACE
    $<$<CONFIG:Debug>:BUILD_DEBUG=1>
    $<$<AND:$<CONFIG:Release,RelWithDebInfo,MinSizeRel>,$<NOT:$<BOOL:${OPTION_SHIPPING_BUILD}>>>:BUILD_RELEASE=1>
    $<$<BOOL:${OPTION_SHIPPING_BUILD}>:BUILD_SHIPPING=1>
    # Drives ENGINE_EXPORT_DATA (Core/Prelude.h). GLOBAL, not per-target, and that is the
    # point: one BMI is shared by the library that defines an entity and every consumer, so
    # the annotation must expand the same way everywhere (shared-libraries.md P5).
    $<$<BOOL:${ENGINE_SHARED_LIBS}>:BUILDSYSTEM_SHARED_LIBS=1>
    # Instrumentation on in Debug/RelWithDebInfo; off (compiles to nothing) in shipping builds.
    $<$<CONFIG:Debug,RelWithDebInfo>:BUILD_PROFILING=1>
    # Build identity as strings, matching the Bin/<Config>/<Platform>-<Compiler> layout: the export
    # host template stamps itself with the config/compiler that built the running tool.
    BUILD_CONFIG=\"$<CONFIG>\"
    BUILD_COMPILER=\"${_dr_comp}\"
    # Optional-module presence flags (0/1) - guard usage in C++ with e.g.
    # `#if OPTION_HAS_EXTENSION_IMGUI` / `#if OPTION_HAS_EXPERIMENTAL_GUI`.
    OPTION_HAS_EXPERIMENTAL_GUI=$<BOOL:${OPTION_EXPERIMENTAL_GUI}>
    OPTION_HAS_EXTENSION_IMGUI=$<BOOL:${OPTION_EXTENSION_IMGUI}>
    USER_DATA_DIR_NAME=u8\"${USER_DATA_DIR_NAME_VALUE}\"
    TEMPLATE_ID_PREFIX=u8\"${TEMPLATE_ID_PREFIX_VALUE}\")

target_link_options(policy INTERFACE
    # Shipping: strip symbol tables at link (what a dist ships; the honest size number).
    $<$<AND:$<BOOL:${OPTION_SHIPPING_BUILD}>,$<CXX_COMPILER_ID:Clang,GNU>>:-s>)

# MSVC: CMake seeds CMAKE_CXX_FLAGS with "/EHsc". Appending /EHs-c- below does win, but cl
# reports every override - 9036 "D9025: overriding '/EHs' with '/EHs-'" lines in one build,
# which buries real diagnostics. Remove the default instead of fighting it; the policy flag
# right below is the single source of truth for exception behavior.
if(MSVC)
    string(REGEX REPLACE "[-/]EH[acsr-]+" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(STRIP "${CMAKE_CXX_FLAGS}" CMAKE_CXX_FLAGS)
endif()

target_compile_options(policy INTERFACE
    $<$<CXX_COMPILER_ID:Clang,GNU>:-fno-exceptions;-fno-rtti>
    # Every static lib shares CMAKE_COMPILE_PDB_OUTPUT_DIRECTORY and none sets a PDB name, so
    # parallel cl.exe jobs all target one vc140.pdb and lose the race ("C1041: cannot open
    # program database"). /FS funnels those writes through mspdbsrv, which is what the
    # diagnostic itself recommends.
    $<$<CXX_COMPILER_ID:MSVC>:/FS>
    # The tree is UTF-8 and uses u8"..." and non-ASCII character literals (e.g. U'●').
    # Without /utf-8 cl decodes sources in the system ANSI codepage and silently mis-reads
    # them - a 3-byte UTF-8 scalar in a char32_t literal becomes "C2015: too many characters
    # in constant". Note this does NOT show up as C4819; that only fires when a character
    # cannot be represented at all, so the absence of C4819 is not evidence /utf-8 is unneeded.
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    # GCC modules: eager gcm loading. The default lazy loader intermittently fails to merge
    # template pendings across module imports ("failed to read compiled module cluster" /
    # "failed to load pendings for ...") - deterministic in Tools.Cook's import mix on GCC 15.2.
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
        # ITS OWN headers - 114 instances in a full build, none from Code/. It is a restatement of
        # a deliberate choice, not a finding, and under /WX it fails ~113 otherwise-clean targets.
        $<$<CXX_COMPILER_ID:MSVC>:/WX;/wd4530>)
    # Clang on Windows emits a spurious "argument unused during compilation: '-c'"
    # during the CMake module-dependency scan step. Suppress just that diagnostic
    # so -Werror doesn't kill the build.
    # Clang 22+ warns that __COUNTER__ is a "C2y extension" when used in C++ mode;
    # doctest (and other widely-used macros) rely on it, so suppress the diagnostic.
    target_compile_options(policy INTERFACE
        $<$<CXX_COMPILER_ID:Clang>:-Wno-error=unused-command-line-argument;-Wno-c2y-extensions>)
    # em++ re-invokes clang++ for the module precompile and does NOT forward
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
    ${CMAKE_SOURCE_DIR}/Code            # shared headers (TestMain.h) + any cross-role prefixes
    ${CMAKE_SOURCE_DIR}/Code/Foundation
    ${CMAKE_SOURCE_DIR}/Code/Engine
    ${CMAKE_SOURCE_DIR}/Code/Editor
    ${CMAKE_SOURCE_DIR}/Code/Tools
    ${CMAKE_SOURCE_DIR}/Code/Experimental
    ${CMAKE_SOURCE_DIR}/Code/Extensions)

# --- Targets -----------------------------------------------------------------
# ONE canonical, dependency-ordered list for every platform. Order is a valid topological order -
# a few libs probe siblings at configure time with if(TARGET ...) (Graphics->RHI backends,
# Editor.Core->Shaders, Script->AngelScript). Web builds the SAME list: the groups it cannot run
# yet - tooling, experimental, extensions - are forced off for EMSCRIPTEN above, so they skip in
# place. No separate web climb to keep in sync; web-only samples + return() sit at the tail.
if(EMSCRIPTEN)
    # Emscripten defaults are sized for tiny demos: 64KB stack (engine code overflows it) and a
    # fixed 16MB heap. 2MB stack + growable heap; link-time-only knobs applied to every target below.
    add_link_options(-sSTACK_SIZE=2097152 -sALLOW_MEMORY_GROWTH=1)
endif()

add_subdirectory(ThirdParty)
add_subdirectory(Code/Foundation/Core)
add_subdirectory(Code/Foundation/Settings)  # typed/versioned settings store over Core (runtime + editor)
add_subdirectory(Code/Foundation/Profiler)  # CPU scope profiler over Core (instrumented across the engine)
add_subdirectory(Code/Foundation/Messaging) # native name-keyed deferred EventBus over Core (run-scope messaging)
add_subdirectory(Code/Foundation/Lod) # LOD selection math leaf over Core (mesh chains + terrain chunks share it)
add_subdirectory(Code/Foundation/Spline) # authorable 3D spline math leaf over Core (component/editor/consumers share it)
add_subdirectory(Code/Foundation/Net)       # real-time game networking: wire + INetTransport + loopback sim (over Core)
add_subdirectory(Code/Foundation/Net.Replication)  # StateReplication foundation: NetworkId + reflection-driven field codec (over Net + Core)
add_subdirectory(Code/Foundation/Http)      # small HTTP/1.1 client+server+SSE over Core/System TCP (MCP transport + WS upgrade substrate)
add_subdirectory(Code/Foundation/Xml)       # DOM XML parser + writer over Core
    add_subdirectory(Code/Foundation/Xml.Serialization)
add_subdirectory(Code/Foundation/Json)      # hand-rolled JSON DOM (MCP wire + game data); RTTI-reflected for script
add_subdirectory(Code/Foundation/Mcp)       # JSON-RPC 2.0 + MCP protocol (tool/resource registry, stdio); over Json
add_subdirectory(Code/Foundation/Mcp.Http)  # MCP streamable-HTTP binding (bearer token + SSE channel); over Http + Mcp
add_subdirectory(Code/Foundation/Mcp.Reflection)  # MCP reflection tools (type_list/type_info) over Mcp+Core
add_subdirectory(Code/Foundation/VFS)       # byte access over Core stream primitives
    add_subdirectory(Code/Foundation/VFS.Pak)
add_subdirectory(Code/Engine/Engine.Project)   # project manifest/layout (runtime-side, editor-free)
add_subdirectory(Code/Foundation/Content)   # content database over Core + VFS
add_subdirectory(Code/Foundation/Resource)  # resource manager over content
add_subdirectory(Code/Foundation/RHI)   # depends only on Core (foundational sibling of Runtime)
# --- RHI backends (Null/Validation always; GPU backends by platform + availability) ---
add_subdirectory(Code/Foundation/RHI.Null)
add_subdirectory(Code/Foundation/RHI.Validation)
add_subdirectory(Code/Foundation/RHI.TestSupport) # offscreen-render test substrate (RHI + Core only)
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
add_subdirectory(Code/Foundation/Image.DDS)  # DDS container + BCn decode (bcdec)
add_subdirectory(Code/Foundation/Image.IO)
add_subdirectory(Code/Foundation/Image.Resource)
add_subdirectory(Code/Foundation/Model)
    add_subdirectory(Code/Foundation/Model.IO)
    add_subdirectory(Code/Foundation/Model.GLTF)
    add_subdirectory(Code/Foundation/Model.FBX)
add_subdirectory(Code/Foundation/Geometry)  # engine runtime mesh format (StaticMesh/SkinnedMesh + primitives)
add_subdirectory(Code/Foundation/Geometry.Resource)
add_subdirectory(Code/Foundation/Heightfield) # pure heightfield grid + sampling math; shared by render/physics/nav
add_subdirectory(Code/Foundation/Heightfield.Resource) # heightfield as a cooked, referenceable resource (Ref<Heightfield>)
add_subdirectory(Code/Foundation/Terrain) # terrain model over a heightfield: chunk grid + quadtree + LOD + splat (no RHI)
add_subdirectory(Code/Foundation/Terrain.Resource) # cooked terrain resource: heightfield + splatmap + layer refs (Ref<Terrain>)
add_subdirectory(Code/Foundation/Vegetation.Resource) # painted vegetation mask resource: density planes + brush cores + cooked source/factory
add_subdirectory(Code/Foundation/Vegetation) # terrain vegetation model: layer params + deterministic per-chunk scatter + fade (no RHI)
add_subdirectory(Code/Foundation/Animation) # skeletal-animation foundation (Sedulous.Animation port; Core-only)
add_subdirectory(Code/Foundation/PropertyAnimation) # reflected property-curve animation (Core-only; NOT skeletal)
add_subdirectory(Code/Foundation/PropertyAnimation.Resource) # cooked property-clip resource + loader (model-A)
add_subdirectory(Code/Foundation/Animation.Resource)
add_subdirectory(Code/Foundation/Particles) # CPU particle runtime (Sedulous.Particles port; Core-only)
add_subdirectory(Code/Foundation/Particles.Resource)
add_subdirectory(Code/Foundation/Physics)   # Jolt-backed rigid-body core (world/bodies/queries/events; JPH never public)
add_subdirectory(Code/Foundation/Physics.Resource)  # cooked collision shapes + physical materials
add_subdirectory(Code/Foundation/Navigation) # Recast/Detour navmesh core (bake + query + crowd; rc*/dt* never public)
add_subdirectory(Code/Foundation/Navigation.Resource) # cooked navmesh-zone resource + factory
add_subdirectory(Code/Foundation/Audio)     # miniaudio-backed audio engine core (voices/buses/handles; ma_* never public)
add_subdirectory(Code/Foundation/Audio.Resource)    # cooked audio clips + factory
add_subdirectory(Code/Foundation/Scene)     # scene / ECS foundation (entities, transforms, components, SceneSubsystem)
add_subdirectory(Code/Engine/Engine.Scene)
add_subdirectory(Code/Foundation/Scene.Resource)
add_subdirectory(Code/Engine/Engine.Domain) # what one engine domain declares (DomainModule) + the composition built from a list of them
add_subdirectory(Code/Foundation/Render.Api) # light scene-rendering interface (ISceneRenderer) - tools drive rendering w/o linking the renderer
add_subdirectory(Code/Foundation/Render)    # renderer: scene->render extraction + (later) the GPU forward path
add_subdirectory(Code/Engine/Engine.Render)
add_subdirectory(Code/Engine/Engine.Animation) # skeletal-animation scene integration (links RenderSubsystem; after Render)
add_subdirectory(Code/Engine/Engine.Particles) # particle scene/render integration (renderer + provider seam; after Render)
add_subdirectory(Code/Engine/Engine.Physics) # physics scene integration (components + per-scene Jolt world + interpolation; after Render)
add_subdirectory(Code/Engine/Engine.Terrain) # terrain scene integration (TerrainComponent)
add_subdirectory(Code/Engine/Engine.Vegetation) # terrain vegetation: per-layer component + per-chunk instanced sets provider (VegetationSubsystem)
add_subdirectory(Code/Engine/Engine.Spline) # authorable 3D spline scene component (data-only; math in Foundation::Spline)
add_subdirectory(Code/Engine/Engine.Navigation) # navmesh scene integration (zone/agent components + per-scene Detour crowd)
add_subdirectory(Code/Engine/Engine.Audio)   # audio scene integration (components + per-scene voice group + listener sync; after Render)
add_subdirectory(Code/Foundation/Fonts)     # font types/interfaces over Core + Image
add_subdirectory(Code/Foundation/Fonts.Coverage)
add_subdirectory(Code/Foundation/Fonts.TrueType)
add_subdirectory(Code/Foundation/Fonts.Coverage.Baker)
add_subdirectory(Code/Foundation/Fonts.DistanceField)
add_subdirectory(Code/Foundation/Fonts.Resource)
if(TARGET ThirdParty::MsdfgenCore)  # DF baker (editor-time TTF -> baked); needs the vendored msdfgen
    add_subdirectory(Code/Foundation/Fonts.DistanceField.Baker)
endif()
add_subdirectory(Code/Foundation/Texture)   # texture descriptors + cooked resource/GPU factory + editor
add_subdirectory(Code/Foundation/Texture.Resource)
add_subdirectory(Code/Foundation/Materials) # data-driven material model (PipelineConfig + bind-group-inferring MaterialSystem)
add_subdirectory(Code/Foundation/Materials.PipelineCache)
add_subdirectory(Code/Foundation/Materials.Resource)
add_subdirectory(Code/Foundation/Model.Resource) # cooked model manifest + runtime composite (runtime side; no tooling deps)
add_subdirectory(Code/Foundation/UI.Resource)   # cooked UI documents + themes (game-ui; UI-framework-free records)
add_subdirectory(Code/Engine/Engine.UI)  # game screen tier: canvases + overlay render + consumption
add_subdirectory(Code/Foundation/VG)        # 2D vector-graphics core over Core + Image
add_subdirectory(Code/Foundation/VG.SVG)
add_subdirectory(Code/Foundation/VG.Renderer)
add_subdirectory(Code/Foundation/UI)        # retained-mode UI framework (faithful Sedulous.UI port) over Core (+ VG/Fonts as phases land)
add_subdirectory(Code/Foundation/UI.VFS)
add_subdirectory(Code/Foundation/UI.Shell)
add_subdirectory(Code/Foundation/UI.Toolkit)
add_subdirectory(Code/Foundation/UI.Gamekit)  # game-UI conveniences (UIScreen + ScreenStack) over foundation.ui, sibling of UI.Toolkit
add_subdirectory(Code/Foundation/UI.Viewport)
# --- UI desktop hosts (UIHost + docking workbench; link Graphics/Runtime.Client, desktop-only) ---
if(NOT EMSCRIPTEN)
    add_subdirectory(Code/Foundation/UI.Runtime)
    add_subdirectory(Code/Foundation/UI.Application)
endif()
if(OPTION_EXPERIMENTAL_GUI)
    add_subdirectory(Code/Experimental/GUI)   # fresh eepp-derived UI framework
endif()
add_subdirectory(Code/Foundation/Script)
# --- Script backends: independently switchable via OPTION_ENABLE_* (declared up top) -----------
# Both OPTION_ENABLE_ANGELSCRIPT/LUAU flags are defined in the option block above. Only AngelScript
# needs reconciling here: its vendored lib is conditional (may not build on a target), so a requested-
# but-absent AngelScript flips OFF. Luau vendors unconditionally - nothing to reconcile. Then enforce
# at-least-one and publish the OPTION_HAS_<backend> presence defines (C++ guards usage with #ifdef).
if(OPTION_ENABLE_ANGELSCRIPT AND NOT TARGET ThirdParty::AngelScript)
    message(STATUS "AngelScript backend requested but its vendored lib did not build - disabling OPTION_ENABLE_ANGELSCRIPT.")
    set(OPTION_ENABLE_ANGELSCRIPT OFF)
endif()
if(NOT OPTION_ENABLE_ANGELSCRIPT AND NOT OPTION_ENABLE_LUAU)
    message(FATAL_ERROR "All script backends are disabled; enable at least one of OPTION_ENABLE_ANGELSCRIPT / OPTION_ENABLE_LUAU.")
endif()
# Presence macros (defined ONLY when enabled), so C++ guards read `#ifdef OPTION_HAS_ANGELSCRIPT`.
target_compile_definitions(policy INTERFACE
    $<$<BOOL:${OPTION_ENABLE_ANGELSCRIPT}>:OPTION_HAS_ANGELSCRIPT>
    $<$<BOOL:${OPTION_ENABLE_LUAU}>:OPTION_HAS_LUAU>)

# Target-or-empty aliases for the OPTIONAL backend libs, so a SHARED consumer (composition roots,
# multi-backend test suites) lists ${SCRIPTLIB_*} in target_link_libraries and it resolves to the
# real target when the backend is on, or to nothing when it is off - no per-site if() needed.
# (The per-backend subdirs are gated whole, so they keep their explicit names.)
set(SCRIPTLIB_ANGELSCRIPT "")
set(SCRIPTLIB_LUAU "")
set(SCRIPTLIB_ANGELSCRIPT_PIPELINE "")
set(SCRIPTLIB_LUAU_PIPELINE "")
set(SCRIPTLIB_ANGELSCRIPT_EDITOR "")
set(SCRIPTLIB_LUAU_EDITOR "")
if(OPTION_ENABLE_ANGELSCRIPT)
    set(SCRIPTLIB_ANGELSCRIPT Foundation::Script.AngelScript)
    set(SCRIPTLIB_ANGELSCRIPT_PIPELINE Pipeline::Script.AngelScript)
    set(SCRIPTLIB_ANGELSCRIPT_EDITOR Editor::Script.AngelScript)
endif()
if(OPTION_ENABLE_LUAU)
    set(SCRIPTLIB_LUAU Foundation::Script.Luau)
    set(SCRIPTLIB_LUAU_PIPELINE Pipeline::Script.Luau)
    set(SCRIPTLIB_LUAU_EDITOR Editor::Script.Luau)
endif()

if(OPTION_ENABLE_LUAU)
    add_subdirectory(Code/Foundation/Script.Luau)
endif()
if(OPTION_ENABLE_ANGELSCRIPT)
    add_subdirectory(Code/Foundation/Script.AngelScript)
endif()
add_subdirectory(Code/Foundation/Script.Resource)   # cooked script classes + factory (source + harvested metadata)
add_subdirectory(Code/Foundation/Script.Facades)    # curated behavior facades (Entity/Log/Time/Random) - shared by cook VM + runtime
add_subdirectory(Code/Foundation/Mcp.Script)        # MCP scripting tools (script_api) over Mcp + Script + Script.Facades
add_subdirectory(Code/Foundation/Net.WebSocket)     # native WS accept path (RFC 6455 over Http) + hybrid UDP+WS socket - browser clients join native hosts
add_subdirectory(Code/Foundation/Net.Manager)       # per-instance networked endpoint (NetworkManager) + Net script facade (over Net + Script)
add_subdirectory(Code/Engine/Engine.Net)     # once-per-context NetworkSubsystem (injects NetworkComponentManager into scenes)
add_subdirectory(Code/Foundation/Shell)     # raw platform/OS service (IShell: window/input/events) over Core; below Runtime
# --- Shell backends (Null always; browser shell on emscripten, SDL3 desktop otherwise) ---
add_subdirectory(Code/Foundation/Shell.Null)
if(EMSCRIPTEN)
    add_subdirectory(Code/Foundation/Shell.Web)
else()
    # SDL3 is resolved in ThirdParty (vendored + static by default; ENGINE_SDL_TARGET set there,
    # SDL3::SDL3-static or system SDL3::SDL3). Consumers link ${ENGINE_SDL_TARGET}.
    if(ENGINE_SDL_TARGET)
        add_subdirectory(Code/Foundation/Shell.Desktop)
    else()
        message(STATUS "SDL3 unavailable - skipping Foundation::Shell.Desktop")
    endif()
endif()
add_subdirectory(Code/Foundation/Input)     # action-mapping layer (InputMap + ActionRuntime + asset/cooked) over Core + Shell
add_subdirectory(Code/Foundation/Input.Resource)
if(OPTION_EXPERIMENTAL_GUI)
    add_subdirectory(Code/Experimental/GUI.Shell)  # platform input bridge (Shell InputEvents -> GUI EventDispatcher)
    add_subdirectory(Code/Experimental/GUI.VFS)   # VFS-backed IResourceProvider (background-image: url())
endif()
add_subdirectory(Code/Foundation/Graphics)  # GraphicsDevice/RenderWindow render host over RHI+Shell; below Runtime
add_subdirectory(Code/Foundation/Runtime)
add_subdirectory(Code/Foundation/Runtime.Client)
add_subdirectory(Code/Engine/Engine.GameInstance)
add_subdirectory(Code/Engine/Engine.UI.Script)  # UI script surface: reflected view handles + `ui` screen-tier facade (over UI.Gamekit)
# The script-surface composition root: links every subsystem facade above and exposes
# RegisterAllScriptFacades() for surface-describing hosts (MCP). Must come after all the subsystem
# libs + Net.Manager/Replication it aggregates.
add_subdirectory(Code/Engine/Engine.DefaultApp)
# --- Runtime runner backend (browser rAF loop vs blocking desktop loop) ---
if(EMSCRIPTEN)
    add_subdirectory(Code/Foundation/Runtime.Web)
else()
    add_subdirectory(Code/Foundation/Runtime.Desktop)
endif()
add_subdirectory(Code/Engine/Engine.Input)  # input runtime hookup (tick + device provider seam) over Runtime
add_subdirectory(Code/Engine/Engine.Script) # entity behaviors (ScriptComponent + the run's ONE gameplay script context) over Runtime
add_subdirectory(Code/Engine/Engine.Integration) # cross-subsystem composition helpers (physics-contact -> script bridge) over Physics + Script
# The scene-surface composition root: aggregates every domain's Add<Domain>SceneManagers (the same
# functions the subsystems' OnSceneCreated delegate to) for headless scene consumers (export
# transcode, MCP scene_validate). Must come after all the domain libs it aggregates (incl. Script).
add_subdirectory(Code/Engine/Engine.Composition) # THE composition root: the one list of domains, facets on demand (scene, reflection, resource types, factories, facades)
if(OPTION_EXTENSION_IMGUI)
    add_subdirectory(Code/Extensions/Imgui) # Dear ImGui debug UI (RHI renderer + subsystem) over Runtime
endif()
add_subdirectory(Code/Engine/Engine.Player)        # Generic game runner


# --- Authoring tooling: the editor libraries + the CLI executables that assemble them.
# Gated by OPTION_BUILD_TOOLS (forced OFF for web / runtime-only / shipping builds) so the
# whole editor/cook surface drops cleanly. The in-folder *.Editor sub-targets are gated the
# same way inside their Foundation libs; every consumer of them lives inside this guard. ---
if(OPTION_BUILD_TOOLS)
    add_subdirectory(Code/Pipeline/Pipeline.Core)  # asset-pipeline authoring base: Asset + builders (tooling)
    add_subdirectory(Code/Pipeline/Pipeline.Importer) # file-import framework (IFileImporter + registry, headless)
    add_subdirectory(Code/Pipeline/Pipeline.Cook)  # incremental cook driver + pipeline DB (tooling, headless)
    add_subdirectory(Code/Editor/Editor.Project) # the headless project half: project/cook/export/jobs/log (tooling; hosts without an editor stand on it)
    add_subdirectory(Code/Editor/Editor.Core) # the editor-facing half: context/commands/pages/selection/actions over Editor.Project (tooling)
    add_subdirectory(Code/Editor/Editor.Mcp) # MCP project tools (project_create/open/info); headless
    add_subdirectory(Code/Pipeline/Physics.Pipeline)    # collision/material assets + builders (tooling)
    add_subdirectory(Code/Pipeline/Render.Pipeline)     # render profile assets + builders (tooling)
    add_subdirectory(Code/Pipeline/Navigation.Pipeline) # navmesh-zone asset + builder (tooling)
    add_subdirectory(Code/Pipeline/Audio.Pipeline)      # audio clip asset/builder/importer (tooling)
    add_subdirectory(Code/Pipeline/UI.Pipeline)     # UI document/theme assets + validating builders (tooling)
    add_subdirectory(Code/Pipeline/ModelImporter) # top-level model import: model IR -> cooked geometry/material/texture/animation resources
    add_subdirectory(Code/Pipeline/Script.Pipeline)  # script class asset/builder/importer (cook; neutral)
    # Per-backend cook + editor-UI (lexer) - each gated on its OPTION_ENABLE_* flag.
    if(OPTION_ENABLE_ANGELSCRIPT)
        add_subdirectory(Code/Pipeline/Script.AngelScript.Pipeline)  # AngelScript cook service
        add_subdirectory(Code/Editor/Editor.Script.AngelScript)  # AngelScript editor-UI (lexers)
    endif()
    if(OPTION_ENABLE_LUAU)
        add_subdirectory(Code/Pipeline/Script.Luau.Pipeline)  # Luau cook service
        add_subdirectory(Code/Editor/Editor.Script.Luau)  # Luau editor-UI (CodeEditView lexer)
    endif()
    add_subdirectory(Code/Editor/Editor.App) # editor UI shell (chrome + layout persistence + EditorApplication)
    add_subdirectory(Code/Editor/Editor.Input) # input-map editing page (editor exe only)
    add_subdirectory(Code/Editor/Editor.PropertyAnimation) # property-animation clip page (editor exe only)
    add_subdirectory(Code/Editor/Editor.GameUI) # UI document page: text + live preview (editor exe only)
    add_subdirectory(Code/Editor/Editor.Audio)  # audio clip audition page: waveform + play (editor exe only)
    add_subdirectory(Code/Editor/Editor.Texture) # texture asset page: import settings + preview (editor exe only)
    add_subdirectory(Code/Editor/Editor.Image) # image asset page: inspect + color-space intent (editor exe only)
    add_subdirectory(Code/Editor/Editor.Heightfield) # heightfield asset page: 2D grayscale preview + fields (editor exe only)
    add_subdirectory(Code/Editor/Editor.Terrain) # terrain asset page: 3D preview + references/layers (editor exe only)
    add_subdirectory(Code/Editor/Editor.Vegetation) # Paint Vegetation brush: the terrain's vegetation mask planes (editor exe only)
    add_subdirectory(Code/Editor/Editor.Spline) # interactive spline viewport tool (editor exe only)
    add_subdirectory(Code/Editor/Editor.Generic) # fallback asset page: serialize-driven property form (editor exe only)
    add_subdirectory(Code/Editor/Editor.Script) # script behavior source page: edit + recook + inline errors (editor exe only)
    add_subdirectory(Code/Editor/Editor.ViewportTools) # viewport tool-mode framework (IViewportTool + manager + provider registry)
    add_subdirectory(Code/Editor/Editor.Camera) # shared free-fly viewport camera (lean, foundation-only) for all editor pages
    add_subdirectory(Code/Editor/Editor.Preview) # shared 3D preview viewport substrate (PreviewViewport, over editor.camera) for bespoke asset pages
    add_subdirectory(Code/Editor/Editor.Scene) # scene editor plugin (SceneEditorPage + RegisterSceneEditor)
    add_subdirectory(Code/Editor/Editor.Navigation) # navmesh bake tooling (collect + Recast bake -> zone asset)

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
    add_subdirectory(Code/Pipeline/Texture.Compression) # BC encoder + policy table (asset-variants)
    add_subdirectory(Code/Pipeline/Texture.Pipeline)
    if(TARGET Foundation::Fonts.DistanceField.Baker)
        add_subdirectory(Code/Pipeline/Fonts.Pipeline)
        add_subdirectory(Code/Editor/Editor.Fonts) # font asset page: bake intent + live atlas preview
        add_subdirectory(Code/Editor/Editor.Physics) # collision-shape asset page: mesh picker + cook
    endif()

    # The pipeline's composition root: links every pipeline module above and exposes the single
    # RegisterAllBuilders/RegisterAllImporters/RegisterPipelineTypes set every host consumes. Must
    # come AFTER all pipeline modules (incl. the fonts block) since it links them.
    add_subdirectory(Code/Pipeline/Pipeline.Registration)

    add_subdirectory(Code/Tools/Tools.Editor)        # Editor executable (editor assembly)
    add_subdirectory(Code/Tools/Tools.Cook)          # Tools.Cook CLI cooker (headless)
    add_subdirectory(Code/Tools/Tools.Mcp)          # headless MCP stdio server (agent access)
    add_subdirectory(Code/Tools/Tools.Export)        # Export dist packager
    add_subdirectory(Code/Tools/Tools.ShaderPack) # ShaderPack: cook shaders.dpak (WGSL for web)
endif()

# --- Tests: every module's flat-sibling doctest suite (guarded by its module target so the
# web subset + platform-gated backends skip when the module was not built) ---
if(OPTION_BUILD_TESTS)
    util_test_suite(Code/Pipeline/Animation.Pipeline.Tests              Pipeline::Animation)
    util_test_suite(Code/Pipeline/PropertyAnimation.Pipeline.Tests       Pipeline::PropertyAnimation)
    util_test_suite(Code/Pipeline/Audio.Pipeline.Tests                  Pipeline::Audio)
    util_test_suite(Code/Engine/Engine.DefaultApp.Tests             Engine::DefaultApp)
    util_test_suite(Code/Editor/Editor.App.Tests                    Editor::App)
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
    util_test_suite(Code/Editor/Editor.Tests                        Pipeline::Core)
    util_test_suite(Code/Editor/Editor.Texture.Tests                Editor::Texture)
    util_test_suite(Code/Editor/Editor.GameUI.Tests                 Editor::GameUI)
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
    util_test_suite(Code/Pipeline/UI.Pipeline.Tests                     Pipeline::UI)
    util_test_suite(Code/Engine/Engine.Animation.Tests              Engine::Animation)
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
    # Monolithic: one test file exercises both backends -> build only when both are on.
    if(OPTION_ENABLE_ANGELSCRIPT AND OPTION_ENABLE_LUAU)
        util_test_suite(Code/Engine/Engine.Script.Tests Engine::Script)
    endif()
    util_test_suite(Code/Engine/Engine.UI.Tests                     Engine::UI)
    util_test_suite(Code/Engine/Engine.UI.Script.Tests              Engine::UI.Script)
    util_test_suite(Code/Experimental/GUI.Shell.Tests               Experimental::GUI.Shell)
    util_test_suite(Code/Experimental/GUI.Tests                     Experimental::GUI)
    util_test_suite(Code/Experimental/GUI.VFS.Tests                 Experimental::GUI.VFS)
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
    util_test_suite(Code/Foundation/Net.Manager.Tests Foundation::Net.Manager)  # its script facade test (as) is file-gated in the suite CMake
    util_test_suite(Code/Foundation/Net.Replication.Tests           Foundation::Net.Replication)
    util_test_suite(Code/Foundation/Net.Tests                       Foundation::Net)
    util_test_suite(Code/Foundation/Net.WebSocket.Tests             Foundation::Net.WebSocket)
    util_test_suite(Code/Foundation/Http.Tests                      Foundation::Http)
    util_test_suite(Code/Foundation/Particles.Resource.Tests        Foundation::Particles.Resource)
    util_test_suite(Code/Foundation/Particles.Tests                 Foundation::Particles)
    util_test_suite(Code/Foundation/Physics.Tests                   Foundation::Physics)
    util_test_suite(Code/Foundation/Navigation.Tests                Foundation::Navigation)
    util_test_suite(Code/Foundation/Profiler.Tests                  Foundation::Profiler)
    util_test_suite(Code/Foundation/RHI.Null.Tests                  Foundation::RHI.Null)
    util_test_suite(Code/Foundation/RHI.Validation.Tests            Foundation::RHI.Validation)
    if(TARGET Foundation::RHI.Vulkan)
        util_test_suite(Code/Foundation/RHI.Vulkan.Tests            Foundation::RHI.Vulkan)
    endif()
    if(NOT EMSCRIPTEN)  # spins up a real device; no headless web path yet
        util_test_suite(Code/Foundation/RHI.WebGPU.Tests Foundation::RHI.WebGPU)
        if(TARGET Foundation::RHI.Vulkan AND TARGET Foundation::RHI.WebGPU)
            # Cross-backend pixel probes (need Vulkan + WebGPU compiled; DX12 added on top where it
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
    util_test_suite(Code/Foundation/Lod.Tests                       Foundation::Lod)
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
    util_test_suite(Code/Foundation/UI.Shell.Tests                  Foundation::UI.Shell)
    util_test_suite(Code/Foundation/UI.Tests                        Foundation::UI)
    util_test_suite(Code/Foundation/UI.Toolkit.Tests                Foundation::UI.Toolkit)
    util_test_suite(Code/Foundation/UI.Application.Tests            Foundation::UI.Application)
    util_test_suite(Code/Foundation/UI.Gamekit.Tests                Foundation::UI.Gamekit)
    util_test_suite(Code/Foundation/UI.VFS.Tests                    Foundation::UI.VFS)
    util_test_suite(Code/Foundation/UI.Viewport.Tests               Foundation::UI.Viewport)
    util_test_suite(Code/Foundation/VFS.Pak.Tests                   Foundation::VFS.Pak)
    util_test_suite(Code/Foundation/VFS.Tests                       Foundation::VFS)
    util_test_suite(Code/Foundation/VG.Renderer.Tests               Foundation::VG.Renderer)
    util_test_suite(Code/Foundation/VG.SVG.Tests                    Foundation::VG.SVG)
    util_test_suite(Code/Foundation/VG.Tests                        Foundation::VG)
    util_test_suite(Code/Foundation/Xml.Serialization.Tests         Foundation::Xml.Serialization)
    util_test_suite(Code/Foundation/Xml.Tests                       Foundation::Xml)
    util_test_suite(Code/Foundation/Json.Tests                     Foundation::Json)
    util_test_suite(Code/Foundation/Mcp.Tests                      Foundation::Mcp)
    util_test_suite(Code/Foundation/Mcp.Reflection.Tests           Foundation::Mcp.Reflection)
    util_test_suite(Code/Foundation/Mcp.Script.Tests               Foundation::Mcp.Script)
    util_test_suite(Code/Foundation/Mcp.Http.Tests                 Foundation::Mcp.Http)

    # Integration - cross-collection flow suites. Tiered behind the
    # Integration. prefix: the fast dev loop runs module suites only; review/pre-push include these.
    # JSON x scripting: the test file runs on angelscript + luau, so build it when either is on.
    if(OPTION_ENABLE_ANGELSCRIPT OR OPTION_ENABLE_LUAU)
        util_test_suite(Code/Integration/Integration.Script            Foundation::Script)
    endif()
    # Subsystem script FACADES (Net.*/Ui.* on angelscript + luau) - relocated here so the subsystem
    # unit suites stay backend-neutral. Each backend leg is #ifdef-guarded; build if either is on.
    if(OPTION_ENABLE_ANGELSCRIPT OR OPTION_ENABLE_LUAU)
        util_test_suite(Code/Integration/Integration.ScriptFacades     Engine::UI)
    endif()
    util_test_suite(Code/Integration/Integration.Mcp                Editor::Mcp)  # golden: create->open->inspect a project
    # Engine composition x pipeline registration: every factory description's cooked form has a builder.
    util_test_suite(Code/Integration/Integration.Composition        Pipeline::Registration)
    # Animation x physics in a running scene: root motion walking a character, which still collides.
    util_test_suite(Code/Integration/Integration.Runtime            Engine::Animation)
    # BC-texture cook-encoder x render: upload + sample block-compressed textures on real GPUs.
    util_test_suite(Code/Integration/Integration.TextureCompression Pipeline::Texture.Compression)
endif()

# Web-only samples, then stop - the rest of Code/Samples is desktop-only. Honor
# OPTION_BUILD_SAMPLES (both wasm presets default it OFF): a samples-off wasm build
# must not build the samples NOR inherit WebScene's native shader-cook tool requirement.
if(EMSCRIPTEN)
    if(OPTION_BUILD_SAMPLES)
        add_subdirectory(Code/Samples/WebTriangle)  # WebShell + web runner + WebGPU factory -> WebTriangle.html
        add_subdirectory(Code/Samples/WebScene)     # DefaultApplication + a lit PBR scene -> WebScene.html
        if(OPTION_EXTENSION_IMGUI)
            add_subdirectory(Code/Samples/TerrainPlayground) # in-code heightfield -> engine.terrain -> TerrainPlayground.html
        endif()
    endif()
    # Game native code (game-native-code.md N5): the wasm lane hosts the game too - the
    # web SHIP player compiles the module in (no dlopen in browsers). Mirrors the
    # non-Emscripten block at the file end (this branch return()s before reaching it).
    if(ENGINE_GAME_NATIVE_DIR)
        add_subdirectory(${ENGINE_GAME_NATIVE_DIR} game-native)
    endif()
    return()
endif()

if(OPTION_BUILD_SAMPLES)
    add_subdirectory(Code/Samples)
    # The experimental GUI's sandbox lives beside the framework it shows; it draws through the
    # samples' framework, so it joins after it.
    if(OPTION_EXPERIMENTAL_GUI AND TARGET Samples::Framework)
        add_subdirectory(Code/Experimental/GUI.Sandbox) # experimental.gui widgets + CSS on the VG renderer
    endif()
endif()

# Game native code (game-native-code.md N3): the game's native directory joins the
# build AFTER every engine target exists, so it can link any of them.
if(ENGINE_GAME_NATIVE_DIR)
    add_subdirectory(${ENGINE_GAME_NATIVE_DIR} game-native)
endif()
