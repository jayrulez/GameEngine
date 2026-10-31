# Engine.Player - the generic game runner (the engine, the project's content and its game script;
# no native game code required).
#
# One runner (PlayerApplication.h), two entry points: Main.cpp on desktop (SDL shell, a device,
# the blocking runner, CLI args) and WebMain.cpp in the browser (web shell, WebGPU, the rAF
# runner, the dist fetched into the virtual FS at startup). The engine, resource and script
# libraries are shared.

# The AssiduousEngine logo baked into every player as its SVG text (EmbeddedEngineLogo.h): the
# default loading screen shows it when a game names no loading document.
set(_engine_logo_svg "${CMAKE_SOURCE_DIR}/Data/Branding/assiduous-logo.svg")
set(_engine_logo_src "${CMAKE_CURRENT_BINARY_DIR}/EmbeddedEngineLogo.cpp")
add_custom_command(
    OUTPUT "${_engine_logo_src}"
    COMMAND ${CMAKE_COMMAND}
        -DIN=${_engine_logo_svg}
        -DOUT=${_engine_logo_src}
        -DSYMBOL=g_embeddedEngineLogo
        -P ${CMAKE_SOURCE_DIR}/cmake/EmbedBinary.cmake
    DEPENDS "${_engine_logo_svg}" "${CMAKE_SOURCE_DIR}/cmake/EmbedBinary.cmake"
    COMMENT "Embedding the engine logo")

if(EMSCRIPTEN)

    # --- Browser player: WebMain.cpp and the web platform trio ---
    # One link list, two players: Engine.Player is the reusable template (no game code);
    # Engine.GamePlayer (below, only when a game is wired in) compiles the game's native
    # module in. Browsers have no dlopen, so native code on the web exists only in a
    # shipping player.
    set(_web_player_libs
        Foundation::Core
        Foundation::Runtime Foundation::Runtime.Client Foundation::Runtime.Web Engine::DefaultApp
        Foundation::Shell.Web Foundation::Shell Foundation::Graphics Foundation::Graphics.Gpu
        Engine::Scene Foundation::Scene.Resource
        Engine::Render Engine::Animation Engine::Particles
        Foundation::Resource Foundation::Geometry.Resource Foundation::Materials.Resource Foundation::Texture.Resource
        Foundation::Animation.Resource Foundation::Particles.Resource Foundation::Image.Resource
        Foundation::Fonts Foundation::Fonts.Resource
        Foundation::Model.Resource
        Foundation::Script
        Foundation::Input Foundation::Input.Resource Engine::Input
        Foundation::Physics Foundation::Physics.Resource Engine::Physics
        Engine::Project Foundation::VFS.Pak Foundation::Content Foundation::VFS Foundation::Xml.Serialization)

    add_executable(Engine.Player WebMain.cpp "${_engine_logo_src}")
    target_link_libraries(Engine.Player PRIVATE ${_web_player_libs})

    # No content is baked into this binary: the player fetches player.xml and Content.pak (the
    # export output) and shaders.dpak (the export-cooked WGSL engine pack) from the serving folder
    # at startup (WebMain.cpp, synchronous under ASYNCIFY). That makes the build a reusable export
    # template (export any project, drop its files beside the player, serve, browse), and the web
    # build needs neither a native cook tool nor a pre-exported dist to configure.

    # Full-window canvas shell, the player's own (shipping-facing: <title>Player</title>, its
    # <canvas id=canvas> matches WebShell's default selector; DPR-aware resize drives the swapchain).
    set(_web_shell "${CMAKE_CURRENT_SOURCE_DIR}/shell.html")
    set_source_files_properties(WebMain.cpp PROPERTIES OBJECT_DEPENDS "${_web_shell}")
    target_link_options(Engine.Player PRIVATE
        "--shell-file=${_web_shell}"
        "-sASYNCIFY"               # device bring-up and per-frame GPU fence waits yield to the browser
        "-sASYNCIFY_STACK_SIZE=1048576" # the deep render-frame pump overflows the 4KB default
        "-lwebsocket.js"           # Net.WebSocket's INTERFACE option does not cross static
                                   # private deps ($<LINK_ONLY>), so restate it here
        "-lidbfs.js"               # saves outlive the page: IndexedDB under the user data directory
    )
    set_target_properties(Engine.Player PROPERTIES SUFFIX ".html")

    # The web shipping player: the game's native module statically compiled in.
    if(ENGINE_GAME_NATIVE_TARGET)
        add_executable(Engine.GamePlayer WebMain.cpp "${_engine_logo_src}")
        target_link_libraries(Engine.GamePlayer PRIVATE
            ${_web_player_libs} ${ENGINE_GAME_NATIVE_TARGET})
        target_compile_definitions(Engine.GamePlayer PRIVATE ENGINE_SHIP_NATIVE_GAME)
        target_link_options(Engine.GamePlayer PRIVATE
            "--shell-file=${_web_shell}"
            "-sASYNCIFY"
            "-sASYNCIFY_STACK_SIZE=1048576"
            "-lwebsocket.js"
            "-lidbfs.js")
        set_source_files_properties(WebMain.cpp PROPERTIES OBJECT_DEPENDS "${_web_shell}")
        # Its own basename (emscripten bakes the .js name into the .html and the .wasm name
        # into the .js, so the trio stays self-consistent as Engine.GamePlayer.*; it cannot
        # reuse the template's name, since both targets live in one build).
        set_target_properties(Engine.GamePlayer PROPERTIES SUFFIX ".html")
    endif()

    # Stage serve.py (a static server with COOP/COEP, the wasm MIME type and no-store) beside the output, and emit
    # the runtime-libs manifest the export template machinery reads: `--template create <this build
    # dir> --install` packages the player page and these sidecars as an importable "Web" template.
    set(_web_out_dir "$<TARGET_FILE_DIR:Engine.Player>")
    # The loading card's logo (shell.html) rides beside the page as well.
    add_custom_command(TARGET Engine.Player POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/serve.py" "${_web_out_dir}/serve.py"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${_engine_logo_svg}" "${_web_out_dir}/assiduous-logo.svg"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${BUILDSYSTEM_NOTICES_FILE}" "${_web_out_dir}/THIRD-PARTY-NOTICES.txt"
        COMMENT "Engine.Player(web): staging serve.py, the logo and the third-party notices"
        VERBATIM)
    file(GENERATE OUTPUT "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Engine.Player.runtime-libs"
         CONTENT "Engine.Player.js\nEngine.Player.wasm\nserve.py\nassiduous-logo.svg\nTHIRD-PARTY-NOTICES.txt\n")

