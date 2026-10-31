// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Skeleton and clip resources: cook (capture into a source, then into the content DB), then build
// back through the ResourceManager via the factory and check the runtime types round-trip, a full
// content-DB round trip like the mesh resource test's.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.resource;
import foundation.animation;
import foundation.animation.resource;

using namespace foundation::core;
using namespace foundation::vfs;
using namespace foundation::resource;
using namespace foundation::animation;

namespace
{
    void RemoveTree()
    {
        FileDelete(u8"scratch_anim_res_db/skel.rasset");
        FileDelete(u8"scratch_anim_res_db/clip.rasset");
        RemoveDirectory(u8"scratch_anim_res_db");
    }

    // A two-bone chain: root(0) and child(1) under it, the child translated +Y.
    void BuildChain(Skeleton& s)
    {
        s.Bones()[0].index = 0;
        s.Bones()[0].parentIndex = -1;
        s.Bones()[0].name = String{u8"root"};
        s.Bones()[1].index = 1;
        s.Bones()[1].parentIndex = 0;
        s.Bones()[1].name = String{u8"child"};
        s.Bones()[1].localBindPose.position = Float3{0, 5, 0};
        s.BuildNameMap();
        s.FindRootBones();
        s.BuildChildIndices();
        s.ComputeInverseBindPoses();
    }
}

TEST_CASE("skeleton resource: round-trips through the resource manager")
{
    GlobalTypeRegistry().Register(SkeletonSource::StaticType());
    RegisterSerializable<SkeletonSource>();
    GlobalTypeRegistry().Register(Skeleton::StaticType());

    RemoveTree();
    NativeFileSystem mount(u8"scratch_anim_res_db", DefaultAllocator());

    Guid id;
    {
        foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                              u8".rasset");
        auto* inst = db.RootGroup()->CreateInstance(u8"skel", SkeletonSource::StaticType());
        id = inst->Id();

        Skeleton skel{2};
        BuildChain(skel);
        SkeletonSource src;
        SkeletonSource::FromSkeleton(skel, src);
        REQUIRE(inst->WriteObject(src).IsOk());
    }

    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                          u8".rasset");
    SkeletonFactory factory(DefaultAllocator());
    ResourceManager manager(DefaultAllocator(), db);
    manager.AddFactory(&factory);

    Proxy<Skeleton> skel = manager.Bind<Skeleton>(id);
    REQUIRE(skel);
    CHECK(skel->BoneCount() == 2);
    CHECK(skel->FindBone(u8"child") == 1); // name map rebuilt
    CHECK(skel->RootBones().Size() == 1);  // hierarchy rebuilt

    // Skinning at the bind pose is identity (inverse-bind reconstructed correctly).
    Float4x4 skin[2];
    skel->ComputeSkinningMatrices(Span<const BoneTransform>{}, Span<Float4x4>{skin, 2});
    CHECK(NearlyEqual(skin[1].m[0][0], 1.0f));
    CHECK(NearlyEqual(skin[1].m[3][1], 0.0f));

    RemoveTree();
}

