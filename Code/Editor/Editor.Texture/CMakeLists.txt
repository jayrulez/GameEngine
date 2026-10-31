# Editor::Texture - `editor.texture`: the TextureAsset page (import settings and preview) and its
# factory, on the GUI. Linked by the editor executable only.
util_add_engine_library(Editor.Texture ALIAS Editor::Texture)
target_link_libraries(Editor.Texture
    PUBLIC
        Foundation::Core Foundation::Content
        Foundation::Image Foundation::Image.IO
        Foundation::Texture Pipeline::Texture
        Foundation::GUI Foundation::GUI.Toolkit
        Editor::Core Editor::App Foundation::Policy)

target_sources(Editor.Texture
    PRIVATE
        TextureEditorPageImpl.cpp
    PUBLIC
        FILE_SET editortexture_modules TYPE CXX_MODULES FILES
            TextureThumbnailGenerator.cppm
            TextureEditorPage.cppm)

