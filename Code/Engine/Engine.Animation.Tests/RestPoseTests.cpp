// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The rest pose: a skinned mesh fed no palette draws its raw vertices, which an FBX file keeps in
// its authoring tool's axes (the bind matrices carry the turn upright). So an animator feeds its
// meshes the player's standing pose even while the scene stands still (the editor's edit mode, a
// thumbnail), the clip waiting for the simulation, and a graph player starts in the rest pose.
#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.animation;
import foundation.scene;
import engine.animation;
import engine.render;

using namespace foundation::core;
namespace scene = foundation::scene;
namespace animation = foundation::animation;

namespace
{
    // One bone at the origin whose inverse bind turns a quarter about X: the palette at rest is
    // that turn, what stands a Z-up FBX mesh upright.
    RefPtr<animation::Skeleton> TurnedRig()
    {
        RefPtr<animation::Skeleton> skeleton = MakeRef<animation::Skeleton>(DefaultAllocator(), 1);
        skeleton->Bones()[0].index = 0;
        skeleton->Bones()[0].parentIndex = -1;
        skeleton->Bones()[0].inverseBindPose = Float4x4::RotationX(-kHalfPi);
        skeleton->FindRootBones();
        skeleton->BuildChildIndices();
        return skeleton;
    }

    Float3 Moved(const Float4x4& m) { return TransformPoint(Float3{0, 0, 1}, m); }
}

TEST_CASE("rest pose: a standing scene's animator feeds the rest pose and holds its clip")
{
    RefPtr<animation::Skeleton> skeleton = TurnedRig();
    RefPtr<animation::AnimationClip> clip = MakeRef<animation::AnimationClip>(DefaultAllocator(), u8"Lift", 1.0f, true);
    clip->GetOrCreatePositionTrack(0)->AddKeyframe(0.0f, Float3{0, 2, 0});
    clip->GetOrCreatePositionTrack(0)->AddKeyframe(1.0f, Float3{0, 2, 0});

    scene::Scene level{DefaultAllocator(), u8"rest"};
    auto* meshes = level.AddSystem<engine::render::MeshComponentManager>();
    engine::animation::AddAnimationSceneManagers(level);
    const scene::EntityHandle body = level.CreateEntity(u8"Body");
    meshes->Add(body);
    auto& animator = level.GetSystem<engine::animation::SkeletalAnimationComponentManager>()->Add(body);
    animator.skeleton.SetDirect(skeleton);
    animator.clip.SetDirect(clip);

    level.SetSimulationEnabled(false);
    level.Update(0.5f);
    const engine::render::MeshComponent* mesh = meshes->Get(body);
    REQUIRE(mesh->boneMatrices != nullptr);
    REQUIRE(mesh->boneCount == 1u);
    const Float3 rest = Moved(Float4x4::RotationX(-kHalfPi));
    CHECK(Moved(mesh->boneMatrices[0]).y == doctest::Approx(rest.y)); // turned, not raw
    CHECK(Moved(mesh->boneMatrices[0]).z == doctest::Approx(rest.z));
    REQUIRE(animator.player.Get() != nullptr);
    CHECK(animator.player->CurrentClip() == nullptr); // the clip waits for the simulation
    CHECK(animator.player->CurrentTime() == doctest::Approx(0.0f));

    // Simulating, the clip plays: the bone lifts by two.
    level.SetSimulationEnabled(true);
    level.Update(0.25f);
    CHECK(animator.player->CurrentClip() == clip.Get());
    CHECK(Moved(meshes->Get(body)->boneMatrices[0]).y == doctest::Approx(rest.y + 2.0f));
}

TEST_CASE("rest pose: an animator with no clip feeds the rest pose while simulating too")
{
    RefPtr<animation::Skeleton> skeleton = TurnedRig();
    scene::Scene level{DefaultAllocator(), u8"rest"};
    auto* meshes = level.AddSystem<engine::render::MeshComponentManager>();
    engine::animation::AddAnimationSceneManagers(level);
    const scene::EntityHandle body = level.CreateEntity(u8"Body");
    meshes->Add(body);
    level.GetSystem<engine::animation::SkeletalAnimationComponentManager>()->Add(body).skeleton.SetDirect(skeleton);
    level.Update(0.5f);
    REQUIRE(meshes->Get(body)->boneMatrices != nullptr);
    CHECK(Moved(meshes->Get(body)->boneMatrices[0]).y == doctest::Approx(Moved(Float4x4::RotationX(-kHalfPi)).y));
}

TEST_CASE("rest pose: a graph player starts in the rig's rest pose, fed while the scene stands still")
{
    RefPtr<animation::Skeleton> skeleton = TurnedRig();
    skeleton->Bones()[0].localBindPose.position = Float3{0, 3, 0};
    RefPtr<animation::AnimationGraph> graph = MakeRef<animation::AnimationGraph>(DefaultAllocator());

    scene::Scene level{DefaultAllocator(), u8"rest"};
    auto* meshes = level.AddSystem<engine::render::MeshComponentManager>();
    engine::animation::AddAnimationSceneManagers(level);
    const scene::EntityHandle body = level.CreateEntity(u8"Body");
    meshes->Add(body);
    auto& component = level.GetSystem<engine::animation::AnimationGraphComponentManager>()->Add(body);
    component.skeleton.SetDirect(skeleton);
    component.graph.SetDirect(graph);
    level.SetSimulationEnabled(false);
    level.Update(0.5f);
    REQUIRE(meshes->Get(body)->boneMatrices != nullptr);
    // Its rest position (three up) under the turn, not the identity a default pose gives.
    CHECK(Moved(meshes->Get(body)->boneMatrices[0]).y ==
          doctest::Approx(Moved(Float4x4::RotationX(-kHalfPi)).y + 3.0f));
}
