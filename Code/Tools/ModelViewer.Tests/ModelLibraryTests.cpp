// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The model viewer's headless half (Documentation/Specs/model-viewer.md, P0): a model file opens
// into a cache folder of its own and binds, with its numbers counted from the file; opened again
// it binds what it cooked before, numbers and all, while a changed file gets a folder of its own;
// and the bound model goes into a scene as entities, a skinned one playing its first clip.
#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.vfs;
import foundation.scene;
import foundation.model.resource;
import engine.render;
import engine.animation;
import engine.model;
import modelviewer;

using namespace foundation::core;
namespace vfs = foundation::vfs;
namespace scene = foundation::scene;

namespace
{
    String TestModel(StringView relative)
    {
        static const String root = vfs::FindDataRoot();
        REQUIRE_FALSE(root.IsEmpty());
        return vfs::DataPath(root.AsView(), relative);
    }

    // A fresh cache under the test's scratch folder.
    String FreshCache(StringView name)
    {
        const String path = PathJoin(GetCurrentDirectory().AsView(), name);
        (void)RemoveDirectoryRecursive(path.AsView());
        REQUIRE(CreateDirectories(path.AsView()));
        return path;
    }
}

TEST_CASE("model viewer: a model file is one the viewer opens by its extension")
{
    CHECK(modelviewer::IsModelFile(u8"a/b/Fox.gltf"));
    CHECK(modelviewer::IsModelFile(u8"Fox.GLB"));
    CHECK(modelviewer::IsModelFile(u8"/x/Unit.FBX"));
    CHECK(modelviewer::IsModelFile(u8"crate.obj"));
    CHECK_FALSE(modelviewer::IsModelFile(u8"texture.png"));
    CHECK_FALSE(modelviewer::IsModelFile(u8"fbx"));
    CHECK_FALSE(modelviewer::IsModelFile(u8"folder.fbx/readme"));
}

TEST_CASE("model viewer: the record beside a cooked model reads back as it was written")
{
    modelviewer::ModelStats stats;
    stats.meshes = 3;
    stats.vertices = 1234;
    stats.triangles = 999;
    stats.materials = 2;
    stats.textures = 1;
    stats.bones = 24;
    stats.skins = 1;
    stats.size = Float3{1.5f, 2.0f, 0.25f};
    stats.clips.PushBack(String(u8"Run"));
    stats.clipSeconds.PushBack(0.75f);
    stats.clips.PushBack(String(u8"Idle and look"));
    stats.clipSeconds.PushBack(3.0f);
    const Guid model(0x1234567890abcdefull, 0x0fedcba987654321ull);

    Guid readModel;
    modelviewer::ModelStats read;
    REQUIRE(modelviewer::ReadRecord(modelviewer::WriteRecord(model, stats).AsView(), readModel, read));
    CHECK(readModel == model);
    CHECK(read.meshes == 3u);
    CHECK(read.vertices == 1234u);
    CHECK(read.triangles == 999u);
    CHECK(read.materials == 2u);
    CHECK(read.textures == 1u);
    CHECK(read.bones == 24u);
    CHECK(read.skins == 1u);
    CHECK(read.size.y == doctest::Approx(2.0f));
    REQUIRE(read.clips.Size() == 2u);
    CHECK(read.clips[1] == u8"Idle and look"); // a name with spaces survives
    CHECK(read.clipSeconds[0] == doctest::Approx(0.75f));
    Guid none;
    modelviewer::ModelStats ignored;
    CHECK_FALSE(modelviewer::ReadRecord(u8"meshes 3\n", none, ignored)); // no model: cooks again
}

