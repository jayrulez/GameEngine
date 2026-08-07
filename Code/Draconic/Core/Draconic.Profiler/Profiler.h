// Draconic::Profiler - instrumentation macros.
//
// Include this header (it's just macros) AND `import draconic.profiler` in a TU that instruments.
// DRACONIC_PROFILE_SCOPE("Name") profiles the enclosing block; the frame macros bracket a frame.
// When DRACONIC_PROFILING is off (shipping builds), every macro compiles to nothing.
#pragma once

#ifndef DRACONIC_PROFILING
#define DRACONIC_PROFILING 0
#endif

#define DRACONIC_PROFILE_CONCAT_(a, b) a##b
#define DRACONIC_PROFILE_CONCAT(a, b) DRACONIC_PROFILE_CONCAT_(a, b)

#if DRACONIC_PROFILING

#define DRACONIC_PROFILE_SCOPE(name)                                                               \
    ::draconic::profiler::ScopedProfile DRACONIC_PROFILE_CONCAT(draconicProfScope_, __LINE__)      \
    {                                                                                              \
        (name)                                                                                     \
    }
#define DRACONIC_PROFILE_FRAME_BEGIN() ::draconic::profiler::Profiler::Get().BeginFrame()
#define DRACONIC_PROFILE_FRAME_END() ::draconic::profiler::Profiler::Get().EndFrame()

#else

#define DRACONIC_PROFILE_SCOPE(name) ((void)0)
#define DRACONIC_PROFILE_FRAME_BEGIN() ((void)0)
#define DRACONIC_PROFILE_FRAME_END() ((void)0)

#endif
