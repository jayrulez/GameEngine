// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Profiler - `foundation.profiler`, :store partition.
///
/// The capture store every profiler view reads. Threads record the scopes they finish into rings
/// of their own, without a lock; at the end of each frame the frame thread gathers them into a
/// frame record and keeps the last few seconds of records. A record refers to
/// nothing in the process (names are ids into a table, threads are indices), so the same record
/// can be saved to a capture file or sent to another process and mean the same there.
///
/// Recording costs a branch until something arms the profiler (a view, a capture, the P key); arms
/// are counted, so two watchers keep it on until both let go. Instrument with PROFILE_SCOPE
/// (Profiler.h), which compiles to nothing when BUILD_PROFILING is off (Shipping).
///
/// Threads: any thread may record scopes and intern names. Everything else (frames, counters, GPU
/// passes, reading records and tables) belongs to the frame thread, the one calling BeginFrame.

module;
#include "Core/Prelude.h"

export module foundation.profiler:store;

import foundation.core;

using namespace foundation::core;

export namespace foundation::profiler
{
    /// A scope's or pass's name, an index into the profiler's name table.
    using NameId = u32;

    /// One scope a thread finished: its name, how deeply it nested on that thread, and its ticks
    /// (core::GetTicks).
    struct ScopeEvent
    {
        NameId name = 0;
        u32 depth = 0;
        u64 beginTick = 0;
        u64 endTick = 0;
    };

    /// What the GPU counted while a pass ran (pipeline statistics), where the device counts.
    struct GpuPassCounts
    {
        u64 inputVertices = 0;
        u64 inputPrimitives = 0;
        u64 vertexInvocations = 0;
        u64 clippingPrimitives = 0;
        u64 fragmentInvocations = 0;
        u64 computeInvocations = 0;
    };

    /// One GPU pass of a frame, its ticks already on the CPU clock.
    struct GpuPassEvent
    {
        NameId name = 0;
        u32 depth = 0;
        u32 queue = 0; // 0 graphics, 1 async compute
        u64 beginTick = 0;
        u64 endTick = 0;
        bool hasCounts = false; // the device counted pipeline statistics
        GpuPassCounts counts;
    };

    enum class CounterUnit : u8
    {
        Count,
        Bytes,
        Milliseconds,
        Percent,
    };

    struct CounterValue
    {
        u32 counter = 0;
        f64 value = 0.0;
    };

    /// The scopes one thread finished during a frame, and how many it dropped (its ring was full).
    struct ThreadFrame
    {
        u32 thread = 0;
        u32 dropped = 0;
        Array<ScopeEvent> scopes;

        explicit ThreadFrame(IAllocator& allocator) noexcept : scopes(allocator) {}
    };

    /// One frame: when it ran, what every thread did in it, its GPU passes once they arrive (a frame
    /// or two late), and its counters.
    struct FrameRecord
    {
        u64 frameNumber = 0;
        u64 beginTick = 0;
        u64 endTick = 0;
        Array<ThreadFrame> threads;
        bool gpuValid = false;
        Array<GpuPassEvent> gpu;
        Array<CounterValue> counters;

        explicit FrameRecord(IAllocator& allocator) noexcept
            : threads(allocator), gpu(allocator), counters(allocator)
        {
        }

        [[nodiscard]] f64 Ms() const noexcept
        {
            return TicksToMilliseconds(endTick >= beginTick ? endTick - beginTick : 0);
        }
    };

    struct ThreadInfo
    {
        u32 index = 0;
        String name;
    };

    struct CounterInfo
    {
        String name;
        CounterUnit unit = CounterUnit::Count;
    };

    class Profiler
    {
    public:
        static constexpr u32 kDefaultHistory = 300;           // about five seconds at 60 fps
        static constexpr u32 kDefaultFrameThreadRing = 65536; // scopes a frame thread can hold unread
        static constexpr u32 kDefaultThreadRing = 16384;      // and any other thread

