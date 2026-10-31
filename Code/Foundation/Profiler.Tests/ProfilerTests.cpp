// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The capture store: threads record the scopes they finish into rings of
// their own, the frame end gathers them into a frame record that refers to nothing in the process
// (names are ids, threads indices), and the last few seconds of records are kept. Each test uses a
// profiler of its own unless it is about the process's one (PROFILE_SCOPE, the job hooks).
#include <doctest/doctest.h>

#include "Core/Prelude.h"
#include "Profiler/Profiler.h"

import foundation.core;
import foundation.profiler;

using namespace foundation::core;
using namespace foundation::profiler;

namespace
{
    constexpr usize kNotFound = ~usize(0);

    // Where `needle` first appears in `text`, or kNotFound.
    usize IndexOf(StringView text, StringView needle)
    {
        for (usize i = 0; i + needle.Size() <= text.Size(); ++i)
        {
            if (text.SubStr(i, needle.Size()) == needle)
            {
                return i;
            }
        }
        return kNotFound;
    }

    GpuPassEvent Pass(NameId name, u32 queue, u64 beginTick, u64 endTick)
    {
        GpuPassEvent pass;
        pass.name = name;
        pass.queue = queue;
        pass.beginTick = beginTick;
        pass.endTick = endTick;
        return pass;
    }

    // Busy for a little while, so a scope has a duration a tick counter can see.
    void Spin()
    {
        const u64 start = GetTicks();
        while (GetTicks() == start)
        {
        }
    }
}

TEST_CASE("profiler: unarmed, nothing is recorded and nothing is kept")
{
    Profiler p(DefaultAllocator());
    CHECK_FALSE(p.Armed());
    p.BeginFrame();
    CHECK_FALSE(p.BeginScope("Alpha"));
    p.EndFrame();
    CHECK(p.LatestFrame() == nullptr);
    CHECK(p.CurrentFrameNumber() == 1u); // frames are counted anyway
    CHECK(IndexOf(p.BuildReport().AsView(), StringView(u8"not armed")) != kNotFound);
}

TEST_CASE("profiler: a frame holds its thread's scopes, nested, in ticks")
{
    Profiler p(DefaultAllocator());
    p.Arm();
    p.BeginFrame();
    REQUIRE(p.BeginScope("Outer"));
    REQUIRE(p.BeginScope("Inner"));
    REQUIRE(p.BeginScope("Deepest"));
    Spin();
    p.EndScope();
    p.EndScope();
    p.EndScope();
    REQUIRE(p.BeginScope("Sibling"));
    p.EndScope();
    p.EndFrame();

    const FrameRecord* frame = p.LatestFrame();
    REQUIRE(frame != nullptr);
    CHECK(frame->frameNumber == 0u);
    CHECK(frame->endTick >= frame->beginTick);
    REQUIRE(frame->threads.Size() == 1u);
    const ThreadFrame& thread = frame->threads[0];
    CHECK(thread.dropped == 0u);
    // Recorded as they finished: the deepest first.
    REQUIRE(thread.scopes.Size() == 4u);
    CHECK(p.NameOf(thread.scopes[0].name) == StringView(u8"Deepest"));
    CHECK(thread.scopes[0].depth == 2u);
    CHECK(p.NameOf(thread.scopes[1].name) == StringView(u8"Inner"));
    CHECK(thread.scopes[1].depth == 1u);
    CHECK(p.NameOf(thread.scopes[2].name) == StringView(u8"Outer"));
    CHECK(thread.scopes[2].depth == 0u);
    CHECK(p.NameOf(thread.scopes[3].name) == StringView(u8"Sibling"));
    CHECK(thread.scopes[3].depth == 0u);
    // Each lies inside its parent, and the frame holds them all.
    CHECK(thread.scopes[0].beginTick >= thread.scopes[1].beginTick);
    CHECK(thread.scopes[0].endTick <= thread.scopes[1].endTick);
    CHECK(thread.scopes[1].endTick <= thread.scopes[2].endTick);
    CHECK(thread.scopes[0].endTick > thread.scopes[0].beginTick);
    CHECK(thread.scopes[2].beginTick >= frame->beginTick);
    CHECK(thread.scopes[3].endTick <= frame->endTick);
    // A literal named twice is one name.
    p.BeginFrame();
    REQUIRE(p.BeginScope("Outer"));
    p.EndScope();
    p.EndFrame();
    CHECK(p.LatestFrame()->threads[0].scopes[0].name == thread.scopes[2].name);
}

