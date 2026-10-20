# Experimental::GUI.Docking - `experimental.gui.docking`: docking: splits, tab groups, drop zones, floating panels in the window and as OS windows (through hosting), layouts saved and restored, and its default sheet.
# foundation.ui's counterpart: UI.Toolkit's docking and UI.Application. Specs/gui-lab-evolution-3.md.
util_add_engine_library(GUI.Docking ALIAS Experimental::GUI.Docking)
target_link_libraries(GUI.Docking
    PUBLIC Foundation::Core Experimental::GUI Foundation::Policy)

target_sources(GUI.Docking
    PUBLIC
        FILE_SET guidocking_modules TYPE CXX_MODULES FILES
            DockingModule.cppm
            DockLayout.cppm
            DockDocument.cppm
            DockController.cppm
            DockSpace.cppm
    PRIVATE
        DockingReflection.cpp)