        /// The process's profiler, which PROFILE_SCOPE records into. Defined out of line
        /// (ProfilerImpl.cpp): an in-class body would give each shared library its own.
        [[nodiscard]] static Profiler& Get() noexcept;

        /// A profiler of its own (tests). Its memory comes from `allocator`.
        explicit Profiler(IAllocator& allocator) noexcept;
        ~Profiler();
        Profiler(const Profiler&) = delete;
        Profiler& operator=(const Profiler&) = delete;

        // ---- arming -------------------------------------------------------------------------

        /// Start recording; each Arm is undone by one Disarm.
        void Arm() noexcept { m_armCount.fetch_add(1, MemoryOrder::AcqRel); }
        void Disarm() noexcept
        {
            u32 count = m_armCount.load(MemoryOrder::Acquire);
            while (count > 0 &&
                   !m_armCount.compare_exchange_weak(count, count - 1, MemoryOrder::AcqRel))
            {
            }
        }
        [[nodiscard]] bool Armed() const noexcept { return m_armCount.load(MemoryOrder::Acquire) > 0; }

        /// How many scopes a thread can hold between two frame ends before it drops new ones. Read
        /// when a thread first records, so set it before arming.
        void SetRingCapacity(u32 frameThread, u32 otherThreads) noexcept
        {
            m_frameThreadRing = RoundUpPow2(frameThread);
            m_threadRing = RoundUpPow2(otherThreads);
        }

        // ---- scopes (any thread) -------------------------------------------------------------

        /// Open a scope named by a string literal (it is remembered by address). False when
        /// nothing was opened (unarmed); then do not EndScope.
        bool BeginScope(const char* literal) noexcept
        {
            if (!Armed())
            {
                return false;
            }
            ThreadData& td = Local();
            NameId id = 0;
            if (const NameId* known = td.literalIds.Find(literal))
            {
                id = *known;
            }
            else
            {
                id = InternName(StringView(reinterpret_cast<const char8_t*>(literal)));
                td.literalIds.InsertOrAssign(literal, id);
            }
            td.stack.PushBack(OpenScope{id, GetTicks()});
            return true;
        }

        /// Open a scope with a name made at run time (a pass's); it is interned each call.
        bool BeginScope(StringView name) noexcept
        {
            if (!Armed())
            {
                return false;
            }
            ThreadData& td = Local();
            td.stack.PushBack(OpenScope{InternName(name), GetTicks()});
            return true;
        }

        /// Open a scope by a name already interned (InternName), which a caller naming the same
        /// scopes every frame keeps.
        bool BeginScope(NameId name) noexcept
        {
            if (!Armed())
            {
                return false;
            }
            Local().stack.PushBack(OpenScope{name, GetTicks()});
            return true;
        }

        /// Close the scope this thread opened last.
        void EndScope() noexcept
        {
            ThreadData& td = Local();
            if (td.stack.IsEmpty())
            {
                return;
            }
            const OpenScope open = td.stack[td.stack.Size() - 1];
            td.stack.PopBack();
            const u64 written = td.written.load(MemoryOrder::Relaxed);
            if (written - td.read.load(MemoryOrder::Acquire) >= td.ring.Size())
            {
                td.dropped.fetch_add(1, MemoryOrder::Relaxed);
                return;
            }
            td.ring[static_cast<usize>(written & (td.ring.Size() - 1))] =
                ScopeEvent{open.name, static_cast<u32>(td.stack.Size()), open.beginTick, GetTicks()};
            td.written.store(written + 1, MemoryOrder::Release);
        }

        /// The id of `name`, added to the table if new. Any thread: a name another thread adds
        /// joins the table the frame thread reads at the next frame end; one the frame thread adds
        /// (a GPU pass's) joins it at once.
        [[nodiscard]] NameId InternName(StringView name)
        {
            ScopedLock lock(m_mutex);
            const String key(name, *m_allocator);
            if (const NameId* known = m_nameIds.Find(key))
            {
                return *known;
            }
            const NameId id = static_cast<NameId>(m_names.Size() + m_pendingNames.Size());
            m_pendingNames.PushBack(key);
            m_nameIds.InsertOrAssign(key, id);
            if (Thread::CurrentId() == m_frameThreadId.load(MemoryOrder::Relaxed))
            {
                PublishPendingNames();
            }
            return id;
        }

