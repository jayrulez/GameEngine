// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Texture - :data partition
//
// TextureData: a CPU-side descriptor of pixel data staged for GPU upload. It
// owns no GPU handle; the consumer creates the rhi::Texture from it.

module;
#include "Core/Prelude.h"

export module foundation.texture:data;

import foundation.core;
import foundation.rhi;
import foundation.image;
import :format_utils;

using namespace foundation::core;

export namespace foundation::texture
{
    namespace rhi = foundation::rhi;
    namespace image = foundation::image;

    // Raw texture data for upload to the GPU. Fields are lowercase (descriptor
    // convention, matching RHI descs); the caller provides correctly-formatted
    // pixel data and owns its lifetime.
    struct TextureData
    {
        const u8* pixels = nullptr; // pixel data (not owned)
        u64 size = 0;               // total bytes
        u32 width = 0;
        u32 height = 0;
        u32 depthOrArrayLayers = 1; // depth (3D) or layer count
        u32 mipLevels = 1;          // data must contain all mips if > 1
        rhi::TextureFormat format = rhi::TextureFormat::RGBA8Unorm;
        rhi::TextureDimension dimension = rhi::TextureDimension::Texture2D;
        u32 bytesPerRow = 0;  // 0 = auto
        u32 rowsPerImage = 0; // 0 = auto

        [[nodiscard]] static TextureData Create2D(const u8* pixels, u64 size, u32 width, u32 height,
                                                  rhi::TextureFormat format)
        {
            return TextureData{
                pixels, size, width, height, 1, 1, format, rhi::TextureDimension::Texture2D, 0, 0};
        }

        [[nodiscard]] static TextureData Create2DWithMips(const u8* pixels, u64 size, u32 width,
                                                          u32 height, u32 mipLevels,
                                                          rhi::TextureFormat format)
        {
            return TextureData{pixels, size,      width,  height,
                               1,      mipLevels, format, rhi::TextureDimension::Texture2D,
                               0,      0};
        }

        // Cubemap: 6 square faces packed as a 2D array of 6 layers.
        [[nodiscard]] static TextureData CreateCube(const u8* pixels, u64 size, u32 faceSize,
                                                    rhi::TextureFormat format)
        {
            return TextureData{pixels, size, faceSize, faceSize,
                               6,      1,    format,   rhi::TextureDimension::Texture2D,
                               0,      0};
        }

        [[nodiscard]] static TextureData Create2DArray(const u8* pixels, u64 size, u32 width,
                                                       u32 height, u32 layers,
                                                       rhi::TextureFormat format)
        {
            return TextureData{pixels, size, width,  height,
                               layers, 1,    format, rhi::TextureDimension::Texture2D,
                               0,      0};
        }

        // Builds 2D texture data from an image. `colorSpace` selects the sRGB GPU
        // format (hardware sRGB->linear on sample) for color imagery, or a linear
        // format for data textures (normal maps, masks, HDR).
        [[nodiscard]] static TextureData FromImage(const image::Image& image,
                                                   image::ImageColorSpace colorSpace)
        {
            const Span<const u8> data = image.PixelData();
            return Create2D(data.Data(), static_cast<u64>(data.Size()), image.Width(),
                            image.Height(),
                            TextureFormatUtils::Convert(image.Format(), colorSpace));
        }

        // Bytes per pixel for an RHI format, read from rhi::BytesPerPixel so there is one
        // table. 0 for block-compressed formats; CalculateMipSize sizes those by block.
        [[nodiscard]] static u32 GetBytesPerPixel(rhi::TextureFormat format)
        {
            return rhi::BytesPerPixel(format);
        }

        // Expected byte size of a mip level.
        [[nodiscard]] u64 CalculateMipSize(u32 mipLevel) const
        {
            const u32 mipWidth = Max(1u, width >> mipLevel);
            const u32 mipHeight = Max(1u, height >> mipLevel);
            if (rhi::IsCompressed(format))
            {
                return static_cast<u64>(rhi::CompressedLevelBytes(format, mipWidth, mipHeight)) *
                       depthOrArrayLayers;
            }
            const u32 bpp = GetBytesPerPixel(format);
            return static_cast<u64>(mipWidth) * mipHeight * depthOrArrayLayers * bpp;
        }
    };
}
