// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Inverse kinematics on scene entities (inverse-kinematics.md P2): the end bone follows a moving
// target entity in the world, through a model space that is the mesh entity's (moved and turned
// apart from the animator); the weight eases over fadeSeconds and the modifier leaves the player
// at zero; components run in their order; an unknown bone disables with one log line; the script
// calls set a point and read the result; and a removed component leaves no modifier behind.
#include <doctest/doctest.h>

#include "Core/Prelude.h"

#include <cmath>
#include <initializer_list>

import foundation.core;
import foundation.animation;
import foundation.scene;
import foundation.script.facades;
import engine.animation;
import engine.render;

using namespace foundation::core;
namespace scene = foundation::scene;
namespace animation = foundation::animation;
using engine::animation::IkStatus;

namespace
{
    // Pelvis(0); Thigh(1) at the hip; Shin(2) 4 below; Foot(3) 3 below that; Spine(4); Head(5).
    RefPtr<animation::Skeleton> MakeSkeleton()
    {
        RefPtr<animation::Skeleton> s = MakeRef<animation::Skeleton>(DefaultAllocator(), 6);
        Array<animation::Bone>& bones = s->Bones();
        const char8_t* names[] = {u8"Pelvis", u8"Thigh", u8"Shin", u8"Foot", u8"Spine", u8"Head"};
        const i32 parents[] = {-1, 0, 1, 2, 0, 4};
        const Float3 offsets[] = {{0, 10, 0}, {1, 0, 0}, {0, -4, 0.2f}, {0, -3, -0.2f}, {0, 2, 0}, {0, 1, 0}};
        for (i32 i = 0; i < 6; ++i)
        {
            bones[static_cast<usize>(i)].index = i;
            bones[static_cast<usize>(i)].name = String(names[i]);
            bones[static_cast<usize>(i)].parentIndex = parents[i];
            bones[static_cast<usize>(i)].localBindPose.position = offsets[i];
        }
        s->BuildNameMap();
        s->FindRootBones();
        s->BuildChildIndices();
        s->ComputeInverseBindPoses();
        return s;
    }

    class CountingSink final : public ILogSink
    {
    public:
        void Write(LogLevel, StringView category, StringView message) noexcept override
        {
            if (category == StringView(u8"Animation") && message.ContainsIgnoreCase(u8"inverse kinematics"))
            {
                ++lines;
            }
        }
        usize lines = 0;
    };

    // A rider: the animator on Rider (moved and turned), its skinned mesh Body offset under it (so
    // model space is Body's world, not Rider's), an IK entity per component under Rider, and
    // targets at the top of the scene.
    struct Stage
    {
        scene::Scene level{DefaultAllocator(), u8"ik"};
        RefPtr<animation::Skeleton> skeleton = MakeSkeleton();
        scene::EntityHandle rider;
        scene::EntityHandle body;

        Stage()
        {
            level.AddSystem<engine::render::MeshComponentManager>();
            engine::animation::AddAnimationSceneManagers(level);
            rider = level.CreateEntity(u8"Rider");
            Transform riderAt;
            riderAt.position = Float3{5, 0, -2};
            riderAt.rotation = Quaternion::FromAxisAngle(Float3{0, 1, 0}, 0.6f);
            level.SetLocalTransform(rider, riderAt);
            body = level.CreateEntity(u8"Body");
            level.SetParent(body, rider);
            Transform bodyAt;
            bodyAt.position = Float3{0, 0.5f, 0.3f};
            bodyAt.rotation = Quaternion::FromAxisAngle(Float3{1, 0, 0}, 0.2f);
            level.SetLocalTransform(body, bodyAt);
            auto* clips = level.GetSystem<engine::animation::SkeletalAnimationComponentManager>();
            engine::animation::SkeletalAnimationComponent& a = clips->Add(rider);
            a.skeleton.SetDirect(skeleton);
            a.meshEntities.PushBack(level.GetEntityId(body));
        }

        scene::EntityHandle Target(StringView name, Float3 at)
        {
            const scene::EntityHandle e = level.CreateEntity(name);
            Transform t;
            t.position = at;
            level.SetLocalTransform(e, t);
            return e;
        }

        engine::animation::TwoBoneIkComponent& Leg(StringView name, scene::EntityHandle target, i32 order = 0)
        {
            const scene::EntityHandle e = level.CreateEntity(name);
            level.SetParent(e, rider);
            auto& c = level.GetSystem<engine::animation::TwoBoneIkComponentManager>()->Add(e);
            c.startBone = String(u8"Thigh");
            c.midBone = String(u8"Shin");
            c.endBone = String(u8"Foot");
            if (level.IsValid(target))
            {
                c.target = level.GetEntityId(target);
            }
            c.fadeSeconds = 0.0f;
            c.order = order;
            return c;
        }

