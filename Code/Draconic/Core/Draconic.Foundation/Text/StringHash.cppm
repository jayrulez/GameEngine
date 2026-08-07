// Draconic Foundation - :string_hash partition
//
// StringHash: a string's identity as a 64-bit FNV-1a hash - cheap to store, compare, and
// key on, constexpr to build from a literal. The text itself is NOT retained (this is an
// identity, not a name); zero is reserved as "no value". HashText is the constexpr twin
// of HashBytes (same constants, same result over the same bytes).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.foundation:string_hash;

import :base;
import :hash;
import :string;

export namespace draconic::foundation
{
    // FNV-1a, 64-bit, over the view's UTF-8 bytes; constexpr-evaluable.
    [[nodiscard]] constexpr u64 HashText(StringView text) noexcept
    {
        u64 hash = 1469598103934665603ull;
        for (usize i = 0; i < text.Size(); ++i)
        {
            hash ^= static_cast<u64>(static_cast<u8>(text[i]));
            hash *= 1099511628211ull;
        }
        return hash;
    }

    class StringHash
    {
    public:
        constexpr StringHash() noexcept = default; // zero: "no value"
        constexpr explicit StringHash(StringView text) noexcept : m_value(HashText(text)) {}

        [[nodiscard]] constexpr u64 Value() const noexcept { return m_value; }
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return m_value != 0; }
        [[nodiscard]] constexpr bool operator==(const StringHash&) const noexcept = default;

    private:
        u64 m_value = 0;
    };

    template <>
    struct Hash<StringHash>
    {
        [[nodiscard]] u64 operator()(StringHash value) const noexcept
        {
            return HashInteger(value.Value());
        }
    };
}
