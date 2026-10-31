// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.scene :manager - SceneManager owns a group of scenes, assembles them through a
// type-erased installer, tears them down through a type-erased uninstaller, and ticks its own group
// (the linchpin of the GameInstance model).
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.scene;
import foundation.run;

using namespace foundation::core;
using namespace foundation::scene;

namespace
{
    // Records assembly/teardown notifications so a test can assert the hooks fired.
    struct Recording
    {
        int installed = 0;
        int removed = 0;
    };
}

TEST_CASE("scene-manager: create/active/current + destroy")
{
    SceneManager mgr{DefaultAllocator()};
    CHECK(mgr.SceneCount() == 0u);
    CHECK(mgr.CurrentScene() == nullptr);

    Scene* a = mgr.CreateScene(u8"A");
    Scene* b = mgr.CreateScene(u8"B");
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(mgr.SceneCount() == 2u);
    CHECK(mgr.ActiveScenes().Size() == 2u);
    CHECK(mgr.CurrentScene() == a); // first created = current by default
    CHECK(mgr.GetScene(u8"B") == b);

    mgr.SetCurrentScene(b);
    CHECK(mgr.CurrentScene() == b);

    mgr.DestroyScene(a);
    CHECK(mgr.SceneCount() == 1u);
    CHECK(mgr.GetScene(u8"A") == nullptr);
    CHECK(mgr.CurrentScene() == b); // destroying a non-current scene leaves current intact

    mgr.DestroyScene(b);
    CHECK(mgr.SceneCount() == 0u);
    CHECK(mgr.CurrentScene() == nullptr); // destroying the current scene clears it
}

TEST_CASE("scene-manager: fans install/uninstall hooks around each scene")
{
    Recording rec;
    SceneManager mgr{DefaultAllocator()};
    // Type-erased: the caller owns whatever the hooks close over (a composition / observer list).
    mgr.SetSceneInstaller([&rec](Scene&) { ++rec.installed; });
    mgr.SetSceneUninstaller([&rec](Scene&) { ++rec.removed; });

    Scene* s = mgr.CreateScene(u8"S");
    CHECK(rec.installed == 1);
    CHECK(rec.removed == 0);

    mgr.DestroyScene(s);
    CHECK(rec.removed == 1);
}

TEST_CASE("scene-manager: group time scale folds into the tick (identity at 1.0)")
{
    SceneManager mgr{DefaultAllocator()};
    Scene* s = mgr.CreateScene(u8"S");
    s->Start();
    s->SetSimulationEnabled(true);
    REQUIRE(mgr.TimeScale() == doctest::Approx(1.0f));

    // Default group scale = 1.0: BeginFrame/Update tick without faulting (identity path).
    mgr.BeginFrame(FrameTime(0.016f, 1.0f, 1.0f, 1.0f, 0.0f));
    mgr.Update(FrameTime(0.016f, 1.0f, 1.0f, 1.0f, 0.0f));

    // A 0 group scale freezes the group (dt reaching scenes is 0) - still must not fault.
    mgr.SetTimeScale(0.0f);
    mgr.BeginFrame(FrameTime(0.016f, 1.0f, 1.0f, 1.0f, 0.0f));
    mgr.Update(FrameTime(0.016f, 1.0f, 1.0f, 1.0f, 0.0f));
    CHECK(mgr.TimeScale() == doctest::Approx(0.0f));

    // A negative scale clamps to 0 (a group never ticks backwards): -2 lands as pause, not rewind.
    mgr.SetTimeScale(-2.0f);
    CHECK(mgr.TimeScale() == doctest::Approx(0.0f));
    mgr.SetTimeScale(0.5f); // and the clamp does not stick - a valid scale still lands
    CHECK(mgr.TimeScale() == doctest::Approx(0.5f));
}

TEST_CASE("scene-manager: Clear destroys the whole group and notifies")
{
    Recording rec;
    SceneManager mgr{DefaultAllocator()};
    mgr.SetSceneInstaller([&rec](Scene&) { ++rec.installed; });
    mgr.SetSceneUninstaller([&rec](Scene&) { ++rec.removed; });

    (void)mgr.CreateScene(u8"A");
    (void)mgr.CreateScene(u8"B");
    CHECK(rec.installed == 2);

    mgr.Clear();
    CHECK(mgr.SceneCount() == 0u);
    CHECK(rec.removed == 2);
    CHECK(mgr.CurrentScene() == nullptr);
}

