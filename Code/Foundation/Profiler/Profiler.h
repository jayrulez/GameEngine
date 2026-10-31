// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Profiler - instrumentation macros.
//
// Include this header (it is only macros) and `import foundation.profiler` in a TU that instruments.
// PROFILE_SCOPE("Name") profiles the enclosing block; the frame macros bracket a frame.
// When BUILD_PROFILING is off (shipping builds), every macro compiles to nothing.
#pragma once

#ifndef BUILD_PROFILING
#define BUILD_PROFILING 0
#endif

#define PROFILE_CONCAT_(a, b) a##b
#define PROFILE_CONCAT(a, b) PROFILE_CONCAT_(a, b)

#if BUILD_PROFILING

#define PROFILE_SCOPE(name)                                                               \
    ::foundation::profiler::ScopedProfile PROFILE_CONCAT(profScope_, __LINE__)      \
    {                                                                                              \
        (name)                                                                                     \
    }
#define PROFILE_FRAME_BEGIN() ::foundation::profiler::Profiler::Get().BeginFrame()
#define PROFILE_FRAME_END() ::foundation::profiler::Profiler::Get().EndFrame()

#else

#define PROFILE_SCOPE(name) ((void)0)
#define PROFILE_FRAME_BEGIN() ((void)0)
#define PROFILE_FRAME_END() ((void)0)

#endif
