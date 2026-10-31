// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// texture.compression tests - the policy table (pure logic), exact block sizes, and a real
// encode and decode round-trip whose PSNR shows the encoders actually run and reconstruct. Decoding
// uses the vendored bc7enc/rgbcx decoders directly (test-only; the module itself only encodes).

#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "rgbcx.h"
#include "bc7decomp.h"
#include "astcenc.h"
#define BCDEC_IMPLEMENTATION
#include "bcdec.h"
#include <cmath>

import foundation.core;
import foundation.rhi;
import texture.compression;

using namespace foundation::core;
namespace rhi = foundation::rhi;
using namespace texcomp;

namespace
{
    // A deterministic width*height RGBA8 test image: smooth channel ramps (compressible) with a
    // touch of block-local variation so the encoder has real work to do.
    Array<u8> MakeImage(u32 w, u32 h, bool alpha)
    {
        Array<u8> px;
        px.Resize(static_cast<usize>(w) * h * 4);
        for (u32 y = 0; y < h; ++y)
        {
            for (u32 x = 0; x < w; ++x)
            {
                u8* p = px.Data() + (static_cast<usize>(y) * w + x) * 4;
                p[0] = static_cast<u8>((x * 255) / (w - 1));
                p[1] = static_cast<u8>((y * 255) / (h - 1));
                p[2] = static_cast<u8>(((x + y) * 255) / (w + h - 2));
                p[3] = alpha ? static_cast<u8>((x * 255) / (w - 1)) : 255;
            }
        }
        return px;
    }

    // PSNR (dB) between two equal-size RGBA8 buffers over the given channel count.
    double Psnr(const u8* a, const u8* b, usize pixels, u32 channels)
    {
        double sse = 0.0;
        for (usize i = 0; i < pixels; ++i)
        {
            for (u32 c = 0; c < channels; ++c)
            {
                const double d = static_cast<double>(a[i * 4 + c]) - static_cast<double>(b[i * 4 + c]);
                sse += d * d;
            }
        }
        if (sse <= 0.0)
        {
            return 99.0;
        }
        const double mse = sse / (static_cast<double>(pixels) * channels);
        return 10.0 * std::log10((255.0 * 255.0) / mse);
    }

    // Decode BC1/BC7 back to a tightly-packed RGBA8 image so we can measure reconstruction error.
    Array<u8> DecodeBc(const Array<byte>& blocks, u32 w, u32 h, rhi::TextureFormat fmt)
    {
        rgbcx::init(rgbcx::bc1_approx_mode::cBC1Ideal);
        Array<u8> out;
        out.Resize(static_cast<usize>(w) * h * 4);
        const u32 bx = (w + 3) / 4;
        const u32 by = (h + 3) / 4;
        const usize blockBytes = (fmt == rhi::TextureFormat::BC1RGBAUnorm) ? 8u : 16u;
        const byte* src = blocks.Data();
        for (u32 gy = 0; gy < by; ++gy)
        {
            for (u32 gx = 0; gx < bx; ++gx)
            {
                u8 tile[64];
                const void* blk = src + (static_cast<usize>(gy) * bx + gx) * blockBytes;
                if (fmt == rhi::TextureFormat::BC1RGBAUnorm)
                {
                    rgbcx::unpack_bc1(blk, tile, true, rgbcx::bc1_approx_mode::cBC1Ideal);
                }
                else
                {
                    bc7decomp::unpack_bc7(blk, reinterpret_cast<bc7decomp::color_rgba*>(tile));
                }
                for (u32 py = 0; py < 4; ++py)
                {
                    const u32 sy = gy * 4 + py;
                    if (sy >= h) { break; }
                    for (u32 px = 0; px < 4; ++px)
                    {
                        const u32 sx = gx * 4 + px;
                        if (sx >= w) { break; }
                        u8* d = out.Data() + (static_cast<usize>(sy) * w + sx) * 4;
                        const u8* s = tile + (static_cast<usize>(py) * 4 + px) * 4;
                        d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
                    }
                }
            }
        }
        return out;
    }

