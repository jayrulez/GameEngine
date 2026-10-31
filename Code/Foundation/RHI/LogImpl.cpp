// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.rhi - :log implementation unit: the one process-wide log-sink slot (a sink set
// by a test or a host must see every backend's lines, whichever shared library emits them).

module;
#include "Core/Prelude.h"

module foundation.rhi;

namespace foundation::rhi
{
    namespace
    {
        LogSink g_sink = nullptr;
        void* g_sinkContext = nullptr;
    }

    void SetLogSink(LogSink sink, void* context) noexcept
    {
        g_sink = sink;
        g_sinkContext = context;
    }

    bool DispatchToLogSink(bool error, const char* utf8) noexcept
    {
        return g_sink != nullptr && g_sink(g_sinkContext, error, utf8);
    }
}
