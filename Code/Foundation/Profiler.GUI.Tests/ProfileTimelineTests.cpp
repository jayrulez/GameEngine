// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Profiler.GUI - the timeline (X7 of Specs/editor-gui.md): the frames chosen as lanes (each thread,
// then the GPU) with their scopes by depth, fitted when the frames change; scopes too narrow to
// draw gathered into one block with their count; a click choosing a scope's name (on nothing,
// none), the hover naming it; a double click zooming to a scope or fitting again; the wheel
// zooming about the pointer, Shift and the wheel scrolling the lanes, a drag panning; a drag on
// the ruler marking a span, a click there letting it go; a lane folded to one row.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

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
    // A timeline 728 by 300 (names 128, the plot 600), frame 1 chosen: ticks 1010 to 1020 shown as
    // 1009.9 to 1020.1, a tick 600 / 10.2 pixels. Lanes: Main at 42 (two rows), Worker at 84,
    // GPU at 108.
    struct Bed
    {
        KnownSource source;
        RefPtr<fgui::SceneRoot> root = MakeRef<fgui::SceneRoot>(DefaultAllocator(), MakeRef<fgui::GUIContext>(DefaultAllocator()));
        RefPtr<ProfileFocus> focus = MakeRef<ProfileFocus>(DefaultAllocator(), DefaultAllocator());
        RefPtr<ProfileTimeline> timeline = MakeRef<ProfileTimeline>(DefaultAllocator());
        NameId update = 0, physics = 0, render = 0, job = 0, shadows = 0, lighting = 0, tiny = 0;

        Bed()
        {
            update = source.Name(u8"Update");
            physics = source.Name(u8"Physics");
            render = source.Name(u8"Render");
            job = source.Name(u8"Job");
            shadows = source.Name(u8"Shadows");
            lighting = source.Name(u8"Lighting");
            tiny = source.Name(u8"Tiny");
            const u32 worker = source.Thread(u8"Worker");
            FrameRecord& f0 = source.AddFrame(10);
            KnownSource::AddScope(f0, update, 0, 0, 8);
            FrameRecord& f1 = source.AddFrame(10);
            KnownSource::AddScope(f1, physics, 1, 2, 6);
            KnownSource::AddScope(f1, render, 1, 6, 8);
            KnownSource::AddScope(f1, update, 0, 1, 9);
            KnownSource::AddScopeOn(f1, worker, job, 0, 2, 7);
            KnownSource::AddPass(f1, shadows, 3, 5);
            KnownSource::AddPass(f1, lighting, 5, 9);
            // Frame 2, a second long: thirty scopes of a tick under one, too narrow to draw apart.
            FrameRecord& f2 = source.AddFrame(1000);
            for (u64 i = 0; i < 30; ++i)
                KnownSource::AddScope(f2, tiny, 1, 100 + i, 101 + i);
            KnownSource::AddScope(f2, update, 0, 0, 900);

            root->SetSize(Float2{800.0f, 400.0f});
            root->EditLayoutStyle([](fgui::LayoutStyle& s) { s.AlignItems = fgui::Align::Start; });
            timeline->EditLayoutStyle([](fgui::LayoutStyle& s)
            {
                s.Width = fgui::LayoutLength::Px(728.0f);
                s.Height = fgui::LayoutLength::Px(300.0f);
            });
            root->AddChild(timeline.Get());
            timeline->SetProfileFocus(focus);
            focus->SetSource(&source);
            focus->SetFrame(1);
            (void)root->UpdateLayout();
            timeline->FitView(); // laid out now: the frames fill the plot
        }
        fgui::EventDispatcher& D() { return *root->GetEventDispatcher(); }
        static f32 X(f64 tick) { return 128.0f + static_cast<f32>((tick - 1009.9) / 10.2 * 600.0); }
        void Click(Float2 at, u32 modifiers = 0)
        {
            D().InjectMouseMove(at, modifiers);
            D().InjectMouseDown(at, fgui::MouseButton::Left, modifiers);
            D().InjectMouseUp(at, fgui::MouseButton::Left, modifiers);
        }
        void DoubleClick(Float2 at)
        {
            Click(at);
            D().InjectMouseDown(at, fgui::MouseButton::Left);
            D().InjectMouseUp(at, fgui::MouseButton::Left);
        }
        void Drag(Float2 from, Float2 to)
        {
            D().InjectMouseMove(from);
            D().InjectMouseDown(from, fgui::MouseButton::Left);
            D().InjectMouseMove(Float2{(from.x + to.x) * 0.5f, (from.y + to.y) * 0.5f});
            D().InjectMouseMove(to);
            D().InjectMouseUp(to, fgui::MouseButton::Left);
        }
        void Pause() { Click(Float2{790.0f, 390.0f}); }
        [[nodiscard]] const ProfileTimeline::Bar* BarOf(NameId name, bool gpu = false) const
        {
            for (const ProfileTimeline::Bar& bar : timeline->Bars())
                if (bar.name == name && bar.gpu == gpu && bar.merged == 0)
                    return &bar;
            return nullptr;
        }
    };
}