TEST_CASE("profiler: a scope can be opened by a name kept as an id")
{
    // A render graph names the same passes every frame: it interns once and opens by id.
    Profiler p(DefaultAllocator());
    const NameId shadows = p.InternName(StringView(u8"shadows"));
    CHECK_FALSE(p.BeginScope(shadows)); // unarmed
    p.Arm();
    p.BeginFrame();
    REQUIRE(p.BeginScope(shadows));
    p.EndScope();
    p.EndFrame();
    REQUIRE(p.LatestFrame() != nullptr);
    CHECK(p.LatestFrame()->threads[0].scopes[0].name == shadows);
    CHECK(p.NameOf(shadows) == StringView(u8"shadows"));
}

TEST_CASE("profiler: names outlive the strings they came from")
{
    // A record refers to nothing in the process: a name made at run time is copied into the table.
    Profiler p(DefaultAllocator());
    p.Arm();
    p.BeginFrame();
    {
        String made;
        AppendFormat(made, u8"Pass {}", 7);
        REQUIRE(p.BeginScope(made.AsView()));
        p.EndScope();
        made = String(u8"overwritten");
    }
    p.EndFrame();
    REQUIRE(p.LatestFrame() != nullptr);
    CHECK(p.NameOf(p.LatestFrame()->threads[0].scopes[0].name) == StringView(u8"Pass 7"));
    CHECK(p.InternName(StringView(u8"Pass 7")) == p.LatestFrame()->threads[0].scopes[0].name);
}

#if !PLATFORM_WEB
namespace
{
    const ThreadFrame* ThreadNamed(const Profiler& p, const FrameRecord& frame, StringView name)
    {
        for (const ThreadFrame& thread : frame.threads)
        {
            if (thread.thread < p.Threads().Size() && p.Threads()[thread.thread].name.AsView() == name)
            {
                return &thread;
            }
        }
        return nullptr;
    }
}

TEST_CASE("profiler: every thread's scopes reach the frame, under the thread's name")
{
    Profiler p(DefaultAllocator());
    p.Arm();
    p.BeginFrame();
    {
        Thread a(
            [&p]()
            {
                SetCurrentThreadName(StringView(u8"Loader"));
                for (int i = 0; i < 5; ++i)
                {
                    REQUIRE(p.BeginScope("Load"));
                    p.EndScope();
                }
            });
        Thread b(
            [&p]()
            {
                SetCurrentThreadName(StringView(u8"Mixer"));
                REQUIRE(p.BeginScope("Mix"));
                REQUIRE(p.BeginScope("Voice"));
                p.EndScope();
                p.EndScope();
            });
        a.Join();
        b.Join();
    }
    p.EndFrame();

    const FrameRecord* frame = p.LatestFrame();
    REQUIRE(frame != nullptr);
    const ThreadFrame* loader = ThreadNamed(p, *frame, StringView(u8"Loader"));
    const ThreadFrame* mixer = ThreadNamed(p, *frame, StringView(u8"Mixer"));
    REQUIRE(loader != nullptr);
    REQUIRE(mixer != nullptr);
    CHECK(loader->scopes.Size() == 5u);
    REQUIRE(mixer->scopes.Size() == 2u);
    CHECK(p.NameOf(mixer->scopes[0].name) == StringView(u8"Voice"));
    CHECK(mixer->scopes[0].depth == 1u);
    CHECK(loader->thread != mixer->thread);
}
#endif