else()

    # --- Desktop player: the launcher as a library ---
    # The whole boot is Engine.Player.Main, a static lib in every mode because it carries the
    # main logic, not engine state. The dev executable is a thin stub calling
    # PlayerMain(nullptr), and the exporter's generated shipping stub links this same library
    # with the game's static plugin. For that reason it is not a util_add_engine_library.
    add_library(Engine.Player.Main STATIC)
    target_sources(Engine.Player.Main
        PUBLIC FILE_SET player_main_modules TYPE CXX_MODULES FILES PlayerMain.cppm
        PRIVATE PlayerMain.cpp "${_engine_logo_src}")
    target_link_libraries(Engine.Player.Main PRIVATE
        Foundation::Core
        Foundation::Runtime Foundation::Runtime.Client Foundation::Runtime.Desktop Engine::DefaultApp
        Foundation::Shell.Desktop Foundation::Shell Foundation::Graphics Foundation::Graphics.Gpu
        Engine::Scene Foundation::Scene.Resource
        Engine::Render Engine::Animation Engine::Particles
        Foundation::Resource Foundation::Geometry.Resource Foundation::Materials.Resource Foundation::Texture.Resource
        Foundation::Animation.Resource Foundation::Particles.Resource Foundation::Image.Resource
        Foundation::Fonts Foundation::Fonts.Resource
        Foundation::Model.Resource
        Foundation::Script
        Foundation::Input Foundation::Input.Resource Engine::Input
        Foundation::Physics Foundation::Physics.Resource Engine::Physics
        Engine::Project Foundation::VFS.Pak Foundation::Content Foundation::VFS Foundation::Xml.Serialization)

    add_executable(Engine.Player Main.cpp)
    target_link_libraries(Engine.Player PRIVATE Engine.Player.Main Foundation::Policy)

    # The shipping player, only when a game's native directory is wired into this build
    # (ENGINE_GAME_NATIVE_DIR/_TARGET, which the exporter sets and which work by hand too).
    # Links the game's plugin statically through the same launcher; ShipMain's
    # CreatePlugin reference pulls the registration from the game archive.
    if(ENGINE_GAME_NATIVE_TARGET)
        add_executable(Engine.GamePlayer ShipMain.cpp)
        target_link_libraries(Engine.GamePlayer PRIVATE
            Engine.Player.Main ${ENGINE_GAME_NATIVE_TARGET} Foundation::Policy)
        util_copy_runtime_deps(Engine.GamePlayer EXTRA ${BUILDSYSTEM_WGPU_LIB} ${BUILDSYSTEM_NOTICES_FILE})
        if(UNIX AND NOT APPLE)
            set_property(TARGET Engine.GamePlayer APPEND PROPERTY BUILD_RPATH "$ORIGIN")
            set_property(TARGET Engine.GamePlayer PROPERTY INSTALL_RPATH "$ORIGIN")
        endif()
    endif()

    # Stage the player's runtime deps beside it and emit Engine.Player.runtime-libs (the export
    # host-template reads that list for its sidecars). SDL3 links statically in a static engine
    # build (no DLL to stage) and shared under ENGINE_SHARED_LIBS, where TARGET_RUNTIME_DLLS
    # stages it like any other link dependency.
    # wgpu-native is dlopen'd at runtime (--webgpu) and is not a link dependency, so it is staged
    # explicitly; on Linux, $ORIGIN on the player's RUNPATH lets the loader find it beside the exe
    # (Windows searches the exe dir for a bare DLL by default).
    # DXC is not staged: a player never compiles shaders. A dist renders from its cooked
    # shaders.dpak, and a dev player run from Bin/ finds the vendored compiler at
    # BUILDSYSTEM_DXC_PATH. Only the editor and the cook tools carry DXC (its glibc 2.38 floor would
    # also keep a template from ever running on an older system, the Steam Deck's).
    # The third-party notices ride along the same way (cmake/ThirdPartyNotices.cmake), so every
    # template and export carries them.
    util_copy_runtime_deps(Engine.Player EXTRA ${BUILDSYSTEM_WGPU_LIB} ${BUILDSYSTEM_NOTICES_FILE})
    if(UNIX AND NOT APPLE)
        set_property(TARGET Engine.Player APPEND PROPERTY BUILD_RPATH "$ORIGIN")
        set_property(TARGET Engine.Player PROPERTY INSTALL_RPATH "$ORIGIN")
    endif()

endif()

set_target_properties(Engine.Player PROPERTIES OUTPUT_NAME ${BUILTIN_PLAYER_BASENAME})
