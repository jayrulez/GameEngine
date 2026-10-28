# Editor::Play - `editor.play`: play in editor (the Game tab, its resolutions, the PIE tools and
# the game-audio settings). Builds on Editor.Scene. Editor exe only.
# On the GUI (Documentation/Specs/editor-gui.md, 5.3, layer 7): the Game tab from its markup
# (Data/Editor/Game.xml) on GUI.Viewport.
util_add_engine_library(Editor.Play ALIAS Editor::Play)
target_link_libraries(Editor.Play PUBLIC
    Foundation::Core Foundation::Content Foundation::Resource Foundation::Settings Foundation::VFS
    Foundation::RHI
    Foundation::Shell Foundation::Graphics Foundation::Runtime Foundation::Runtime.Client
    Foundation::Scene Foundation::Scene.Resource Engine::Scene Engine::Render Engine::DefaultApp
    Engine::GameInstance Engine::Project Foundation::Audio Foundation::Audio.Resource Engine::Audio
    Foundation::Input Foundation::Input.Resource Engine::Input Engine::Physics Foundation::Script
    Foundation::Script.Resource Engine::Script Foundation::GUI Foundation::GUI.Hosting
    Foundation::GUI.Viewport Foundation::Mcp Foundation::Json Editor::Core Editor::App Editor::Scene
    Foundation::Policy)
target_sources(Editor.Play
    PRIVATE
        GamePageImpl.cpp
        PieMcpToolsImpl.cpp
        PieRunToolImpl.cpp
    PUBLIC
        FILE_SET editor_play_modules TYPE CXX_MODULES FILES
            GameResolution.cppm
            PieInstancePage.cppm
            PieMcpTools.cppm
            GamePage.cppm
            PlayInEditor.cppm
            EditorPlayModule.cppm)
