// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// texture.compression - the BC6H (unsigned half float) encoder.
//
// In-house rather than vendored: richgel999's bc7enc_rdo covers BC1-7 but not BC6H, Basis
// carries a BC6H encoder only inside its UASTC-HDR transcoder, and DirectXTex's is a 3000-line
// DirectXMath dependency. What HDR skies and IBL sources need is the mode real-time encoders
// use, mode 11: one region, two 10-bit endpoints per channel, 4-bit interpolation weights, no
// delta coding. The two-region partitioned modes buy quality on hard edges that radiance maps
// rarely have, and are not implemented.
//
// The decoder's arithmetic (D3D11.3 spec, section 19.5) drives every choice here:
//   unquantize(q, 10 bits, unsigned): q == 0 -> 0; q == 1023 -> 0xFFFF; else (q << 6) + 32
//   interpolate(a, b, w)             : (a * (64 - w) + b * w + 32) >> 6, w from the 4-bit table
//   finish(v)                        : (v * 31) >> 6   = the half-float bit pattern
// so the encoder works in the decoder's 16-bit "internal" domain u (half bits * 64 / 31),
// measures error there (half bits are near-logarithmic, which suits radiance), and only
// converts to the half representation at the boundary. Endpoints start from the bounding
// box, are refined by least squares against the assigned weights, and flat blocks get their
// two endpoints straddling the value so the weights recover the precision 10 bits lose.

module;
#include "Core/Prelude.h"
#include "Core/Debug/Assert.h"

#include <cstring>

module texture.compression;

import foundation.core;
import foundation.rhi;

using namespace foundation::core;

namespace texcomp
{
    namespace
    {
        constexpr i32 kInternalMax = 0xFFFF;
        constexpr i32 kWeights4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

        // float -> IEEE half bit pattern, round to nearest even. Unsigned BC6H: negatives, NaN
        // and -inf clamp to 0; above the largest finite half clamps to it.
        u16 FloatToHalfBits(f32 value) noexcept
        {
            if (!(value == value) || value <= 0.0f)
            {
                return 0;
            }
            if (value >= 65504.0f)
            {
                return 0x7BFF;
            }
            u32 bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            const u32 exponent = (bits >> 23) & 0xFFu;
            u32 mantissa = bits & 0x7FFFFFu;
            const i32 e = static_cast<i32>(exponent) - 127 + 15;
            if (e <= 0)
            {
                if (e < -10)
                {
                    return 0;
                }
                mantissa |= 0x800000u;
                const u32 shift = static_cast<u32>(14 - e);
                u32 half = mantissa >> shift;
                const u32 remainder = mantissa & ((1u << shift) - 1u);
                const u32 halfway = 1u << (shift - 1);
                if (remainder > halfway || (remainder == halfway && (half & 1u)))
                {
                    ++half;
                }
                return static_cast<u16>(half);
            }
            u32 half = (static_cast<u32>(e) << 10) | (mantissa >> 13);
            const u32 remainder = mantissa & 0x1FFFu;
            if (remainder > 0x1000u || (remainder == 0x1000u && (half & 1u)))
            {
                ++half;
            }
            return static_cast<u16>(half > 0x7BFFu ? 0x7BFFu : half);
        }

        // Half bits -> the decoder's internal domain (inverse of finish: v such that
        // (v * 31) >> 6 == h).
        i32 HalfToInternal(u16 half) noexcept
        {
            const i32 v = (static_cast<i32>(half) * 64 + 30) / 31;
            return v > kInternalMax ? kInternalMax : v;
        }

        i32 Unquantize10(i32 q) noexcept
        {
            if (q <= 0)
            {
                return 0;
            }
            if (q >= 1023)
            {
                return kInternalMax;
            }
            return (q << 6) + 32;
        }

