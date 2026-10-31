// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// DDS (DirectDraw Surface) container: the file format GPU-ready textures ship in. It holds
/// block-compressed (BC1-BC7, BC6H) or uncompressed levels, a full mip chain, 2D, cubemap or
/// array, with either the legacy FourCC header or the DX10 extension header that names the DXGI
/// format (and so the colour space). This module reads the container as-is (a `DdsImage`
/// keeps the payload bytes), decodes any level to an `Image` (bcdec), and writes the DX10 form.
///
/// Policy lives elsewhere: the texture pipeline decides whether a DDS payload passes through
/// to the cooked texture untouched or is decoded and re-encoded; Image.IO's loaders sniff the
/// magic and hand back level 0 so every image consumer (thumbnails, model loaders) reads DDS.

module;
#include "Core/Prelude.h"

export module foundation.image.dds;

import foundation.core;
import foundation.image;

using namespace foundation::core;

export namespace foundation::image::dds
{
    /// The subset of DXGI formats a DDS may carry that the engine reads. Legacy FourCC headers
    /// map onto the same values (DXT1 -> BC1, DXT5 -> BC3, ATI2 -> BC5, ...).
    enum class DdsFormat : u8
    {
        Unknown,
        R8,
        RG8,
        RGBA8,
        RGBA8Srgb,
        BGRA8,
        BGRA8Srgb,
        RGBA16F,
        RGBA32F,
        BC1,
        BC1Srgb,
        BC2,
        BC2Srgb,
        BC3,
        BC3Srgb,
        BC4,
        BC4Snorm,
        BC5,
        BC5Snorm,
        BC6HUf, // unsigned half float radiance
        BC6HSf, // signed half float
        BC7,
        BC7Srgb,
    };

    [[nodiscard]] constexpr bool IsBlockCompressed(DdsFormat f) noexcept
    {
        return f >= DdsFormat::BC1 && f <= DdsFormat::BC7Srgb;
    }

    /// Bytes per 4x4 block (0 for uncompressed).
    [[nodiscard]] constexpr u32 BlockBytes(DdsFormat f) noexcept
    {
        switch (f)
        {
        case DdsFormat::BC1:
        case DdsFormat::BC1Srgb:
        case DdsFormat::BC4:
        case DdsFormat::BC4Snorm:
            return 8;
        default:
            return IsBlockCompressed(f) ? 16u : 0u;
        }
    }

    /// Bytes per texel (0 for block-compressed).
    [[nodiscard]] constexpr u32 BytesPerPixel(DdsFormat f) noexcept
    {
        switch (f)
        {
        case DdsFormat::R8:
            return 1;
        case DdsFormat::RG8:
            return 2;
        case DdsFormat::RGBA8:
        case DdsFormat::RGBA8Srgb:
        case DdsFormat::BGRA8:
        case DdsFormat::BGRA8Srgb:
            return 4;
        case DdsFormat::RGBA16F:
            return 8;
        case DdsFormat::RGBA32F:
            return 16;
        default:
            return 0;
        }
    }

    [[nodiscard]] constexpr bool IsSrgb(DdsFormat f) noexcept
    {
        switch (f)
        {
        case DdsFormat::RGBA8Srgb:
        case DdsFormat::BGRA8Srgb:
        case DdsFormat::BC1Srgb:
        case DdsFormat::BC2Srgb:
        case DdsFormat::BC3Srgb:
        case DdsFormat::BC7Srgb:
            return true;
        default:
            return false;
        }
    }

    /// True for the float formats (BC6H and the half / float texel formats): they decode to
    /// RGBA32F, everything else to RGBA8.
    [[nodiscard]] constexpr bool IsHdr(DdsFormat f) noexcept
    {
        return f == DdsFormat::RGBA16F || f == DdsFormat::RGBA32F || f == DdsFormat::BC6HUf ||
               f == DdsFormat::BC6HSf;
    }

    /// The sRGB / non-sRGB twin of a format, where one exists (same bytes, another GPU view);
    /// formats without a twin come back unchanged.
    [[nodiscard]] constexpr DdsFormat WithSrgb(DdsFormat f, bool srgb) noexcept
    {
        switch (f)
        {
        case DdsFormat::RGBA8:
        case DdsFormat::RGBA8Srgb:
            return srgb ? DdsFormat::RGBA8Srgb : DdsFormat::RGBA8;
        case DdsFormat::BGRA8:
        case DdsFormat::BGRA8Srgb:
            return srgb ? DdsFormat::BGRA8Srgb : DdsFormat::BGRA8;
        case DdsFormat::BC1:
        case DdsFormat::BC1Srgb:
            return srgb ? DdsFormat::BC1Srgb : DdsFormat::BC1;
        case DdsFormat::BC2:
        case DdsFormat::BC2Srgb:
            return srgb ? DdsFormat::BC2Srgb : DdsFormat::BC2;
        case DdsFormat::BC3:
        case DdsFormat::BC3Srgb:
            return srgb ? DdsFormat::BC3Srgb : DdsFormat::BC3;
        case DdsFormat::BC7:
        case DdsFormat::BC7Srgb:
            return srgb ? DdsFormat::BC7Srgb : DdsFormat::BC7;
        default:
            return f;
        }
    }

