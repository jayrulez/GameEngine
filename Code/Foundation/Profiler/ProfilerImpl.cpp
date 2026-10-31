// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The process's profiler, the per-thread cache and the instance serials. They are defined here, one
// per process, because PROFILE_SCOPE expands in many libraries; inline definitions in the interface
// would give each shared library its own profiler and thread caches.

module;
#include "Core/Prelude.h"

module foundation.profiler;

import foundation.core;

using namespace foundation::core;

namespace foundation::profiler
{
    namespace
    {
        Atomic<u64> g_nextSerial{1};

        // The job system's hooks: every job a JobSystem runs shows as a "Job" scope on its
        // worker's lane, under which its own scopes nest.
        bool BeginJobScope() { return Profiler::Get().BeginScope("Job"); }
        void EndJobScope() { Profiler::Get().EndScope(); }
        const JobHooks kJobHooks{&BeginJobScope, &EndJobScope};
    }

    Profiler& Profiler::Get() noexcept
    {
        // The process's profiler is a process root, so it lives on the system allocator, and it
        // shows every job system's jobs.
        static Profiler instance(DefaultAllocator());
        static const bool hooked = (SetJobHooks(&kJobHooks), true);
        (void)hooked;
        return instance;
    }

    Profiler::Profiler(IAllocator& allocator) noexcept
        : m_allocator(&allocator), m_serial(NextSerial()), m_threads(allocator),
          m_pendingNames(allocator), m_nameIds(allocator), m_names(allocator),
          m_threadInfos(allocator), m_history(allocator), m_counters(allocator),
          m_counterValues(allocator), m_counterSet(allocator)
    {
        SetHistoryLength(kDefaultHistory);
    }

    Profiler::~Profiler()
    {
        for (ThreadData* td : m_threads)
        {
            m_allocator->Delete(td);
        }
    }

    Profiler::LocalCache& Profiler::LocalSlot() noexcept
    {
        static thread_local LocalCache cache;
        return cache;
    }

    u64 Profiler::NextSerial() noexcept { return g_nextSerial.fetch_add(1, MemoryOrder::Relaxed); }
} // namespace foundation::profiler
