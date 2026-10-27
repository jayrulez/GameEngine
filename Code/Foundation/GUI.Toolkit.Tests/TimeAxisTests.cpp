// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Toolkit - one time axis (T5 of Specs/gui-lab-evolution-3.md): the scale, the scroll, a frame rate
// and the playhead held once and told to every view on it; the ruler's ticks at a step whose labels
// never collide, in seconds or frames; a drag on it scrubbing, the wheel zooming about the pointer
// and a middle drag panning; a marker source's markers chosen, moved and named on it.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.gui;
import foundation.gui.toolkit;

using namespace foundation::gui;
namespace core = foundation::core;
using core::f32;
using core::usize;

namespace
{
    struct Counter : ITimeAxisClient
    {
        int changes = 0;
        void OnTimeAxisChanged() override { ++changes; }
    };

    // Markers in memory: a move's changes counted, a cancel putting the marker back.
    struct Markers : ITimeMarkerSource
    {
        core::Array<TimeMarker> markers;
        f32 before = 0.0f;
        int commits = 0;
        int cancels = 0;
        usize MarkerCount() const override { return markers.Size(); }
        TimeMarker GetMarker(usize marker) const override { return markers[marker]; }
        void BeginMarkerMove(usize marker) override
        {
            moving = marker;
            before = markers[marker].Time;
        }
        void PreviewMarkerMove(usize marker, f32 time) override { markers[marker].Time = time; }
        void CommitMarkerMove() override { ++commits; }
        void CancelMarkerMove() override
        {
            markers[moving].Time = before;
            ++cancels;
        }
        void Changed() { NotifyTimeMarkersChanged(); }
        usize moving = 0;
    };

    struct Bed
    {
        core::RefPtr<SceneRoot> root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
        core::RefPtr<TimeAxis> axis = core::MakeRef<TimeAxis>(core::DefaultAllocator());
        core::RefPtr<TimeRuler> ruler = core::MakeRef<TimeRuler>(core::DefaultAllocator());

        Bed()
        {
            root->SetSize(core::Float2{600.0f, 100.0f});
            root->EditLayoutStyle([](LayoutStyle& s) { s.AlignItems = Align::Start; });
            ruler->EditLayoutStyle([](LayoutStyle& s) { s.Width = LayoutLength::Px(500.0f); });
            root->AddChild(ruler.Get());
            ruler->SetTimeAxis(axis);
            (void)root->UpdateLayout();
        }
        EventDispatcher& D() { return *root->GetEventDispatcher(); }
    };
}

TEST_CASE("TimeAxis: offsets follow the scale and the scroll, and a zoom keeps the time under the pointer")
{
    auto axis = core::MakeRef<TimeAxis>(core::DefaultAllocator());
    axis->SetDuration(20.0f);
    axis->SetPixelsPerSecond(50.0f);
    axis->SetScroll(2.0f);
    CHECK(axis->TimeToOffset(4.0f) == doctest::Approx(100.0f));
    CHECK(axis->OffsetToTime(100.0f) == doctest::Approx(4.0f));

    axis->ZoomAbout(4.0f, 2.0f);
    CHECK(axis->PixelsPerSecond() == doctest::Approx(100.0f));
    CHECK(axis->TimeToOffset(4.0f) == doctest::Approx(100.0f));

    axis->Frame(5.0f, 10.0f, 250.0f);
    CHECK(axis->Scroll() == doctest::Approx(5.0f));
    CHECK(axis->TimeToOffset(10.0f) == doctest::Approx(250.0f));
}

TEST_CASE("TimeAxis: the scroll and the playhead stay in the clip; a frame rate snaps")
{
    auto axis = core::MakeRef<TimeAxis>(core::DefaultAllocator());
    axis->SetDuration(4.0f);
    axis->SetScroll(-1.0f);
    CHECK(axis->Scroll() == 0.0f);
    axis->SetScroll(9.0f);
    CHECK(axis->Scroll() == 4.0f);
    axis->SetPlayhead(7.0f);
    CHECK(axis->Playhead() == 4.0f);
    axis->SetDuration(2.0f);
    CHECK(axis->Scroll() == 2.0f);
    CHECK(axis->Playhead() == 2.0f);

    CHECK(axis->Snap(0.37f) == 0.37f);
    axis->SetFrameRate(10.0f);
    CHECK(axis->Snap(0.37f) == doctest::Approx(0.4f));
}

TEST_CASE("TimeAxis: every view on it is told of a change, and one that leaves is not")
{
    auto axis = core::MakeRef<TimeAxis>(core::DefaultAllocator());
    Counter a, b;
    axis->Join(&a);
    axis->Join(&b);
    axis->SetPixelsPerSecond(80.0f);
    axis->SetPlayhead(1.0f);
    CHECK(a.changes == 2);
    CHECK(b.changes == 2);
    axis->Leave(&a);
    axis->SetScroll(1.0f);
    CHECK(a.changes == 2);
    CHECK(b.changes == 3);
    axis->SetScroll(1.0f); // no change, no word
    CHECK(b.changes == 3);

    float played = -1.0f;
    axis->SetOnPlayheadChanged([&played](f32 t) { played = t; });
    axis->SetPlayhead(2.5f);
    CHECK(played == doctest::Approx(2.5f));
}

