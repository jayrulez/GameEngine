// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Profiler - :stream partition.
///
/// The one encoding of profiler frames: a capture file (.aeprofile) is a saved stream, and a socket
/// could carry the same bytes. A stream is a header
/// (the recording process: application, platform, build, tick frequency), then chunks, each a tag,
/// a byte length and its payload, so a reader skips a chunk it does not know. The tables a frame's
/// ids point into travel beside it as deltas: names, threads and counters are sent when new, each
/// once. Little-endian, as every target is.

module;
#include "Core/Prelude.h"

export module foundation.profiler:stream;

import foundation.core;
import :store;
import :source;

using namespace foundation::core;

export namespace foundation::profiler
{
    inline constexpr u32 kStreamMagic = 0x46504541u; // "AEPF"
    inline constexpr u32 kStreamVersion = 1;

    enum class StreamChunk : u8
    {
        Names = 1,    // first id, then names in id order
        Threads = 2,  // first index, then (index, name)
        Counters = 3, // first id, then (name, unit)
        Frame = 4,    // one frame record
    };

    /// Who recorded a stream.
    struct StreamHeader
    {
        u64 tickFrequency = 0;
        String application;
        String platform;
        String build;
    };

    /// Writes a stream: the header once, then frames, each preceded by whatever table entries it
    /// needs that were not sent yet.
    class ProfileStreamWriter
    {
    public:
        ProfileStreamWriter(IAllocator& allocator, IStream& out) noexcept : m_allocator(&allocator), m_out(out) {}

        bool WriteHeader(const StreamHeader& header)
        {
            m_out.Write(kStreamMagic);
            m_out.Write(kStreamVersion);
            m_out.Write(header.tickFrequency);
            m_out.WriteString(header.application.AsView());
            m_out.WriteString(header.platform.AsView());
            m_out.WriteString(header.build.AsView());
            return m_out.IsOk();
        }

        /// One frame of `source`, the tables it leans on first.
        bool WriteFrame(const IProfileSource& source, const FrameRecord& frame)
        {
            SendTables(source);
            MemoryStream payload(*m_allocator);
            BinaryWriter w(payload);
            w.Write(frame.frameNumber);
            w.Write(frame.beginTick);
            w.Write(frame.endTick);
            w.Write(static_cast<u32>(frame.threads.Size()));
            for (const ThreadFrame& thread : frame.threads)
            {
                w.Write(thread.thread);
                w.Write(thread.dropped);
                w.Write(static_cast<u32>(thread.scopes.Size()));
                for (const ScopeEvent& scope : thread.scopes)
                {
                    w.Write(scope.name);
                    w.Write(scope.depth);
                    w.Write(scope.beginTick);
                    w.Write(scope.endTick);
                }
            }
            w.Write(static_cast<u8>(frame.gpuValid ? 1 : 0));
            w.Write(static_cast<u32>(frame.gpu.Size()));
            for (const GpuPassEvent& pass : frame.gpu)
            {
                w.Write(pass.name);
                w.Write(pass.depth);
                w.Write(pass.queue);
                w.Write(pass.beginTick);
                w.Write(pass.endTick);
                w.Write(static_cast<u8>(pass.hasCounts ? 1 : 0));
                if (pass.hasCounts)
                {
                    w.Write(pass.counts);
                }
            }
            w.Write(static_cast<u32>(frame.counters.Size()));
            for (const CounterValue& value : frame.counters)
            {
                w.Write(value.counter);
                w.Write(value.value);
            }
            return WriteChunk(StreamChunk::Frame, payload);
        }

        [[nodiscard]] bool IsOk() const noexcept { return m_out.IsOk(); }

    private:
        void SendTables(const IProfileSource& source)
        {
            if (source.NameCount() > m_namesSent)
            {
                MemoryStream payload(*m_allocator);
                BinaryWriter w(payload);
                w.Write(m_namesSent);
                w.Write(source.NameCount() - m_namesSent);
                for (u32 id = m_namesSent; id < source.NameCount(); ++id)
                {
                    w.WriteString(source.NameOf(id));
                }
                WriteChunk(StreamChunk::Names, payload);
                m_namesSent = source.NameCount();
            }
            const Span<const ThreadInfo> threads = source.Threads();
            if (threads.Size() > m_threadsSent)
            {
                MemoryStream payload(*m_allocator);
                BinaryWriter w(payload);
                w.Write(m_threadsSent);
                w.Write(static_cast<u32>(threads.Size()) - m_threadsSent);
                for (usize i = m_threadsSent; i < threads.Size(); ++i)
                {
                    w.Write(threads[i].index);
                    w.WriteString(threads[i].name.AsView());
                }
                WriteChunk(StreamChunk::Threads, payload);
                m_threadsSent = static_cast<u32>(threads.Size());
            }
            const Span<const CounterInfo> counters = source.Counters();
            if (counters.Size() > m_countersSent)
            {
                MemoryStream payload(*m_allocator);
                BinaryWriter w(payload);
                w.Write(m_countersSent);
                w.Write(static_cast<u32>(counters.Size()) - m_countersSent);
                for (usize i = m_countersSent; i < counters.Size(); ++i)
                {
                    w.WriteString(counters[i].name.AsView());
                    w.Write(static_cast<u8>(counters[i].unit));
                }
                WriteChunk(StreamChunk::Counters, payload);
                m_countersSent = static_cast<u32>(counters.Size());
            }
        }

