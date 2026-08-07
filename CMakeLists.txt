# Draconic - game engine root build
#
# C++23, C++ modules. Requires CMake >= 3.28 and a modules-capable toolchain
# (Clang 17+, GCC 14+, or MSVC 19.36+) with the Ninja generator.

cmake_minimum_required(VERSION 3.28)

project(Draconic
    VERSION 0.1.0
    DESCRIPTION "Draconic game engine"
    LANGUAGES CXX ASM)

# --- Global language settings ------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Position-independent code so the static engine libraries can be linked into
# dynamically-loaded plugin shared objects (Draconic::Runtime plugin host).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

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
# project that lived beside the binaries). Set DRACONIC_OUTPUT_SUFFIX (e.g. "-ASAN") in
# sanitizer configure runs.
set(DRACONIC_OUTPUT_SUFFIX "" CACHE STRING "Suffix for the Bin output dir (e.g. -ASAN)")
set(DRACONIC_BIN_DIR "${CMAKE_SOURCE_DIR}/Bin/$<CONFIG>/${_dr_plat}-${_dr_comp}${DRACONIC_OUTPUT_SUFFIX}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY     "${DRACONIC_BIN_DIR}")       # exes + Windows DLLs
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY     "${DRACONIC_BIN_DIR}")       # .so plugins/modules (beside the exe)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY     "${DRACONIC_BIN_DIR}/lib")   # static .a/.lib + MSVC import libs
set(CMAKE_PDB_OUTPUT_DIRECTORY         "${DRACONIC_BIN_DIR}")       # MSVC linker PDBs (beside the exe)
set(CMAKE_COMPILE_PDB_OUTPUT_DIRECTORY "${DRACONIC_BIN_DIR}/lib")   # MSVC static-lib compile PDBs

# Reusable helpers (draconic_copy_runtime_deps, ...).
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/DraconicHelpers.cmake)

# Add a module's flat-sibling doctest suite, but only when the module target was actually built -
# so the web subset and platform-gated backends (Shell.Desktop, ...) skip cleanly; a test never
# outlives its module. The whole cluster is wrapped once in if(DRACONIC_BUILD_TESTS) at the call site.
macro(draconic_test_suite dir target)
    if(TARGET ${target})
        add_subdirectory(${dir})
    endif()
endmacro()

# --- Options -----------------------------------------------------------------
option(DRACONIC_BUILD_TESTS "Build Draconic unit tests" ON)
option(DRACONIC_BUILD_SAMPLES "Build Draconic sample applications" ON)
option(DRACONIC_BUILD_TOOLS "Build editor/authoring tooling (the editor libraries + CLI exes)" ON)
# Experimental modules and optional extensions are independently switchable; each exposes a
# DRACONIC_HAS_* compile define (see the policy block) so C++ can guard usage with #if.
option(DRACONIC_EXPERIMENTAL_GUI "Build the experimental GUI framework (draconic.gui + shell/vfs)" ON)
option(DRACONIC_EXTENSION_IMGUI  "Build the Dear ImGui debug-UI extension (draconic.imgui)" ON)
option(DRACONIC_WARNINGS_AS_ERRORS "Treat warnings as errors" ON)

# A web (Emscripten) build ships a game, not the authoring/side toolchain: force the tooling,
# experimental, and extension groups off up front, before the policy defines below capture them.
if(EMSCRIPTEN)
    set(DRACONIC_BUILD_TOOLS OFF)
    set(DRACONIC_EXPERIMENTAL_GUI OFF)
    # DRACONIC_EXTENSION_IMGUI stays available on web: Dear ImGui + draconic.imgui compile for
    # wasm (the renderer is pure RHI, the subsystem consumes the shell's input), so the debug
    # panel works in the browser too (WebScene's tweak UI).
endif()

# --- Product binary basenames: single source of truth ------------------------
# A few executables are referenced by name/path from OUTSIDE their own target, so the name cannot
# live only in the target definition:
#   - the export host-template stages the player and reads "<player>.runtime-libs" beside the tool
#     (Editor.Core gets the name below as the DRACONIC_PLAYER_BASENAME compile define)
#   - the web build invokes the NATIVE shader-pack cook by PATH (cross-config: the wasm build reaches
#     into the desktop Bin dir, so it cannot use $<TARGET_FILE:>) - Player/WebScene read this name
# Change a name here and every reference (OUTPUT_NAME, the C++ constant, the sidecar, the cook path)
# follows. Dotted, matching the target names.
set(DRACONIC_PLAYER_BASENAME         "Draconic.Engine.Player")
set(DRACONIC_SHADERPACKCOOK_BASENAME "Draconic.Tools.ShaderPack")

# Where the DESKTOP build put its executables, as seen from a CROSS build (wasm). The web
# build shells out to native cook tools by path - it cannot use $<TARGET_FILE:> because those
# targets do not exist in the wasm configure - so it needs the host's Bin dir. Derived from
# CMAKE_HOST_* (CMAKE_SYSTEM_NAME is "Emscripten" here, so the _dr_plat above would be wrong),
# and the exe suffix is the HOST's - CMAKE_EXECUTABLE_SUFFIX is ".js" under Emscripten.
# Assumes the desktop build used Clang (the documented dev toolchain); override the cache
# variable for a GCC/MSVC host build or a non-default Bin location.
if(CMAKE_HOST_WIN32)
    set(_dr_host_plat "Win64")
    set(DRACONIC_HOST_EXE_SUFFIX ".exe")
