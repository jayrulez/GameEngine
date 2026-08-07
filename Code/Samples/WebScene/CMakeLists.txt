# WebScene - the FULL-RENDERER exercise scene (WebSceneApp.h), built for BOTH targets:
#   - desktop (Main.cpp): `WebScene --vulkan` vs `--webgpu` compares the same scene per backend,
#     and DRACONIC_USE_SHADER_PACK=1 DRACONIC_WEBGPU_WGSL=1 runs the browser's WGSL shader path
#     on wgpu-native - the fast local repro for web-render bugs.
#   - web (WebMain.cpp): the same app on WebShell + the rAF runner, bundling a cooked WGSL
#     shaders.dpak (no compiler in the browser). ASYNCIFY for the synchronous GPU waits.
# The ImGui tweak panel links when the extension option is on (both targets - the web build
# climbed the extension too).

set(_webscene_common_libs
    Draconic::Foundation
    Draconic::RHI
    Draconic::Runtime
    Draconic::Runtime.Client
    Draconic::Graphics
    Draconic::Graphics.Gpu
    Draconic::Engine.DefaultApp
    Draconic::Scene
    Draconic::Engine.Scene
    Draconic::Engine.Render
    Draconic::Geometry
    Draconic::Materials
    Draconic::Particles
    Draconic::Engine.Particles
    Draconic::UI
    Draconic::UI.Resource
    Draconic::Engine.UI
    Draconic::Policy)

if(EMSCRIPTEN)

    add_executable(WebScene WebMain.cpp)
    target_link_libraries(WebScene PRIVATE
        ${_webscene_common_libs}
        Draconic::Shell.Web
        Draconic::Runtime.Web)
    if(DRACONIC_EXTENSION_IMGUI)
        target_link_libraries(WebScene PRIVATE Draconic::Imgui)
    endif()

    # Cook the WGSL shader pack with the NATIVE tool (the browser has no compiler) and bundle it
    # into the virtual FS at /shaders.dpak, where the shader host finds it. Draconic.Tools.ShaderPack
    # must be built first by any desktop preset (a native authoring tool, never built for wasm).
    set(_web_shader_pack "${CMAKE_CURRENT_BINARY_DIR}/shaders.dpak")
    set(_cook_tool
        "${DRACONIC_HOST_BIN_DIR}/${DRACONIC_SHADERPACKCOOK_BASENAME}${DRACONIC_HOST_EXE_SUFFIX}")
    file(GLOB _engine_shader_sources CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/Data/Shaders/*.hlsl" "${CMAKE_SOURCE_DIR}/Data/Shaders/*.hlsli")
    add_custom_command(
        OUTPUT "${_web_shader_pack}"
        COMMAND "${_cook_tool}" "${CMAKE_SOURCE_DIR}/Data/Shaders" "${_web_shader_pack}" wgsl
        DEPENDS ${_engine_shader_sources} "${_cook_tool}"
        COMMENT "WebScene: cooking WGSL shaders.dpak (native Draconic.Tools.ShaderPack)"
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
        "--preload-file=${_web_shader_pack}@shaders.dpak"
        # The game-UI default font, at the exact dev-tree path the UI subsystem probes -
        # without it the HUD renders shapes but no text (the startup warning).
        "--preload-file=${CMAKE_SOURCE_DIR}/Data/Assets/fonts/roboto/Roboto-Regular.ttf@Data/Assets/fonts/roboto/Roboto-Regular.ttf")

    set_target_properties(WebScene PROPERTIES SUFFIX ".html")

else()

    add_executable(WebScene Main.cpp)
    target_link_libraries(WebScene PRIVATE
        ${_webscene_common_libs}
        Draconic::Shell.Desktop
        Draconic::Runtime.Desktop)
    if(DRACONIC_EXTENSION_IMGUI)
        target_link_libraries(WebScene PRIVATE Draconic::Imgui)
    endif()
    draconic_copy_runtime_deps(WebScene EXTRA ${DRACONIC_SDL3_DLL})

endif()
