// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// DDS level decode: block formats through bcdec (the vendored single-header BCn decoder; this
// unit is its one implementation site in the engine), uncompressed texels by swizzle.

module;
#include "Core/Prelude.h"
#include <cstring>
#include <cmath>

#define BCDEC_IMPLEMENTATION
#define BCDEC_STATIC          // internal linkage: no bcdec symbols leave this unit (shared builds)
#define BCDEC_BC4BC5_PRECISE // the signed / unsigned-aware BC4 / BC5 entry points
#include "bcdec.h"

module foundation.image.dds;

import foundation.core;
import foundation.image;

using namespace foundation::core;

namespace foundation::image::dds
{
    namespace
    {
        [[nodiscard]] f32 HalfToFloat(u16 h) noexcept
        {
            const u32 sign = (static_cast<u32>(h) & 0x8000u) << 16;
            u32 exponent = (h >> 10) & 0x1Fu;
            u32 mantissa = h & 0x3FFu;
            u32 bits;
            if (exponent == 0)
            {
                if (mantissa == 0)
                {
                    bits = sign; // signed zero
                }
                else
                {
                    // Subnormal: normalise.
                    exponent = 127 - 15 + 1;
                    while ((mantissa & 0x400u) == 0)
                    {
                        mantissa <<= 1;
                        --exponent;
                    }
                    mantissa &= 0x3FFu;
                    bits = sign | (exponent << 23) | (mantissa << 13);
                }
            }
            else if (exponent == 0x1Fu)
            {
                bits = sign | 0x7F800000u | (mantissa << 13); // inf / nan
            }
            else
            {
                bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
            }
            f32 f;
            std::memcpy(&f, &bits, sizeof(f));
            return f;
        }

        [[nodiscard]] u8 UnitToByte(f32 v) noexcept
        {
            v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            return static_cast<u8>(v * 255.0f + 0.5f);
        }
        [[nodiscard]] u8 SignedToByte(f32 v) noexcept // [-1, 1] -> [0, 255]
        {
            return UnitToByte(v * 0.5f + 0.5f);
        }
        // A two-channel tangent-space normal's Z, so the decoded image is a whole normal map.
        [[nodiscard]] u8 ReconstructZ(u8 r, u8 g) noexcept
        {
            const f32 x = static_cast<f32>(r) / 255.0f * 2.0f - 1.0f;
            const f32 y = static_cast<f32>(g) / 255.0f * 2.0f - 1.0f;
            const f32 zz = 1.0f - x * x - y * y;
            const f32 z = zz > 0.0f ? std::sqrt(zz) : 0.0f;
            return SignedToByte(z);
        }

        [[nodiscard]] ImageColorSpace ColorSpaceFor(const DdsImage& dds) noexcept
        {
            if (IsSrgb(dds.format))
            {
                return ImageColorSpace::Srgb;
            }
            if (dds.colorSpaceKnown)
            {
                return ImageColorSpace::Linear;
            }
            // Legacy header: a colour format is sRGB by the authoring norm, data formats linear.
            switch (dds.format)
            {
            case DdsFormat::RGBA8:
            case DdsFormat::BGRA8:
            case DdsFormat::BC1:
            case DdsFormat::BC2:
            case DdsFormat::BC3:
            case DdsFormat::BC7:
                return ImageColorSpace::Srgb;
            default:
                return ImageColorSpace::Linear;
            }
        }

