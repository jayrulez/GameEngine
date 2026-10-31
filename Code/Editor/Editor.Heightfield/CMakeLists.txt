# Editor::Heightfield - `editor.heightfield`: the HeightfieldAsset page for inspecting and authoring
# (a 2D grayscale height preview and the authored fields) and its factory, on the GUI. Linked by the
# editor executable only.
util_add_engine_library(Editor.Heightfield ALIAS Editor::Heightfield)
target_link_libraries(Editor.Heightfield
    PUBLIC
        Foundation::Core Foundation::Content
        Foundation::Image Foundation::Image.IO Pipeline::Heightfield
        Foundation::GUI Foundation::GUI.Toolkit
        Editor::Core Editor::App Foundation::Policy)

target_sources(Editor.Heightfield
    PRIVATE
        HeightfieldEditorPageImpl.cpp
    PUBLIC
        FILE_SET editorheightfield_modules TYPE CXX_MODULES FILES
            HeightfieldEditorPage.cppm HeightfieldThumbnailGenerator.cppm)