TEST_CASE("profiler: a full ring drops new scopes and says how many")
{
    Profiler p(DefaultAllocator());
    p.SetRingCapacity(4, 4);
    p.Arm();
    p.BeginFrame();
    for (int i = 0; i < 10; ++i)
    {
        REQUIRE(p.BeginScope("Burst"));
        p.EndScope();
    }
    p.EndFrame();
    REQUIRE(p.LatestFrame() != nullptr);
    CHECK(p.LatestFrame()->threads[0].scopes.Size() == 4u);
    CHECK(p.LatestFrame()->threads[0].dropped == 6u);

    // The next frame has room again.
    p.BeginFrame();
    REQUIRE(p.BeginScope("Burst"));
    p.EndScope();
    p.EndFrame();
    CHECK(p.LatestFrame()->threads[0].scopes.Size() == 1u);
    CHECK(p.LatestFrame()->threads[0].dropped == 0u);
}

TEST_CASE("profiler: the history keeps the last frames, each found by its number")
{
    Profiler p(DefaultAllocator());
    p.SetHistoryLength(3);
    p.Arm();
    for (int f = 0; f < 5; ++f)
    {
        p.BeginFrame();
        REQUIRE(p.BeginScope("Work"));
        p.EndScope();
        p.EndFrame();
    }
    CHECK(p.Frame(0) == nullptr);
    CHECK(p.Frame(1) == nullptr);
    REQUIRE(p.Frame(2) != nullptr);
    CHECK(p.Frame(2)->frameNumber == 2u);
    REQUIRE(p.Frame(4) != nullptr);
    CHECK(p.LatestFrame() == p.Frame(4));
    CHECK(p.Frame(5) == nullptr); // not run yet
    CHECK(p.Frame(3)->beginTick >= p.Frame(2)->endTick);
}

TEST_CASE("profiler: arms are counted, and an unarmed frame keeps no record")
{
    Profiler p(DefaultAllocator());
    p.Arm();
    p.Arm();
    p.Disarm();
    CHECK(p.Armed());
    p.Disarm();
    CHECK_FALSE(p.Armed());
    p.Disarm(); // one too many does not wrap
    CHECK_FALSE(p.Armed());
    p.Arm();
    CHECK(p.Armed());
    p.Disarm();

    // Scopes left in a ring when the profiler is disarmed are let go, not shown later.
    p.Arm();
    p.BeginFrame();
    REQUIRE(p.BeginScope("Before"));
    p.EndScope();
    p.Disarm();
    p.EndFrame();
    p.Arm();
    p.BeginFrame();
    p.EndFrame();
    REQUIRE(p.LatestFrame() != nullptr);
    CHECK(p.LatestFrame()->threads.IsEmpty());
}

TEST_CASE("profiler: a scope opened while unarmed is not closed by arming inside it")
{
    // PROFILE_SCOPE closes only what it opened, so the thread's nesting stays right.
    Profiler& p = Profiler::Get();
    REQUIRE_FALSE(p.Armed());
    p.BeginFrame();
    {
        ScopedProfile outer("Unrecorded");
        p.Arm();
        {
            ScopedProfile inner("Recorded");
        }
    }
    p.EndFrame();
    const FrameRecord* frame = p.LatestFrame();
    REQUIRE(frame != nullptr);
    usize found = 0;
    for (const ThreadFrame& thread : frame->threads)
    {
        for (const ScopeEvent& scope : thread.scopes)
        {
            CHECK(p.NameOf(scope.name) != StringView(u8"Unrecorded"));
            if (p.NameOf(scope.name) == StringView(u8"Recorded"))
            {
                CHECK(scope.depth == 0u);
                ++found;
            }
        }
    }
    CHECK(found == 1u);
    p.Disarm();
}

