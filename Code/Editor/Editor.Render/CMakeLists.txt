# Editor::Render - `editor.render`: the settings profile page (environment and post-process
# profiles, previewed on a built-in or a project scene), on the GUI. Builds on Editor.Scene. Editor exe only.
util_add_engine_library(Editor.Render ALIAS Editor::Render)
target_link_libraries(Editor.Render PUBLIC
    Foundation::Core Foundation::Content Foundation::Resource Foundation::Settings Foundation::Graphics
    Foundation::Runtime Foundation::Runtime.Client Foundation::Scene Foundation::Scene.Resource Foundation::Geometry
    Foundation::Materials Engine::Render Pipeline::Render Foundation::GUI Foundation::GUI.Toolkit Editor::Core Editor::App Editor::Preview Editor::Scene
    Foundation::Policy)
target_sources(Editor.Render
    PRIVATE
        SettingsProfilePageImpl.cpp
    PUBLIC
        FILE_SET editor_render_modules TYPE CXX_MODULES FILES
            SettingsProfilePage.cppm
            EditorRenderModule.cppm)
