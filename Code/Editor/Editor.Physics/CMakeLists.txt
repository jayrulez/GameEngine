# Editor::Physics - `editor.physics` (the CollisionShapeAsset authoring page:
# the cooked outline beside the asset's rows), on the GUI. Tooling only.

util_add_engine_library(Editor.Physics ALIAS Editor::Physics)
target_link_libraries(Editor.Physics
    PUBLIC Foundation::Core Foundation::Content Foundation::Runtime Foundation::Runtime.Client
           Foundation::Resource Pipeline::Physics Foundation::Physics.Resource
           Foundation::Render Foundation::Graphics
           Foundation::Scene Engine::Scene Engine::Render Foundation::Geometry Foundation::Materials
           Foundation::GUI Foundation::GUI.Toolkit
           Editor::Core Editor::App Editor::Preview
           Foundation::Policy)

target_sources(Editor.Physics
    PRIVATE
        CollisionShapePageImpl.cpp
        CollisionThumbnailImpl.cpp
    PUBLIC
        FILE_SET editor_physics_modules TYPE CXX_MODULES FILES
            CollisionShapePage.cppm CollisionThumbnail.cppm)
