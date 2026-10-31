# Foundation::Profiler.Capture - `foundation.profiler.capture`: what a capture becomes outside
# the process: a Chrome trace JSON export of any profile source (for
# Perfetto and chrome://tracing) and the recorder that writes capture files, apart from the
# profiler itself so the store every library links stays on Core alone.

util_add_engine_library(Profiler.Capture ALIAS Foundation::Profiler.Capture)
target_link_libraries(Profiler.Capture PUBLIC Foundation::Profiler Foundation::Json Foundation::Core Foundation::Policy)

# Module implementation unit: the process's recorder (one per process).
target_sources(Profiler.Capture PRIVATE ProfilerCaptureImpl.cpp)

target_sources(Profiler.Capture
    PUBLIC
        FILE_SET profiler_capture_modules TYPE CXX_MODULES FILES
            ChromeTrace.cppm
            CaptureRecorder.cppm
            ProfilerCaptureModule.cppm)