TEST_CASE("profiler: a scope skipped while unarmed leaves its recorded parent open")
{
    // The case above cannot tell: its outer scope is at depth 0, so a stray close finds the stack
    // empty. Here the skipped scope sits inside a recorded parent; closing it must not close the
    // parent, or the child after it lands at depth 0, outside the parent.
    Profiler& p = Profiler::Get();
    REQUIRE_FALSE(p.Armed());
    p.Arm();
    p.BeginFrame();
    {
        ScopedProfile parent("Parent");
        p.Disarm();
        {
            ScopedProfile skipped("Skipped");
            p.Arm();
        }
        {
            ScopedProfile child("Child");
        }
    }
    p.EndFrame();
    const FrameRecord* frame = p.LatestFrame();
    REQUIRE(frame != nullptr);
    usize found = 0;
    for (const ThreadFrame& thread : frame->threads)
    {
        for (const ScopeEvent& scope : thread.scopes)
        {
            CHECK(p.NameOf(scope.name) != StringView(u8"Skipped"));
            if (p.NameOf(scope.name) == StringView(u8"Child"))
            {
                CHECK(scope.depth == 1u);
                ++found;
            }
        }
    }
    CHECK(found == 1u);
    p.Disarm();
}

TEST_CASE("profiler: GPU passes join their frame's record when they arrive")
{
    Profiler p(DefaultAllocator());
    p.SetHistoryLength(2);
    p.Arm();
    p.BeginFrame();
    p.EndFrame(); // frame 0
    p.BeginFrame();
    p.EndFrame(); // frame 1
    CHECK_FALSE(p.Frame(0)->gpuValid);

    // A frame or two late, frame 0's passes arrive.
    const NameId shadow = p.InternName(StringView(u8"shadow"));
    const GpuPassEvent passes[] = {Pass(shadow, 0, 100, 250), Pass(shadow, 1, 260, 300)};
    p.SubmitGpuPasses(0, Span<const GpuPassEvent>(passes, 2));
    REQUIRE(p.Frame(0)->gpuValid);
    REQUIRE(p.Frame(0)->gpu.Size() == 2u);
    CHECK(p.Frame(0)->gpu[1].queue == 1u);
    CHECK(p.Frame(0)->gpu[0].endTick == 250u);

    // Another render graph of the same frame adds its passes beside them, with its counts.
    GpuPassEvent other = Pass(p.InternName(StringView(u8"preview")), 0, 400, 450);
    other.hasCounts = true;
    other.counts.inputVertices = 36;
    p.SubmitGpuPasses(0, Span<const GpuPassEvent>(&other, 1));
    REQUIRE(p.Frame(0)->gpu.Size() == 3u);
    CHECK(p.Frame(0)->gpu[2].hasCounts);
    CHECK(p.Frame(0)->gpu[2].counts.inputVertices == 36u);
    CHECK_FALSE(p.Frame(0)->gpu[0].hasCounts);

    // Passes for a frame the history no longer keeps are let go.
    p.BeginFrame();
    p.EndFrame(); // frame 2 takes frame 0's slot
    p.SubmitGpuPasses(0, Span<const GpuPassEvent>(passes, 2));
    CHECK_FALSE(p.Frame(2)->gpuValid);
    CHECK(p.Frame(0) == nullptr);
}

