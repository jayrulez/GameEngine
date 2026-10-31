// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The process's capture recorder. It is defined here, one per process, because the key, the flag
// and the tools that start a capture live in different libraries.

module;
#include "Core/Prelude.h"

module foundation.profiler.capture;

import foundation.core;

using namespace foundation::core;

namespace foundation::profiler
{
    CaptureRecorder& CaptureRecorder::Get() noexcept
    {
        // The process's recorder is a process root, so it lives on the system allocator.
        static CaptureRecorder instance(DefaultAllocator());
        return instance;
    }
} // namespace foundation::profiler