TEST_CASE("TimeAxis: the tick step is 1, 2 or 5 times a power of ten, of seconds or of frames")
{
    auto axis = core::MakeRef<TimeAxis>(core::DefaultAllocator());
    axis->SetPixelsPerSecond(100.0f);
    CHECK(axis->TickStep(60.0f) == doctest::Approx(1.0f)); // 0.6 s at least
    CHECK(axis->TickStep(100.0f) == doctest::Approx(1.0f));
    CHECK(axis->TickStep(150.0f) == doctest::Approx(2.0f));
    axis->SetPixelsPerSecond(1000.0f);
    CHECK(axis->TickStep(60.0f) == doctest::Approx(0.1f));
    CHECK(axis->TickStep(150.0f) == doctest::Approx(0.2f));
    CHECK(axis->TickStep(300.0f) == doctest::Approx(0.5f));
    CHECK(axis->FormatTime(0.3f, 0.1f) == core::StringView(u8"0.3"));
    CHECK(axis->FormatTime(4.0f, 2.0f) == core::StringView(u8"4"));

    // Frames: never less than one, and labels are frame numbers.
    axis->SetFrameRate(30.0f);
    CHECK(axis->TickStep(60.0f) * 30.0f == doctest::Approx(2.0f)); // 1.8 frames at least
    axis->SetPixelsPerSecond(100000.0f); // clamped, but a frame is still the least
    CHECK(axis->TickStep(1.0f) * 30.0f == doctest::Approx(1.0f));
    CHECK(axis->FormatTime(0.5f, 1.0f / 30.0f) == core::StringView(u8"15"));
}

TEST_CASE("TimeRuler: its labelled ticks are at least the spacing apart and cover what it shows")
{
    Bed bed;
    bed.axis->SetDuration(100.0f);
    bed.axis->SetPixelsPerSecond(100.0f);
    core::Array<TimeTick> ticks{core::DefaultAllocator()};
    bed.ruler->GetTicks(ticks);
    REQUIRE(!ticks.IsEmpty());
    CHECK(ticks[0].Time == 0.0f);
    CHECK(ticks[0].Major);
    CHECK(ticks[0].Label == core::StringView(u8"0"));
    f32 lastMajor = -1000.0f;
    int majors = 0;
    for (const TimeTick& tick : ticks)
    {
        CHECK(tick.Offset <= 500.0f + 0.01f);
        if (!tick.Major)
            continue;
        CHECK(tick.Offset - lastMajor >= TimeRuler::kMinLabelSpacing - 0.01f);
        lastMajor = tick.Offset;
        ++majors;
    }
    CHECK(majors == 6); // 0 to 5 at 100 px a second over 500 px
    CHECK(ticks.Size() == 26); // fifths of a second between them

    // Scrolled to 2.5 s, the first tick is the first minor one in view.
    bed.axis->SetScroll(2.5f);
    bed.ruler->GetTicks(ticks);
    CHECK(ticks[0].Time == doctest::Approx(2.6f));
    CHECK(ticks[0].Offset == doctest::Approx(10.0f));
}

TEST_CASE("TimeRuler: a drag scrubs the playhead, on frames with a frame rate")
{
    Bed bed;
    bed.axis->SetDuration(4.0f);
    bed.axis->SetPixelsPerSecond(100.0f);
    bed.axis->SetFrameRate(10.0f);
    bed.D().InjectMouseMove(core::Float2{123.0f, 10.0f});
    bed.D().InjectMouseDown(core::Float2{123.0f, 10.0f}, MouseButton::Left);
    CHECK(bed.axis->Playhead() == doctest::Approx(1.2f));
    bed.D().InjectMouseMove(core::Float2{257.0f, 10.0f});
    CHECK(bed.axis->Playhead() == doctest::Approx(2.6f));
    bed.D().InjectMouseMove(core::Float2{580.0f, 40.0f}); // past its end: held, kept in the clip
    CHECK(bed.axis->Playhead() == doctest::Approx(4.0f));
    bed.D().InjectMouseUp(core::Float2{580.0f, 40.0f}, MouseButton::Left);
    bed.D().InjectMouseMove(core::Float2{50.0f, 10.0f});
    CHECK(bed.axis->Playhead() == doctest::Approx(4.0f));
}