        animation::AnimationPlayer& Player()
        {
            return *level.GetSystem<engine::animation::SkeletalAnimationComponentManager>()->Get(rider)->player;
        }

        // The bone's world position as the palette draws it: the final pose, through Body's world.
        Float3 BoneWorld(i32 bone)
        {
            animation::AnimationPlayer& p = Player();
            (void)p.GetSkinningMatrices();
            animation::ModelPoseCache cache;
            cache.Build(*skeleton, p.GetFinalPoses());
            const Float4x4& m = cache.At(bone);
            return TransformPoint(Float3{m.m[3][0], m.m[3][1], m.m[3][2]}, level.GetWorldMatrix(body));
        }

        void Tick(f32 seconds = 1.0f / 60.0f)
        {
            level.Update(seconds);
            (void)Player().GetSkinningMatrices();
        }
    };

    scene::EntityHandle OwnerOf(Stage& s, StringView name) { return s.level.FindEntityByName(name); }
}

TEST_CASE("ik component: the end bone follows a moving target entity through the mesh's model space")
{
    Stage s;
    const scene::EntityHandle target = s.Target(u8"Step", Float3{6, 4, 0});
    (void)s.Leg(u8"LegIk", target);
    s.Tick(); // the animator builds its player
    s.Tick(); // the IK component finds it and solves
    CHECK(Length(s.BoneWorld(3) - Float3{6, 4, 0}) < 1.0e-3f);

    for (const Float3 at : {Float3{7, 4, 1}, Float3{4.5f, 5, -1}, Float3{6, 6, 2}})
    {
        Transform t;
        t.position = at;
        s.level.SetLocalTransform(target, t);
        s.Tick(); // the same frame's target: composed fresh in PostUpdate
        CHECK(Length(s.BoneWorld(3) - at) < 1.0e-3f);
    }
    // The rider walks on: the chain's start moves with it and the foot stays planted.
    Transform riderAt = s.level.GetLocalTransform(s.rider);
    riderAt.position.x += 0.5f;
    s.level.SetLocalTransform(s.rider, riderAt);
    s.Tick();
    CHECK(Length(s.BoneWorld(3) - Float3{6, 6, 2}) < 1.0e-3f);
}

TEST_CASE("ik component: the weight eases over fadeSeconds, and at zero the modifier leaves the player")
{
    Stage s;
    const scene::EntityHandle target = s.Target(u8"Step", Float3{6, 4, 0});
    engine::animation::TwoBoneIkComponent& leg = s.Leg(u8"LegIk", target);
    leg.fadeSeconds = 0.5f;
    const scene::EntityHandle owner = OwnerOf(s, u8"LegIk");
    auto* legs = s.level.GetSystem<engine::animation::TwoBoneIkComponentManager>();
    s.Tick(0.1f);
    for (i32 frame = 1; frame <= 5; ++frame)
    {
        s.Tick(0.1f);
        CHECK(legs->Get(owner)->runtime.weight == doctest::Approx(0.2f * static_cast<f32>(frame)));
    }
    CHECK(Length(s.BoneWorld(3) - Float3{6, 4, 0}) < 1.0e-3f); // fully on
    CHECK(s.Player().Modifiers().Count() == 1u);

    legs->Get(owner)->active = false;
    s.Tick(0.25f);
    CHECK(legs->Get(owner)->runtime.weight == doctest::Approx(0.5f));
    CHECK(Length(s.BoneWorld(3) - Float3{6, 4, 0}) > 1.0e-2f); // half way back
    s.Tick(0.25f);
    CHECK(legs->Get(owner)->runtime.weight == 0.0f);
    CHECK(s.Player().Modifiers().IsEmpty()); // no solve, no model-space build
}

TEST_CASE("ik component: components on one animator run in their order")
{
    // Two chains on the same leg reaching for different points: the later one has the last word.
    Stage s;
    const scene::EntityHandle a = s.Target(u8"A", Float3{6, 4, 0});
    const scene::EntityHandle b = s.Target(u8"B", Float3{7, 5, 1});
    engine::animation::TwoBoneIkComponent& first = s.Leg(u8"IkA", a, 0);
    (void)first;
    (void)s.Leg(u8"IkB", b, 1);
    s.Tick();
    s.Tick();
    CHECK(Length(s.BoneWorld(3) - Float3{7, 5, 1}) < 1.0e-3f);

    auto* legs = s.level.GetSystem<engine::animation::TwoBoneIkComponentManager>();
    legs->Get(OwnerOf(s, u8"IkA"))->order = 2; // A now runs after B
    s.Tick();
    CHECK(Length(s.BoneWorld(3) - Float3{6, 4, 0}) < 1.0e-3f);
    CHECK(s.Player().Modifiers().Count() == 2u);
}

