// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// texture.compression impl - the encoder dispatch and policy table. The heavy encoder headers live
// only here, out of the module interface. bc7enc does BC7; rgbcx does BC1/3/4/5.

module;
#include "Core/Prelude.h"
#include "bc7enc.h"
#include "rgbcx.h"
#include "astcenc.h"

module texture.compression;

import foundation.core;
import foundation.rhi;

using namespace foundation::core;

namespace texcomp
{
    namespace rhi = foundation::rhi;

    // The encoders need a one-time global table init before first use.
    static void EnsureInit()
    {
        static const bool once = []()
        {
            bc7enc_compress_block_init();
            rgbcx::init(rgbcx::bc1_approx_mode::cBC1Ideal);
            return true;
        }();
        (void)once;
    }

    // Gather the 4x4 RGBA block at block coord (bx, by), clamping source coords so edge/NPOT/small
    // levels pad with their border texels (the encoder handles sub-block levels this way).
    static void GatherBlock(const u8* rgba, u32 w, u32 h, u32 bx, u32 by, u8 out[64]) noexcept
    {
        for (u32 py = 0; py < 4; ++py)
        {
            const u32 sy = Min(by * 4 + py, h - 1);
            for (u32 px = 0; px < 4; ++px)
            {
                const u32 sx = Min(bx * 4 + px, w - 1);
                const u8* src = rgba + (static_cast<usize>(sy) * w + sx) * 4;
                u8* dst = out + (static_cast<usize>(py) * 4 + px) * 4;
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
                dst[3] = src[3];
            }
        }
    }

    usize BlockCompressedSize(rhi::TextureFormat format, u32 width, u32 height) noexcept
    {
        return rhi::CompressedLevelBytes(format, width, height);
    }

    rhi::TextureFormat ResolveCompressedFormat(TextureUsage usage, bool sRGB, bool hasAlpha,
                                               bool multiChannel,
                                               CompressionChoice choice, u32 width, u32 height,
                                               const TargetProfile& profile,
                                               rhi::TextureFormat uncompressed) noexcept
    {
        using F = rhi::TextureFormat;
        // Escape hatches: authored None, small/UI/pixel-art; HDR has its own row (BC6H).
        if (choice == CompressionChoice::None)
        {
            return uncompressed;
        }
        if (width <= 64 && height <= 64)
        {
            return uncompressed;
        }
        if (usage == TextureUsage::HDR)
        {
            // BC6H (unsigned) is part of the same BC feature we already require on desktop and
            // desktop browsers; there is no ASTC-HDR path, so other targets keep HDR uncompressed.
            return profile.bc ? F::BC6HRGBUfloat : uncompressed;
        }

        if (profile.bc) // desktop and desktop browsers: prefer BC (checked first if a profile had both)
        {
            switch (usage)
            {
            case TextureUsage::Color:
                if (hasAlpha || choice == CompressionChoice::Quality)
                {
                    return sRGB ? F::BC7RGBAUnormSrgb : F::BC7RGBAUnorm;
                }
                return sRGB ? F::BC1RGBAUnormSrgb : F::BC1RGBAUnorm; // opaque, Default
            case TextureUsage::Normal:
                // BC7-linear, not BC5: the shaders decode rgb * 2 - 1 and BC5 has no B
                // (z decodes to -1, so normals shade white). Same 1 byte/texel as BC5;
                // BC5 needs the shaders to reconstruct z first.
                return F::BC7RGBAUnorm;
            case TextureUsage::Mask:
                // A packed ORM/ARM authored as Mask carries three meaningful channels;
                // BC4 would keep R only. The cook's channel sniff routes those to
                // BC7-linear; true single-channel masks (rough/AO/height) stay BC4.
                return multiChannel ? F::BC7RGBAUnorm : F::BC4RUnorm;
            case TextureUsage::HDR:
                return uncompressed; // handled above
            }
        }
        if (profile.astc) // mobile browsers: one 4x4 format covers every LDR usage (alpha built in)
        {
            // ASTC has no per-usage format split like BC: the 4x4 block encodes RGBA at ~8 bpp, and
            // the encoder is told the semantic (normal/perceptual) via flags at encode time. sRGB
            // only meaningfully applies to color; Normal/Mask stay linear.
            return (sRGB && usage == TextureUsage::Color) ? F::ASTC4x4UnormSrgb : F::ASTC4x4Unorm;
        }
        // ETC2 and any other family stay uncompressed.
        return uncompressed;
    }

    bool HasDistinctChannels(const u8* rgba, u32 width, u32 height, u8 tolerance) noexcept
    {
        if (rgba == nullptr)
        {
            return false;
        }
        const usize texels = static_cast<usize>(width) * height;
        const i32 tol = static_cast<i32>(tolerance);
        for (usize i = 0; i < texels; ++i)
        {
            const i32 r = rgba[i * 4 + 0];
            const i32 g = rgba[i * 4 + 1];
            const i32 b = rgba[i * 4 + 2];
            if ((g > r ? g - r : r - g) > tol || (b > r ? b - r : r - b) > tol)
            {
                return true;
            }
        }
        return false;
    }

