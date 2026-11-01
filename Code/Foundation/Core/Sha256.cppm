// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Core - :sha256 partition
//
// SHA-256 (FIPS 180-4), fed in pieces: to check a download against the digest its publisher gives
// (a release's samples.json), hashing each piece as it arrives. Not for passwords or keys: the TLS
// backend has the crypto for those.

module;
#include "Core/Prelude.h"

export module foundation.core:sha256;

import :base;
import :span;
import :string;

export namespace foundation::core
{
    namespace detail
    {
        inline constexpr u32 kSha256Rounds[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
        };
    }

    /// A SHA-256 digest: 32 bytes.
    struct Sha256Digest
    {
        u8 bytes[32]{};

        /// Its 64 lowercase hex digits, as published digests are written.
        [[nodiscard]] String ToHex() const
        {
            static constexpr char8_t kDigits[] = u8"0123456789abcdef";
            String hex;
            for (const u8 b : bytes)
            {
                hex.Append(kDigits[b >> 4]);
                hex.Append(kDigits[b & 0x0F]);
            }
            return hex;
        }
        [[nodiscard]] bool operator==(const Sha256Digest&) const noexcept = default;
    };

    /// SHA-256 over bytes given in any number of pieces: Update with each, then Finish once.
    class Sha256
    {
    public:
        void Update(Span<const byte> bytes) noexcept
        {
            for (const byte b : bytes)
            {
                m_block[m_used++] = static_cast<u8>(b);
                if (m_used == 64)
                {
                    Compress();
                    m_used = 0;
                }
            }
            m_length += bytes.Size();
        }

        /// The digest of everything given. The hash is spent after this; make a new one to go on.
        [[nodiscard]] Sha256Digest Finish() noexcept
        {
            const u64 bits = m_length * 8u;
            m_block[m_used++] = 0x80;
            if (m_used > 56)
            {
                while (m_used < 64)
                    m_block[m_used++] = 0;
                Compress();
                m_used = 0;
            }
            while (m_used < 56)
                m_block[m_used++] = 0;
            for (int i = 7; i >= 0; --i)
                m_block[m_used++] = static_cast<u8>(bits >> (i * 8));
            Compress();
            Sha256Digest digest;
            for (usize i = 0; i < 8; ++i)
                for (usize k = 0; k < 4; ++k)
                    digest.bytes[i * 4 + k] = static_cast<u8>(m_state[i] >> (24 - 8 * k));
            return digest;
        }

        /// The digest of `bytes` in one call.
        [[nodiscard]] static Sha256Digest Of(Span<const byte> bytes) noexcept
        {
            Sha256 hash;
            hash.Update(bytes);
            return hash.Finish();
        }

    private:
        [[nodiscard]] static constexpr u32 Rotate(u32 v, u32 n) noexcept { return (v >> n) | (v << (32 - n)); }

        void Compress() noexcept
        {
            u32 w[64];
            for (usize i = 0; i < 16; ++i)
                w[i] = (static_cast<u32>(m_block[i * 4]) << 24) | (static_cast<u32>(m_block[i * 4 + 1]) << 16) |
                       (static_cast<u32>(m_block[i * 4 + 2]) << 8) | static_cast<u32>(m_block[i * 4 + 3]);
            for (usize i = 16; i < 64; ++i)
            {
                const u32 s0 = Rotate(w[i - 15], 7) ^ Rotate(w[i - 15], 18) ^ (w[i - 15] >> 3);
                const u32 s1 = Rotate(w[i - 2], 17) ^ Rotate(w[i - 2], 19) ^ (w[i - 2] >> 10);
                w[i] = w[i - 16] + s0 + w[i - 7] + s1;
            }
            u32 a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3];
            u32 e = m_state[4], f = m_state[5], g = m_state[6], h = m_state[7];
            for (usize i = 0; i < 64; ++i)
            {
                const u32 s1 = Rotate(e, 6) ^ Rotate(e, 11) ^ Rotate(e, 25);
                const u32 choose = (e & f) ^ (~e & g);
                const u32 t1 = h + s1 + choose + detail::kSha256Rounds[i] + w[i];
                const u32 s0 = Rotate(a, 2) ^ Rotate(a, 13) ^ Rotate(a, 22);
                const u32 majority = (a & b) ^ (a & c) ^ (b & c);
                const u32 t2 = s0 + majority;
                h = g;
                g = f;
                f = e;
                e = d + t1;
                d = c;
                c = b;
                b = a;
                a = t1 + t2;
            }
            m_state[0] += a;
            m_state[1] += b;
            m_state[2] += c;
            m_state[3] += d;
            m_state[4] += e;
            m_state[5] += f;
            m_state[6] += g;
            m_state[7] += h;
        }

        u32 m_state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        u8 m_block[64]{};
        usize m_used = 0;
        u64 m_length = 0;
    };
}
