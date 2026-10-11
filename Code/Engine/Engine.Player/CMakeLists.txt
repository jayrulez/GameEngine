# Engine.Player - the generic game runner (engine + project content + game script; no native
# game code required).
#
# One runner (PlayerApplication.h), two entry points: Main.cpp on desktop (SDL shell + a device +
# the blocking runner + CLI args) and WebMain.cpp in the browser (web shell + WebGPU + the rAF
# runner, dist PRELOADED into the virtual FS). The engine/resource/script libraries are shared.

# The AssiduousEngine logo baked into every player as its SVG text (EmbeddedEngineLogo.h): the
# default loading screen shows it when a game names no loading document.
set(_engine_logo_svg "${CMAKE_SOURCE_DIR}/Data/Assets/branding/assiduous-logo.svg")
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

    # --- Browser player: WebMain.cpp + the web platform trio + preloaded content ---
    # One link list, two players: Engine.Player is the reusable TEMPLATE (no game code);
    # Engine.GamePlayer (below, only when a game is wired in) compiles the game's native
    # module IN - browsers have no dlopen, so web native is ship-only by design
    # (game-native-code.md N5).
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
        Foundation::UI.Resource Engine::UI
        Foundation::Physics Foundation::Physics.Resource Engine::Physics
        Engine::Project Foundation::VFS.Pak Foundation::Content Foundation::VFS Foundation::Xml.Serialization)

    add_executable(Engine.Player WebMain.cpp "${_engine_logo_src}")
    target_link_libraries(Engine.Player PRIVATE ${_web_player_libs})

    # NOTHING is baked into this binary: the player FETCHES player.xml + Content.pak (the export
    # output) and shaders.dpak (the export-cooked WGSL engine pack) from the serving folder at
    # startup (WebMain.cpp, synchronous under ASYNCIFY). That makes the build a reusable EXPORT
    # TEMPLATE - export any project, drop its files beside the player, serve, browse - and the
    # web build no longer needs a native cook tool or a pre-exported dist to CONFIGURE.

    # Full-window canvas shell, the player's own (shipping-facing: <title>Player</title>, its
    # <canvas id=canvas> matches WebShell's default selector; DPR-aware resize drives the swapchain).
    set(_web_shell "${CMAKE_CURRENT_SOURCE_DIR}/shell.html")
    set_source_files_properties(WebMain.cpp PROPERTIES OBJECT_DEPENDS "${_web_shell}")
    target_link_options(Engine.Player PRIVATE
        "--shell-file=${_web_shell}"
        "-sASYNCIFY"               # device bring-up + per-frame GPU fence waits yield to the browser
        "-sASYNCIFY_STACK_SIZE=1048576" # the deep render-frame pump overflows the 4KB default
        "-lwebsocket.js"           # Net.WebSocket's INTERFACE option does not cross static
                                   # private deps ($<LINK_ONLY>) - restate it (WebScene precedent)
        "-lidbfs.js"               # saves outlive the page: IndexedDB under the user data directory
    )
    set_target_properties(Engine.Player PROPERTIES SUFFIX ".html")

    # The web SHIP player: the game's native module statically compiled in.
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
        # reuse the template's name - both targets live in one build).
        set_target_properties(Engine.GamePlayer PROPERTIES SUFFIX ".html")
    endif()

    # Stage serve.py (COOP/COEP + wasm MIME + no-store static server) beside the output, and emit
    # the runtime-libs manifest the export template machinery reads: `--template create <this build
    # dir> --install` packages the player page + these sidecars as an importable "Web" template.
    set(_web_out_dir "$<TARGET_FILE_DIR:Engine.Player>")
    # The loading card's logo (shell.html) rides beside the page as well.
    add_custom_command(TARGET Engine.Player POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/serve.py" "${_web_out_dir}/serve.py"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${_engine_logo_svg}" "${_web_out_dir}/assiduous-logo.svg"
        COMMENT "Engine.Player(web): staging serve.py and the logo"
        VERBATIM)
    file(GENERATE OUTPUT "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Engine.Player.runtime-libs"
         CONTENT "Engine.Player.js\nEngine.Player.wasm\nserve.py\nassiduous-logo.svg\n")

else()

    # --- Desktop player: launcher-as-library (game-native-code.md N1) ---
    # The whole boot is Engine.Player.Main (a STATIC lib in every mode - it carries main
    # logic, not engine state); the dev executable is a thin stub calling
    # PlayerMain(nullptr), and the exporter's generated SHIP stub links this same library
    # with the game's static plugin. Deliberately NOT util_add_engine_library.
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
        Foundation::UI.Resource Engine::UI
        Foundation::Physics Foundation::Physics.Resource Engine::Physics
        Engine::Project Foundation::VFS.Pak Foundation::Content Foundation::VFS Foundation::Xml.Serialization)

    add_executable(Engine.Player Main.cpp)
    target_link_libraries(Engine.Player PRIVATE Engine.Player.Main Foundation::Policy)

    # The SHIP player (game-native-code.md N3): only when a game's native directory is
    # wired into this build (ENGINE_GAME_NATIVE_DIR/_TARGET - the exporter's seam, also
    # usable by hand). Links the game's plugin STATICALLY through the same launcher;
    # ShipMain's CreatePlugin reference pulls the registration from the game archive.
    if(ENGINE_GAME_NATIVE_TARGET)
        add_executable(Engine.GamePlayer ShipMain.cpp)
        target_link_libraries(Engine.GamePlayer PRIVATE
            Engine.Player.Main ${ENGINE_GAME_NATIVE_TARGET} Foundation::Policy)
        util_copy_runtime_deps(Engine.GamePlayer EXTRA ${BUILDSYSTEM_WGPU_LIB})
        if(UNIX AND NOT APPLE)
            set_property(TARGET Engine.GamePlayer APPEND PROPERTY BUILD_RPATH "$ORIGIN")
            set_property(TARGET Engine.GamePlayer PROPERTY INSTALL_RPATH "$ORIGIN")
        endif()
    endif()

    # Stage the player's runtime deps beside it + emit Engine.Player.runtime-libs (the export
    # host-template reads that list for its sidecars). SDL3 links statically in a static engine
    # build (no DLL to stage) and shared under ENGINE_SHARED_LIBS, where TARGET_RUNTIME_DLLS
    # stages it like any other link dependency.
    # wgpu-native is dlopen'd at runtime (--webgpu) and NOT a link dependency, so it is staged
    # explicitly; on Linux, $ORIGIN on the player's RUNPATH lets the loader find it beside the exe
    # (Windows searches the exe dir for a bare DLL by default).
    # DXC is NOT staged: a player never compiles shaders. A dist renders from its cooked
    # shaders.dpak, and a dev player run from Bin/ finds the vendored compiler at
    # BUILDSYSTEM_DXC_PATH. Only the editor and the cook tools carry DXC (its glibc 2.38 floor would
    # also keep a template from ever running on an older system, the Steam Deck's).
    util_copy_runtime_deps(Engine.Player EXTRA ${BUILDSYSTEM_WGPU_LIB})
    if(UNIX AND NOT APPLE)
        set_property(TARGET Engine.Player APPEND PROPERTY BUILD_RPATH "$ORIGIN")
        set_property(TARGET Engine.Player PROPERTY INSTALL_RPATH "$ORIGIN")
    endif()

endif()

set_target_properties(Engine.Player PROPERTIES OUTPUT_NAME ${BUILTIN_PLAYER_BASENAME})
