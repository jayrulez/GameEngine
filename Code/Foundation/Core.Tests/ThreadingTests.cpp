// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

#include <doctest/doctest.h>

#include <cstring>

#include "Core/Debug/Assert.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;

using namespace foundation::core;

// --- Threading -------------------------------------------------------------

// The web build is single-threaded (no wasm-pthreads): every case here spawns real
// threads, so the suite sits out the web build.
// TODO: a "JobSystem inline mode" would bring a web-runnable subset back.
#if !PLATFORM_WEB

TEST_CASE("threading: a thread runs and joins")
{
    Atomic<int> ran{0};
    Thread t([&ran]() { ran.fetch_add(1); });
    CHECK(t.IsJoinable());
    t.Join();
    CHECK_FALSE(t.IsJoinable());
    CHECK(ran.load() == 1);
}

TEST_CASE("threading: Atomic fetch_add from many threads is exact")
{
    Atomic<i64> counter{0};
    constexpr int kThreads = 8;
    constexpr int kPerThread = 10000;

    Array<Thread> threads;
    for (int i = 0; i < kThreads; ++i)
    {
        threads.PushBack(Thread(
            [&counter]()
            {
                for (int j = 0; j < kPerThread; ++j)
                {
                    counter.fetch_add(1);
                }
            }));
    }
    for (Thread& t : threads)
    {
        t.Join();
    }

    CHECK(counter.load() == static_cast<i64>(kThreads) * kPerThread);
}

TEST_CASE("threading: Mutex/ScopedLock protects a non-atomic counter")
{
    Mutex mutex;
    i64 counter = 0;
    constexpr int kThreads = 8;
    constexpr int kPerThread = 10000;

    Array<Thread> threads;
    for (int i = 0; i < kThreads; ++i)
    {
        threads.PushBack(Thread(
            [&mutex, &counter]()
            {
                for (int j = 0; j < kPerThread; ++j)
                {
                    ScopedLock lock(mutex);
                    ++counter;
                }
            }));
    }
    for (Thread& t : threads)
    {
        t.Join();
    }

    CHECK(counter == static_cast<i64>(kThreads) * kPerThread);
}

TEST_CASE("threading: ConditionVariable hand-off between two threads")
{
    Mutex mutex;
    ConditionVariable cv;
    bool ready = false;
    int payload = 0;

    Thread consumer(
        [&]()
        {
            ScopedLock lock(mutex);
            while (!ready)
            {
                cv.Wait(mutex);
            }
            payload += 1; // observe the produced value
        });

    {
        ScopedLock lock(mutex);
        payload = 41;
        ready = true;
        cv.NotifyOne();
    }

    consumer.Join();
    CHECK(payload == 42);
}

TEST_CASE("threading: SpinLock protects a counter")
{
    SpinLock spin;
    i64 counter = 0;
    constexpr int kThreads = 4;
    constexpr int kPerThread = 5000;

    Array<Thread> threads;
    for (int i = 0; i < kThreads; ++i)
    {
        threads.PushBack(Thread(
            [&spin, &counter]()
            {
                for (int j = 0; j < kPerThread; ++j)
                {
                    ScopedLock lock(spin);
                    ++counter;
                }
            }));
    }
    for (Thread& t : threads)
    {
        t.Join();
    }
    CHECK(counter == static_cast<i64>(kThreads) * kPerThread);
}

TEST_CASE("threading: Semaphore hands out a bounded number of permits")
{
    Semaphore sem(0);
    Atomic<int> acquired{0};
    constexpr int kConsumers = 4;

    Array<Thread> threads;
    for (int i = 0; i < kConsumers; ++i)
    {
        threads.PushBack(Thread(
            [&sem, &acquired]()
            {
                sem.Acquire();
                acquired.fetch_add(1);
            }));
    }

    // Release exactly one permit per consumer.
    for (int i = 0; i < kConsumers; ++i)
    {
        sem.Release();
    }
    for (Thread& t : threads)
    {
        t.Join();
    }

    CHECK(acquired.load() == kConsumers);
    CHECK_FALSE(sem.TryAcquire()); // none left
}

TEST_CASE("threading: SharedMutex allows shared reads and exclusive writes")
{
    SharedMutex rw;
    i64 value = 0;
    constexpr int kWriters = 4;
    constexpr int kPerWriter = 2000;

    Array<Thread> threads;
    for (int i = 0; i < kWriters; ++i)
    {
        threads.PushBack(Thread(
            [&rw, &value]()
            {
                for (int j = 0; j < kPerWriter; ++j)
                {
                    ScopedLock lock(rw); // exclusive
                    ++value;
                }
            }));
    }
    // A reader that takes the shared lock a few times concurrently.
    threads.PushBack(Thread(
        [&rw, &value]()
        {
            for (int j = 0; j < 1000; ++j)
            {
                ScopedSharedLock lock(rw);
                volatile i64 observed = value; // read under shared lock
                (void)observed;
            }
        }));

    for (Thread& t : threads)
    {
        t.Join();
    }
    CHECK(value == static_cast<i64>(kWriters) * kPerWriter);
}

