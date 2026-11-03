// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;

using namespace foundation::core;

// --- TaskPool (Archive/task-layer-history.md) ----------------------------------------------------------

namespace
{
#if !PLATFORM_WEB
    // Spins (yielding) until `ready` holds or about two seconds pass; false on the timeout, so a
    // broken pool fails a check instead of hanging the suite. Only the threaded cases use it, and
    // web has no workers, so it is compiled where they are.
    template <typename Ready>
    bool Eventually(Ready ready)
    {
        const Stopwatch clock = Stopwatch::StartNew();
        while (!ready())
        {
            if (clock.Elapsed() > Duration::FromMilliseconds(2000.0))
                return false;
            Thread::YieldThread();
        }
        return true;
    }
#endif
}

TEST_CASE("task pool: with no workers, tasks run in Pump, in order, and Wait runs the queue until its task ends")
{
    TaskPool pool(DefaultAllocator(), 0u);
    Array<int> order(DefaultAllocator());
    Array<int> completed(DefaultAllocator());
    RefPtr<TaskHandle> first = pool.Submit(u8"first", [&order](TaskContext&) { order.PushBack(1); return Status{}; },
                                           [&completed](Status) { completed.PushBack(1); });
    RefPtr<TaskHandle> second = pool.Submit(u8"second", [&order](TaskContext&) { order.PushBack(2); return Status{}; });
    CHECK(first->State() == TaskState::Queued); // nothing runs until the owner pumps
    CHECK(order.IsEmpty());

    pool.Wait(*second); // runs the queue here until `second` has ended
    CHECK(order.Size() == 2u);
    CHECK(order[0] == 1);
    CHECK(second->State() == TaskState::Done);
    CHECK(completed.IsEmpty()); // completions wait for Pump

    pool.Pump();
    REQUIRE(completed.Size() == 1u);
    CHECK(completed[0] == 1);

    RefPtr<TaskHandle> third = pool.Submit(u8"third", [](TaskContext&) { return Status(ErrorCode::NotFound); });
    pool.Pump(); // runs it within the budget
    CHECK(third->State() == TaskState::Failed);
    CHECK(third->Result().Code() == ErrorCode::NotFound);
}

TEST_CASE("task pool: a task cancelled before it starts is dropped unrun, and its completion never runs")
{
    TaskPool pool(DefaultAllocator(), 0u);
    bool ran = false;
    bool completed = false;
    RefPtr<TaskHandle> task = pool.Submit(u8"dropped", [&ran](TaskContext&) { ran = true; return Status{}; },
                                          [&completed](Status) { completed = true; });
    task->Cancel();
    pool.Wait(*task);
    CHECK(task->State() == TaskState::Cancelled);
    pool.Pump();
    CHECK_FALSE(ran);
    CHECK_FALSE(completed);
}

TEST_CASE("task pool: progress and log lines from the work reach the pumping thread")
{
    TaskPool pool(DefaultAllocator(), 0u);
    RefPtr<TaskHandle> task = pool.Submit(u8"reporting", [](TaskContext& context)
                                          {
                                              context.SetProgress(0.5f, u8"Halfway");
                                              context.Log(u8"half done");
                                              return Status{};
                                          });
    Array<String> lines(DefaultAllocator());
    pool.Pump([&lines](const TaskHandle& from, StringView line) { lines.PushBack(Format(u8"{}: {}", from.Name(), line)); });
    CHECK(task->Progress().fraction == doctest::Approx(0.5f));
    CHECK(task->Progress().step == StringView(u8"Halfway"));

    RefPtr<TaskHandle> stepped = pool.Submit(u8"stepped", [](TaskContext& context)
                                             {
                                                 context.SetFraction(0.25f);
                                                 context.SetStep(u8"Writing Rider.glb", 2, 12);
                                                 return Status{};
                                             });
    pool.Pump();
    const TaskProgress progress = stepped->Progress();
    CHECK(progress.fraction == doctest::Approx(0.25f)); // a step keeps the fraction
    CHECK(progress.step == StringView(u8"Writing Rider.glb"));
    CHECK(progress.stepIndex == 2u);
    CHECK(progress.stepCount == 12u);
    REQUIRE(lines.Size() == 1u);
    CHECK(lines[0] == StringView(u8"reporting: half done"));
}

