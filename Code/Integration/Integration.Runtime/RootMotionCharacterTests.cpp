// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Root motion walking a physics character (root-motion.md P2): an animator in Character mode on
// the character's model walks it by the clip's travel through the scene's character capability
// (animation and physics never link each other), so it collides: into a wall, it stops at the
// wall instead of walking through it; and when the clip stops, the character stops.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.scene;
import foundation.animation;
import foundation.physics;
import engine.physics;
import engine.render;
import engine.animation;

using namespace foundation::core;
namespace scene = foundation::scene;
namespace animation = foundation::animation;

TEST_CASE("root motion: a character walked by its clip stops at a wall, and when the clip does")
{
    engine::physics::RegisterPhysicsComponentReflection();
    scene::Scene level{DefaultAllocator(), u8"walk"};
    level.AddSystem<engine::physics::RigidBodyComponentManager>();
    level.AddSystem<engine::physics::ColliderComponentManager>();
    auto* characters = level.AddSystem<engine::physics::CharacterComponentManager>();
    level.AddSystem<engine::physics::PhysicsSceneSystem>();
    level.AddSystem<engine::render::MeshComponentManager>();
    engine::animation::AddAnimationSceneManagers(level);

    const auto box = [&](StringView name, Float3 at, Float3 half)
    {
        const scene::EntityHandle e = level.CreateEntity(name);
        level.SetLocalPosition(e, at);
        auto& body = level.GetSystem<engine::physics::RigidBodyComponentManager>()->Add(e);
        body.motion = foundation::physics::MotionKind::Static;
        body.layer = foundation::physics::PhysicsLayer::Static;
        body.halfExtents = half;
        return e;
    };
    (void)box(u8"floor", Float3{0, -0.5f, 0}, Float3{50, 0.5f, 50});
    (void)box(u8"wall", Float3{0, 1, 4}, Float3{5, 1, 0.25f}); // its near face at z = 3.75

    const scene::EntityHandle hero = level.CreateEntity(u8"Hero");
    level.SetLocalPosition(hero, Float3{0, 0.9f, 0});
    (void)characters->Add(hero);
    // The model under it, its origin at the feet; the animator walks 2 m a second along +Z.
    const scene::EntityHandle model = level.CreateEntity(u8"Model");
    level.SetParent(model, hero);
    level.SetLocalPosition(model, Float3{0, -0.9f, 0});
    RefPtr<animation::Skeleton> skeleton = MakeRef<animation::Skeleton>(DefaultAllocator(), 1);
    skeleton->Bones()[0].index = 0;
    skeleton->Bones()[0].parentIndex = -1;
    skeleton->FindRootBones();
    skeleton->BuildChildIndices();
    RefPtr<animation::AnimationClip> walk = MakeRef<animation::AnimationClip>(DefaultAllocator(), u8"Walk", 1.0f, true);
    walk->rootMotion.horizontal = true;
    for (i32 i = 0; i <= 10; ++i)
    {
        walk->rootMotion.times.PushBack(static_cast<f32>(i) * 0.1f);
        walk->rootMotion.positions.PushBack(Float3{0, 0, 0.2f * static_cast<f32>(i)});
        walk->rootMotion.yaws.PushBack(0.0f);
    }
    walk->GetOrCreatePositionTrack(0)->AddKeyframe(0.0f, Float3{});
    walk->GetOrCreatePositionTrack(0)->AddKeyframe(1.0f, Float3{});
    auto& animator = level.GetSystem<engine::animation::SkeletalAnimationComponentManager>()->Add(model);
    animator.skeleton.SetDirect(skeleton);
    animator.clip.SetDirect(walk);
    animator.rootMotion = engine::animation::RootMotionMode::Character;

    level.UpdateTransforms();
    level.Start();
    level.SetSimulationEnabled(true);
    const f32 dt = 1.0f / 60.0f;
    const auto frame = [&]()
    {
        level.FixedUpdate(dt);
        level.Update(dt);
    };
    for (i32 i = 0; i < 60; ++i) // a second: about 2 m
    {
        frame();
    }
    // Where the controller has it (the entity's transform is written by the render-time interpolation).
    const auto heroZ = [&]() { return characters->Get(hero)->currPosition.z; };
    const f32 afterOne = heroZ();
    CHECK(afterOne == doctest::Approx(2.0f).epsilon(0.1));
    for (i32 i = 0; i < 180; ++i) // three more: it would be at 8 m through the wall
    {
        frame();
    }
    const f32 atWall = heroZ();
    CHECK(atWall < 3.75f);                   // stopped by the wall
    CHECK(atWall > 3.75f - 0.35f - 0.1f);    // against it (its radius from the face)

    // The clip stops: one zero move, and the character stands (no walking on by the last one).
    animator.player->Stop();
    for (i32 i = 0; i < 30; ++i)
    {
        frame();
    }
    CHECK(characters->Get(hero)->moveVelocity.z == doctest::Approx(0.0f));
    // And switched to Ignore, the animator lets the character go.
    animator.player->Play(walk.Get());
    frame();
    CHECK(characters->Get(hero)->moveVelocity.z > 1.0f);
    animator.rootMotion = engine::animation::RootMotionMode::Ignore;
    frame();
    CHECK(characters->Get(hero)->moveVelocity.z == doctest::Approx(0.0f));
}
