// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The editor panel's views (Specs/profiler.md, P5), on a source of known frames: the frame strip
// fitted to every frame and selecting by click and drag; the timeline laying a frame's lanes and
// nesting out on its time axis, zooming about the pointer and panning; the scope table's totals,
// its sorting, and a row selecting its bars in the timeline.
#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.vg;
import foundation.ui;
import foundation.profiler;
import foundation.profiler.ui;

#include "KnownSource.h"

using namespace foundation::core;
using namespace foundation::profiler;
using namespace foundation::profiler::ui;
namespace vg = foundation::vg;
namespace fui = foundation::ui;
using profiler_tests::KnownSource;

namespace
{
    struct Stage
    {
        fui::UIContext context{DefaultAllocator()};
        RefPtr<fui::RootView> root = MakeRef<fui::RootView>(DefaultAllocator());

        Stage(fui::View& view, Float2 size)
        {
            root->ViewportSize = size;
            context.AddRootView(root.Get());
            fui::LayoutStyle fill;
            fill.Width = fui::SizeSpec::Match();
            fill.Height = fui::SizeSpec::Match();
            root->AddView(&view, fill);
            context.BeginFrame(0.016f);
            root->Measure(fui::BoxConstraints::Tight(size.x, size.y));
            root->Layout(0, 0, size.x, size.y);
        }

        ~Stage() { context.RemoveRootView(root.Get()); }

        usize Draw()
        {
            vg::VGContext vgContext;
            fui::UIDrawContext draw(vgContext, 1.0f, nullptr);
            root->OnDraw(draw);
            return vgContext.GetBatch().VertexCount();
        }
    };

    fui::MouseEventArgs Mouse(f32 x, f32 y, i32 clicks = 1)
    {
        fui::MouseEventArgs e;
        e.X = x;
        e.Y = y;
        e.Button = fui::MouseButton::Left;
        e.ClickCount = clicks;
        return e;
    }

    // One 16 ms frame: Main's Update (0..10) holds Physics (1..5) and Audio (6..8) at depth 1;
    // Worker 0 runs a Job (2..12); the GPU runs shadows (3..7) and forward (8..15).
    void OneFrame(KnownSource& source)
    {
        const u32 worker = source.Thread(u8"Worker 0");
        const NameId update = source.Name(u8"Update");
        const NameId physics = source.Name(u8"Physics");
        const NameId audio = source.Name(u8"Audio");
        const NameId job = source.Name(u8"Job");
        const NameId shadows = source.Name(u8"shadows");
        const NameId forward = source.Name(u8"forward");
        FrameRecord& frame = source.AddFrame(16);
        KnownSource::AddScope(frame, physics, 1, 1, 5);
        KnownSource::AddScope(frame, audio, 1, 6, 8);
        KnownSource::AddScope(frame, update, 0, 0, 10);
        KnownSource::AddScopeOn(frame, worker, job, 0, 2, 12);
        KnownSource::AddPass(frame, shadows, 3, 7);
        KnownSource::AddPass(frame, forward, 8, 15);
    }
}

