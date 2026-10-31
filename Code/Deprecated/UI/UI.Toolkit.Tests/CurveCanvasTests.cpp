// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Tests for the toolkit CurveCanvas: channels, keys read back, defaults, key selection events,
// dragging past the framed range, zoom and pan. No font or VG rendering; input goes straight to the
// mouse handlers.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.ui;
import foundation.ui.toolkit;

using namespace foundation::ui;
using namespace foundation::ui::toolkit;
using namespace foundation::core;
namespace core = foundation::core;

TEST_CASE("toolkit-curvecanvas: SetChannelsAndKeys")
{
    auto cv = core::MakeRef<CurveCanvas>(core::DefaultAllocator());

    // Defaults on a fresh canvas.
    CHECK(cv->ChannelCount() == 0);
    CHECK(cv->SelectedChannel() == -1);
    CHECK(cv->SelectedKeyIndex() == -1);
    CHECK(cv->MaxKeys == 8);
    CHECK(cv->LinkedTime == false);
    CHECK(cv->AutoFitValueRange == true);

    // Configure two channels.
    ChannelDescriptor descs[2];
    descs[0].Name = String(u8"X");
    descs[0].StrokeColor = core::Color{1, 0, 0, 1};
    descs[0].Interpolation = CurveInterpolation::Hermite;
    descs[1].Name = String(u8"Y");
    descs[1].StrokeColor = core::Color{0, 1, 0, 1};
    descs[1].Interpolation = CurveInterpolation::Linear;

    cv->SetChannels(Span<const ChannelDescriptor>(descs, 2));
    CHECK(cv->ChannelCount() == 2);
    // Selection resets to channel 0 when channels exist.
    CHECK(cv->SelectedChannel() == 0);
    CHECK(cv->SelectedKeyIndex() == -1);

    // Descriptor round-trips.
    CHECK(cv->GetChannelDescriptor(1).Interpolation == CurveInterpolation::Linear);

    // Channels start with no keys.
    CHECK(cv->GetKeyCount(0) == 0);
    CHECK(cv->GetKeyCount(1) == 0);
}

TEST_CASE("toolkit-curvecanvas: SetKeysRoundTrip")
{
    auto cv = core::MakeRef<CurveCanvas>(core::DefaultAllocator());

    ChannelDescriptor descs[1];
    descs[0].Name = String(u8"V");
    cv->SetChannels(Span<const ChannelDescriptor>(descs, 1));

    CurveCanvas::Key keys[3] = {
        CurveCanvas::Key{0.0f, 0.0f},
        CurveCanvas::Key{0.5f, 1.0f, 0.25f, -0.25f, TangentMode::Free},
        CurveCanvas::Key{1.0f, 0.0f},
    };
    cv->SetKeys(0, Span<const CurveCanvas::Key>(keys, 3));

    CHECK(cv->GetKeyCount(0) == 3);

    const CurveCanvas::Key k1 = cv->GetKey(0, 1);
    CHECK(k1.Time == doctest::Approx(0.5f));
    CHECK(k1.Value == doctest::Approx(1.0f));
    CHECK(k1.TangentIn == doctest::Approx(0.25f));
    CHECK(k1.TangentOut == doctest::Approx(-0.25f));
    CHECK(k1.Mode == TangentMode::Free);

    // An out-of-range channel index does nothing (no crash, no change).
    cv->SetKeys(5, Span<const CurveCanvas::Key>(keys, 3));
    CHECK(cv->GetKeyCount(0) == 3);
}

TEST_CASE("toolkit-curvecanvas: ValueRangeDefaults")
{
    auto cv = core::MakeRef<CurveCanvas>(core::DefaultAllocator());
    CHECK(cv->ValueMin == doctest::Approx(0.0f));
    CHECK(cv->ValueMax == doctest::Approx(1.0f));

    // An explicit value range is honored when auto-fit is disabled.
    cv->AutoFitValueRange = false;
    cv->ValueMin = -2.0f;
    cv->ValueMax = 3.0f;
    CHECK(cv->ValueMin == doctest::Approx(-2.0f));
    CHECK(cv->ValueMax == doctest::Approx(3.0f));
}