TEST_CASE("model viewer: a skinned glTF opens into its cache, counted, and binds again from it")
{
    const String fox = TestModel(u8"Tests/Models/FoxNested/glTF/Fox.gltf");
    const String cache = FreshCache(u8"scratch_model_viewer_cache");

    modelviewer::ModelStats first;
    String folder;
    {
        modelviewer::OpenModel open(DefaultAllocator());
        REQUIRE(open.Open(fox.AsView(), cache.AsView(), nullptr) == modelviewer::OpenModel::Outcome::Opened);
        REQUIRE(open.IsOpen());
        first = open.Stats();
        folder = String(open.CacheFolder());
        CHECK(first.meshes > 0u);
        CHECK(first.triangles > 0u);
        CHECK(first.bones > 0u);
        CHECK_FALSE(first.clips.IsEmpty());
        CHECK(open.Model()->meshes.Size() > 0u);
        CHECK(open.Model()->skeleton);
    }
    CHECK(FileExists(PathJoin(folder.AsView(), StringView(modelviewer::OpenModel::kRecordName)).AsView()));

    // Opened again: bound from the cache, the same numbers, nothing cooked.
    modelviewer::OpenModel again(DefaultAllocator());
    REQUIRE(again.Open(fox.AsView(), cache.AsView(), nullptr) == modelviewer::OpenModel::Outcome::FromCache);
    CHECK(again.CacheFolder() == folder.AsView());
    CHECK(again.Stats().triangles == first.triangles);
    CHECK(again.Stats().clips.Size() == first.clips.Size());
    CHECK(again.Model()->animations.Size() == first.clips.Size());

    // A file the cache never saw (another path) has a folder of its own; a missing one is said so.
    CHECK(modelviewer::CacheFolderFor(cache.AsView(), u8"/no/such/model.fbx").IsEmpty());
    modelviewer::OpenModel missing(DefaultAllocator());
    CHECK(missing.Open(u8"/no/such/model.fbx", cache.AsView(), nullptr) == modelviewer::OpenModel::Outcome::NotFound);
}

TEST_CASE("model viewer: the bound model goes into a scene, a skinned one playing its first clip")
{
    const String fox = TestModel(u8"Tests/Models/FoxNested/glTF/Fox.gltf");
    const String cache = FreshCache(u8"scratch_model_viewer_scene");
    modelviewer::OpenModel open(DefaultAllocator());
    REQUIRE(open.IsOpen() == false);
    REQUIRE(open.Open(fox.AsView(), cache.AsView(), nullptr) == modelviewer::OpenModel::Outcome::Opened);

    scene::Scene level{DefaultAllocator(), u8"viewer"};
    level.AddSystem<engine::render::MeshComponentManager>();
    engine::animation::AddAnimationSceneManagers(level);
    engine::model::InstantiateOptions options;
    options.rootTransform.scale = Float3{0.5f, 0.5f, 0.5f};
    const engine::model::ModelInstance instance = engine::model::InstantiateModel(level, *open.Model(), u8"Fox", options);
    REQUIRE(level.IsValid(instance.root));
    CHECK(level.GetLocalTransform(instance.root).scale.x == doctest::Approx(0.5f));
    CHECK_FALSE(instance.skinned.IsEmpty());
    CHECK(instance.animated);
    auto* animators = level.GetSystem<engine::animation::SkeletalAnimationComponentManager>();
    REQUIRE(animators != nullptr);
    const engine::animation::SkeletalAnimationComponent* animator = animators->Get(instance.root);
    REQUIRE(animator != nullptr);
    CHECK(animator->meshEntities.Size() == instance.skinned.Size());
    // A skinned mesh sits in its skeleton's space: under the skeleton's parent node (the root when
    // it has none) at identity, as the project's model prefab puts it.
    const foundation::model::ModelResource& model = *open.Model();
    REQUIRE(model.skeletonParentNode >= -1);
    for (const scene::EntityHandle skinned : instance.skinned)
    {
        const Transform local = level.GetLocalTransform(skinned);
        CHECK(local.position.x == doctest::Approx(0.0f));
        CHECK(local.scale.y == doctest::Approx(1.0f));
        if (model.skeletonParentNode == -1)
            CHECK(level.GetParent(skinned) == instance.root);
        else
            CHECK(level.GetEntityName(level.GetParent(skinned)) == model.nodes[static_cast<usize>(model.skeletonParentNode)].name.AsView());
    }

    // Without the animation managers it still shows, unanimated; asked not to animate, the same.
    scene::Scene still{DefaultAllocator(), u8"still"};
    still.AddSystem<engine::render::MeshComponentManager>();
    const engine::model::ModelInstance plain = engine::model::InstantiateModel(still, *open.Model(), u8"Fox");
    CHECK(still.IsValid(plain.root));
    CHECK_FALSE(plain.animated);
    engine::model::InstantiateOptions frozen;
    frozen.animate = false;
    CHECK_FALSE(engine::model::InstantiateModel(level, *open.Model(), u8"Fox2", frozen).animated);
}
