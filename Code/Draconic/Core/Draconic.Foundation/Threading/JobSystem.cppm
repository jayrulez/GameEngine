// Draconic Foundation - :job_system partition
//
// A stackless work-stealing job system: a fixed (pre-sized) worker pool with per-worker
// deques, ParallelFor fan-out, and counter-based dependencies (Submit / SubmitAfter /
// Wait). NO fibers - the renderer's parallelism is broad data-parallel fan-out plus a few
// dependent stages, and fibers don't translate to WASM (Asyncify-only).
//
// The load-bearing property is CALLER PARTICIPATION: a thread that Waits (on a Counter, a
// ParallelFor, or all jobs) runs jobs itself instead of blocking. This (a) keeps the
// calling thread busy, (b) is WASM-safe (the main thread never blocks indefinitely on a
// condition variable - it works then returns), and (c) gives a correct single-threaded
// fallback for free when worker count is 0 (the caller runs everything).
//
// WASM constraints honored: the worker pool is pre-sized at construction (no spawn
// mid-run), and waiting threads participate rather than block.

module;
#include "Draconic.Foundation/Prelude.h"
#include <atomic>
#include <thread>
#include <type_traits>

export module draconic.foundation:job_system;

import :base;
import :allocator;
import :unique_ptr;
import :array;
import :system;
import :thread;
import :mutex;
import :condition_variable;
import :scoped_lock;

export namespace draconic::foundation
{
    class Counter;

    namespace detail
    {
        // A type-erased unit of work. `data` is a heap-allocated callable; `signal` (if
        // set) is decremented when the job completes (driving dependencies + waits).
        struct JobItem
        {
            void (*invoke)(void*) = nullptr;
            void (*destroy)(void*) = nullptr;
            void* data = nullptr;
            Counter* signal = nullptr;
        };
    }

    // A completion counter: jobs SIGNAL it (decrement on completion) and may DEPEND on it
    // (run only once it reaches 0). Caller-owned - keep it alive until it reaches 0 and all
    // waits/continuations on it are done (typically a stack local in the frame loop). Not
    // copyable/movable (a stable address is referenced by in-flight jobs).
    class Counter
    {
    public:
        Counter() = default;
        explicit Counter(i32 initial) noexcept : m_count(initial) {}
        Counter(const Counter&) = delete;
        Counter& operator=(const Counter&) = delete;

        [[nodiscard]] i32 Value() const noexcept { return m_count.load(std::memory_order_acquire); }

    private:
        friend class JobSystem;
        std::atomic<i32> m_count{0};
        Mutex m_lock;                           // guards m_continuations
        Array<detail::JobItem> m_continuations; // jobs to schedule when m_count hits 0
    };

    class JobSystem
    {
    public:
        // workerCount == 0 picks (logical cores - 1). A pool of 0 workers is valid: every
        // Wait/ParallelFor then runs inline on the calling thread (single-threaded fallback).
        explicit JobSystem(u32 workerCount = 0)
        {
            u32 count = workerCount;
            if (count == 0)
            {
                const u32 cores = LogicalCoreCount();
                count = (cores > 1) ? (cores - 1) : 0; // 0 cores-extra => inline fallback
            }
            m_workerCount = count;
            for (u32 i = 0; i < count; ++i)
            {
                m_deques.PushBack(MakeUnique<Deque>(DefaultAllocator()));
            }
            for (u32 i = 0; i < count; ++i)
            {
                m_workers.PushBack(Thread([this, i]() { WorkerLoop(i); }));
            }
        }

        JobSystem(const JobSystem&) = delete;
        JobSystem& operator=(const JobSystem&) = delete;

        ~JobSystem()
        {
            {
                ScopedLock lock(m_idleMutex);
                m_stop = true;
            }
            m_jobAvailable.NotifyAll();
            for (Thread& worker : m_workers)
            {
                worker.Join();
            }
        }

        [[nodiscard]] u32 WorkerCount() const noexcept { return m_workerCount; }

