// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Timeline widget tests (headless). The Timeline is
// domain-agnostic (no clip), so these exercise the pure surface: the shared time<->pixel transform
// round-trips through zoom + scroll, the ruler tick step follows {1,2,5}x10^n and never lets
// labels collide, and the playhead clamps + fires OnPlayheadMoved only on a real change.

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.ui;
import foundation.ui.toolkit;

using namespace foundation::core;
using namespace foundation::ui;
using namespace foundation::ui::toolkit;

namespace
{
    foundation::ui::MouseEventArgs Mouse(f32 x, f32 y)
    {
        foundation::ui::MouseEventArgs e;
        e.Button = foundation::ui::MouseButton::Left;
        e.X = x;
        e.Y = y;
        return e;
    }
}

TEST_CASE("timeline: PickTickStep follows {1,2,5}x10^n and never collides labels")
{
    // 1s = 100px, min label 48px -> min step 0.48s -> 0.5s (5 x 0.1).
    CHECK(Timeline::PickTickStep(100.0f, 48.0f) == doctest::Approx(0.5f));
    // 1s = 20px -> min 2.4s -> 5s.
    CHECK(Timeline::PickTickStep(20.0f, 48.0f) == doctest::Approx(5.0f));
    // 1s = 1000px -> min 0.048s -> 0.05s (5 x 0.01).
    CHECK(Timeline::PickTickStep(1000.0f, 48.0f) == doctest::Approx(0.05f));
    // 1s = 10px -> min 4.8s -> 5s.
    CHECK(Timeline::PickTickStep(10.0f, 48.0f) == doctest::Approx(5.0f));
    // 1s = 5px -> min 9.6s -> 10s (rolls to the next decade).
    CHECK(Timeline::PickTickStep(5.0f, 48.0f) == doctest::Approx(10.0f));

    // The chosen step always spans at least the minimum label width on screen (no collisions).
    const f32 zooms[] = {5.0f, 10.0f, 37.0f, 100.0f, 250.0f, 1000.0f, 3999.0f};
    for (const f32 pps : zooms)
    {
        const f32 step = Timeline::PickTickStep(pps, 48.0f);
        CHECK(step * pps >= 48.0f);
    }
    // Degenerate inputs never divide-by-zero.
    CHECK(Timeline::PickTickStep(0.0f, 48.0f) == doctest::Approx(1.0f));
}

TEST_CASE("timeline: time<->pixel transform round-trips through zoom + scroll (D1)")
{
    auto tl = MakeRef<Timeline>(DefaultAllocator());
    tl->SetPixelsPerSecond(120.0f);
    tl->SetScrollSeconds(0.5f);
    tl->LabelColumnWidth = 40.0f;

    // x = label + (t - scroll) * pps.
    CHECK(tl->TimeToX(0.5f) == doctest::Approx(40.0f));
    CHECK(tl->TimeToX(1.5f) == doctest::Approx(160.0f));

    const f32 times[] = {0.0f, 0.5f, 1.2f, 3.7f};
    for (const f32 t : times)
    {
        CHECK(tl->XToTime(tl->TimeToX(t)) == doctest::Approx(t));
    }

    // Zoom is clamped (no runaway); the transform stays consistent after a clamp.
    tl->SetPixelsPerSecond(1.0f); // below the min -> clamped
    CHECK(tl->PixelsPerSecond() >= 4.0f);
    CHECK(tl->XToTime(tl->TimeToX(2.0f)) == doctest::Approx(2.0f));
}

TEST_CASE("timeline: playhead clamps to [0,duration] and fires only on change")
{
    auto tl = MakeRef<Timeline>(DefaultAllocator());
    tl->SetDuration(2.0f);

    i32 fires = 0;
    f32 last = -1.0f;
    tl->OnPlayheadMoved.Add(
        [&](f32 t)
        {
            ++fires;
            last = t;
        });

    tl->SetPlayheadTime(1.0f);
    CHECK(tl->PlayheadTime() == doctest::Approx(1.0f));
    CHECK(fires == 1);
    CHECK(last == doctest::Approx(1.0f));

    tl->SetPlayheadTime(1.0f); // no change -> no event
    CHECK(fires == 1);

    tl->SetPlayheadTime(5.0f); // clamp to duration
    CHECK(tl->PlayheadTime() == doctest::Approx(2.0f));
    CHECK(fires == 2);

    tl->SetPlayheadTime(-3.0f); // clamp to 0
    CHECK(tl->PlayheadTime() == doctest::Approx(0.0f));
    CHECK(fires == 3);

    // Shrinking the duration re-clamps the playhead.
    tl->SetPlayheadTime(2.0f);
    tl->SetDuration(1.0f);
    CHECK(tl->PlayheadTime() == doctest::Approx(1.0f));
}