        // Decode every 4x4 block of `src` through `decodeBlock` (which fills `tile`), then copy
        // the in-bounds texels of the tile into the RGBA8 `dst`.
        template <typename DecodeBlock>
        void DecodeBlocks(const u8* src, u32 blockBytes, u32 width, u32 height, u8* dst,
                          DecodeBlock&& decodeBlock)
        {
            const u32 blocksX = (width + 3u) / 4u;
            const u32 blocksY = (height + 3u) / 4u;
            u8 tile[4 * 4 * 4];
            for (u32 by = 0; by < blocksY; ++by)
            {
                for (u32 bx = 0; bx < blocksX; ++bx)
                {
                    decodeBlock(src + (static_cast<usize>(by) * blocksX + bx) * blockBytes, tile);
                    for (u32 y = 0; y < 4; ++y)
                    {
                        const u32 py = by * 4 + y;
                        if (py >= height)
                        {
                            break;
                        }
                        for (u32 x = 0; x < 4; ++x)
                        {
                            const u32 px = bx * 4 + x;
                            if (px >= width)
                            {
                                break;
                            }
                            std::memcpy(dst + (static_cast<usize>(py) * width + px) * 4,
                                        tile + (y * 4 + x) * 4, 4);
                        }
                    }
                }
            }
        }
    } // namespace

