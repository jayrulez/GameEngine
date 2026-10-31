// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// SceneAnimation's graph parameter ops reach the entity's graph player by name: a Float, an Int,
// a Bool and a Trigger, each set from a script's side and read back from the player, so a graph
// whose conditions compare any of them can be driven from gameplay.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.scene;
import foundation.animation;
import foundation.script.facades;
import engine.animation;

using namespace foundation::core;
namespace scene = foundation::scene;
namespace animation = foundation::animation;

TEST_CASE("animation: SceneAnimation sets a graph's Float, Int, Bool and Trigger parameters")
{
    animation::Skeleton skeleton{1};
    skeleton.Bones()[0].index = 0;
    skeleton.Bones()[0].parentIndex = -1;
    skeleton.FindRootBones();
    skeleton.BuildChildIndices();

    animation::AnimationGraph graph;
    const i32 speed = graph.AddParameter(u8"Speed", animation::AnimationParameterType::Float);
    const i32 weapon = graph.AddParameter(u8"Weapon", animation::AnimationParameterType::Int);
    const i32 grounded = graph.AddParameter(u8"Grounded", animation::AnimationParameterType::Bool);
    const i32 jump = graph.AddParameter(u8"Jump", animation::AnimationParameterType::Trigger);

    scene::Scene level{DefaultAllocator(), u8"graph"};
    auto* graphs = level.AddSystem<engine::animation::AnimationGraphComponentManager>();
    const scene::EntityHandle hero = level.CreateEntity(u8"Hero");
    engine::animation::AnimationGraphComponent& component = graphs->Add(hero);
    // The player the manager builds on its first tick, built here without the resources.
    component.player = MakeUnique<animation::AnimationGraphPlayer>(DefaultAllocator(), graph, skeleton);
    animation::AnimationGraphPlayer& player = *component.player;

    const foundation::script::Entity entity{&level, hero.index, hero.generation};
    const engine::animation::SceneAnimation animations{&level};
    animations.setFloat(entity, String(u8"Speed"), 3.5f);
    animations.setInt(entity, String(u8"Weapon"), 2);
    animations.setBool(entity, String(u8"Grounded"), true);
    animations.setTrigger(entity, String(u8"Jump"));

    CHECK(player.GetFloat(speed) == doctest::Approx(3.5f));
    CHECK(player.GetInt(weapon) == 2);
    CHECK(player.GetBool(grounded));
    CHECK(player.GetBool(jump)); // a trigger is a bool until the next update consumes it

    animations.setInt(entity, String(u8"Missing"), 7); // an unknown name changes nothing
    CHECK(player.GetInt(weapon) == 2);
}
