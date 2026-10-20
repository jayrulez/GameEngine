# Experimental::GUI.Resources - `experimental.gui.resources`: cooked GUI documents and themes, free of the GUI framework.
# foundation.ui's counterpart: UI.Resource. Specs/gui-lab-evolution-3.md.
util_add_engine_library(GUI.Resources ALIAS Experimental::GUI.Resources)
target_link_libraries(GUI.Resources
    PUBLIC Foundation::Core Experimental::GUI Foundation::Policy)

target_sources(GUI.Resources
    PUBLIC
        FILE_SET guiresources_modules TYPE CXX_MODULES FILES
            ResourcesModule.cppm)
