# Draconic::Editor.Generic - `draconic.editor.generic`: the serialize-driven FALLBACK asset
# page (every asset without a bespoke page). Linked by the editor EXECUTABLE only.
add_library(Draconic.Editor.Generic STATIC)
add_library(Draconic::Editor.Generic ALIAS Draconic.Editor.Generic)

target_link_libraries(Draconic.Editor.Generic
    PUBLIC
        Draconic::Foundation Draconic::Content
        Draconic::UI Draconic::UI.Toolkit
        Draconic::Editor.Core Draconic::Editor.App Draconic::Policy)

target_sources(Draconic.Editor.Generic
    PRIVATE
        AssetFormPageImpl.cpp
    PUBLIC
        FILE_SET editorgeneric_modules TYPE CXX_MODULES FILES
            AssetFormPage.cppm)