// === dopesheet: lanes, selection, box-select, drag-is-visual ===
// pps=100, scroll=0, labelCol=0 -> TimeToX(t)=t*100. kRulerHeight=24; lane height 22 -> lane0 cy=35.

TEST_CASE("timeline dopesheet: click selects the nearest key on the lane + fires OnSelectionChanged")
{
    auto tl = MakeRef<Timeline>(DefaultAllocator());
    tl->SetPixelsPerSecond(100.0f);
    Array<DopesheetLane> lanes;
    {
        DopesheetLane l;
        l.keyTimes.PushBack(0.2f);
        l.keyTimes.PushBack(0.5f);
        lanes.PushBack(Move(l));
    }
    tl->SetLanes(Move(lanes));
    CHECK(tl->LaneCount() == 1u);

    int selEvents = 0;
    tl->OnSelectionChanged.Add([&] { ++selEvents; });

    auto down = Mouse(50.0f, 35.0f); // t=0.5 on lane 0 (index 1)
    tl->OnMouseDown(down);
    CHECK(tl->SelectedCount() == 1u);
    CHECK(tl->IsKeySelected(0, 1));
    CHECK_FALSE(tl->IsKeySelected(0, 0));
    CHECK(selEvents == 1);

    int moves = 0;
    tl->OnKeysMoved.Add([&](f32) { ++moves; });
    auto up = Mouse(50.0f, 35.0f);
    tl->OnMouseUp(up); // click, no move -> no drag
    CHECK(moves == 0);
}

TEST_CASE("timeline dopesheet: dragging a selected key emits OnKeysMoved(seconds); model stays put")
{
    auto tl = MakeRef<Timeline>(DefaultAllocator());
    tl->SetPixelsPerSecond(100.0f);
    Array<DopesheetLane> lanes;
    {
        DopesheetLane l;
        l.keyTimes.PushBack(0.5f);
        lanes.PushBack(Move(l));
    }
    tl->SetLanes(Move(lanes));

    f32 moved = -999.0f;
    int moves = 0;
    tl->OnKeysMoved.Add([&](f32 d) { moved = d; ++moves; });

    auto down = Mouse(50.0f, 35.0f);
    tl->OnMouseDown(down); // selects (0,0), begins drag
    CHECK(tl->IsKeySelected(0, 0));
    auto move = Mouse(80.0f, 35.0f);
    tl->OnMouseMove(move);
    auto up = Mouse(80.0f, 35.0f);
    tl->OnMouseUp(up);
    CHECK(moves == 1);
    CHECK(moved == doctest::Approx(0.3f)); // 30px / 100pps

    // Drags are visual: the widget never moved the key, so it is still hit at its ORIGINAL x=50.
    auto probe = Mouse(50.0f, 35.0f);
    tl->OnMouseDown(probe);
    CHECK(tl->IsKeySelected(0, 0));
    tl->OnMouseUp(probe);
}

TEST_CASE("timeline dopesheet: a sub-epsilon move is a click, not a drag")
{
    auto tl = MakeRef<Timeline>(DefaultAllocator());
    tl->SetPixelsPerSecond(100.0f);
    Array<DopesheetLane> lanes;
    {
        DopesheetLane l;
        l.keyTimes.PushBack(0.5f);
        lanes.PushBack(Move(l));
    }
    tl->SetLanes(Move(lanes));

    int moves = 0;
    tl->OnKeysMoved.Add([&](f32) { ++moves; });

    auto down = Mouse(50.0f, 35.0f);
    tl->OnMouseDown(down);
    auto move = Mouse(51.0f, 35.0f); // 1px < epsilon(3)
    tl->OnMouseMove(move);
    auto up = Mouse(51.0f, 35.0f);
    tl->OnMouseUp(up);
    CHECK(moves == 0);
}

TEST_CASE("timeline dopesheet: box-select picks the keys inside the rectangle")
{
    auto tl = MakeRef<Timeline>(DefaultAllocator());
    tl->SetPixelsPerSecond(100.0f);
    Array<DopesheetLane> lanes;
    {
        DopesheetLane l;
        l.keyTimes.PushBack(0.2f); // x=20
        l.keyTimes.PushBack(0.5f); // x=50
        lanes.PushBack(Move(l));
    }
    {
        DopesheetLane l;
        l.keyTimes.PushBack(0.8f); // x=80, lane1 cy=57
        lanes.PushBack(Move(l));
    }
    tl->SetLanes(Move(lanes));

    // Box from (10,26) to (60,60): lane0 both keys (x=20,50; cy=35) in; lane1 key x=80 out.
    auto down = Mouse(10.0f, 26.0f);
    tl->OnMouseDown(down); // empty area -> begins box-select
    auto move = Mouse(60.0f, 60.0f);
    tl->OnMouseMove(move);
    auto up = Mouse(60.0f, 60.0f);
    tl->OnMouseUp(up);
    CHECK(tl->SelectedCount() == 2u);
    CHECK(tl->IsKeySelected(0, 0));
    CHECK(tl->IsKeySelected(0, 1));
    CHECK_FALSE(tl->IsKeySelected(1, 0));
}

