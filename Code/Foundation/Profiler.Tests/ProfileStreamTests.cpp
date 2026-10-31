// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The profiler's stream: frames written with the tables they lean on and read back as a capture,
// a source like the live store. A capture file is this stream saved.
#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.profiler;

using namespace foundation::core;
using namespace foundation::profiler;

namespace
{
    GpuPassEvent Pass(NameId name, u64 beginTick, u64 endTick)
    {
        GpuPassEvent pass;
        pass.name = name;
        pass.beginTick = beginTick;
        pass.endTick = endTick;
        return pass;
    }

    // Three frames: scopes, a GPU pass with counts on the second, a counter on each.
    void Record(Profiler& p)
    {
        const u32 draws = p.RegisterCounter(StringView(u8"Draws"), CounterUnit::Count);
        p.Arm();
        for (u32 f = 0; f < 3; ++f)
        {
            p.BeginFrame();
            REQUIRE(p.BeginScope("Update"));
            REQUIRE(p.BeginScope(f == 2 ? "Late name" : "Physics"));
            p.EndScope();
            p.EndScope();
            p.SetCounter(draws, 10.0 + f);
            p.EndFrame();
        }
        GpuPassEvent pass = Pass(p.InternName(StringView(u8"forward")), 100, 200);
        pass.hasCounts = true;
        pass.counts.inputPrimitives = 12;
        pass.counts.fragmentInvocations = 4096;
        p.SubmitGpuPasses(1, Span<const GpuPassEvent>(&pass, 1));
    }
}

TEST_CASE("profile stream: a capture reads back frame for frame")
{
    Profiler p(DefaultAllocator());
    Record(p);
    LiveProfileSource live(p);

    MemoryStream bytes(DefaultAllocator());
    ProfileStreamWriter writer(DefaultAllocator(), bytes);
    StreamHeader header;
    header.tickFrequency = live.TickFrequency();
    header.application = String(u8"Probe");
    header.platform = String(u8"Linux64");
    header.build = String(u8"test");
    REQUIRE(writer.WriteHeader(header));
    for (u64 n = live.OldestFrame(); n <= live.NewestFrame(); ++n)
    {
        if (const FrameRecord* frame = live.Frame(n))
        {
            REQUIRE(writer.WriteFrame(live, *frame));
        }
    }

    (void)bytes.Seek(0, SeekOrigin::Begin);
    ProfileCapture capture(DefaultAllocator());
    REQUIRE(capture.Read(bytes));
    CHECK(capture.Header().application == String(u8"Probe"));
    CHECK(capture.TickFrequency() == live.TickFrequency());
    REQUIRE(capture.FrameCount() == 3u);
    CHECK(capture.OldestFrame() == live.OldestFrame());
    CHECK(capture.NewestFrame() == live.NewestFrame());
    CHECK(capture.NameCount() == live.NameCount());
    REQUIRE(capture.Threads().Size() == live.Threads().Size());
    CHECK(capture.Threads()[0].name == live.Threads()[0].name);
    REQUIRE(capture.Counters().Size() == 1u);
    CHECK(capture.Counters()[0].unit == CounterUnit::Count);

    for (u64 n = live.OldestFrame(); n <= live.NewestFrame(); ++n)
    {
        const FrameRecord* a = live.Frame(n);
        const FrameRecord* b = capture.Frame(n);
        REQUIRE(a != nullptr);
        REQUIRE(b != nullptr);
        CHECK(b->beginTick == a->beginTick);
        CHECK(b->endTick == a->endTick);
        REQUIRE(b->threads.Size() == a->threads.Size());
        REQUIRE(b->threads[0].scopes.Size() == a->threads[0].scopes.Size());
        for (usize i = 0; i < a->threads[0].scopes.Size(); ++i)
        {
            const ScopeEvent& sa = a->threads[0].scopes[i];
            const ScopeEvent& sb = b->threads[0].scopes[i];
            CHECK(capture.NameOf(sb.name) == live.NameOf(sa.name));
            CHECK(sb.depth == sa.depth);
            CHECK(sb.endTick == sa.endTick);
        }
        CHECK(b->gpuValid == a->gpuValid);
        REQUIRE(b->gpu.Size() == a->gpu.Size());
        REQUIRE(b->counters.Size() == 1u);
        CHECK(b->counters[0].value == doctest::Approx(a->counters[0].value));
    }
    const FrameRecord* withGpu = capture.Frame(1);
    REQUIRE(withGpu->gpu.Size() == 1u);
    CHECK(capture.NameOf(withGpu->gpu[0].name) == StringView(u8"forward"));
    CHECK(withGpu->gpu[0].hasCounts);
    CHECK(withGpu->gpu[0].counts.fragmentInvocations == 4096u);
    CHECK(capture.Frame(7) == nullptr);
}