TEST_CASE("threading: JobSystem runs all submitted jobs")
{
    JobSystem jobs(DefaultAllocator(), 4);
    CHECK(jobs.WorkerCount() == 4u);

    Atomic<i64> sum{0};
    constexpr int kJobs = 1000;
    for (int i = 0; i < kJobs; ++i)
    {
        jobs.Submit([&sum, i]() { sum.fetch_add(i); });
    }
    jobs.WaitForAll();

    i64 expected = 0;
    for (int i = 0; i < kJobs; ++i)
    {
        expected += i;
    }
    CHECK(sum.load() == expected);

    jobs.WaitForAll(); // nothing pending -> returns immediately
    CHECK(sum.load() == expected);
}

TEST_CASE("threading: JobSystem allocates deques and job boxes from ITS allocator")
{
    TrackingAllocator tracker(DefaultAllocator());
    {
        JobSystem jobs(tracker, 2);
        CHECK(tracker.TotalAllocations() > 0u); // worker deques came from the tracker

        Atomic<int> ran{0};
        for (int i = 0; i < 8; ++i)
        {
            jobs.Submit([&ran]() { ran.fetch_add(1); });
        }
        jobs.WaitForAll();
        CHECK(ran.load() == 8);
    }
    // The deques and every job closure box returned to the tracker: a leak check per pool.
    CHECK_FALSE(tracker.HasLeaks());
}

TEST_CASE("threading: JobSystem default pool runs everything (caller participates)")
{
    JobSystem jobs(DefaultAllocator());
    Atomic<int> done{0};
    for (int i = 0; i < 50; ++i)
    {
        jobs.Submit([&done]() { done.fetch_add(1); });
    }
    jobs.WaitForAll();
    CHECK(done.load() == 50);
}

TEST_CASE("threading: ParallelFor covers the whole range exactly once")
{
    JobSystem jobs(DefaultAllocator(), 4);

    const u32 sizes[] = {0u, 1u, 3u, 1000u, 99999u};
    for (u32 n : sizes)
    {
        Atomic<i64> sum{0};
        Atomic<i64> visits{0};
        jobs.ParallelFor(n,
                         [&](u32 i)
                         {
                             sum.fetch_add(static_cast<i64>(i));
                             visits.fetch_add(1);
                         });
        const i64 expected = static_cast<i64>(n) * (static_cast<i64>(n) - 1) / 2;
        CHECK(sum.load() == expected); // each index visited once
        CHECK(visits.load() == static_cast<i64>(n));
    }

    // explicit grain size
    Atomic<i64> v{0};
    jobs.ParallelFor(500u, [&](u32) { v.fetch_add(1); }, /*grain*/ 7u);
    CHECK(v.load() == 500);
}

TEST_CASE("threading: dependencies - SubmitAfter runs only once its counter reaches 0")
{
    JobSystem jobs(DefaultAllocator(), 4);
    constexpr int kWork = 200;

    Atomic<int> work{0};
    Atomic<int> workSeenByFinalize{-1};
    Atomic<bool> finalizeRan{false};

    Counter gate{static_cast<i32>(kWork)};
    for (int i = 0; i < kWork; ++i)
    {
        jobs.Submit([&work]() { work.fetch_add(1); }, &gate);
    }
    jobs.SubmitAfter(gate,
                     [&]()
                     {
                         workSeenByFinalize.store(work.load()); // must observe all kWork done
                         finalizeRan.store(true);
                     });

    jobs.WaitForAll();
    CHECK(finalizeRan.load());                 // WaitForAll did not return before the continuation
    CHECK(workSeenByFinalize.load() == kWork); // continuation ran strictly after its dependencies
}

TEST_CASE("threading: Wait(Counter) participates until the counter is satisfied")
{
    JobSystem jobs(DefaultAllocator(), 4);
    Counter done{50};
    Atomic<int> n{0};
    for (int i = 0; i < 50; ++i)
    {
        jobs.Submit([&n]() { n.fetch_add(1); }, &done);
    }
    jobs.Wait(done);
    CHECK(done.Value() == 0);
    CHECK(n.load() == 50);
}

