# Draconic.Engine.Player - the generic game runner (engine + project content + game script; no native
# game code required). See docs/design/roadmap.md "Milestone: MVP-to-Export".
#
# One runner (PlayerApplication.h), two entry points: Main.cpp on desktop (SDL shell + a device +
# the blocking runner + CLI args) and WebMain.cpp in the browser (web shell + WebGPU + the rAF
# runner, dist PRELOADED into the virtual FS). The engine/resource/script libraries are shared.

if(EMSCRIPTEN)

    # --- Browser player: WebMain.cpp + the web platform trio + preloaded content ---
    add_executable(Draconic.Engine.Player WebMain.cpp)
    target_link_libraries(Draconic.Engine.Player PRIVATE
        Draconic::Foundation
        Draconic::Runtime Draconic::Runtime.Client Draconic::Runtime.Web Draconic::Engine.DefaultApp
        Draconic::Shell.Web Draconic::Shell Draconic::Graphics Draconic::Graphics.Gpu
        Draconic::Engine.Scene Draconic::Scene.Resource
        Draconic::Engine.Render Draconic::Engine.Animation Draconic::Engine.Particles
        Draconic::Resource Draconic::Geometry.Resource Draconic::Materials.Resource Draconic::Texture.Resource
        Draconic::Animation.Resource Draconic::Particles.Resource Draconic::Image.Resource
        Draconic::Fonts Draconic::Fonts.Resource
        Draconic::Model.Resource
        Draconic::Script
        Draconic::Input Draconic::Input.Resource Draconic::Engine.Input
        Draconic::UI.Resource Draconic::Engine.UI
        Draconic::Physics Draconic::Physics.Resource Draconic::Engine.Physics
        Draconic::Engine.Project Draconic::VFS.Pak Draconic::Content Draconic::VFS Draconic::Xml.Serialization)

    # NOTHING is baked into this binary: the player FETCHES player.xml + Content.pak (the export
    # output) and shaders.dpak (the export-cooked WGSL engine pack) from the serving folder at
    # startup (WebMain.cpp, synchronous under ASYNCIFY). That makes the build a reusable EXPORT
    # TEMPLATE - export any project, drop its files beside the player, serve, browse - and the
    # web build no longer needs a native cook tool or a pre-exported dist to CONFIGURE.

    # Full-window canvas shell, the player's own (shipping-facing: <title>Draconic Player</title>, its
    # <canvas id=canvas> matches WebShell's default selector; DPR-aware resize drives the swapchain).
    set(_web_shell "${CMAKE_CURRENT_SOURCE_DIR}/shell.html")
    set_source_files_properties(WebMain.cpp PROPERTIES OBJECT_DEPENDS "${_web_shell}")
    target_link_options(Draconic.Engine.Player PRIVATE
        "--shell-file=${_web_shell}"
        "-sASYNCIFY"               # device bring-up + per-frame GPU fence waits yield to the browser
        "-sASYNCIFY_STACK_SIZE=1048576" # the deep render-frame pump overflows the 4KB default
    )
    set_target_properties(Draconic.Engine.Player PROPERTIES SUFFIX ".html")

    # Stage serve.py (COOP/COEP + wasm MIME + no-store static server) beside the output, and emit
    # the runtime-libs manifest the export template machinery reads: `--template create <this build
    # dir> --install` packages the player page + these sidecars as an importable "Web" template.
    set(_web_out_dir "$<TARGET_FILE_DIR:Draconic.Engine.Player>")
    add_custom_command(TARGET Draconic.Engine.Player POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/serve.py" "${_web_out_dir}/serve.py"
        COMMENT "Draconic.Engine.Player(web): staging serve.py"
        VERBATIM)
    file(GENERATE OUTPUT "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Draconic.Engine.Player.runtime-libs"
         CONTENT "Draconic.Engine.Player.js\nDraconic.Engine.Player.wasm\nserve.py\n")

else()

    # --- Desktop player: Main.cpp + SDL shell + a device + the blocking runner ---
    add_executable(Draconic.Engine.Player Main.cpp)
    target_link_libraries(Draconic.Engine.Player PRIVATE
        Draconic::Foundation
        Draconic::Runtime Draconic::Runtime.Client Draconic::Runtime.Desktop Draconic::Engine.DefaultApp
        Draconic::Shell.Desktop Draconic::Shell Draconic::Graphics Draconic::Graphics.Gpu
        Draconic::Engine.Scene Draconic::Scene.Resource
        Draconic::Engine.Render Draconic::Engine.Animation Draconic::Engine.Particles
        Draconic::Resource Draconic::Geometry.Resource Draconic::Materials.Resource Draconic::Texture.Resource
        Draconic::Animation.Resource Draconic::Particles.Resource Draconic::Image.Resource
        Draconic::Fonts Draconic::Fonts.Resource
        Draconic::Model.Resource
        Draconic::Script
        Draconic::Input Draconic::Input.Resource Draconic::Engine.Input
        Draconic::UI.Resource Draconic::Engine.UI
        Draconic::Physics Draconic::Physics.Resource Draconic::Engine.Physics
        Draconic::Engine.Project Draconic::VFS.Pak Draconic::Content Draconic::VFS Draconic::Xml.Serialization)

    # Stage the player's runtime deps beside it + emit Draconic.Engine.Player.runtime-libs (the export
    # host-template reads that list for its sidecars). SDL3 rides in as EXTRA (no-op when
    # DRACONIC_SDL3_DLL is unset); DXC (libdxcompiler.so / dxcompiler.dll) is dlopen'd at runtime by
    # the renderer's shader compiler and is NOT a link dependency, so it must be staged explicitly -
    # without it a copied dist finds no DXC and the renderer stays inert. On Linux, $ORIGIN on the
    # player's RUNPATH lets the loader find the staged copy beside the exe (Windows searches the exe
    # dir for a bare DLL by default).
    draconic_copy_runtime_deps(Draconic.Engine.Player EXTRA ${DRACONIC_SDL3_DLL} ${DRACONIC_DXC_LIB})
    if(UNIX AND NOT APPLE)
        set_property(TARGET Draconic.Engine.Player APPEND PROPERTY BUILD_RPATH "$ORIGIN")
        set_property(TARGET Draconic.Engine.Player PROPERTY INSTALL_RPATH "$ORIGIN")
    endif()

endif()

set_target_properties(Draconic.Engine.Player PROPERTIES OUTPUT_NAME ${DRACONIC_PLAYER_BASENAME})
