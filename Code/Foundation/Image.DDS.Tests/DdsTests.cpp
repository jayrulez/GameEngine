// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// DDS container: both header forms parse, the payload layout (layers x levels) is addressed
// right, every block format decodes through bcdec to the expected texels, the DX10 writer
// round-trips, and the refusals (truncated, volume, unknown format) are the documented codes.
// Blocks are hand-built: a solid colour is one endpoint repeated with every index 0.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include <cstring>

import foundation.core;
import foundation.image;
import foundation.image.dds;

using namespace foundation::core;
using namespace foundation::image;
using namespace foundation::image::dds;

namespace
{
    void PutU32(Array<u8>& out, u32 v)
    {
        for (u32 i = 0; i < 4; ++i)
        {
            out.PushBack(static_cast<u8>((v >> (8 * i)) & 0xFFu));
        }
    }
    void PutU16(Array<u8>& out, u16 v)
    {
        out.PushBack(static_cast<u8>(v & 0xFFu));
        out.PushBack(static_cast<u8>(v >> 8));
    }
    constexpr u32 FourCC(const char (&s)[5])
    {
        return static_cast<u32>(static_cast<u8>(s[0])) | (static_cast<u32>(static_cast<u8>(s[1])) << 8) |
               (static_cast<u32>(static_cast<u8>(s[2])) << 16) | (static_cast<u32>(static_cast<u8>(s[3])) << 24);
    }

    // A legacy-header DDS (no DX10 block): FourCC or bit-mask pixel format.
    Array<u8> LegacyHeader(u32 width, u32 height, u32 mips, u32 pfFlags, u32 fourCC, u32 bitCount,
                           u32 rMask, u32 gMask, u32 bMask, u32 aMask, u32 caps2 = 0)
    {
        Array<u8> out;
        PutU32(out, 0x20534444u);
        PutU32(out, 124);
        PutU32(out, 0x1u | 0x2u | 0x4u | 0x1000u | (mips > 1 ? 0x20000u : 0u));
        PutU32(out, height);
        PutU32(out, width);
        PutU32(out, 0); // pitch / linear size (readers ignore it)
        PutU32(out, 0); // depth
        PutU32(out, mips);
        for (u32 i = 0; i < 11; ++i)
        {
            PutU32(out, 0);
        }
        PutU32(out, 32);
        PutU32(out, pfFlags);
        PutU32(out, fourCC);
        PutU32(out, bitCount);
        PutU32(out, rMask);
        PutU32(out, gMask);
        PutU32(out, bMask);
        PutU32(out, aMask);
        PutU32(out, 0x1000u);
        PutU32(out, caps2);
        PutU32(out, 0);
        PutU32(out, 0);
        PutU32(out, 0);
        return out;
    }

    // Solid 4x4 blocks.
    void Bc1Block(Array<u8>& out, u16 rgb565)
    {
        PutU16(out, rgb565);
        PutU16(out, rgb565);
        PutU32(out, 0); // every index 0 -> endpoint 0
    }
    void Bc4Block(Array<u8>& out, u8 value)
    {
        out.PushBack(value);
        out.PushBack(value);
        for (u32 i = 0; i < 6; ++i)
        {
            out.PushBack(0);
        }
    }
    void Bc3Block(Array<u8>& out, u8 alpha, u16 rgb565)
    {
        Bc4Block(out, alpha);
        Bc1Block(out, rgb565);
    }
    void Bc5Block(Array<u8>& out, u8 r, u8 g)
    {
        Bc4Block(out, r);
        Bc4Block(out, g);
    }
    // BC7 mode 6 (one subset, RGBA 7.7.7.7 + per-endpoint P bit, 4-bit indices), LSB-first.
    // Both endpoints the same colour and every index 0 reproduces (7-bit << 1 | p) exactly.
    struct BitWriter
    {
        u8 bytes[16] = {};
        u32 at = 0;
        // `value` holds at most 32 bits; a wider field (the 60 index bits) writes zeros past them
        // (shifting a u32 by 32 or more is undefined).
        void Put(u32 value, u32 bits)
        {
            for (u32 i = 0; i < bits; ++i, ++at)
            {
                if (i < 32 && ((value >> i) & 1u))
                {
                    bytes[at / 8] |= static_cast<u8>(1u << (at % 8));
                }
            }
        }
    };
    void Bc7SolidBlock(Array<u8>& out, u8 r, u8 g, u8 b, u8 a)
    {
        BitWriter w;
        w.Put(0b1000000u, 7); // mode 6
        const u8 channels[4] = {r, g, b, a};
        for (u8 c : channels)
        {
            w.Put(c >> 1, 7);
            w.Put(c >> 1, 7);
        }
        w.Put(r & 1u, 1); // p0 (the same parity in every channel, by construction)
        w.Put(r & 1u, 1); // p1
        w.Put(0, 3);      // anchor index (3 bits)
        w.Put(0, 15 * 4); // the other 15 indices
        REQUIRE(w.at == 128u);
        for (u8 byte : w.bytes)
        {
            out.PushBack(byte);
        }
    }