elseif(CMAKE_HOST_APPLE)
    set(_dr_host_plat "Mac64")
    set(DRACONIC_HOST_EXE_SUFFIX "")
else()
    set(_dr_host_plat "Linux64")
    set(DRACONIC_HOST_EXE_SUFFIX "")
endif()
set(DRACONIC_HOST_PLATFORM "${_dr_host_plat}" CACHE STRING
    "Host platform folder name under Bin/<config>/ (cross builds locate native tools there)")
set(DRACONIC_HOST_BIN_DIR "${CMAKE_SOURCE_DIR}/Bin/Debug/${DRACONIC_HOST_PLATFORM}-Clang"
    CACHE PATH "Desktop Bin dir holding the native cook tools a cross (web) build shells out to")

# Enable testing before any subdirectory so add_test() calls made by modules that
# register their own Tests/ (e.g. Runtime, Script/Wren) are picked up by ctest.
if(DRACONIC_BUILD_TESTS)
    enable_testing()
endif()

# --- Engine-wide compile policy ----------------------------------------------
# Exceptions and C++ RTTI are disabled engine-wide (see Documentation/Planning/Core.md §1).
add_library(draconic_policy INTERFACE)
add_library(Draconic::Policy ALIAS draconic_policy)

target_compile_features(draconic_policy INTERFACE cxx_std_23)

# Shipping flavor (Prelude: "define exactly one" of DEBUG/RELEASE/SHIPPING): an optimized
# build with asserts compiled out + stripped binaries - the dist/size-measurement config.
# Presets clang-shipping/gcc-shipping turn it on (Release + DRACONIC_OUTPUT_SUFFIX so it
# never clobbers a plain Release Bin dir).
option(DRACONIC_SHIPPING_BUILD "Shipping config: asserts compiled out, binaries stripped" OFF)

target_compile_definitions(draconic_policy INTERFACE
    $<$<CONFIG:Debug>:DRACONIC_DEBUG=1>
    $<$<AND:$<CONFIG:Release,RelWithDebInfo,MinSizeRel>,$<NOT:$<BOOL:${DRACONIC_SHIPPING_BUILD}>>>:DRACONIC_RELEASE=1>
    $<$<BOOL:${DRACONIC_SHIPPING_BUILD}>:DRACONIC_SHIPPING=1>
    # Instrumentation on in Debug/RelWithDebInfo; off (compiles to nothing) in shipping builds.
    $<$<CONFIG:Debug,RelWithDebInfo>:DRACONIC_PROFILING=1>
    # Build identity as strings, matching the Bin/<Config>/<Platform>-<Compiler> layout: the export
    # host template stamps itself with the config/compiler that built the running tool (export-templates.md).
    DRACONIC_BUILD_CONFIG=\"$<CONFIG>\"
    DRACONIC_BUILD_COMPILER=\"${_dr_comp}\"
    # Optional-module presence flags (0/1) - guard usage in C++ with e.g.
    # `#if DRACONIC_HAS_EXTENSION_IMGUI` / `#if DRACONIC_HAS_EXPERIMENTAL_GUI`.
    DRACONIC_HAS_EXPERIMENTAL_GUI=$<BOOL:${DRACONIC_EXPERIMENTAL_GUI}>
    DRACONIC_HAS_EXTENSION_IMGUI=$<BOOL:${DRACONIC_EXTENSION_IMGUI}>)

target_link_options(draconic_policy INTERFACE
    # Shipping: strip symbol tables at link (what a dist ships; the honest size number).
    $<$<AND:$<BOOL:${DRACONIC_SHIPPING_BUILD}>,$<CXX_COMPILER_ID:Clang,GNU>>:-s>)

target_compile_options(draconic_policy INTERFACE
    $<$<CXX_COMPILER_ID:Clang,GNU>:-fno-exceptions;-fno-rtti>
    # GCC modules: eager gcm loading. The default lazy loader intermittently fails to merge
    # template pendings across module imports ("failed to read compiled module cluster" /
    # "failed to load pendings for ...") - deterministic in DraconicCook's import mix on GCC 15.2.
    $<$<CXX_COMPILER_ID:GNU>:-fno-module-lazy>
    $<$<CXX_COMPILER_ID:MSVC>:/EHs-c-;/GR->)

# Warnings
target_compile_options(draconic_policy INTERFACE
    $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall;-Wextra;-Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>)

