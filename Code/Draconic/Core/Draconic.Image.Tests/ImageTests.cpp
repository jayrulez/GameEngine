#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.image;
import draconic.image.io;

using namespace draconic::foundation;
using namespace draconic::image;

TEST_CASE("image: procedural create + pixel access")
{
    Image img = Image::CreateSolidColor(4, 4, Color32{10, 20, 30, 255});
    CHECK(img.Width() == 4u);
    CHECK(img.Height() == 4u);
    CHECK(img.Format() == PixelFormat::RGBA8);
    const Color32 p = img.GetPixel(1, 1);
    CHECK(p.r == 10);
    CHECK(p.g == 20);
    CHECK(p.b == 30);
}

TEST_CASE("image: save PNG and reload via stb")
{
    Image img = Image::CreateCheckerboard(64);
    REQUIRE(img.Width() == 64u);

    const StringView path = u8"/tmp/draconic_image_roundtrip.png";
    REQUIRE(io::SaveImage(img, path, io::ImageFileFormat::PNG).IsOk());

    Image loaded;
    REQUIRE(io::LoadImage(path, loaded).IsOk());
    CHECK(loaded.Width() == 64u);
    CHECK(loaded.Height() == 64u);
    CHECK(loaded.Format() == PixelFormat::RGBA8); // stb loads as RGBA8
}
