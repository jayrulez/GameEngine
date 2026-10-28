# Foundation::Profiler.UI - `foundation.profiler.ui` (Specs/profiler.md, P4): the profiler's views,
# drawn with VG on foundation.ui, which the in-game overlay and the editor's panel each assemble.
# Every view reads an IProfileSource, so each works on the live store and on a capture file alike.
# Apart from the profiler so the store every library links stays UI-free.

util_add_engine_library(Profiler.UI ALIAS Foundation::Profiler.UI)
target_link_libraries(Profiler.UI PUBLIC Foundation::Profiler Foundation::UI Foundation::VG Foundation::Fonts Foundation::Core)

target_sources(Profiler.UI
    PUBLIC
        FILE_SET profiler_ui_modules TYPE CXX_MODULES FILES
            ProfilerView.cppm
            FrameTimeGraph.cppm
            FrameStats.cppm
            TopList.cppm
            CounterStrip.cppm
            Timeline.cppm
            ScopeTable.cppm
            ProfilerOverlay.cppm
            ProfilerUIModule.cppm)