        // The number of distinct slots a body may run on: one per worker plus one for any
        // non-worker (external / caller-participating) thread. Size per-worker scratch (e.g.
        // the renderer's per-worker frame arenas) by this and index with CurrentSlot().
        [[nodiscard]] u32 SlotCount() const noexcept { return m_workerCount + 1; }

        // The slot of the calling thread: [0, WorkerCount()) for a worker, or WorkerCount()
        // for any non-worker thread. Stable for the duration of a job/body invocation, so it
        // can index per-worker scratch without locking.
        [[nodiscard]] u32 CurrentSlot() const noexcept
        {
            const i32 self = WorkerSlot();
            return (self >= 0) ? static_cast<u32>(self) : m_workerCount;
        }

        // ---- submission ----

        // Run `fn` sometime on the pool. Optionally decrement `signal` when it completes.
        template <typename Fn>
        void Submit(Fn&& fn, Counter* signal = nullptr)
        {
            Schedule(MakeJob(static_cast<Fn&&>(fn), signal));
        }

        // Run `fn` only after `dep` reaches 0 (a dependency edge). If `dep` is already 0 it
        // runs immediately. Optionally signals `signal` on completion. `dep` must outlive
        // this call until it reaches 0.
        template <typename Fn>
        void SubmitAfter(Counter& dep, Fn&& fn, Counter* signal = nullptr)
        {
            detail::JobItem job = MakeJob(static_cast<Fn&&>(fn), signal);
            bool runNow = false;
            {
                ScopedLock lock(dep.m_lock);
                if (dep.m_count.load(std::memory_order_acquire) == 0)
                {
                    runNow = true;
                }
                else
                {
                    dep.m_continuations.PushBack(job);
                }
            }
            if (runNow)
            {
                Schedule(job);
            }
        }

        // Runs fn(u32 i) for i in [0, count), in parallel across the pool, BLOCKING (with
        // caller participation) until all are done. `grainSize` items per chunk; 0 = auto.
        template <typename Fn>
        void ParallelFor(u32 count, Fn&& fn, u32 grainSize = 0)
        {
            if (count == 0)
            {
                return;
            }

            const u32 workers = (m_workerCount > 0) ? m_workerCount : 1;
            u32 grain = grainSize;
            if (grain == 0)
            {
                // ~4 chunks per worker for load balancing; at least 1 item per chunk.
                grain = (count + (4u * workers) - 1u) / (4u * workers);
                if (grain == 0)
                {
                    grain = 1;
                }
            }
            const u32 chunks = (count + grain - 1u) / grain;

            Counter done{static_cast<i32>(chunks)};
            for (u32 c = 0; c < chunks; ++c)
            {
                const u32 begin = c * grain;
                const u32 end = (begin + grain < count) ? (begin + grain) : count;
                // `fn` outlives this call (we Wait below), so chunk jobs reference it.
                Schedule(MakeJob(
                    [&fn, begin, end]()
                    {
                        for (u32 i = begin; i < end; ++i)
                        {
                            fn(i);
                        }
                    },
                    &done));
            }
            Wait(done);
        }

        // ---- waiting (caller participates) ----

        // Runs jobs until `counter` reaches 0. The calling thread is a worker for the duration.
        void Wait(Counter& counter)
        {
            while (counter.Value() != 0)
            {
                if (!RunOneJob())
                {
                    YieldThread();
                }
            }
            // Lifetime fence: the thread that drove the count to 0 decremented it WHILE holding
            // m_lock (see RunJob) and may still be inside that critical section. Acquiring the
            // lock here blocks until it has released it, so a caller that destroys `counter`
            // right after Wait returns (e.g. ParallelFor's stack `done`) can't free it out from
            // under that signaling thread.
            {
                ScopedLock lock(counter.m_lock);
            }
        }

        // Runs jobs until every submitted job has completed.
        void WaitForAll()
        {
            while (m_pending.load(std::memory_order_acquire) != 0)
            {
                if (!RunOneJob())
                {
                    YieldThread();
                }
            }
        }

    private:
        struct Deque
        {
            Mutex mutex;
            Array<detail::JobItem> items;
        };

