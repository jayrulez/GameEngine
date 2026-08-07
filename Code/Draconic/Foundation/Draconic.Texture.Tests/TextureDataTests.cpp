// Tests for draconic.texture: descriptor factories, format conversion, mip sizes.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.rhi;
import draconic.image;
import draconic.texture;
using namespace draconic::foundation;
using namespace draconic::texture;
namespace rhi = draconic::rhi;
namespace image = draconic::image;

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
