// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// A viewer's own copy of frames: the frames after it began, once their
// GPU passes have had time to land, with the tables they lean on; the oldest dropped when full;
// a source like any other, so it saves as a capture. The store it copies from is left alone.
#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.profiler;

using namespace foundation::core;
using namespace foundation::profiler;

namespace
{
    // `count` frames, each an Update scope (a new name on the frame numbered `lateNameAt`).
    void RunFrames(Profiler& p, u32 count, u64 lateNameAt = ~0ull)
    {
        for (u32 f = 0; f < count; ++f)
        {
            p.BeginFrame();
            REQUIRE(p.BeginScope(p.CurrentFrameNumber() == lateNameAt ? "Late" : "Update"));
            p.EndScope();
            p.EndFrame();
        }
    }
}

TEST_CASE("profile recording: frames after it began, once settled, with their tables")
{
    Profiler p(DefaultAllocator());
    p.Arm();
    RunFrames(p, 3); // before it began: not recorded
    LiveProfileSource live(p);
    ProfileRecording recording(DefaultAllocator());
    CHECK_FALSE(recording.HasFrames());
    recording.Begin(live);
    CHECK(recording.TickFrequency() == live.TickFrequency());

    RunFrames(p, 6, 5); // frames 3..8; frame 5 interns a new name
    // A GPU pass for frame 4 arrives late, as a pass does; the recording waits for it.
    GpuPassEvent pass;
    pass.name = p.InternName(StringView(u8"forward"));
    pass.beginTick = p.Frame(4)->beginTick;
    pass.endTick = pass.beginTick + 10;
    p.SubmitGpuPasses(4, Span<const GpuPassEvent>(&pass, 1));

    recording.Pull(live); // settled: up to 8 - 4 = frame 4
    REQUIRE(recording.HasFrames());
    CHECK(recording.OldestFrame() == 3u);
    CHECK(recording.NewestFrame() == 4u);
    REQUIRE(recording.Frame(4) != nullptr);
    CHECK(recording.Frame(4)->gpuValid);
    REQUIRE(recording.Frame(4)->gpu.Size() == 1);
    CHECK(recording.NameOf(recording.Frame(4)->gpu[0].name) == StringView(u8"forward"));
    CHECK(recording.Frame(2) == nullptr); // before it began

    recording.Pull(live, 0); // stopping: the rest as they are
    CHECK(recording.NewestFrame() == 8u);
    CHECK(recording.FrameCount() == 6u);
    const FrameRecord* late = recording.Frame(5);
    REQUIRE(late != nullptr);
    REQUIRE(late->threads.Size() == 1);
    REQUIRE(late->threads[0].scopes.Size() == 1);
    CHECK(recording.NameOf(late->threads[0].scopes[0].name) == StringView(u8"Late"));
    CHECK(recording.Threads().Size() == live.Threads().Size());

    // A second pull with nothing new copies nothing twice.
    recording.Pull(live, 0);
    CHECK(recording.FrameCount() == 6u);
    // The store it copies from is untouched: its history is all still there.
    CHECK(live.OldestFrame() == 0u);
    CHECK(live.NewestFrame() == 8u);
}

TEST_CASE("profile recording: full, it drops the oldest; cleared, it starts again")
{
    Profiler p(DefaultAllocator());
    p.Arm();
    LiveProfileSource live(p);
    ProfileRecording recording(DefaultAllocator(), 4);
    recording.Begin(live);
    RunFrames(p, 10);
    recording.Pull(live, 0);
    CHECK(recording.FrameCount() == 4u);
    CHECK(recording.OldestFrame() == 6u);
    CHECK(recording.NewestFrame() == 9u);
    for (u64 n = 6; n <= 9; ++n)
    {
        REQUIRE(recording.Frame(n) != nullptr);
        CHECK(recording.Frame(n)->frameNumber == n);
    }
    CHECK(recording.Frame(5) == nullptr);

    recording.Begin(live); // afresh: only what comes next
    CHECK_FALSE(recording.HasFrames());
    RunFrames(p, 2);
    recording.Pull(live, 0);
    CHECK(recording.OldestFrame() == 10u);
    CHECK(recording.NewestFrame() == 11u);
    REQUIRE(recording.Frame(10) != nullptr);
    CHECK(recording.Frame(10)->frameNumber == 10u);
}

TEST_CASE("profile recording: saves as a capture that reads back frame for frame")
{
    Profiler p(DefaultAllocator());
    p.Arm();
    LiveProfileSource live(p);
    ProfileRecording recording(DefaultAllocator());
    recording.Begin(live);
    RunFrames(p, 5, 2);
    recording.Pull(live, 0);

    MemoryStream bytes(DefaultAllocator());
    ProfileStreamWriter writer(DefaultAllocator(), bytes);
    StreamHeader header;
    header.tickFrequency = recording.TickFrequency();
    header.application = String(u8"Editor");
    REQUIRE(writer.WriteHeader(header));
    for (u64 n = recording.OldestFrame(); n <= recording.NewestFrame(); ++n)
    {
        REQUIRE(writer.WriteFrame(recording, *recording.Frame(n)));
    }
    (void)bytes.Seek(0, SeekOrigin::Begin);
    ProfileCapture capture(DefaultAllocator());
    REQUIRE(capture.Read(bytes));
    CHECK(capture.FrameCount() == 5u);
    CHECK(capture.OldestFrame() == recording.OldestFrame());
    const FrameRecord* frame = capture.Frame(2);
    REQUIRE(frame != nullptr);
    CHECK(capture.NameOf(frame->threads[0].scopes[0].name) == StringView(u8"Late"));
    CHECK(frame->endTick - frame->beginTick == recording.Frame(2)->endTick - recording.Frame(2)->beginTick);
}