TEST_CASE("ProfileTimeline: a lane per thread, then the GPU's, the scopes by depth, fitted to the frames")
{
    Bed bed;
    CHECK(bed.timeline->ViewBegin() == doctest::Approx(1009.9));
    CHECK(bed.timeline->ViewEnd() == doctest::Approx(1020.1));
    const Span<const ProfileTimeline::Lane> lanes = bed.timeline->Lanes();
    REQUIRE(lanes.Size() == 3);
    CHECK(lanes[0].name == String(u8"Main"));
    CHECK(lanes[0].depths == 2);
    CHECK(lanes[0].y == doctest::Approx(42.0f));
    CHECK(lanes[1].name == String(u8"Worker"));
    CHECK(lanes[1].y == doctest::Approx(84.0f));
    CHECK(lanes[2].gpu);
    CHECK(lanes[2].y == doctest::Approx(108.0f));

    const ProfileTimeline::Bar* physics = bed.BarOf(bed.physics);
    REQUIRE(physics != nullptr);
    CHECK(physics->lane == 0);
    CHECK(physics->depth == 1);
    CHECK(physics->rect.x == doctest::Approx(Bed::X(1012.0)).epsilon(0.001));
    CHECK(physics->rect.y == doctest::Approx(61.0f));
    const ProfileTimeline::Bar* shadows = bed.BarOf(bed.shadows, true);
    REQUIRE(shadows != nullptr);
    CHECK(shadows->lane == 2);
    CHECK(bed.BarOf(bed.tiny) == nullptr); // frame 2's: not chosen
}

TEST_CASE("ProfileTimeline: scopes too narrow to draw gather into one block with their count")
{
    Bed bed;
    bed.focus->SetFrame(2);
    bool found = false;
    for (const ProfileTimeline::Bar& bar : bed.timeline->Bars())
        if (bar.merged > 0)
        {
            CHECK(bar.merged == 30);
            CHECK(bar.depth == 1);
            found = true;
        }
    CHECK(found);
    // Zoomed in, they are scopes of their own again.
    const ProfileTimeline::Bar* update = bed.BarOf(bed.update);
    REQUIRE(update != nullptr);
    bed.timeline->ZoomTo(1020.0 + 100.0, 1020.0 + 130.0); // frame 2 starts at 1020
    usize tiny = 0;
    for (const ProfileTimeline::Bar& bar : bed.timeline->Bars())
        if (bar.name == bed.tiny && bar.merged == 0)
            ++tiny;
    CHECK(tiny == 30);
}