TEST_CASE("profile stream: tables travel once, a frame's new names just before it")
{
    // Written in two goes (as a live stream is): the second sends only what is new since.
    Profiler p(DefaultAllocator());
    p.Arm();
    p.BeginFrame();
    REQUIRE(p.BeginScope("First"));
    p.EndScope();
    p.EndFrame();
    LiveProfileSource live(p);
    MemoryStream bytes(DefaultAllocator());
    ProfileStreamWriter writer(DefaultAllocator(), bytes);
    REQUIRE(writer.WriteHeader(StreamHeader{}));
    REQUIRE(writer.WriteFrame(live, *p.LatestFrame()));
    const i64 afterFirst = bytes.Size();
    REQUIRE(writer.WriteFrame(live, *p.LatestFrame()));
    const i64 sameAgain = bytes.Size() - afterFirst; // no tables: just the frame chunk
    p.BeginFrame();
    REQUIRE(p.BeginScope("Second"));
    p.EndScope();
    p.EndFrame();
    const i64 beforeThird = bytes.Size();
    REQUIRE(writer.WriteFrame(live, *p.LatestFrame()));
    CHECK(bytes.Size() - beforeThird > sameAgain); // carries the new name
    (void)bytes.Seek(0, SeekOrigin::Begin);
    ProfileCapture capture(DefaultAllocator());
    CHECK_FALSE(capture.Read(bytes)); // the repeated frame number is refused
    CHECK(capture.NameCount() == 1u); // read up to it
}

TEST_CASE("profile stream: an unknown chunk is skipped, a foreign or cut stream refused")
{
    Profiler p(DefaultAllocator());
    Record(p);
    LiveProfileSource live(p);
    MemoryStream bytes(DefaultAllocator());
    ProfileStreamWriter writer(DefaultAllocator(), bytes);
    REQUIRE(writer.WriteHeader(StreamHeader{}));
    // A chunk from a later version: tag 200, three bytes.
    const u8 unknown[] = {200, 3, 0, 0, 0, 1, 2, 3};
    (void)bytes.Write(unknown, sizeof(unknown));
    REQUIRE(writer.WriteFrame(live, *p.LatestFrame()));
    (void)bytes.Seek(0, SeekOrigin::Begin);
    ProfileCapture capture(DefaultAllocator());
    CHECK(capture.Read(bytes));
    CHECK(capture.FrameCount() == 1u);

    // Cut short in the frame chunk.
    const Span<const byte> whole = bytes.Bytes();
    MemoryStream cut(DefaultAllocator());
    (void)cut.Write(whole.Data(), whole.Size() - 5);
    (void)cut.Seek(0, SeekOrigin::Begin);
    ProfileCapture partial(DefaultAllocator());
    CHECK_FALSE(partial.Read(cut));

    // Not a profile stream at all.
    MemoryStream other(DefaultAllocator());
    const u32 notMagic = 0x12345678u;
    (void)other.Write(&notMagic, sizeof(notMagic));
    (void)other.Seek(0, SeekOrigin::Begin);
    ProfileCapture foreign(DefaultAllocator());
    CHECK_FALSE(foreign.Read(other));
}
