// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Resource-reference layer: SkeletalAnimationComponent's skeleton/clip resource::Ref fields
// round-trip through scene serialization by Guid and resolve through the ResourceManager's
// proxy handles, as the MeshComponent tests in Engine.Render.Tests do.

#include <doctest/doctest.h>

#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.resource;
import foundation.animation;
import foundation.animation.resource;
import engine.animation;
import engine.render; // MeshComponentManager (the skeletal tick's feed target)
import foundation.scene;
import foundation.scene.resource;

using namespace foundation::core;
namespace scene = foundation::scene;
namespace resource = foundation::resource;
namespace animation = foundation::animation;

namespace
{
    void RemoveTree(StringView root)
    {
        foundation::vfs::NativeFileSystem fs(root, foundation::core::DefaultAllocator());
        Array<foundation::vfs::DirEntry> entries;
        if (fs.AsEnumerable()->Enumerate(u8"", entries).IsOk())
        {
            for (const auto& e : entries)
            {
                if (!e.isDirectory)
                {
                    (void)fs.AsWritable()->Delete(e.name.AsView());
                }
            }
        }
        (void)RemoveDirectory(root);
    }

    // A 2-bone chain root(0) -> child(1), enough structure to assert survival through the cook.
    void BuildSkeleton(animation::Skeleton& s)
    {
        Array<animation::Bone>& bones = s.Bones();
        bones[0].index = 0;
        bones[0].parentIndex = -1;
        bones[0].name = String{u8"root"};
        bones[1].index = 1;
        bones[1].parentIndex = 0;
        bones[1].name = String{u8"child"};
        bones[1].localBindPose.position = Float3{0, 5, 0};
        s.BuildNameMap();
        s.FindRootBones();
        s.BuildChildIndices();
    }
}

