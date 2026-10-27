// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Profiler.GUI - the frame strip and the counters' plots on one frame axis (X7 of
// Specs/editor-gui.md): the axis fitting the source and following it until a zoom or a pan; a
// column a frame, or the frames sharing a pixel as one showing the longest; the budget's scale;
// a click choosing a frame, a drag a range, a double click fitting again; the name chosen drawn
// as its share of each frame; a counter's value each frame.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include <initializer_list>

import foundation.core;
import foundation.gui;
import foundation.profiler;
import foundation.profiler.gui;

#include "KnownSource.h"

using namespace foundation::core;
using namespace foundation::profiler;
using namespace foundation::profiler::gui;
using profiler_tests::KnownSource;
namespace fgui = foundation::gui;

namespace
{
    // A strip 528 by 64 and a counter plot under it, both a 128 gutter and a 400 plot, over 40
    // frames of 10 ms (frame 7 of 30, frame 20 of 50): frame n's column at 128 + 10 n.
    struct Bed
    {
        KnownSource source;
        RefPtr<fgui::SceneRoot> root = MakeRef<fgui::SceneRoot>(DefaultAllocator(), MakeRef<fgui::GUIContext>(DefaultAllocator()));
        RefPtr<ProfileFocus> focus = MakeRef<ProfileFocus>(DefaultAllocator(), DefaultAllocator());
        RefPtr<FrameAxis> axis = MakeRef<FrameAxis>(DefaultAllocator(), DefaultAllocator());
        RefPtr<FrameStrip> strip = MakeRef<FrameStrip>(DefaultAllocator());
        RefPtr<CounterPlot> plot = MakeRef<CounterPlot>(DefaultAllocator());
        NameId update = 0;
        u32 draws = 0;
        int picked = 0;

        Bed()
        {
            update = source.Name(u8"Update");
            draws = source.Counter(u8"Draw calls", CounterUnit::Count);
            for (u32 n = 0; n < 40; ++n)
            {
                FrameRecord& frame = source.AddFrame(n == 7 ? 30 : n == 20 ? 50 : 10);
                KnownSource::AddScope(frame, update, 0, 0, n == 20 ? 30 : 4);
                frame.counters.PushBack(CounterValue{draws, 100.0 + static_cast<f64>(n)});
            }
            root->SetSize(Float2{600.0f, 200.0f});
            root->EditLayoutStyle([](fgui::LayoutStyle& s)
            {
                s.Direction = fgui::FlexDirection::Column;
                s.AlignItems = fgui::Align::Start;
            });
            for (FrameAxisView* view : {static_cast<FrameAxisView*>(strip.Get()), static_cast<FrameAxisView*>(plot.Get())})
            {
                view->EditLayoutStyle([](fgui::LayoutStyle& s) { s.Width = fgui::LayoutLength::Px(528.0f); });
                root->AddChild(view);
                view->SetProfileFocus(focus);
                view->SetFrameAxis(axis);
            }
            strip->OnFramesPicked = [this]() { ++picked; };
            focus->SetSource(&source);
            (void)root->UpdateLayout();
        }
        fgui::EventDispatcher& D() { return *root->GetEventDispatcher(); }
        static Float2 At(f32 frame) { return Float2{128.0f + 10.0f * frame + 5.0f, 30.0f}; }
        void Click(Float2 at)
        {
            D().InjectMouseMove(at);
            D().InjectMouseDown(at, fgui::MouseButton::Left);
            D().InjectMouseUp(at, fgui::MouseButton::Left);
        }
        void Pause() { Click(Float2{590.0f, 190.0f}); }
    };
}