TEST_CASE("ik component: an unknown bone or no animator disables it with one log line")
{
    CountingSink sink;
    GlobalLogger().AddSink(&sink);
    {
        Stage s;
        const scene::EntityHandle target = s.Target(u8"Step", Float3{6, 4, 0});
        engine::animation::TwoBoneIkComponent& leg = s.Leg(u8"LegIk", target);
        leg.midBone = String(u8"Knee"); // not in the skeleton
        for (i32 frame = 0; frame < 10; ++frame)
        {
            s.Tick();
        }
        auto* legs = s.level.GetSystem<engine::animation::TwoBoneIkComponentManager>();
        CHECK(legs->Get(OwnerOf(s, u8"LegIk"))->runtime.status == IkStatus::UnknownBone);
        CHECK(sink.lines == 1u);
        CHECK(s.Player().Modifiers().IsEmpty());

        // Fixed, it solves; an IK entity with no animator above it logs once more.
        legs->Get(OwnerOf(s, u8"LegIk"))->midBone = String(u8"Shin");
        legs->Get(OwnerOf(s, u8"LegIk"))->runtime.resolvedFor = nullptr; // an edit re-resolves
        s.Tick();
        CHECK(legs->Get(OwnerOf(s, u8"LegIk"))->runtime.status == IkStatus::Solving);
        const scene::EntityHandle stray = s.level.CreateEntity(u8"Stray");
        legs->Add(stray).startBone = String(u8"Thigh");
        for (i32 frame = 0; frame < 5; ++frame)
        {
            s.Tick();
        }
        CHECK(legs->Get(stray)->runtime.status == IkStatus::NoAnimator);
        CHECK(sink.lines == 2u);
    }
    GlobalLogger().RemoveSink(&sink);
}

TEST_CASE("ik component: the script calls set a point and read the result; removal leaves nothing behind")
{
    Stage s;
    (void)s.Leg(u8"LegIk", scene::EntityHandle::Invalid()); // no target entity: the script's point
    const scene::EntityHandle owner = OwnerOf(s, u8"LegIk");
    const foundation::script::Entity entity{&s.level, owner.index, owner.generation};
    const engine::animation::SceneAnimation animation{&s.level};
    animation.setIkTarget(entity, Float3{6, 4, 0});
    s.Tick();
    s.Tick();
    CHECK(animation.ikReached(entity));
    CHECK(animation.ikError(entity) < 1.0e-3f);
    CHECK(Length(s.BoneWorld(3) - Float3{6, 4, 0}) < 1.0e-3f);

    animation.setIkTarget(entity, Float3{30, 4, 0}); // out of reach
    s.Tick();
    CHECK_FALSE(animation.ikReached(entity));
    CHECK(animation.ikError(entity) > 10.0f);

    // An aim on the head alongside it: both must reach for ikReached.
    const scene::EntityHandle head = s.level.CreateEntity(u8"HeadIk");
    s.level.SetParent(head, s.rider);
    auto& aim = s.level.GetSystem<engine::animation::AimIkComponentManager>()->Add(head);
    aim.bones.PushBack(engine::animation::AimIkBone{String(u8"Head"), 1.0f});
    aim.fadeSeconds = 0.0f;
    const foundation::script::Entity headEntity{&s.level, head.index, head.generation};
    animation.setIkTarget(headEntity, s.BoneWorld(5) + Float3{1, 0, 3});
    s.Tick();
    CHECK(animation.ikReached(headEntity));
    CHECK(s.Player().Modifiers().Count() == 2u);

    // Removing the components takes their modifiers off the player (it holds them borrowed).
    s.level.GetSystem<engine::animation::TwoBoneIkComponentManager>()->Remove(owner);
    CHECK(s.Player().Modifiers().Count() == 1u);
    s.level.DestroyEntity(head);
    s.Tick();
    CHECK(s.Player().Modifiers().IsEmpty());
    s.Tick(); // and the player runs on with nothing dangling
    CHECK_FALSE(animation.ikReached(headEntity));
}

namespace
{
    // Flat ground at `height` everywhere, answered as the scene's solid-surface ray query (the seam
    // physics fills in a running game); counts the rays it is asked.
    class FlatGround final : public scene::SceneSystem, public scene::ISceneRayQuery
    {
    public:
        [[nodiscard]] scene::ISceneRayQuery* AsRayQuery() noexcept override { return this; }
        bool CastRay(Float3 origin, Float3 direction, f32 maxDistance, u32, scene::SceneRayHit& out) override
        {
            ++casts;
            if (direction.y >= -1.0e-6f)
            {
                return false;
            }
            const f32 t = (origin.y - height) / -direction.y;
            if (t < 0.0f || t > maxDistance)
            {
                return false;
            }
            out.distance = t;
            out.position = origin + direction * t;
            out.normal = Float3{0, 1, 0};
            return true;
        }
        f32 height = 0.0f;
        usize casts = 0;
    };

