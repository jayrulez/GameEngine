// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.image;
import foundation.image.io;
import foundation.image.dds;

using namespace foundation::core;
using namespace foundation::image;

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

    const StringView path = u8"scratch_image_roundtrip.png"; // relative to the test CWD (portable; /tmp is POSIX-only)
    REQUIRE(io::SaveImage(img, path, io::ImageFileFormat::PNG).IsOk());

    Image loaded;
    REQUIRE(io::LoadImage(path, loaded).IsOk());
    CHECK(loaded.Width() == 64u);
    CHECK(loaded.Height() == 64u);
    CHECK(loaded.Format() == PixelFormat::RGBA8); // stb loads as RGBA8
}

TEST_CASE("image.io: a file's header says its size, channels and depth without decoding it")
{
    const u8 grey[6] = {0, 40, 80, 120, 160, 200};
    const Image img(3, 2, PixelFormat::R8, Span<const u8>(grey, sizeof(grey)));
    const StringView path = u8"scratch_image_info.png";
    REQUIRE(io::SaveImage(img, path, io::ImageFileFormat::PNG).IsOk());

    io::ImageFileInfo info;
    REQUIRE(io::ReadImageFileInfo(path, info));
    CHECK(info.width == 3u);
    CHECK(info.height == 2u);
    CHECK(info.channels == 1u); // as stored, though the loader widens it to RGBA
    CHECK_FALSE(info.sixteenBit);
    CHECK_FALSE(info.hdr);
    FileDelete(path);

    CHECK_FALSE(io::ReadImageFileInfo(u8"scratch_image_info_missing.png", info));
}

TEST_CASE("image.io: a DDS buffer loads through the generic loader as its decoded level 0")
{
    // Every image consumer (thumbnails, model loaders) reads DDS without knowing it: the
    // loaders sniff the magic and hand back level 0.
    foundation::image::dds::DdsImage src;
    src.width = 1;
    src.height = 1;
    src.format = foundation::image::dds::DdsFormat::RGBA8Srgb;
    src.colorSpaceKnown = true;
    src.data.PushBack(9);
    src.data.PushBack(8);
    src.data.PushBack(7);
    src.data.PushBack(255);
    Array<u8> file;
    REQUIRE(foundation::image::dds::WriteDds(src, file).IsOk());
    Image img;
    REQUIRE(io::LoadImageFromMemory(Span<const u8>(file.Data(), file.Size()), img).IsOk());
    CHECK(img.Format() == PixelFormat::RGBA8);
    CHECK(img.ColorSpace() == ImageColorSpace::Srgb);
    const Color32 p = img.GetPixel(0, 0);
    CHECK(p.r == 9);
    CHECK(p.g == 8);
    CHECK(p.b == 7);
    // And from a path: the sniff reads the magic, then the file.
    REQUIRE(WriteFile(u8"scratch_image_io_probe.dds", Span<const byte>(reinterpret_cast<const byte*>(file.Data()), file.Size())).IsOk());
    Image fromPath;
    REQUIRE(io::LoadImage(u8"scratch_image_io_probe.dds", fromPath).IsOk());
    CHECK(fromPath.GetPixel(0, 0).r == 9);
    FileDelete(u8"scratch_image_io_probe.dds");
}

TEST_CASE("image: HalfToFloat decodes normals, subnormals, zeros, infinities and the extremes exactly")
{
    CHECK(HalfToFloat(0x3C00) == 1.0f);
    CHECK(HalfToFloat(0x3800) == 0.5f);
    CHECK(HalfToFloat(0xC000) == -2.0f);
    CHECK(HalfToFloat(0x3555) == doctest::Approx(0.333251953125f)); // the nearest half to 1/3
    CHECK(HalfToFloat(0x7BFF) == 65504.0f);                          // the largest finite half
    CHECK(HalfToFloat(0x0400) == doctest::Approx(6.103515625e-05f)); // the smallest normal
    CHECK(HalfToFloat(0x0001) == doctest::Approx(5.9604644775390625e-08f)); // the smallest subnormal
    CHECK(HalfToFloat(0x0000) == 0.0f);
    CHECK(__builtin_bit_cast(u32, HalfToFloat(0x8000)) == 0x80000000u); // negative zero keeps its sign
    CHECK(HalfToFloat(0x7C00) > 3.0e38f);                             // +inf
    CHECK(HalfToFloat(0xFC00) < -3.0e38f);                            // -inf
    CHECK(HalfToFloat(0x7E00) != HalfToFloat(0x7E00));                // NaN

    // To a byte: clamped, quantised, never re-encoded.
    CHECK(HalfToUnorm8(0x3C00) == 255);
    CHECK(HalfToUnorm8(0x3800) == 128); // 0.5 * 255 + 0.5 rounds up
    CHECK(HalfToUnorm8(0x3555) == 85);  // 1/3
    CHECK(HalfToUnorm8(0x4400) == 255); // 4.0 clamps high
    CHECK(HalfToUnorm8(0xBC00) == 0);   // -1.0 clamps low
    CHECK(HalfToUnorm8(0x0000) == 0);
}

TEST_CASE("image: an RGBA16F image reads its pixels quantised, so ConvertFormat(RGBA8) is the CPU readback")
{
    const u16 texels[2][4] = {{0x3C00, 0x3800, 0x0000, 0x3C00},  // 1.0, 0.5, 0.0, 1.0
                              {0x4400, 0xBC00, 0x3555, 0x3C00}}; // 4.0, -1.0, 1/3, 1.0
    const Image half(2, 1, PixelFormat::RGBA16F,
                     Span<const u8>{reinterpret_cast<const u8*>(texels), sizeof(texels)});
    CHECK(half.GetPixel(0, 0).r == 255);
    CHECK(half.GetPixel(0, 0).g == 128);
    CHECK(half.GetPixel(1, 0).r == 255);
    CHECK(half.GetPixel(1, 0).g == 0);
    CHECK(half.GetPixel(1, 0).b == 85);
    const Image bytes = half.ConvertFormat(PixelFormat::RGBA8);
    CHECK(bytes.Format() == PixelFormat::RGBA8);
    CHECK(bytes.PixelData().Size() == 8u);
    CHECK(bytes.GetPixel(1, 0).b == 85);
    CHECK(bytes.GetPixel(1, 0).a == 255);
}