TEST_CASE("FrameAxis: it fits the source and follows it, until a zoom or a pan; kept over the frames")
{
    Bed bed;
    CHECK(bed.axis->Fits());
    CHECK(bed.axis->First() == 0.0);
    CHECK(bed.axis->Count() == 40.0);

    for (int i = 0; i < 10; ++i)
        bed.source.AddFrame(10);
    bed.focus->SourceGrew();
    CHECK(bed.axis->Count() == 50.0); // fitting: it follows

    bed.axis->ZoomAbout(25.0, 2.0);
    CHECK_FALSE(bed.axis->Fits());
    CHECK(bed.axis->Count() == 25.0);
    CHECK(bed.axis->First() == doctest::Approx(12.5)); // frame 25 stays halfway
    bed.source.AddFrame(10);
    bed.focus->SourceGrew();
    CHECK(bed.axis->Count() == 25.0); // not fitting: it stays

    bed.axis->ZoomAbout(25.0, 1000.0);
    CHECK(bed.axis->Count() == FrameAxis::kMinFrames);
    bed.axis->ZoomAbout(25.0, 0.0001);
    CHECK(bed.axis->Count() == 51.0); // never more than every frame
    bed.axis->PanBy(-100.0);
    CHECK(bed.axis->First() == 0.0); // kept over the frames
}

TEST_CASE("FrameStrip: a column a frame, or the frames sharing a pixel as one showing the longest")
{
    Bed bed;
    Array<FrameColumn> columns;
    bed.strip->GetColumns(columns);
    REQUIRE(columns.Size() == 40);
    CHECK(columns[7].first == 7u);
    CHECK(columns[7].x == doctest::Approx(70.0f));
    CHECK(columns[7].width == doctest::Approx(10.0f));

    // Two thousand frames over 400 pixels: five a column; the hitch shows in its column.
    KnownSource many;
    for (u32 n = 0; n < 2000; ++n)
        many.AddFrame(n == 1003 ? 40 : 10);
    bed.focus->SetSource(&many);
    bed.strip->GetColumns(columns);
    CHECK(columns.Size() == 400);
    CHECK(columns[200].first == 1000u);
    CHECK(columns[200].last == 1004u);
    Array<FrameColumnTimes> times;
    bed.strip->GetColumnTimes(columns.AsSpan(), times);
    CHECK(times[200].cpuMs == doctest::Approx(40.0f));
    CHECK(times[199].cpuMs == doctest::Approx(10.0f));
}

TEST_CASE("FrameStrip: the scale is twice the budget, or without one the longest shown rounded up")
{
    Bed bed;
    Array<FrameColumn> columns;
    bed.strip->GetColumns(columns);
    Array<FrameColumnTimes> times;
    bed.strip->GetColumnTimes(columns.AsSpan(), times);
    CHECK(bed.strip->ScaleMs(times.AsSpan()) == doctest::Approx(50.0f)); // the 50 ms frame, already round
    bed.strip->SetBudgetMs(16.0f);
    CHECK(bed.strip->ScaleMs(times.AsSpan()) == doctest::Approx(32.0f));
    bed.strip->SetBudgetMs(0.0f);
    times[20].cpuMs = 61.0f;
    CHECK(bed.strip->ScaleMs(times.AsSpan()) == doctest::Approx(100.0f));
}

TEST_CASE("FrameStrip: a click chooses a frame, a drag a range, a double click fits again")
{
    Bed bed;
    bed.Click(Bed::At(7));
    CHECK_FALSE(bed.focus->Frames().empty);
    CHECK(bed.focus->Frames().first == 7u);
    CHECK(bed.focus->Frames().last == 7u);
    CHECK(bed.picked == 1);

    bed.Pause();
    bed.D().InjectMouseMove(Bed::At(12));
    bed.D().InjectMouseDown(Bed::At(12), foundation::gui::MouseButton::Left);
    bed.D().InjectMouseMove(Bed::At(10));
    bed.D().InjectMouseMove(Bed::At(9));
    bed.D().InjectMouseUp(Bed::At(9), foundation::gui::MouseButton::Left);
    CHECK(bed.focus->Frames().first == 9u);
    CHECK(bed.focus->Frames().last == 12u);

    // The chosen frames' columns say so.
    Array<FrameColumn> columns;
    bed.strip->GetColumns(columns);
    Array<FrameColumnTimes> times;
    bed.strip->GetColumnTimes(columns.AsSpan(), times);
    CHECK(times[10].chosen);
    CHECK_FALSE(times[13].chosen);

    // The wheel zooms (and stops fitting); a double click fits again.
    bed.Pause();
    bed.D().InjectMouseWheel(Bed::At(20), Float2{0.0f, 1.0f});
    CHECK_FALSE(bed.axis->Fits());
    CHECK(bed.axis->Count() < 40.0);
    bed.Pause();
    bed.D().InjectMouseMove(Bed::At(5));
    bed.D().InjectMouseDown(Bed::At(5), foundation::gui::MouseButton::Left);
    bed.D().InjectMouseUp(Bed::At(5), foundation::gui::MouseButton::Left);
    bed.D().InjectMouseDown(Bed::At(5), foundation::gui::MouseButton::Left);
    bed.D().InjectMouseUp(Bed::At(5), foundation::gui::MouseButton::Left);
    CHECK(bed.axis->Fits());
    CHECK(bed.axis->Count() == 40.0);
}

