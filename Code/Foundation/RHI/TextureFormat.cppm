// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Texture format enum and query helpers.

export module foundation.rhi:texture_format;

import foundation.core;

using namespace foundation::core;

export namespace foundation::rhi
{

    /// Pixel formats for textures and render targets. Naming follows
    /// WebGPU and Vulkan conventions: component layout, then bit depth, then type.
    enum class TextureFormat : u32
    {
        Undefined = 0,

        // 8-bit per channel.
        R8Unorm,
        R8Snorm,
        R8Uint,
        R8Sint,

        // 16-bit per channel.
        R16Uint,
        R16Sint,
        R16Float,
        RG8Unorm,
        RG8Snorm,
        RG8Uint,
        RG8Sint,

        // 32-bit per channel.
        R32Uint,
        R32Sint,
        R32Float,
        RG16Uint,
        RG16Sint,
        RG16Float,
        RGBA8Unorm,
        RGBA8UnormSrgb,
        RGBA8Snorm,
        RGBA8Uint,
        RGBA8Sint,
        BGRA8Unorm,
        BGRA8UnormSrgb,
        RGB10A2Unorm,
        RGB10A2Uint,
        RG11B10Float,
        RGB9E5Float,

        // 64-bit per channel.
        RG32Uint,
        RG32Sint,
        RG32Float,
        RGBA16Uint,
        RGBA16Sint,
        RGBA16Float,
        RGBA16Unorm,
        RGBA16Snorm,

        // 128-bit per channel.
        RGBA32Uint,
        RGBA32Sint,
        RGBA32Float,

        // Depth / stencil.
        Depth16Unorm,
        Depth24Plus,
        Depth24PlusStencil8,
        Depth32Float,
        Depth32FloatStencil8,
        Stencil8,

        // BC compressed.
        BC1RGBAUnorm,
        BC1RGBAUnormSrgb,
        BC2RGBAUnorm,
        BC2RGBAUnormSrgb,
        BC3RGBAUnorm,
        BC3RGBAUnormSrgb,
        BC4RUnorm,
        BC4RSnorm,
        BC5RGUnorm,
        BC5RGSnorm,
        BC6HRGBUfloat,
        BC6HRGBFloat,
        BC7RGBAUnorm,
        BC7RGBAUnormSrgb,

        // ASTC compressed.
        ASTC4x4Unorm,
        ASTC4x4UnormSrgb,
        ASTC5x5Unorm,
        ASTC5x5UnormSrgb,
        ASTC6x6Unorm,
        ASTC6x6UnormSrgb,
        ASTC8x8Unorm,
        ASTC8x8UnormSrgb,
    };

    /// True for Depth16Unorm, Depth24Plus, Depth32Float and their stencil variants.
    [[nodiscard]] constexpr bool IsDepthFormat(TextureFormat f)
    {
        return f >= TextureFormat::Depth16Unorm && f <= TextureFormat::Depth32FloatStencil8;
    }

    /// True for any depth or stencil format (includes Stencil8).
    [[nodiscard]] constexpr bool IsDepthStencil(TextureFormat f)
    {
        return f >= TextureFormat::Depth16Unorm && f <= TextureFormat::Stencil8;
    }

    /// True if the format has a depth component.
    [[nodiscard]] constexpr bool HasDepth(TextureFormat f)
    {
        switch (f)
        {
        case TextureFormat::Depth16Unorm:
        case TextureFormat::Depth24Plus:
        case TextureFormat::Depth24PlusStencil8:
        case TextureFormat::Depth32Float:
        case TextureFormat::Depth32FloatStencil8:
            return true;
        default:
            return false;
        }
    }

    /// True if the format has a stencil component.
    [[nodiscard]] constexpr bool HasStencil(TextureFormat f)
    {
        switch (f)
        {
        case TextureFormat::Depth24PlusStencil8:
        case TextureFormat::Depth32FloatStencil8:
        case TextureFormat::Stencil8:
            return true;
        default:
            return false;
        }
    }

    /// True for BC or ASTC compressed formats.
    [[nodiscard]] constexpr bool IsCompressed(TextureFormat f)
    {
        return f >= TextureFormat::BC1RGBAUnorm && f <= TextureFormat::ASTC8x8UnormSrgb;
    }

