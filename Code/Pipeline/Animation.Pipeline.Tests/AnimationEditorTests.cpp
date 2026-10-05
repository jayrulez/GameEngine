// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Animation editor: cook a SkeletonAsset through its builder into the content DB, then load it back
// through the resource factory and verify the runtime skeleton. Exercises the authoring -> cook ->
// product path.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.resource;
import pipeline.core;
import foundation.animation;
import foundation.animation.resource;
import animation.pipeline;

using namespace foundation::core;
using namespace pipeline;
using namespace foundation::vfs;
using namespace foundation::resource;
using namespace foundation::animation;

TEST_CASE("skeleton asset: builder cooks into the content DB, factory loads it back")
{
    GlobalTypeRegistry().Register(SkeletonSource::StaticType());
    RegisterSerializable<SkeletonSource>();
    GlobalTypeRegistry().Register(Skeleton::StaticType());

    FileDelete(u8"scratch_anim_ed_db/skel.rasset");
    RemoveDirectory(u8"scratch_anim_ed_db");
    NativeFileSystem mount(u8"scratch_anim_ed_db", DefaultAllocator());

    Guid id;
    {
        foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                              u8".rasset");
        auto* inst = db.RootGroup()->CreateInstance(u8"skel", SkeletonSource::StaticType());
        id = inst->Id();

        // Author a skeleton asset (source populated from a runtime skeleton - model importer later).
        Skeleton skel{2};
        skel.Bones()[0].index = 0;
        skel.Bones()[0].parentIndex = -1;
        skel.Bones()[0].name = String{u8"root"};
        skel.Bones()[1].index = 1;
        skel.Bones()[1].parentIndex = 0;
        skel.Bones()[1].name = String{u8"child"};
        skel.BuildNameMap();
        skel.FindRootBones();
        skel.BuildChildIndices();
        skel.ComputeInverseBindPoses();

        SkeletonAsset asset;
        asset.fileName = foundation::vfs::SourcePath(u8"models/char.gltf");
        SkeletonSource::FromSkeleton(skel, asset.source);

        SkeletonAssetBuilder builder;
        pipeline::AssetBuildContext ctx{DefaultAllocator()};
        ctx.output = inst;
        REQUIRE(builder.Build(asset, ctx).IsOk());
    }

    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                          u8".rasset");
    SkeletonFactory factory(DefaultAllocator());
    ResourceManager manager(DefaultAllocator(), db);
    manager.AddFactory(&factory);

    Proxy<Skeleton> skel = manager.Bind<Skeleton>(id);
    REQUIRE(skel);
    CHECK(skel->BoneCount() == 2);
    CHECK(skel->FindBone(u8"child") == 1);
    CHECK(skel->RootBones().Size() == 1);

    FileDelete(u8"scratch_anim_ed_db/skel.rasset");
    RemoveDirectory(u8"scratch_anim_ed_db");
}

TEST_CASE("clip asset: the builder bakes root motion from a named root through the clip's skeleton")
{
    // root-motion.md P0: the clip names its root bone; the builder resolves it against the skeleton
    // the clip addresses (read from the SOURCE database, and declared as a read so a changed
    // skeleton re-cooks the clip), bakes the travel and strips it from the cooked pose.
    RegisterAnimationAssets();
    RegisterAnimationResourceTypes();
    FileDelete(u8"scratch_anim_rm_db/skel.rasset");
    FileDelete(u8"scratch_anim_rm_db/walk.rasset");
    FileDelete(u8"scratch_anim_rm_db/cooked.rasset");
    RemoveDirectory(u8"scratch_anim_rm_db");
    NativeFileSystem mount(u8"scratch_anim_rm_db", DefaultAllocator());
    foundation::content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");

    SkeletonAsset skeleton;
    skeleton.source.boneNames.PushBack(String(u8"root"));
    skeleton.source.boneNames.PushBack(String(u8"hips"));
    skeleton.source.parentIndices.PushBack(-1);
    skeleton.source.parentIndices.PushBack(0);
    auto* skelInst = db.RootGroup()->CreateInstance(u8"skel", SkeletonAsset::StaticType());
    REQUIRE(skelInst->WriteObject(skeleton).IsOk());

    AnimationClipAsset walk;
    walk.skeleton = skelInst->Id();
    walk.source.name = String(u8"Walk");
    walk.source.duration = 1.0f;
    walk.source.trackBone.PushBack(1); // the hips
    walk.source.trackKind.PushBack(static_cast<u8>(AnimationClipSource::TrackKind::Position));
    walk.source.trackInterp.PushBack(static_cast<u8>(InterpolationMode::Linear));
    walk.source.trackStart.PushBack(0);
    walk.source.trackCount.PushBack(2);
    walk.source.keyTimes.PushBack(0.0f);
    walk.source.keyTimes.PushBack(1.0f);
    walk.source.keyValues.PushBack(Float4{0, 1, 0, 0});
    walk.source.keyValues.PushBack(Float4{0, 1, 2, 0});
    walk.source.rootMotion.rootBone = String(u8"hips");
    walk.source.rootMotion.horizontal = true;

    AnimationClipAssetBuilder builder;
    CHECK(builder.Version() >= 2u); // every clip re-cooks into the new layout
    pipeline::AssetBuildContext ctx{DefaultAllocator()};
    pipeline::AssetDependencies deps;
    builder.ScanDependencies(walk, ctx, deps);
    REQUIRE(deps.reads.Size() == 1u);
    CHECK(deps.reads[0] == skelInst->Id());

    auto* cookedInst = db.RootGroup()->CreateInstance(u8"cooked", AnimationClipSource::StaticType());
    ctx.output = cookedInst;
    ctx.sourceDb = &db;
    REQUIRE(builder.Build(walk, ctx).IsOk());
    RefPtr<ISerializable> object = cookedInst->ReadObject();
    const auto* cooked = Cast<AnimationClipSource>(object.Get());
    REQUIRE(cooked != nullptr);
    REQUIRE_FALSE(cooked->rootTimes.IsEmpty());
    CHECK(cooked->rootPositions[cooked->rootPositions.Size() - 1].z == doctest::Approx(2.0f));
    CHECK(cooked->keyValues[1].z == doctest::Approx(0.0f)); // the cooked hips walk in place
    CHECK(walk.source.keyValues[1].z == doctest::Approx(2.0f)); // the authored clip is untouched

    // A root the skeleton does not have, or no skeleton at all, fails the cook loudly.
    walk.source.rootMotion.rootBone = String(u8"pelvis");
    CHECK_FALSE(builder.Build(walk, ctx).IsOk());
    walk.source.rootMotion.rootBone = String(u8"hips");
    walk.skeleton = Guid{};
    CHECK_FALSE(builder.Build(walk, ctx).IsOk());
    // Root motion off: no skeleton needed, the clip cooks as authored.
    walk.source.rootMotion = RootMotionSettings{};
    REQUIRE(builder.Build(walk, ctx).IsOk());
    deps = pipeline::AssetDependencies{};
    builder.ScanDependencies(walk, ctx, deps);
    CHECK(deps.reads.IsEmpty());
}