    Status DecodeLevel(const DdsImage& dds, u32 layer, u32 level, Image& out)
    {
        if (layer >= dds.arrayLayers || level >= dds.mipLevels)
        {
            return ErrorCode::OutOfRange;
        }
        const Span<const u8> src = dds.Level(layer, level);
        const u32 width = dds.LevelWidth(level);
        const u32 height = dds.LevelHeight(level);
        if (src.IsEmpty())
        {
            return ErrorCode::InvalidArgument;
        }
        const usize texels = static_cast<usize>(width) * height;

        if (IsHdr(dds.format))
        {
            Image image(width, height, PixelFormat::RGBA32F);
            f32* dst = reinterpret_cast<f32*>(image.PixelDataMut().Data());
            switch (dds.format)
            {
            case DdsFormat::RGBA32F:
                std::memcpy(dst, src.Data(), texels * 16);
                break;
            case DdsFormat::RGBA16F:
            {
                const u16* halves = reinterpret_cast<const u16*>(src.Data());
                for (usize i = 0; i < texels * 4; ++i)
                {
                    u16 h;
                    std::memcpy(&h, halves + i, sizeof(h));
                    dst[i] = HalfToFloat(h);
                }
                break;
            }
            case DdsFormat::BC6HUf:
            case DdsFormat::BC6HSf:
            {
                const int isSigned = dds.format == DdsFormat::BC6HSf ? 1 : 0;
                const u32 blocksX = (width + 3u) / 4u;
                const u32 blocksY = (height + 3u) / 4u;
                f32 tile[4 * 4 * 3];
                for (u32 by = 0; by < blocksY; ++by)
                {
                    for (u32 bx = 0; bx < blocksX; ++bx)
                    {
                        bcdec_bc6h_float(src.Data() + (static_cast<usize>(by) * blocksX + bx) * 16,
                                         tile, 4 * 3, isSigned);
                        for (u32 y = 0; y < 4 && by * 4 + y < height; ++y)
                        {
                            for (u32 x = 0; x < 4 && bx * 4 + x < width; ++x)
                            {
                                f32* p = dst + (static_cast<usize>(by * 4 + y) * width + bx * 4 + x) * 4;
                                p[0] = tile[(y * 4 + x) * 3 + 0];
                                p[1] = tile[(y * 4 + x) * 3 + 1];
                                p[2] = tile[(y * 4 + x) * 3 + 2];
                                p[3] = 1.0f;
                            }
                        }
                    }
                }
                break;
            }
            default:
                return ErrorCode::NotSupported;
            }
            image.SetColorSpace(ImageColorSpace::Linear);
            out = Move(image);
            return ErrorCode::Ok;
        }

        Image image(width, height, PixelFormat::RGBA8);
        u8* dst = image.PixelDataMut().Data();
        switch (dds.format)
        {
        case DdsFormat::RGBA8:
        case DdsFormat::RGBA8Srgb:
            std::memcpy(dst, src.Data(), texels * 4);
            break;
        case DdsFormat::BGRA8:
        case DdsFormat::BGRA8Srgb:
            for (usize i = 0; i < texels; ++i)
            {
                dst[i * 4 + 0] = src[i * 4 + 2];
                dst[i * 4 + 1] = src[i * 4 + 1];
                dst[i * 4 + 2] = src[i * 4 + 0];
                dst[i * 4 + 3] = src[i * 4 + 3];
            }
            break;
        case DdsFormat::R8:
            for (usize i = 0; i < texels; ++i)
            {
                dst[i * 4 + 0] = dst[i * 4 + 1] = dst[i * 4 + 2] = src[i];
                dst[i * 4 + 3] = 255;
            }
            break;
        case DdsFormat::RG8:
            for (usize i = 0; i < texels; ++i)
            {
                dst[i * 4 + 0] = src[i * 2 + 0];
                dst[i * 4 + 1] = src[i * 2 + 1];
                dst[i * 4 + 2] = ReconstructZ(src[i * 2 + 0], src[i * 2 + 1]);
                dst[i * 4 + 3] = 255;
            }
            break;
        case DdsFormat::BC1:
        case DdsFormat::BC1Srgb:
            DecodeBlocks(src.Data(), 8, width, height, dst,
                         [](const u8* block, u8* tile) { bcdec_bc1(block, tile, 4 * 4); });
            break;
        case DdsFormat::BC2:
        case DdsFormat::BC2Srgb:
            DecodeBlocks(src.Data(), 16, width, height, dst,
                         [](const u8* block, u8* tile) { bcdec_bc2(block, tile, 4 * 4); });
            break;
        case DdsFormat::BC3:
        case DdsFormat::BC3Srgb:
            DecodeBlocks(src.Data(), 16, width, height, dst,
                         [](const u8* block, u8* tile) { bcdec_bc3(block, tile, 4 * 4); });
            break;
        case DdsFormat::BC7:
        case DdsFormat::BC7Srgb:
            DecodeBlocks(src.Data(), 16, width, height, dst,
                         [](const u8* block, u8* tile) { bcdec_bc7(block, tile, 4 * 4); });
            break;
        case DdsFormat::BC4:
        case DdsFormat::BC4Snorm:
        {
            const bool isSigned = dds.format == DdsFormat::BC4Snorm;
            DecodeBlocks(src.Data(), 8, width, height, dst,
                         [isSigned](const u8* block, u8* tile)
                         {
                             u8 r[16];
                             if (isSigned)
                             {
                                 f32 rf[16];
                                 bcdec_bc4_float(block, rf, 4, 1);
                                 for (u32 i = 0; i < 16; ++i)
                                 {
                                     r[i] = SignedToByte(rf[i]);
                                 }
                             }
                             else
                             {
                                 bcdec_bc4(block, r, 4, 0);
                             }
                             for (u32 i = 0; i < 16; ++i)
                             {
                                 tile[i * 4 + 0] = tile[i * 4 + 1] = tile[i * 4 + 2] = r[i];
                                 tile[i * 4 + 3] = 255;
                             }
                         });
            break;
        }
        case DdsFormat::BC5:
        case DdsFormat::BC5Snorm:
        {
            const bool isSigned = dds.format == DdsFormat::BC5Snorm;
            DecodeBlocks(src.Data(), 16, width, height, dst,
                         [isSigned](const u8* block, u8* tile)
                         {
                             u8 rg[32];
                             if (isSigned)
                             {
                                 f32 rgf[32];
                                 bcdec_bc5_float(block, rgf, 4 * 2, 1);
                                 for (u32 i = 0; i < 32; ++i)
                                 {
                                     rg[i] = SignedToByte(rgf[i]);
                                 }
                             }
                             else
                             {
                                 bcdec_bc5(block, rg, 4 * 2, 0);
                             }
                             for (u32 i = 0; i < 16; ++i)
                             {
                                 tile[i * 4 + 0] = rg[i * 2 + 0];
                                 tile[i * 4 + 1] = rg[i * 2 + 1];
                                 tile[i * 4 + 2] = ReconstructZ(rg[i * 2 + 0], rg[i * 2 + 1]);
                                 tile[i * 4 + 3] = 255;
                             }
                         });
            break;
        }
        default:
            return ErrorCode::NotSupported;
        }
        image.SetColorSpace(ColorSpaceFor(dds));
        out = Move(image);
        return ErrorCode::Ok;
    }
}
