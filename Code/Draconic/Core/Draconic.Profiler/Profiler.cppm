/// Draconic::Profiler - `draconic.profiler`.
///
/// A lightweight hierarchical CPU scope profiler (the design ported from Sedulous.Profiler, made
/// C++-idiomatic). Scopes nest into a per-thread tree; at frame end every thread's samples merge
/// into the completed-frame snapshot. Timing uses foundation::GetTicks (the high-res monotonic counter).
/// Instrument with the DRACONIC_PROFILE_SCOPE macro (Profiler.h) - it compiles to nothing when
/// DRACONIC_PROFILING is off. Thread-safe: scopes touch only thread-local state; the registry +
/// frame swap are mutex-guarded, and the merge runs at frame end when workers are idle.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.profiler;

import draconic.foundation;

using namespace draconic::foundation;

export namespace draconic::profiler
{

    // One completed scope. `name` is a borrowed string literal (the macro passes literals).
    struct ProfileSample
    {
        const char* name = "";
        u64 startTick = 0; // foundation::GetTicks() at scope entry
        u64 durationTicks = 0;
        u32 depth = 0; // nesting depth within its thread
        u32 threadIndex = 0;
    };

    // A finished frame: its samples (across all threads) + the frame's wall-clock span.
    struct ProfileFrame
    {
        u64 frameNumber = 0;
        u64 frameStartTick = 0;
        u64 frameDurationTicks = 0;
        Array<ProfileSample> samples;

        [[nodiscard]] f64 FrameMs() const noexcept
        {
            return TicksToMilliseconds(frameDurationTicks);
        }
    };

    // The profiler singleton. BeginFrame/EndFrame bracket a frame; BeginScope/EndScope (via the RAII
    // helper) record nested scopes. CompletedFrame() returns the last finished frame for reporting.
    class Profiler
    {
    public:
        [[nodiscard]] static Profiler& Get() noexcept
        {
            static Profiler instance;
            return instance;
        }

        void SetEnabled(bool e) noexcept { m_enabled = e; }
        [[nodiscard]] bool Enabled() const noexcept { return m_enabled; }

        void BeginFrame() noexcept
        {
            if (!m_enabled)
            {
                return;
            }
            m_frameStartTick = GetTicks();
        }

        void EndFrame() noexcept
        {
            if (!m_enabled)
            {
                return;
            }
            const u64 endTick = GetTicks();
            ScopedLock lock(m_mutex);
            m_completed.samples.Clear();
            m_completed.frameNumber = m_frameNumber;
            m_completed.frameStartTick = m_frameStartTick;
            m_completed.frameDurationTicks =
                (endTick >= m_frameStartTick) ? (endTick - m_frameStartTick) : 0;
            // Workers are idle at frame end, so draining their buffers here needs no per-thread lock.
            for (ThreadData* td : m_threads)
            {
                for (const ProfileSample& s : td->samples)
                {
                    m_completed.samples.PushBack(s);
                }
                td->samples.Clear();
                td->stack
                    .Clear(); // defensive: any unbalanced scopes don't leak into the next frame
            }
            PushHistory(m_completed.FrameMs());
            ++m_frameNumber;
        }

        void BeginScope(const char* name) noexcept
        {
            if (!m_enabled)
            {
                return;
            }
            ThreadData& td = Local();
            td.stack.PushBack(ActiveScope{name, GetTicks(), static_cast<u32>(td.stack.Size())});
        }

        void EndScope() noexcept
        {
            if (!m_enabled)
            {
                return;
            }
            ThreadData& td = Local();
            if (td.stack.IsEmpty())
            {
                return;
            }
            const ActiveScope a = td.stack[td.stack.Size() - 1];
            td.stack.PopBack();
            const u64 now = GetTicks();
            td.samples.PushBack(ProfileSample{a.name, a.startTick,
                                              (now >= a.startTick) ? (now - a.startTick) : 0,
                                              a.depth, td.index});
        }

        [[nodiscard]] const ProfileFrame& CompletedFrame() const noexcept { return m_completed; }

