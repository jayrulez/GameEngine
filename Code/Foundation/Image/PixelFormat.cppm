// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Pixel format enum for CPU-side image data.

export module foundation.image:pixel_format;

import foundation.core;

using namespace foundation::core;

export namespace foundation::image
{

    enum class PixelFormat : u32
    {
        R8,
        RG8,
        RGB8,
        RGBA8,
        R16F,
        RG16F,
        RGB16F,
        RGBA16F,
        R32F,
        RG32F,
        RGB32F,
        RGBA32F,
        BGR8,
        BGRA8,
        R16, // unsigned 16-bit single channel (heightmaps); appended to keep enum values stable
    };

    [[nodiscard]] constexpr u32 BytesPerPixel(PixelFormat f)
    {
        switch (f)
        {
        case PixelFormat::R8:
            return 1;
        case PixelFormat::RG8:
            return 2;
        case PixelFormat::RGB8:
            return 3;
        case PixelFormat::BGR8:
            return 3;
        case PixelFormat::RGBA8:
            return 4;
        case PixelFormat::BGRA8:
            return 4;
        case PixelFormat::R16:
            return 2;
        case PixelFormat::R16F:
            return 2;
        case PixelFormat::RG16F:
            return 4;
        case PixelFormat::RGB16F:
            return 6;
        case PixelFormat::RGBA16F:
            return 8;
        case PixelFormat::R32F:
            return 4;
        case PixelFormat::RG32F:
            return 8;
        case PixelFormat::RGB32F:
            return 12;
        case PixelFormat::RGBA32F:
            return 16;
        }
        return 4;
    }

    [[nodiscard]] constexpr u32 ChannelCount(PixelFormat f)
    {
        switch (f)
        {
        case PixelFormat::R8:
        case PixelFormat::R16:
        case PixelFormat::R16F:
        case PixelFormat::R32F:
            return 1;
        case PixelFormat::RG8:
        case PixelFormat::RG16F:
        case PixelFormat::RG32F:
            return 2;
        case PixelFormat::RGB8:
        case PixelFormat::BGR8:
        case PixelFormat::RGB16F:
        case PixelFormat::RGB32F:
            return 3;
        case PixelFormat::RGBA8:
        case PixelFormat::BGRA8:
        case PixelFormat::RGBA16F:
        case PixelFormat::RGBA32F:
            return 4;
        }
        return 0;
    }

    [[nodiscard]] constexpr bool HasAlpha(PixelFormat f)
    {
        return f == PixelFormat::RGBA8 || f == PixelFormat::BGRA8 || f == PixelFormat::RGBA16F ||
               f == PixelFormat::RGBA32F;
    }

    /// IEEE 754 binary16 -> f32, exact: sign, the five exponent bits and the ten mantissa bits
    /// re-based (subnormals normalised, infinities and NaNs kept). The decoder behind every
    /// 16-bit float pixel format read on the CPU (a render target read back for a thumbnail
    /// or a viewport capture).
    [[nodiscard]] constexpr f32 HalfToFloat(u16 h) noexcept
    {
        const u32 sign = static_cast<u32>(h >> 15) & 1u;
        const u32 exponent = static_cast<u32>(h >> 10) & 0x1Fu;
        const u32 mantissa = static_cast<u32>(h) & 0x3FFu;
        u32 bits;
        if (exponent == 0)
        {
            if (mantissa == 0)
            {
                bits = sign << 31; // signed zero
            }
            else
            {
                // Subnormal half: normalise into a float exponent.
                u32 e = 127 - 15 + 1;
                u32 m = mantissa;
                while ((m & 0x400u) == 0)
                {
                    m <<= 1;
                    --e;
                }
                bits = (sign << 31) | (e << 23) | ((m & 0x3FFu) << 13);
            }
        }
        else if (exponent == 0x1F)
        {
            bits = (sign << 31) | 0x7F800000u | (mantissa << 13); // inf / nan
        }
        else
        {
            bits = (sign << 31) | ((exponent - 15 + 127) << 23) | (mantissa << 13);
        }
        return __builtin_bit_cast(f32, bits);
    }

    /// A 16-bit float channel as an 8-bit one: clamped to [0, 1] and quantised. No encoding:
    /// a display-referred 16F target (a tonemapped viewport) already holds encoded values.
    [[nodiscard]] constexpr u8 HalfToUnorm8(u16 h) noexcept
    {
        const f32 value = HalfToFloat(h);
        const f32 clamped = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
        return static_cast<u8>(clamped * 255.0f + 0.5f);
    }

} // namespace foundation::image
