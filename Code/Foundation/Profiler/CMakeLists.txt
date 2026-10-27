# Foundation::Profiler - `foundation.profiler` (lightweight hierarchical CPU scope profiler over Core).
# Instrumented across the engine via the PROFILE_SCOPE macro (Profiler.h); compiles to
# nothing when BUILD_PROFILING is off.

util_add_engine_library(Profiler ALIAS Foundation::Profiler)
target_link_libraries(Profiler PUBLIC Foundation::Core Foundation::Policy)

# Module implementation unit: the singleton + per-thread slot (one per process).
target_sources(Profiler PRIVATE ProfilerImpl.cpp)

target_sources(Profiler
    PUBLIC
        FILE_SET profiler_modules TYPE CXX_MODULES FILES
            Profiler.cppm
            ProfileSource.cppm
            ProfileStream.cppm
            ProfileRecording.cppm
            ProfileWindow.cppm
            ProfilerModule.cppm)