        // ---- frames (the frame thread) -------------------------------------------------------

        void BeginFrame() noexcept
        {
            m_frameBeginTick = GetTicks();
            m_frameThreadId.store(Thread::CurrentId(), MemoryOrder::Relaxed);
        }

        /// Close the frame: gather what every thread finished into its record (when armed), publish
        /// new names and threads, and move on to the next frame number.
        void EndFrame()
        {
            const u64 endTick = GetTicks();
            Array<ThreadData*> threads(*m_allocator);
            {
                ScopedLock lock(m_mutex);
                PublishPendingNames();
                for (ThreadData* td : m_threads)
                {
                    threads.PushBack(td);
                    if (td->index >= m_threadInfos.Size())
                    {
                        m_threadInfos.PushBack(ThreadInfo{td->index, td->name});
                    }
                }
            }

            FrameRecord* record = nullptr;
            if (Armed())
            {
                record = &m_history[static_cast<usize>(m_frameNumber % m_history.Size())];
                record->frameNumber = m_frameNumber;
                record->beginTick = m_frameBeginTick;
                record->endTick = endTick;
                record->threads.Clear();
                record->gpuValid = false;
                record->gpu.Clear();
                record->counters.Clear();
                ++m_recordedFrames;
                m_latestRecorded = m_frameNumber;
            }
            for (ThreadData* td : threads)
            {
                const u64 written = td->written.load(MemoryOrder::Acquire);
                const u64 read = td->read.load(MemoryOrder::Relaxed);
                const u32 dropped = td->dropped.exchange(0, MemoryOrder::Relaxed);
                if (record != nullptr && (written > read || dropped > 0))
                {
                    ThreadFrame frame(*m_allocator);
                    frame.thread = td->index;
                    frame.dropped = dropped;
                    for (u64 i = read; i < written; ++i)
                    {
                        frame.scopes.PushBack(td->ring[static_cast<usize>(i & (td->ring.Size() - 1))]);
                    }
                    record->threads.PushBack(Move(frame));
                }
                td->read.store(written, MemoryOrder::Release); // unarmed: what was left is dropped
            }
            if (record != nullptr)
            {
                for (u32 id = 0; id < m_counterSet.Size(); ++id)
                {
                    if (m_counterSet[id])
                    {
                        record->counters.PushBack(CounterValue{id, m_counterValues[id]});
                        m_counterSet[id] = false;
                    }
                }
            }
            PushFrameTime(TicksToMilliseconds(endTick >= m_frameBeginTick ? endTick - m_frameBeginTick : 0));
            ++m_frameNumber;
        }

        /// The number the frame now running will have.
        [[nodiscard]] u64 CurrentFrameNumber() const noexcept { return m_frameNumber; }

        /// Keep the last `frames` records (at least one); forgets the ones kept so far.
        void SetHistoryLength(u32 frames)
        {
            const u32 count = frames > 0 ? frames : 1;
            m_history.Clear();
            for (u32 i = 0; i < count; ++i)
            {
                m_history.PushBack(FrameRecord(*m_allocator));
            }
            m_recordedFrames = 0;
        }
        [[nodiscard]] u32 HistoryLength() const noexcept { return static_cast<u32>(m_history.Size()); }

