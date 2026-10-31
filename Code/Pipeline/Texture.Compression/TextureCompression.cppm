// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::Texture.Compression - `texture.compression`.
//
// The only code that includes the block-compression encoder headers (bc7enc/rgbcx). Two jobs:
//   - ResolveCompressedFormat: the format policy table. Given a source's authored semantics and the
//     export target's capabilities, pick the cooked rhi::TextureFormat (or an uncompressed fallback).
//   - EncodeBlockCompressed: encode one mip level of RGBA8 to a BC format's block bytes.
// Cook and import time only, with no UI; the heavy encoder headers stay in the impl unit, out of
// the module interface. BC (BC6H for HDR) and ASTC are implemented.

module;
#include "Core/Prelude.h"

export module texture.compression;

import foundation.core;
import foundation.rhi;

using namespace foundation::core;

export namespace texcomp
{
    namespace rhi = foundation::rhi;

    // What a source texture is for: the semantic hint the policy table keys on. Authored on the asset
    // (never the runtime struct); model import sets it from the material slot, standalone defaults Color.
    enum class TextureUsage : u8
    {
        Color,  // albedo / UI: sRGB (or linear) color
        Normal, // tangent-space normal map (linear RG)
        Mask,   // single-channel mask / height / roughness
        HDR,    // RGBE / half-float radiance (linear): BC6H unsigned on BC targets
    };

    // Authored compression choice on the asset.
    enum class CompressionChoice : u8
    {
        Default, // the policy table below
        None,    // force uncompressed (the raw path, the escape hatch)
        Quality, // force the high-quality format (BC7) and high encoder effort
    };

    // The compressed-texture families the export target's devices support: a capability, not a
    // platform: keyed on "what families does this target support", never "which platform".
    struct TargetProfile
    {
        bool bc = false;   // BC1-BC7 (desktop and desktop browsers)
        bool astc = false; // ASTC (mobile browsers)
        bool etc2 = false; // ETC2 (encoding not implemented; selecting it falls back to uncompressed)
    };

    // The always-warm host desktop profile (BC-capable).
    [[nodiscard]] inline TargetProfile DesktopProfile() noexcept
    {
        return TargetProfile{true, false, false};
    }

    // The mobile-web profile (ASTC-capable, no BC): mobile browsers expose texture-compression-astc,
    // not -bc. The web export cooks both this and the desktop BC variant.
    [[nodiscard]] inline TargetProfile MobileProfile() noexcept
    {
        return TargetProfile{false, true, false};
    }

    // The format policy table. Returns `uncompressed` when policy says do not compress (authored
    // None, small/UI textures <= 64px, HDR on a target without BC, or a target with no supported
    // family). `sRGB` selects the *Srgb color formats; `hasAlpha` splits color into BC1 (opaque)
    // and BC7 (alpha). `multiChannel` says the source carries distinct channel content (the cook's
    // cheap sniff): a multichannel Mask (packed ORM/ARM) cooks BC7-linear, since BC4 would keep R
    // only and silently drop roughness/metallic. Normal cooks BC7-linear, not BC5: the shaders
    // decode rgb * 2 - 1, and BC5's missing B samples as 0, giving tangent z = -1 and visibly
    // broken shading. BC5 needs the shaders to reconstruct z first.
    [[nodiscard]] rhi::TextureFormat ResolveCompressedFormat(TextureUsage usage, bool sRGB, bool hasAlpha,
                                                             bool multiChannel,
                                                             CompressionChoice choice, u32 width,
                                                             u32 height, const TargetProfile& profile,
                                                             rhi::TextureFormat uncompressed) noexcept;

    // Channel-content sniff for the Mask guard (and the editor's Cooks-to preview):
    // true when any texel's G or B deviates from R by more than `tolerance` (default 8
    // absorbs JPEG chroma noise on gray sources). A packed ORM/ARM reads true; a gray
    // rough/AO/height map reads false.
    [[nodiscard]] bool HasDistinctChannels(const u8* rgba, u32 width, u32 height,
                                           u8 tolerance = 8) noexcept;

    // Encode one mip level of tightly-packed RGBA8 pixels (`width*height*4` bytes) to `format`'s
    // block-compressed bytes. `format` must be a BC format this build supports (BC1/3/4/5/7) or
    // ASTC 4x4; returns
    // empty otherwise. Edge blocks on NPOT/small levels are clamp-padded. `quality` 0..255 maps to the
    // encoder effort (Default ~ mid, Quality ~ max).
    // `jobs`: the block rows fan out over it (ParallelFor; the caller participates), else the
    // encode runs inline. Byte-identical either way, since every block is encoded on its own.
    [[nodiscard]] Array<byte> EncodeBlockCompressed(const u8* rgba, u32 width, u32 height,
                                                    rhi::TextureFormat format, u8 quality,
                                                    JobSystem* jobs = nullptr);

    // Bytes one mip level of a BC `format` occupies (4x4 block-ceil), for the exact-size cook assertion.
    [[nodiscard]] usize BlockCompressedSize(rhi::TextureFormat format, u32 width, u32 height) noexcept;

    // Encode one level of tightly-packed RGBA32F pixels to BC6H (unsigned half float, the
    // BC6HRGBUfloat format): 16 bytes per 4x4 block, edge blocks clamp-replicated. Alpha is
    // dropped (BC6H is RGB); negative, NaN and out-of-half-range values clamp, since radiance maps
    // are non-negative by construction and the signed variant is not plumbed. `quality` 0..255
    // buys endpoint refinement passes. In-house encoder (Bc6hEncoderImpl.cpp): the single-region
    // mode the real-time encoders use, least-squares refined.
    [[nodiscard]] Array<byte> EncodeBlockCompressedHdr(const f32* rgba, u32 width, u32 height,
                                                       u8 quality);

    // Register TextureUsage and CompressionChoice enum reflection (by name), so the generic asset
    // page renders them as dropdowns. Idempotent; call from the asset's registration. The bodies
    // live in the reflection impl unit, as REFLECT_* bodies stay out of module interfaces.
    void RegisterCompressionReflection();
}
