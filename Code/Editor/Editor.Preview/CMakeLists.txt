# Editor::Preview - `editor.preview`: the shared 3D-preview substrate (EditorCamera +
# PreviewViewport) that the bespoke asset editor pages CONTAIN instead of each hand-rolling
# a viewport + preview scene + camera + render loop, and the thumbnail stage. Linked by the editor
# app and its page libs; never by the runtime.
# On the GUI in parts (Documentation/Specs/editor-gui.md, 5.3): the thumbnail stage is back; the
# preview viewport (PreviewViewport.cppm, PreviewViewportImpl.cpp) is still on foundation.ui and
# stays out, unedited, until the asset pages port (X6).

util_add_engine_library(Editor.Preview ALIAS Editor::Preview)
target_link_libraries(Editor.Preview
    PUBLIC
        Foundation::Core Foundation::Shell Foundation::Runtime Foundation::Runtime.Client
        Foundation::Graphics Foundation::RHI Foundation::Scene Engine::Scene
        Foundation::Render Engine::Render Editor::Camera
        Foundation::Image Foundation::Resource Editor::Core
        Foundation::Policy)

target_sources(Editor.Preview
    PRIVATE
        ThumbnailStageImpl.cpp
    PUBLIC
        FILE_SET editor_preview_modules TYPE CXX_MODULES FILES
            ThumbnailStage.cppm
            EditorPreviewModule.cppm)
