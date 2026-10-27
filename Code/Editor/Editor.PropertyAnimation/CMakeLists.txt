# Editor::PropertyAnimation - `editor.propertyanimation`: property animation authored in the scene
# (Documentation/Specs/editor-gui.md, 5.4, layer 9). In parts: the UI-free half (the clip document,
# the clip as the dopesheet's and curve view's source, the preview, the seeds, the transport) so far;
# the panel on the GUI with part 2 (the old panel's files stay out of the build until then).
util_add_engine_library(Editor.PropertyAnimation ALIAS Editor::PropertyAnimation)
target_link_libraries(Editor.PropertyAnimation
    PUBLIC
        Foundation::Core Foundation::Content Foundation::Scene
        Foundation::PropertyAnimation Foundation::PropertyAnimation.Resource Pipeline::PropertyAnimation
        Foundation::GUI Foundation::GUI.Toolkit
        Editor::Core Foundation::Policy)

target_sources(Editor.PropertyAnimation
    PRIVATE
        ClipDocumentImpl.cpp
        ClipSourceImpl.cpp
        ClipPreviewImpl.cpp
    PUBLIC
        FILE_SET editorpropanim_modules TYPE CXX_MODULES FILES
            ClipDocument.cppm
            ClipSource.cppm
            ClipPreview.cppm
            PropertyAnimationEditorModule.cppm)