    Color32 PixelAt(const Image& img, u32 x, u32 y)
    {
        return img.GetPixel(x, y);
    }
}

TEST_CASE("dds: IsDds sniffs the magic, not an extension")
{
    const u8 png[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    CHECK_FALSE(IsDds(Span<const u8>(png, 8)));
    const u8 dds[4] = {'D', 'D', 'S', ' '};
    CHECK(IsDds(Span<const u8>(dds, 4)));
    CHECK_FALSE(IsDds(Span<const u8>(dds, 3)));
}

TEST_CASE("dds: a legacy DXT1 file parses as BC1 with no colour-space fact and decodes solid red")
{
    Array<u8> file = LegacyHeader(4, 4, 1, 0x4u, FourCC("DXT1"), 0, 0, 0, 0, 0);
    Bc1Block(file, 0xF800); // RGB565 pure red
    DdsImage dds;
    REQUIRE(LoadDds(Span<const u8>(file.Data(), file.Size()), dds).IsOk());
    CHECK(dds.format == DdsFormat::BC1);
    CHECK_FALSE(dds.colorSpaceKnown);
    CHECK(dds.width == 4u);
    CHECK(dds.mipLevels == 1u);
    CHECK(dds.arrayLayers == 1u);
    CHECK(dds.data.Size() == 8u);
    Image img;
    REQUIRE(DecodeLevel(dds, 0, 0, img).IsOk());
    CHECK(img.Format() == PixelFormat::RGBA8);
    CHECK(img.ColorSpace() == ImageColorSpace::Srgb); // a legacy colour format: the authoring norm
    const Color32 p = PixelAt(img, 3, 3);
    CHECK(p.r == 255);
    CHECK(p.g == 0);
    CHECK(p.b == 0);
    CHECK(p.a == 255);
}

TEST_CASE("dds: the legacy FourCC table covers DXT3/DXT5/ATI1/ATI2 and the BC4/BC5 spellings")
{
    struct Row
    {
        const char code[5];
        DdsFormat format;
    };
    const Row rows[] = {{"DXT3", DdsFormat::BC2}, {"DXT5", DdsFormat::BC3}, {"ATI1", DdsFormat::BC4},
                        {"BC4U", DdsFormat::BC4}, {"BC4S", DdsFormat::BC4Snorm},
                        {"ATI2", DdsFormat::BC5}, {"BC5U", DdsFormat::BC5}, {"BC5S", DdsFormat::BC5Snorm}};
    for (const Row& row : rows)
    {
        Array<u8> file = LegacyHeader(4, 4, 1, 0x4u, FourCC(row.code), 0, 0, 0, 0, 0);
        for (u32 i = 0; i < 16; ++i)
        {
            file.PushBack(0); // one (zero) block, enough bytes for either block size
        }
        DdsImage dds;
        REQUIRE(LoadDds(Span<const u8>(file.Data(), file.Size()), dds).IsOk());
        CHECK(dds.format == row.format);
    }
}

TEST_CASE("dds: BC3 carries alpha, BC4 replicates its channel, BC5 gets a reconstructed Z")
{
    {
        Array<u8> file = LegacyHeader(4, 4, 1, 0x4u, FourCC("DXT5"), 0, 0, 0, 0, 0);
        Bc3Block(file, 128, 0x07E0); // half alpha, pure green
        DdsImage dds;
        REQUIRE(LoadDds(Span<const u8>(file.Data(), file.Size()), dds).IsOk());
        Image img;
        REQUIRE(DecodeLevel(dds, 0, 0, img).IsOk());
        const Color32 p = PixelAt(img, 0, 0);
        CHECK(p.g == 255);
        CHECK(p.r == 0);
        CHECK(p.a == 128);
    }
    {
        Array<u8> file = LegacyHeader(4, 4, 1, 0x4u, FourCC("ATI1"), 0, 0, 0, 0, 0);
        Bc4Block(file, 77);
        DdsImage dds;
        REQUIRE(LoadDds(Span<const u8>(file.Data(), file.Size()), dds).IsOk());
        Image img;
        REQUIRE(DecodeLevel(dds, 0, 0, img).IsOk());
        CHECK(img.ColorSpace() == ImageColorSpace::Linear); // a data format
        const Color32 p = PixelAt(img, 2, 1);
        CHECK(p.r == 77);
        CHECK(p.g == 77);
        CHECK(p.b == 77);
        CHECK(p.a == 255);
    }
    {
        Array<u8> file = LegacyHeader(4, 4, 1, 0x4u, FourCC("ATI2"), 0, 0, 0, 0, 0);
        Bc5Block(file, 128, 128); // a flat normal's XY
        DdsImage dds;
        REQUIRE(LoadDds(Span<const u8>(file.Data(), file.Size()), dds).IsOk());
        Image img;
        REQUIRE(DecodeLevel(dds, 0, 0, img).IsOk());
        const Color32 p = PixelAt(img, 1, 1);
        CHECK(p.r == 128);
        CHECK(p.g == 128);
        CHECK(p.b >= 254); // z = sqrt(1 - x^2 - y^2) ~ 1 -> a whole flat normal
        CHECK(p.a == 255);
    }
}

TEST_CASE("dds: a DX10 BC7 sRGB file with a mip chain round-trips through WriteDds and decodes every level")
{
    DdsImage src;
    src.width = 8;
    src.height = 8;
    src.mipLevels = 4; // 8, 4, 2, 1
    src.format = DdsFormat::BC7Srgb;
    src.colorSpaceKnown = true;
    for (u32 level = 0; level < 4; ++level)
    {
        const u32 blocks = level == 0 ? 4u : 1u;
        for (u32 b = 0; b < blocks; ++b)
        {
            Bc7SolidBlock(src.data, 201, 101, 51, 255);
        }
    }
    REQUIRE(src.data.Size() == src.LayerSize());
    CHECK(src.LayerSize() == 64u + 16u + 16u + 16u);
    CHECK(src.LevelOffset(0, 2) == 80u);
    CHECK(src.LevelWidth(3) == 1u);

    Array<u8> file;
    REQUIRE(WriteDds(src, file).IsOk());
    CHECK(file.Size() == 4u + 124u + 20u + src.LayerSize());

    DdsImage dds;
    REQUIRE(LoadDds(Span<const u8>(file.Data(), file.Size()), dds).IsOk());
    CHECK(dds.format == DdsFormat::BC7Srgb);
    CHECK(dds.colorSpaceKnown);
    CHECK(dds.mipLevels == 4u);
    CHECK(dds.width == 8u);
    CHECK(dds.data.Size() == src.data.Size());
    CHECK(std::memcmp(dds.data.Data(), src.data.Data(), src.data.Size()) == 0);
    for (u32 level = 0; level < 4; ++level)
    {
        Image img;
        REQUIRE(DecodeLevel(dds, 0, level, img).IsOk());
        CHECK(img.Width() == dds.LevelWidth(level));
        CHECK(img.ColorSpace() == ImageColorSpace::Srgb);
        const Color32 p = PixelAt(img, img.Width() - 1, img.Height() - 1);
        CHECK(p.r == 201);
        CHECK(p.g == 101);
        CHECK(p.b == 51);
        CHECK(p.a == 255);
    }
    // A DX10 header that names the non-sRGB twin is a linear fact, not a guess.
    src.format = DdsFormat::BC7;
    REQUIRE(WriteDds(src, file).IsOk());
    REQUIRE(LoadDds(Span<const u8>(file.Data(), file.Size()), dds).IsOk());
    Image linear;
    REQUIRE(DecodeLevel(dds, 0, 0, linear).IsOk());
    CHECK(linear.ColorSpace() == ImageColorSpace::Linear);
    CHECK(WithSrgb(DdsFormat::BC7, true) == DdsFormat::BC7Srgb);
    CHECK(WithSrgb(DdsFormat::BC5, true) == DdsFormat::BC5); // no twin
}

TEST_CASE("dds: uncompressed legacy A8R8G8B8 is BGRA in memory and swizzles to RGBA8")
{
    Array<u8> file = LegacyHeader(2, 1, 1, 0x40u | 0x1u, 0, 32, 0x00FF0000u, 0x0000FF00u,
                                  0x000000FFu, 0xFF000000u);
    const u8 texels[8] = {10, 20, 30, 40, 50, 60, 70, 80}; // B G R A per texel
    for (u8 b : texels)
    {
        file.PushBack(b);
    }
    DdsImage dds;
    REQUIRE(LoadDds(Span<const u8>(file.Data(), file.Size()), dds).IsOk());
    CHECK(dds.format == DdsFormat::BGRA8);
    Image img;
    REQUIRE(DecodeLevel(dds, 0, 0, img).IsOk());
    const Color32 p = PixelAt(img, 1, 0);
    CHECK(p.r == 70);
    CHECK(p.g == 60);
    CHECK(p.b == 50);
    CHECK(p.a == 80);
}

TEST_CASE("dds: RGBA16F decodes to RGBA32F with the half values converted")
{
    DdsImage src;
    src.width = 1;
    src.height = 1;
    src.format = DdsFormat::RGBA16F;
    src.colorSpaceKnown = true;
    const u16 halves[4] = {0x3C00, 0x3800, 0x0000, 0xC000}; // 1.0, 0.5, 0.0, -2.0
    for (u16 h : halves)
    {
        PutU16(src.data, h);
    }
    Array<u8> file;
    REQUIRE(WriteDds(src, file).IsOk());
    DdsImage dds;
    REQUIRE(LoadDds(Span<const u8>(file.Data(), file.Size()), dds).IsOk());
    Image img;
    REQUIRE(DecodeLevel(dds, 0, 0, img).IsOk());
    CHECK(img.Format() == PixelFormat::RGBA32F);
    CHECK(img.ColorSpace() == ImageColorSpace::Linear);
    const f32* f = reinterpret_cast<const f32*>(img.PixelData().Data());
    CHECK(f[0] == doctest::Approx(1.0f));
    CHECK(f[1] == doctest::Approx(0.5f));
    CHECK(f[2] == doctest::Approx(0.0f));
    CHECK(f[3] == doctest::Approx(-2.0f));
}

TEST_CASE("dds: a DX10 cubemap is six layers, addressed layer-major")
{
    DdsImage src;
    src.width = 4;
    src.height = 4;
    src.format = DdsFormat::BC1;
    src.cubemap = true;
    src.arrayLayers = 6;
    src.colorSpaceKnown = true;
    const u16 faceColours[6] = {0xF800, 0x07E0, 0x001F, 0xFFFF, 0x0000, 0xF81F};
    for (u16 c : faceColours)
    {
        Bc1Block(src.data, c);
    }
    Array<u8> file;
    REQUIRE(WriteDds(src, file).IsOk());
    DdsImage dds;
    REQUIRE(LoadDds(Span<const u8>(file.Data(), file.Size()), dds).IsOk());
    CHECK(dds.cubemap);
    CHECK(dds.arrayLayers == 6u);
    CHECK(dds.LevelOffset(3, 0) == 24u);
    Image face;
    REQUIRE(DecodeLevel(dds, 2, 0, face).IsOk()); // +Y face: pure blue
    const Color32 p = PixelAt(face, 0, 0);
    CHECK(p.b == 255);
    CHECK(p.r == 0);
    CHECK(DecodeLevel(dds, 6, 0, face).Code() == ErrorCode::OutOfRange);
    CHECK(DecodeLevel(dds, 0, 1, face).Code() == ErrorCode::OutOfRange);
}

TEST_CASE("dds: refusals - truncated payload, a volume, an unknown format, a partial cubemap")
{
    {
        Array<u8> file = LegacyHeader(8, 8, 1, 0x4u, FourCC("DXT1"), 0, 0, 0, 0, 0);
        Bc1Block(file, 0xF800); // one block where four are due
        DdsImage dds;
        CHECK(LoadDds(Span<const u8>(file.Data(), file.Size()), dds).Code() == ErrorCode::InvalidArgument);
    }
    {
        Array<u8> file = LegacyHeader(4, 4, 1, 0x4u, FourCC("DXT1"), 0, 0, 0, 0, 0, /*caps2*/ 0x200000u);
        Bc1Block(file, 0xF800);
        DdsImage dds;
        CHECK(LoadDds(Span<const u8>(file.Data(), file.Size()), dds).Code() == ErrorCode::NotSupported);
    }
    {
        Array<u8> file = LegacyHeader(4, 4, 1, 0x4u, FourCC("UYVY"), 0, 0, 0, 0, 0);
        for (u32 i = 0; i < 64; ++i)
        {
            file.PushBack(0);
        }
        DdsImage dds;
        CHECK(LoadDds(Span<const u8>(file.Data(), file.Size()), dds).Code() == ErrorCode::NotSupported);
    }
    {
        Array<u8> file = LegacyHeader(4, 4, 1, 0x4u, FourCC("DXT1"), 0, 0, 0, 0, 0, /*caps2*/ 0x200u | 0x400u);
        Bc1Block(file, 0xF800);
        DdsImage dds;
        CHECK(LoadDds(Span<const u8>(file.Data(), file.Size()), dds).Code() == ErrorCode::NotSupported);
    }
    {
        const u8 junk[3] = {'D', 'D', 'S'};
        DdsImage dds;
        CHECK(LoadDds(Span<const u8>(junk, 3), dds).Code() == ErrorCode::InvalidArgument);
    }
}

TEST_CASE("dds: LoadDdsAsImage is level 0 of layer 0")
{
    Array<u8> file = LegacyHeader(4, 4, 3, 0x4u, FourCC("DXT1"), 0, 0, 0, 0, 0);
    Bc1Block(file, 0x001F); // level 0 blue
    Bc1Block(file, 0xF800); // level 1
    Bc1Block(file, 0xF800); // level 2
    Image img;
    REQUIRE(LoadDdsAsImage(Span<const u8>(file.Data(), file.Size()), img).IsOk());
    CHECK(img.Width() == 4u);
    CHECK(PixelAt(img, 0, 0).b == 255);
}

TEST_CASE("dds: ParseDdsHeader reads the facts from the first bytes alone; ReadDdsHeader reads only the header from a file")
{
    // An importer classifies a texture by its header (BC5 = normal, BC4 = mask, HDR = sky, a
    // DX10 colour space). It must not need the payload, so it never has to load every file
    // whole to learn this.
    DdsImage source;
    source.width = 16;
    source.height = 8;
    source.mipLevels = 3;
    source.format = DdsFormat::BC5;
    source.colorSpaceKnown = true;
    source.data.Resize(source.LayerSize(), 0);
    Array<u8> file;
    REQUIRE(WriteDds(source, file).IsOk());
    REQUIRE(file.Size() > kDdsHeaderBytes);

    DdsHeader header;
    REQUIRE(ParseDdsHeader(Span<const u8>(file.Data(), kDdsHeaderBytes), header).IsOk());
    CHECK(header.width == 16u);
    CHECK(header.height == 8u);
    CHECK(header.mipLevels == 3u);
    CHECK(header.arrayLayers == 1u);
    CHECK(!header.cubemap);
    CHECK(header.format == DdsFormat::BC5);
    CHECK(header.colorSpaceKnown);
    // The facts equal the full load's.
    DdsImage loaded;
    REQUIRE(LoadDds(Span<const u8>(file.Data(), file.Size()), loaded).IsOk());
    CHECK(loaded.format == header.format);
    CHECK(loaded.mipLevels == header.mipLevels);
    // A legacy header (128 bytes, no DX10 block) parses from those 128 alone.
    Array<u8> legacy = LegacyHeader(4, 4, 1, 0x4u, FourCC("DXT1"), 0, 0, 0, 0, 0);
    DdsHeader legacyHeader;
    REQUIRE(ParseDdsHeader(Span<const u8>(legacy.Data(), legacy.Size()), legacyHeader).IsOk());
    CHECK(legacyHeader.format == DdsFormat::BC1);
    CHECK(!legacyHeader.colorSpaceKnown);
    // Too short is refused; a DX10 header cut before its extension is refused.
    DdsHeader refused;
    CHECK(ParseDdsHeader(Span<const u8>(file.Data(), 100), refused).Code() == ErrorCode::InvalidArgument);
    CHECK(ParseDdsHeader(Span<const u8>(file.Data(), 130), refused).Code() == ErrorCode::InvalidArgument);
    // From a file: only the header is read (the payload may be anything, even truncated).
    const StringView path = u8"dds_header_probe.dds";
    Array<byte> truncated;
    for (usize i = 0; i < kDdsHeaderBytes + 4; ++i)
    {
        truncated.PushBack(static_cast<byte>(file[i]));
    }
    REQUIRE(WriteFile(path, Span<const byte>(truncated.Data(), truncated.Size())).IsOk());
    DdsHeader fromFile;
    REQUIRE(ReadDdsHeader(path, fromFile).IsOk());
    CHECK(fromFile.format == DdsFormat::BC5);
    CHECK(fromFile.width == 16u);
    CHECK(ReadDdsHeader(u8"dds_header_missing.dds", fromFile).Code() == ErrorCode::NotFound);
    (void)FileDelete(path);
}
