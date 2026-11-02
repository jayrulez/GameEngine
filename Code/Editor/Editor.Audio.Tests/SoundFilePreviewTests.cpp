// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The import window's preview of a sound file: read off the main thread, then its waveform and its
// facts (length, channels and rate, peak). Play needs an audio engine, which a test has none of,
// and says so. A file that does not decode says that instead.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.audio;
import editor.core;
import editor.audio;

using namespace foundation::core;
namespace audio = foundation::audio;

TEST_CASE("sound file preview: read off the main thread, then the waveform with its length, channels and rate, and peak")
{
    // One second of mono 8 kHz at half scale: a peak of -6.0 dB.
    constexpr u32 kRate = 8000;
    Array<i16> samples;
    samples.Resize(kRate);
    for (u32 i = 0; i < kRate; ++i)
        samples[i] = (i % 8 < 4) ? i16{16384} : i16{-16384};
    Array<byte> wav;
    REQUIRE(audio::EncodeWavFromPcm16(Span<const i16>(samples.Data(), samples.Size()), 1, kRate, wav));
    const StringView path = u8"scratch_sound_file_preview.wav";
    REQUIRE(WriteFile(path, Span<const byte>(wav.Data(), wav.Size())).IsOk());

    editor::ImportPreviewReader reader(DefaultAllocator());
    {
        editor::SoundFilePreview preview(path, reader, DefaultAllocator(), nullptr);
        u32 changed = 0;
        preview.onChanged = [&changed]() { ++changed; };
        CHECK_FALSE(preview.HasSound()); // still reading
        CHECK(preview.Facts().IsEmpty());
        reader.Finish();
        CHECK(changed == 1u);
        REQUIRE(preview.HasSound());
        CHECK_FALSE(preview.Waveform().Peaks().IsEmpty());
        const Span<const String> facts = preview.Facts();
        REQUIRE(facts.Size() == 3u);
        CHECK(facts[0] == StringView(u8"1.00 s"));
        CHECK(facts[1] == StringView(u8"Mono, 8000 Hz"));
        CHECK(facts[2] == StringView(u8"Peak -6.0 dB"));

        // No engine here: Play says so and nothing follows.
        CHECK_FALSE(preview.Play());
        CHECK(preview.State() == StringView(u8"No audio engine"));
        preview.Stop();
        CHECK(preview.State() == StringView(u8"Stopped"));
        CHECK(preview.Waveform().Playhead() < 0.0f);
    }
    FileDelete(path);

    editor::SoundFilePreview missing(u8"scratch_sound_file_preview_missing.wav", reader, DefaultAllocator(), nullptr);
    reader.Finish();
    CHECK_FALSE(missing.HasSound());
    CHECK(missing.Facts().IsEmpty());
}

TEST_CASE("sound file preview: a peak said in decibels, silence as silent")
{
    CHECK(editor::PeakText(1.0f) == StringView(u8"Peak 0.0 dB"));
    CHECK(editor::PeakText(0.5f) == StringView(u8"Peak -6.0 dB"));
    CHECK(editor::PeakText(0.0f) == StringView(u8"Silent"));
}