TEST_CASE("resource-ref: scene round-trip resolves skeleton + clip refs through proxy handles")
{
    const StringView dir = u8"scratch_animref_test_db";
    RemoveTree(dir);
    (void)CreateDirectory(dir);
    foundation::vfs::NativeFileSystem mount(dir, foundation::core::DefaultAllocator());

    GlobalTypeRegistry().Register(animation::SkeletonSource::StaticType());
    RegisterSerializable<animation::SkeletonSource>();
    GlobalTypeRegistry().Register(animation::AnimationClipSource::StaticType());
    RegisterSerializable<animation::AnimationClipSource>();

    // Cook a skeleton and a clip into the content DB (the products a model import fans out).
    foundation::content::ContentDatabase cookedDb(foundation::core::DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");
    Guid skeletonId;
    Guid clipId;
    {
        animation::Skeleton skel{2};
        BuildSkeleton(skel);
        animation::SkeletonSource source;
        animation::SkeletonSource::FromSkeleton(skel, source);
        foundation::content::Instance* inst =
            cookedDb.RootGroup()->CreateInstance(u8"Skel", animation::SkeletonSource::StaticType());
        REQUIRE(inst != nullptr);
        REQUIRE(inst->WriteObject(source).IsOk());
        skeletonId = inst->Id();
    }
    {
        animation::AnimationClip clip;
        clip.Name() = String{u8"walk"};
        clip.duration = 2.0f;
        clip.isLooping = true;
        animation::AnimationTrack<Float3>* track = clip.GetOrCreatePositionTrack(1);
        track->AddKeyframe(0.0f, Float3{0, 0, 0});
        track->AddKeyframe(2.0f, Float3{0, 1, 0});
        animation::AnimationClipSource source;
        animation::AnimationClipSource::FromClip(clip, source);
        foundation::content::Instance* inst = cookedDb.RootGroup()->CreateInstance(
            u8"Walk", animation::AnimationClipSource::StaticType());
        REQUIRE(inst != nullptr);
        REQUIRE(inst->WriteObject(source).IsOk());
        clipId = inst->Id();
    }

    resource::ResourceManager resources(DefaultAllocator(), cookedDb);
    animation::SkeletonFactory skeletonFactory(DefaultAllocator());
    animation::AnimationClipFactory clipFactory(DefaultAllocator());
    resources.AddFactory(&skeletonFactory);
    resources.AddFactory(&clipFactory);

    // Author a scene whose SkeletalAnimationComponent references both by Guid only.
    MemoryStream blob;
    {
        scene::Scene scene{DefaultAllocator()};
        scene.AddSystem<engine::animation::SkeletalAnimationComponentManager>();
        const scene::EntityHandle e = scene.CreateEntity(u8"Rig");
        engine::animation::SkeletalAnimationComponent& a =
            scene.GetSystem<engine::animation::SkeletalAnimationComponentManager>()->Add(e);
        a.skeleton.SetId(skeletonId);
        a.clip.SetId(clipId);
        a.speed = 1.5f;
        a.startTime = 0.25f;

        BinarySerializer ar(blob, SerializeMode::Write);
        scene::SerializeScene(ar, scene);
        REQUIRE(ar.IsOk());
    }

    // Load into a fresh scene, then run the post-load resolve pass.
    scene::Scene loaded{DefaultAllocator()};
    loaded.AddSystem<engine::render::MeshComponentManager>();
    loaded.AddSystem<engine::animation::SkeletalAnimationComponentManager>();
    REQUIRE(blob.Seek(0, SeekOrigin::Begin) == 0);
    {
        BinarySerializer ar(blob, SerializeMode::Read);
        scene::SerializeScene(ar, loaded);
        REQUIRE(ar.IsOk());
    }

    auto* mgr = loaded.GetSystem<engine::animation::SkeletalAnimationComponentManager>();
    REQUIRE(mgr != nullptr);
    REQUIRE(mgr->ComponentCount() == 1u);
    engine::animation::SkeletalAnimationComponent* a = nullptr;
    mgr->ForEach([&](engine::animation::SkeletalAnimationComponent& c, scene::EntityHandle) { a = &c; });
    REQUIRE(a != nullptr);

    // Identity and tunables survived; nothing is bound until the resolve pass runs.
    CHECK(a->skeleton.id == skeletonId);
    CHECK(a->clip.id == clipId);
    CHECK(a->skeleton.Get() == nullptr);
    CHECK(a->clip.Get() == nullptr);
    CHECK(a->speed == doctest::Approx(1.5f));
    CHECK(a->startTime == doctest::Approx(0.25f));

    scene::ResolveSceneResources(loaded, resources);
    animation::Skeleton* skel = a->skeleton.Get();
    REQUIRE(skel != nullptr);
    CHECK(skel->BoneCount() == 2);
    CHECK(skel->FindBone(u8"child") == 1);
    animation::AnimationClip* clip = a->clip.Get();
    REQUIRE(clip != nullptr);
    CHECK(clip->duration == doctest::Approx(2.0f));
    CHECK(clip->isLooping);

    // The close-and-reopen editor flow: after load and resolve, the first tick builds the player
    // and autoPlay starts the persisted clip at the persisted startTime.
    CHECK(a->player.Get() == nullptr);
    loaded.Update(1.0f / 60.0f);
    REQUIRE(a->player.Get() != nullptr);
    CHECK(a->player->CurrentClip() == clip);

    RemoveTree(dir);
}

TEST_CASE("resource-ref: raw runtime skeleton/clip pointers still assign (sample/spawn path)")
{
    // Samples hand the component objects that live behind proxies elsewhere: the raw-pointer
    // conversion must hold its own strong ref and skip serialization (nil id).
    RefPtr<animation::Skeleton> skel = MakeRef<animation::Skeleton>(DefaultAllocator(), 2);
    BuildSkeleton(*skel);
    RefPtr<animation::AnimationClip> clip = MakeRef<animation::AnimationClip>(DefaultAllocator());
    clip->duration = 1.0f;

    engine::animation::SkeletalAnimationComponent a;
    a.skeleton = skel.Get();
    a.clip = clip.Get();
    CHECK(a.skeleton.Get() == skel.Get());
    CHECK(a.clip.Get() == clip.Get());
    CHECK(a.skeleton.id.IsNil());
    CHECK(a.clip.id.IsNil());
}

TEST_CASE("resource-ref: sequential picks start playback (skeleton first, clip later)")
{
    // The editor flow: refs land one at a time across frames. The manager must start playback
    // when the clip arrives after the player was already built for the skeleton, and replay when
    // the clip behind the ref changes (a hot reload or a different pick).
    scene::Scene scene{DefaultAllocator()};
    scene.AddSystem<engine::render::MeshComponentManager>();
    auto* mgr = scene.AddSystem<engine::animation::SkeletalAnimationComponentManager>();
    const scene::EntityHandle e = scene.CreateEntity(u8"Rig");

    RefPtr<animation::Skeleton> skel = MakeRef<animation::Skeleton>(DefaultAllocator(), 2);
    BuildSkeleton(*skel);
    RefPtr<animation::AnimationClip> walk = MakeRef<animation::AnimationClip>(DefaultAllocator());
    walk->duration = 1.0f;
    RefPtr<animation::AnimationClip> run = MakeRef<animation::AnimationClip>(DefaultAllocator());
    run->duration = 0.5f;

    engine::animation::SkeletalAnimationComponent& a = mgr->Add(e);

    // Frame 1: only the skeleton is picked; a player exists but nothing plays.
    a.skeleton = skel.Get();
    scene.Update(1.0f / 60.0f);
    REQUIRE(a.player.Get() != nullptr);
    CHECK(a.player->CurrentClip() == nullptr);

    // Frame 2: the clip pick lands, and autoPlay must start it on the existing player.
    a.clip = walk.Get();
    scene.Update(1.0f / 60.0f);
    CHECK(a.player->CurrentClip() == walk.Get());

    // Picking a different clip replays (the same mechanism covers hot reload's product swap).
    a.clip = run.Get();
    scene.Update(1.0f / 60.0f);
    CHECK(a.player->CurrentClip() == run.Get());
}

TEST_CASE("mesh-entities: one animator feeds multiple parts by EntityRef, and the refs persist")
{
    RefPtr<animation::Skeleton> skel = MakeRef<animation::Skeleton>(DefaultAllocator(), 2);
    BuildSkeleton(*skel);
    RefPtr<animation::AnimationClip> clip = MakeRef<animation::AnimationClip>(DefaultAllocator());
    clip->duration = 1.0f;
    clip->isLooping = true;

    scene::Scene scene{DefaultAllocator()};
    auto* meshes = scene.AddSystem<engine::render::MeshComponentManager>();
    auto* mgr = scene.AddSystem<engine::animation::SkeletalAnimationComponentManager>();
    const scene::EntityHandle rig = scene.CreateEntity(u8"Rig");
    const scene::EntityHandle partA = scene.CreateEntity(u8"PartA");
    const scene::EntityHandle partB = scene.CreateEntity(u8"PartB");
    (void)meshes->Add(partA);
    (void)meshes->Add(partB);

    // The multi-part character: one animator on the rig, parts referenced by stable guid.
    engine::animation::SkeletalAnimationComponent& a = mgr->Add(rig);
    a.skeleton = skel.Get();
    a.clip = clip.Get();
    a.meshEntities.PushBack(scene.GetEntityId(partA));
    a.meshEntities.PushBack(scene.GetEntityId(partB));

    scene.Update(1.0f / 60.0f);
    engine::render::MeshComponent* fedA = meshes->Get(partA);
    engine::render::MeshComponent* fedB = meshes->Get(partB);
    REQUIRE(fedA != nullptr);
    REQUIRE(fedB != nullptr);
    CHECK(fedA->boneMatrices != nullptr); // both parts received the one player's matrices
    CHECK(fedB->boneMatrices != nullptr);
    CHECK(fedA->boneCount == 2u);
    CHECK(fedA->boneMatrices == fedB->boneMatrices); // same evaluation, not per-part players

    // The refs are part of the component's persisted state (data version 1): a scene round-trip
    // keeps them, still naming the same entities by guid.
    MemoryStream blob;
    {
        BinarySerializer ar(blob, SerializeMode::Write);
        scene::SerializeScene(ar, scene);
        REQUIRE(ar.IsOk());
    }
    scene::Scene loaded{DefaultAllocator()};
    loaded.AddSystem<engine::render::MeshComponentManager>();
    auto* loadedMgr = loaded.AddSystem<engine::animation::SkeletalAnimationComponentManager>();
    REQUIRE(blob.Seek(0, SeekOrigin::Begin) == 0);
    {
        BinarySerializer ar(blob, SerializeMode::Read);
        scene::SerializeScene(ar, loaded);
        REQUIRE(ar.IsOk());
    }
    engine::animation::SkeletalAnimationComponent* la = nullptr;
    loadedMgr->ForEach(
        [&](engine::animation::SkeletalAnimationComponent& c, scene::EntityHandle) { la = &c; });
    REQUIRE(la != nullptr);
    REQUIRE(la->meshEntities.Size() == 2u);
    CHECK(loaded.GetEntityName(loaded.FindEntity(la->meshEntities[0].id)) ==
          StringView(u8"PartA"));
    CHECK(loaded.GetEntityName(loaded.FindEntity(la->meshEntities[1].id)) ==
          StringView(u8"PartB"));
}
