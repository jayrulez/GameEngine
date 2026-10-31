// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Profiler.GUI - the in-game overlay. Covers the frame time graph plotting the newest frames on
// its scale; the statistics, top lists and counters showing the source's numbers; the panel
// sitting in its corner; and the overlay never taking the pointer from the game under it.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.vg;
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
    // Ten frames of 10 ms, then one of 30 ms (a hitch). Each has an Update scope, a forward pass
    // and two counters.
    void Fill(KnownSource& source)
    {
        const NameId update = source.Name(u8"Update");
        const NameId forward = source.Name(u8"forward");
        const u32 draws = source.Counter(u8"Draws", CounterUnit::Count);
        const u32 memory = source.Counter(u8"Buffer memory", CounterUnit::Bytes);
        for (u32 f = 0; f < 11; ++f)
        {
            FrameRecord& frame = source.AddFrame(f == 10 ? 30 : 10);
            KnownSource::AddScope(frame, update, 0, 0, 6);
            KnownSource::AddPass(frame, forward, 1, 5);
            frame.counters.PushBack(CounterValue{draws, 120.0});
            frame.counters.PushBack(CounterValue{memory, 2.0 * 1024 * 1024});
        }
    }

    // The overlay fills a 1280 by 720 root, over a known source.
    struct Bed
    {
        KnownSource source;
        RefPtr<fgui::SceneRoot> root = MakeRef<fgui::SceneRoot>(DefaultAllocator(), MakeRef<fgui::GUIContext>(DefaultAllocator()));
        RefPtr<ProfileFocus> focus = MakeRef<ProfileFocus>(DefaultAllocator(), DefaultAllocator());
        RefPtr<ProfilerOverlay> overlay = MakeRef<ProfilerOverlay>(DefaultAllocator());

        Bed()
        {
            Fill(source);
            root->SetSize(Float2{1280.0f, 720.0f});
            overlay->EditLayoutStyle([](fgui::LayoutStyle& s)
            {
                s.Width = fgui::LayoutLength::Percent(100.0f);
                s.Height = fgui::LayoutLength::Percent(100.0f);
            });
            root->AddChild(overlay.Get());
            overlay->SetProfileFocus(focus);
            focus->SetSource(&source);
            (void)root->UpdateLayout();
            overlay->Refresh();
        }
        usize Draw()
        {
            foundation::vg::VGContext canvas;
            fgui::DrawContext ctx{canvas};
            root->Draw(ctx);
            return canvas.GetBatch().VertexCount();
        }
    };
}

TEST_CASE("profiler overlay: the graph plots the newest frames on its scale, newest at the right edge")
{
    Bed bed;
    CHECK(bed.overlay->ScaleMs() == doctest::Approx(40.0f)); // 30 ms needs the 20 ms scale doubled once
    CHECK(ProfilerOverlay::ScaleFor(10.0f) == doctest::Approx(20.0f));
    CHECK(ProfilerOverlay::ScaleFor(100.0f) == doctest::Approx(160.0f));
    REQUIRE(bed.overlay->CpuPoints().Size() == 11);
    REQUIRE(bed.overlay->GpuPoints().Size() == 11);
    const f32 width = kProfilerOverlayPanelWidth - 2 * kProfilerOverlayPadding;
    const f32 step = width / static_cast<f32>(kProfilerOverlayGraphFrames - 1);
    CHECK(bed.overlay->CpuPoints()[10].x == doctest::Approx(width));
    CHECK(bed.overlay->CpuPoints()[0].x == doctest::Approx(width - 10.0f * step)); // a short history grows in from the right
    CHECK(bed.overlay->CpuPoints()[0].y == doctest::Approx(48.0f));                // 10 ms is a quarter of the way up the 64 pixels
    CHECK(bed.overlay->CpuPoints()[10].y == doctest::Approx(16.0f));               // 30 ms is three quarters of the way up
    CHECK(bed.overlay->GpuPoints()[0].y == doctest::Approx(64.0f * (1.0f - 4.0f / 40.0f)));
}

