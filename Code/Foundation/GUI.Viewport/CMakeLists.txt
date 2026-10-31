# Foundation::GUI.Viewport - `foundation.gui.viewport`: Viewport, a node showing content drawn
# into a target of its own (a 3D scene), with its input gated to the node; the host binds, places
# and records it (GUI.Hosting's IHostedContent).
util_add_engine_library(GUI.Viewport ALIAS Foundation::GUI.Viewport)
target_link_libraries(GUI.Viewport
    PUBLIC Foundation::Core Foundation::GUI Foundation::GUI.Hosting Foundation::RHI Foundation::Image Foundation::VG
           Foundation::VG.Renderer Foundation::Shell Foundation::Policy)

target_sources(GUI.Viewport
    PUBLIC
        FILE_SET guiviewport_modules TYPE CXX_MODULES FILES
            Viewport.cppm)
