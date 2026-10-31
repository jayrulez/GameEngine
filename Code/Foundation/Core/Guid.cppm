// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Core - :guid partition
//
// Guid: a 128-bit globally unique identifier (asset ids, object ids, ...).
// Generate() produces an RFC 4122 version-4 (random) GUID from a caller-owned
// PRNG, so ids are deterministic/replayable when the generator is seeded.
// Trivially copyable, so it hashes out of the box (the generic Hash<T> hashes its bytes)
// and works as a HashMap/HashSet key.

module;
#include "Core/Prelude.h"
#include "Core/System/SystemBackend.h" // sys::FillEntropy (TryGenerateFromSystemEntropy)

export module foundation.core:guid;

import :base;
import :random;
import :string;

export namespace foundation::core
{
    struct ENGINE_EXPORT_DATA Guid
    {
        u64 high = 0;
        u64 low = 0;

        constexpr Guid() noexcept = default;
        constexpr Guid(u64 h, u64 l) noexcept : high(h), low(l) {}

        // Parses the canonical 36-char form (the ToChars spelling). Malformed
        // input yields Nil - check IsNil() when the source is untrusted; use
        // TryParse for an explicit failure signal.
        explicit Guid(StringView text) noexcept
        {
            Guid parsed;
            if (TryParse(text, parsed))
            {
                *this = parsed;
            }
        }

        [[nodiscard]] constexpr bool IsNil() const noexcept { return high == 0 && low == 0; }
        [[nodiscard]] explicit constexpr operator bool() const noexcept { return !IsNil(); }

        // Random (version-4) GUID. The caller owns the generator: seed it for
        // reproducible ids, or share one device for unique ids.
        [[nodiscard]] static Guid Generate(Random& rng) noexcept
        {
            return Tagged(Guid{rng.NextU64(), rng.NextU64()});
        }

        // Random (version-4) GUID from OS entropy: one nobody can predict or replay (a
        // secret, a token), never for ids that must reproduce from a seed. False when the
        // OS refused entropy; `out` is untouched then.
        [[nodiscard]] static bool TryGenerateFromSystemEntropy(Guid& out) noexcept
        {
            u64 words[2];
            if (!sys::FillEntropy(words, sizeof(words)))
            {
                return false;
            }
            out = Tagged(Guid{words[0], words[1]});
            return true;
        }

        // Canonical lowercase 8-4-4-4-12 form into `out` (36 chars + null).
        void ToChars(utf8char out[37]) const noexcept
        {
            static constexpr utf8char kHex[] = u8"0123456789abcdef";
            usize pos = 0;
            for (usize i = 0; i < 16; ++i)
            {
                if (i == 4 || i == 6 || i == 8 || i == 10)
                {
                    out[pos++] = utf8char('-');
                }
                const u64 source = (i < 8) ? high : low;
                const u32 shift = static_cast<u32>((7 - (i & 7)) * 8);
                const u8 byte = static_cast<u8>((source >> shift) & 0xFFull);
                out[pos++] = kHex[byte >> 4];
                out[pos++] = kHex[byte & 0xF];
            }
            out[pos] = utf8char('\0');
        }

        // Parses the canonical 36-char form. Returns false (leaving `out`
        // untouched) on malformed input.
        [[nodiscard]] static bool TryParse(StringView text, Guid& out) noexcept
        {
            if (text.Size() != 36)
            {
                return false;
            }

            u64 hi = 0;
            u64 lo = 0;
            usize nibbles = 0;
            for (usize i = 0; i < 36; ++i)
            {
                const utf8char c = text[i];
                if (i == 8 || i == 13 || i == 18 || i == 23)
                {
                    if (c != utf8char('-'))
                    {
                        return false;
                    }
                    continue;
                }
                const i32 value = HexValue(c);
                if (value < 0)
                {
                    return false;
                }
                if (nibbles < 16)
                {
                    hi = (hi << 4) | static_cast<u64>(value);
                }
                else
                {
                    lo = (lo << 4) | static_cast<u64>(value);
                }
                ++nibbles;
            }
            if (nibbles != 32)
            {
                return false;
            }

            out = Guid{hi, lo};
            return true;
        }

        static const Guid Nil;

    private:
        // The version (4) and variant (RFC 4122, 10xx) bits over 128 random bits.
        [[nodiscard]] static constexpr Guid Tagged(Guid g) noexcept
        {
            g.high = (g.high & 0xFFFFFFFFFFFF0FFFull) | 0x0000000000004000ull;
            g.low = (g.low & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull;
            return g;
        }

        [[nodiscard]] static constexpr i32 HexValue(utf8char c) noexcept
        {
            if (c >= utf8char('0') && c <= utf8char('9'))
            {
                return static_cast<u8>(c) - '0';
            }
            if (c >= utf8char('a') && c <= utf8char('f'))
            {
                return static_cast<u8>(c) - 'a' + 10;
            }
            if (c >= utf8char('A') && c <= utf8char('F'))
            {
                return static_cast<u8>(c) - 'A' + 10;
            }
            return -1;
        }
    };

    inline constexpr Guid Guid::Nil{0, 0};

    [[nodiscard]] constexpr bool operator==(Guid a, Guid b) noexcept
    {
        return a.high == b.high && a.low == b.low;
    }
    [[nodiscard]] constexpr bool operator!=(Guid a, Guid b) noexcept { return !(a == b); }
    [[nodiscard]] constexpr bool operator<(Guid a, Guid b) noexcept
    {
        return (a.high != b.high) ? (a.high < b.high) : (a.low < b.low);
    }
}