        bool WriteChunk(StreamChunk tag, const MemoryStream& payload)
        {
            const Span<const byte> bytes = payload.Bytes();
            m_out.Write(static_cast<u8>(tag));
            m_out.Write(static_cast<u32>(bytes.Size()));
            m_out.WriteBytes(bytes.Data(), bytes.Size());
            return m_out.IsOk();
        }

        IAllocator* m_allocator;
        BinaryWriter m_out;
        u32 m_namesSent = 0;
        u32 m_threadsSent = 0;
        u32 m_countersSent = 0;
    };

    /// A stream read back: its frames and tables, owned, read as a source.
    class ProfileCapture final : public IProfileSource
    {
    public:
        explicit ProfileCapture(IAllocator& allocator) noexcept
            : m_allocator(&allocator), m_names(allocator), m_threads(allocator), m_counters(allocator),
              m_frames(allocator)
        {
        }

        /// Read a whole stream; false (and a partial capture) on a malformed or foreign one.
        [[nodiscard]] bool Read(IStream& in)
        {
            BinaryReader r(in);
            u32 magic = 0, version = 0;
            r.Read(magic);
            r.Read(version);
            if (!r.IsOk() || magic != kStreamMagic || version != kStreamVersion)
            {
                return false;
            }
            r.Read(m_header.tickFrequency);
            r.ReadString(m_header.application);
            r.ReadString(m_header.platform);
            r.ReadString(m_header.build);
            if (!r.IsOk())
            {
                return false;
            }
            for (;;)
            {
                u8 tag = 0;
                if (in.Read(&tag, 1) != 1)
                {
                    return true; // the end
                }
                u32 length = 0;
                r.Read(length);
                Array<u8> bytes(*m_allocator);
                bytes.Resize(length);
                if (!r.IsOk() || !r.ReadBytes(bytes.Data(), length))
                {
                    return false;
                }
                MemoryStream payload(*m_allocator);
                (void)payload.Write(bytes.Data(), length);
                (void)payload.Seek(0, SeekOrigin::Begin);
                if (!ReadChunk(static_cast<StreamChunk>(tag), payload))
                {
                    return false;
                }
            }
        }

        [[nodiscard]] const StreamHeader& Header() const noexcept { return m_header; }
        [[nodiscard]] usize FrameCount() const noexcept { return m_frames.Size(); }

        u64 TickFrequency() const noexcept override { return m_header.tickFrequency; }
        bool HasFrames() const noexcept override { return !m_frames.IsEmpty(); }
        u64 OldestFrame() const noexcept override { return m_frames.IsEmpty() ? 0 : m_frames[0].frameNumber; }
        u64 NewestFrame() const noexcept override
        {
            return m_frames.IsEmpty() ? 0 : m_frames[m_frames.Size() - 1].frameNumber;
        }
        const FrameRecord* Frame(u64 frameNumber) const noexcept override
        {
            // Frames are in number order: a binary search.
            usize lo = 0, hi = m_frames.Size();
            while (lo < hi)
            {
                const usize mid = lo + (hi - lo) / 2;
                if (m_frames[mid].frameNumber < frameNumber)
                {
                    lo = mid + 1;
                }
                else
                {
                    hi = mid;
                }
            }
            return lo < m_frames.Size() && m_frames[lo].frameNumber == frameNumber ? &m_frames[lo] : nullptr;
        }
        StringView NameOf(NameId id) const noexcept override
        {
            return id < m_names.Size() ? m_names[id].AsView() : StringView();
        }
        u32 NameCount() const noexcept override { return static_cast<u32>(m_names.Size()); }
        Span<const ThreadInfo> Threads() const noexcept override
        {
            return Span<const ThreadInfo>(m_threads.Data(), m_threads.Size());
        }
        Span<const CounterInfo> Counters() const noexcept override
        {
            return Span<const CounterInfo>(m_counters.Data(), m_counters.Size());
        }