TEST_CASE("animation clip resource: round-trips tracks + events")
{
    GlobalTypeRegistry().Register(AnimationClipSource::StaticType());
    RegisterSerializable<AnimationClipSource>();
    GlobalTypeRegistry().Register(AnimationClip::StaticType());

    RemoveTree();
    NativeFileSystem mount(u8"scratch_anim_res_db", DefaultAllocator());

    Guid id;
    {
        foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                              u8".rasset");
        auto* inst = db.RootGroup()->CreateInstance(u8"clip", AnimationClipSource::StaticType());
        id = inst->Id();

        AnimationClip clip{u8"move", 1.0f, /*looping*/ true};
        AnimationClip::Vec3Track* pos = clip.GetOrCreatePositionTrack(0);
        pos->AddKeyframe(0.0f, Float3{0, 0, 0});
        pos->AddKeyframe(1.0f, Float3{0, 10, 0});
        clip.GetOrCreateRotationTrack(1)->AddKeyframe(0.0f, Quaternion::Identity);
        clip.AddEvent(0.5f, u8"Footstep");

        AnimationClipSource src;
        AnimationClipSource::FromClip(clip, src);
        REQUIRE(inst->WriteObject(src).IsOk());
    }

    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                          u8".rasset");
    AnimationClipFactory factory(DefaultAllocator());
    ResourceManager manager(DefaultAllocator(), db);
    manager.AddFactory(&factory);

    Proxy<AnimationClip> clip = manager.Bind<AnimationClip>(id);
    REQUIRE(clip);
    CHECK(NearlyEqual(clip->duration, 1.0f));
    CHECK(clip->isLooping == true);
    CHECK(clip->PositionTracks().Size() == 1);
    CHECK(clip->RotationTracks().Size() == 1);
    CHECK(clip->Events().Size() == 1);
    CHECK(clip->Events()[0].name == StringView{u8"Footstep"});

    // The position track still samples correctly after the round-trip.
    Skeleton skel{2};
    skel.Bones()[0].index = 0;
    skel.Bones()[0].parentIndex = -1;
    skel.Bones()[1].index = 1;
    skel.Bones()[1].parentIndex = 0;
    skel.FindRootBones();
    skel.BuildChildIndices();
    BoneTransform poses[2] = {};
    SampleClip(*clip.Get(), skel, 0.5f, Span<BoneTransform>{poses, 2});
    CHECK(NearlyEqual(poses[0].position, Float3{0, 5, 0}));

    RemoveTree();
}

TEST_CASE("animation clip resource: a run past the keyframe pool refuses to build")
{
    GlobalTypeRegistry().Register(AnimationClipSource::StaticType());
    RegisterSerializable<AnimationClipSource>();
    GlobalTypeRegistry().Register(AnimationClip::StaticType());

    // A record whose track table claims more keys than the pool holds must fail closed rather
    // than index past the pool.
    AnimationClipSource overrun;
    overrun.trackBone.PushBack(0);
    overrun.trackKind.PushBack(0);
    overrun.trackInterp.PushBack(1);
    overrun.trackStart.PushBack(0);
    overrun.trackCount.PushBack(3); // the pool below holds 2
    overrun.keyTimes.PushBack(0.0f);
    overrun.keyTimes.PushBack(1.0f);
    overrun.keyValues.PushBack(Float4{0, 0, 0, 0});
    overrun.keyValues.PushBack(Float4{0, 1, 0, 0});
    AnimationClip clip;
    CHECK_FALSE(overrun.FillClip(clip));
    CHECK(clip.PositionTracks().IsEmpty());

    // A track table shorter than its bone list is the same refusal.
    AnimationClipSource shortTable;
    shortTable.trackBone.PushBack(0);
    shortTable.trackBone.PushBack(1);
    shortTable.trackStart.PushBack(0);
    shortTable.trackCount.PushBack(0);
    CHECK_FALSE(shortTable.FillClip(clip));

    // A well-formed record still fills; a run ending exactly at the pool is fine.
    overrun.trackCount[0] = 2;
    CHECK(overrun.FillClip(clip));
    CHECK(clip.PositionTracks().Size() == 1);
    CHECK(clip.PositionTracks()[0]->Keyframes().Size() == 2);

    // And through the manager the malformed record binds nothing rather than a corrupt clip.
    RemoveTree();
    NativeFileSystem mount(u8"scratch_anim_res_db", DefaultAllocator());
    Guid id;
    {
        foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount,
                                              foundation::core::BinarySerializerFactory(), u8".rasset");
        auto* inst = db.RootGroup()->CreateInstance(u8"clip", AnimationClipSource::StaticType());
        id = inst->Id();
        overrun.trackCount[0] = 3;
        REQUIRE(inst->WriteObject(overrun).IsOk());
    }
    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount,
                                          foundation::core::BinarySerializerFactory(), u8".rasset");
    AnimationClipFactory factory(DefaultAllocator());
    ResourceManager manager(DefaultAllocator(), db);
    manager.AddFactory(&factory);
    Proxy<AnimationClip> bound = manager.Bind<AnimationClip>(id);
    CHECK_FALSE(bound);
    RemoveTree();
}

