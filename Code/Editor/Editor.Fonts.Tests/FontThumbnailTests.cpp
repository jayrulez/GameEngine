// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Font thumbnail generator: the worker half runs headless against a real bundled face, and the
// glyph sample must put ink over the tile's ground.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.vfs; // the engine data root (FindDataRoot / DataPath)
import foundation.image;
import editor.fonts;
import editor.core;

using namespace foundation::core;
namespace image = foundation::image;

TEST_CASE("font thumbnail rasterizes a glyph sample from TTF bytes")
{
    Array<byte> ttf;
    {
        const String root = foundation::vfs::FindDataRoot();
        REQUIRE_FALSE(root.IsEmpty());
        FileStream stream(
            foundation::vfs::DataPath(root.AsView(), u8"Engine/Fonts/roboto/Roboto-Bold.ttf").AsView(),
            FileMode::Read);
        REQUIRE(stream.IsValid());
        const i64 size = stream.Size();
        REQUIRE(size > 0);
        ttf.Resize(static_cast<usize>(size));
        REQUIRE(stream.Read(ttf.Data(), static_cast<u64>(size)) == static_cast<u64>(size));
    }

    editor::FontThumbnailGenerator generator;
    image::Image tile;
    REQUIRE(generator.Generate(Span<const byte>(ttf.Data(), ttf.Size()), tile).IsOk());
    REQUIRE(tile.Width() == editor::kThumbnailServiceThumbnailSize);

    // Ink coverage: a meaningful share of texels must differ from the flat ground.
    Span<const u8> px = tile.PixelData();
    usize inked = 0;
    for (usize p = 0; p < px.Size(); p += 4)
    {
        if (px[p] > 60)
        {
            ++inked;
        }
    }
    CHECK(inked > 200); // "Ag" at 72px covers far more than noise
}

TEST_CASE("font thumbnail rejects non-font bytes")
{
    const byte junk[32] = {};
    editor::FontThumbnailGenerator generator;
    image::Image tile;
    CHECK(!generator.Generate(Span<const byte>(junk, 32), tile).IsOk());
}