// === panning + scroll clamping (the "can't reach a section / ends drift past the clip" fixes) ===

TEST_CASE("timeline: scroll clamps to the content extent at every zoom")
{
    auto tl = MakeRef<Timeline>(DefaultAllocator());
    tl->SetDuration(3.0f);
    tl->SetPixelsPerSecond(100.0f);
    tl->Measure(BoxConstraints::Tight(400.0f, 60.0f));
    tl->Layout(0.0f, 0.0f, 400.0f, 60.0f);

    // Whole clip fits (4 s visible > 3 s clip): scroll is pinned at 0 - zooming out can
    // never push both ends away from the content.
    tl->SetScrollSeconds(5.0f);
    CHECK(tl->ScrollSeconds() == doctest::Approx(0.0f));

    // Zoomed in (1 s visible): scroll reaches the clip end plus a small grab tail, no further.
    tl->SetPixelsPerSecond(400.0f);
    tl->SetScrollSeconds(50.0f);
    const f32 visible = tl->VisibleSeconds();
    CHECK(visible == doctest::Approx(1.0f));
    CHECK(tl->ScrollSeconds() == doctest::Approx(3.0f + visible * 0.15f - visible));

    // Zooming back out re-clamps the stored scroll.
    tl->SetPixelsPerSecond(100.0f);
    CHECK(tl->ScrollSeconds() == doctest::Approx(0.0f));

    // Shrinking the clip re-clamps too.
    tl->SetPixelsPerSecond(400.0f);
    tl->SetScrollSeconds(50.0f);
    CHECK(tl->ScrollSeconds() > 0.0f);
    tl->SetDuration(0.5f);
    CHECK(tl->ScrollSeconds() == doctest::Approx(0.0f));
}

TEST_CASE("timeline: middle-drag pans to any section and clamps at the edges")
{
    auto tl = MakeRef<Timeline>(DefaultAllocator());
    tl->SetDuration(10.0f);
    tl->SetPixelsPerSecond(100.0f);
    tl->Measure(BoxConstraints::Tight(400.0f, 60.0f));
    tl->Layout(0.0f, 0.0f, 400.0f, 60.0f);

    foundation::ui::MouseEventArgs down = Mouse(200.0f, 10.0f);
    down.Button = foundation::ui::MouseButton::Middle;
    tl->OnMouseDown(down);
    CHECK(down.Handled);

    // Drag 100 px left -> the view moves 1 s forward in time.
    foundation::ui::MouseEventArgs move = Mouse(100.0f, 10.0f);
    tl->OnMouseMove(move);
    CHECK(tl->ScrollSeconds() == doctest::Approx(1.0f));

    // Drag far right -> clamped at 0 (never before the clip start).
    foundation::ui::MouseEventArgs farRight = Mouse(4000.0f, 10.0f);
    tl->OnMouseMove(farRight);
    CHECK(tl->ScrollSeconds() == doctest::Approx(0.0f));

    foundation::ui::MouseEventArgs up = Mouse(4000.0f, 10.0f);
    up.Button = foundation::ui::MouseButton::Middle;
    tl->OnMouseUp(up);

    // After release, moves no longer pan.
    foundation::ui::MouseEventArgs after = Mouse(100.0f, 10.0f);
    tl->OnMouseMove(after);
    CHECK(tl->ScrollSeconds() == doctest::Approx(0.0f));
}