        // Nearest 10-bit endpoint for an internal value (representable values sit at 64q + 32).
        i32 Quantize10(i32 v) noexcept
        {
            if (v <= 0)
            {
                return 0;
            }
            if (v >= kInternalMax)
            {
                return 1023;
            }
            const i32 q = v >> 6;
            return q > 1023 ? 1023 : q;
        }

        i32 Interpolate(i32 a, i32 b, i32 w) noexcept { return (a * (64 - w) + b * w + 32) >> 6; }

        struct Endpoints
        {
            i32 e0[3];
            i32 e1[3];
        };

        struct Quantized
        {
            i32 q0[3];
            i32 q1[3];
        };

        Quantized Quantize(const Endpoints& ep) noexcept
        {
            Quantized q{};
            for (i32 c = 0; c < 3; ++c)
            {
                q.q0[c] = Quantize10(ep.e0[c]);
                q.q1[c] = Quantize10(ep.e1[c]);
            }
            return q;
        }

        Endpoints Unquantize(const Quantized& q) noexcept
        {
            Endpoints ep{};
            for (i32 c = 0; c < 3; ++c)
            {
                ep.e0[c] = Unquantize10(q.q0[c]);
                ep.e1[c] = Unquantize10(q.q1[c]);
            }
            return ep;
        }

        // Each texel takes the 4-bit weight nearest its projection onto the e0 -> e1 segment.
        void AssignWeights(const i32 (&u)[16][3], const Endpoints& ep, u8 (&index)[16]) noexcept
        {
            f32 d[3];
            f32 length2 = 0.0f;
            for (i32 c = 0; c < 3; ++c)
            {
                d[c] = static_cast<f32>(ep.e1[c] - ep.e0[c]);
                length2 += d[c] * d[c];
            }
            for (i32 i = 0; i < 16; ++i)
            {
                if (length2 <= 0.0f)
                {
                    index[i] = 0;
                    continue;
                }
                f32 t = 0.0f;
                for (i32 c = 0; c < 3; ++c)
                {
                    t += static_cast<f32>(u[i][c] - ep.e0[c]) * d[c];
                }
                t /= length2;
                t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
                index[i] = static_cast<u8>(static_cast<i32>(t * 15.0f + 0.5f));
            }
        }

        u64 BlockError(const i32 (&u)[16][3], const Endpoints& ep, const u8 (&index)[16]) noexcept
        {
            u64 error = 0;
            for (i32 i = 0; i < 16; ++i)
            {
                const i32 w = kWeights4[index[i]];
                for (i32 c = 0; c < 3; ++c)
                {
                    const i64 diff = Interpolate(ep.e0[c], ep.e1[c], w) - u[i][c];
                    error += static_cast<u64>(diff * diff);
                }
            }
            return error;
        }

        // Least-squares endpoints for fixed weights (the classic 2x2 normal equations, per
        // channel). False when the weights carry no spread (all texels on one endpoint).
        bool SolveEndpoints(const i32 (&u)[16][3], const u8 (&index)[16], Endpoints& ep) noexcept
        {
            f64 alpha2 = 0.0, beta2 = 0.0, alphaBeta = 0.0;
            f64 alphaX[3] = {0.0, 0.0, 0.0}, betaX[3] = {0.0, 0.0, 0.0};
            for (i32 i = 0; i < 16; ++i)
            {
                const f64 b = static_cast<f64>(kWeights4[index[i]]) / 64.0;
                const f64 a = 1.0 - b;
                alpha2 += a * a;
                beta2 += b * b;
                alphaBeta += a * b;
                for (i32 c = 0; c < 3; ++c)
                {
                    alphaX[c] += a * u[i][c];
                    betaX[c] += b * u[i][c];
                }
            }
            const f64 det = alpha2 * beta2 - alphaBeta * alphaBeta;
            if (det < 1.0e-6 && det > -1.0e-6)
            {
                return false;
            }
            for (i32 c = 0; c < 3; ++c)
            {
                const f64 e0 = (alphaX[c] * beta2 - betaX[c] * alphaBeta) / det;
                const f64 e1 = (betaX[c] * alpha2 - alphaX[c] * alphaBeta) / det;
                ep.e0[c] = static_cast<i32>(e0 < 0.0 ? 0.0 : (e0 > kInternalMax ? kInternalMax : e0 + 0.5));
                ep.e1[c] = static_cast<i32>(e1 < 0.0 ? 0.0 : (e1 > kInternalMax ? kInternalMax : e1 + 0.5));
            }
            return true;
        }

