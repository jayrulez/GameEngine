# Draconic::Scene.Editor - `draconic.scene.editor` (tooling: the scene SAVE/cook path).
# Never linked by the runtime.
add_library(Draconic.Scene.Editor STATIC)
add_library(Draconic::Scene.Editor ALIAS Draconic.Scene.Editor)

target_link_libraries(Draconic.Scene.Editor
    PUBLIC Draconic::Foundation Draconic::Content Draconic::Scene Draconic::Scene.Resource Draconic::Policy
           Draconic::Xml.Serialization)

target_sources(Draconic.Scene.Editor
    PUBLIC
        FILE_SET scene_editor_modules TYPE CXX_MODULES FILES
            SceneEditor.cppm)

