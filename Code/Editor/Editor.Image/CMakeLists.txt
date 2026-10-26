# Editor::Image - `editor.image`: the ImageAsset inspect + color-space
# page + factory (the Texture page's source-side sibling), on the GUI. Linked by the editor EXECUTABLE only.
util_add_engine_library(Editor.Image ALIAS Editor::Image)
target_link_libraries(Editor.Image
    PUBLIC
        Foundation::Core Foundation::Content
        Foundation::Image Foundation::Image.IO Pipeline::Image
        Foundation::GUI Foundation::GUI.Toolkit
        Editor::Core Editor::App Foundation::Policy)

target_sources(Editor.Image
    PRIVATE
        ImageEditorPageImpl.cpp
    PUBLIC
        FILE_SET editorimage_modules TYPE CXX_MODULES FILES
            ImageEditorPage.cppm)

