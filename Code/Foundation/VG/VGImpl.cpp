// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Process-wide VG state: the path instance-id counter. Lives in an implementation unit so
// there is exactly one counter per process (a static in a module interface is instantiated
// per shared library and would hand two libraries the same id).
module;
#include "Core/Prelude.h"
#include <atomic>

module foundation.vg;

import foundation.core;

namespace foundation::vg::detail
{
    core::u64 NextPathInstanceId() noexcept
    {
        static std::atomic<core::u64> counter{1};
        return counter.fetch_add(1, std::memory_order_relaxed);
    }
}