TEST_CASE("profiler overlay: the statistics, the top lists and the counters show the source's numbers")
{
    Bed bed;
    REQUIRE(bed.overlay->StatLines().Size() == 3);
    CHECK(bed.overlay->StatLines()[0].AsView().StartsWith(u8"CPU  30.00 ms"));
    CHECK(bed.overlay->StatLines()[1].AsView().StartsWith(u8"GPU  4.00 ms"));
    CHECK(bed.overlay->StatLines()[2].AsView().StartsWith(u8"85 fps over 11 frames")); // 1000 / (130 / 11)
    REQUIRE(bed.overlay->Scopes().Size() == 1);
    CHECK(bed.overlay->Scopes()[0].name == StringView(u8"Update"));
    CHECK(bed.overlay->Scopes()[0].msPerFrame == doctest::Approx(6.0f));
    CHECK(bed.overlay->Scopes()[0].bar == doctest::Approx(1.0f));
    REQUIRE(bed.overlay->Passes().Size() == 1);
    CHECK(bed.overlay->Passes()[0].name == StringView(u8"forward"));
    CHECK(bed.overlay->Passes()[0].msPerFrame == doctest::Approx(4.0f));
    REQUIRE(bed.overlay->Counters().Size() == 2);
    CHECK(bed.overlay->Counters()[0].name == StringView(u8"Draws"));
    CHECK(bed.overlay->Counters()[0].value == StringView(u8"120"));
    CHECK(bed.overlay->Counters()[1].value == StringView(u8"2.0 MiB")); // a held amount shows the newest value
    CHECK(bed.Draw() > 0);

    // After a new frame, Refresh reads the source again.
    FrameRecord& frame = bed.source.AddFrame(50);
    (void)frame;
    bed.overlay->Refresh();
    CHECK(bed.overlay->StatLines()[0].AsView().StartsWith(u8"CPU  50.00 ms"));
    CHECK(bed.overlay->ScaleMs() == doctest::Approx(80.0f));
}

TEST_CASE("profiler overlay: the panel sits in its corner, and the pointer passes through to the game")
{
    Bed bed;
    CHECK_FALSE(bed.overlay->IsHitTestVisible());
    fgui::Rect panel = bed.overlay->Panel();
    CHECK(panel.width == doctest::Approx(f32{kProfilerOverlayPanelWidth}));
    CHECK(panel.x == doctest::Approx(1280.0f - kProfilerOverlayMargin - kProfilerOverlayPanelWidth));
    CHECK(panel.y == doctest::Approx(f32{kProfilerOverlayMargin}));
    CHECK(panel.height == doctest::Approx(bed.overlay->PanelHeight()));

    bed.overlay->SetCorner(OverlayCorner::BottomLeft);
    panel = bed.overlay->Panel();
    CHECK(panel.x == doctest::Approx(f32{kProfilerOverlayMargin}));
    CHECK(panel.y + panel.height == doctest::Approx(720.0f - kProfilerOverlayMargin));

    // A widget under the overlay still receives a click inside the panel.
    RefPtr<fgui::UIWidget> under = MakeRef<fgui::UIWidget>(DefaultAllocator());
    under->EditLayoutStyle([](fgui::LayoutStyle& s)
    {
        s.Placement = fgui::Position::Absolute;
        s.Left = fgui::LayoutLength::Px(0.0f);
        s.Top = fgui::LayoutLength::Px(0.0f);
        s.Width = fgui::LayoutLength::Px(1280.0f);
        s.Height = fgui::LayoutLength::Px(720.0f);
    });
    bed.root->AddChildAt(under.Get(), 0);
    (void)bed.root->UpdateLayout();
    const Float2 inside{panel.x + 20.0f, panel.y + 20.0f};
    CHECK(bed.root->GetEventDispatcher()->NodeAt(inside) == under.Get());
}
