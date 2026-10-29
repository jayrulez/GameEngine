# Editor::Materials - `editor.materials`: the material page (preview + parameters), on the GUI. Editor exe only.
util_add_engine_library(Editor.Materials ALIAS Editor::Materials)
target_link_libraries(Editor.Materials PUBLIC
    Foundation::Core Foundation::Content Foundation::Resource Foundation::Settings Foundation::VFS
    Foundation::RHI Foundation::Shell Foundation::Graphics Foundation::Runtime
    Foundation::Runtime.Client Foundation::Scene Engine::Scene Foundation::Render Engine::Render
    Foundation::Geometry Foundation::Materials Foundation::Materials.Resource Pipeline::Materials
    Foundation::Shaders Foundation::Shaders.Resource Foundation::Shaders.System Foundation::Texture.Resource
    Foundation::GUI Foundation::GUI.Toolkit Editor::Core Editor::App Editor::Preview Foundation::Policy)
target_sources(Editor.Materials
    PRIVATE
        MaterialPageImpl.cpp
    PUBLIC
        FILE_SET editor_materials_modules TYPE CXX_MODULES FILES
            MaterialPage.cppm
            EditorMaterialsModule.cppm)
