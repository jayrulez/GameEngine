// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::Scene tests - the zoom readout's pure parts: the scale bar's round length and its pixels
// at the focus, the words for a length, and when the readout shows (held, then fading).
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import editor.scene;

using namespace foundation::core;

TEST_CASE("zoom readout: the scale bar is the longest round length within its target pixels")
{
    // At 60 degrees the view is 2 tan(30) d tall: at 10 m over 1000 px, 1.1547 cm a pixel.
    const f32 fovY = 1.0471976f;
    const f32 metresPerPixel = 2.0f * 10.0f * Tan(0.5f * fovY) / 1000.0f;
    const editor::ScaleBar bar = editor::ScaleBarAt(10.0f, fovY, 1000.0f, 120.0f);
    // 120 px is 1.39 m there: the bar is 1 m (2 m would not fit), spanning 86.6 px.
    CHECK(bar.metres == doctest::Approx(1.0f));
    CHECK(bar.pixels == doctest::Approx(1.0f / metresPerPixel));
    CHECK(bar.pixels <= 120.0f);

    // Steps 1, 2, 5 through the decades as the focus moves out.
    CHECK(editor::ScaleBarAt(20.0f, fovY, 1000.0f).metres == doctest::Approx(2.0f));
    CHECK(editor::ScaleBarAt(40.0f, fovY, 1000.0f).metres == doctest::Approx(5.0f));
    CHECK(editor::ScaleBarAt(100.0f, fovY, 1000.0f).metres == doctest::Approx(10.0f));
    CHECK(editor::ScaleBarAt(0.5f, fovY, 1000.0f).metres == doctest::Approx(0.05f));

    // Every bar fits its target and is more than half of it (1, 2, 5 steps are at most 2.5 apart).
    for (f32 distance = 0.05f; distance < 5000.0f; distance *= 1.37f)
    {
        CAPTURE(distance);
        const editor::ScaleBar b = editor::ScaleBarAt(distance, fovY, 900.0f, 120.0f);
        CHECK(b.pixels <= 120.0f + 1e-3f);
        CHECK(b.pixels >= 120.0f * 0.4f - 1e-3f);
    }

    // A degenerate view has no bar.
    CHECK(editor::ScaleBarAt(0.0f, fovY, 1000.0f).metres == 0.0f);
    CHECK(editor::ScaleBarAt(10.0f, fovY, 0.0f).metres == 0.0f);
}

TEST_CASE("zoom readout: a length reads in millimetres, centimetres, metres or kilometres")
{
    CHECK(editor::FormatMetres(0.005f) == StringView(u8"5 mm"));
    CHECK(editor::FormatMetres(0.05f) == StringView(u8"5 cm"));
    CHECK(editor::FormatMetres(0.5f) == StringView(u8"50 cm"));
    CHECK(editor::FormatMetres(1.0f) == StringView(u8"1 m"));
    CHECK(editor::FormatMetres(2.5f) == StringView(u8"2.5 m"));
    CHECK(editor::FormatMetres(12.4f) == StringView(u8"12 m"));
    CHECK(editor::FormatMetres(500.0f) == StringView(u8"500 m"));
    CHECK(editor::FormatMetres(1200.0f) == StringView(u8"1.2 km"));
    CHECK(editor::FormatMetres(2000.0f) == StringView(u8"2 km"));
}

TEST_CASE("zoom readout: shown whole after a zoom, fading once the zoom stops, then gone")
{
    editor::ZoomReadout readout;
    CHECK(readout.Opacity() == 0.0f);

    readout.Zoomed();
    CHECK(readout.Opacity() == 1.0f);
    readout.Advance(editor::ZoomReadout::kHoldSeconds * 0.9f);
    CHECK(readout.Opacity() == 1.0f);

    // Another notch holds it again from the start.
    readout.Zoomed();
    readout.Advance(editor::ZoomReadout::kHoldSeconds * 0.9f);
    CHECK(readout.Opacity() == 1.0f);

    // Half way through the fade, half there.
    readout.Advance(editor::ZoomReadout::kHoldSeconds * 0.1f + editor::ZoomReadout::kFadeSeconds * 0.5f);
    CHECK(readout.Opacity() == doctest::Approx(0.5f).epsilon(0.01));

    readout.Advance(editor::ZoomReadout::kFadeSeconds);
    CHECK(readout.Opacity() == 0.0f);
}
