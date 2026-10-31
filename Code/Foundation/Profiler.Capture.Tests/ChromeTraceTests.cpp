// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The Chrome trace export: a source's frames as the JSON chrome://tracing and Perfetto open, a
// lane per thread as well as the frames and the GPU, and counters as counter tracks.
#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.json;
import foundation.profiler;
import foundation.profiler.capture;

using namespace foundation::core;
using namespace foundation::profiler;
namespace json = foundation::json;

namespace
{
    void Record(Profiler& p)
    {
        const u32 draws = p.RegisterCounter(StringView(u8"Draws"), CounterUnit::Count);
        p.Arm();
        for (u32 f = 0; f < 2; ++f)
        {
            p.BeginFrame();
            REQUIRE(p.BeginScope("Update"));
            REQUIRE(p.BeginScope("Physics"));
            const u64 start = GetTicks();
            while (GetTicks() == start)
            {
            }
            p.EndScope();
            p.EndScope();
            p.SetCounter(draws, 7.0 + f);
            p.EndFrame();
        }
        GpuPassEvent pass;
        pass.name = p.InternName(StringView(u8"forward"));
        pass.beginTick = p.Frame(0)->beginTick;
        pass.endTick = p.Frame(0)->beginTick + GetTickFrequency() / 1000; // 1 ms
        pass.hasCounts = true;
        pass.counts.fragmentInvocations = 4096;
        p.SubmitGpuPasses(0, Span<const GpuPassEvent>(&pass, 1));
    }

    // The events named `name` with phase `phase`.
    Array<json::JsonValue> Events(const json::JsonValue& trace, StringView name, StringView phase)
    {
        Array<json::JsonValue> out;
        const json::JsonValue events = trace.Get(String(u8"traceEvents"));
        for (i64 i = 0; i < events.Count(); ++i)
        {
            const json::JsonValue e = events.At(i);
            if (e.Get(String(u8"name")).AsString() == String(name) && e.Get(String(u8"ph")).AsString() == String(phase))
            {
                out.PushBack(e);
            }
        }
        return out;
    }

    bool LaneNamed(const json::JsonValue& trace, StringView laneName)
    {
        for (const json::JsonValue& e : Events(trace, StringView(u8"thread_name"), StringView(u8"M")))
        {
            if (e.Get(String(u8"args")).Get(String(u8"name")).AsString() == String(laneName))
            {
                return true;
            }
        }
        return false;
    }
}

TEST_CASE("chrome trace: frames, a lane per thread, the GPU lane and the counters")
{
    Profiler p(DefaultAllocator());
    Record(p);
    LiveProfileSource live(p);
    const String text = WriteChromeTrace(live, 0, 1, StringView(u8"Probe"));
    const json::JsonValue trace = json::JsonValue::Parse(text);
    REQUIRE(trace.Get(String(u8"traceEvents")).IsArray());

    CHECK(LaneNamed(trace, StringView(u8"Frames")));
    CHECK(LaneNamed(trace, StringView(u8"GPU")));
    CHECK(LaneNamed(trace, p.Threads()[0].name.AsView()));

    // A bar for each frame, the first at zero.
    const Array<json::JsonValue> frames = Events(trace, StringView(u8"Frame 0"), StringView(u8"X"));
    REQUIRE(frames.Size() == 1u);
    CHECK(frames[0].Get(String(u8"ts")).AsNumber(-1) == doctest::Approx(0.0));
    CHECK(Events(trace, StringView(u8"Frame 1"), StringView(u8"X")).Size() == 1u);

    // Scopes nest by time on their thread's lane.
    const Array<json::JsonValue> update = Events(trace, StringView(u8"Update"), StringView(u8"X"));
    const Array<json::JsonValue> physics = Events(trace, StringView(u8"Physics"), StringView(u8"X"));
    REQUIRE(update.Size() == 2u);
    REQUIRE(physics.Size() == 2u);
    const f64 outerStart = update[0].Get(String(u8"ts")).AsNumber();
    const f64 innerStart = physics[0].Get(String(u8"ts")).AsNumber();
    CHECK(innerStart >= outerStart);
    CHECK(innerStart + physics[0].Get(String(u8"dur")).AsNumber() <=
          outerStart + update[0].Get(String(u8"dur")).AsNumber() + 0.001);
    CHECK(physics[0].Get(String(u8"tid")).AsNumber() == update[0].Get(String(u8"tid")).AsNumber());

    // The GPU pass with its counts, a millisecond long.
    const Array<json::JsonValue> forward = Events(trace, StringView(u8"forward"), StringView(u8"X"));
    REQUIRE(forward.Size() == 1u);
    CHECK(forward[0].Get(String(u8"dur")).AsNumber() == doctest::Approx(1000.0).epsilon(0.01));
    CHECK(forward[0].Get(String(u8"args")).Get(String(u8"fragment invocations")).AsNumber() == doctest::Approx(4096.0));

    // The counter: a value each frame.
    const Array<json::JsonValue> draws = Events(trace, StringView(u8"Draws"), StringView(u8"C"));
    REQUIRE(draws.Size() == 2u);
    CHECK(draws[1].Get(String(u8"args")).Get(String(u8"value")).AsNumber() == doctest::Approx(8.0));
}

TEST_CASE("chrome trace: a capture read back exports as the live store did")
{
    Profiler p(DefaultAllocator());
    Record(p);
    LiveProfileSource live(p);
    MemoryStream bytes(DefaultAllocator());
    ProfileStreamWriter writer(DefaultAllocator(), bytes);
    StreamHeader header;
    header.tickFrequency = live.TickFrequency();
    REQUIRE(writer.WriteHeader(header));
    for (u64 n = 0; n <= 1; ++n)
    {
        REQUIRE(writer.WriteFrame(live, *live.Frame(n)));
    }
    (void)bytes.Seek(0, SeekOrigin::Begin);
    ProfileCapture capture(DefaultAllocator());
    REQUIRE(capture.Read(bytes));
    CHECK(WriteChromeTrace(capture, 0, 1, StringView(u8"Probe")) == WriteChromeTrace(live, 0, 1, StringView(u8"Probe")));
}