TEST_CASE("FrameStrip: the name chosen is its share of each frame; the hover names the frame")
{
    Bed bed;
    Array<FrameColumn> columns;
    bed.strip->GetColumns(columns);
    Array<FrameColumnTimes> times;
    bed.strip->GetColumnTimes(columns.AsSpan(), times);
    CHECK(times[20].chosenMs == 0.0f);
    bed.focus->ChooseName(bed.update, false);
    bed.strip->GetColumnTimes(columns.AsSpan(), times);
    CHECK(times[20].chosenMs == doctest::Approx(30.0f));
    CHECK(times[3].chosenMs == doctest::Approx(4.0f));

    bed.D().InjectMouseMove(Bed::At(20));
    CHECK(bed.strip->GetTooltipText() == StringView(u8"Frame 20: 50.00 ms, Update 30.00 ms"));
}

TEST_CASE("CounterPlot: a counter's value each frame, the most of a column's; the hover names it")
{
    Bed bed;
    bed.plot->SetCounter(bed.draws);
    FrameColumn column;
    column.first = 3;
    column.last = 6;
    f64 value = 0.0;
    REQUIRE(bed.plot->ColumnValue(column, value));
    CHECK(value == 106.0);
    bed.plot->SetCounter(5); // no such counter: nothing set
    CHECK_FALSE(bed.plot->ColumnValue(column, value));

    bed.plot->SetCounter(bed.draws);
    const Float2 over{Bed::At(3).x, 64.0f + 13.0f};
    bed.D().InjectMouseMove(over);
    CHECK(bed.plot->GetTooltipText() == StringView(u8"Draw calls at frame 3: 103"));
    // A click on the plot chooses as the strip's does.
    bed.Click(over);
    CHECK(bed.focus->Frames().first == 3u);
}

TEST_CASE("Profiler.GUI's sheet reaches the parts every view names, a name's hues among them")
{
    Bed bed;
    RefPtr<ProfileTimeline> timeline = MakeRef<ProfileTimeline>(DefaultAllocator());
    RefPtr<ScopeTable> table = MakeRef<ScopeTable>(DefaultAllocator());
    RefPtr<ScopeDetail> detail = MakeRef<ScopeDetail>(DefaultAllocator());
    bed.root->AddChild(timeline.Get());
    bed.root->AddChild(table.Get());
    bed.root->AddChild(detail.Get());
    fgui::StyleManager styles;
    const String theme = Format(u8"{}\n{}", fgui::DefaultThemeCSS(), ProfilerSheetCSS()); // its tokens under it, as in the editor
    styles.SetUserAgentSheet(fgui::CSSParser::Parse(theme.AsView()));
    styles.ApplyTree(*bed.root.Get());
    CHECK(bed.strip->GetPartStyle(u8"on-time").HasBackground);
    CHECK(bed.strip->GetPartStyle(u8"far-over").HasBackground);
    CHECK(bed.plot->GetPartStyle(u8"bar").HasBackground);
    CHECK(timeline->GetPartStyle(u8"bar").HasColor);
    CHECK(timeline->GetPartStyle(u8"hatch").HasBackground);
    CHECK(table->GetPartStyle(u8"heading").HasBackground);
    CHECK(detail->GetPartStyle(u8"lit").HasBackground);
    // A name's hue: each of the eight, on every view.
    for (const char8_t* hue : {u8"hue-0", u8"hue-3", u8"hue-7"})
    {
        CHECK(timeline->GetPartStyle(StringView(hue)).HasBackground);
        CHECK(table->GetPartStyle(StringView(hue)).HasBackground);
        CHECK(detail->GetPartStyle(StringView(hue)).HasBackground);
    }
}
