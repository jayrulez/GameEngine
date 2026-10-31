# Editor::PropertyAnimation - `editor.propertyanimation`: property animation authored in the scene.
# The UI-free half holds the clip document, the clip as the source of the dopesheet and the curve
# view, the preview, the seeds and the transport; the panel on the GUI sits under the scene page's
# viewport.
util_add_engine_library(Editor.PropertyAnimation ALIAS Editor::PropertyAnimation)
target_link_libraries(Editor.PropertyAnimation
    PUBLIC
        Foundation::Core Foundation::Content Foundation::Scene
        Foundation::PropertyAnimation Foundation::PropertyAnimation.Resource Pipeline::PropertyAnimation
        Foundation::GUI Foundation::GUI.Toolkit
        Editor::Core Editor::App Foundation::Policy)

target_sources(Editor.PropertyAnimation
    PRIVATE
        ClipDocumentImpl.cpp
        ClipSourceImpl.cpp
        ClipPreviewImpl.cpp
        PropertyAnimationPanelImpl.cpp
    PUBLIC
        FILE_SET editorpropanim_modules TYPE CXX_MODULES FILES
            ClipDocument.cppm
            ClipSource.cppm
            ClipPreview.cppm
            PropertyAnimationPanel.cppm
            PropertyAnimationEditorModule.cppm)