        // Build a heap-allocated, type-erased job from a callable.
        template <typename Fn>
        [[nodiscard]] detail::JobItem MakeJob(Fn&& fn, Counter* signal)
        {
            using Stored = std::decay_t<Fn>;
            Stored* held = DefaultAllocator().New<Stored>(static_cast<Fn&&>(fn));
            return detail::JobItem{[](void* p) { (*static_cast<Stored*>(p))(); }, [](void* p)
                                   { DefaultAllocator().Delete(static_cast<Stored*>(p)); }, held,
                                   signal};
        }

        // Push a job onto a deque (the submitter's own if it is a worker, else round-robin)
        // and wake a sleeping worker.
        void Schedule(const detail::JobItem& job)
        {
            m_pending.fetch_add(1, std::memory_order_relaxed);

            if (m_workerCount == 0)
            {
                // No workers: park on deque 0's stand-in (a single shared list). A Wait on
                // the calling thread will drain it. Use the external list.
                {
                    ScopedLock lock(m_externalMutex);
                    m_external.PushBack(job);
                }
                return;
            }

            const i32 self = WorkerSlot();
            u32 target;
            if (self >= 0)
            {
                target = static_cast<u32>(self);
            }
            else
            {
                target = m_nextExternal.fetch_add(1, std::memory_order_relaxed) % m_workerCount;
            }

            {
                ScopedLock lock(m_deques[target]->mutex);
                m_deques[target]->items.PushBack(job);
            }
            // Fence against the wait/sleep decision (see WorkerLoop): acquiring m_idleMutex
            // here closes the lost-wakeup window.
            {
                ScopedLock lock(m_idleMutex);
            }
            m_jobAvailable.NotifyOne();
        }

        // Try to run one job (own deque LIFO, else steal FIFO from others / external).
        // Returns false if no job was found.
        bool RunOneJob()
        {
            detail::JobItem job{};
            if (!TryGetJob(job))
            {
                return false;
            }
            RunJob(job);
            return true;
        }

        bool TryGetJob(detail::JobItem& out)
        {
            const i32 self = WorkerSlot();

            // 1) own deque (LIFO - cache-friendly)
            if (self >= 0)
            {
                Deque& d = *m_deques[static_cast<u32>(self)];
                ScopedLock lock(d.mutex);
                if (!d.items.IsEmpty())
                {
                    out = d.items.Back();
                    d.items.PopBack();
                    return true;
                }
            }

            // 2) steal from other worker deques (FIFO - take the oldest, least contended)
            for (u32 k = 0; k < m_workerCount; ++k)
            {
                if (self >= 0 && k == static_cast<u32>(self))
                {
                    continue;
                }
                Deque& d = *m_deques[k];
                ScopedLock lock(d.mutex);
                if (!d.items.IsEmpty())
                {
                    out = d.items.Front();
                    d.items.RemoveAt(0);
                    return true;
                }
            }

            // 3) external list (jobs submitted by non-workers, and the 0-worker fallback)
            {
                ScopedLock lock(m_externalMutex);
                if (!m_external.IsEmpty())
                {
                    out = m_external.Front();
                    m_external.RemoveAt(0);
                    return true;
                }
            }
            return false;
        }

        void RunJob(const detail::JobItem& job)
        {
            job.invoke(job.data);
            job.destroy(job.data);

            // Handle the completion signal (and schedule its dependents) BEFORE dropping
            // this job from m_pending - a continuation is added to m_pending before its
            // predecessor leaves, so m_pending never momentarily hits 0 while a dependent is
            // still pending (which would let WaitForAll return early).
            if (Counter* signal = job.signal; signal != nullptr)
            {
                // Decrement INSIDE the lock: this holds m_lock across the count's 1->0
                // transition, which (paired with the fence in Wait) stops a released waiter
                // from destroying the Counter while we are still touching it. Once we leave
                // this scope with the count at 0, `signal` may be freed - never touch it again.
                bool last = false;
                Array<detail::JobItem> ready;
                {
                    ScopedLock lock(signal->m_lock);
                    if (signal->m_count.fetch_sub(1, std::memory_order_acq_rel) == 1)
                    {
                        last = true;
                        ready = Move(signal->m_continuations);
                    }
                }
                if (last)
                {
                    for (const detail::JobItem& cont : ready)
                    {
                        Schedule(cont);
                    }
                }
            }

            m_pending.fetch_sub(1, std::memory_order_acq_rel);
        }

