# Foundation::Profiler.GUI - `foundation.profiler.gui` (X7 of Specs/editor-gui.md): the profiler's
# views on the GUI (the frame strip, the counters, the timeline, the scope table and a scope's
# detail), each reading an IProfileSource through one focus, so they work on the live store and on
# a capture alike. Profiler.UI's counterpart; Profiler.UI stays for the in-game overlay until X8.

util_add_engine_library(Profiler.GUI ALIAS Foundation::Profiler.GUI)
target_link_libraries(Profiler.GUI PUBLIC Foundation::Profiler Foundation::GUI Foundation::VG Foundation::Core)

target_sources(Profiler.GUI
    PUBLIC
        FILE_SET profiler_gui_modules TYPE CXX_MODULES FILES
            ProfilerGUIModule.cppm
            ProfileFocus.cppm
            ProfilerWidget.cppm
            FrameStrip.cppm
            ProfilerSheet.cppm
    PRIVATE
        ProfilerGUIReflection.cpp)
