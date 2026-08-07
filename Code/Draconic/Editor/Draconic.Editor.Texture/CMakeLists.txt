# Draconic::Editor.Texture - `draconic.editor.texture`: the TextureAsset import-settings +
# preview page + factory. Linked by the editor EXECUTABLE only.
add_library(Draconic.Editor.Texture STATIC)
add_library(Draconic::Editor.Texture ALIAS Draconic.Editor.Texture)

target_link_libraries(Draconic.Editor.Texture
    PUBLIC
        Draconic::Foundation Draconic::Content
        Draconic::Image Draconic::Image.IO
        Draconic::Texture Draconic::Texture.Editor
        Draconic::UI Draconic::UI.Toolkit
        Draconic::Editor.Core Draconic::Editor.App Draconic::Policy)

target_sources(Draconic.Editor.Texture
    PRIVATE
        TextureEditorPageImpl.cpp
    PUBLIC
        FILE_SET editortexture_modules TYPE CXX_MODULES FILES
            TextureEditorPage.cppm)

