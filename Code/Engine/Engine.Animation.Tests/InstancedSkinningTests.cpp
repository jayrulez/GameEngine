// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Instanced skinning with several clips: the pose pool holds a block of phases
// per clip, each clip on its own clock, and the instanced meshes it feeds learn how many poses a
// clip has, so an instance's clip picks its block; the skeleton and clips are asset references and
// the targets entity references, saved with the scene.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.scene;
import foundation.scene.resource;
import foundation.animation;
import engine.animation;
import engine.render;
import foundation.render; // SelectPose (how an instance picks its pose)

using namespace foundation::core;
namespace scene = foundation::scene;
namespace animation = foundation::animation;

namespace
{
    // A clip holding the one bone at `x` the whole time.
    RefPtr<animation::AnimationClip> HoldAt(StringView name, f32 x)
    {
        RefPtr<animation::AnimationClip> clip = MakeRef<animation::AnimationClip>(DefaultAllocator(), name, 1.0f, true);
        clip->GetOrCreatePositionTrack(0)->AddKeyframe(0.0f, Float3{x, 0, 0});
        clip->GetOrCreatePositionTrack(0)->AddKeyframe(1.0f, Float3{x, 0, 0});
        return clip;
    }

    RefPtr<animation::Skeleton> OneBone()
    {
        RefPtr<animation::Skeleton> skeleton = MakeRef<animation::Skeleton>(DefaultAllocator(), 1);
        skeleton->Bones()[0].index = 0;
        skeleton->Bones()[0].parentIndex = -1;
        skeleton->FindRootBones();
        skeleton->BuildChildIndices();
        return skeleton;
    }
}

TEST_CASE("instanced skinning: a block of poses per clip, and the mesh told how many a clip has")
{
    scene::Scene level{DefaultAllocator(), u8"crowd"};
    level.AddSystem<engine::render::InstancedMeshComponentManager>();
    engine::animation::AddAnimationSceneManagers(level);
    const scene::EntityHandle crowd = level.CreateEntity(u8"Guests");
    engine::render::InstancedMeshComponent& mesh =
        level.GetSystem<engine::render::InstancedMeshComponentManager>()->Add(crowd);
    auto& skinning = level.GetSystem<engine::animation::InstancedSkinningComponentManager>()->Add(crowd);
    skinning.skeleton.SetDirect(OneBone());
    skinning.clips.Resize(2);
    skinning.clips[0].SetDirect(HoldAt(u8"Idle", 1.0f));
    skinning.clips[1].SetDirect(HoldAt(u8"Dance", 2.0f));
    skinning.poseCount = 3;

    level.Update(1.0f / 30.0f);
    REQUIRE(mesh.posePool != nullptr);
    CHECK(mesh.poseCount == 6u);    // two clips x three phases
    CHECK(mesh.posesPerClip == 3u); // so an instance's clip picks its block
    CHECK(mesh.boneCount == 1u);
    for (u32 pose = 0; pose < 6; ++pose)
    {
        const f32 x = mesh.posePool[pose].m[3][0];
        CHECK(x == doctest::Approx(pose < 3 ? 1.0f : 2.0f)); // Idle's block, then Dance's
    }
    // An instance on clip 1 draws from Dance's block, whatever the policy picks for its phase.
    CHECK(foundation::render::SelectPose(foundation::render::PoseAssignment::Hashed, 7, mesh.poseCount, nullptr, 1,
                                         mesh.posesPerClip) >= 3u);
}

TEST_CASE("instanced skinning: the skeleton, clips and targets are saved with the scene")
{
    Random rng(11);
    const Guid skeletonId = Guid::Generate(rng);
    const Guid idleId = Guid::Generate(rng);
    const Guid danceId = Guid::Generate(rng);
    MemoryStream blob;
    Guid partId;
    {
        scene::Scene level{DefaultAllocator(), u8"crowd"};
        level.AddSystem<engine::render::InstancedMeshComponentManager>();
        engine::animation::AddAnimationSceneManagers(level);
        const scene::EntityHandle crowd = level.CreateEntity(u8"Guests");
        const scene::EntityHandle part = level.CreateEntity(u8"Masks");
        partId = level.GetEntityId(part);
        auto& skinning = level.GetSystem<engine::animation::InstancedSkinningComponentManager>()->Add(crowd);
        skinning.skeleton.SetId(skeletonId);
        skinning.clips.Resize(2);
        skinning.clips[0].SetId(idleId);
        skinning.clips[1].SetId(danceId);
        skinning.poseCount = 5;
        skinning.speed = 1.5f;
        skinning.targets.PushBack(partId);
        BinarySerializer ar(blob, SerializeMode::Write);
        scene::SerializeScene(ar, level);
        REQUIRE(ar.IsOk());
    }
    scene::Scene loaded{DefaultAllocator(), u8"crowd"};
    loaded.AddSystem<engine::render::InstancedMeshComponentManager>();
    engine::animation::AddAnimationSceneManagers(loaded);
    REQUIRE(blob.Seek(0, SeekOrigin::Begin) == 0);
    {
        BinarySerializer ar(blob, SerializeMode::Read);
        scene::SerializeScene(ar, loaded);
        REQUIRE(ar.IsOk());
    }
    const engine::animation::InstancedSkinningComponent* skinning = nullptr;
    loaded.GetSystem<engine::animation::InstancedSkinningComponentManager>()->ForEach(
        [&](engine::animation::InstancedSkinningComponent& s, scene::EntityHandle) { skinning = &s; });
    REQUIRE(skinning != nullptr);
    CHECK(skinning->skeleton.id == skeletonId);
    REQUIRE(skinning->clips.Size() == 2u);
    CHECK(skinning->clips[0].id == idleId);
    CHECK(skinning->clips[1].id == danceId);
    CHECK(skinning->poseCount == 5u);
    CHECK(skinning->speed == doctest::Approx(1.5f));
    REQUIRE(skinning->targets.Size() == 1u);
    CHECK(skinning->targets[0].id == partId);
}
