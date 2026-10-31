// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The process-global JobSystem pointer and the per-thread worker slot. Defined in
// Core's implementation unit, not inline, so exactly one of each exists per
// process; inline definitions in the interface would be duplicated per shared library.

module;
#include "Core/Prelude.h"

module foundation.core;

namespace foundation::core
{
    namespace
    {
        JobSystem* g_globalJobs = nullptr;
        Atomic<const JobHooks*> g_jobHooks{nullptr};
    }

    void SetJobHooks(const JobHooks* hooks) noexcept { g_jobHooks.store(hooks, MemoryOrder::Release); }

    const JobHooks* CurrentJobHooks() noexcept { return g_jobHooks.load(MemoryOrder::Acquire); }

    i32& JobSystem::WorkerSlot() noexcept
    {
        static thread_local i32 slot = -1;
        return slot;
    }

    void InitGlobalJobSystem(u32 workerCount)
    {
        if (g_globalJobs == nullptr)
        {
            // Process-root decision: the engine-wide pool lives on the system allocator
            // (this accessor pair is a composition root, like DefaultAllocator itself).
            g_globalJobs = DefaultAllocator().New<JobSystem>(DefaultAllocator(), workerCount);
        }
    }

    void ShutdownGlobalJobSystem()
    {
        if (g_globalJobs != nullptr)
        {
            DefaultAllocator().Delete(g_globalJobs);
            g_globalJobs = nullptr;
        }
    }

    bool HasGlobalJobSystem() noexcept { return g_globalJobs != nullptr; }
    JobSystem& GlobalJobs() noexcept { return *g_globalJobs; }
} // namespace foundation::core