TEST_CASE("profiler panel views: the frame strip fits every frame and selects by click and drag")
{
    KnownSource source;
    for (u32 f = 0; f < 11; ++f)
    {
        source.AddFrame(10);
    }
    auto strip = MakeRef<FrameTimeGraph>(DefaultAllocator(), DefaultAllocator());
    strip->SetFitToSource(true);
    strip->SetInteractive(true);
    strip->SetSource(&source);
    Stage stage(*strip, Float2{300, FrameTimeGraph::kHeight});
    strip->Refresh();
    REQUIRE(strip->CpuPoints().Size() == 11); // all of them, however many FrameCount says
    CHECK(strip->CpuPoints()[0].x == doctest::Approx(0.0f));
    CHECK(strip->CpuPoints()[10].x == doctest::Approx(300.0f));
    CHECK(strip->FrameAt(300.0f).first == 10u);
    CHECK(strip->FrameAt(0.0f).first == 0u);
    CHECK(strip->FrameAt(-50.0f).first == 0u); // clamped to the oldest

    Array<FrameRange> heard(DefaultAllocator());
    strip->onSelectionChanged = [&heard](FrameRange range) { heard.PushBack(range); };
    fui::MouseEventArgs down = Mouse(300.0f, 10.0f);
    strip->OnMouseDown(down);
    CHECK(down.Handled);
    REQUIRE(heard.Size() == 1);
    CHECK(heard[0].first == 10u);
    CHECK(heard[0].last == 10u);
    fui::MouseEventArgs move = Mouse(241.0f, 10.0f); // two frames back
    strip->OnMouseMove(move);
    fui::MouseEventArgs up = Mouse(241.0f, 10.0f);
    strip->OnMouseUp(up);
    REQUIRE(heard.Size() == 2);
    CHECK(strip->Selection().first == 8u);
    CHECK(strip->Selection().last == 10u);
    CHECK(stage.Draw() > 0);

    fui::MouseEventArgs after = Mouse(0.0f, 10.0f); // not dragging: a move selects nothing
    strip->OnMouseMove(after);
    CHECK(heard.Size() == 2);
}

TEST_CASE("profiler panel views: the timeline lays a frame's lanes and nesting out on its time axis")
{
    KnownSource source;
    OneFrame(source);
    auto timeline = MakeRef<Timeline>(DefaultAllocator(), DefaultAllocator());
    timeline->SetSource(&source);
    Stage stage(*timeline, Float2{Timeline::kHeaderWidth + 600.0f, 240.0f});
    timeline->SetFrames(LastFrames(source, 1));

    // Main, then Worker 0, then the GPU; Main two depths deep, the others one.
    REQUIRE(timeline->Lanes().Size() == 3);
    CHECK(timeline->Lanes()[0].name == StringView(u8"Main"));
    CHECK(timeline->Lanes()[0].depths == 2u);
    CHECK(timeline->Lanes()[1].name == StringView(u8"Worker 0"));
    CHECK(timeline->Lanes()[2].gpu);
    CHECK(timeline->Lanes()[1].y > timeline->Lanes()[0].y + timeline->Lanes()[0].height);

    // The whole frame in view: 16 ms across 600 units.
    CHECK(timeline->XAt(timeline->ViewBegin()) == doctest::Approx(f32{Timeline::kHeaderWidth}));
    CHECK(timeline->ViewEnd() - timeline->ViewBegin() == doctest::Approx(16.0));
    REQUIRE(timeline->Bars().Size() == 6);
    const Timeline::Bar* update = nullptr;
    const Timeline::Bar* physics = nullptr;
    for (const Timeline::Bar& bar : timeline->Bars())
    {
        if (source.NameOf(bar.name) == StringView(u8"Update"))
        {
            update = &bar;
        }
        if (source.NameOf(bar.name) == StringView(u8"Physics"))
        {
            physics = &bar;
        }
    }
    REQUIRE(update != nullptr);
    REQUIRE(physics != nullptr);
    CHECK(update->rect.x == doctest::Approx(f32{Timeline::kHeaderWidth}));
    CHECK(update->rect.width == doctest::Approx(600.0f * 10.0f / 16.0f));
    CHECK(physics->depth == 1u);
    CHECK(physics->rect.y == doctest::Approx(update->rect.y + Timeline::kRowHeight)); // nested under it
    CHECK(physics->rect.x == doctest::Approx(Timeline::kHeaderWidth + 600.0f / 16.0f));
    CHECK(timeline->BarAt(Float2{physics->rect.x + 2, physics->rect.y + 2}) == physics);
    REQUIRE(timeline->FrameEdges().Size() == 1);
    CHECK(stage.Draw() > 0);
}

