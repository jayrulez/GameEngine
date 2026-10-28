# Editor::PropertyAnimation - `editor.propertyanimation`: property animation authored in the scene
# (Documentation/Specs/editor-gui.md, 5.4, layer 9): the UI-free half (the clip document, the clip
# as the dopesheet's and curve view's source, the preview, the seeds, the transport) and the panel
# on the GUI the scene page holds under its viewport.
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