        /// Frame `frameNumber`'s record while it is still kept, else null.
        [[nodiscard]] const FrameRecord* Frame(u64 frameNumber) const noexcept
        {
            if (m_recordedFrames == 0)
            {
                return nullptr;
            }
            const FrameRecord& record = m_history[static_cast<usize>(frameNumber % m_history.Size())];
            return record.frameNumber == frameNumber && record.endTick != 0 ? &record : nullptr;
        }
        /// The newest record, null when nothing was recorded.
        [[nodiscard]] const FrameRecord* LatestFrame() const noexcept
        {
            return m_recordedFrames == 0 ? nullptr : Frame(m_latestRecorded);
        }
        /// Whether any frame was recorded; then the frames kept lie between OldestKeptFrame and
        /// LatestRecordedFrame (an unarmed frame between them has no record).
        [[nodiscard]] bool HasFrames() const noexcept { return m_recordedFrames > 0; }
        [[nodiscard]] u64 LatestRecordedFrame() const noexcept { return m_latestRecorded; }
        [[nodiscard]] u64 OldestKeptFrame() const noexcept
        {
            const u64 size = m_history.Size();
            return m_latestRecorded + 1 > size ? m_latestRecorded + 1 - size : 0;
        }

        // ---- GPU passes (the frame thread) ---------------------------------------------------

        /// Add GPU passes to frame `frameNumber`'s record when they arrive (each render graph of
        /// the frame sends its own); ignored once the record is gone.
        void SubmitGpuPasses(u64 frameNumber, Span<const GpuPassEvent> passes)
        {
            if (m_recordedFrames == 0)
            {
                return;
            }
            FrameRecord& record = m_history[static_cast<usize>(frameNumber % m_history.Size())];
            if (record.frameNumber != frameNumber || record.endTick == 0)
            {
                return;
            }
            for (const GpuPassEvent& pass : passes)
            {
                record.gpu.PushBack(pass);
            }
            record.gpuValid = true;
        }

        // ---- counters (the frame thread) -----------------------------------------------------

        /// A counter's id, registered once under its name.
        [[nodiscard]] u32 RegisterCounter(StringView name, CounterUnit unit)
        {
            for (u32 id = 0; id < m_counters.Size(); ++id)
            {
                if (m_counters[id].name.AsView() == name)
                {
                    return id;
                }
            }
            m_counters.PushBack(CounterInfo{String(name, *m_allocator), unit});
            m_counterValues.PushBack(0.0);
            m_counterSet.PushBack(false);
            return static_cast<u32>(m_counters.Size() - 1);
        }
        /// This frame's value of counter `id`; the frame's record takes the last one set.
        void SetCounter(u32 id, f64 value) noexcept
        {
            if (id < m_counterValues.Size())
            {
                m_counterValues[id] = value;
                m_counterSet[id] = true;
            }
        }

        // ---- tables (the frame thread; as of the last frame end) -----------------------------

        [[nodiscard]] StringView NameOf(NameId id) const noexcept
        {
            return id < m_names.Size() ? m_names[id].AsView() : StringView();
        }
        [[nodiscard]] u32 NameCount() const noexcept { return static_cast<u32>(m_names.Size()); }
        [[nodiscard]] Span<const ThreadInfo> Threads() const noexcept
        {
            return Span<const ThreadInfo>(m_threadInfos.Data(), m_threadInfos.Size());
        }
        [[nodiscard]] Span<const CounterInfo> Counters() const noexcept
        {
            return Span<const CounterInfo>(m_counters.Data(), m_counters.Size());
        }

        // ---- summaries -----------------------------------------------------------------------

        /// The mean of the last 64 frame times (ms), armed or not.
        [[nodiscard]] f64 AverageFrameMs() const noexcept
        {
            if (m_frameTimeCount == 0)
            {
                return 0.0;
            }
            f64 sum = 0.0;
            for (u32 i = 0; i < m_frameTimeCount; ++i)
            {
                sum += m_frameTimes[i];
            }
            return sum / static_cast<f64>(m_frameTimeCount);
        }

