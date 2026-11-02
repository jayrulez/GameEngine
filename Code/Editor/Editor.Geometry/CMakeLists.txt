# Editor::Geometry - `editor.geometry`: the mesh page (preview and stats), and the import window's
# model preview. Editor exe only.
util_add_engine_library(Editor.Geometry ALIAS Editor::Geometry)
target_link_libraries(Editor.Geometry PUBLIC
    Foundation::Core Foundation::Content Foundation::Resource Foundation::Settings
    Foundation::Graphics Foundation::Runtime Foundation::Runtime.Client Foundation::Scene
    Engine::Scene Foundation::Render Engine::Render Foundation::Geometry Pipeline::Geometry
    Foundation::Materials Foundation::GUI Foundation::GUI.Toolkit
    Foundation::Model Foundation::Model.Resource Foundation::Animation Engine::Model Pipeline::ModelImporter
    Editor::Core Editor::App Editor::Preview Editor::ModelPreview Editor::Animation Foundation::Policy)
target_sources(Editor.Geometry
    PRIVATE
        MeshPageImpl.cpp
        ModelFilePreviewImpl.cpp
    PUBLIC
        FILE_SET editor_geometry_modules TYPE CXX_MODULES FILES
            MeshPage.cppm
            ModelFilePreview.cppm
            EditorGeometryModule.cppm)
