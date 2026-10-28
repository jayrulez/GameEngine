# TerrainPlayground - the terrain showcase (TerrainPlaygroundApp.h), built for BOTH targets like
# WebScene:
#   - desktop (Main.cpp): `TerrainPlayground --vulkan` vs `--webgpu` runs the same terrain per
#     backend; OPTION_USE_SHADER_PACK=1 ENV_WEBGPU_WGSL=1 runs the browser's WGSL shader path on
#     wgpu-native - the fast local repro for web-render bugs.
#   - web (WebMain.cpp): the same app on WebShell + the rAF runner, bundling a cooked WGSL
#     shaders.dpak (no compiler in the browser). ASYNCIFY for the synchronous GPU waits.
# No pipeline: the terrain is an in-memory heightfield (Ref<> direct products), so this needs only
# the runtime host set + engine.terrain. The ImGui HUD is mandatory (gated in Samples/CMakeLists).

set(_terrain_common_libs
    Foundation::Core
    Foundation::RHI
    Foundation::Runtime
    Foundation::Runtime.Client
    Foundation::Graphics
    Foundation::Graphics.Gpu
    Engine::DefaultApp
    Foundation::Scene
    Engine::Scene
    Engine::Render
    Foundation::Render
    Extensions::Imgui
    Foundation::Geometry
    Foundation::Materials
    Foundation::Heightfield
    Foundation::Terrain.Resource
    Engine::Terrain
    Engine::Vegetation
    Foundation::Vegetation
    Foundation::Policy)

if(EMSCRIPTEN)

    add_executable(TerrainPlayground WebMain.cpp)
    target_link_libraries(TerrainPlayground PRIVATE
        ${_terrain_common_libs}
        Foundation::Shell.Web
        Foundation::Runtime.Web)

    # Cook the WGSL shader pack with the NATIVE tool (the browser has no compiler) and bundle it
    # into the virtual FS at /Data/Engine/Shaders/shaders.dpak, where the shader host finds it. Tools.ShaderPack must
    # be built first by any desktop preset (a native authoring tool, never built for wasm).
    set(_web_shader_pack "${CMAKE_CURRENT_BINARY_DIR}/shaders.dpak")
    set(_cook_tool
        "${BUILDSYSTEM_HOST_BIN_DIR}/${BUILDSYSTEM_SHADERPACKCOOK_BASENAME}${BUILDSYSTEM_HOST_EXE_SUFFIX}")
    file(GLOB _engine_shader_sources CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/Data/Engine/Shaders/*.hlsl" "${CMAKE_SOURCE_DIR}/Data/Engine/Shaders/*.hlsli")
    add_custom_command(
        OUTPUT "${_web_shader_pack}"
        COMMAND "${_cook_tool}" "${CMAKE_SOURCE_DIR}/Data/Engine/Shaders" "${_web_shader_pack}" wgsl
        DEPENDS ${_engine_shader_sources} "${_cook_tool}"
        COMMENT "TerrainPlayground: cooking WGSL shaders.dpak (native Tools.ShaderPack)"
        VERBATIM)
    add_custom_target(TerrainPlaygroundShaderPack DEPENDS "${_web_shader_pack}")
    add_dependencies(TerrainPlayground TerrainPlaygroundShaderPack)

    # Full-window canvas: a custom HTML shell sizes the <canvas> to the viewport (the default
    # Emscripten shell is a small fixed canvas). Relinks when the shell changes.
    set(_web_shell "${CMAKE_CURRENT_SOURCE_DIR}/shell.html")
    set_source_files_properties(WebMain.cpp PROPERTIES OBJECT_DEPENDS "${_web_shell}")

    target_link_options(TerrainPlayground PRIVATE
        "--shell-file=${_web_shell}"
        "-sASYNCIFY"              # synchronous device init + per-frame GPU fence waits yield to the browser
        # ASYNCIFY unwinds the WHOLE C++ stack into this buffer on every emscripten_sleep. The
        # render frame's GPU-completion pump yields from deep inside the render path, which overflows
        # the 4 KB default and traps as "unreachable". 1 MB comfortably fits the deepest render path.
        "-sASYNCIFY_STACK_SIZE=1048576"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sASSERTIONS=1"
        # The data tree at the layout every executable discovers (Data/.dataroot at "/Data"):
        # the cooked WGSL pack under Data/Engine/Shaders, exactly where a dist stages it.
        "--preload-file=${CMAKE_SOURCE_DIR}/Data/.dataroot@Data/.dataroot"
        "--preload-file=${_web_shader_pack}@Data/Engine/Shaders/shaders.dpak"
        # The ImGui HUD font, at the exact dev-tree path the subsystem probes.
        "--preload-file=${CMAKE_SOURCE_DIR}/Data/Engine/Fonts/roboto/Roboto-Regular.ttf@Data/Engine/Fonts/roboto/Roboto-Regular.ttf")

    set_target_properties(TerrainPlayground PROPERTIES SUFFIX ".html")

else()

    add_executable(TerrainPlayground Main.cpp)
    target_link_libraries(TerrainPlayground PRIVATE
        ${_terrain_common_libs}
        Foundation::Shell.Desktop
        Foundation::Runtime.Desktop)
    util_copy_sdl3_dll(TerrainPlayground)

endif()