        /// The newest record as text (the P key): its time, then each thread's scopes in the order
        /// they began, indented by depth, then the GPU passes.
        [[nodiscard]] String BuildReport() const
        {
            String out(*m_allocator);
            const FrameRecord* frame = LatestFrame();
            if (frame == nullptr)
            {
                out.Append(Armed() ? u8"=== Profile: no frame recorded yet ===\n"
                                   : u8"=== Profile: not armed ===\n");
                return out;
            }
            usize scopes = 0;
            for (const ThreadFrame& thread : frame->threads)
            {
                scopes += thread.scopes.Size();
            }
            AppendFormat(out, u8"=== Profile: frame {}  {} ms (avg {} ms)  {} scopes ===\n",
                         frame->frameNumber, frame->Ms(), AverageFrameMs(), scopes);
            for (const ThreadFrame& thread : frame->threads)
            {
                const StringView threadName =
                    thread.thread < m_threadInfos.Size() ? m_threadInfos[thread.thread].name.AsView()
                                                         : StringView(u8"?");
                AppendFormat(out, u8"  [{}]", threadName);
                if (thread.dropped > 0)
                {
                    AppendFormat(out, u8" ({} dropped)", thread.dropped);
                }
                out.Append(u8"\n");
                Array<u32> order(*m_allocator);
                for (u32 i = 0; i < thread.scopes.Size(); ++i)
                {
                    order.PushBack(i);
                }
                // Parents begin before their children; a stable insertion sort keeps equal ticks
                // in the order they finished (children first), so ties put the deeper first.
                for (u32 i = 1; i < order.Size(); ++i)
                {
                    const u32 v = order[i];
                    u32 j = i;
                    while (j > 0 && Before(thread.scopes[v], thread.scopes[order[j - 1]]))
                    {
                        order[j] = order[j - 1];
                        --j;
                    }
                    order[j] = v;
                }
                for (u32 index : order)
                {
                    const ScopeEvent& scope = thread.scopes[index];
                    out.Append(u8"    ");
                    for (u32 d = 0; d < scope.depth; ++d)
                    {
                        out.Append(u8"  ");
                    }
                    AppendFormat(out, u8"{}: {} ms\n", NameOf(scope.name),
                                 TicksToMilliseconds(scope.endTick - scope.beginTick));
                }
            }
            if (frame->gpuValid)
            {
                out.Append(u8"  [GPU]\n");
                for (const GpuPassEvent& pass : frame->gpu)
                {
                    AppendFormat(out, u8"    {}: {} ms\n", NameOf(pass.name),
                                 TicksToMilliseconds(pass.endTick - pass.beginTick));
                }
            }
            return out;
        }

    private:
        struct OpenScope
        {
            NameId name;
            u64 beginTick;
        };

        // One recording thread. The ring is single-producer (its thread) single-consumer (the
        // frame thread at frame end): `written` and `read` count events ever, the ring indexes
        // them modulo its power-of-two size.
        struct ThreadData
        {
            u32 index = 0;
            u64 threadId = 0;
            String name;
            Array<ScopeEvent> ring;
            Atomic<u64> written{0};
            Atomic<u64> read{0};
            Atomic<u32> dropped{0};
            Array<OpenScope> stack;              // its thread only
            HashMap<const char*, NameId> literalIds; // its thread only

            explicit ThreadData(IAllocator& allocator) noexcept
                : name(allocator), ring(allocator), stack(allocator), literalIds(allocator)
            {
            }
        };

        // The calling thread's data for this profiler: a per-thread cache of the last profiler it
        // recorded into (by serial, so a profiler made where an old one was is not mistaken for
        // it), else found or made under the lock.
        struct LocalCache
        {
            u64 serial = 0;
            ThreadData* data = nullptr;
        };
        [[nodiscard]] static LocalCache& LocalSlot() noexcept; // ProfilerImpl.cpp, one per process
        [[nodiscard]] static u64 NextSerial() noexcept;        // ProfilerImpl.cpp