TEST_CASE("ProfileTimeline: a click chooses a scope's name, on nothing none; the hover names it")
{
    Bed bed;
    bed.Click(Float2{Bed::X(1014.0), 69.0f}); // Physics
    CHECK(bed.focus->Name().Is(bed.physics, false));
    CHECK(bed.timeline->IsChosen(*bed.BarOf(bed.physics)));
    CHECK_FALSE(bed.timeline->IsChosen(*bed.BarOf(bed.update)));
    bed.Pause();
    bed.Click(Float2{Bed::X(1014.0), 117.0f}); // the GPU's Shadows
    CHECK(bed.focus->Name().Is(bed.shadows, true));
    bed.Pause();
    bed.Click(Float2{400.0f, 250.0f}); // under the lanes
    CHECK_FALSE(bed.focus->Name().any);

    bed.D().InjectMouseMove(Float2{Bed::X(1015.0), 51.0f});
    REQUIRE(bed.timeline->Hovered() != nullptr);
    CHECK(bed.timeline->Hovered()->name == bed.update);
    CHECK(bed.timeline->GetTooltipText() == StringView(u8"Update  8.000 ms"));
}

TEST_CASE("ProfileTimeline: a double click zooms to a scope, on nothing fits; Fit in the corner fits")
{
    Bed bed;
    bed.DoubleClick(Float2{Bed::X(1014.0), 69.0f}); // Physics, 1012 to 1016
    CHECK(bed.timeline->ViewBegin() == doctest::Approx(1011.96));
    CHECK(bed.timeline->ViewEnd() == doctest::Approx(1016.04));
    bed.Pause();
    bed.DoubleClick(Float2{400.0f, 250.0f});
    CHECK(bed.timeline->ViewBegin() == doctest::Approx(1009.9));
    bed.timeline->ZoomTo(1012.0, 1013.0);
    bed.Pause();
    bed.Click(Float2{60.0f, 18.0f}); // the corner
    CHECK(bed.timeline->ViewEnd() == doctest::Approx(1020.1));
}

TEST_CASE("ProfileTimeline: the wheel zooms about the pointer, Shift and the wheel scroll, a drag pans")
{
    Bed bed;
    const f32 x = Bed::X(1015.0);
    bed.D().InjectMouseWheel(Float2{x, 51.0f}, Float2{0.0f, 1.0f});
    CHECK(bed.timeline->ViewEnd() - bed.timeline->ViewBegin() == doctest::Approx(10.2 / 1.25));
    CHECK(bed.timeline->TickAt(x) == doctest::Approx(1015.0)); // the time under the pointer stays

    const f64 before = bed.timeline->ViewBegin();
    bed.Drag(Float2{400.0f, 51.0f}, Float2{460.0f, 51.0f}); // right: earlier time
    CHECK(bed.timeline->ViewBegin() < before);
    CHECK_FALSE(bed.focus->Name().any); // a drag chooses nothing

    // Not enough lanes to scroll in 300 pixels: the scroll stays at the top.
    bed.D().InjectMouseWheel(Float2{x, 51.0f}, Float2{0.0f, -1.0f}, fgui::KeyModShift);
    CHECK(bed.timeline->LaneScroll() == 0.0f);
}

TEST_CASE("ProfileTimeline: a drag on the ruler marks a span, a click there lets it go")
{
    Bed bed;
    bed.Drag(Float2{Bed::X(1012.0), 10.0f}, Float2{Bed::X(1016.0), 10.0f});
    REQUIRE(bed.focus->Span().any);
    CHECK(bed.focus->Span().beginTick == 1012u);
    CHECK(bed.focus->Span().endTick == 1016u);
    bed.Pause();
    bed.Click(Float2{Bed::X(1018.0), 10.0f});
    CHECK_FALSE(bed.focus->Span().any);
}

TEST_CASE("ProfileTimeline: a click on a lane's arrow folds it to one row of its top scopes")
{
    Bed bed;
    bed.Click(Float2{8.0f, 51.0f}); // Main's arrow
    CHECK(bed.timeline->IsFolded(0));
    const Span<const ProfileTimeline::Lane> lanes = bed.timeline->Lanes();
    CHECK(lanes[0].folded);
    CHECK(lanes[0].height == doctest::Approx(ProfileTimeline::kRowHeight));
    CHECK(lanes[1].y == doctest::Approx(66.0f));
    CHECK(bed.BarOf(bed.physics) == nullptr);
    CHECK(bed.BarOf(bed.update) != nullptr);
    bed.Pause();
    bed.Click(Float2{8.0f, 51.0f});
    CHECK_FALSE(bed.timeline->IsFolded(0));
}
