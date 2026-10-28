// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Splatmap thumbnail generator: the worker half runs headless - a raster with each slot
// saturated in its own quadrant must color the quadrants with the four distinct slot hues.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.image;
import editor.terrain;
import editor.core;

using namespace foundation::core;
namespace image = foundation::image;

namespace
{
    void AppendU32(Array<byte>& payload, u32 value)
    {
        for (u32 shift = 0; shift < 32; shift += 8)
        {
            payload.PushBack(static_cast<byte>((value >> shift) & 0xff));
        }
    }

    // The tile color at a point, packed for easy comparison.
    u32 TexelAt(const image::Image& tile, u32 x, u32 y)
    {
        Span<const u8> px = tile.PixelData();
        const usize p = (static_cast<usize>(y) * tile.Width() + x) * 4;
        return (static_cast<u32>(px[p]) << 16) | (static_cast<u32>(px[p + 1]) << 8) | px[p + 2];
    }
}

TEST_CASE("splatmap thumbnail gives each dominant slot a distinct hue")
{
    constexpr u32 kSide = 32;
    Array<byte> payload;
    payload.PushBack(byte{'S'});
    payload.PushBack(byte{'P'});
    payload.PushBack(byte{'L'});
    payload.PushBack(byte{'T'});
    AppendU32(payload, kSide);
    AppendU32(payload, kSide);
    for (u32 y = 0; y < kSide; ++y)
    {
        for (u32 x = 0; x < kSide; ++x)
        {
            const u32 slot = (x < kSide / 2 ? 0u : 1u) + (y < kSide / 2 ? 0u : 2u);
            for (u32 k = 0; k < 4; ++k)
            {
                payload.PushBack(byte{k == slot ? u8{255} : u8{0}});
            }
        }
    }

    editor::SplatmapThumbnailGenerator generator;
    image::Image tile;
    REQUIRE(generator.Generate(Span<const byte>(payload.Data(), payload.Size()), tile).IsOk());
    REQUIRE(tile.Width() == editor::kThumbnailServiceThumbnailSize);

    const u32 quadrant[4] = {TexelAt(tile, 32, 32), TexelAt(tile, 96, 32),
                             TexelAt(tile, 32, 96), TexelAt(tile, 96, 96)};
    for (u32 i = 0; i < 4; ++i)
    {
        for (u32 j = i + 1; j < 4; ++j)
        {
            CHECK(quadrant[i] != quadrant[j]);
        }
    }
}