if(DRACONIC_WARNINGS_AS_ERRORS)
    target_compile_options(draconic_policy INTERFACE
        $<$<CXX_COMPILER_ID:Clang,GNU>:-Werror>
        $<$<CXX_COMPILER_ID:MSVC>:/WX>)
    # Clang on Windows emits a spurious "argument unused during compilation: '-c'"
    # during the CMake module-dependency scan step. Suppress just that diagnostic
    # so -Werror doesn't kill the build.
    # Clang 22+ warns that __COUNTER__ is a "C2y extension" when used in C++ mode;
    # doctest (and other widely-used macros) rely on it, so suppress the diagnostic.
    target_compile_options(draconic_policy INTERFACE
        $<$<CXX_COMPILER_ID:Clang>:-Wno-error=unused-command-line-argument;-Wno-c2y-extensions>)
    # em++ re-invokes clang++ for the module precompile and does NOT forward
    # -Wno-error=unused-command-line-argument to that inner driver, so the same spurious
    # "'-c' unused" diagnostic is fatal again on the wasm build. Suppress it outright here
    # (it is never actionable: CMake, not us, passes both --precompile and -c).
    if(EMSCRIPTEN)
        target_compile_options(draconic_policy INTERFACE -Wno-unused-command-line-argument)
    endif()
endif()

# --- Include roots -----------------------------------------------------------
# Role-grouped reorg: modules live at Code/Draconic/<Role>/Draconic.<Name> and expose their
# path-prefixed public headers as "Draconic.<Name>/...". The ROLE dirs are the global include
# roots so a header resolves by its dotted name regardless of which role owns it (moving a module
# between roles never touches an #include). Code/Draconic stays a root during the migration so
# not-yet-moved modules keep their legacy "Runtime/..."/"Shell/..." prefixes until their role lands.
include_directories(
    ${CMAKE_SOURCE_DIR}/Code/Draconic            # transitional: unmigrated modules' legacy prefixes
    ${CMAKE_SOURCE_DIR}/Code/Draconic/Foundation
    ${CMAKE_SOURCE_DIR}/Code/Draconic/Engine
    ${CMAKE_SOURCE_DIR}/Code/Draconic/Editor
    ${CMAKE_SOURCE_DIR}/Code/Draconic/Tools
    ${CMAKE_SOURCE_DIR}/Code/Draconic/Experimental
    ${CMAKE_SOURCE_DIR}/Code/Draconic/Extensions)

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
add_subdirectory(Code/Draconic/Foundation/Draconic.Foundation)
add_subdirectory(Code/Draconic/Foundation/Draconic.Settings)  # typed/versioned settings store over Foundation (runtime + editor)
add_subdirectory(Code/Draconic/Foundation/Draconic.Profiler)  # CPU scope profiler over Foundation (instrumented across the engine)
add_subdirectory(Code/Draconic/Foundation/Draconic.Net)       # real-time game networking: wire + INetTransport + loopback sim (over Foundation)
add_subdirectory(Code/Draconic/Foundation/Draconic.Net.Replication)  # StateReplication foundation: NetworkId + reflection-driven field codec (over Net + Foundation)
add_subdirectory(Code/Draconic/Foundation/Draconic.Xml)       # DOM XML parser + writer over Foundation
    add_subdirectory(Code/Draconic/Foundation/Draconic.Xml.Serialization)
add_subdirectory(Code/Draconic/Foundation/Draconic.VFS)       # byte access over Foundation stream primitives
    add_subdirectory(Code/Draconic/Foundation/Draconic.VFS.Pak)
add_subdirectory(Code/Draconic/Engine/Draconic.Engine.Project)   # project manifest/layout (runtime-side, editor-free)
add_subdirectory(Code/Draconic/Foundation/Draconic.Content)   # content database over Core + VFS
add_subdirectory(Code/Draconic/Foundation/Draconic.Resource)  # resource manager over content
add_subdirectory(Code/Draconic/Foundation/Draconic.RHI)   # depends only on Foundation (foundational sibling of Runtime)
# --- RHI backends (Null/Validation always; GPU backends by platform + availability) ---
add_subdirectory(Code/Draconic/Foundation/Draconic.RHI.Null)
add_subdirectory(Code/Draconic/Foundation/Draconic.RHI.Validation)
find_package(Vulkan QUIET GLOBAL)
if(Vulkan_FOUND)
    add_subdirectory(Code/Draconic/Foundation/Draconic.RHI.Vulkan)
else()
    message(STATUS "Vulkan not found - skipping Draconic::RHI.Vulkan")
endif()
if(WIN32)
    add_subdirectory(Code/Draconic/Foundation/Draconic.RHI.DX12)
endif()
if(EMSCRIPTEN OR TARGET Draconic::WgpuHeaders)
    add_subdirectory(Code/Draconic/Foundation/Draconic.RHI.WebGPU)
else()
    message(STATUS "Skipping Draconic::RHI.WebGPU (no wgpu headers)")
endif()
add_subdirectory(Code/Draconic/Foundation/Draconic.RenderGraph)  # render graph over RHI
add_subdirectory(Code/Draconic/Foundation/Draconic.Shaders)
add_subdirectory(Code/Draconic/Foundation/Draconic.Shaders.System)
add_subdirectory(Code/Draconic/Foundation/Draconic.Shaders.Resource)
add_subdirectory(Code/Draconic/Foundation/Draconic.Image)
add_subdirectory(Code/Draconic/Foundation/Draconic.Image.IO)
add_subdirectory(Code/Draconic/Foundation/Draconic.Image.Resource)
add_subdirectory(Code/Draconic/Foundation/Draconic.Model)
    add_subdirectory(Code/Draconic/Foundation/Draconic.Model.IO)
    add_subdirectory(Code/Draconic/Foundation/Draconic.Model.GLTF)
    add_subdirectory(Code/Draconic/Foundation/Draconic.Model.FBX)
