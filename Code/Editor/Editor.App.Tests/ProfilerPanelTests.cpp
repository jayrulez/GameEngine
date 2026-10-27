// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The editor's Profiler page (X7 of Documentation/Specs/editor-gui.md) over its markup
// (Data/Editor/Profiler.gml), on a test's own store: Record arms it and copies settled frames into
// the page's recording, following the newest until a frame is chosen, Live following again; Stop
// lets the store go and keeps the frames; another viewer's arm and the store's history are left
// alone; Stop on spike ends the recording at the first frame over its time; a plot per counter;
// Longest and Median; Save writes a capture that Open shows, and Live goes back.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.gui;
import foundation.gui.toolkit;
import foundation.profiler;
import foundation.profiler.capture;
import foundation.profiler.gui;
import editor.app;

using namespace foundation::core;
namespace profiler = foundation::profiler;
using namespace editor::app;
namespace gui = foundation::gui;
namespace pgui = foundation::profiler::gui;

namespace
{
    RefPtr<gui::Node> LoadPage()
    {
        const String path = PathJoin(PathJoin(StringView(reinterpret_cast<const char8_t*>(RAPTOR_SOURCE_DIR)), u8"Data").AsView(), StringView(ProfilerPanel::kPath));
        Result<Array<byte>> bytes = ReadFile(path.AsView());
        REQUIRE(bytes.HasValue());
        gui::RegisterGuiElements();
        gui::RegisterToolkitElements();
        pgui::RegisterProfilerElements();
        static const gui::ElementSet elements = gui::ElementSet::All();
        gui::MarkupLoader loader(elements, DefaultAllocator());
        return loader.LoadFromString(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
    }
    template <typename T>
    T* FindId(gui::Node& node, StringView id)
    {
        if (auto* widget = Cast<gui::UIWidget>(&node); widget != nullptr && widget->GetId() == id)
            return Cast<T>(&node);
        for (usize i = 0; i < node.ChildCount(); ++i)
            if (T* found = FindId<T>(*node.GetChildAt(i), id))
                return found;
        return nullptr;
    }

    // `count` frames of `ms` each (a busy wait, so a spike is a real one).
    void RunFrames(profiler::Profiler& p, u32 count, f64 ms = 0.0)
    {
        for (u32 f = 0; f < count; ++f)
        {
            p.BeginFrame();
            if (p.BeginScope("Editor.Frame"))
            {
                if (ms > 0.0)
                {
                    const u64 until = GetTicks() + static_cast<u64>(ms / 1000.0 * static_cast<f64>(GetTickFrequency()));
                    while (GetTicks() < until)
                    {
                    }
                }
                p.EndScope();
            }
            p.EndFrame();
        }
    }

    struct Bed
    {
        profiler::Profiler store{DefaultAllocator()};
        profiler::CaptureRecorder recorder{DefaultAllocator()};
        UniquePtr<ProfilerPanel> panel;
        Bed()
        {
            ProfilerPanelSeams seams;
            seams.store = &store;
            seams.recorder = &recorder;
            seams.folder = []() { return String(u8"scratch_profiler_panel"); };
            panel = MakeUnique<ProfilerPanel>(DefaultAllocator(), DefaultAllocator(), Move(seams), LoadPage());
            REQUIRE(panel->Content().Get() != nullptr);
        }
        template <typename T>
        T* Find(StringView id) { return FindId<T>(*panel->Content(), id); }
    };
}

TEST_CASE("profiler page: its views on one focus, from its markup")
{
    Bed bed;
    auto* strip = bed.Find<pgui::FrameStrip>(u8"profiler-strip");
    auto* timeline = bed.Find<pgui::ProfileTimeline>(u8"profiler-timeline");
    auto* table = bed.Find<pgui::ScopeTable>(u8"profiler-table");
    auto* detail = bed.Find<pgui::ScopeDetail>(u8"profiler-detail");
    REQUIRE(strip != nullptr);
    REQUIRE(timeline != nullptr);
    REQUIRE(table != nullptr);
    REQUIRE(detail != nullptr);
    CHECK(strip->GetProfileFocus() == &bed.panel->Focus());
    CHECK(timeline->GetProfileFocus() == &bed.panel->Focus());
    CHECK(table->GetProfileFocus() == &bed.panel->Focus());
    CHECK(detail->GetProfileFocus() == &bed.panel->Focus());
    CHECK(strip->GetFrameAxis() == &bed.panel->Axis());
    CHECK(strip->BudgetMs() == doctest::Approx(1000.0f / 60.0f)); // the markup's 60 fps
    bed.panel->SetBudgetChoice(3);
    CHECK(strip->BudgetMs() == 0.0f);
    CHECK(bed.panel->StatusText().ContainsIgnoreCase(u8"Play-in-editor")); // it says what it shows: the whole process
}

TEST_CASE("profiler page: Record copies settled frames and follows the newest; Stop keeps them")
{
    Bed bed;
    CHECK_FALSE(bed.panel->IsRecording());
    CHECK_FALSE(bed.store.Armed());

    bed.store.Arm(); // another viewer (a PIE game's overlay) already watching
    RunFrames(bed.store, 3);
    bed.panel->Start();
    CHECK(bed.panel->IsRecording());
    CHECK(bed.Find<gui::ToolButton>(u8"profiler-record")->IsChecked());
    RunFrames(bed.store, 10); // frames 3..12
    bed.panel->Update();
    CHECK(bed.panel->Recording().OldestFrame() == 3u);
    CHECK(bed.panel->Recording().NewestFrame() == 12u - profiler::ProfileRecording::kSettleFrames);
    // Following: the newest settled frame is the one chosen, in every view.
    CHECK(bed.panel->IsFollowing());
    CHECK(bed.Find<gui::ToolButton>(u8"profiler-live")->IsChecked());
    CHECK(bed.panel->Focus().Frames().first == bed.panel->Recording().NewestFrame());
    CHECK(bed.Find<pgui::ScopeTable>(u8"profiler-table")->Rows().Size() == 2); // Main, Editor.Frame
    CHECK(bed.panel->StatusText().ContainsIgnoreCase(u8"Recording"));

    // A chosen frame stops the following; Live brings it back.
    bed.panel->SetFollowing(false);
    bed.panel->Focus().SetFrame(4);
    RunFrames(bed.store, 3);
    bed.panel->Update();
    CHECK(bed.panel->Focus().Frames().first == 4u);
    CHECK_FALSE(bed.Find<gui::ToolButton>(u8"profiler-live")->IsChecked());
    bed.panel->ShowLive();
    CHECK(bed.panel->Focus().Frames().first == bed.panel->Recording().NewestFrame());

    bed.panel->Stop();
    CHECK_FALSE(bed.panel->IsRecording());
    CHECK_FALSE(bed.Find<gui::ToolButton>(u8"profiler-record")->IsChecked());
    CHECK(bed.panel->Recording().NewestFrame() == 15u); // the rest taken as they were
    CHECK(bed.store.Armed());                           // the other viewer's arm is still held
    bed.store.Disarm();
    CHECK_FALSE(bed.store.Armed());
    CHECK(bed.store.OldestKeptFrame() == 0u); // the store's history untouched
    RunFrames(bed.store, 2);
    bed.panel->Update(); // stopped: nothing more
    CHECK(bed.panel->Recording().NewestFrame() == 15u);
}

TEST_CASE("profiler page: Stop on spike ends the recording at the first frame over its time")
{
    Bed bed;
    bed.panel->SetSpikeChoice(0); // 25 ms
    bed.panel->SetStopOnSpike(true);
    CHECK(bed.Find<gui::ToolButton>(u8"profiler-spike")->IsChecked());
    bed.panel->Start();
    RunFrames(bed.store, 2);
    RunFrames(bed.store, 1, 40.0); // frame 2: the spike
    RunFrames(bed.store, 4);
    bed.panel->Update();
    CHECK_FALSE(bed.panel->IsRecording());
    CHECK_FALSE(bed.panel->IsFollowing());
    CHECK(bed.panel->Focus().Frames().first == 2u);
    CHECK(bed.panel->StatusText().ContainsIgnoreCase(u8"Stopped on frame 2"));
}

TEST_CASE("profiler page: a plot per counter; Longest and Median choose frames")
{
    Bed bed;
    const u32 draws = bed.store.RegisterCounter(u8"Draw calls", profiler::CounterUnit::Count);
    bed.panel->Start();
    for (u32 f = 0; f < 6; ++f)
    {
        bed.store.SetCounter(draws, static_cast<f64>(f));
        RunFrames(bed.store, 1, f == 3 ? 12.0 : 0.0);
    }
    bed.panel->Stop();
    CHECK(bed.panel->CounterPlotCount() == 1);
    CHECK(bed.Find<gui::Panel>(u8"profiler-counters")->ChildCount() == 1);
    bed.panel->ChooseLongest();
    CHECK(bed.panel->Focus().Frames().first == 3u);
    CHECK_FALSE(bed.panel->IsFollowing());
    bed.panel->ChooseMedian();
    CHECK(bed.panel->Focus().Frames().first != 3u);
}

TEST_CASE("profiler page: Save writes a capture; Open shows it; Live goes back")
{
    Bed bed;
    bed.recorder.SetBuild(StringView(u8"Linux64"), StringView(u8"test-build"));
    CHECK(bed.panel->SaveRecording().IsEmpty()); // nothing recorded yet

    bed.panel->Start();
    RunFrames(bed.store, 8);
    bed.panel->Stop();
    const String path = bed.panel->SaveRecording();
    REQUIRE_FALSE(path.IsEmpty());
    CHECK(path.AsView().EndsWith(u8".aeprofile"));
    CHECK(path.AsView().ContainsIgnoreCase(u8"scratch_profiler_panel/Editor-"));
    CHECK(bed.panel->StatusText().ContainsIgnoreCase(u8"Saved 8 frames"));

    REQUIRE(bed.panel->OpenCapture(path.AsView()));
    CHECK(bed.panel->ViewingCapture());
    const profiler::IProfileSource* shown = bed.panel->Focus().Source();
    REQUIRE(shown != nullptr);
    CHECK(shown != &bed.panel->Recording());
    CHECK(shown->NewestFrame() == bed.panel->Recording().NewestFrame());
    CHECK(bed.panel->Focus().Frames().first == shown->NewestFrame()); // the newest frame shown
    CHECK(bed.panel->StatusText().ContainsIgnoreCase(u8"test-build"));
    CHECK(bed.panel->SaveRecording().IsEmpty()); // a capture is already a file

    bed.panel->ShowLive();
    CHECK_FALSE(bed.panel->ViewingCapture());
    CHECK(bed.panel->Focus().Source() == &bed.panel->Recording());

    CHECK_FALSE(bed.panel->OpenCapture(u8"scratch_profiler_panel/not-there.aeprofile"));
    CHECK(bed.panel->StatusText().ContainsIgnoreCase(u8"Could not read"));
    CHECK_FALSE(bed.panel->ViewingCapture());
}

TEST_CASE("profiler page: recording while closing lets the store go")
{
    profiler::Profiler store(DefaultAllocator());
    profiler::CaptureRecorder recorder(DefaultAllocator());
    {
        ProfilerPanelSeams seams;
        seams.store = &store;
        seams.recorder = &recorder;
        seams.folder = []() { return String(u8"scratch_profiler_panel"); };
        ProfilerPanel panel(DefaultAllocator(), Move(seams), LoadPage());
        panel.Start();
        CHECK(store.Armed());
    }
    CHECK_FALSE(store.Armed());
}