TEST_CASE("TimeRuler: the wheel zooms about the pointer and a middle drag pans")
{
    Bed bed;
    bed.axis->SetDuration(60.0f);
    bed.axis->SetPixelsPerSecond(100.0f);
    bed.axis->SetScroll(1.0f);
    const f32 under = bed.axis->OffsetToTime(200.0f); // 3 s
    bed.D().InjectMouseMove(core::Float2{200.0f, 10.0f});
    bed.D().InjectMouseWheel(core::Float2{200.0f, 10.0f}, core::Float2{0.0f, 1.0f});
    CHECK(bed.axis->PixelsPerSecond() == doctest::Approx(120.0f));
    CHECK(bed.axis->OffsetToTime(200.0f) == doctest::Approx(under));
    bed.D().InjectMouseWheel(core::Float2{200.0f, 10.0f}, core::Float2{0.0f, -1.0f});
    CHECK(bed.axis->PixelsPerSecond() == doctest::Approx(100.0f));

    bed.D().InjectMouseDown(core::Float2{200.0f, 10.0f}, MouseButton::Middle);
    bed.D().InjectMouseMove(core::Float2{100.0f, 10.0f});
    CHECK(bed.axis->Scroll() == doctest::Approx(2.0f));
    bed.D().InjectMouseUp(core::Float2{100.0f, 10.0f}, MouseButton::Middle);
    CHECK(bed.axis->Playhead() == 0.0f); // panning is not scrubbing
}

TEST_CASE("TimeRuler: a ruler that goes leaves its axis")
{
    auto axis = core::MakeRef<TimeAxis>(core::DefaultAllocator());
    {
        auto ruler = core::MakeRef<TimeRuler>(core::DefaultAllocator());
        ruler->SetTimeAxis(axis);
        CHECK(axis->ClientCount() == 1);
        auto other = core::MakeRef<TimeAxis>(core::DefaultAllocator());
        ruler->SetTimeAxis(other);
        CHECK(axis->ClientCount() == 0);
        ruler->SetTimeAxis(axis);
    }
    CHECK(axis->ClientCount() == 0);
    axis->SetScroll(1.0f); // nothing left to tell
}

TEST_CASE("TimeRuler: a click chooses a marker, a drag moves it as one change, Escape gives the move up")
{
    Bed bed;
    bed.axis->SetDuration(4.0f);
    bed.axis->SetPixelsPerSecond(100.0f);
    bed.axis->SetFrameRate(10.0f);
    Markers markers;
    markers.markers.PushBack(TimeMarker{1.0f, core::String(u8"Footstep")});
    markers.markers.PushBack(TimeMarker{3.0f, core::String(u8"Land")});
    bed.ruler->SetMarkerSource(&markers);
    core::i32 told = -2;
    bed.ruler->OnMarkerChosen = [&told](core::i32 marker) { told = marker; };

    // Pressed near it: chosen, the playhead left where it was.
    bed.D().InjectMouseMove(core::Float2{303.0f, 10.0f});
    CHECK(bed.ruler->GetTooltipText() == core::StringView(u8"Land at 30")); // in frames, as the ruler labels
    bed.D().InjectMouseDown(core::Float2{303.0f, 10.0f}, MouseButton::Left);
    CHECK(bed.ruler->GetChosenMarker() == 1);
    CHECK(told == 1);
    bed.D().InjectMouseUp(core::Float2{303.0f, 10.0f}, MouseButton::Left);
    CHECK(bed.axis->Playhead() == 0.0f);
    CHECK(markers.commits == 0); // a click moves nothing

    // Dragged: on frames, one change.
    bed.D().InjectMouseDown(core::Float2{100.0f, 10.0f}, MouseButton::Left);
    CHECK(bed.ruler->GetChosenMarker() == 0);
    bed.D().InjectMouseMove(core::Float2{152.0f, 10.0f});
    CHECK(markers.markers[0].Time == doctest::Approx(1.5f));
    bed.D().InjectMouseMove(core::Float2{177.0f, 10.0f});
    CHECK(markers.markers[0].Time == doctest::Approx(1.8f));
    bed.D().InjectMouseUp(core::Float2{177.0f, 10.0f}, MouseButton::Left);
    CHECK(markers.commits == 1);
    CHECK(bed.axis->Playhead() == 0.0f);

    // Escape: back where it was, nothing committed.
    bed.D().InjectMouseDown(core::Float2{180.0f, 10.0f}, MouseButton::Left);
    bed.D().InjectMouseMove(core::Float2{260.0f, 10.0f});
    CHECK(markers.markers[0].Time == doctest::Approx(2.6f));
    bed.D().InjectKeyDown(static_cast<core::u32>(KeyCode::Escape));
    CHECK(markers.markers[0].Time == doctest::Approx(1.8f));
    CHECK(markers.cancels == 1);
    bed.D().InjectMouseUp(core::Float2{260.0f, 10.0f}, MouseButton::Left);
    CHECK(markers.commits == 1);

    // Away from every marker, a press scrubs as before.
    bed.D().InjectMouseDown(core::Float2{50.0f, 10.0f}, MouseButton::Left);
    CHECK(bed.axis->Playhead() == doctest::Approx(0.5f));
    bed.D().InjectMouseUp(core::Float2{50.0f, 10.0f}, MouseButton::Left);
    CHECK(bed.ruler->GetChosenMarker() == 0); // a scrub keeps the choice

    // The chosen marker gone: none chosen, told.
    markers.markers.RemoveAt(0);
    markers.markers.RemoveAt(0);
    markers.Changed();
    CHECK(bed.ruler->GetChosenMarker() == -1);
    CHECK(told == -1);
    bed.ruler->SetMarkerSource(nullptr);
}
