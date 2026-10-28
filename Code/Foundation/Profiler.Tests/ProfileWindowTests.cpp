// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// What the profiler's views show, computed from a source of known frames (Specs/profiler.md, P4):
// the newest frames, their CPU and GPU times and statistics, the scopes by their own time and the
// GPU passes by theirs, the counters' newest values and means, and counters as text in their unit.
#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.profiler;

#include "KnownSource.h"

using namespace foundation::core;
using namespace foundation::profiler;
using profiler_tests::KnownSource;

TEST_CASE("profiler window: the newest frames' times and statistics")
{
    KnownSource source;
    CHECK(LastFrames(source, 10).empty);
    const NameId forward = source.Name(u8"forward");
    const u64 lengths[] = {10, 20, 30, 40};
    for (const u64 ms : lengths)
    {
        FrameRecord& frame = source.AddFrame(ms);
        if (ms != 40) // the newest frame's GPU passes have not arrived
        {
            KnownSource::AddPass(frame, forward, 1, 1 + ms / 2);
            KnownSource::AddPass(frame, forward, 2, 3); // inside the first: the span is unchanged
        }
    }

    const FrameRange range = LastFrames(source, 3);
    CHECK_FALSE(range.empty);
    CHECK(range.first == 1u);
    CHECK(range.last == 3u);
    CHECK(LastFrames(source, 100).first == 0u); // no further back than the source holds

    Array<FrameTime> times(DefaultAllocator());
    CollectFrameTimes(source, range, times);
    REQUIRE(times.Size() == 3);
    CHECK(times[0].frame == 1u);
    CHECK(times[0].cpuMs == doctest::Approx(20.0f));
    CHECK(times[0].hasGpu);
    CHECK(times[0].gpuMs == doctest::Approx(10.0f));
    CHECK(times[2].cpuMs == doctest::Approx(40.0f));
    CHECK_FALSE(times[2].hasGpu);

    const FrameWindowStats stats = StatsOf(times.AsSpan(), DefaultAllocator());
    CHECK(stats.cpu.count == 3u);
    CHECK(stats.cpu.min == doctest::Approx(20.0f));
    CHECK(stats.cpu.median == doctest::Approx(30.0f));
    CHECK(stats.cpu.max == doctest::Approx(40.0f));
    CHECK(stats.cpu.mean == doctest::Approx(30.0f));
    CHECK(stats.gpu.count == 2u); // only the frames whose passes arrived
    CHECK(stats.gpu.max == doctest::Approx(15.0f));
}

TEST_CASE("profiler window: scopes rank by their own time, passes by theirs")
{
    KnownSource source;
    const NameId update = source.Name(u8"Update");
    const NameId physics = source.Name(u8"Physics");
    const NameId audio = source.Name(u8"Audio");
    const NameId render = source.Name(u8"Render");
    const NameId shadows = source.Name(u8"shadows");
    const NameId forward = source.Name(u8"forward");
    for (u32 f = 0; f < 2; ++f)
    {
        FrameRecord& frame = source.AddFrame(16);
        // In the order they end: Update (0..10) holds Physics (1..6) and Audio (6..7, 7..8).
        KnownSource::AddScope(frame, physics, 1, 1, 6);
        KnownSource::AddScope(frame, audio, 1, 6, 7);
        KnownSource::AddScope(frame, audio, 1, 7, 8);
        KnownSource::AddScope(frame, update, 0, 0, 10);
        KnownSource::AddScope(frame, render, 0, 10, 14);
        KnownSource::AddPass(frame, shadows, 0, 2);
        KnownSource::AddPass(frame, forward, 2, 9);
    }

    Array<TopEntry> top(DefaultAllocator());
    CollectTop(source, LastFrames(source, 2), TopLane::Scopes, 3, top);
    REQUIRE(top.Size() == 3); // four names, the three longest kept
    CHECK(top[0].name == physics);
    CHECK(top[0].msPerFrame == doctest::Approx(5.0f));
    CHECK(top[1].name == render);
    CHECK(top[1].msPerFrame == doctest::Approx(4.0f));
    CHECK(top[2].name == update); // 10 ms less its children's 7
    CHECK(top[2].msPerFrame == doctest::Approx(3.0f));

    CollectTop(source, LastFrames(source, 2), TopLane::Scopes, 10, top);
    REQUIRE(top.Size() == 4);
    CHECK(top[3].name == audio);
    CHECK(top[3].msPerFrame == doctest::Approx(2.0f));
    CHECK(top[3].callsPerFrame == doctest::Approx(2.0f));

    CollectTop(source, LastFrames(source, 2), TopLane::GpuPasses, 10, top);
    REQUIRE(top.Size() == 2);
    CHECK(top[0].name == forward);
    CHECK(top[0].msPerFrame == doctest::Approx(7.0f));
    CHECK(top[1].name == shadows);
}

TEST_CASE("profiler window: counters' newest values and means, as text in their unit")
{
    KnownSource source;
    const u32 draws = source.Counter(u8"Draws", CounterUnit::Count);
    const u32 memory = source.Counter(u8"Texture memory", CounterUnit::Bytes);
    source.Counter(u8"Never set", CounterUnit::Count);
    const f64 values[] = {10.0, 20.0, 60.0};
    for (const f64 v : values)
    {
        FrameRecord& frame = source.AddFrame(16);
        frame.counters.PushBack(CounterValue{draws, v});
        frame.counters.PushBack(CounterValue{memory, v * 1024.0 * 1024.0});
    }
    Array<CounterReading> readings(DefaultAllocator());
    CollectCounters(source, LastFrames(source, 10), readings);
    REQUIRE(readings.Size() == 3);
    CHECK(readings[0].present);
    CHECK(readings[0].latest == doctest::Approx(60.0));
    CHECK(readings[0].mean == doctest::Approx(30.0));
    CHECK(readings[1].latest == doctest::Approx(60.0 * 1024.0 * 1024.0));
    CHECK_FALSE(readings[2].present);

    IAllocator& a = DefaultAllocator();
    CHECK(FormatCounter(183.4, CounterUnit::Count, a) == StringView(u8"183"));
    CHECK(FormatCounter(512.0, CounterUnit::Bytes, a) == StringView(u8"0.5 KiB"));
    CHECK(FormatCounter(3.0 * 1024 * 1024, CounterUnit::Bytes, a) == StringView(u8"3.0 MiB"));
    CHECK(FormatCounter(6.0 * 1024 * 1024 * 1024, CounterUnit::Bytes, a) == StringView(u8"6.00 GiB"));
    CHECK(FormatCounter(1.5, CounterUnit::Milliseconds, a) == StringView(u8"1.50 ms"));
    CHECK(FormatCounter(42.34, CounterUnit::Percent, a) == StringView(u8"42.3%"));
}
