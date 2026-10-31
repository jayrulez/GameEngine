// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Tests for foundation.texture: descriptor factories, format conversion, mip sizes.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.rhi;
import foundation.image;
import foundation.texture;
using namespace foundation::core;
using namespace foundation::texture;
namespace rhi = foundation::rhi;
namespace image = foundation::image;

TEST_CASE("textures.data: Create2D / mips / cube / array")
{
    u8 pixels[16] = {};
    TextureData t = TextureData::Create2D(pixels, 16, 2, 2, rhi::TextureFormat::RGBA8Unorm);
    CHECK(t.width == 2u);
    CHECK(t.height == 2u);
    CHECK(t.depthOrArrayLayers == 1u);
    CHECK(t.mipLevels == 1u);
    CHECK(t.dimension == rhi::TextureDimension::Texture2D);
    CHECK(t.format == rhi::TextureFormat::RGBA8Unorm);

    TextureData mips =
        TextureData::Create2DWithMips(pixels, 16, 4, 4, 3, rhi::TextureFormat::RGBA8Unorm);
    CHECK(mips.mipLevels == 3u);

    TextureData cube = TextureData::CreateCube(pixels, 16, 8, rhi::TextureFormat::RGBA8Unorm);
    CHECK(cube.depthOrArrayLayers == 6u);
    CHECK(cube.width == 8u);
    CHECK(cube.height == 8u);

    TextureData arr =
        TextureData::Create2DArray(pixels, 16, 4, 4, 5, rhi::TextureFormat::RGBA8Unorm);
    CHECK(arr.depthOrArrayLayers == 5u);
}

TEST_CASE("textures.data: bytes per pixel")
{
    CHECK(TextureData::GetBytesPerPixel(rhi::TextureFormat::R8Unorm) == 1u);
    CHECK(TextureData::GetBytesPerPixel(rhi::TextureFormat::RG8Unorm) == 2u);
    CHECK(TextureData::GetBytesPerPixel(rhi::TextureFormat::RGBA8Unorm) == 4u);
    CHECK(TextureData::GetBytesPerPixel(rhi::TextureFormat::RGBA8UnormSrgb) == 4u);
    CHECK(TextureData::GetBytesPerPixel(rhi::TextureFormat::RGBA16Float) == 8u);
    CHECK(TextureData::GetBytesPerPixel(rhi::TextureFormat::RGBA32Float) == 16u);
    CHECK(TextureData::GetBytesPerPixel(rhi::TextureFormat::Depth16Unorm) == 2u);
    // A blanket default of 4 bytes would be wrong for these two.
    CHECK(TextureData::GetBytesPerPixel(rhi::TextureFormat::RGBA16Unorm) == 8u);
    CHECK(TextureData::GetBytesPerPixel(rhi::TextureFormat::Stencil8) == 1u);
    // Block-compressed formats have no per-texel size; mips are sized by block.
    CHECK(TextureData::GetBytesPerPixel(rhi::TextureFormat::BC7RGBAUnorm) == 0u);
}

TEST_CASE("textures.data: mip size halves")
{
    u8 pixels[1] = {};
    TextureData t = TextureData::Create2D(pixels, 0, 8, 8, rhi::TextureFormat::RGBA8Unorm);
    CHECK(t.CalculateMipSize(0) == static_cast<u64>(8 * 8 * 4)); // 256
    CHECK(t.CalculateMipSize(1) == static_cast<u64>(4 * 4 * 4)); // 64
    CHECK(t.CalculateMipSize(3) == static_cast<u64>(1 * 1 * 4)); // clamps to 1x1
}

TEST_CASE("textures.format: PixelFormat -> TextureFormat with color space")
{
    using image::ImageColorSpace;
    using image::PixelFormat;
    // sRGB color imagery selects the sRGB GPU format.
    CHECK(TextureFormatUtils::Convert(PixelFormat::RGBA8, ImageColorSpace::Srgb) ==
          rhi::TextureFormat::RGBA8UnormSrgb);
    CHECK(TextureFormatUtils::Convert(PixelFormat::RGB8, ImageColorSpace::Srgb) ==
          rhi::TextureFormat::RGBA8UnormSrgb);
    CHECK(TextureFormatUtils::Convert(PixelFormat::BGRA8, ImageColorSpace::Srgb) ==
          rhi::TextureFormat::BGRA8UnormSrgb);
    // Linear data textures stay unorm.
    CHECK(TextureFormatUtils::Convert(PixelFormat::RGBA8, ImageColorSpace::Linear) ==
          rhi::TextureFormat::RGBA8Unorm);
    CHECK(TextureFormatUtils::Convert(PixelFormat::R8, ImageColorSpace::Linear) ==
          rhi::TextureFormat::R8Unorm);
    // 3-channel maps to RGBA; floats pass through (no sRGB variant).
    CHECK(TextureFormatUtils::Convert(PixelFormat::RGB8, ImageColorSpace::Linear) ==
          rhi::TextureFormat::RGBA8Unorm);
    CHECK(TextureFormatUtils::Convert(PixelFormat::RGB16F, ImageColorSpace::Srgb) ==
          rhi::TextureFormat::RGBA16Float);
    CHECK(TextureFormatUtils::Convert(PixelFormat::R32F, ImageColorSpace::Linear) ==
          rhi::TextureFormat::R32Float);
    // R16 (heightmap loads) has no unorm GPU format on our backends: it maps to the
    // R16Uint integer container the terrain height upload uses, never to RGBA8 (wrong
    // channel count and depth).
    CHECK(TextureFormatUtils::Convert(PixelFormat::R16, ImageColorSpace::Linear) ==
          rhi::TextureFormat::R16Uint);
    CHECK(TextureFormatUtils::Convert(PixelFormat::R16, ImageColorSpace::Srgb) ==
          rhi::TextureFormat::R16Uint); // no sRGB variant either
}

TEST_CASE("textures.data: FromImage")
{
    image::Image image(2, 2, image::PixelFormat::RGBA8);
    TextureData t = TextureData::FromImage(image, image::ImageColorSpace::Srgb);
    CHECK(t.width == 2u);
    CHECK(t.height == 2u);
    CHECK(t.format == rhi::TextureFormat::RGBA8UnormSrgb);
    CHECK(t.pixels == image.PixelData().Data());
    CHECK(t.size == static_cast<u64>(image.PixelData().Size()));
}
