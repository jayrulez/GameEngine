# Editor::Play - `editor.play`: play in editor (the Game tab, its resolutions, the PIE tools and
# the game-audio settings). Builds on Editor.Scene. Editor exe only.
# On the GUI in parts (Documentation/Specs/editor-gui.md, 5.3, layer 7): its UI-free half so far
# (the resolutions, the PIE instance seam, the PIE MCP tools); the Game tab and its registration,
# still on foundation.ui, stay out, unedited, until part 3.
util_add_engine_library(Editor.Play ALIAS Editor::Play)
target_link_libraries(Editor.Play PUBLIC
    Foundation::Core Foundation::Input Foundation::Scene Engine::Project Foundation::Mcp
    Foundation::Json Editor::Core Editor::Scene)
target_sources(Editor.Play
    PRIVATE
        PieMcpToolsImpl.cpp
        PieRunToolImpl.cpp
    PUBLIC
        FILE_SET editor_play_modules TYPE CXX_MODULES FILES
            GameResolution.cppm
            PieInstancePage.cppm
            PieMcpTools.cppm
            EditorPlayModule.cppm)