add_subdirectory(Code/Draconic/Foundation/Draconic.Geometry)  # engine runtime mesh format (StaticMesh/SkinnedMesh + primitives)
add_subdirectory(Code/Draconic/Foundation/Draconic.Geometry.Resource)
add_subdirectory(Code/Draconic/Foundation/Draconic.Animation) # skeletal-animation foundation (Sedulous.Animation port; Core-only)
add_subdirectory(Code/Draconic/Foundation/Draconic.Animation.Resource)
add_subdirectory(Code/Draconic/Foundation/Draconic.Particles) # CPU particle runtime (Sedulous.Particles port; Core-only)
add_subdirectory(Code/Draconic/Foundation/Draconic.Particles.Resource)
add_subdirectory(Code/Draconic/Foundation/Draconic.Physics)   # Jolt-backed rigid-body core (world/bodies/queries/events; JPH never public)
add_subdirectory(Code/Draconic/Foundation/Draconic.Physics.Resource)  # cooked collision shapes + physical materials
add_subdirectory(Code/Draconic/Foundation/Draconic.Audio)     # miniaudio-backed audio engine core (voices/buses/handles; ma_* never public)
add_subdirectory(Code/Draconic/Foundation/Draconic.Audio.Resource)    # cooked audio clips + factory
add_subdirectory(Code/Draconic/Foundation/Draconic.Scene)     # scene / ECS foundation (entities, transforms, components, SceneSubsystem)
add_subdirectory(Code/Draconic/Engine/Draconic.Engine.Scene)
add_subdirectory(Code/Draconic/Foundation/Draconic.Scene.Resource)
add_subdirectory(Code/Draconic/Foundation/Draconic.Render.Api) # light scene-rendering interface (ISceneRenderer) - tools drive rendering w/o linking the renderer
add_subdirectory(Code/Draconic/Foundation/Draconic.Render)    # renderer: scene->render extraction + (later) the GPU forward path
add_subdirectory(Code/Draconic/Engine/Draconic.Engine.Render)
add_subdirectory(Code/Draconic/Engine/Draconic.Engine.Animation) # skeletal-animation scene integration (links RenderSubsystem; after Render)
add_subdirectory(Code/Draconic/Engine/Draconic.Engine.Particles) # particle scene/render integration (renderer + provider seam; after Render)
add_subdirectory(Code/Draconic/Engine/Draconic.Engine.Physics) # physics scene integration (components + per-scene Jolt world + interpolation; after Render)
add_subdirectory(Code/Draconic/Engine/Draconic.Engine.Audio)   # audio scene integration (components + per-scene voice group + listener sync; after Render)
add_subdirectory(Code/Draconic/Foundation/Draconic.Fonts)     # font types/interfaces over Foundation + Image
add_subdirectory(Code/Draconic/Foundation/Draconic.Fonts.Baked)
add_subdirectory(Code/Draconic/Foundation/Draconic.Fonts.IO)
add_subdirectory(Code/Draconic/Foundation/Draconic.Fonts.TTF)
add_subdirectory(Code/Draconic/Foundation/Draconic.Fonts.Importer)
add_subdirectory(Code/Draconic/Foundation/Draconic.Fonts.DistanceField)
add_subdirectory(Code/Draconic/Foundation/Draconic.Fonts.Resource)
if(TARGET Draconic::MsdfgenCore)  # DF baker (editor-time TTF -> baked); needs the vendored msdfgen
    add_subdirectory(Code/Draconic/Foundation/Draconic.Fonts.DistanceField.Baker)
endif()
add_subdirectory(Code/Draconic/Foundation/Draconic.Texture)   # texture descriptors + cooked resource/GPU factory + editor
add_subdirectory(Code/Draconic/Foundation/Draconic.Texture.Resource)
add_subdirectory(Code/Draconic/Foundation/Draconic.Materials) # data-driven material model (PipelineConfig + bind-group-inferring MaterialSystem)
add_subdirectory(Code/Draconic/Foundation/Draconic.Materials.PipelineCache)
add_subdirectory(Code/Draconic/Foundation/Draconic.Materials.Resource)
add_subdirectory(Code/Draconic/Foundation/Draconic.Model.Resource) # cooked model manifest + runtime composite (runtime side; no tooling deps)
add_subdirectory(Code/Draconic/Foundation/Draconic.UI.Resource)   # cooked UI documents + themes (game-ui; UI-framework-free records)
add_subdirectory(Code/Draconic/Engine/Draconic.Engine.UI)  # game screen tier: canvases + overlay render + consumption
add_subdirectory(Code/Draconic/Foundation/Draconic.VG)        # 2D vector-graphics core over Core + Image
add_subdirectory(Code/Draconic/Foundation/Draconic.VG.SVG)
add_subdirectory(Code/Draconic/Foundation/Draconic.VG.Renderer)
add_subdirectory(Code/Draconic/Foundation/Draconic.UI)        # retained-mode UI framework (faithful Sedulous.UI port) over Core (+ VG/Fonts as phases land)
add_subdirectory(Code/Draconic/Foundation/Draconic.UI.VFS)
add_subdirectory(Code/Draconic/Foundation/Draconic.UI.Shell)
add_subdirectory(Code/Draconic/Foundation/Draconic.UI.Toolkit)
add_subdirectory(Code/Draconic/Foundation/Draconic.UI.Viewport)
# --- UI desktop hosts (UIHost + docking workbench; link Graphics/Runtime.Client, desktop-only) ---
if(NOT EMSCRIPTEN)
    add_subdirectory(Code/Draconic/Foundation/Draconic.UI.Runtime)
    add_subdirectory(Code/Draconic/Foundation/Draconic.UI.Application)
