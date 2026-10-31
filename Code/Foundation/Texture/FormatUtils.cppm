// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Texture - :format_utils partition
//
// Maps an image PixelFormat to the RHI TextureFormat, honoring the source
// data's color space.

module;
#include "Core/Prelude.h"

export module foundation.texture:format_utils;

import foundation.core;
import foundation.rhi;
import foundation.image;

using namespace foundation::core;

export namespace foundation::texture
{
    namespace rhi = foundation::rhi;
    namespace image = foundation::image;

    class TextureFormatUtils
    {
    public:
        // Image PixelFormat to RHI TextureFormat. When the data is sRGB and the
        // format has an sRGB variant (8-bit RGB, RGBA, BGR, BGRA), the sRGB GPU
        // format is returned so the hardware decodes sRGB to linear on sample. Float
        // and one- and two-channel formats pass through. Three-channel maps to RGBA
        // (GPUs don't support three-channel). R16 (heightmap loads) has no unorm GPU
        // format on our backends (WebGPU core lacks it), so it maps to the R16Uint
        // integer container, the same one the terrain height upload uses; shaders read
        // it as uint, not through a float sampler. The switch is exhaustive: a new
        // PixelFormat is a compile error here, never a silent RGBA8 guess.
        [[nodiscard]] static rhi::TextureFormat Convert(image::PixelFormat format,
                                                        image::ImageColorSpace colorSpace)
        {
            if (colorSpace == image::ImageColorSpace::Srgb)
            {
                switch (format)
                {
                case image::PixelFormat::RGB8:
                case image::PixelFormat::RGBA8:
                    return rhi::TextureFormat::RGBA8UnormSrgb;
                case image::PixelFormat::BGR8:
                case image::PixelFormat::BGRA8:
                    return rhi::TextureFormat::BGRA8UnormSrgb;
                default:
                    break;
                }
            }

            switch (format)
            {
            case image::PixelFormat::R8:
                return rhi::TextureFormat::R8Unorm;
            case image::PixelFormat::RG8:
                return rhi::TextureFormat::RG8Unorm;
            case image::PixelFormat::RGB8:
                return rhi::TextureFormat::RGBA8Unorm;
            case image::PixelFormat::RGBA8:
                return rhi::TextureFormat::RGBA8Unorm;
            case image::PixelFormat::BGR8:
                return rhi::TextureFormat::BGRA8Unorm;
            case image::PixelFormat::BGRA8:
                return rhi::TextureFormat::BGRA8Unorm;
            case image::PixelFormat::R16F:
                return rhi::TextureFormat::R16Float;
            case image::PixelFormat::RG16F:
                return rhi::TextureFormat::RG16Float;
            case image::PixelFormat::RGB16F:
                return rhi::TextureFormat::RGBA16Float;
            case image::PixelFormat::RGBA16F:
                return rhi::TextureFormat::RGBA16Float;
            case image::PixelFormat::R32F:
                return rhi::TextureFormat::R32Float;
            case image::PixelFormat::RG32F:
                return rhi::TextureFormat::RG32Float;
            case image::PixelFormat::RGB32F:
                return rhi::TextureFormat::RGBA32Float;
            case image::PixelFormat::RGBA32F:
                return rhi::TextureFormat::RGBA32Float;
            case image::PixelFormat::R16:
                return rhi::TextureFormat::R16Uint; // integer container, see above
            }
            return rhi::TextureFormat::Undefined; // unreachable: the switch is exhaustive
        }
    };
}