        [[nodiscard]] ThreadData& Local()
        {
            LocalCache& cache = LocalSlot();
            if (cache.serial == m_serial && cache.data != nullptr)
            {
                return *cache.data;
            }
            const u64 threadId = Thread::CurrentId();
            ScopedLock lock(m_mutex);
            ThreadData* found = nullptr;
            for (ThreadData* td : m_threads)
            {
                if (td->threadId == threadId)
                {
                    found = td;
                    break;
                }
            }
            if (found == nullptr)
            {
                found = m_allocator->New<ThreadData>(*m_allocator);
                found->index = static_cast<u32>(m_threads.Size());
                found->threadId = threadId;
                const StringView named = CurrentThreadName();
                if (named.IsEmpty())
                {
                    AppendFormat(found->name, u8"Thread {}", found->index);
                }
                else
                {
                    found->name = String(named, *m_allocator);
                }
                const u32 capacity = threadId == m_frameThreadId.load(MemoryOrder::Relaxed)
                                         ? m_frameThreadRing
                                         : m_threadRing;
                found->ring.Resize(capacity);
                m_threads.PushBack(found);
            }
            cache.serial = m_serial;
            cache.data = found;
            return *found;
        }

        // Under the lock, on the frame thread: pending names join the table it reads, in id order.
        void PublishPendingNames()
        {
            for (String& name : m_pendingNames)
            {
                m_names.PushBack(Move(name));
            }
            m_pendingNames.Clear();
        }

        [[nodiscard]] static bool Before(const ScopeEvent& a, const ScopeEvent& b) noexcept
        {
            return a.beginTick < b.beginTick || (a.beginTick == b.beginTick && a.depth < b.depth);
        }

        [[nodiscard]] static u32 RoundUpPow2(u32 value) noexcept
        {
            u32 result = 1;
            while (result < value && result < (1u << 30))
            {
                result <<= 1;
            }
            return result;
        }

        void PushFrameTime(f64 ms) noexcept
        {
            m_frameTimes[m_frameTimeHead] = ms;
            m_frameTimeHead = (m_frameTimeHead + 1) % kFrameTimes;
            if (m_frameTimeCount < kFrameTimes)
            {
                ++m_frameTimeCount;
            }
        }

        static constexpr u32 kFrameTimes = 64;

        IAllocator* m_allocator;
        u64 m_serial;
        Atomic<u32> m_armCount{0};
        u32 m_frameThreadRing = kDefaultFrameThreadRing;
        u32 m_threadRing = kDefaultThreadRing;

        Mutex m_mutex; // the thread registry, the pending names, the name lookup
        Array<ThreadData*> m_threads;
        Array<String> m_pendingNames;
        HashMap<String, NameId> m_nameIds;

        // The frame thread's view, published at frame end.
        Array<String> m_names;
        Array<ThreadInfo> m_threadInfos;

        u64 m_frameNumber = 0;
        u64 m_frameBeginTick = 0;
        Atomic<u64> m_frameThreadId{0}; // read by recording threads
        Array<FrameRecord> m_history;
        u64 m_recordedFrames = 0;
        u64 m_latestRecorded = 0;

        Array<CounterInfo> m_counters;
        Array<f64> m_counterValues;
        Array<bool> m_counterSet;

        f64 m_frameTimes[kFrameTimes] = {};
        u32 m_frameTimeHead = 0;
        u32 m_frameTimeCount = 0;
    };

    /// Brackets a profiled region; use PROFILE_SCOPE (Profiler.h). Closes only what it opened, so
    /// arming or disarming inside a scope leaves the thread's nesting balanced.
    class ScopedProfile
    {
    public:
        explicit ScopedProfile(const char* name) noexcept : m_began(Profiler::Get().BeginScope(name)) {}
        ~ScopedProfile() noexcept
        {
            if (m_began)
            {
                Profiler::Get().EndScope();
            }
        }
        ScopedProfile(const ScopedProfile&) = delete;
        ScopedProfile& operator=(const ScopedProfile&) = delete;

    private:
        bool m_began;
    };

} // namespace foundation::profiler