TEST_CASE("timeline: shift+wheel and horizontal wheel pan; plain wheel still zooms")
{
    auto tl = MakeRef<Timeline>(DefaultAllocator());
    tl->SetDuration(10.0f);
    tl->SetPixelsPerSecond(100.0f);
    tl->Measure(BoxConstraints::Tight(400.0f, 60.0f));
    tl->Layout(0.0f, 0.0f, 400.0f, 60.0f);

    foundation::ui::MouseWheelEventArgs shiftWheel;
    shiftWheel.X = 200.0f;
    shiftWheel.DeltaY = -1.0f;
    shiftWheel.Modifiers = foundation::ui::KeyModifiers::Shift;
    tl->OnMouseWheel(shiftWheel);
    CHECK(shiftWheel.Handled);
    CHECK(tl->ScrollSeconds() == doctest::Approx(0.4f)); // 40 px / 100 pps

    foundation::ui::MouseWheelEventArgs horizontal;
    horizontal.X = 200.0f;
    horizontal.DeltaX = -1.0f;
    tl->OnMouseWheel(horizontal);
    CHECK(tl->ScrollSeconds() == doctest::Approx(0.8f));

    // A plain vertical wheel zooms (pixels-per-second changes), it does not pan.
    const f32 scrollBefore = tl->ScrollSeconds();
    const f32 ppsBefore = tl->PixelsPerSecond();
    foundation::ui::MouseWheelEventArgs zoom;
    zoom.X = 0.0f; // anchor at the left edge: the time under x=0 stays put
    zoom.DeltaY = 1.0f;
    tl->OnMouseWheel(zoom);
    CHECK(tl->PixelsPerSecond() > ppsBefore);
    CHECK(tl->ScrollSeconds() == doctest::Approx(scrollBefore));
}

TEST_CASE("timeline: lane labels select the track - gutter click, key pick, programmatic set")
{
    auto tl = MakeRef<Timeline>(DefaultAllocator());
    tl->SetDuration(4.0f);
    tl->SetPixelsPerSecond(100.0f);
    tl->LabelColumnWidth = 120.0f;
    Array<DopesheetLane> lanes;
    {
        DopesheetLane a;
        a.label = String(u8"Transform.position");
        a.keyTimes.PushBack(1.0f);
        lanes.PushBack(Move(a));
        DopesheetLane b;
        b.label = String(u8"Light.intensity");
        b.keyTimes.PushBack(2.0f);
        lanes.PushBack(Move(b));
    }
    tl->SetLanes(Move(lanes));
    tl->Measure(BoxConstraints::Tight(520.0f, 120.0f));
    tl->Layout(0.0f, 0.0f, 520.0f, 120.0f);
    CHECK(tl->SelectedLane() == -1);

    i32 fired = -2;
    i32 fires = 0;
    tl->OnLaneSelected.Add(
        [&](i32 lane)
        {
            fired = lane;
            ++fires;
        });

    // Gutter click on the SECOND lane row (ruler 24 + lane0 22 -> lane1 spans y 46..68).
    foundation::ui::MouseEventArgs down = Mouse(30.0f, 50.0f);
    tl->OnMouseDown(down);
    CHECK(down.Handled);
    CHECK(tl->SelectedLane() == 1);
    CHECK(fired == 1);
    CHECK(fires == 1);

    // Re-clicking the same lane does not re-fire.
    foundation::ui::MouseEventArgs again = Mouse(30.0f, 50.0f);
    tl->OnMouseDown(again);
    CHECK(fires == 1);

    // Picking a KEY selects its lane too (lane 0's key at t=1 -> x = 120 + 1*100 = 220, cy=35).
    foundation::ui::MouseEventArgs keyDown = Mouse(220.0f, 35.0f);
    tl->OnMouseDown(keyDown);
    foundation::ui::MouseEventArgs keyUp = keyDown;
    tl->OnMouseUp(keyUp);
    CHECK(tl->SelectedLane() == 0);
    CHECK(fired == 0);
    CHECK(fires == 2);

    // Programmatic set never fires; out-of-range clamps to -1; a shrunken lane set resets.
    tl->SetSelectedLane(1);
    CHECK(tl->SelectedLane() == 1);
    CHECK(fires == 2);
    tl->SetSelectedLane(9);
    CHECK(tl->SelectedLane() == -1);
    tl->SetSelectedLane(1);
    Array<DopesheetLane> one;
    {
        DopesheetLane a;
        a.label = String(u8"only");
        one.PushBack(Move(a));
    }
    tl->SetLanes(Move(one));
    CHECK(tl->SelectedLane() == -1);
}

TEST_CASE("timeline: OnViewChanged fires on zoom + scroll (the D1 shared-transform sync trigger)")
{
    auto tl = MakeRef<Timeline>(DefaultAllocator());
    tl->SetDuration(100.0f);
    tl->Measure(BoxConstraints::Tight(800.0f, 200.0f));
    tl->Layout(0.0f, 0.0f, 800.0f, 200.0f);

    int views = 0;
    tl->OnViewChanged.Add([&] { ++views; });

    tl->SetPixelsPerSecond(200.0f); // zoom in from the default 100
    CHECK(views >= 1);

    const int afterZoom = views;
    tl->SetScrollSeconds(5.0f); // duration 100, width 800 @ 200px/s -> 5s scroll is in range
    CHECK(tl->ScrollSeconds() == doctest::Approx(5.0f));
    CHECK(views > afterZoom);
}