    // Decode ASTC 4x4 back to RGBA8 via astcenc (test-only; the module only encodes).
    Array<u8> DecodeAstc(const Array<byte>& blocks, u32 w, u32 h, bool srgb)
    {
        Array<u8> out;
        astcenc_config config{};
        const astcenc_profile profile = srgb ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR;
        if (astcenc_config_init(profile, 4, 4, 1, ASTCENC_PRE_MEDIUM, 0, &config) != ASTCENC_SUCCESS)
        {
            return out;
        }
        astcenc_context* ctx = nullptr;
        if (astcenc_context_alloc(&config, 1, &ctx, nullptr) != ASTCENC_SUCCESS)
        {
            return out;
        }
        out.Resize(static_cast<usize>(w) * h * 4);
        void* slice = out.Data();
        astcenc_image image{};
        image.dim_x = w;
        image.dim_y = h;
        image.dim_z = 1;
        image.data_type = ASTCENC_TYPE_U8;
        image.data = &slice;
        const astcenc_swizzle swizzle{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
        const astcenc_error err = astcenc_decompress_image(
            ctx, reinterpret_cast<const uint8_t*>(blocks.Data()), blocks.Size(), &image, &swizzle, 0);
        astcenc_context_free(ctx);
        if (err != ASTCENC_SUCCESS)
        {
            out.Clear();
        }
        return out;
    }
}

TEST_CASE("ResolveCompressedFormat - Decision 5 policy table (BC profile)")
{
    const TargetProfile bc = DesktopProfile();
    const auto uncompressed = rhi::TextureFormat::RGBA8Unorm;

    // Color: opaque Default is BC1; alpha is BC7; Quality forces BC7 even opaque; sRGB variants.
    CHECK(ResolveCompressedFormat(TextureUsage::Color, false, false, false, CompressionChoice::Default, 256, 256, bc, uncompressed) == rhi::TextureFormat::BC1RGBAUnorm);
    CHECK(ResolveCompressedFormat(TextureUsage::Color, true, false, false, CompressionChoice::Default, 256, 256, bc, uncompressed) == rhi::TextureFormat::BC1RGBAUnormSrgb);
    CHECK(ResolveCompressedFormat(TextureUsage::Color, false, true, false, CompressionChoice::Default, 256, 256, bc, uncompressed) == rhi::TextureFormat::BC7RGBAUnorm);
    CHECK(ResolveCompressedFormat(TextureUsage::Color, true, true, false, CompressionChoice::Default, 256, 256, bc, uncompressed) == rhi::TextureFormat::BC7RGBAUnormSrgb);
    CHECK(ResolveCompressedFormat(TextureUsage::Color, false, false, false, CompressionChoice::Quality, 256, 256, bc, uncompressed) == rhi::TextureFormat::BC7RGBAUnorm);

    // Normal is BC7-linear, Mask is BC4 (both linear, ignore sRGB).
    CHECK(ResolveCompressedFormat(TextureUsage::Normal, false, false, false, CompressionChoice::Default, 256, 256, bc, uncompressed) == rhi::TextureFormat::BC7RGBAUnorm);
    CHECK(ResolveCompressedFormat(TextureUsage::Mask, false, false, false, CompressionChoice::Default, 256, 256, bc, uncompressed) == rhi::TextureFormat::BC4RUnorm);
    // Packed ORM/ARM authored as Mask: distinct channels give BC7-linear, never channel-dropping BC4.
    CHECK(ResolveCompressedFormat(TextureUsage::Mask, false, false, true, CompressionChoice::Default, 256, 256, bc, uncompressed) == rhi::TextureFormat::BC7RGBAUnorm);

    // Escape hatches to uncompressed: authored None, small (<=64px). HDR has its own row (BC6H).
    CHECK(ResolveCompressedFormat(TextureUsage::Color, true, true, false, CompressionChoice::None, 256, 256, bc, uncompressed) == uncompressed);
    CHECK(ResolveCompressedFormat(TextureUsage::Color, true, false, false, CompressionChoice::Default, 64, 64, bc, uncompressed) == uncompressed);
    CHECK(ResolveCompressedFormat(TextureUsage::HDR, false, false, false,
                                  CompressionChoice::Default, 256, 256, bc, uncompressed) ==
          rhi::TextureFormat::BC6HRGBUfloat); // HDR row: BC6H

    // No supported family: uncompressed regardless of usage.
    const TargetProfile none{false, false, false};
    CHECK(ResolveCompressedFormat(TextureUsage::Color, false, true, false, CompressionChoice::Default, 256, 256, none, uncompressed) == uncompressed);
}

TEST_CASE("BlockCompressedSize - exact 4x4 block-ceil bytes")
{
    // 256x256 is 64x64 blocks, 4096 blocks. BC1/BC4 take 8B/block, BC7/BC5/BC3 16B/block.
    CHECK(BlockCompressedSize(rhi::TextureFormat::BC1RGBAUnorm, 256, 256) == 4096u * 8u);
    CHECK(BlockCompressedSize(rhi::TextureFormat::BC4RUnorm, 256, 256) == 4096u * 8u);
    CHECK(BlockCompressedSize(rhi::TextureFormat::BC7RGBAUnorm, 256, 256) == 4096u * 16u);
    CHECK(BlockCompressedSize(rhi::TextureFormat::BC5RGUnorm, 256, 256) == 4096u * 16u);
    // NPOT / sub-block rounds up: 65x65 is 17x17 blocks.
    CHECK(BlockCompressedSize(rhi::TextureFormat::BC7RGBAUnorm, 65, 65) == 17u * 17u * 16u);
}

TEST_CASE("EncodeBlockCompressed - BC1 round-trip PSNR + exact size")
{
    const u32 w = 128, h = 128;
    const Array<u8> img = MakeImage(w, h, /*alpha*/ false);
    const Array<byte> enc = EncodeBlockCompressed(img.Data(), w, h, rhi::TextureFormat::BC1RGBAUnorm, 200);

    REQUIRE(enc.Size() == BlockCompressedSize(rhi::TextureFormat::BC1RGBAUnorm, w, h));
    const Array<u8> dec = DecodeBc(enc, w, h, rhi::TextureFormat::BC1RGBAUnorm);
    const double psnr = Psnr(img.Data(), dec.Data(), static_cast<usize>(w) * h, 3);
    // BC1 on a smooth ramp reconstructs well above this floor; a broken encoder scores far lower.
    CHECK(psnr > 30.0);
}

TEST_CASE("EncodeBlockCompressed - BC7 round-trip PSNR (RGBA) beats BC1")
{
    const u32 w = 128, h = 128;
    const Array<u8> img = MakeImage(w, h, /*alpha*/ true);

    const Array<byte> bc7 = EncodeBlockCompressed(img.Data(), w, h, rhi::TextureFormat::BC7RGBAUnorm, 255);
    REQUIRE(bc7.Size() == BlockCompressedSize(rhi::TextureFormat::BC7RGBAUnorm, w, h));
    const Array<u8> dec7 = DecodeBc(bc7, w, h, rhi::TextureFormat::BC7RGBAUnorm);
    const double psnr7 = Psnr(img.Data(), dec7.Data(), static_cast<usize>(w) * h, 4);
    // BC7 is near-lossless on a smooth gradient; high floor guards the full 4-channel path.
    CHECK(psnr7 > 40.0);
}

TEST_CASE("ResolveCompressedFormat - ASTC (mobile) profile")
{
    const TargetProfile astc = MobileProfile();
    const auto uncompressed = rhi::TextureFormat::RGBA8Unorm;

    // One 4x4 format covers every LDR usage; sRGB only applies to color.
    CHECK(ResolveCompressedFormat(TextureUsage::Color, true, true, false, CompressionChoice::Default, 256, 256, astc, uncompressed) == rhi::TextureFormat::ASTC4x4UnormSrgb);
    CHECK(ResolveCompressedFormat(TextureUsage::Color, false, false, false, CompressionChoice::Default, 256, 256, astc, uncompressed) == rhi::TextureFormat::ASTC4x4Unorm);
    CHECK(ResolveCompressedFormat(TextureUsage::Normal, false, false, false, CompressionChoice::Default, 256, 256, astc, uncompressed) == rhi::TextureFormat::ASTC4x4Unorm);
    CHECK(ResolveCompressedFormat(TextureUsage::Mask, true, false, false, CompressionChoice::Default, 256, 256, astc, uncompressed) == rhi::TextureFormat::ASTC4x4Unorm); // linear map ignores sRGB

    // Same escape hatches as BC: None, small, HDR.
    CHECK(ResolveCompressedFormat(TextureUsage::Color, true, false, false, CompressionChoice::None, 256, 256, astc, uncompressed) == uncompressed);
    CHECK(ResolveCompressedFormat(TextureUsage::Color, true, false, false, CompressionChoice::Default, 64, 64, astc, uncompressed) == uncompressed);
    CHECK(ResolveCompressedFormat(TextureUsage::HDR, false, false, false, CompressionChoice::Default, 256, 256, astc, uncompressed) == uncompressed);

    // A profile that supports both families prefers BC (desktop-first).
    const TargetProfile both{true, true, false};
    CHECK(ResolveCompressedFormat(TextureUsage::Color, false, false, false, CompressionChoice::Default, 256, 256, both, uncompressed) == rhi::TextureFormat::BC1RGBAUnorm);
}

TEST_CASE("EncodeBlockCompressed - ASTC 4x4 round-trip PSNR + exact size")
{
    const u32 w = 128, h = 128;
    const Array<u8> img = MakeImage(w, h, /*alpha*/ true);

    const Array<byte> enc =
        EncodeBlockCompressed(img.Data(), w, h, rhi::TextureFormat::ASTC4x4Unorm, 200);
    REQUIRE(enc.Size() == BlockCompressedSize(rhi::TextureFormat::ASTC4x4Unorm, w, h));
    // ASTC 4x4 is 16 bytes per 4x4 block (8 bpp), the same footprint as BC7.
    CHECK(enc.Size() == static_cast<usize>((w / 4) * (h / 4)) * 16u);

    const Array<u8> dec = DecodeAstc(enc, w, h, /*srgb*/ false);
    REQUIRE(dec.Size() == static_cast<usize>(w) * h * 4);
    const double psnr = Psnr(img.Data(), dec.Data(), static_cast<usize>(w) * h, 4);
    // ASTC 4x4 is near-lossless on a smooth gradient; a broken encode scores far lower.
    CHECK(psnr > 40.0);
}

TEST_CASE("EncodeBlockCompressed - rejects non-BC formats + null input")
{
    const u32 w = 8, h = 8;
    const Array<u8> img = MakeImage(w, h, false);
    CHECK(EncodeBlockCompressed(img.Data(), w, h, rhi::TextureFormat::RGBA8Unorm, 128).Size() == 0);
    CHECK(EncodeBlockCompressed(nullptr, w, h, rhi::TextureFormat::BC1RGBAUnorm, 128).Size() == 0);
}

namespace
{
    // Decode BC6H (unsigned) blocks back to tightly-packed RGB floats through the vendored
    // reference decoder - independent bit-packing code, so a packing mistake shows here.
    Array<f32> DecodeBc6h(const Array<byte>& blocks, u32 w, u32 h)
    {
        Array<f32> out;
        out.Resize(static_cast<usize>(w) * h * 3, 0.0f);
        const u32 bx = (w + 3) / 4;
        const u32 by = (h + 3) / 4;
        usize off = 0;
        for (u32 y = 0; y < by; ++y)
        {
            for (u32 x = 0; x < bx; ++x)
            {
                f32 tile[16 * 3];
                bcdec_bc6h_float(blocks.Data() + off, tile, 4 * 3, 0);
                off += 16;
                for (u32 ty = 0; ty < 4; ++ty)
                {
                    for (u32 tx = 0; tx < 4; ++tx)
                    {
                        const u32 px = x * 4 + tx, py = y * 4 + ty;
                        if (px >= w || py >= h) continue;
                        const f32* s = tile + (ty * 4 + tx) * 3;
                        f32* d = out.Data() + (static_cast<usize>(py) * w + px) * 3;
                        d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
                    }
                }
            }
        }
        return out;
    }