    private:
        [[nodiscard]] bool ReadChunk(StreamChunk tag, IStream& payload)
        {
            BinaryReader r(payload);
            switch (tag)
            {
            case StreamChunk::Names:
            {
                u32 first = 0, count = 0;
                r.Read(first);
                r.Read(count);
                if (!r.IsOk() || first != m_names.Size())
                {
                    return false;
                }
                for (u32 i = 0; i < count && r.IsOk(); ++i)
                {
                    String name(*m_allocator);
                    r.ReadString(name);
                    m_names.PushBack(Move(name));
                }
                return r.IsOk();
            }
            case StreamChunk::Threads:
            {
                u32 first = 0, count = 0;
                r.Read(first);
                r.Read(count);
                if (!r.IsOk() || first != m_threads.Size())
                {
                    return false;
                }
                for (u32 i = 0; i < count && r.IsOk(); ++i)
                {
                    ThreadInfo info;
                    info.name = String(*m_allocator);
                    r.Read(info.index);
                    r.ReadString(info.name);
                    m_threads.PushBack(Move(info));
                }
                return r.IsOk();
            }
            case StreamChunk::Counters:
            {
                u32 first = 0, count = 0;
                r.Read(first);
                r.Read(count);
                if (!r.IsOk() || first != m_counters.Size())
                {
                    return false;
                }
                for (u32 i = 0; i < count && r.IsOk(); ++i)
                {
                    CounterInfo info;
                    info.name = String(*m_allocator);
                    u8 unit = 0;
                    r.ReadString(info.name);
                    r.Read(unit);
                    info.unit = static_cast<CounterUnit>(unit);
                    m_counters.PushBack(Move(info));
                }
                return r.IsOk();
            }
            case StreamChunk::Frame:
                return ReadFrame(r);
            }
            return true; // a chunk this reader does not know: skipped
        }

        [[nodiscard]] bool ReadFrame(BinaryReader& r)
        {
            FrameRecord frame(*m_allocator);
            r.Read(frame.frameNumber);
            r.Read(frame.beginTick);
            r.Read(frame.endTick);
            u32 threadCount = 0;
            r.Read(threadCount);
            for (u32 t = 0; t < threadCount && r.IsOk(); ++t)
            {
                ThreadFrame thread(*m_allocator);
                u32 scopeCount = 0;
                r.Read(thread.thread);
                r.Read(thread.dropped);
                r.Read(scopeCount);
                for (u32 i = 0; i < scopeCount && r.IsOk(); ++i)
                {
                    ScopeEvent scope;
                    r.Read(scope.name);
                    r.Read(scope.depth);
                    r.Read(scope.beginTick);
                    r.Read(scope.endTick);
                    thread.scopes.PushBack(scope);
                }
                frame.threads.PushBack(Move(thread));
            }
            u8 gpuValid = 0;
            u32 gpuCount = 0;
            r.Read(gpuValid);
            r.Read(gpuCount);
            frame.gpuValid = gpuValid != 0;
            for (u32 i = 0; i < gpuCount && r.IsOk(); ++i)
            {
                GpuPassEvent pass;
                u8 hasCounts = 0;
                r.Read(pass.name);
                r.Read(pass.depth);
                r.Read(pass.queue);
                r.Read(pass.beginTick);
                r.Read(pass.endTick);
                r.Read(hasCounts);
                pass.hasCounts = hasCounts != 0;
                if (pass.hasCounts)
                {
                    r.Read(pass.counts);
                }
                frame.gpu.PushBack(pass);
            }
            u32 counterCount = 0;
            r.Read(counterCount);
            for (u32 i = 0; i < counterCount && r.IsOk(); ++i)
            {
                CounterValue value;
                r.Read(value.counter);
                r.Read(value.value);
                frame.counters.PushBack(value);
            }
            if (!r.IsOk() || (!m_frames.IsEmpty() && frame.frameNumber <= m_frames[m_frames.Size() - 1].frameNumber))
            {
                return false; // frames come in number order
            }
            m_frames.PushBack(Move(frame));
            return true;
        }

        IAllocator* m_allocator;
        StreamHeader m_header;
        Array<String> m_names;
        Array<ThreadInfo> m_threads;
        Array<CounterInfo> m_counters;
        Array<FrameRecord> m_frames;
    };
}
