// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.run: a run's services (owned, borrowed,
// replaced, per session), its sessions and their hooks, its id never reused, and the registry
// announcing each transition once to every listener, clearing the focus with a destroyed run,
// and letting a late listener catch up.
#include <doctest/doctest.h>
#include <initializer_list>
#include "Core/Prelude.h"

import foundation.core;
import foundation.messaging;
import foundation.run;

using namespace foundation::core;
using namespace foundation::run;

namespace
{
    // What happened, in order, across services and listeners.
    Array<String>& Log()
    {
        static Array<String> log;
        return log;
    }

    struct Hooked : IRunService
    {
        explicit Hooked(StringView name) : name(name) {}
        ~Hooked() override { Log().PushBack(Format(u8"~{}", name.AsView())); }
        void OnRunPaused(IRun&, bool paused) override { Log().PushBack(Format(u8"{} paused {}", name.AsView(), paused)); }
        void OnRunStopping(IRun&) override { Log().PushBack(Format(u8"{} stopping", name.AsView())); }
        String name;
    };
    struct First : Hooked
    {
        First() : Hooked(u8"first") {}
    };
    struct Second : Hooked
    {
        Second() : Hooked(u8"second") {}
    };
    struct Session : Hooked
    {
        Session() : Hooked(u8"session") {}
    };
    struct Plain
    {
        int value = 7;
    };

    struct Recorder : IRunListener
    {
        explicit Recorder(StringView name) : name(name) {}
        void OnRunCreated(IRun& r) override { Log().PushBack(Format(u8"{} created {}", name.AsView(), r.Name())); }
        void OnRunStarted(IRun& r) override { Log().PushBack(Format(u8"{} started {}", name.AsView(), r.Name())); }
        void OnRunPaused(IRun& r, bool p) override { Log().PushBack(Format(u8"{} paused {} {}", name.AsView(), r.Name(), p)); }
        void OnRunStopping(IRun& r) override { Log().PushBack(Format(u8"{} stopping {}", name.AsView(), r.Name())); }
        void OnRunDestroyed(IRun& r) override { Log().PushBack(Format(u8"{} destroyed {}", name.AsView(), r.Name())); }
        void OnFocusChanged(IRun* r) override
        {
            Log().PushBack(Format(u8"{} focus {}", name.AsView(), r != nullptr ? r->Name() : StringView(u8"none")));
        }
        String name;
    };

    struct TestRun final : RunBase
    {
        explicit TestRun(StringView name) : RunBase(DefaultAllocator(), name) {}
    };

    bool Logged(std::initializer_list<const char*> expected)
    {
        Array<String>& log = Log();
        bool same = log.Size() == expected.size();
        usize i = 0;
        for (const char* e : expected)
        {
            if (!same)
                break;
            same = log[i++] == StringView(reinterpret_cast<const utf8char*>(e));
        }
        if (!same)
            for (const String& line : log)
                MESSAGE(doctest::String(reinterpret_cast<const char*>(line.CStr())));
        log.Clear();
        return same;
    }
}

TEST_CASE("run: services by type, owned destroyed last-first, borrowed left alone, a second of a type replaces")
{
    Log().Clear();
    Plain borrowed;
    {
        TestRun run(u8"Game 1");
        run.Add<First>();
        run.Add<Second>();
        CHECK(&run.Register<Plain>(borrowed) == &borrowed);
        CHECK(run.Has<First>());
        CHECK(run.Get<Plain>()->value == 7);
        CHECK(run.Get<Session>() == nullptr);
        run.Add<First>(); // replaces: the old one goes now
        CHECK(Logged({"~first"}));
        run.Remove<Plain>();
        CHECK_FALSE(run.Has<Plain>());
    }
    // Reverse order of registration: the replacement First was registered after Second.
    CHECK(Logged({"~first", "~second"}));
    CHECK(borrowed.value == 7);
}

