# WebScene - the FULL-RENDERER exercise scene (WebSceneApp.h), built for BOTH targets:
#   - desktop (Main.cpp): `WebScene --vulkan` vs `--webgpu` compares the same scene per backend,
#     and OPTION_USE_SHADER_PACK=1 ENV_WEBGPU_WGSL=1 runs the browser's WGSL shader path
#     on wgpu-native - the fast local repro for web-render bugs.
#   - web (WebMain.cpp): the same app on WebShell + the rAF runner, bundling a cooked WGSL
#     shaders.dpak (no compiler in the browser). ASYNCIFY for the synchronous GPU waits.
# The ImGui tweak panel links when the extension option is on (both targets - the web build
# climbed the extension too).

set(_webscene_common_libs
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
    Foundation::Geometry
    Foundation::Navigation
    Foundation::Navigation.Resource
    Engine::Navigation
    Foundation::Materials
    Foundation::Particles
    Engine::Particles
    Foundation::GUI
    Foundation::GUI.Resources
    Engine::GUI
    Foundation::Policy)

if(EMSCRIPTEN)

    add_executable(WebScene WebMain.cpp)
    target_link_libraries(WebScene PRIVATE
        ${_webscene_common_libs}
        Foundation::Shell.Web
        Foundation::Runtime.Web)
    if(OPTION_EXTENSION_IMGUI)
        target_link_libraries(WebScene PRIVATE Extensions::Imgui)
    endif()

    # Cook the WGSL shader pack with the NATIVE tool (the browser has no compiler) and bundle it
    # into the virtual FS at /Data/Engine/Shaders/shaders.dpak, where the shader host finds it. Tools.ShaderPack
    # must be built first by any desktop preset (a native authoring tool, never built for wasm).
    set(_web_shader_pack "${CMAKE_CURRENT_BINARY_DIR}/shaders.dpak")
    set(_cook_tool
        "${BUILDSYSTEM_HOST_BIN_DIR}/${BUILDSYSTEM_SHADERPACKCOOK_BASENAME}${BUILDSYSTEM_HOST_EXE_SUFFIX}")
    file(GLOB _engine_shader_sources CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/Data/Engine/Shaders/*.hlsl" "${CMAKE_SOURCE_DIR}/Data/Engine/Shaders/*.hlsli")
    add_custom_command(
        OUTPUT "${_web_shader_pack}"
        COMMAND "${_cook_tool}" "${CMAKE_SOURCE_DIR}/Data/Engine/Shaders" "${_web_shader_pack}" wgsl
        DEPENDS ${_engine_shader_sources} "${_cook_tool}"
        COMMENT "WebScene: cooking WGSL shaders.dpak (native Tools.ShaderPack)"
        VERBATIM)
    add_custom_target(WebSceneShaderPack DEPENDS "${_web_shader_pack}")
    add_dependencies(WebScene WebSceneShaderPack)

    # Full-window canvas: a custom HTML shell sizes the <canvas> to the viewport (the default
    # Emscripten shell is a small fixed canvas). Relinks when the shell changes.
    set(_web_shell "${CMAKE_CURRENT_SOURCE_DIR}/shell.html")
    set_source_files_properties(WebMain.cpp PROPERTIES OBJECT_DEPENDS "${_web_shell}")

    target_link_options(WebScene PRIVATE
        "--shell-file=${_web_shell}"
        "-sASYNCIFY"              # synchronous device init + per-frame GPU fence waits yield to the browser
        # ASYNCIFY unwinds the WHOLE C++ stack into this buffer on every emscripten_sleep. The full
        # renderer's GPU-completion pump (PumpUntilWithDevice) yields from deep inside the render frame
        # (app -> render subsystem -> render frame -> passes -> RHI -> WebGPU), which overflows the 4 KB
        # default and traps as "unreachable". 1 MB comfortably fits the deepest render path.
        "-sASYNCIFY_STACK_SIZE=1048576"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sASSERTIONS=1"
        "-lwebsocket.js"          # the browser WS client socket (JoinServer on web)
        # The data tree at the layout every executable discovers (Data/.dataroot at "/Data"):
        # the cooked WGSL pack under Data/Engine/Shaders, exactly where a dist stages it.
        "--preload-file=${CMAKE_SOURCE_DIR}/Data/.dataroot@Data/.dataroot"
        "--preload-file=${_web_shader_pack}@Data/Engine/Shaders/shaders.dpak"
        # The game-UI default font, at the exact dev-tree path the UI subsystem probes -
        # without it the HUD renders shapes but no text (the startup warning).
        "--preload-file=${CMAKE_SOURCE_DIR}/Data/Assets/fonts/roboto/Roboto-Regular.ttf@Data/Assets/fonts/roboto/Roboto-Regular.ttf")

    set_target_properties(WebScene PROPERTIES SUFFIX ".html")

else()

    add_executable(WebScene Main.cpp)
    target_link_libraries(WebScene PRIVATE
        ${_webscene_common_libs}
        Foundation::Shell.Desktop
        Foundation::Runtime.Desktop)
    if(OPTION_EXTENSION_IMGUI)
        target_link_libraries(WebScene PRIVATE Extensions::Imgui)
    endif()
    util_copy_runtime_deps(WebScene) # SDL3 is vendored static now; no DLL to stage

endif()
