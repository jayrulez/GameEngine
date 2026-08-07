// Draconic Foundation - :hash partition
//
// Hashing utilities: a byte hash (FNV-1a, 64-bit), an integer finalizer, and
// the Hash<T> function object used by hashed containers. (Hash specializations
// for the string types live with those types in :string.)
// specializations.

module;
#include "Draconic.Foundation/Prelude.h"
#include <type_traits>

export module draconic.foundation:hash;

import :base;

export namespace draconic::foundation
{
    // FNV-1a, 64-bit.
    [[nodiscard]] inline u64 HashBytes(const void* data, usize size,
                                       u64 seed = 1469598103934665603ull) noexcept
    {
        const auto* bytes = static_cast<const u8*>(data);
        u64 hash = seed;
        for (usize i = 0; i < size; ++i)
        {
            hash ^= static_cast<u64>(bytes[i]);
            hash *= 1099511628211ull;
        }
        return hash;
    }

    // splitmix64 finalizer - good avalanche for integer keys.
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
                              "No Hash for this type; specialize draconic::foundation::Hash.");
                return HashBytes(&value, sizeof(T));
            }
        }
    };
}
