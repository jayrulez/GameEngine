# Draconic::Materials.Editor - `draconic.materials.editor` (tooling: MaterialAsset ->
# MaterialSource cook). Never linked by the runtime.
add_library(Draconic.Materials.Editor STATIC)
add_library(Draconic::Materials.Editor ALIAS Draconic.Materials.Editor)

target_link_libraries(Draconic.Materials.Editor
    PUBLIC Draconic::Foundation Draconic::Editor Draconic::Content
           Draconic::Materials Draconic::Materials.Resource Draconic::Policy)

target_sources(Draconic.Materials.Editor
    PRIVATE
        MaterialAssetImpl.cpp   # MaterialAsset reflection body (Nested source; GCC hygiene)
    PUBLIC
        FILE_SET material_editor_modules TYPE CXX_MODULES FILES
            MaterialAsset.cppm)