// Every draw recomputes the bars; the bar under the pointer must survive it, or the hover box
// (its name and time) never draws.
TEST_CASE("profiler panel views: a draw keeps the timeline's hovered bar")
{
    KnownSource source;
    OneFrame(source);
    auto timeline = MakeRef<Timeline>(DefaultAllocator(), DefaultAllocator());
    timeline->SetSource(&source);
    Stage stage(*timeline, Float2{Timeline::kHeaderWidth + 600.0f, 240.0f});
    timeline->SetFrames(LastFrames(source, 1));
    REQUIRE(!timeline->Bars().IsEmpty());
    const Timeline::Bar first = timeline->Bars()[0];

    fui::MouseEventArgs over = Mouse(first.rect.x + 2.0f, first.rect.y + 2.0f);
    timeline->OnMouseMove(over);
    REQUIRE(timeline->Hovered() != nullptr);
    const usize plain = stage.Draw();
    REQUIRE(timeline->Hovered() != nullptr); // still hovered after the draw's recompute
    CHECK(timeline->Hovered()->name == first.name);

    // A zoom with the pointer still: the hovered bar is whatever is under the pointer now.
    timeline->ZoomAt(first.rect.x + 200.0f, 4.0f);
    (void)stage.Draw();
    CHECK(timeline->Hovered() == timeline->BarAt(Float2{first.rect.x + 2.0f, first.rect.y + 2.0f}));

    timeline->OnMouseLeave();
    CHECK(stage.Draw() < plain); // gone: the box no longer drawn
    CHECK(timeline->Hovered() == nullptr);
}

TEST_CASE("profiler panel views: zoom keeps the time under the pointer; a drag pans")
{
    KnownSource source;
    OneFrame(source);
    auto timeline = MakeRef<Timeline>(DefaultAllocator(), DefaultAllocator());
    timeline->SetSource(&source);
    Stage stage(*timeline, Float2{Timeline::kHeaderWidth + 600.0f, 240.0f});
    timeline->SetFrames(LastFrames(source, 1));

    const f32 x = Timeline::kHeaderWidth + 150.0f;
    const f64 under = timeline->TickAt(x);
    timeline->ZoomAt(x, 2.0f);
    CHECK(timeline->TickAt(x) == doctest::Approx(under));
    CHECK(timeline->ViewEnd() - timeline->ViewBegin() == doctest::Approx(8.0));
    timeline->ZoomAt(x, 0.5f);
    CHECK(timeline->ViewEnd() - timeline->ViewBegin() == doctest::Approx(16.0));
    CHECK(timeline->TickAt(x) == doctest::Approx(under));

    // A wheel notch zooms in about the pointer too.
    fui::MouseWheelEventArgs wheel;
    wheel.X = x;
    wheel.DeltaY = 1.0f;
    timeline->OnMouseWheel(wheel);
    CHECK(timeline->TickAt(x) == doctest::Approx(under));
    CHECK(timeline->ViewEnd() - timeline->ViewBegin() < 16.0);

    // A drag 60 units right shows earlier time; a double click fits the frame again.
    const f64 begin = timeline->ViewBegin();
    const f64 perUnit = (timeline->ViewEnd() - timeline->ViewBegin()) / 600.0;
    fui::MouseEventArgs down = Mouse(400.0f, 50.0f);
    timeline->OnMouseDown(down);
    fui::MouseEventArgs move = Mouse(460.0f, 50.0f);
    timeline->OnMouseMove(move);
    fui::MouseEventArgs up = Mouse(460.0f, 50.0f);
    timeline->OnMouseUp(up);
    CHECK(timeline->ViewBegin() == doctest::Approx(begin - 60.0 * perUnit));
    fui::MouseEventArgs twice = Mouse(400.0f, 50.0f, 2);
    timeline->OnMouseDown(twice);
    CHECK(timeline->ViewEnd() - timeline->ViewBegin() == doctest::Approx(16.0));
}

