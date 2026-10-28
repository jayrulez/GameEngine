// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Profiler - :recording partition.
///
/// A viewer's own copy of frames (Specs/profiler.md, P5): the editor's panel records what the
/// store keeps into one of these, so its pause, its longer history and its selection never touch
/// the store another viewer reads (a game's overlay). Frames are copied once their GPU passes
/// have had time to arrive (`settleFrames` behind the newest), with the name, thread and counter
/// tables they point into; the oldest go when it is full. It is a source, so the views read it
/// and a capture file is written from it like any other.

module;
#include "Core/Prelude.h"

export module foundation.profiler:recording;

import foundation.core;
import :store;
import :source;

using namespace foundation::core;

export namespace foundation::profiler
{
    /// ProfileRecording::kSettleFrames, at namespace scope for another library to read (MSVC exports no class's
    /// static constexpr member from a shared library).
    inline constexpr u32 kProfileRecordingSettleFrames = 4;

    class ProfileRecording final : public IProfileSource
    {
    public:
        static constexpr u32 kDefaultCapacity = 3600; // a minute at 60 fps
        static constexpr u32 kSettleFrames = kProfileRecordingSettleFrames;       // frames in flight before a frame's GPU passes land

        explicit ProfileRecording(IAllocator& allocator, u32 capacity = kDefaultCapacity)
            : m_allocator(&allocator), m_frames(allocator), m_names(allocator), m_threads(allocator),
              m_counters(allocator)
        {
            m_capacity = Max<u32>(capacity, 1);
        }

        /// Start afresh from `source`: frames after its newest now are recorded, none before.
        void Begin(const IProfileSource& source)
        {
            Clear();
            m_tickFrequency = source.TickFrequency();
            m_next = source.HasFrames() ? source.NewestFrame() + 1 : 0;
            m_started = true;
        }

        /// Copy the frames `source` has settled since the last pull (`settleFrames` behind its
        /// newest, so their GPU passes are in; 0 when stopping, taking the rest as they are).
        void Pull(const IProfileSource& source, u32 settleFrames = kSettleFrames)
        {
            if (!m_started || !source.HasFrames() || source.NewestFrame() < settleFrames)
            {
                return;
            }
            const u64 last = source.NewestFrame() - settleFrames;
            if (m_next < source.OldestFrame())
            {
                m_next = source.OldestFrame(); // fell behind the store's history: what is left
            }
            if (m_next > last)
            {
                return;
            }
            SyncTables(source);
            for (u64 n = m_next; n <= last; ++n)
            {
                if (const FrameRecord* frame = source.Frame(n))
                {
                    Append(*frame);
                }
            }
            m_next = last + 1;
        }

        /// Drop every frame (the tables stay: ids keep their meaning).
        void Clear()
        {
            m_count = 0;
            m_head = 0;
        }

        [[nodiscard]] u32 Capacity() const noexcept { return m_capacity; }
        [[nodiscard]] u32 FrameCount() const noexcept { return m_count; }

        u64 TickFrequency() const noexcept override { return m_tickFrequency; }
        bool HasFrames() const noexcept override { return m_count > 0; }
        u64 OldestFrame() const noexcept override { return m_count > 0 ? At(0).frameNumber : 0; }
        u64 NewestFrame() const noexcept override { return m_count > 0 ? At(m_count - 1).frameNumber : 0; }
        const FrameRecord* Frame(u64 frameNumber) const noexcept override
        {
            // Frames are kept in order, mostly consecutive: look where it would be, else search.
            if (m_count == 0 || frameNumber < OldestFrame() || frameNumber > NewestFrame())
            {
                return nullptr;
            }
            const u64 guess = frameNumber - OldestFrame();
            if (guess < m_count && At(static_cast<u32>(guess)).frameNumber == frameNumber)
            {
                return &At(static_cast<u32>(guess));
            }
            u32 low = 0;
            u32 high = m_count;
            while (low < high)
            {
                const u32 mid = (low + high) / 2;
                if (At(mid).frameNumber < frameNumber)
                {
                    low = mid + 1;
                }
                else
                {
                    high = mid;
                }
            }
            return low < m_count && At(low).frameNumber == frameNumber ? &At(low) : nullptr;
        }
        StringView NameOf(NameId id) const noexcept override
        {
            return id < m_names.Size() ? m_names[id].AsView() : StringView{};
        }
        u32 NameCount() const noexcept override { return static_cast<u32>(m_names.Size()); }
        Span<const ThreadInfo> Threads() const noexcept override { return m_threads.AsSpan(); }
        Span<const CounterInfo> Counters() const noexcept override { return m_counters.AsSpan(); }