TEST_CASE("task pool: a completion may submit more")
{
    TaskPool pool(DefaultAllocator(), 0u);
    int stage = 0;
    TaskPool* p = &pool;
    (void)pool.Submit(u8"one", [](TaskContext&) { return Status{}; },
                      [p, &stage](Status)
                      {
                          stage = 1;
                          (void)p->Submit(u8"two", [](TaskContext&) { return Status{}; }, [&stage](Status) { stage = 2; });
                      });
    pool.Pump();
    CHECK(stage == 1);
    pool.Pump();
    CHECK(stage == 2);
}

#if !PLATFORM_WEB
TEST_CASE("task pool: work runs on a worker, never the pumping thread; its completion runs on the pumping thread")
{
    TaskPool pool(DefaultAllocator(), 2u);
    CHECK(pool.WorkerCount() == 2u);
    const u64 main = Thread::CurrentId();
    Atomic<u64> ranOn{0};
    u64 completedOn = 0;
    RefPtr<TaskHandle> task = pool.Submit(u8"off", [&ranOn](TaskContext&) { ranOn.store(Thread::CurrentId()); return Status{}; },
                                          [&completedOn](Status) { completedOn = Thread::CurrentId(); });
    pool.Wait(*task);
    CHECK(task->State() == TaskState::Done);
    CHECK(ranOn.load() != main);
    CHECK(completedOn == 0u); // not before Pump
    pool.Pump();
    CHECK(completedOn == main);
}

TEST_CASE("task pool: a lane runs its tasks one at a time in order; the shared lane runs them side by side")
{
    TaskPool pool(DefaultAllocator(), 3u);
    const TaskLane lane = pool.CreateLane(u8"build");
    Atomic<int> inside{0};
    Atomic<int> most{0};
    Mutex orderLock;
    Array<int> order(DefaultAllocator());
    Array<RefPtr<TaskHandle>> tasks(DefaultAllocator());
    for (int i = 0; i < 6; ++i)
        tasks.PushBack(pool.Submit(u8"serial", [i, &inside, &most, &orderLock, &order](TaskContext&)
                                   {
                                       const int now = inside.fetch_add(1) + 1;
                                       int seen = most.load();
                                       while (now > seen && !most.compare_exchange_weak(seen, now)) {}
                                       {
                                           ScopedLock lock(orderLock);
                                           order.PushBack(i);
                                       }
                                       for (int spin = 0; spin < 200; ++spin)
                                           Thread::YieldThread();
                                       inside.fetch_sub(1);
                                       return Status{};
                                   },
                                   {}, lane));
    CHECK(pool.IsLaneBusy(lane));
    pool.Wait(*tasks[tasks.Size() - 1]);
    CHECK(most.load() == 1);
    REQUIRE(order.Size() == 6u);
    for (int i = 0; i < 6; ++i)
        CHECK(order[static_cast<usize>(i)] == i);
    pool.Pump();
    CHECK_FALSE(pool.IsLaneBusy(lane));

    // Two shared tasks that each wait to see the other running: they finish only side by side.
    Atomic<int> arrived{0};
    const auto meet = [&arrived](TaskContext&)
    {
        arrived.fetch_add(1);
        return Eventually([&arrived]() { return arrived.load() >= 2; }) ? Status{} : Status(ErrorCode::Unknown);
    };
    RefPtr<TaskHandle> a = pool.Submit(u8"a", meet);
    RefPtr<TaskHandle> b = pool.Submit(u8"b", meet);
    pool.Wait(*a);
    pool.Wait(*b);
    CHECK(a->State() == TaskState::Done);
    CHECK(b->State() == TaskState::Done);
}

TEST_CASE("task pool: a running task stops when cancelled; one queued behind it in its lane is dropped at once by Wait")
{
    TaskPool pool(DefaultAllocator(), 1u);
    const TaskLane lane = pool.CreateLane(u8"build");
    Atomic<bool> started{false};
    bool completed = false;
    RefPtr<TaskHandle> running = pool.Submit(u8"long", [&started](TaskContext& context)
                                             {
                                                 started.store(true);
                                                 while (!context.IsCancelled())
                                                     Thread::YieldThread();
                                                 return Status(ErrorCode::Cancelled);
                                             },
                                             [&completed](Status) { completed = true; }, lane);
    bool queuedRan = false;
    RefPtr<TaskHandle> queued = pool.Submit(u8"behind", [&queuedRan](TaskContext&) { queuedRan = true; return Status{}; }, {}, lane);
    REQUIRE(Eventually([&started]() { return started.load(); }));

    queued->Cancel();
    pool.Wait(*queued); // dropped from its lane without waiting for the one running
    CHECK(queued->State() == TaskState::Cancelled);
    CHECK(running->State() == TaskState::Running);

    running->Cancel();
    pool.Wait(*running);
    CHECK(running->State() == TaskState::Cancelled);
    pool.Pump();
    CHECK_FALSE(completed);
    CHECK_FALSE(queuedRan);
    CHECK_FALSE(pool.IsLaneBusy(lane));
}