    /// True for floating-point color formats: they store linear values as written, and whatever
    /// shows them encodes for display.
    [[nodiscard]] constexpr bool IsFloat(TextureFormat f)
    {
        switch (f)
        {
        case TextureFormat::R16Float:
        case TextureFormat::R32Float:
        case TextureFormat::RG16Float:
        case TextureFormat::RG11B10Float:
        case TextureFormat::RG32Float:
        case TextureFormat::RGBA16Float:
        case TextureFormat::RGBA32Float:
            return true;
        default:
            return false;
        }
    }

    /// True for sRGB variants.
    [[nodiscard]] constexpr bool IsSrgb(TextureFormat f)
    {
        switch (f)
        {
        case TextureFormat::RGBA8UnormSrgb:
        case TextureFormat::BGRA8UnormSrgb:
        case TextureFormat::BC1RGBAUnormSrgb:
        case TextureFormat::BC2RGBAUnormSrgb:
        case TextureFormat::BC3RGBAUnormSrgb:
        case TextureFormat::BC7RGBAUnormSrgb:
        case TextureFormat::ASTC4x4UnormSrgb:
        case TextureFormat::ASTC5x5UnormSrgb:
        case TextureFormat::ASTC6x6UnormSrgb:
        case TextureFormat::ASTC8x8UnormSrgb:
            return true;
        default:
            return false;
        }
    }

    /// The format's sRGB or UNORM twin (RGBA8Unorm <-> RGBA8UnormSrgb, the BGRA8 and block-
    /// compressed pairs likewise): the same bytes read with or without the sRGB transfer.
    /// Undefined for a format that has none.
    [[nodiscard]] constexpr TextureFormat SrgbTwin(TextureFormat f)
    {
        switch (f)
        {
        case TextureFormat::RGBA8Unorm: return TextureFormat::RGBA8UnormSrgb;
        case TextureFormat::RGBA8UnormSrgb: return TextureFormat::RGBA8Unorm;
        case TextureFormat::BGRA8Unorm: return TextureFormat::BGRA8UnormSrgb;
        case TextureFormat::BGRA8UnormSrgb: return TextureFormat::BGRA8Unorm;
        case TextureFormat::BC1RGBAUnorm: return TextureFormat::BC1RGBAUnormSrgb;
        case TextureFormat::BC1RGBAUnormSrgb: return TextureFormat::BC1RGBAUnorm;
        case TextureFormat::BC2RGBAUnorm: return TextureFormat::BC2RGBAUnormSrgb;
        case TextureFormat::BC2RGBAUnormSrgb: return TextureFormat::BC2RGBAUnorm;
        case TextureFormat::BC3RGBAUnorm: return TextureFormat::BC3RGBAUnormSrgb;
        case TextureFormat::BC3RGBAUnormSrgb: return TextureFormat::BC3RGBAUnorm;
        case TextureFormat::BC7RGBAUnorm: return TextureFormat::BC7RGBAUnormSrgb;
        case TextureFormat::BC7RGBAUnormSrgb: return TextureFormat::BC7RGBAUnorm;
        default: return TextureFormat::Undefined;
        }
    }

