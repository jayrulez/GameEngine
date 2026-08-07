# Draconic::Editor.Fonts - `draconic.editor.fonts` (the FontAsset authoring page:
# live atlas bake preview + bake-intent property grid). Tooling only.

add_library(Draconic.Editor.Fonts STATIC)
add_library(Draconic::Editor.Fonts ALIAS Draconic.Editor.Fonts)

target_link_libraries(Draconic.Editor.Fonts
    PUBLIC Draconic::Foundation Draconic::Content Draconic::VFS Draconic::Image
           Draconic::Fonts Draconic::Fonts.TTF Draconic::Fonts.IO Draconic::Fonts.Importer
           Draconic::Fonts.DistanceField.Baker Draconic::Fonts.Editor
           Draconic::UI Draconic::UI.Toolkit Draconic::Editor.Core Draconic::Editor.App
           Draconic::Policy)

target_sources(Draconic.Editor.Fonts
    PRIVATE
        FontEditorPageImpl.cpp
    PUBLIC
        FILE_SET editor_fonts_modules TYPE CXX_MODULES FILES
            FontEditorPage.cppm)
