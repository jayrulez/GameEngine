# Experimental::GUI.Toolkit - `experimental.gui.toolkit`: editor widgets: the property grid, colour pickers, curves, node graphs, tool and status bars. Docking is not here (GUI.Docking).
# foundation.ui's counterpart: UI.Toolkit less docking. Specs/gui-lab-evolution-3.md.
util_add_engine_library(GUI.Toolkit ALIAS Experimental::GUI.Toolkit)
target_link_libraries(GUI.Toolkit
    PUBLIC Foundation::Core Experimental::GUI Foundation::Policy)

target_sources(GUI.Toolkit
    PUBLIC
        FILE_SET guitoolkit_modules TYPE CXX_MODULES FILES
            ToolkitModule.cppm)