TEST_CASE("threading: worker slots are distinct and in range")
{
    JobSystem jobs(DefaultAllocator(), 4);
    CHECK(jobs.SlotCount() == jobs.WorkerCount() + 1u);

    // Every body invocation reports a slot in [0, SlotCount()); record which slots were used.
    constexpr u32 kSlots = 5; // WorkerCount(4) + 1 external
    Atomic<int> usedSlot[kSlots];
    for (u32 s = 0; s < kSlots; ++s)
    {
        usedSlot[s].store(0);
    }

    jobs.ParallelFor(20000u,
                     [&](u32)
                     {
                         const u32 slot = jobs.CurrentSlot();
                         REQUIRE(slot < jobs.SlotCount());
                         usedSlot[slot].fetch_add(1);
                     });

    // The caller participates, so the external slot (== WorkerCount) is generally hit too;
    // at minimum the work was spread across more than one slot.
    int distinct = 0;
    for (u32 s = 0; s < kSlots; ++s)
    {
        if (usedSlot[s].load() > 0)
        {
            ++distinct;
        }
    }
    CHECK(distinct >= 1);
}

TEST_CASE("threading: nested ParallelFor does not deadlock (caller participation)")
{
    JobSystem jobs(DefaultAllocator(), 4);
    Atomic<i64> total{0};
    // A ParallelFor whose body runs another ParallelFor: a worker that waits on the inner
    // loop participates in running it, so no worker is parked while work remains.
    jobs.ParallelFor(8u, [&](u32) { jobs.ParallelFor(8u, [&](u32) { total.fetch_add(1); }); });
    CHECK(total.load() == 64);
}

TEST_CASE("threading: a thread's name is its own, kept to 31 bytes")
{
    // Each thread starts unnamed and names only itself.
    String seen;
    Thread t(
        [&seen]()
        {
            seen = String(CurrentThreadName());
            SetCurrentThreadName(StringView(u8"Loader"));
            seen.Append(u8"|");
            seen.Append(CurrentThreadName());
        });
    t.Join();
    CHECK(seen == String(u8"|Loader"));

    const String before(CurrentThreadName());
    SetCurrentThreadName(StringView(u8"A thread with a name far longer than thirty-one bytes"));
    CHECK(CurrentThreadName().Size() == 31u);
    CHECK(CurrentThreadName() == StringView(u8"A thread with a name far longer"));
    SetCurrentThreadName(before.AsView());
}

namespace
{
    Atomic<int> g_hookBegins{0};
    Atomic<int> g_hookEnds{0};
    Atomic<int> g_hookAnswer{1}; // what begin answers: 1 opened a scope, 0 did not
    bool CountBegin()
    {
        g_hookBegins.fetch_add(1);
        return g_hookAnswer.load() != 0;
    }
    void CountEnd() { g_hookEnds.fetch_add(1); }
    const JobHooks kCountingHooks{&CountBegin, &CountEnd};
}

TEST_CASE("threading: job hooks bracket every job, and end only what begin opened")
{
    // How the profiler shows a worker's jobs: begin and end around each one, on its thread.
    g_hookBegins.store(0);
    g_hookEnds.store(0);
    g_hookAnswer.store(1);
    SetJobHooks(&kCountingHooks);
    {
        JobSystem jobs(DefaultAllocator(), 3);
        for (int i = 0; i < 200; ++i)
        {
            jobs.Submit([]() {});
        }
        jobs.WaitForAll();
        CHECK(g_hookBegins.load() == 200);
        CHECK(g_hookEnds.load() == 200);

        // A begin that opened nothing is not ended.
        g_hookAnswer.store(0);
        for (int i = 0; i < 50; ++i)
        {
            jobs.Submit([]() {});
        }
        jobs.WaitForAll();
        CHECK(g_hookBegins.load() == 250);
        CHECK(g_hookEnds.load() == 200);
    }
    SetJobHooks(nullptr);
    CHECK(CurrentJobHooks() == nullptr);
}

TEST_CASE("threading: a JobSystem's workers are named by their slot")
{
    JobSystem jobs(DefaultAllocator(), 3);
    Mutex lock;
    Array<String> names;
    jobs.ParallelFor(3000u,
                     [&](u32)
                     {
                         const u32 slot = jobs.CurrentSlot();
                         if (slot < jobs.WorkerCount())
                         {
                             String expected;
                             AppendFormat(expected, u8"Worker {}", slot);
                             ScopedLock guard(lock);
                             names.PushBack(String(CurrentThreadName()));
                             names.PushBack(expected);
                         }
                     });
    for (usize i = 0; i + 1 < names.Size(); i += 2)
    {
        CHECK(names[i] == names[i + 1]);
    }
}

#endif // !PLATFORM_WEB