endif()
if(DRACONIC_EXPERIMENTAL_GUI)
    add_subdirectory(Code/Draconic/Experimental/Draconic.GUI)   # fresh eepp-derived UI framework (docs/design/gui-port.md)
endif()
add_subdirectory(Code/Draconic/Foundation/Draconic.Script)
# --- Script backends (Wren always; AngelScript where the vendored lib built) ---
add_subdirectory(Code/Draconic/Foundation/Draconic.Script.Wren)
if(TARGET Draconic::AngelScript)
    add_subdirectory(Code/Draconic/Foundation/Draconic.Script.AngelScript)
endif()
add_subdirectory(Code/Draconic/Foundation/Draconic.Script.Resource)   # cooked script classes + factory (source + harvested metadata)
add_subdirectory(Code/Draconic/Foundation/Draconic.Script.Facades)    # curated behavior facades (Entity/Log/Time/Random) - shared by cook VM + runtime
add_subdirectory(Code/Draconic/Foundation/Draconic.Net.Manager)       # per-instance networked endpoint (NetworkManager) + Net script facade (over Net + Script)
add_subdirectory(Code/Draconic/Engine/Draconic.Engine.Net)     # once-per-context NetworkSubsystem (injects NetworkComponentManager into scenes)
add_subdirectory(Code/Draconic/Foundation/Draconic.Shell)     # raw platform/OS service (IShell: window/input/events) over Core; below Runtime
# --- Shell backends (Null always; browser shell on emscripten, SDL3 desktop otherwise) ---
add_subdirectory(Code/Draconic/Foundation/Draconic.Shell.Null)
if(EMSCRIPTEN)
    add_subdirectory(Code/Draconic/Foundation/Draconic.Shell.Web)
else()
    find_package(SDL3 QUIET GLOBAL)  # GLOBAL: consumers across roles (Shell.Desktop, its tests, runners) link SDL3::SDL3
    if(SDL3_FOUND)
        add_subdirectory(Code/Draconic/Foundation/Draconic.Shell.Desktop)
    else()
        message(STATUS "SDL3 not found - skipping Draconic::Shell.Desktop")
    endif()
endif()
add_subdirectory(Code/Draconic/Foundation/Draconic.Input)     # action-mapping layer (InputMap + ActionRuntime + asset/cooked) over Foundation + Shell
add_subdirectory(Code/Draconic/Foundation/Draconic.Input.Resource)
if(DRACONIC_EXPERIMENTAL_GUI)
    add_subdirectory(Code/Draconic/Experimental/Draconic.GUI.Shell)  # platform input bridge (Shell InputEvents -> GUI EventDispatcher)
    add_subdirectory(Code/Draconic/Experimental/Draconic.GUI.VFS)   # VFS-backed IResourceProvider (background-image: url())
endif()
add_subdirectory(Code/Draconic/Foundation/Draconic.Graphics)  # GraphicsDevice/RenderWindow render host over RHI+Shell; below Runtime
add_subdirectory(Code/Draconic/Foundation/Draconic.Runtime)
add_subdirectory(Code/Draconic/Foundation/Draconic.Runtime.Client)
add_subdirectory(Code/Draconic/Engine/Draconic.Engine.GameInstance)
add_subdirectory(Code/Draconic/Engine/Draconic.Engine.DefaultApp)
# --- Runtime runner backend (browser rAF loop vs blocking desktop loop) ---
if(EMSCRIPTEN)
    add_subdirectory(Code/Draconic/Foundation/Draconic.Runtime.Web)
else()
    add_subdirectory(Code/Draconic/Foundation/Draconic.Runtime.Desktop)
endif()
add_subdirectory(Code/Draconic/Engine/Draconic.Engine.Input)  # input runtime hookup (tick + device provider seam) over Runtime
add_subdirectory(Code/Draconic/Engine/Draconic.Engine.Script) # entity behaviors (ScriptComponent + the run's ONE gameplay script context) over Runtime
add_subdirectory(Code/Draconic/Engine/Draconic.Engine.Integration) # cross-subsystem composition helpers (physics-contact -> script bridge) over Physics + Script
if(DRACONIC_EXTENSION_IMGUI)
    add_subdirectory(Code/Draconic/Extensions/Draconic.Imgui) # Dear ImGui debug UI (RHI renderer + subsystem) over Runtime
endif()
add_subdirectory(Code/Draconic/Engine/Draconic.Engine.Player)        # DraconicPlayer generic game runner