    private:
        [[nodiscard]] const FrameRecord& At(u32 index) const noexcept
        {
            return m_frames[(m_head + index) % m_frames.Size()];
        }

        // The tables only grow in a source, so what is new is what lies past ours.
        void SyncTables(const IProfileSource& source)
        {
            for (u32 id = static_cast<u32>(m_names.Size()); id < source.NameCount(); ++id)
            {
                m_names.PushBack(String(source.NameOf(id), *m_allocator));
            }
            for (usize i = m_threads.Size(); i < source.Threads().Size(); ++i)
            {
                ThreadInfo info;
                info.index = source.Threads()[i].index;
                info.name = String(source.Threads()[i].name.AsView(), *m_allocator);
                m_threads.PushBack(Move(info));
            }
            for (usize i = m_counters.Size(); i < source.Counters().Size(); ++i)
            {
                CounterInfo info;
                info.name = String(source.Counters()[i].name.AsView(), *m_allocator);
                info.unit = source.Counters()[i].unit;
                m_counters.PushBack(Move(info));
            }
        }

        // Into the next slot, reusing its arrays' memory once the ring has filled.
        void Append(const FrameRecord& frame)
        {
            FrameRecord* slot = nullptr;
            if (m_count < m_frames.Size())
            {
                slot = &m_frames[(m_head + m_count) % m_frames.Size()]; // a slot a Clear freed
                ++m_count;
            }
            else if (m_frames.Size() < m_capacity)
            {
                m_frames.PushBack(FrameRecord(*m_allocator));
                slot = &m_frames[m_frames.Size() - 1];
                ++m_count;
            }
            else
            {
                slot = &m_frames[m_head]; // the oldest goes
                m_head = (m_head + 1) % static_cast<u32>(m_frames.Size());
            }
            slot->frameNumber = frame.frameNumber;
            slot->beginTick = frame.beginTick;
            slot->endTick = frame.endTick;
            slot->gpuValid = frame.gpuValid;
            slot->threads.Clear();
            for (const ThreadFrame& thread : frame.threads)
            {
                ThreadFrame copy(*m_allocator);
                copy.thread = thread.thread;
                copy.dropped = thread.dropped;
                for (const ScopeEvent& scope : thread.scopes)
                {
                    copy.scopes.PushBack(scope);
                }
                slot->threads.PushBack(Move(copy));
            }
            slot->gpu.Clear();
            for (const GpuPassEvent& pass : frame.gpu)
            {
                slot->gpu.PushBack(pass);
            }
            slot->counters.Clear();
            for (const CounterValue& value : frame.counters)
            {
                slot->counters.PushBack(value);
            }
        }

        IAllocator* m_allocator;
        Array<FrameRecord> m_frames; // a ring once full: m_head is the oldest
        Array<String> m_names;
        Array<ThreadInfo> m_threads;
        Array<CounterInfo> m_counters;
        u32 m_capacity = kDefaultCapacity;
        u32 m_count = 0;
        u32 m_head = 0;
        u64 m_next = 0;
        u64 m_tickFrequency = 1;
        bool m_started = false;
    };
}