    /// Bytes one level of `width` x `height` occupies: block-ceil for compressed formats.
    [[nodiscard]] constexpr usize LevelBytes(DdsFormat f, u32 width, u32 height) noexcept
    {
        if (IsBlockCompressed(f))
        {
            const usize bw = (width + 3u) / 4u;
            const usize bh = (height + 3u) / 4u;
            return bw * bh * BlockBytes(f);
        }
        return static_cast<usize>(width) * height * BytesPerPixel(f);
    }

    /// A parsed DDS: the header facts plus the payload bytes exactly as the file holds them,
    /// layer-major (each layer's levels 0..mipLevels-1 concatenated, each level tight).
    struct DdsImage
    {
        u32 width = 0;
        u32 height = 0;
        u32 mipLevels = 1;
        u32 arrayLayers = 1; // 6 x arraySize for cubemaps
        bool cubemap = false;
        DdsFormat format = DdsFormat::Unknown;
        // A DX10 header names the DXGI format, so sRGB-or-not is a fact of the file; a legacy
        // FourCC header says nothing about colour space (readers fall back on usage).
        bool colorSpaceKnown = false;
        Array<u8> data;

        [[nodiscard]] u32 LevelWidth(u32 level) const noexcept
        {
            const u32 w = width >> level;
            return w > 0 ? w : 1u;
        }
        [[nodiscard]] u32 LevelHeight(u32 level) const noexcept
        {
            const u32 h = height >> level;
            return h > 0 ? h : 1u;
        }
        [[nodiscard]] usize LevelSize(u32 level) const noexcept
        {
            return LevelBytes(format, LevelWidth(level), LevelHeight(level));
        }
        [[nodiscard]] usize LayerSize() const noexcept
        {
            usize total = 0;
            for (u32 level = 0; level < mipLevels; ++level)
            {
                total += LevelSize(level);
            }
            return total;
        }
        [[nodiscard]] usize LevelOffset(u32 layer, u32 level) const noexcept
        {
            usize offset = LayerSize() * layer;
            for (u32 l = 0; l < level; ++l)
            {
                offset += LevelSize(l);
            }
            return offset;
        }
        [[nodiscard]] Span<const u8> Level(u32 layer, u32 level) const noexcept
        {
            const usize offset = LevelOffset(layer, level);
            const usize size = LevelSize(level);
            if (offset + size > data.Size())
            {
                return {};
            }
            return Span<const u8>{data.Data() + offset, size};
        }
    };

    /// True when the bytes start with the DDS magic.
    [[nodiscard]] bool IsDds(Span<const u8> bytes) noexcept;

    /// True when the file at `path` starts with the DDS magic (a four-byte probe, not a read of
    /// the file): how a loader tells a GPU-ready container from an image it should decode.
    [[nodiscard]] bool IsDdsFile(StringView path) noexcept;

    /// The facts a DDS header states, without the payload: what an importer needs to decide a
    /// texture's usage (BC5 = normal map, BC4 = mask, an HDR format = a skybox) and colour
    /// space. ReadDdsHeader reads only kDdsHeaderBytes, so classifying a large set of DDS files
    /// does not read gigabytes of payload.
    struct DdsHeader
    {
        u32 width = 0;
        u32 height = 0;
        u32 mipLevels = 1;
        u32 arrayLayers = 1; // 6 x arraySize for cubemaps
        bool cubemap = false;
        DdsFormat format = DdsFormat::Unknown;
        bool colorSpaceKnown = false;
    };

    /// Magic + DDS_HEADER + the DX10 extension: the most bytes any header can take.
    inline constexpr usize kDdsHeaderBytes = 4 + 124 + 20;

    /// Parse only the header (legacy or DX10) from the first bytes of a file: kDdsHeaderBytes is
    /// always enough, a legacy header needs 128. The same refusals as LoadDds (a volume, an
    /// unknown format, a partial cubemap: NotSupported; too short: InvalidArgument); nothing
    /// about the payload is checked.
    [[nodiscard]] Status ParseDdsHeader(Span<const u8> bytes, DdsHeader& out);

    /// ParseDdsHeader over the first kDdsHeaderBytes of the file at `path`: a header read, never
    /// a file read.
    [[nodiscard]] Status ReadDdsHeader(StringView path, DdsHeader& out);

    /// Parse a DDS file (legacy or DX10 header) into `out`, copying the payload. Volumes and
    /// formats outside DdsFormat are NotSupported; a truncated payload is InvalidArgument.
    [[nodiscard]] Status LoadDds(Span<const u8> bytes, DdsImage& out);

    /// Decode one level of one layer to an Image: RGBA8 for LDR formats (single-channel R
    /// replicated to RGB, BC5 / RG8 given a reconstructed Z so a two-channel normal map reads
    /// as a whole one), RGBA32F for the float formats. The colour space follows the format
    /// (sRGB formats -> Srgb; data formats -> Linear; a legacy colour format -> Srgb, the
    /// authoring norm).
    [[nodiscard]] Status DecodeLevel(const DdsImage& dds, u32 layer, u32 level, Image& out);

    /// Level 0 of layer 0 as an Image: what a generic image loader hands back for a DDS.
    [[nodiscard]] Status LoadDdsAsImage(Span<const u8> bytes, Image& out);

    /// Write `dds` with a DX10 header (always: the form that names the format).
    [[nodiscard]] Status WriteDds(const DdsImage& dds, Array<u8>& out);
}
