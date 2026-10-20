# Experimental::GUI.Viewport - `experimental.gui.viewport`: a node showing content drawn into a target of its own (a 3D scene), registered with its window's renderer as an external texture, its input gated to the node.
# foundation.ui's counterpart: UI.Viewport. Specs/gui-lab-evolution-3.md.
util_add_engine_library(GUI.Viewport ALIAS Experimental::GUI.Viewport)
target_link_libraries(GUI.Viewport
    PUBLIC Foundation::Core Experimental::GUI Foundation::Policy)

target_sources(GUI.Viewport
    PUBLIC
        FILE_SET guiviewport_modules TYPE CXX_MODULES FILES
            ViewportModule.cppm)