    /// Bytes per texel of an uncompressed format; 0 for block-compressed or unknown formats (size
    /// those with BlockBytes and CompressedLevelBytes). Every uncompressed enumerator is listed,
    /// since a missing one sizes its uploads at zero bytes. Depth24Plus is opaque; 4 is the
    /// footprint every backend allocates for it.
    [[nodiscard]] constexpr u32 BytesPerPixel(TextureFormat f)
    {
        switch (f)
        {
        case TextureFormat::R8Unorm:
        case TextureFormat::R8Snorm:
        case TextureFormat::R8Uint:
        case TextureFormat::R8Sint:
        case TextureFormat::Stencil8:
            return 1;
        case TextureFormat::R16Uint:
        case TextureFormat::R16Sint:
        case TextureFormat::R16Float:
        case TextureFormat::RG8Unorm:
        case TextureFormat::RG8Snorm:
        case TextureFormat::RG8Uint:
        case TextureFormat::RG8Sint:
        case TextureFormat::Depth16Unorm:
            return 2;
        case TextureFormat::R32Uint:
        case TextureFormat::R32Sint:
        case TextureFormat::R32Float:
        case TextureFormat::RG16Uint:
        case TextureFormat::RG16Sint:
        case TextureFormat::RG16Float:
        case TextureFormat::RGBA8Unorm:
        case TextureFormat::RGBA8UnormSrgb:
        case TextureFormat::RGBA8Snorm:
        case TextureFormat::RGBA8Uint:
        case TextureFormat::RGBA8Sint:
        case TextureFormat::BGRA8Unorm:
        case TextureFormat::BGRA8UnormSrgb:
        case TextureFormat::RGB10A2Unorm:
        case TextureFormat::RGB10A2Uint:
        case TextureFormat::RG11B10Float:
        case TextureFormat::RGB9E5Float:
        case TextureFormat::Depth24Plus:
        case TextureFormat::Depth24PlusStencil8:
        case TextureFormat::Depth32Float:
            return 4;
        case TextureFormat::RG32Uint:
        case TextureFormat::RG32Sint:
        case TextureFormat::RG32Float:
        case TextureFormat::RGBA16Uint:
        case TextureFormat::RGBA16Sint:
        case TextureFormat::RGBA16Float:
        case TextureFormat::RGBA16Unorm:
        case TextureFormat::RGBA16Snorm:
        case TextureFormat::Depth32FloatStencil8:
            return 8;
        case TextureFormat::RGBA32Uint:
        case TextureFormat::RGBA32Sint:
        case TextureFormat::RGBA32Float:
            return 16;
        default:
            return 0; // block-compressed: BlockBytes and CompressedLevelBytes
        }
    }

    /// Block footprint (texels) of a compressed format; {1,1} for uncompressed. BC is always 4x4;
    /// ASTC block dimensions vary with the format (4x4 to 8x8).
    [[nodiscard]] constexpr u32 BlockWidth(TextureFormat f)
    {
        switch (f)
        {
        case TextureFormat::ASTC5x5Unorm:
        case TextureFormat::ASTC5x5UnormSrgb:
            return 5;
        case TextureFormat::ASTC6x6Unorm:
        case TextureFormat::ASTC6x6UnormSrgb:
            return 6;
        case TextureFormat::ASTC8x8Unorm:
        case TextureFormat::ASTC8x8UnormSrgb:
            return 8;
        default:
            return IsCompressed(f) ? 4u : 1u; // BC and ASTC4x4
        }
    }
    [[nodiscard]] constexpr u32 BlockHeight(TextureFormat f)
    {
        return BlockWidth(f); // all supported blocks are square
    }

    /// Bytes one compressed block occupies; 0 for uncompressed. BC1 and BC4 blocks take 8 bytes;
    /// every other BC block and every ASTC block takes 16.
    [[nodiscard]] constexpr u32 BlockBytes(TextureFormat f)
    {
        switch (f)
        {
        case TextureFormat::BC1RGBAUnorm:
        case TextureFormat::BC1RGBAUnormSrgb:
        case TextureFormat::BC4RUnorm:
        case TextureFormat::BC4RSnorm:
            return 8;
        default:
            return IsCompressed(f) ? 16u : 0u;
        }
    }

    /// Bytes between the start of consecutive block-rows (the upload `bytesPerRow`) for a compressed
    /// level of the given width; 0 for uncompressed.
    [[nodiscard]] constexpr u32 CompressedRowPitch(TextureFormat f, u32 width)
    {
        if (!IsCompressed(f))
        {
            return 0;
        }
        const u32 bw = BlockWidth(f);
        return ((width + bw - 1) / bw) * BlockBytes(f);
    }

    /// Total bytes a compressed 2D level of width x height occupies; 0 for uncompressed.
    [[nodiscard]] constexpr usize CompressedLevelBytes(TextureFormat f, u32 width, u32 height)
    {
        if (!IsCompressed(f))
        {
            return 0;
        }
        const u32 bh = BlockHeight(f);
        return static_cast<usize>(CompressedRowPitch(f, width)) * ((height + bh - 1) / bh);
    }

} // namespace foundation::rhi
