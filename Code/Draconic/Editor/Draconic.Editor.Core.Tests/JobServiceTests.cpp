// EditorJobService: a background job runs on a worker, reports through JobContext, and completes on
// the main thread from Update(). Tests pump Update() in a spin loop (the workers are fast) and check
// completion status, log drain, error propagation, and one-at-a-time ordering.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.editor.core;

using namespace draconic::foundation;
namespace editor = draconic::editor;

namespace
{
    // Pump Update() until no job is in flight (bounded, so a hang fails instead of spinning forever).
    void PumpUntilIdle(editor::EditorJobService& jobs, const Function<void(StringView)>& log = {})
    {
        int guard = 0;
        while (jobs.IsBusy() && guard++ < 2'000'000)
        {
            jobs.Update(log);
        }
        jobs.Update(log); // final drain
    }
}

TEST_CASE("jobs: a submitted job runs on a worker, reports, and completes on Update")
{
    editor::EditorJobService jobs;

    bool doneFired = false;
    bool okStatus = false;
    jobs.Submit(
        u8"Work",
        [](editor::JobContext& ctx) -> Status
        {
            ctx.SetStep(u8"phase one", 1, 2);
            ctx.SetFraction(0.5f);
            ctx.Log(u8"halfway");
            ctx.SetStep(u8"phase two", 2, 2);
            ctx.SetFraction(1.0f);
            return Status{};
        },
        [&](Status s)
        {
            doneFired = true;
            okStatus = s.IsOk();
        });

    CHECK(jobs.IsBusy()); // running synchronously spawned before Submit returned

    Array<String> logs;
    PumpUntilIdle(jobs, [&](StringView l) { logs.PushBack(String(l)); });

    CHECK(doneFired);
    CHECK(okStatus);
    CHECK_FALSE(jobs.IsBusy());
    CHECK_FALSE(jobs.Progress().active); // idle => no active progress

    bool sawLog = false;
    for (const String& l : logs)
    {
        if (l == u8"halfway")
        {
            sawLog = true;
        }
    }
    CHECK(sawLog);
}

TEST_CASE("jobs: a failing job propagates its Status to onDone")
{
    editor::EditorJobService jobs;
    bool failed = false;
    jobs.Submit(
        u8"Bad", [](editor::JobContext&) -> Status { return Status{ErrorCode::Internal}; },
        [&](Status s) { failed = !s.IsOk(); });
    PumpUntilIdle(jobs);
    CHECK(failed);
}

TEST_CASE("jobs: submissions run one at a time, in order")
{
    editor::EditorJobService jobs;
    Array<int> order;
    // The completion callbacks run on the main thread (from Update), so appending is race-free.
    for (int i = 0; i < 3; ++i)
    {
        jobs.Submit(
            u8"n", [](editor::JobContext&) -> Status { return Status{}; },
            [&order, i](Status) { order.PushBack(i); });
    }
    PumpUntilIdle(jobs);
    REQUIRE(order.Size() == 3u);
    CHECK(order[0] == 0);
    CHECK(order[1] == 1);
    CHECK(order[2] == 2);
}

TEST_CASE("cook service: external mutation lock defers RunWhenIdle until released")
{
    // The app wires ExternalMutationLock to the job service (a background export reads the
    // DBs from its worker) - structural mutations must defer exactly like during a cook.
    draconic::editor::EditorCookService cook;
    bool busy = false;
    cook.ExternalMutationLock = [&busy]() { return busy; };
    CHECK(!cook.MutationLocked());

    int ran = 0;
    cook.RunWhenIdle(Function<void()>{[&ran]() { ++ran; }});
    CHECK(ran == 1); // unlocked -> runs immediately

    busy = true;
    CHECK(cook.MutationLocked());
    cook.RunWhenIdle(Function<void()>{[&ran]() { ++ran; }});
    CHECK(ran == 1); // locked -> deferred
    cook.Update({});
    CHECK(ran == 1); // still locked

    busy = false;
    cook.Update({});
    CHECK(ran == 2); // released -> the deferred action replays
}

TEST_CASE("jobs: light lane runs concurrently and never trips IsBusy")
{
    editor::EditorJobService jobs;

    // Light work completes on Update without ever making the BUILD lane busy
    // (IsBusy gates cook mutations - previews must not lock the build).
    int value = 0;
    bool lightDone = false;
    jobs.SubmitLight(Function<void()>{[&value]() { value = 42; }},
                     Function<void()>{[&lightDone]() { lightDone = true; }});
    CHECK_FALSE(jobs.IsBusy());
    CHECK(jobs.IsLightBusy());

    int guard = 0;
    while (jobs.IsLightBusy() && guard++ < 2'000'000)
    {
        jobs.Update();
    }
    jobs.Update();
    CHECK(lightDone);
    CHECK(value == 42); // the work's writes are published before onDone
    CHECK_FALSE(jobs.IsLightBusy());
    CHECK_FALSE(jobs.IsBusy());
}

TEST_CASE("jobs: queued light jobs run in order; onDone may chain another")
{
    editor::EditorJobService jobs;

    Array<i32> order;
    bool chained = false;
    jobs.SubmitLight(Function<void()>{}, Function<void()>{[&order]() { order.PushBack(1); }});
    jobs.SubmitLight(Function<void()>{},
                     Function<void()>{[&order, &chained, &jobs]()
                                      {
                                          order.PushBack(2);
                                          if (!chained)
                                          {
                                              chained = true;
                                              jobs.SubmitLight(Function<void()>{},
                                                               Function<void()>{[&order]()
                                                                                { order.PushBack(3); }});
                                          }
                                      }});

    int guard = 0;
    while (jobs.IsLightBusy() && guard++ < 2'000'000)
    {
        jobs.Update();
    }
    jobs.Update();
    REQUIRE(order.Size() == 3);
    CHECK(order[0] == 1);
    CHECK(order[1] == 2);
    CHECK(order[2] == 3);
}
