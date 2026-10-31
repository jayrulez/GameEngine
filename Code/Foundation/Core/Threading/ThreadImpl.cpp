// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The calling thread's name. Defined in Core's implementation unit, not inline, so a thread has
// one name per process, whichever shared library asks.

module;
#include "Core/Prelude.h"

module foundation.core;

namespace foundation::core
{
    namespace
    {
        struct ThreadName
        {
            static constexpr usize kCapacity = 32;
            char8_t bytes[kCapacity] = {};
            usize length = 0;
        };

        ThreadName& LocalThreadName() noexcept
        {
            static thread_local ThreadName name;
            return name;
        }
    }

    void SetCurrentThreadName(StringView name) noexcept
    {
        ThreadName& local = LocalThreadName();
        const usize length = Min(name.Size(), ThreadName::kCapacity - 1);
        for (usize i = 0; i < length; ++i)
        {
            local.bytes[i] = name.Data()[i];
        }
        local.bytes[length] = 0;
        local.length = length;
    }

    StringView CurrentThreadName() noexcept
    {
        const ThreadName& local = LocalThreadName();
        return StringView(local.bytes, local.length);
    }
} // namespace foundation::core