TEST_CASE("profiler: counters are registered once and a frame keeps the values set in it")
{
    Profiler p(DefaultAllocator());
    const u32 draws = p.RegisterCounter(StringView(u8"Draws"), CounterUnit::Count);
    const u32 memory = p.RegisterCounter(StringView(u8"Texture memory"), CounterUnit::Bytes);
    CHECK(p.RegisterCounter(StringView(u8"Draws"), CounterUnit::Count) == draws);
    REQUIRE(p.Counters().Size() == 2u);
    CHECK(p.Counters()[memory].unit == CounterUnit::Bytes);

    p.Arm();
    p.BeginFrame();
    p.SetCounter(draws, 10.0);
    p.SetCounter(draws, 12.0); // the last value set wins
    p.SetCounter(memory, 4096.0);
    p.EndFrame();
    REQUIRE(p.LatestFrame()->counters.Size() == 2u);
    CHECK(p.LatestFrame()->counters[0].counter == draws);
    CHECK(p.LatestFrame()->counters[0].value == doctest::Approx(12.0));

    p.BeginFrame();
    p.SetCounter(memory, 8192.0);
    p.EndFrame();
    REQUIRE(p.LatestFrame()->counters.Size() == 1u); // draws was not set this frame
    CHECK(p.LatestFrame()->counters[0].counter == memory);
}

#if !PLATFORM_WEB
TEST_CASE("profiler: a job system's jobs show on their workers' lanes, with their scopes inside")
{
    Profiler& p = Profiler::Get(); // the process's profiler holds the job hooks
    REQUIRE_FALSE(p.Armed());
    p.Arm();
    p.BeginFrame();
    {
        JobSystem jobs(DefaultAllocator(), 2);
        for (int i = 0; i < 40; ++i)
        {
            jobs.Submit(
                []()
                {
                    PROFILE_SCOPE("Inside a job");
                    Spin();
                });
        }
        jobs.WaitForAll();
    }
    p.EndFrame();
    const FrameRecord* frame = p.LatestFrame();
    REQUIRE(frame != nullptr);
    // The caller helps run jobs while it waits, so a job's lane is a worker's or the caller's.
    const String caller(CurrentThreadName().IsEmpty() ? StringView(u8"Thread") : CurrentThreadName());
    usize jobScopes = 0, insideScopes = 0;
    for (const ThreadFrame& thread : frame->threads)
    {
        const StringView lane = p.Threads()[thread.thread].name.AsView();
        for (const ScopeEvent& scope : thread.scopes)
        {
            if (p.NameOf(scope.name) == StringView(u8"Job"))
            {
                ++jobScopes;
                CHECK(scope.depth == 0u);
                CHECK((lane.StartsWith(StringView(u8"Worker ")) || lane.StartsWith(caller.AsView())));
            }
            if (p.NameOf(scope.name) == StringView(u8"Inside a job"))
            {
                ++insideScopes;
                CHECK(scope.depth == 1u);
            }
        }
    }
    CHECK(jobScopes == 40u);
    CHECK(insideScopes == 40u);
    p.Disarm();
}
#endif

TEST_CASE("profiler: the report shows the newest frame by thread, indented by depth")
{
    Profiler p(DefaultAllocator());
    p.Arm();
    p.BeginFrame();
    REQUIRE(p.BeginScope("Update"));
    REQUIRE(p.BeginScope("Physics"));
    p.EndScope();
    p.EndScope();
    p.EndFrame();
    const NameId pass = p.InternName(StringView(u8"forward"));
    const GpuPassEvent gpu[] = {Pass(pass, 0, 0, 10)};
    p.SubmitGpuPasses(0, Span<const GpuPassEvent>(gpu, 1));
    const String report = p.BuildReport();
    const StringView text = report.AsView();
    CHECK(IndexOf(text, StringView(u8"=== Profile: frame 0")) != kNotFound);
    CHECK(IndexOf(text, StringView(u8"2 scopes")) != kNotFound);
    CHECK(IndexOf(text, StringView(u8"    Update: ")) != kNotFound);
    CHECK(IndexOf(text, StringView(u8"      Physics: ")) != kNotFound);
    CHECK(IndexOf(text, StringView(u8"[GPU]")) != kNotFound);
    CHECK(IndexOf(text, StringView(u8"    forward: ")) != kNotFound);
    // Update begins first, so it is listed before its child.
    CHECK(IndexOf(text, StringView(u8"Update")) < IndexOf(text, StringView(u8"Physics")));
}
