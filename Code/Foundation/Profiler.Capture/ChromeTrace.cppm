// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Profiler.Capture - :chrome partition.
///
/// A profile source's frames as Chrome trace JSON, the format chrome://tracing and Perfetto open:
/// a "Frames" lane with one bar a frame, a lane per thread with its scopes as complete events
/// (they nest by time), a "GPU" lane with the passes (their pipeline statistics as arguments), and
/// each counter as a counter track. Times are microseconds from the first frame's start.

module;
#include "Core/Prelude.h"

export module foundation.profiler.capture:chrome;

import foundation.core;
import foundation.json;
import foundation.profiler;

using namespace foundation::core;
namespace json = foundation::json;

export namespace foundation::profiler
{
    namespace detail
    {
        inline constexpr f64 kFramesLane = 0;
        inline constexpr f64 kGpuLane = 1000000;

        inline json::JsonValue Event(StringView name, StringView phase, f64 lane)
        {
            json::JsonValue e = json::JsonValue::MakeObject();
            e.Set(String(u8"name"), json::JsonValue::MakeString(String(name)));
            e.Set(String(u8"ph"), json::JsonValue::MakeString(String(phase)));
            e.Set(String(u8"pid"), json::JsonValue::MakeNumber(1));
            e.Set(String(u8"tid"), json::JsonValue::MakeNumber(lane));
            return e;
        }

        inline json::JsonValue LaneName(f64 lane, StringView name, f64 sortIndex)
        {
            json::JsonValue e = Event(StringView(u8"thread_name"), StringView(u8"M"), lane);
            json::JsonValue args = json::JsonValue::MakeObject();
            args.Set(String(u8"name"), json::JsonValue::MakeString(String(name)));
            e.Set(String(u8"args"), Move(args));
            json::JsonValue sort = Event(StringView(u8"thread_sort_index"), StringView(u8"M"), lane);
            json::JsonValue sortArgs = json::JsonValue::MakeObject();
            sortArgs.Set(String(u8"sort_index"), json::JsonValue::MakeNumber(sortIndex));
            sort.Set(String(u8"args"), Move(sortArgs));
            json::JsonValue both = json::JsonValue::MakeArray();
            both.Add(Move(e));
            both.Add(Move(sort));
            return both;
        }

        inline json::JsonValue Complete(StringView name, f64 lane, f64 startUs, f64 durationUs)
        {
            json::JsonValue e = Event(name, StringView(u8"X"), lane);
            e.Set(String(u8"ts"), json::JsonValue::MakeNumber(startUs));
            e.Set(String(u8"dur"), json::JsonValue::MakeNumber(durationUs));
            return e;
        }
    }

    /// Frames `firstFrame`..`lastFrame` of `source` (those it holds) as Chrome trace JSON text.
    /// `title` names the process (the application).
    [[nodiscard]] inline String WriteChromeTrace(const IProfileSource& source, u64 firstFrame, u64 lastFrame,
                                                 StringView title)
    {
        json::JsonValue events = json::JsonValue::MakeArray();
        const f64 frequency = static_cast<f64>(Max<u64>(source.TickFrequency(), 1));
        u64 origin = 0;
        bool haveOrigin = false;
        for (u64 n = firstFrame; n <= lastFrame && !haveOrigin; ++n)
        {
            if (const FrameRecord* frame = source.Frame(n))
            {
                origin = frame->beginTick;
                haveOrigin = true;
            }
        }
        const auto us = [&](u64 tick) { return (static_cast<f64>(tick) - static_cast<f64>(origin)) * 1.0e6 / frequency; };
        const auto lengthUs = [&](u64 begin, u64 end) { return static_cast<f64>(end >= begin ? end - begin : 0) * 1.0e6 / frequency; };

        // Lanes: the frames, each thread (at its index plus one), and the GPU.
        json::JsonValue process = detail::Event(StringView(u8"process_name"), StringView(u8"M"), 0);
        json::JsonValue processArgs = json::JsonValue::MakeObject();
        processArgs.Set(String(u8"name"), json::JsonValue::MakeString(String(title)));
        process.Set(String(u8"args"), Move(processArgs));
        events.Add(Move(process));
        const auto addLane = [&](f64 lane, StringView name, f64 sort)
        {
            json::JsonValue pair = detail::LaneName(lane, name, sort);
            events.Add(pair.At(0));
            events.Add(pair.At(1));
        };
        addLane(detail::kFramesLane, StringView(u8"Frames"), 0);
        for (const ThreadInfo& thread : source.Threads())
        {
            addLane(static_cast<f64>(thread.index) + 1.0, thread.name.AsView(), static_cast<f64>(thread.index) + 1.0);
        }
        addLane(detail::kGpuLane, StringView(u8"GPU"), detail::kGpuLane);

        for (u64 n = firstFrame; n <= lastFrame; ++n)
        {
            const FrameRecord* frame = source.Frame(n);
            if (frame == nullptr)
            {
                continue;
            }
            String frameName;
            AppendFormat(frameName, u8"Frame {}", frame->frameNumber);
            events.Add(detail::Complete(frameName.AsView(), detail::kFramesLane, us(frame->beginTick),
                                    lengthUs(frame->beginTick, frame->endTick)));
            for (const ThreadFrame& thread : frame->threads)
            {
                for (const ScopeEvent& scope : thread.scopes)
                {
                    events.Add(detail::Complete(source.NameOf(scope.name), static_cast<f64>(thread.thread) + 1.0,
                                            us(scope.beginTick), lengthUs(scope.beginTick, scope.endTick)));
                }
            }
            for (const GpuPassEvent& pass : frame->gpu)
            {
                json::JsonValue e = detail::Complete(source.NameOf(pass.name), detail::kGpuLane, us(pass.beginTick),
                                                 lengthUs(pass.beginTick, pass.endTick));
                if (pass.hasCounts)
                {
                    json::JsonValue args = json::JsonValue::MakeObject();
                    args.Set(String(u8"input primitives"), json::JsonValue::MakeNumber(static_cast<f64>(pass.counts.inputPrimitives)));
                    args.Set(String(u8"vertex invocations"), json::JsonValue::MakeNumber(static_cast<f64>(pass.counts.vertexInvocations)));
                    args.Set(String(u8"fragment invocations"), json::JsonValue::MakeNumber(static_cast<f64>(pass.counts.fragmentInvocations)));
                    args.Set(String(u8"compute invocations"), json::JsonValue::MakeNumber(static_cast<f64>(pass.counts.computeInvocations)));
                    e.Set(String(u8"args"), Move(args));
                }
                events.Add(Move(e));
            }
            for (const CounterValue& value : frame->counters)
            {
                if (value.counter >= source.Counters().Size())
                {
                    continue;
                }
                json::JsonValue e = detail::Event(source.Counters()[value.counter].name.AsView(), StringView(u8"C"), 0);
                e.Set(String(u8"ts"), json::JsonValue::MakeNumber(us(frame->beginTick)));
                json::JsonValue args = json::JsonValue::MakeObject();
                args.Set(String(u8"value"), json::JsonValue::MakeNumber(value.value));
                e.Set(String(u8"args"), Move(args));
                events.Add(Move(e));
            }
        }

        json::JsonValue root = json::JsonValue::MakeObject();
        root.Set(String(u8"traceEvents"), Move(events));
        root.Set(String(u8"displayTimeUnit"), json::JsonValue::MakeString(String(u8"ms")));
        return json::Write(root);
    }
}
