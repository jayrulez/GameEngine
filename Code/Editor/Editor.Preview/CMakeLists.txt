# Editor::Preview - `editor.preview`: the thumbnail stage, and the preview viewport re-exported
# from its own library (Editor::Preview.Viewport, which a tool can link without the editor's core).
# Linked by the editor app and its page libs; never by the runtime.
# On the GUI (Documentation/Specs/editor-gui.md, 5.3 and 5.4): the thumbnail stage, then the preview
# viewport on GUI.Viewport with the asset pages (X6, layer 3).

util_add_engine_library(Editor.Preview ALIAS Editor::Preview)
target_link_libraries(Editor.Preview
    PUBLIC
        Foundation::Core Foundation::Shell Foundation::Runtime Foundation::Runtime.Client
        Foundation::Graphics Foundation::RHI Foundation::Scene Engine::Scene
        Foundation::Render Engine::Render Editor::Camera Editor::Preview.Viewport
        Foundation::Image Foundation::Resource Editor::Core
        Foundation::GUI Foundation::GUI.Hosting Foundation::GUI.Viewport # the preview's viewport
        Foundation::Policy)

target_sources(Editor.Preview
    PRIVATE
        ThumbnailStageImpl.cpp
    PUBLIC
        FILE_SET editor_preview_modules TYPE CXX_MODULES FILES
            Preview.cppm
            ThumbnailStage.cppm)
