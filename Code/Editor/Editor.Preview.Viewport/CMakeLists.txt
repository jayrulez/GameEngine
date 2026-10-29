# Editor::Preview.Viewport - `editor.preview.viewport`: the shared 3D-preview substrate (a GUI
# viewport, a private preview scene, the editor camera, the render loop) that the editor's asset
# pages contain, and that a standalone tool (the model viewer) can have without the editor's core.
# Never linked by the runtime.

util_add_engine_library(Editor.Preview.Viewport ALIAS Editor::Preview.Viewport)
target_link_libraries(Editor.Preview.Viewport
    PUBLIC
        Foundation::Core Foundation::Shell Foundation::Runtime Foundation::Runtime.Client
        Foundation::Graphics Foundation::RHI Foundation::Scene Engine::Scene
        Foundation::Render Engine::Render Editor::Camera
        Foundation::GUI Foundation::GUI.Hosting Foundation::GUI.Viewport
        Foundation::Policy)

target_sources(Editor.Preview.Viewport
    PRIVATE
        PreviewViewportImpl.cpp
    PUBLIC
        FILE_SET editor_preview_viewport_modules TYPE CXX_MODULES FILES
            PreviewViewport.cppm)