# --- Authoring tooling: the editor libraries + the CLI executables that assemble them.
# Gated by DRACONIC_BUILD_TOOLS (forced OFF for web / runtime-only / shipping builds) so the
# whole editor/cook surface drops cleanly. The in-folder *.Editor sub-targets are gated the
# same way inside their Foundation libs; every consumer of them lives inside this guard. ---
if(DRACONIC_BUILD_TOOLS)
    add_subdirectory(Code/Draconic/Editor/Draconic.Editor)     # asset-pipeline authoring base (tooling)
    add_subdirectory(Code/Draconic/Editor/Draconic.Editor.Cook) # incremental cook driver + pipeline DB (tooling, headless)
    add_subdirectory(Code/Draconic/Editor/Draconic.Editor.Core) # headless editor domain: context/registries/commands/pages/project (tooling)
    add_subdirectory(Code/Draconic/Editor/Draconic.Physics.Editor)    # collision/material assets + builders (tooling)
    add_subdirectory(Code/Draconic/Editor/Draconic.Audio.Editor)      # audio clip asset/builder/importer (tooling)
    add_subdirectory(Code/Draconic/Editor/Draconic.UI.Editor)     # UI document/theme assets + validating builders (tooling)
    add_subdirectory(Code/Draconic/Editor/Draconic.ModelImporter) # top-level model import: model IR -> cooked geometry/material/texture/animation resources
    add_subdirectory(Code/Draconic/Editor/Draconic.Script.Editor)     # script class asset/builder/importer (tooling; neutral - resolves a per-language cook)
    add_subdirectory(Code/Draconic/Editor/Draconic.Script.Wren.Editor)        # Wren cook service (compile-check + `static properties` probe + starter)
    add_subdirectory(Code/Draconic/Editor/Draconic.Script.AngelScript.Editor) # AngelScript cook service (compile-check + handler scan + starter; property harvest deferred)
    add_subdirectory(Code/Draconic/Editor/Draconic.Editor.App) # editor UI shell (chrome + layout persistence + EditorApplication)
    add_subdirectory(Code/Draconic/Editor/Draconic.Editor.Input) # input-map editing page (editor exe only)
    add_subdirectory(Code/Draconic/Editor/Draconic.Editor.GameUI) # UI document page: text + live preview (editor exe only)
    add_subdirectory(Code/Draconic/Editor/Draconic.Editor.Audio)  # audio clip audition page: waveform + play (editor exe only)
    add_subdirectory(Code/Draconic/Editor/Draconic.Editor.Texture) # texture asset page: import settings + preview (editor exe only)
    add_subdirectory(Code/Draconic/Editor/Draconic.Editor.Image) # image asset page: inspect + color-space intent (editor exe only)
    add_subdirectory(Code/Draconic/Editor/Draconic.Editor.Generic) # fallback asset page: serialize-driven property form (editor exe only)
    add_subdirectory(Code/Draconic/Editor/Draconic.Editor.Script) # script behavior source page: edit + recook + inline errors (editor exe only)
    add_subdirectory(Code/Draconic/Editor/Draconic.Editor.Scene) # scene editor plugin (SceneEditorPage + RegisterSceneEditor)

    add_subdirectory(Code/Draconic/Editor/Draconic.Animation.Editor)
    add_subdirectory(Code/Draconic/Editor/Draconic.Geometry.Editor)
    add_subdirectory(Code/Draconic/Editor/Draconic.Image.Editor)
    add_subdirectory(Code/Draconic/Editor/Draconic.Input.Editor)
    add_subdirectory(Code/Draconic/Editor/Draconic.Materials.Editor)
    add_subdirectory(Code/Draconic/Editor/Draconic.Particles.Editor)
    add_subdirectory(Code/Draconic/Editor/Draconic.Scene.Editor)
    add_subdirectory(Code/Draconic/Editor/Draconic.Shaders.Editor)
    add_subdirectory(Code/Draconic/Editor/Draconic.Texture.Editor)
    if(TARGET Draconic::Fonts.DistanceField.Baker)
        add_subdirectory(Code/Draconic/Editor/Draconic.Fonts.Editor)
        add_subdirectory(Code/Draconic/Editor/Draconic.Editor.Fonts) # font asset page: bake intent + live atlas preview
    endif()

    add_subdirectory(Code/Draconic/Tools/Draconic.Tools.Editor)        # DraconicEditor executable (editor assembly)
    add_subdirectory(Code/Draconic/Tools/Draconic.Tools.Cook)          # DraconicCook CLI cooker (headless)
    add_subdirectory(Code/Draconic/Tools/Draconic.Tools.Export)        # DraconicExport dist packager
    add_subdirectory(Code/Draconic/Tools/Draconic.Tools.ShaderPack) # DraconicShaderPackCook: cook shaders.dpak (WGSL for web)
endif()

