// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Core - :hash partition
//
// Hashing utilities: a byte hash (FNV-1a, 64-bit), an integer finalizer, and
// the Hash<T> function object used by hashed containers. (Hash specializations
// for the string types live with those types in :string.)

module;
#include "Core/Prelude.h"
#include <type_traits>

export module foundation.core:hash;

import :base;

export namespace foundation::core
{
    /// FNV-1a 64-bit, as published: the offset basis and the prime. Type ids, script property
    /// hashes and every stable key are built on these, so use these constants rather than a
    /// private copy that can drift.
    inline constexpr u64 kFnv1a64OffsetBasis = 14695981039346656037ull;
    inline constexpr u64 kFnv1a64Prime = 1099511628211ull;

    // FNV-1a, 64-bit. `seed` chains a hash over several pieces (pass the previous result).
    [[nodiscard]] inline u64 HashBytes(const void* data, usize size,
                                       u64 seed = kFnv1a64OffsetBasis) noexcept
    {
        const auto* bytes = static_cast<const u8*>(data);
        u64 hash = seed;
        for (usize i = 0; i < size; ++i)
        {
            hash ^= static_cast<u64>(bytes[i]);
            hash *= kFnv1a64Prime;
        }
        return hash;
    }

    // splitmix64 finalizer: good avalanche for integer keys.
    [[nodiscard]] constexpr u64 HashInteger(u64 x) noexcept
    {
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdull;
        x ^= x >> 33;
        x *= 0xc4ceb9fe1a85ec53ull;
        x ^= x >> 33;
        return x;
    }

    template <typename T>
    struct Hash
    {
        [[nodiscard]] u64 operator()(const T& value) const noexcept
        {
            if constexpr (std::is_integral_v<T> || std::is_enum_v<T>)
            {
                return HashInteger(static_cast<u64>(value));
            }
            else if constexpr (std::is_pointer_v<T>)
            {
                return HashInteger(static_cast<u64>(reinterpret_cast<uptr>(value)));
            }
            else
            {
                static_assert(std::is_trivially_copyable_v<T>,
                              "No Hash for this type; specialize foundation::core::Hash.");
                return HashBytes(&value, sizeof(T));
            }
        }
    };
}
