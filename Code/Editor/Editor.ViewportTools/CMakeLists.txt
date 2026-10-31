# Editor::ViewportTools - the viewport tool-mode framework (editor.viewporttools).
#
# The thin layer between Editor.Core and the pages: IViewportTool, its manager and the provider
# registry. Editor.Core is UI-free because headless hosts link it, so tool contracts that speak
# pick rays and overlay drawing cannot live there. Domain editor libs (Editor.Terrain, ...)
# implement tools against this interface without linking any page; the scene page hosts the
# manager. No toolkit dependency: input is a plain struct the host fills, as GizmoFrameInput is.

util_add_engine_library(Editor.ViewportTools ALIAS Editor::ViewportTools)
target_link_libraries(Editor.ViewportTools
    PUBLIC
        Foundation::Core Foundation::Scene Foundation::Render Foundation::Shell
        Editor::Core Foundation::Policy)

target_sources(Editor.ViewportTools
    PRIVATE
        ViewportToolsImpl.cpp
    PUBLIC
        FILE_SET editor_viewporttools_modules TYPE CXX_MODULES FILES
            ViewportTools.cppm)
