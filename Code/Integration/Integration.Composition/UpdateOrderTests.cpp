// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The Update order across the domains, on a scene composed as a game composes it: the scene's scripts
// run before the systems that act on what a script writes, so a destination, a sound or a graph
// parameter set in onUpdate takes effect that frame (user 2026-10-10, as Sedulous orders it); the
// Level script just before the entity behaviors.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.scene;
import engine.composition;
import engine.script;
import engine.navigation;

using namespace foundation::core;
namespace scene = foundation::scene;

TEST_CASE("integration.composition: scripts update before the crowd and every system after them")
{
    scene::Scene level(DefaultAllocator(), u8"level");
    engine::AddAllSceneManagers(level);
    auto* behaviors = level.GetSystem<engine::script::ScriptSceneSystem>();
    auto* levelScript = level.GetSystem<engine::script::SceneScriptSystem>();
    auto* crowd = level.GetSystem<engine::navigation::NavigationSceneSystem>();
    REQUIRE(behaviors != nullptr);
    REQUIRE(levelScript != nullptr);
    REQUIRE(crowd != nullptr);
    CHECK(levelScript->UpdateOrder() < behaviors->UpdateOrder());
    CHECK(behaviors->UpdateOrder() < crowd->UpdateOrder());

    // Nothing the scripts drive runs between the Level and the behaviors, and every other gameplay
    // system of the scene comes after them: only the frame-early domains (physics, the UI lane)
    // and the scene's own bookkeeping run before.
    level.ForEachSystem(
        [&](scene::SceneSystem& system)
        {
            if (&system == behaviors || &system == levelScript)
            {
                return;
            }
            const i32 order = system.UpdateOrder();
            CHECK_FALSE((order > levelScript->UpdateOrder() && order < behaviors->UpdateOrder()));
            if (order > behaviors->UpdateOrder())
            {
                CHECK(order >= crowd->UpdateOrder());
            }
        });
}
