// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// RHI - :resources implementation unit.
//
// Out-of-line bodies whose headers stay out of the interface, where they would make GCC emit an
// unreadable gcm cluster for consumers: the TextureView unique-id counter (<atomic>).

module;
#include <atomic>

module foundation.rhi;

import foundation.core;
namespace core = foundation::core;

namespace foundation::rhi
{
    core::u64 NextTextureViewUniqueId() noexcept
    {
        static std::atomic<core::u64> counter{0};
        return counter.fetch_add(1, std::memory_order_relaxed) + 1;
    }
}