    f32 MaxRelativeError(const Array<f32>& rgba, const Array<f32>& rgb, u32 w, u32 h)
    {
        f32 worst = 0.0f;
        for (usize i = 0; i < static_cast<usize>(w) * h; ++i)
        {
            for (u32 c = 0; c < 3; ++c)
            {
                const f32 a = rgba[i * 4 + c], b = rgb[i * 3 + c];
                const f32 denom = a > 1.0e-3f ? a : 1.0e-3f;
                const f32 rel = (a > b ? a - b : b - a) / denom;
                worst = rel > worst ? rel : worst;
            }
        }
        return worst;
    }
}

TEST_CASE("EncodeBlockCompressedHdr - a flat HDR block round-trips within half precision")
{
    // Sky-like: one radiance value across the block, well above 1.0. The straddle path lets the
    // 4-bit weights recover what 10-bit endpoints drop, so this lands near half precision.
    const u32 w = 4, h = 4;
    Array<f32> src;
    src.Resize(w * h * 4, 0.0f);
    for (usize i = 0; i < w * h; ++i)
    {
        src[i * 4 + 0] = 0.37f;
        src[i * 4 + 1] = 2.5f;
        src[i * 4 + 2] = 11.0f;
        src[i * 4 + 3] = 1.0f;
    }
    const Array<byte> enc = EncodeBlockCompressedHdr(src.Data(), w, h, 255);
    REQUIRE(enc.Size() == 16u);
    CHECK(BlockCompressedSize(rhi::TextureFormat::BC6HRGBUfloat, w, h) == 16u);
    const Array<f32> dec = DecodeBc6h(enc, w, h);
    const f32 rel = MaxRelativeError(src, dec, w, h);
    MESSAGE("flat block max relative error: ", rel);
    CHECK(rel < 0.002f);
}

TEST_CASE("EncodeBlockCompressedHdr - a gradient reconstructs closely and edge blocks clamp")
{
    // 13x9: not a multiple of 4, so the right/bottom blocks replicate edge texels. A smooth
    // radiance ramp across a wide dynamic range, per channel offset so the segment is not
    // axis-aligned.
    const u32 w = 13, h = 9;
    Array<f32> src;
    src.Resize(static_cast<usize>(w) * h * 4, 0.0f);
    for (u32 y = 0; y < h; ++y)
    {
        for (u32 x = 0; x < w; ++x)
        {
            const f32 t = static_cast<f32>(x + y) / static_cast<f32>(w + h - 2);
            f32* p = src.Data() + (static_cast<usize>(y) * w + x) * 4;
            // A realistic sky gradient: no more than ~20% change across any one 4x4 block.
            p[0] = 0.8f + 1.2f * t;
            p[1] = 1.5f + 1.0f * t * t;
            p[2] = 4.0f - 1.5f * t;
            p[3] = 1.0f;
        }
    }
    const Array<byte> enc = EncodeBlockCompressedHdr(src.Data(), w, h, 255);
    REQUIRE(enc.Size() == BlockCompressedSize(rhi::TextureFormat::BC6HRGBUfloat, w, h));
    CHECK(enc.Size() == 4u * 3u * 16u);
    const Array<f32> dec = DecodeBc6h(enc, w, h);
    const f32 rel = MaxRelativeError(src, dec, w, h);
    MESSAGE("gradient max relative error: ", rel);
    CHECK(rel < 0.03f); // single-region mode across a 4x4 ramp

    // More effort never makes it worse.
    const Array<byte> fast = EncodeBlockCompressedHdr(src.Data(), w, h, 0);
    const f32 relFast = MaxRelativeError(src, DecodeBc6h(fast, w, h), w, h);
    CHECK(rel <= relFast + 1.0e-6f);
}

TEST_CASE("EncodeBlockCompressedHdr - unsigned clamps negatives, NaN and beyond-half values")
{
    const u32 w = 4, h = 4;
    Array<f32> src;
    src.Resize(w * h * 4, 1.0f);
    src[0] = -5.0f;                 // negative clamps to 0
    src[4] = std::nanf("");        // NaN becomes 0 (guarded: never poisons the block); a constant
                                   // 0.0f / 0.0f is a hard error under MSVC (C2124)
    src[8] = 1.0e9f;                // beyond half clamps to the largest finite half
    const Array<byte> enc = EncodeBlockCompressedHdr(src.Data(), w, h, 128);
    const Array<f32> dec = DecodeBc6h(enc, w, h);
    CHECK(dec[0] >= 0.0f);
    CHECK(dec[3] >= 0.0f);
    CHECK(dec[3] == dec[3]); // not NaN
    CHECK(dec[6] > 60000.0f);
    CHECK(dec[6] <= 65504.0f);
    // Empty/null input is an empty result, not a crash.
    CHECK(EncodeBlockCompressedHdr(nullptr, 4, 4, 128).Size() == 0u);
    CHECK(EncodeBlockCompressedHdr(src.Data(), 0, 4, 128).Size() == 0u);
}

TEST_CASE("ResolveCompressedFormat - the HDR row is BC6H on BC targets, uncompressed elsewhere")
{
    using F = rhi::TextureFormat;
    CHECK(ResolveCompressedFormat(TextureUsage::HDR, false, false, false, CompressionChoice::Default,
                                  512, 256, DesktopProfile(), F::RGBA32Float) == F::BC6HRGBUfloat);
    // Mobile (ASTC) has no BC6H, and there is no ASTC-HDR path, so HDR stays uncompressed there.
    CHECK(ResolveCompressedFormat(TextureUsage::HDR, false, false, false, CompressionChoice::Default,
                                  512, 256, MobileProfile(), F::RGBA32Float) == F::RGBA32Float);
    // The authored escape and the small-texture escape still apply.
    CHECK(ResolveCompressedFormat(TextureUsage::HDR, false, false, false, CompressionChoice::None,
                                  512, 256, DesktopProfile(), F::RGBA32Float) == F::RGBA32Float);
    CHECK(ResolveCompressedFormat(TextureUsage::HDR, false, false, false, CompressionChoice::Default,
                                  32, 32, DesktopProfile(), F::RGBA32Float) == F::RGBA32Float);
}


TEST_CASE("EncodeBlockCompressed - the job-system fan-out is byte-identical to the inline encode")
{
    // The cook fans the block rows out over its job system (a 4k texture's BC7 encode is slow on one
    // core). Every block is encoded on its own, so the bytes must not depend on how
    // the rows were split across workers, for every BC format.
    const u32 w = 96, h = 64; // 24 x 16 blocks: more rows than workers, uneven chunks
    const Array<u8> img = MakeImage(w, h, /*alpha*/ true);
    JobSystem jobs(DefaultAllocator(), 3);
    for (const rhi::TextureFormat format :
         {rhi::TextureFormat::BC1RGBAUnorm, rhi::TextureFormat::BC3RGBAUnorm,
          rhi::TextureFormat::BC4RUnorm, rhi::TextureFormat::BC5RGUnorm,
          rhi::TextureFormat::BC7RGBAUnorm})
    {
        const Array<byte> inline_ = EncodeBlockCompressed(img.Data(), w, h, format, 128, nullptr);
        const Array<byte> fanned = EncodeBlockCompressed(img.Data(), w, h, format, 128, &jobs);
        REQUIRE(inline_.Size() == BlockCompressedSize(format, w, h));
        REQUIRE(fanned.Size() == inline_.Size());
        CHECK(MemCompare(fanned.Data(), inline_.Data(), inline_.Size()) == 0);
    }
}
