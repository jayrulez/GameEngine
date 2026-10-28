// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The profiler's views (Specs/profiler.md, P4), laid out in a root and drawn into a VG context
// headless (no fonts, so the shapes alone): each shows the numbers of a source of known frames,
// and the overlay stacks them on a panel in its corner.
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
    // `view` alone in a root of `size`, laid out; answers the vertices a draw of it makes.
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

    // Ten frames of 10 ms, then one of 30 (a hitch); each with an Update scope and a forward pass.
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
}

TEST_CASE("profiler views: the frame time graph plots the newest frames on its scale")
{
    KnownSource source;
    Fill(source);
    auto graph = MakeRef<FrameTimeGraph>(DefaultAllocator(), DefaultAllocator());
    Stage stage(*graph, Float2{300, FrameTimeGraph::kHeight});
    CHECK(stage.Draw() > 0); // its plot background, even with no source
    CHECK(graph->CpuPoints().IsEmpty());

    graph->SetFrameCount(11);
    graph->SetSource(&source);
    REQUIRE(graph->CpuPoints().Size() == 11);
    REQUIRE(graph->GpuPoints().Size() == 11);
    CHECK(graph->ScaleMs() == doctest::Approx(40.0f)); // 30 ms needs 20 doubled
    CHECK(FrameTimeGraph::ScaleFor(10.0f) == doctest::Approx(20.0f));
    CHECK(FrameTimeGraph::ScaleFor(100.0f) == doctest::Approx(160.0f));
    // Newest at the right edge, oldest at the left; 10 ms a quarter up, 30 three quarters up.
    CHECK(graph->CpuPoints()[10].x == doctest::Approx(300.0f));
    CHECK(graph->CpuPoints()[0].x == doctest::Approx(0.0f));
    CHECK(graph->CpuPoints()[0].y == doctest::Approx(48.0f));
    CHECK(graph->CpuPoints()[10].y == doctest::Approx(16.0f));
    CHECK(graph->GpuPoints()[0].y == doctest::Approx(64.0f * (1.0f - 4.0f / 40.0f)));

    // A shorter history grows in from the right rather than stretching.
    graph->SetFrameCount(21);
    CHECK(graph->CpuPoints()[0].x == doctest::Approx(150.0f));
    CHECK(stage.Draw() > 0);
}

TEST_CASE("profiler views: statistics, top lists and counters show the source's numbers")
{
    KnownSource source;
    Fill(source);

    auto stats = MakeRef<FrameStats>(DefaultAllocator(), DefaultAllocator());
    stats->SetFrameCount(11);
    stats->SetSource(&source);
    CHECK(stats->NewestCpuMs() == doctest::Approx(30.0f));
    CHECK(stats->NewestGpuMs() == doctest::Approx(4.0f));
    CHECK(stats->Stats().cpu.median == doctest::Approx(10.0f));
    CHECK(stats->Stats().cpu.max == doctest::Approx(30.0f));
    REQUIRE(stats->Lines().Size() == 3);
    CHECK(stats->Lines()[0].AsView().StartsWith(u8"CPU  30.00 ms"));
    CHECK(stats->Lines()[2].AsView().StartsWith(u8"85 fps")); // 1000 / (130 / 11)

    auto scopes = MakeRef<TopList>(DefaultAllocator(), DefaultAllocator(), TopLane::Scopes, 4);
    scopes->SetSource(&source);
    REQUIRE(scopes->Rows().Size() == 1);
    CHECK(scopes->Rows()[0].name == StringView(u8"Update"));
    CHECK(scopes->Rows()[0].msPerFrame == doctest::Approx(6.0f));
    CHECK(scopes->Rows()[0].bar == doctest::Approx(1.0f));
    auto passes = MakeRef<TopList>(DefaultAllocator(), DefaultAllocator(), TopLane::GpuPasses, 4);
    passes->SetSource(&source);
    REQUIRE(passes->Rows().Size() == 1);
    CHECK(passes->Rows()[0].name == StringView(u8"forward"));
    CHECK(passes->Rows()[0].msPerFrame == doctest::Approx(4.0f));
    Stage stage(*passes, Float2{300, 200});
    CHECK(stage.Draw() > 0); // the bar

    auto counters = MakeRef<CounterStrip>(DefaultAllocator(), DefaultAllocator());
    counters->SetSource(&source);
    REQUIRE(counters->Rows().Size() == 2);
    CHECK(counters->Rows()[0].name == StringView(u8"Draws"));
    CHECK(counters->Rows()[0].value == StringView(u8"120"));
    CHECK(counters->Rows()[1].value == StringView(u8"2.0 MiB"));
}

TEST_CASE("profiler views: the overlay stacks the views on a panel in its corner")
{
    KnownSource source;
    Fill(source);
    auto overlay = MakeRef<ProfilerOverlay>(DefaultAllocator(), DefaultAllocator());
    overlay->SetSource(&source);
    CHECK_FALSE(overlay->IsHitTestVisible);
    Stage stage(*overlay, Float2{1280, 720});

    const Rectangle panel = overlay->Panel();
    CHECK(panel.width == doctest::Approx(f32{ProfilerOverlay::kPanelWidth}));
    CHECK(panel.x == doctest::Approx(1280.0f - ProfilerOverlay::kMargin - ProfilerOverlay::kPanelWidth));
    CHECK(panel.y == doctest::Approx(f32{ProfilerOverlay::kMargin}));
    // The views inside the panel, one under the next.
    CHECK(overlay->Graph().Width() == doctest::Approx(ProfilerOverlay::kPanelWidth - 2 * ProfilerOverlay::kPadding));
    CHECK(overlay->Stats().Bounds.y > overlay->Graph().Bounds.y);
    CHECK(overlay->Counters().Bounds.y + overlay->Counters().Height() <= panel.y + panel.height);
    CHECK(overlay->Scopes().Rows().Size() == 1);
    CHECK(stage.Draw() > 0);

    overlay->SetCorner(OverlayCorner::BottomLeft);
    stage.root->Measure(fui::BoxConstraints::Tight(1280, 720));
    stage.root->Layout(0, 0, 1280, 720);
    CHECK(overlay->Panel().x == doctest::Approx(f32{ProfilerOverlay::kMargin}));
    CHECK(overlay->Panel().y + overlay->Panel().height == doctest::Approx(720.0f - ProfilerOverlay::kMargin));
}
