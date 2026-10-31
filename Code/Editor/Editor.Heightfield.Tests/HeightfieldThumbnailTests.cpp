// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Heightfield thumbnail generator: the worker half runs headless on a crafted payload, and a
// vertical u16 gradient must normalize to a full black-to-white ramp in the tile.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.image;
import editor.heightfield;
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
}

TEST_CASE("heightfield thumbnail normalizes a low-relief gradient to the full ramp")
{
    constexpr u32 kSide = 64;
    Array<byte> payload;
    payload.PushBack(byte{'R'});
    payload.PushBack(byte{'1'});
    payload.PushBack(byte{'6'});
    payload.PushBack(byte{' '});
    AppendU32(payload, kSide);
    for (u32 y = 0; y < kSide; ++y)
    {
        for (u32 x = 0; x < kSide; ++x)
        {
            // A shallow band well inside u16 range: raw >>8 would render near-flat gray.
            const u16 sample = static_cast<u16>(1000 + y * 8);
            payload.PushBack(static_cast<byte>(sample & 0xff));
            payload.PushBack(static_cast<byte>(sample >> 8));
        }
    }

    editor::HeightfieldThumbnailGenerator generator;
    image::Image tile;
    REQUIRE(generator.Generate(Span<const byte>(payload.Data(), payload.Size()), tile).IsOk());
    REQUIRE(tile.Width() == editor::kThumbnailServiceThumbnailSize);
    Span<const u8> px = tile.PixelData();
    const usize lastRow =
        (static_cast<usize>(tile.Height() - 1) * tile.Width()) * 4;
    CHECK(px[0] <= 8);            // top of the ramp normalizes to black
    CHECK(px[lastRow] >= 247);    // bottom normalizes to white
    CHECK(px[3] == 255);
}

TEST_CASE("heightfield thumbnail rejects a truncated payload")
{
    Array<byte> payload;
    payload.PushBack(byte{'R'});
    payload.PushBack(byte{'1'});
    payload.PushBack(byte{'6'});
    payload.PushBack(byte{' '});
    AppendU32(payload, 64);
    payload.PushBack(byte{0}); // far too few samples

    editor::HeightfieldThumbnailGenerator generator;
    image::Image tile;
    CHECK(!generator.Generate(Span<const byte>(payload.Data(), payload.Size()), tile).IsOk());
}
