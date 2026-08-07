# Draconic::Input.Editor - `draconic.input.editor`: authored input-map asset + builder (tooling).
add_library(Draconic.Input.Editor STATIC)
add_library(Draconic::Input.Editor ALIAS Draconic.Input.Editor)

target_link_libraries(Draconic.Input.Editor
    PUBLIC Draconic::Foundation Draconic::Content Draconic::Editor Draconic::Input
           Draconic::Input.Resource Draconic::Policy)

target_sources(Draconic.Input.Editor
    PRIVATE
        InputMapAssetImpl.cpp   # InputMapAsset reflection body (Nested map; GCC hygiene)
    PUBLIC
        FILE_SET input_editor_modules TYPE CXX_MODULES FILES
            InputMapAsset.cppm)