        // Rolling average of recent frame times (ms), for a stable headline number.
        [[nodiscard]] f64 AverageFrameMs() const noexcept
        {
            if (m_historyCount == 0)
            {
                return 0.0;
            }
            f64 sum = 0.0;
            for (u32 i = 0; i < m_historyCount; ++i)
            {
                sum += m_history[i];
            }
            return sum / static_cast<f64>(m_historyCount);
        }

        // Human-readable dump: frame headline (this frame + rolling avg) then the scope tree, indented
        // by depth and ordered by start time so parents precede children. (The P-key prints this.)
        [[nodiscard]] String BuildReport() const
        {
            String out;
            const ProfileFrame& f = m_completed;
            AppendFormat(out, u8"=== Profile: frame {}  {} ms (avg {} ms)  {} samples ===\n",
                         f.frameNumber, f.FrameMs(), AverageFrameMs(), f.samples.Size());

            // Order by start time (so parents precede children) without mutating m_completed.
            Array<u32> order;
            for (u32 i = 0; i < static_cast<u32>(f.samples.Size()); ++i)
            {
                order.PushBack(i);
            }
            for (u32 i = 1; i < static_cast<u32>(order.Size()); ++i)
            {
                const u32 v = order[i];
                u32 j = i;
                while (j > 0 && f.samples[order[j - 1]].startTick > f.samples[v].startTick)
                {
                    order[j] = order[j - 1];
                    --j;
                }
                order[j] = v;
            }
            for (u32 idx : order)
            {
                const ProfileSample& s = f.samples[idx];
                out.Append(u8"  ");
                for (u32 d = 0; d < s.depth; ++d)
                {
                    out.Append(u8"  ");
                }
                out.Append(reinterpret_cast<const char8_t*>(
                    s.name)); // scope names are ASCII (valid UTF-8)
                AppendFormat(out, u8": {} ms [t{}]\n", TicksToMilliseconds(s.durationTicks),
                             s.threadIndex);
            }
            return out;
        }

        Profiler(const Profiler&) = delete;
        Profiler& operator=(const Profiler&) = delete;

    private:
        Profiler() = default;
        ~Profiler()
        {
            for (ThreadData* td : m_threads)
            {
                DefaultAllocator().Delete(td);
            }
        }

        struct ActiveScope
        {
            const char* name;
            u64 startTick;
            u32 depth;
        };
        struct ThreadData
        {
            u32 index = 0;
            Array<ProfileSample> samples; // completed scopes this frame
            Array<ActiveScope> stack;     // currently-open scopes
        };

        // Per-thread state, created + registered on first use (registry guarded by the mutex).
        [[nodiscard]] ThreadData& Local()
        {
            if (s_local == nullptr)
            {
                ThreadData* td = DefaultAllocator().New<ThreadData>();
                ScopedLock lock(m_mutex);
                td->index = m_nextThreadIndex++;
                m_threads.PushBack(td);
                s_local = td;
            }
            return *s_local;
        }

        void PushHistory(f64 ms) noexcept
        {
            m_history[m_historyHead] = ms;
            m_historyHead = (m_historyHead + 1) % kHistory;
            if (m_historyCount < kHistory)
            {
                ++m_historyCount;
            }
        }

        static constexpr u32 kHistory = 64;

        bool m_enabled = true;
        u64 m_frameNumber = 0;
        u64 m_frameStartTick = 0;
        ProfileFrame m_completed;
        Mutex m_mutex; // guards the thread registry + the frame swap
        Array<ThreadData*> m_threads;
        u32 m_nextThreadIndex = 0;
        f64 m_history[kHistory] = {};
        u32 m_historyHead = 0;
        u32 m_historyCount = 0;

        inline static thread_local ThreadData* s_local = nullptr;
    };

    // RAII scope: brackets a profiled region. Use via DRACONIC_PROFILE_SCOPE (Profiler.h).
    class ScopedProfile
    {
    public:
        explicit ScopedProfile(const char* name) noexcept { Profiler::Get().BeginScope(name); }
        ~ScopedProfile() noexcept { Profiler::Get().EndScope(); }
        ScopedProfile(const ScopedProfile&) = delete;
        ScopedProfile& operator=(const ScopedProfile&) = delete;
    };

} // namespace draconic::profiler
