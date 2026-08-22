// foundation.scene :manager - SceneManager owns a group of scenes, assembles them through a
// type-erased installer, tears them down through a type-erased uninstaller, and ticks its own group
// (the linchpin of the GameInstance model; game-instance.md §11).
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.scene;

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
    SceneManager mgr;
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
    SceneManager mgr;
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
    SceneManager mgr;
    Scene* s = mgr.CreateScene(u8"S");
    s->Start();
    s->SetSimulationEnabled(true);
    REQUIRE(mgr.TimeScale() == doctest::Approx(1.0f));

    // Default group scale = 1.0: BeginFrame/Update tick without faulting (identity path).
    mgr.BeginFrame(0.016f, 1.0f, 0.0f);
    mgr.Update(0.016f);

    // A 0 group scale freezes the group (dt reaching scenes is 0) - still must not fault.
    mgr.SetTimeScale(0.0f);
    mgr.BeginFrame(0.016f, 1.0f, 0.0f);
    mgr.Update(0.016f);
    CHECK(mgr.TimeScale() == doctest::Approx(0.0f));
}

TEST_CASE("scene-manager: Clear destroys the whole group and notifies")
{
    Recording rec;
    SceneManager mgr;
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
    SceneManager mgr;

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
    Scene foreign(u8"foreign");
    mgr.ActivateScene(&foreign);
    CHECK_FALSE(mgr.IsActive(&foreign));
    CHECK(mgr.ActiveScenes().Size() == 2u);
}