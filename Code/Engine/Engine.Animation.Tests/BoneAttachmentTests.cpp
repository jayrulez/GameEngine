// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Bone attachments: an entity that follows one bone of the animator above it (a weapon in a hand),
// its children riding along, while the scene runs. Standing still it stays where its node put it
// (the rest pose's place); an unknown bone leaves it there.
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
    Float3 Position(const Float4x4& m) { return Float3{m.m[3][0], m.m[3][1], m.m[3][2]}; }

    // A character moved ten along X: an animator on its root playing a clip that slides the root
    // bone two along X; a hand bone one above it; the hand's node with an attachment, a sword one
    // along Z under it; the skinned body (the model entity) under the root at identity.
    struct Stage
    {
        scene::Scene level{DefaultAllocator(), u8"attach"};
        RefPtr<animation::Skeleton> skeleton = MakeRef<animation::Skeleton>(DefaultAllocator(), 2);
        RefPtr<animation::AnimationClip> clip = MakeRef<animation::AnimationClip>(DefaultAllocator(), u8"Slide", 1.0f, true);
        scene::EntityHandle root;
        scene::EntityHandle hand;
        scene::EntityHandle sword;

        explicit Stage(StringView bone)
        {
            skeleton->Bones()[0].index = 0;
            skeleton->Bones()[0].parentIndex = -1;
            skeleton->Bones()[0].name = String(u8"Root");
            skeleton->Bones()[1].index = 1;
            skeleton->Bones()[1].parentIndex = 0;
            skeleton->Bones()[1].name = String(u8"Hand");
            skeleton->Bones()[1].localBindPose.position = Float3{0, 1, 0};
            skeleton->BuildNameMap();
            skeleton->FindRootBones();
            skeleton->BuildChildIndices();
            skeleton->ComputeInverseBindPoses();
            clip->GetOrCreatePositionTrack(0)->AddKeyframe(0.0f, Float3{2, 0, 0});
            clip->GetOrCreatePositionTrack(0)->AddKeyframe(1.0f, Float3{2, 0, 0});

            level.AddSystem<engine::render::MeshComponentManager>();
            engine::animation::AddAnimationSceneManagers(level);
            root = level.CreateEntity(u8"Knight");
            Transform at;
            at.position = Float3{10, 0, 0};
            level.SetLocalTransform(root, at);
            const scene::EntityHandle body = level.CreateEntity(u8"Body");
            level.SetParent(body, root);
            auto& animator = level.GetSystem<engine::animation::SkeletalAnimationComponentManager>()->Add(root);
            animator.skeleton.SetDirect(skeleton);
            animator.clip.SetDirect(clip);
            animator.meshEntities.PushBack(level.GetEntityId(body));

            hand = level.CreateEntity(u8"Hand");
            level.SetParent(hand, root);
            Transform rest;
            rest.position = Float3{0, 1, 0};
            level.SetLocalTransform(hand, rest);
            level.GetSystem<engine::animation::BoneAttachmentComponentManager>()->Add(hand).bone = String(bone);
            sword = level.CreateEntity(u8"Sword");
            level.SetParent(sword, hand);
            Transform held;
            held.position = Float3{0, 0, 1};
            level.SetLocalTransform(sword, held);
            level.UpdateTransforms();
        }
    };
}

TEST_CASE("bone attachment: an entity follows its bone while the scene runs, its children along")
{
    Stage stage(u8"Hand");
    stage.level.Update(1.0f / 30.0f);
    // The root bone slid two along X, so the hand stands at 12,1,0 in the world, the sword at 12,1,1.
    const Float3 hand = Position(stage.level.GetWorldMatrix(stage.hand));
    CHECK(hand.x == doctest::Approx(12.0f));
    CHECK(hand.y == doctest::Approx(1.0f));
    const Float3 sword = Position(stage.level.GetWorldMatrix(stage.sword));
    CHECK(sword.x == doctest::Approx(12.0f));
    CHECK(sword.z == doctest::Approx(1.0f));
    CHECK(stage.level.GetSystem<engine::animation::BoneAttachmentComponentManager>()->Get(stage.hand)->runtime.status ==
          engine::animation::BoneAttachmentStatus::Following);

    // The character moves: the hand comes along the same frame.
    Transform moved;
    moved.position = Float3{20, 0, 0};
    stage.level.SetLocalTransform(stage.root, moved);
    stage.level.Update(1.0f / 30.0f);
    CHECK(Position(stage.level.GetWorldMatrix(stage.hand)).x == doctest::Approx(22.0f));
}

TEST_CASE("bone attachment: standing still, or naming no bone, it stays where its node put it")
{
    Stage still(u8"Hand");
    still.level.SetSimulationEnabled(false);
    still.level.Update(1.0f / 30.0f);
    CHECK(Position(still.level.GetWorldMatrix(still.hand)).x == doctest::Approx(10.0f));

    Stage unknown(u8"Tail");
    unknown.level.Update(1.0f / 30.0f);
    CHECK(Position(unknown.level.GetWorldMatrix(unknown.hand)).x == doctest::Approx(10.0f));
    CHECK(unknown.level.GetSystem<engine::animation::BoneAttachmentComponentManager>()->Get(unknown.hand)->runtime.status ==
          engine::animation::BoneAttachmentStatus::UnknownBone);
}