# --- Tests: every module's flat-sibling doctest suite (guarded by its module target so the
# web subset + platform-gated backends skip when the module was not built) ---
if(DRACONIC_BUILD_TESTS)
    draconic_test_suite(Code/Draconic/Editor/Draconic.Animation.Editor.Tests              Draconic::Animation.Editor)
    draconic_test_suite(Code/Draconic/Editor/Draconic.Audio.Editor.Tests                  Draconic::Audio.Editor)
    draconic_test_suite(Code/Draconic/Editor/Draconic.Editor.App.Tests                    Draconic::Editor.App)
    draconic_test_suite(Code/Draconic/Editor/Draconic.Editor.Audio.Tests                  Draconic::Editor.Audio)
    draconic_test_suite(Code/Draconic/Editor/Draconic.Editor.Cook.Tests                   Draconic::Editor.Cook)
    draconic_test_suite(Code/Draconic/Editor/Draconic.Editor.Core.Tests                   Draconic::Editor.Core)
    draconic_test_suite(Code/Draconic/Editor/Draconic.Editor.Generic.Tests                Draconic::Editor.Generic)
    draconic_test_suite(Code/Draconic/Editor/Draconic.Editor.Image.Tests                  Draconic::Editor.Image)
    draconic_test_suite(Code/Draconic/Editor/Draconic.Editor.Scene.Tests                  Draconic::Editor.Scene)
    draconic_test_suite(Code/Draconic/Editor/Draconic.Editor.Script.Tests                 Draconic::Editor.Script)
    draconic_test_suite(Code/Draconic/Editor/Draconic.Editor.Tests                        Draconic::Editor)
    draconic_test_suite(Code/Draconic/Editor/Draconic.Editor.Texture.Tests                Draconic::Editor.Texture)
    draconic_test_suite(Code/Draconic/Editor/Draconic.Geometry.Editor.Tests               Draconic::Geometry.Editor)
    draconic_test_suite(Code/Draconic/Editor/Draconic.Image.Editor.Tests                  Draconic::Image.Editor)
    draconic_test_suite(Code/Draconic/Editor/Draconic.Input.Editor.Tests                  Draconic::Input.Editor)
    draconic_test_suite(Code/Draconic/Editor/Draconic.Materials.Editor.Tests              Draconic::Materials.Editor)
    draconic_test_suite(Code/Draconic/Editor/Draconic.ModelImporter.Tests                 Draconic::ModelImporter)
    draconic_test_suite(Code/Draconic/Editor/Draconic.Particles.Editor.Tests              Draconic::Particles.Editor)
    draconic_test_suite(Code/Draconic/Editor/Draconic.Physics.Editor.Tests                Draconic::Physics.Editor)
    draconic_test_suite(Code/Draconic/Editor/Draconic.Scene.Editor.Tests                  Draconic::Scene.Editor)
    draconic_test_suite(Code/Draconic/Editor/Draconic.Script.AngelScript.Editor.Tests     Draconic::Script.AngelScript.Editor)
    draconic_test_suite(Code/Draconic/Editor/Draconic.Script.Editor.Tests                 Draconic::Script.Editor)
    draconic_test_suite(Code/Draconic/Editor/Draconic.Script.Wren.Editor.Tests            Draconic::Script.Wren.Editor)
    draconic_test_suite(Code/Draconic/Editor/Draconic.Shaders.Editor.Tests                Draconic::Shaders.Editor)
    draconic_test_suite(Code/Draconic/Editor/Draconic.Texture.Editor.Tests                Draconic::Texture.Editor)
    draconic_test_suite(Code/Draconic/Editor/Draconic.Fonts.Editor.Tests                  Draconic::Fonts.Editor)
    draconic_test_suite(Code/Draconic/Editor/Draconic.UI.Editor.Tests                     Draconic::UI.Editor)
    draconic_test_suite(Code/Draconic/Engine/Draconic.Engine.Animation.Tests              Draconic::Engine.Animation)
    draconic_test_suite(Code/Draconic/Engine/Draconic.Engine.Audio.Tests                  Draconic::Engine.Audio)
    draconic_test_suite(Code/Draconic/Engine/Draconic.Engine.GameInstance.Tests Draconic::Script.AngelScript)  # links the AngelScript backend; builds wherever it exists (incl. web now)
    draconic_test_suite(Code/Draconic/Engine/Draconic.Engine.Integration.Tests            Draconic::Engine.Integration)
    draconic_test_suite(Code/Draconic/Engine/Draconic.Engine.Net.Tests                    Draconic::Engine.Net)
    draconic_test_suite(Code/Draconic/Engine/Draconic.Engine.Physics.Tests                Draconic::Engine.Physics)
    draconic_test_suite(Code/Draconic/Engine/Draconic.Engine.Render.Tests                 Draconic::Engine.Render)
    draconic_test_suite(Code/Draconic/Engine/Draconic.Engine.Scene.Tests                  Draconic::Engine.Scene)
    draconic_test_suite(Code/Draconic/Engine/Draconic.Engine.Script.Tests Draconic::Script.AngelScript)  # links the AngelScript backend; builds wherever it exists (incl. web now)
    draconic_test_suite(Code/Draconic/Engine/Draconic.Engine.UI.Tests                     Draconic::Engine.UI)
    draconic_test_suite(Code/Draconic/Experimental/Draconic.GUI.Shell.Tests               Draconic::GUI.Shell)
    draconic_test_suite(Code/Draconic/Experimental/Draconic.GUI.Tests                     Draconic::GUI)
    draconic_test_suite(Code/Draconic/Experimental/Draconic.GUI.VFS.Tests                 Draconic::GUI.VFS)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Animation.Resource.Tests        Draconic::Animation.Resource)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Animation.Tests                 Draconic::Animation)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Audio.Tests                     Draconic::Audio)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Content.Tests                   Draconic::Content)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Foundation.Tests                Draconic::Foundation)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Fonts.Baked.Tests               Draconic::Fonts.Baked)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Fonts.DistanceField.Baker.Tests Draconic::Fonts.DistanceField.Baker)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Fonts.DistanceField.Tests       Draconic::Fonts.DistanceField)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Fonts.IO.Tests                  Draconic::Fonts.IO)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Fonts.Importer.Tests            Draconic::Fonts.Importer)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Fonts.TTF.Tests                 Draconic::Fonts.TTF)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Fonts.Tests                     Draconic::Fonts)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Geometry.Resource.Tests         Draconic::Geometry.Resource)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Geometry.Tests                  Draconic::Geometry)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Graphics.Tests                  Draconic::Graphics)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Image.Tests                     Draconic::Image)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Input.Tests                     Draconic::Input)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Materials.PipelineCache.Tests   Draconic::Materials.PipelineCache)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Materials.Resource.Tests        Draconic::Materials.Resource)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Materials.Tests                 Draconic::Materials)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Model.Tests                     Draconic::Model)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Net.Manager.Tests Draconic::Script.AngelScript)  # links the AngelScript backend; builds wherever it exists (incl. web now)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Net.Replication.Tests           Draconic::Net.Replication)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Net.Tests                       Draconic::Net)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Particles.Resource.Tests        Draconic::Particles.Resource)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Particles.Tests                 Draconic::Particles)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Physics.Tests                   Draconic::Physics)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Profiler.Tests                  Draconic::Profiler)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.RHI.Null.Tests                  Draconic::RHI.Null)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.RHI.Validation.Tests            Draconic::RHI.Validation)
    if(NOT EMSCRIPTEN)  # spins up a real device; no headless web path yet
        draconic_test_suite(Code/Draconic/Foundation/Draconic.RHI.WebGPU.Tests Draconic::RHI.WebGPU)
        if(TARGET Draconic::RHI.Vulkan AND TARGET Draconic::RHI.WebGPU)
            # Cross-backend pixel probe (needs BOTH backends compiled; GPUs checked at runtime).
            add_subdirectory(Code/Draconic/Foundation/Draconic.Render.Backend.Tests)
            add_subdirectory(Code/Draconic/Foundation/Draconic.VG.Backend.Tests)
        endif()
    endif()
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Render.Tests                    Draconic::Render)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.RenderGraph.Tests               Draconic::RenderGraph)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Resource.Tests                  Draconic::Resource)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Runtime.Client.Tests            Draconic::Runtime.Client)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Runtime.Tests                   Draconic::Runtime)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Scene.Resource.Tests            Draconic::Scene.Resource)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Scene.Tests                     Draconic::Scene)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Script.AngelScript.Tests        Draconic::Script.AngelScript)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Script.Resource.Tests           Draconic::Script.Resource)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Script.Tests                    Draconic::Script)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Script.Wren.Tests               Draconic::Script.Wren)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Settings.Tests                  Draconic::Settings)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Shaders.Resource.Tests          Draconic::Shaders.Resource)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Shaders.System.Tests            Draconic::Shaders.System)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Shaders.Tests                   Draconic::Shaders)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Shell.Desktop.Tests             Draconic::Shell.Desktop)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Shell.Null.Tests                Draconic::Shell.Null)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Texture.Resource.Tests          Draconic::Texture.Resource)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Fonts.Resource.Tests            Draconic::Fonts.Resource)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Texture.Tests                   Draconic::Texture)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.UI.Shell.Tests                  Draconic::UI.Shell)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.UI.Tests                        Draconic::UI)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.UI.Toolkit.Tests                Draconic::UI.Toolkit)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.UI.VFS.Tests                    Draconic::UI.VFS)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.UI.Viewport.Tests               Draconic::UI.Viewport)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.VFS.Pak.Tests                   Draconic::VFS.Pak)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.VFS.Tests                       Draconic::VFS)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.VG.Renderer.Tests               Draconic::VG.Renderer)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.VG.SVG.Tests                    Draconic::VG.SVG)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.VG.Tests                        Draconic::VG)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Xml.Serialization.Tests         Draconic::Xml.Serialization)
    draconic_test_suite(Code/Draconic/Foundation/Draconic.Xml.Tests                       Draconic::Xml)
endif()

# Web-only samples, then stop - the rest of Code/Samples is desktop-only. Honor
# DRACONIC_BUILD_SAMPLES (both wasm presets default it OFF): a samples-off wasm build
# must not build the samples NOR inherit WebScene's native shader-cook tool requirement.
if(EMSCRIPTEN)
    if(DRACONIC_BUILD_SAMPLES)
        add_subdirectory(Code/Samples/WebTriangle)  # WebShell + web runner + WebGPU factory -> WebTriangle.html
        add_subdirectory(Code/Samples/WebScene)     # DefaultApplication + a lit PBR scene -> WebScene.html
    endif()
    return()
endif()

if(DRACONIC_BUILD_SAMPLES)
    add_subdirectory(Code/Samples)
endif()
