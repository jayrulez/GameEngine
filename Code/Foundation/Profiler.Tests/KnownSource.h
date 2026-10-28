// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// A profile source of hand-made frames on a millisecond clock (a tick is a millisecond), so a
// test's numbers come out exact. Included after the module imports; the viewers' suite
// (Profiler.GUI.Tests) includes it from here.
#pragma once

namespace profiler_tests
{
    using namespace foundation::core;
    using namespace foundation::profiler;

    class KnownSource final : public IProfileSource
    {
    public:
        KnownSource() : m_names(DefaultAllocator()), m_threads(DefaultAllocator()), m_counters(DefaultAllocator()),
                        m_frames(DefaultAllocator())
        {
            ThreadInfo main;
            main.index = 0;
            main.name = String(u8"Main");
            m_threads.PushBack(Move(main));
        }

        NameId Name(StringView name)
        {
            for (usize i = 0; i < m_names.Size(); ++i)
            {
                if (m_names[i].AsView() == name)
                {
                    return static_cast<NameId>(i);
                }
            }
            m_names.PushBack(String(name));
            return static_cast<NameId>(m_names.Size() - 1);
        }

        u32 Counter(StringView name, CounterUnit unit)
        {
            CounterInfo info;
            info.name = String(name);
            info.unit = unit;
            m_counters.PushBack(Move(info));
            return static_cast<u32>(m_counters.Size() - 1);
        }

        /// A frame `cpuMs` long after the last; its scopes and passes added through the result.
        FrameRecord& AddFrame(u64 cpuMs)
        {
            FrameRecord frame(DefaultAllocator());
            frame.frameNumber = m_frames.IsEmpty() ? 0 : m_frames[m_frames.Size() - 1].frameNumber + 1;
            frame.beginTick = m_nextTick;
            frame.endTick = m_nextTick + cpuMs;
            m_nextTick = frame.endTick;
            ThreadFrame thread(DefaultAllocator());
            frame.threads.PushBack(Move(thread));
            m_frames.PushBack(Move(frame));
            return m_frames[m_frames.Size() - 1];
        }

        static void AddScope(FrameRecord& frame, NameId name, u32 depth, u64 begin, u64 end)
        {
            frame.threads[0].scopes.PushBack(ScopeEvent{name, depth, frame.beginTick + begin, frame.beginTick + end});
        }

        /// A second thread's table entry; its scopes go in with AddScopeOn.
        u32 Thread(StringView name)
        {
            ThreadInfo info;
            info.index = static_cast<u32>(m_threads.Size());
            info.name = String(name);
            m_threads.PushBack(Move(info));
            return static_cast<u32>(m_threads.Size() - 1);
        }

        static void AddScopeOn(FrameRecord& frame, u32 thread, NameId name, u32 depth, u64 begin, u64 end)
        {
            ThreadFrame* lane = nullptr;
            for (ThreadFrame& t : frame.threads)
            {
                if (t.thread == thread)
                {
                    lane = &t;
                }
            }
            if (lane == nullptr)
            {
                ThreadFrame added(DefaultAllocator());
                added.thread = thread;
                frame.threads.PushBack(Move(added));
                lane = &frame.threads[frame.threads.Size() - 1];
            }
            lane->scopes.PushBack(ScopeEvent{name, depth, frame.beginTick + begin, frame.beginTick + end});
        }

        static void AddPass(FrameRecord& frame, NameId name, u64 begin, u64 end)
        {
            GpuPassEvent pass;
            pass.name = name;
            pass.beginTick = frame.beginTick + begin;
            pass.endTick = frame.beginTick + end;
            frame.gpu.PushBack(pass);
            frame.gpuValid = true;
        }

        u64 TickFrequency() const noexcept override { return 1000; }
        bool HasFrames() const noexcept override { return !m_frames.IsEmpty(); }
        u64 OldestFrame() const noexcept override { return m_frames.IsEmpty() ? 0 : m_frames[0].frameNumber; }
        u64 NewestFrame() const noexcept override
        {
            return m_frames.IsEmpty() ? 0 : m_frames[m_frames.Size() - 1].frameNumber;
        }
        const FrameRecord* Frame(u64 frameNumber) const noexcept override
        {
            for (const FrameRecord& frame : m_frames)
            {
                if (frame.frameNumber == frameNumber)
                {
                    return &frame;
                }
            }
            return nullptr;
        }
        StringView NameOf(NameId id) const noexcept override
        {
            return id < m_names.Size() ? m_names[id].AsView() : StringView{};
        }
        u32 NameCount() const noexcept override { return static_cast<u32>(m_names.Size()); }
        Span<const ThreadInfo> Threads() const noexcept override { return m_threads.AsSpan(); }
        Span<const CounterInfo> Counters() const noexcept override { return m_counters.AsSpan(); }

    private:
        Array<String> m_names;
        Array<ThreadInfo> m_threads;
        Array<CounterInfo> m_counters;
        Array<FrameRecord> m_frames;
        u64 m_nextTick = 1000;
    };
}