        struct Candidate
        {
            Quantized q{};
            u8 index[16] = {};
            u64 error = ~0ull;
        };

        void Consider(const i32 (&u)[16][3], const Endpoints& ep, Candidate& best) noexcept
        {
            const Quantized q = Quantize(ep);
            const Endpoints unq = Unquantize(q);
            u8 index[16];
            AssignWeights(u, unq, index);
            const u64 error = BlockError(u, unq, index);
            if (error < best.error)
            {
                best.error = error;
                best.q = q;
                std::memcpy(best.index, index, sizeof(index));
            }
        }

        // LSB-first bit writer over the 128-bit block.
        struct BitWriter
        {
            u8* out;
            u32 position = 0;
            void Write(u32 value, u32 bits) noexcept
            {
                for (u32 i = 0; i < bits; ++i)
                {
                    if ((value >> i) & 1u)
                    {
                        out[(position + i) >> 3] |= static_cast<u8>(1u << ((position + i) & 7u));
                    }
                }
                position += bits;
            }
        };

        // 16 texels of RGBA32F (4 floats each) -> one 16-byte BC6H unsigned block, mode 11.
        void EncodeBlockMode11(const f32* rgba, u8 quality, u8* out) noexcept
        {
            i32 u[16][3];
            i32 lo[3] = {kInternalMax, kInternalMax, kInternalMax};
            i32 hi[3] = {0, 0, 0};
            for (i32 i = 0; i < 16; ++i)
            {
                for (i32 c = 0; c < 3; ++c)
                {
                    u[i][c] = HalfToInternal(FloatToHalfBits(rgba[i * 4 + c]));
                    lo[c] = u[i][c] < lo[c] ? u[i][c] : lo[c];
                    hi[c] = u[i][c] > hi[c] ? u[i][c] : hi[c];
                }
            }

            Candidate best;

            // 1. The bounding-box diagonal, refined by least squares `passes` times.
            Endpoints ep{{lo[0], lo[1], lo[2]}, {hi[0], hi[1], hi[2]}};
            const i32 passes = 1 + static_cast<i32>(quality) / 64; // 1..4
            for (i32 pass = 0; pass < passes; ++pass)
            {
                Consider(u, ep, best);
                u8 index[16];
                AssignWeights(u, ep, index);
                if (!SolveEndpoints(u, index, ep))
                {
                    break;
                }
            }

            // 2. Flat and near-flat blocks (most of a sky). One shared weight per texel cannot
            //    satisfy three channels whose quantization residues differ, so instead fix every
            //    weight at the table's first step (w = 4) and give each channel its own second
            //    endpoint: interpolate(a, b, 4) = 60a/64 + 4b/64, so with a = 64q + 32 and
            //    b = a + 64d the reconstruction is 64q + 32 + 4d, a 4-unit grid the 10-bit
            //    endpoints alone (a 64-unit grid) cannot reach. Considered whenever the block's
            //    range fits inside one quantization step; the error check keeps it only if it wins.
            bool narrow = true;
            for (i32 c = 0; c < 3; ++c)
            {
                narrow = narrow && (hi[c] - lo[c]) <= 64;
            }
            if (narrow)
            {
                Quantized flat{};
                for (i32 c = 0; c < 3; ++c)
                {
                    const i32 mid = (lo[c] + hi[c]) / 2;
                    i32 q0 = Quantize10(mid);
                    if (q0 == 0)
                    {
                        q0 = 1; // keep the endpoint on the 64q + 32 grid (q = 0 decodes to 0)
                    }
                    const i32 residual = mid - Unquantize10(q0);            // [-32, 32)
                    const i32 d = (residual + (residual >= 0 ? 2 : -2)) / 4; // nearest 4-unit step
                    i32 q1 = q0 + d;
                    q1 = q1 < 1 ? 1 : (q1 > 1022 ? 1022 : q1);
                    flat.q0[c] = q0;
                    flat.q1[c] = q1;
                }
                const Endpoints unq = Unquantize(flat);
                u8 index[16];
                for (u8& w : index)
                {
                    w = 1;
                }
                const u64 error = BlockError(u, unq, index);
                if (error < best.error)
                {
                    best.error = error;
                    best.q = flat;
                    std::memcpy(best.index, index, sizeof(index));
                }
            }

            // 3. The anchor index (texel 0) stores only 3 bits: its top bit must be 0. Swapping
            //    the endpoints mirrors every weight, which the interpolation table allows.
            Quantized q = best.q;
            u8 index[16];
            std::memcpy(index, best.index, sizeof(index));
            if (index[0] >= 8)
            {
                for (i32 c = 0; c < 3; ++c)
                {
                    const i32 t = q.q0[c];
                    q.q0[c] = q.q1[c];
                    q.q1[c] = t;
                }
                for (u8& w : index)
                {
                    w = static_cast<u8>(15 - w);
                }
            }

            // 4. Pack: mode 11 = 0b00011 (5 bits), then rw gw bw rx gx bx at 10 bits each, then
            //    the anchor's 3 bits and fifteen 4-bit indices = 128 bits exactly.
            std::memset(out, 0, 16);
            BitWriter writer{out};
            writer.Write(0x03u, 5);
            writer.Write(static_cast<u32>(q.q0[0]), 10);
            writer.Write(static_cast<u32>(q.q0[1]), 10);
            writer.Write(static_cast<u32>(q.q0[2]), 10);
            writer.Write(static_cast<u32>(q.q1[0]), 10);
            writer.Write(static_cast<u32>(q.q1[1]), 10);
            writer.Write(static_cast<u32>(q.q1[2]), 10);
            writer.Write(index[0], 3);
            for (i32 i = 1; i < 16; ++i)
            {
                writer.Write(index[i], 4);
            }
            DIAGNOSTIC_ASSERT_MSG(writer.position == 128, "BC6H block must pack to 128 bits");
        }
    }