TEST_CASE("toolkit-curvecanvas: clicking ANY key fires OnSelectionChanged with its indices")
{
    // Every key pick reports itself through OnSelectionChanged, not only the first.
    auto cv = core::MakeRef<CurveCanvas>(core::DefaultAllocator());
    ChannelDescriptor d;
    d.Name = String(u8"X");
    cv->SetChannels(Span<const ChannelDescriptor>(&d, 1));
    cv->TimeSpan = 4.0f;
    // Pin the value range: hit-testing auto-fits (with a margin) before testing, so click
    // positions computed from the defaults before the fit would miss and add a key instead.
    cv->AutoFitValueRange = false;
    cv->ValueMin = 0.0f;
    cv->ValueMax = 1.0f;
    CurveCanvas::Key keys[3] = {CurveCanvas::Key{0.0f, 0.0f}, CurveCanvas::Key{2.0f, 0.5f},
                                CurveCanvas::Key{4.0f, 1.0f}};
    cv->SetKeys(0, Span<const CurveCanvas::Key>(keys, 3));
    cv->Measure(BoxConstraints::Tight(400.0f, 100.0f));
    cv->Layout(0.0f, 0.0f, 400.0f, 100.0f);

    i32 selCh = -2;
    i32 selKey = -2;
    i32 fires = 0;
    cv->OnSelectionChanged.Add(
        [&](i32 ch, i32 ki)
        {
            selCh = ch;
            selKey = ki;
            ++fires;
        });

    // TimeSpan 4 over 400 px puts t=2 s at x=200. Click each key in turn; every one (not just
    // the first) reports its selection.
    const f32 keyXs[3] = {0.0f, 200.0f, 400.0f};
    for (i32 i = 0; i < 3; ++i)
    {
        foundation::ui::MouseEventArgs down;
        down.Button = foundation::ui::MouseButton::Left;
        down.X = keyXs[i];
        // AutoFit frames the value range with a margin; hit the key's exact screen y.
        down.Y = 100.0f * (1.0f - (keys[i].Value - cv->ValueMin) /
                                      (cv->ValueMax - cv->ValueMin));
        cv->OnMouseDown(down);
        foundation::ui::MouseEventArgs up = down;
        cv->OnMouseUp(up);
        CHECK(selCh == 0);
        CHECK(selKey == i);
    }
    CHECK(fires == 3); // one per pick, none skipped, no duplicate for re-clicking
}

TEST_CASE("toolkit-curvecanvas: value axis is a viewport - drags extrapolate, wheel zooms, "
          "middle-drag pans")
{
    // Clicks and drags map through an unclamped YToValue, so a value outside the framed range is
    // reachable by direct manipulation.
    auto cv = core::MakeRef<CurveCanvas>(core::DefaultAllocator());
    ChannelDescriptor d;
    d.Name = String(u8"X");
    cv->SetChannels(Span<const ChannelDescriptor>(&d, 1));
    cv->TimeSpan = 2.0f;
    cv->AutoFitValueRange = false;
    cv->ValueMin = 0.0f;
    cv->ValueMax = 1.0f;
    CurveCanvas::Key keys[2] = {CurveCanvas::Key{0.0f, 0.0f}, CurveCanvas::Key{2.0f, 1.0f}};
    cv->SetKeys(0, Span<const CurveCanvas::Key>(keys, 2));
    cv->Measure(BoxConstraints::Tight(200.0f, 100.0f));
    cv->Layout(0.0f, 0.0f, 200.0f, 100.0f);

    // Drag the key at t=2 (x=200, value 1.0 at y=0) far above the canvas: the value
    // extrapolates past the framed max instead of pinning at 1.0.
    foundation::ui::MouseEventArgs down;
    down.Button = foundation::ui::MouseButton::Left;
    down.X = 200.0f;
    down.Y = 0.0f;
    cv->OnMouseDown(down);
    foundation::ui::MouseEventArgs move = down;
    move.Y = -100.0f; // one full canvas height above the top
    cv->OnMouseMove(move);
    foundation::ui::MouseEventArgs up = move;
    cv->OnMouseUp(up);
    CHECK(cv->GetKey(0, 1).Value == doctest::Approx(2.0f));

    // Wheel zoom-in shrinks the framed range about the cursor's value and pins the frame
    // (auto-fit stays off, as it already is here).
    foundation::ui::MouseWheelEventArgs zoom;
    zoom.Y = 50.0f; // mid-height, so the anchor is the middle of the range
    zoom.DeltaY = 1.0f;
    const f32 spanBefore = cv->ValueMax - cv->ValueMin;
    cv->OnMouseWheel(zoom);
    CHECK(cv->ValueMax - cv->ValueMin < spanBefore);
    CHECK(!cv->AutoFitValueRange);

    // Middle-drag pans the frame: both ends shift by the same amount.
    const f32 lo = cv->ValueMin;
    const f32 hi = cv->ValueMax;
    foundation::ui::MouseEventArgs panDown;
    panDown.Button = foundation::ui::MouseButton::Middle;
    panDown.X = 100.0f;
    panDown.Y = 50.0f;
    cv->OnMouseDown(panDown);
    CHECK(panDown.Handled);
    foundation::ui::MouseEventArgs panMove = panDown;
    panMove.Y = 30.0f; // dragging up shifts the frame
    cv->OnMouseMove(panMove);
    foundation::ui::MouseEventArgs panUp = panMove;
    cv->OnMouseUp(panUp);
    const f32 shift = cv->ValueMin - lo;
    CHECK(shift != doctest::Approx(0.0f));
    CHECK(cv->ValueMax - hi == doctest::Approx(shift));
    CHECK(cv->ValueMax - cv->ValueMin == doctest::Approx(hi - lo)); // pan preserves the span
}
