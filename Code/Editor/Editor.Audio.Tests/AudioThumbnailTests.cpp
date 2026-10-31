// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Audio thumbnail generator: the worker half runs headless. An encoded WAV with a loud middle and
// silent edges must produce tall center bars and midline-only edges.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.audio;
import foundation.image;
import editor.audio;
import editor.core;

using namespace foundation::core;
namespace image = foundation::image;
namespace audio = foundation::audio;

namespace
{
    // Bar height at column x: count teal rows (the ground is untouched outside bars).
    u32 BarHeight(const image::Image& tile, u32 x)
    {
        Span<const u8> px = tile.PixelData();
        u32 rows = 0;
        for (u32 y = 0; y < tile.Height(); ++y)
        {
            const usize p = (static_cast<usize>(y) * tile.Width() + x) * 4;
            if (px[p + 1] > 150) // the teal's green channel
            {
                ++rows;
            }
        }
        return rows;
    }
}

TEST_CASE("audio thumbnail waveform tracks the clip's envelope")
{
    // 1 second mono 8kHz: silence, then a loud square wave, then silence.
    constexpr u32 kRate = 8000;
    Array<i16> samples;
    samples.Resize(kRate);
    for (u32 i = 0; i < kRate; ++i)
    {
        const bool loud = i >= kRate / 3 && i < (kRate * 2) / 3;
        samples[i] = loud ? ((i % 8 < 4) ? i16{28000} : i16{-28000}) : i16{0};
    }
    Array<byte> wav;
    REQUIRE(audio::EncodeWavFromPcm16(Span<const i16>(samples.Data(), samples.Size()), 1, kRate,
                                      wav));

    editor::AudioClipThumbnailGenerator generator;
    image::Image tile;
    REQUIRE(generator.Generate(Span<const byte>(wav.Data(), wav.Size()), tile).IsOk());
    REQUIRE(tile.Width() == editor::kThumbnailServiceThumbnailSize);

    CHECK(BarHeight(tile, 64) > 80);  // loud middle fills most of the strip
    CHECK(BarHeight(tile, 4) <= 4);   // silent edges keep only the 1px midline
    CHECK(BarHeight(tile, 124) <= 4);
}

TEST_CASE("audio thumbnail rejects undecodable bytes")
{
    const byte junk[16] = {};
    editor::AudioClipThumbnailGenerator generator;
    image::Image tile;
    CHECK(!generator.Generate(Span<const byte>(junk, 16), tile).IsOk());
}