    Array<byte> EncodeBlockCompressedHdr(const f32* rgba, u32 width, u32 height, u8 quality)
    {
        Array<byte> out;
        if (rgba == nullptr || width == 0 || height == 0)
        {
            return out;
        }
        const u32 blocksX = (width + 3) / 4;
        const u32 blocksY = (height + 3) / 4;
        out.Resize(rhi::CompressedLevelBytes(rhi::TextureFormat::BC6HRGBUfloat, width, height));
        usize offset = 0;
        f32 block[16 * 4];
        for (u32 by = 0; by < blocksY; ++by)
        {
            for (u32 bx = 0; bx < blocksX; ++bx)
            {
                for (u32 y = 0; y < 4; ++y)
                {
                    const u32 sy = Min(by * 4 + y, height - 1);
                    for (u32 x = 0; x < 4; ++x)
                    {
                        const u32 sx = Min(bx * 4 + x, width - 1);
                        const f32* src = rgba + (static_cast<usize>(sy) * width + sx) * 4;
                        f32* dst = block + (y * 4 + x) * 4;
                        dst[0] = src[0];
                        dst[1] = src[1];
                        dst[2] = src[2];
                        dst[3] = src[3];
                    }
                }
                EncodeBlockMode11(block, quality, reinterpret_cast<u8*>(out.Data() + offset));
                offset += 16;
            }
        }
        return out;
    }
}