        void WorkerLoop(u32 index)
        {
            WorkerSlot() = static_cast<i32>(index);
            for (;;)
            {
                if (RunOneJob())
                {
                    continue;
                }

                // No work found - sleep until notified or stopped (re-check under the lock
                // to close the lost-wakeup window; Schedule fences on m_idleMutex).
                ScopedLock lock(m_idleMutex);
                if (m_stop)
                {
                    return;
                }
                if (HasAnyWork())
                {
                    continue;
                } // a job arrived between the failed run and the lock
                m_jobAvailable.Wait(m_idleMutex);
                if (m_stop && !HasAnyWork())
                {
                    return;
                }
            }
        }

        [[nodiscard]] bool HasAnyWork()
        {
            for (u32 k = 0; k < m_workerCount; ++k)
            {
                ScopedLock lock(m_deques[k]->mutex);
                if (!m_deques[k]->items.IsEmpty())
                {
                    return true;
                }
            }
            ScopedLock lock(m_externalMutex);
            return !m_external.IsEmpty();
        }

        static void YieldThread() noexcept { std::this_thread::yield(); }

        // The calling thread's worker slot: a worker's index, or -1 for any non-worker thread.
        // A function-local thread_local (single COMDAT instance across TUs) rather than a static
        // data member - the latter, odr-used by an inline accessor from an importing TU, would
        // emit a duplicate definition and fail to link.
        static i32& WorkerSlot() noexcept
        {
            static thread_local i32 slot = -1;
            return slot;
        }

        u32 m_workerCount = 0;
        Array<UniquePtr<Deque>> m_deques; // one per worker
        Mutex m_externalMutex;
        Array<detail::JobItem> m_external; // non-worker submissions / 0-worker fallback
        Array<Thread> m_workers;
        Mutex m_idleMutex; // guards sleep/wake decision
        ConditionVariable m_jobAvailable;
        std::atomic<i64> m_pending{0};
        std::atomic<u32> m_nextExternal{0};
        bool m_stop = false;
    };

    // ---- process-global JobSystem -----------------------------------------------------------
    //
    // The engine-wide JobSystem, mirroring DefaultAllocator()'s global-accessor shape (and
    // Sedulous's static JobSystem). The client Application brackets its lifetime - init before
    // any subsystem starts, shutdown after every subsystem + GPU teardown - so it outlives all
    // users. Code that wants to parallelize must tolerate its absence (HasGlobalJobSystem()) and
    // fall back to serial: in unit tests / headless tools that never start an Application it is
    // never initialized. The JobSystem class itself stays instance-constructible (tests build
    // their own); this is just a managed global instance.

    namespace detail
    {
        inline JobSystem* g_globalJobs = nullptr;
    }

    // Create the global JobSystem (no-op if already created). workerCount 0 => cores-1.
    inline void InitGlobalJobSystem(u32 workerCount = 0)
    {
        if (detail::g_globalJobs == nullptr)
        {
            detail::g_globalJobs = DefaultAllocator().New<JobSystem>(workerCount);
        }
    }

    // Destroy the global JobSystem (joins its workers; no-op if absent).
    inline void ShutdownGlobalJobSystem()
    {
        if (detail::g_globalJobs != nullptr)
        {
            DefaultAllocator().Delete(detail::g_globalJobs);
            detail::g_globalJobs = nullptr;
        }
    }

    [[nodiscard]] inline bool HasGlobalJobSystem() noexcept
    {
        return detail::g_globalJobs != nullptr;
    }
    [[nodiscard]] inline JobSystem& GlobalJobs() noexcept { return *detail::g_globalJobs; }
}
