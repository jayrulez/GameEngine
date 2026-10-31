// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// profiler_capture: start the recorder, answer NotFinished while it captures (the editor re-enters
// the call each frame), then read the written capture back as a source and summarise it.

module;
#include "Core/Prelude.h"

module editor.app;

import foundation.core;
import foundation.json;
import foundation.mcp;
import foundation.profiler;
import foundation.profiler.capture;
import editor.core;

using namespace foundation::core;
using foundation::json::JsonValue;
namespace profiler = foundation::profiler;
using foundation::mcp::SchemaBuilder;
using foundation::mcp::ToolAnnotations;
using foundation::mcp::ToolCall;
using foundation::mcp::ToolCallState;
using foundation::mcp::ToolOutcome;
using foundation::mcp::ToolResult;

namespace editor::app
{
    namespace
    {
        constexpr f64 kDefaultSeconds = 3.0;
        constexpr f64 kMaxSeconds = 30.0;
        constexpr usize kTopCount = 10;

        // The handler's shared reference keeps this state alive past the server.
        class ProfilerToolState final : public RefCounted
        {
        public:
            explicit ProfilerToolState(ProfilerToolSeams seams) : seams(Move(seams)) {}
            ProfilerToolSeams seams;
        };

        struct Capture final : ToolCallState
        {
            f64 seconds = 0.0;
            String previous; // the recorder's last capture before this one
            Stopwatch clock;
        };

        struct Total
        {
            String name;
            f64 ms = 0.0;
            u64 count = 0;
        };

        void Add(Array<Total>& totals, StringView name, f64 ms)
        {
            for (Total& t : totals)
            {
                if (t.name.AsView() == name)
                {
                    t.ms += ms;
                    ++t.count;
                    return;
                }
            }
            totals.PushBack(Total{String(name), ms, 1});
        }

        JsonValue Top(Array<Total>& totals)
        {
            for (usize i = 0; i < totals.Size(); ++i) // by total, longest first (a short list)
            {
                for (usize j = i + 1; j < totals.Size(); ++j)
                {
                    if (totals[j].ms > totals[i].ms)
                    {
                        const Total t = totals[i];
                        totals[i] = totals[j];
                        totals[j] = t;
                    }
                }
            }
            JsonValue out = JsonValue::MakeArray();
            for (usize i = 0; i < Min(totals.Size(), kTopCount); ++i)
            {
                JsonValue row = JsonValue::MakeObject();
                row.Set(String(u8"name"), JsonValue::MakeString(totals[i].name));
                row.Set(String(u8"totalMs"), JsonValue::MakeNumber(totals[i].ms));
                row.Set(String(u8"count"), JsonValue::MakeNumber(static_cast<f64>(totals[i].count)));
                out.Add(Move(row));
            }
            return out;
        }

