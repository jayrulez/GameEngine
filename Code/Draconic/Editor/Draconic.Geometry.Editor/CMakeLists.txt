# Draconic::Geometry.Editor - `draconic.geometry.editor` (tooling: Static/SkinnedMeshAsset ->
# Static/SkinnedMeshSource cook). Never linked by the runtime.
add_library(Draconic.Geometry.Editor STATIC)
add_library(Draconic::Geometry.Editor ALIAS Draconic.Geometry.Editor)

target_link_libraries(Draconic.Geometry.Editor
    PUBLIC Draconic::Foundation Draconic::Editor Draconic::Content
           Draconic::Geometry Draconic::Geometry.Resource Draconic::Policy)

target_sources(Draconic.Geometry.Editor
    PUBLIC
        FILE_SET mesh_editor_modules TYPE CXX_MODULES FILES
            MeshAsset.cppm)

