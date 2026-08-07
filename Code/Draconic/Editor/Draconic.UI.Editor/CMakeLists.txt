# Draconic::UI.Editor - `draconic.ui.editor` (tooling: UI document/theme assets + validating builders + importer).
add_library(Draconic.UI.Editor STATIC)
add_library(Draconic::UI.Editor ALIAS Draconic.UI.Editor)
target_link_libraries(Draconic.UI.Editor PUBLIC
    Draconic::Foundation Draconic::Editor Draconic::Editor.Core Draconic::Content Draconic::UI Draconic::UI.Resource Draconic::Policy)
target_sources(Draconic.UI.Editor
    PRIVATE
        UIAssetImpl.cpp
    PUBLIC FILE_SET ui_editor_modules TYPE CXX_MODULES FILES UIAsset.cppm)