    // ASTC is a whole-image codec (not block-by-block like BC): astcenc takes the full RGBA8 level
    // and writes the packed 4x4 blocks. Single-threaded (cook-time, not a hot path). Returns empty
    // on any astcenc error. `srgb` selects the LDR_SRGB profile so error is weighted in sRGB space.
    static Array<byte> EncodeAstc(const u8* rgba, u32 width, u32 height, bool srgb, u8 quality)
    {
        Array<byte> out;
        const astcenc_profile profile = srgb ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR;
        const float effort = ASTCENC_PRE_FASTEST +
                             (static_cast<float>(quality) / 255.0f) *
                                 (ASTCENC_PRE_THOROUGH - ASTCENC_PRE_FASTEST);
        astcenc_config config{};
        if (astcenc_config_init(profile, 4, 4, 1, effort, 0, &config) != ASTCENC_SUCCESS)
        {
            return out;
        }
        astcenc_context* context = nullptr;
        if (astcenc_context_alloc(&config, 1, &context, nullptr) != ASTCENC_SUCCESS)
        {
            return out;
        }

        // astcenc wants a non-const slice-pointer array; the codec only reads it when compressing.
        void* slice = const_cast<u8*>(rgba);
        astcenc_image image{};
        image.dim_x = width;
        image.dim_y = height;
        image.dim_z = 1;
        image.data_type = ASTCENC_TYPE_U8;
        image.data = &slice;
        const astcenc_swizzle swizzle{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};

        out.Resize(BlockCompressedSize(rhi::TextureFormat::ASTC4x4Unorm, width, height));
        const astcenc_error err = astcenc_compress_image(
            context, &image, &swizzle, reinterpret_cast<u8*>(out.Data()), out.Size(), 0);
        astcenc_context_free(context);
        if (err != ASTCENC_SUCCESS)
        {
            return Array<byte>{};
        }
        return out;
    }

    Array<byte> EncodeBlockCompressed(const u8* rgba, u32 width, u32 height, rhi::TextureFormat format,
                                      u8 quality, JobSystem* jobs)
    {
        Array<byte> out;
        if (rgba == nullptr || width == 0 || height == 0 || !rhi::IsCompressed(format))
        {
            return out;
        }
        // ASTC has its own whole-image codec path.
        if (format == rhi::TextureFormat::ASTC4x4Unorm ||
            format == rhi::TextureFormat::ASTC4x4UnormSrgb)
        {
            return EncodeAstc(rgba, width, height, format == rhi::TextureFormat::ASTC4x4UnormSrgb,
                              quality);
        }
        EnsureInit();
        const u32 bx = (width + 3) / 4;
        const u32 by = (height + 3) / 4;
        usize blockBytes = 0;
        switch (format)
        {
        case rhi::TextureFormat::BC1RGBAUnorm:
        case rhi::TextureFormat::BC1RGBAUnormSrgb:
        case rhi::TextureFormat::BC4RUnorm:
        case rhi::TextureFormat::BC4RSnorm:
            blockBytes = 8;
            break;
        case rhi::TextureFormat::BC3RGBAUnorm:
        case rhi::TextureFormat::BC3RGBAUnormSrgb:
        case rhi::TextureFormat::BC5RGUnorm:
        case rhi::TextureFormat::BC5RGSnorm:
        case rhi::TextureFormat::BC7RGBAUnorm:
        case rhi::TextureFormat::BC7RGBAUnormSrgb:
            blockBytes = 16;
            break;
        default:
            return Array<byte>{}; // not an RGBA8 block format (BC6H goes through EncodeBlockCompressedHdr)
        }
        out.Resize(BlockCompressedSize(format, width, height));
        // rgbcx level 0..18 (BC1/3); bc7enc uber level 0..4. Map both from `quality` 0..255.
        const u32 rgbcxLevel = (static_cast<u32>(quality) * rgbcx::MAX_LEVEL) / 255u;
        bc7enc_compress_block_params bc7params;
        bc7enc_compress_block_params_init(&bc7params);
        // Default (128) is bc7enc's uber 0, its own default, and about 4x faster than uber 2 for a
        // fraction of a dB; Quality (255) is uber 4, the export-grade effort.
        bc7params.m_uber_level = quality >= 255u ? 4u : (quality >= 192u ? 2u : 0u);
        // One block row per work item: each row writes its own slice of `out`, every block is
        // encoded on its own (the encoders keep no state past EnsureInit), so the fan-out is
        // byte-identical to the inline loop.
        byte* const outBytes = out.Data();
        const auto encodeRow = [&](u32 y)
        {
            u8 block[64];
            byte* dst = outBytes + static_cast<usize>(y) * bx * blockBytes;
            for (u32 x = 0; x < bx; ++x, dst += blockBytes)
            {
                GatherBlock(rgba, width, height, x, y, block);
                switch (format)
                {
                case rhi::TextureFormat::BC1RGBAUnorm:
                case rhi::TextureFormat::BC1RGBAUnormSrgb:
                    rgbcx::encode_bc1(rgbcxLevel, dst, block, /*allow_3color*/ true,
                                      /*use_transparent_texels_for_black*/ false);
                    break;
                case rhi::TextureFormat::BC3RGBAUnorm:
                case rhi::TextureFormat::BC3RGBAUnormSrgb:
                    rgbcx::encode_bc3(rgbcxLevel, dst, block);
                    break;
                case rhi::TextureFormat::BC4RUnorm:
                case rhi::TextureFormat::BC4RSnorm:
                    rgbcx::encode_bc4(dst, block, /*stride*/ 4);
                    break;
                case rhi::TextureFormat::BC5RGUnorm:
                case rhi::TextureFormat::BC5RGSnorm:
                    rgbcx::encode_bc5(dst, block, /*chan0*/ 0, /*chan1*/ 1, /*stride*/ 4);
                    break;
                default: // BC7
                    bc7enc_compress_block(dst, block, &bc7params);
                    break;
                }
            }
        };
        if (jobs != nullptr && by > 1)
        {
            jobs->ParallelFor(by, [&](u32 y) { encodeRow(y); });
        }
        else
        {
            for (u32 y = 0; y < by; ++y)
            {
                encodeRow(y);
            }
        }
        return out;
    }
}
