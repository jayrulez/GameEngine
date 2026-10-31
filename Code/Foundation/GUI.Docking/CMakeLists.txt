# Foundation::GUI.Docking - `foundation.gui.docking`: docking: splits, tab groups, drop zones, floating panels in the window and as OS windows (through hosting), layouts saved and restored, and its default sheet.
# Docking is one whole feature, so this library links hosting for the OS windows of its floating
# panels rather than leaving them to a layer above.
util_add_engine_library(GUI.Docking ALIAS Foundation::GUI.Docking)
target_link_libraries(GUI.Docking
    PUBLIC Foundation::Core Foundation::GUI Foundation::GUI.Hosting Foundation::Shell Foundation::Graphics Foundation::Policy)

target_sources(GUI.Docking
    PUBLIC
        FILE_SET guidocking_modules TYPE CXX_MODULES FILES
            DockingModule.cppm
            DockLayout.cppm
            DockDocument.cppm
            DockController.cppm
            DockSpace.cppm
            DockArea.cppm
            DockHostWindows.cppm
    PRIVATE
        DockingReflection.cpp)