        // What an agent reads first: frame times, where the time went (CPU scopes and GPU passes
        // by total over the capture), and each counter's average.
        ToolResult Summary(StringView path)
        {
            FileStream file(path, FileMode::Read);
            profiler::ProfileCapture capture(editor::EditorRootAllocator());
            if (!file.IsValid() || !capture.Read(file) || !capture.HasFrames())
            {
                return Err(Format(u8"the capture at {} could not be read back", path));
            }
            const f64 toMs = 1000.0 / static_cast<f64>(Max<u64>(capture.TickFrequency(), 1));
            IAllocator& allocator = editor::EditorRootAllocator();
            Array<f64> frameMs(allocator);
            Array<Total> scopes(allocator), passes(allocator);
            Array<f64> counterSum(allocator), counterFrames(allocator);
            counterSum.Resize(capture.Counters().Size(), 0.0);
            counterFrames.Resize(capture.Counters().Size(), 0.0);
            for (u64 n = capture.OldestFrame(); n <= capture.NewestFrame(); ++n)
            {
                const profiler::FrameRecord* frame = capture.Frame(n);
                if (frame == nullptr)
                {
                    continue;
                }
                frameMs.PushBack(static_cast<f64>(frame->endTick - frame->beginTick) * toMs);
                for (const profiler::ThreadFrame& thread : frame->threads)
                {
                    for (const profiler::ScopeEvent& scope : thread.scopes)
                    {
                        Add(scopes, capture.NameOf(scope.name), static_cast<f64>(scope.endTick - scope.beginTick) * toMs);
                    }
                }
                for (const profiler::GpuPassEvent& pass : frame->gpu)
                {
                    Add(passes, capture.NameOf(pass.name), static_cast<f64>(pass.endTick - pass.beginTick) * toMs);
                }
                for (const profiler::CounterValue& value : frame->counters)
                {
                    if (value.counter < counterSum.Size())
                    {
                        counterSum[value.counter] += value.value;
                        counterFrames[value.counter] += 1.0;
                    }
                }
            }
            for (usize i = 1; i < frameMs.Size(); ++i) // sorted for the median
            {
                const f64 v = frameMs[i];
                usize j = i;
                while (j > 0 && frameMs[j - 1] > v)
                {
                    frameMs[j] = frameMs[j - 1];
                    --j;
                }
                frameMs[j] = v;
            }
            JsonValue out = JsonValue::MakeObject();
            out.Set(String(u8"path"), JsonValue::MakeString(String(path)));
            String jsonPath(StringView(path.Data(), path.Size() - String(u8".aeprofile").Size()));
            jsonPath.Append(u8".json");
            out.Set(String(u8"chromeTrace"), JsonValue::MakeString(Move(jsonPath)));
            out.Set(String(u8"frames"), JsonValue::MakeNumber(static_cast<f64>(frameMs.Size())));
            JsonValue times = JsonValue::MakeObject();
            if (!frameMs.IsEmpty())
            {
                times.Set(String(u8"min"), JsonValue::MakeNumber(frameMs[0]));
                times.Set(String(u8"median"), JsonValue::MakeNumber(frameMs[frameMs.Size() / 2]));
                times.Set(String(u8"max"), JsonValue::MakeNumber(frameMs[frameMs.Size() - 1]));
            }
            out.Set(String(u8"frameMs"), Move(times));
            out.Set(String(u8"topScopes"), Top(scopes));
            out.Set(String(u8"topGpuPasses"), Top(passes));
            JsonValue counters = JsonValue::MakeObject();
            for (usize i = 0; i < counterSum.Size(); ++i)
            {
                if (counterFrames[i] > 0.0)
                {
                    counters.Set(capture.Counters()[i].name, JsonValue::MakeNumber(counterSum[i] / counterFrames[i]));
                }
            }
            out.Set(String(u8"counterAverages"), Move(counters));
            return out;
        }
    }

    void RegisterProfilerTools(foundation::mcp::McpServer& server, ProfilerToolSeams seams)
    {
        RefPtr<ProfilerToolState> state = MakeRef<ProfilerToolState>(editor::EditorRootAllocator(), Move(seams));
        server.RegisterTool(
            u8"profiler_capture",
            u8"Capture the next few seconds of the running editor (its pages and any Play-in-editor game) "
            u8"with the profiler, and answer a summary: frame times (min, median, max), the CPU scopes and "
            u8"GPU passes that took longest over the capture, and each counter's average (draws, passes, "
            u8"GPU memory). The capture is written as a .aeprofile (the editor's profiler panel opens it) "
            u8"and Chrome trace JSON beside it (Perfetto, chrome://tracing); both paths are in the answer. "
            u8"The call lasts the capture plus a few frames.",
            SchemaBuilder()
                .Number(u8"seconds", u8"How long to capture (default 3, at most 30).")
                .Build(),
            ToolAnnotations::Creates(),
            [state](ToolCall& call, const JsonValue& args) -> ToolOutcome
            {
                const ProfilerToolSeams& seams = state->seams;
                if (Capture* capture = call.State<Capture>())
                {
                    if (seams.recorder->Capturing())
                    {
                        if (capture->clock.Elapsed().AsSeconds() > capture->seconds * 4.0 + 30.0)
                        {
                            return Err(String(u8"the capture did not finish (the editor stopped running frames?)"));
                        }
                        return ToolOutcome::NotFinished();
                    }
                    if (seams.recorder->LastCapture() == capture->previous.AsView())
                    {
                        return Err(String(u8"the capture could not be written (see the log)"));
                    }
                    return Summary(seams.recorder->LastCapture());
                }
#if !BUILD_PROFILING
                return Err(String(u8"the profiler is compiled out of this build (Shipping)"));
#else
                if (seams.recorder->Capturing())
                {
                    return Err(String(u8"a capture is already running; call again when it has finished"));
                }
                const f64 seconds = Clamp(args.Get(String(u8"seconds")).AsNumber(kDefaultSeconds), 0.0, kMaxSeconds);
                auto capture = MakeUnique<Capture>(editor::EditorRootAllocator());
                capture->seconds = seconds;
                capture->previous = String(seams.recorder->LastCapture());
                capture->clock.Start();
                if (!seams.recorder->Start(*seams.profiler, seconds, seams.folder().AsView()))
                {
                    return Err(String(u8"the capture could not start"));
                }
                call.state = Move(capture);
                return ToolOutcome::NotFinished();
#endif
            });
    }
}