TEST_CASE("scene-manager: inactive create + activate/deactivate gate (task #123 async level load)")
{
    SceneManager mgr{DefaultAllocator()};

    // Inactive create: owned, but not ticked/rendered (not active) and not the spawn target.
    Scene* s = mgr.CreateScene(u8"loading", /*activate*/ false);
    REQUIRE(s != nullptr);
    CHECK(mgr.SceneCount() == 1u);
    CHECK(mgr.ActiveScenes().Size() == 0u);
    CHECK_FALSE(mgr.IsActive(s));
    CHECK(mgr.CurrentScene() == nullptr);

    // Activate once resources are ready: now active + current (none was).
    mgr.ActivateScene(s);
    CHECK(mgr.ActiveScenes().Size() == 1u);
    CHECK(mgr.IsActive(s));
    CHECK(mgr.CurrentScene() == s);

    // Idempotent - a second activate does not double-add.
    mgr.ActivateScene(s);
    CHECK(mgr.ActiveScenes().Size() == 1u);

    // Deactivate: dropped from the active/render set + current cleared, but NOT destroyed.
    mgr.DeactivateScene(s);
    CHECK(mgr.ActiveScenes().Size() == 0u);
    CHECK_FALSE(mgr.IsActive(s));
    CHECK(mgr.CurrentScene() == nullptr);
    CHECK(mgr.SceneCount() == 1u);

    // Re-activation works.
    mgr.ActivateScene(s);
    CHECK(mgr.IsActive(s));

    // Default create still auto-activates (unchanged behavior).
    Scene* d = mgr.CreateScene(u8"default");
    CHECK(mgr.IsActive(d));
    CHECK(mgr.ActiveScenes().Size() == 2u);

    // Activating a scene this manager does not own is a no-op (Owns guard).
    Scene foreign(DefaultAllocator(), u8"foreign");
    mgr.ActivateScene(&foreign);
    CHECK_FALSE(mgr.IsActive(&foreign));
    CHECK(mgr.ActiveScenes().Size() == 2u);
}
namespace
{
    // Counts stops into a count that outlives the scene (the system goes with it).
    class StopWitness final : public SceneSystem
    {
    public:
        int* stops = nullptr;
        void OnSceneStopped() override { ++*stops; }
    };
}

TEST_CASE("scene-manager: a running scene is stopped before it is destroyed")
{
    // A scene system unhooks itself from what outlives the scene in its stop hook (a script
    // system's handlers on a run's event bus). A running scene destroyed without a stop (a run
    // loading the scene it is already in) left those handlers pointing at freed memory.
    SceneManager mgr{DefaultAllocator()};
    int stops = 0;
    Scene* running = mgr.CreateScene(u8"running");
    running->AddSystem<StopWitness>()->stops = &stops;
    running->Start();
    mgr.DestroyScene(running);
    CHECK(stops == 1);

    // A scene that never started is not stopped (its systems never began).
    Scene* idle = mgr.CreateScene(u8"idle");
    idle->AddSystem<StopWitness>()->stops = &stops;
    mgr.DestroyScene(idle);
    CHECK(stops == 1);

    // A scene already stopped is not stopped twice.
    Scene* stopped = mgr.CreateScene(u8"stopped");
    stopped->AddSystem<StopWitness>()->stops = &stops;
    stopped->Start();
    stopped->Stop();
    CHECK(stops == 2);
    mgr.DestroyScene(stopped);
    CHECK(stops == 2);
}

TEST_CASE("scene-manager: created scenes carry the run the manager was given")
{
    SceneManager loose{DefaultAllocator()};
    CHECK(loose.CreateScene(u8"page")->Run() == nullptr); // no run: an editor page's scenes

    struct TestRun final : foundation::run::RunBase
    {
        TestRun() : RunBase(DefaultAllocator(), u8"test") {}
    } runIdentity;
    SceneManager owned{DefaultAllocator()};
    owned.SetSceneRun(&runIdentity);
    Scene* a = owned.CreateScene(u8"a");
    Scene* b = owned.CreateScene(u8"b", /*activate*/ false); // the async-load path too
    CHECK(a->Run() == &runIdentity);
    CHECK(b->Run() == &runIdentity);
}