    // A biped's hips and legs (origin at its feet, ankles 0.1 up) under an animator on Walker.
    RefPtr<animation::Skeleton> MakeBiped()
    {
        RefPtr<animation::Skeleton> s = MakeRef<animation::Skeleton>(DefaultAllocator(), 7);
        Array<animation::Bone>& bones = s->Bones();
        const char8_t* names[] = {u8"Hips", u8"ThighL", u8"ShinL", u8"FootL", u8"ThighR", u8"ShinR", u8"FootR"};
        const i32 parents[] = {-1, 0, 1, 2, 0, 4, 5};
        const Float3 offsets[] = {{0, 1, 0},          {0.15f, 0, 0},      {0, -0.45f, 0.02f}, {0, -0.45f, -0.02f},
                                  {-0.15f, 0, 0},     {0, -0.45f, 0.02f}, {0, -0.45f, -0.02f}};
        for (i32 i = 0; i < 7; ++i)
        {
            bones[static_cast<usize>(i)].index = i;
            bones[static_cast<usize>(i)].name = String(names[i]);
            bones[static_cast<usize>(i)].parentIndex = parents[i];
            bones[static_cast<usize>(i)].localBindPose.position = offsets[i];
        }
        s->BuildNameMap();
        s->FindRootBones();
        s->BuildChildIndices();
        s->ComputeInverseBindPoses();
        return s;
    }
}

TEST_CASE("ik foot component: feet stand on the ground the scene's ray query finds, probed once a frame")
{
    scene::Scene level{DefaultAllocator(), u8"feet"};
    level.AddSystem<engine::render::MeshComponentManager>();
    engine::animation::AddAnimationSceneManagers(level);
    FlatGround* ground = level.AddSystem<FlatGround>();
    RefPtr<animation::Skeleton> skeleton = MakeBiped();

    const scene::EntityHandle walker = level.CreateEntity(u8"Walker");
    Transform at;
    at.position = Float3{2, 0, 1};
    at.rotation = Quaternion::FromAxisAngle(Float3{0, 1, 0}, 0.6f);
    level.SetLocalTransform(walker, at);
    auto& animator = level.GetSystem<engine::animation::SkeletalAnimationComponentManager>()->Add(walker);
    animator.skeleton.SetDirect(skeleton);
    const scene::EntityHandle feet = level.CreateEntity(u8"FeetIk");
    level.SetParent(feet, walker);
    auto& foot = level.GetSystem<engine::animation::FootIkComponentManager>()->Add(feet);
    foot.legs.PushBack(engine::animation::FootIkLegBones{String(u8"ThighL"), String(u8"ShinL"), String(u8"FootL"), {}});
    foot.legs.PushBack(engine::animation::FootIkLegBones{String(u8"ThighR"), String(u8"ShinR"), String(u8"FootR"), {}});
    foot.pelvisBone = String(u8"Hips");
    foot.fadeSeconds = 0.0f;

    animation::AnimationPlayer* player = nullptr;
    auto footWorldY = [&](i32 bone)
    {
        (void)player->GetSkinningMatrices();
        animation::ModelPoseCache cache;
        cache.Build(*skeleton, player->GetFinalPoses());
        const Float4x4& m = cache.At(bone);
        return TransformPoint(Float3{m.m[3][0], m.m[3][1], m.m[3][2]}, level.GetWorldMatrix(walker)).y;
    };

    ground->height = 0.25f; // a raised floor: both feet rise onto it, the pelvis stays
    level.Update(1.0f / 60.0f);
    player = animator.player.Get();
    REQUIRE(player != nullptr);
    level.Update(1.0f / 60.0f);
    const usize castsBefore = ground->casts;
    CHECK(footWorldY(3) == doctest::Approx(0.35f).epsilon(1e-4));
    CHECK(footWorldY(6) == doctest::Approx(0.35f).epsilon(1e-4));
    // Each read above evaluated the player again: the hits were reused, no ray cast twice.
    CHECK(ground->casts == castsBefore);
    level.Update(1.0f / 60.0f);
    (void)player->GetSkinningMatrices();
    CHECK(ground->casts == castsBefore + 2u); // one per foot per frame

    // A floor below the animation's: the pelvis drops to it (clamped at pelvisDropMax 0.3).
    ground->height = -0.2f;
    for (i32 frame = 0; frame < 120; ++frame)
    {
        level.Update(1.0f / 60.0f);
    }
    CHECK(footWorldY(3) == doctest::Approx(-0.1f).epsilon(1e-3));
    CHECK(footWorldY(0) == doctest::Approx(0.8f).epsilon(1e-3));
}