TEST_CASE("animation graph resource: composite - resolves clip refs through the manager")
{
    GlobalTypeRegistry().Register(AnimationClipSource::StaticType());
    RegisterSerializable<AnimationClipSource>();
    GlobalTypeRegistry().Register(AnimationClip::StaticType());
    GlobalTypeRegistry().Register(AnimationGraphSource::StaticType());
    RegisterSerializable<AnimationGraphSource>();
    GlobalTypeRegistry().Register(AnimationGraph::StaticType());

    FileDelete(u8"scratch_anim_res_db/walk.rasset");
    FileDelete(u8"scratch_anim_res_db/graph.rasset");
    RemoveDirectory(u8"scratch_anim_res_db");
    NativeFileSystem mount(u8"scratch_anim_res_db", DefaultAllocator());

    Guid graphId;
    {
        foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                              u8".rasset");

        // A clip resource the graph will reference by id.
        auto* clipInst =
            db.RootGroup()->CreateInstance(u8"walk", AnimationClipSource::StaticType());
        const Guid walkId = clipInst->Id();
        {
            AnimationClip walk{u8"walk", 2.0f, true};
            walk.GetOrCreatePositionTrack(0)->AddKeyframe(0.0f, Float3{0, 0, 0});
            AnimationClipSource csrc;
            AnimationClipSource::FromClip(walk, csrc);
            REQUIRE(clipInst->WriteObject(csrc).IsOk());
        }

        // A graph: one Bool param, one layer with a single clip state referencing `walk`.
        auto* graphInst =
            db.RootGroup()->CreateInstance(u8"graph", AnimationGraphSource::StaticType());
        graphId = graphInst->Id();

        AnimationGraphSource gsrc;
        gsrc.paramNames.PushBack(String{u8"Moving"});
        gsrc.paramTypes.PushBack(static_cast<u8>(AnimationParameterType::Bool));

        GraphLayerData layer;
        layer.name = String{u8"Base"};
        GraphStateData state;
        state.name = String{u8"Walk"};
        state.node.kind = 0;         // clip node
        state.node.clipRef = walkId; // referenced by id
        layer.states.PushBack(static_cast<GraphStateData&&>(state));
        gsrc.layers.PushBack(static_cast<GraphLayerData&&>(layer));

        REQUIRE(graphInst->WriteObject(gsrc).IsOk());
    }

    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                          u8".rasset");
    AnimationClipFactory clipFactory(DefaultAllocator());
    AnimationGraphFactory graphFactory(DefaultAllocator());
    ResourceManager manager(DefaultAllocator(), db);
    manager.AddFactory(&clipFactory);
    manager.AddFactory(&graphFactory);

    Proxy<AnimationGraph> graph = manager.Bind<AnimationGraph>(graphId);
    REQUIRE(graph);
    CHECK(graph->Parameters().Size() == 1);
    CHECK(graph->Layers().Size() == 1);
    AnimationGraphState* st = graph->Layers()[0]->GetState(0);
    REQUIRE(st != nullptr);
    REQUIRE(st->Node() != nullptr);
    CHECK(st->Node()->Type() == NodeType::Clip);
    // The clip ref resolved to the actual cooked clip (duration 2.0) via the composite Bind.
    CHECK(NearlyEqual(st->Node()->Duration(), 2.0f));

    FileDelete(u8"scratch_anim_res_db/walk.rasset");
    FileDelete(u8"scratch_anim_res_db/graph.rasset");
    RemoveDirectory(u8"scratch_anim_res_db");
}