TEST_CASE("profiler panel views: the scope table totals, sorts, and a row selects its bars")
{
    KnownSource source;
    OneFrame(source);
    auto table = MakeRef<ScopeTable>(DefaultAllocator(), DefaultAllocator());
    auto timeline = MakeRef<Timeline>(DefaultAllocator(), DefaultAllocator());
    table->SetSource(&source);
    timeline->SetSource(&source);
    Stage tableStage(*table, Float2{420, 240});
    Stage timelineStage(*timeline, Float2{Timeline::kHeaderWidth + 600.0f, 240.0f});
    const FrameRange frame = LastFrames(source, 1);
    table->SetFrames(frame);
    timeline->SetFrames(frame);

    // By own time, largest first: Job 10, forward 7, Physics 4, shadows 4, Update 4 (10 less 6), Audio 2.
    REQUIRE(table->Rows().Size() == 6);
    CHECK(table->SortColumn() == ScopeTable::Column::Self);
    CHECK(table->Rows()[0].label == StringView(u8"Job"));
    CHECK(table->Rows()[0].selfMs == doctest::Approx(10.0));
    CHECK(table->Rows()[1].label == StringView(u8"forward"));
    CHECK(table->Rows()[1].gpu);
    CHECK(table->Rows()[5].label == StringView(u8"Audio"));
    for (const ScopeTable::Row& row : table->Rows())
    {
        if (row.label == StringView(u8"Update"))
        {
            CHECK(row.totalMs == doctest::Approx(10.0));
            CHECK(row.selfMs == doctest::Approx(4.0));
            CHECK(row.count == 1u);
        }
    }

    // A heading click sorts by its column: Name A to Z, then Z to A.
    const f32 numbers = 420.0f - 3 * ScopeTable::kNumberWidth;
    fui::MouseEventArgs name = Mouse(10.0f, 5.0f);
    table->OnMouseDown(name);
    CHECK(table->SortColumn() == ScopeTable::Column::Name);
    CHECK(table->Rows()[0].label == StringView(u8"Audio"));
    fui::MouseEventArgs again = Mouse(10.0f, 5.0f);
    table->OnMouseDown(again);
    CHECK(table->Rows()[0].label == StringView(u8"shadows"));
    fui::MouseEventArgs total = Mouse(numbers + 5.0f, 5.0f);
    table->OnMouseDown(total);
    CHECK(table->SortColumn() == ScopeTable::Column::Total);
    CHECK(table->Rows()[0].totalMs >= table->Rows()[1].totalMs);

    // A row click selects its name; the timeline highlights that name's bars and no other.
    table->onSelect = [&timeline](const ScopeTable::Row* row)
    {
        if (row != nullptr)
        {
            timeline->SetHighlight(row->name, row->gpu);
        }
        else
        {
            timeline->ClearHighlight();
        }
    };
    i32 physicsRow = -1;
    for (usize i = 0; i < table->Rows().Size(); ++i)
    {
        if (table->Rows()[i].label == StringView(u8"Physics"))
        {
            physicsRow = static_cast<i32>(i);
        }
    }
    REQUIRE(physicsRow >= 0);
    fui::MouseEventArgs row = Mouse(30.0f, ScopeTable::kRowHeight * (static_cast<f32>(physicsRow) + 1.5f));
    table->OnMouseDown(row);
    CHECK(table->SelectedRow() == physicsRow);
    u32 highlighted = 0;
    for (const Timeline::Bar& bar : timeline->Bars())
    {
        if (timeline->IsHighlighted(bar))
        {
            ++highlighted;
            CHECK(source.NameOf(bar.name) == StringView(u8"Physics"));
        }
    }
    CHECK(highlighted == 1u);
    // The selection follows its name through a re-sort.
    table->SortBy(ScopeTable::Column::Count, true);
    REQUIRE(table->SelectedRow() >= 0);
    CHECK(table->Rows()[static_cast<usize>(table->SelectedRow())].label == StringView(u8"Physics"));
    CHECK(tableStage.Draw() > 0);
    CHECK(timelineStage.Draw() > 0);
}