TEST_CASE("run: sessions start, pause once per change, stop with hooks last-first, and per-session services go")
{
    Log().Clear();
    TestRun run(u8"Game 1");
    run.Add<First>();
    run.AddForSession<Session>();
    run.Add<Second>();
    CHECK(run.State() == RunState::Idle);
    run.SetPaused(true); // no session: nothing
    run.Stop();
    CHECK(Logged({}));

    run.Start();
    CHECK(run.State() == RunState::Running);
    run.SetPaused(true);
    run.SetPaused(true); // the same value again: nothing
    CHECK(run.State() == RunState::Paused);
    run.SetPaused(false);
    CHECK(Logged({"second paused true", "session paused true", "first paused true", "second paused false", "session paused false",
                  "first paused false"}));

    run.Stop();
    CHECK(run.State() == RunState::Idle);
    CHECK(Logged({"second stopping", "session stopping", "first stopping", "~session"}));
    CHECK_FALSE(run.Has<Session>());
    CHECK(run.Has<First>());

    // Play again: session-long services are the same objects, per-session ones come back fresh.
    First* first = run.Get<First>();
    run.Start();
    run.AddForSession<Session>();
    CHECK(run.Get<First>() == first);
    run.Dispose(); // stops the session under way first (this session's Session came after Second)
    CHECK(run.State() == RunState::Disposed);
    CHECK(Logged({"session stopping", "second stopping", "first stopping", "~session", "~second", "~first"}));
    run.Dispose(); // idempotent
    CHECK(Logged({}));
}

TEST_CASE("run: ids are never reused, and each run has its own bus")
{
    RunId first = 0;
    {
        TestRun a(u8"a");
        first = a.Id();
        CHECK(first != 0);
    }
    TestRun b(u8"b");
    TestRun c(u8"c");
    CHECK(b.Id() > first);
    CHECK(c.Id() > b.Id());
    CHECK(&b.Events() != &c.Events());
    int heard = 0;
    (void)b.Events().Subscribe(StringHash(u8"ping"), [&](const Variant&) { ++heard; });
    b.Events().Publish(StringHash(u8"ping"), Variant{});
    c.Events().Drain();
    CHECK(heard == 0);
    b.Events().Drain();
    CHECK(heard == 1);
}

TEST_CASE("run registry: every transition announced once, in order, to every listener; destroyed runs leave")
{
    Log().Clear();
    RunRegistry registry(DefaultAllocator());
    Recorder audio(u8"audio");
    Recorder gui(u8"gui");
    registry.Subscribe(audio);
    registry.Subscribe(gui);
    registry.Subscribe(audio); // once only
    {
        TestRun run(u8"Game 1");
        registry.Add(run);
        CHECK(registry.Count() == 1);
        CHECK(registry.Find(run.Id()) == &run);
        run.Start();
        run.SetPaused(true);
        run.Stop();
        CHECK(Logged({"audio created Game 1", "gui created Game 1", "audio started Game 1", "gui started Game 1",
                      "audio paused Game 1 true", "gui paused Game 1 true", "audio stopping Game 1", "gui stopping Game 1"}));
        registry.SetFocused(&run);
        registry.SetFocused(&run); // unchanged: nothing
        CHECK(Logged({"audio focus Game 1", "gui focus Game 1"}));
    }
    // The run's end took it out, the focus with it.
    CHECK(Logged({"audio focus none", "gui focus none", "audio destroyed Game 1", "gui destroyed Game 1"}));
    CHECK(registry.Count() == 0);
    CHECK(registry.Focused() == nullptr);

    registry.Unsubscribe(gui);
    TestRun next(u8"Game 2");
    registry.Add(next);
    CHECK(Logged({"audio created Game 2"}));
}

TEST_CASE("run registry: a late listener catches up by listing the runs; a registry gone leaves its runs alone")
{
    Log().Clear();
    TestRun a(u8"A");
    TestRun b(u8"B");
    {
        RunRegistry registry(DefaultAllocator());
        registry.Add(a);
        b.Start();
        registry.Add(b); // added running: announced as created and started
        Recorder late(u8"late");
        registry.Subscribe(late);
        Array<String> seen;
        registry.ForEachRun([&](IRun& run) { seen.PushBack(String(run.Name())); });
        REQUIRE(seen.Size() == 2);
        CHECK(seen[0] == StringView(u8"A"));
        CHECK(seen[1] == StringView(u8"B"));
        registry.Unsubscribe(late);
    }
    Log().Clear();
    b.Stop(); // the registry is gone: nothing tells it
    a.Dispose();
    CHECK(Logged({}));
}
