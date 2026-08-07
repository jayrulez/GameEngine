# Draconic::Editor.Image - `draconic.editor.image`: the ImageAsset inspect + color-space
# page + factory (the Texture page's source-side sibling). Linked by the editor EXECUTABLE only.
add_library(Draconic.Editor.Image STATIC)
add_library(Draconic::Editor.Image ALIAS Draconic.Editor.Image)

target_link_libraries(Draconic.Editor.Image
    PUBLIC
        Draconic::Foundation Draconic::Content
        Draconic::Image Draconic::Image.IO Draconic::Image.Editor
        Draconic::UI Draconic::UI.Toolkit
        Draconic::Editor.Core Draconic::Editor.App Draconic::Policy)

target_sources(Draconic.Editor.Image
    PRIVATE
        ImageEditorPageImpl.cpp
    PUBLIC
        FILE_SET editorimage_modules TYPE CXX_MODULES FILES
            ImageEditorPage.cppm)