TEST_CASE("task pool: going away cancels what runs, drops what waits, and calls no completion")
{
    Atomic<bool> started{false};
    Atomic<bool> sawCancel{false};
    bool completed = false;
    bool queuedRan = false;
    {
        TaskPool pool(DefaultAllocator(), 1u);
        (void)pool.Submit(u8"long", [&started, &sawCancel](TaskContext& context)
                          {
                              started.store(true);
                              while (!context.IsCancelled())
                                  Thread::YieldThread();
                              sawCancel.store(true);
                              return Status(ErrorCode::Cancelled);
                          },
                          [&completed](Status) { completed = true; });
        (void)pool.Submit(u8"waiting", [&queuedRan](TaskContext&) { queuedRan = true; return Status{}; });
        REQUIRE(Eventually([&started]() { return started.load(); }));
    }
    CHECK(sawCancel.load());
    CHECK_FALSE(queuedRan);
    CHECK_FALSE(completed);
}
#endif // !PLATFORM_WEB

TEST_CASE("task pool: the default worker count leaves the frame its threads")
{
    const u32 count = TaskPool::DefaultWorkerCount();
#if PLATFORM_WEB
    CHECK(count == 0u);
#else
    CHECK(count >= 1u);
    CHECK(count <= 4u);
#endif
}

TEST_CASE("task pool: Finish waits for every task, then hands them back; InFlight counts what is not handed back yet")
{
    const u32 counts[] = {0u, 2u}; // no workers, then two
    for (u32 workers : counts)
    {
#if PLATFORM_WEB
        if (workers > 0u)
            continue;
#endif
        TaskPool pool(DefaultAllocator(), workers);
        const TaskLane lane = pool.CreateLane(u8"serial");
        Atomic<int> ran{0};
        int completed = 0;
        for (int i = 0; i < 3; ++i)
            (void)pool.Submit(u8"shared", [&ran](TaskContext&) { ran.fetch_add(1); return Status{}; }, [&completed](Status) { ++completed; });
        for (int i = 0; i < 3; ++i)
            (void)pool.Submit(u8"lane", [&ran](TaskContext&) { ran.fetch_add(1); return Status{}; }, [&completed](Status) { ++completed; }, lane);
        CHECK(pool.InFlight() == 6u);
        pool.Finish();
        CHECK(ran.load() == 6);
        CHECK(completed == 6);
        CHECK(pool.InFlight() == 0u);
    }
}

TEST_CASE("task pool: CancelAll drops what waits and tells what runs; Finish then waits, and no cancelled completion runs")
{
    const u32 counts[] = {0u, 2u};
    for (u32 workers : counts)
    {
#if PLATFORM_WEB
        if (workers > 0u)
            continue;
#endif
        TaskPool pool(DefaultAllocator(), workers);
        const TaskLane lane = pool.CreateLane(u8"serial");
        Atomic<bool> started{false};
        int completed = 0;
        (void)pool.Submit(u8"long", [&started](TaskContext& context)
                          {
                              started.store(true);
                              while (!context.IsCancelled())
                                  Thread::YieldThread();
                              return Status(ErrorCode::Cancelled);
                          },
                          [&completed](Status) { ++completed; }, lane);
        bool queuedRan = false;
        (void)pool.Submit(u8"behind", [&queuedRan](TaskContext&) { queuedRan = true; return Status{}; }, [&completed](Status) { ++completed; }, lane);
        if (workers > 0u)
        {
            const Stopwatch clock = Stopwatch::StartNew();
            while (!started.load() && clock.Elapsed() < Duration::FromMilliseconds(2000.0))
                Thread::YieldThread();
        }
        pool.CancelAll();
        pool.Finish();
        CHECK_FALSE(queuedRan);
        CHECK(completed == 0);
        CHECK(pool.InFlight() == 0u);
        CHECK_FALSE(pool.IsLaneBusy(lane));
    }
}
