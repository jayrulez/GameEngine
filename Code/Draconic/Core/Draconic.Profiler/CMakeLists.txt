# Draconic::Profiler - `draconic.profiler` (lightweight hierarchical CPU scope profiler over Foundation).
# Instrumented across the engine via the DRACONIC_PROFILE_SCOPE macro (Profiler.h); compiles to
# nothing when DRACONIC_PROFILING is off.

add_library(Draconic.Profiler STATIC)
add_library(Draconic::Profiler ALIAS Draconic.Profiler)

target_link_libraries(Draconic.Profiler PUBLIC Draconic::Foundation Draconic::Policy)

target_sources(Draconic.Profiler
    PUBLIC
        FILE_SET profiler_modules TYPE CXX_MODULES FILES
            Profiler.cppm)

