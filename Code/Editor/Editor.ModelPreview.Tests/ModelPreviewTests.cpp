// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// A model file opened for viewing without a project (the model viewer, the import window). It opens
// into its own cache folder and binds, with its stats counted from the file; opened again it binds
// what was cooked before, stats included. The bound model goes into a scene as entities, a skinned
// one playing its first clip, and its clips are driven. Also covered: the record beside the cooked
// files, which files open by extension, the materials and textures listed, and the loose texture
// rule.
#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.vfs;
import foundation.scene;
import foundation.model;
import foundation.model.resource;
import foundation.image;
import foundation.image.io;
import engine.render;
import engine.animation;
import engine.model;
import editor.modelpreview;

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

TEST_CASE("model preview: a model file is one the viewer opens by its extension")
{
    CHECK(editor::IsModelFile(u8"a/b/Fox.gltf"));
    CHECK(editor::IsModelFile(u8"Fox.GLB"));
    CHECK(editor::IsModelFile(u8"/x/Unit.FBX"));
    CHECK(editor::IsModelFile(u8"crate.obj"));
    CHECK_FALSE(editor::IsModelFile(u8"texture.png"));
    CHECK_FALSE(editor::IsModelFile(u8"fbx"));
    CHECK_FALSE(editor::IsModelFile(u8"folder.fbx/readme"));
}
TEST_CASE("model preview: the record beside a cooked model reads back as it was written")
{
    editor::ModelStats stats;
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
    editor::ModelStats read;
    REQUIRE(editor::ReadRecord(editor::WriteRecord(model, stats).AsView(), readModel, read));
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
    editor::ModelStats ignored;
    CHECK_FALSE(editor::ReadRecord(u8"meshes 3\n", none, ignored)); // no model guid: the model cooks again
}
TEST_CASE("model preview: a skinned glTF opens into its cache, counted, and binds again from it")
{
    const String fox = TestModel(u8"Tests/Models/FoxNested/glTF/Fox.gltf");
    const String cache = FreshCache(u8"scratch_model_viewer_cache");

    editor::ModelStats first;
    String folder;
    {
        editor::OpenModel open(DefaultAllocator());
        REQUIRE(open.Open(fox.AsView(), cache.AsView(), nullptr) == editor::OpenModel::Outcome::Opened);
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
    CHECK(FileExists(PathJoin(folder.AsView(), StringView(editor::OpenModel::kRecordName)).AsView()));

    // Opened again: bound from the cache with the same stats, nothing cooked.
    editor::OpenModel again(DefaultAllocator());
    REQUIRE(again.Open(fox.AsView(), cache.AsView(), nullptr) == editor::OpenModel::Outcome::FromCache);
    CHECK(again.CacheFolder() == folder.AsView());
    CHECK(again.Stats().triangles == first.triangles);
    CHECK(again.Stats().clips.Size() == first.clips.Size());
    CHECK(again.Model()->animations.Size() == first.clips.Size());

    // A file the cache has not seen gets its own folder; a missing file gets none.
    CHECK(editor::CacheFolderFor(cache.AsView(), u8"/no/such/model.fbx").IsEmpty());
    editor::OpenModel missing(DefaultAllocator());
    CHECK(missing.Open(u8"/no/such/model.fbx", cache.AsView(), nullptr) == editor::OpenModel::Outcome::NotFound);
}
TEST_CASE("model preview: the bound model goes into a scene, a skinned one playing its first clip")
{
    const String fox = TestModel(u8"Tests/Models/FoxNested/glTF/Fox.gltf");
    const String cache = FreshCache(u8"scratch_model_viewer_scene");
    editor::OpenModel open(DefaultAllocator());
    REQUIRE(open.IsOpen() == false);
    REQUIRE(open.Open(fox.AsView(), cache.AsView(), nullptr) == editor::OpenModel::Outcome::Opened);

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
    // A skinned mesh sits in its skeleton's space: at identity under the skeleton's parent node
    // (the root when it has none), as the project's model prefab places it.
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

    // Without the animation managers it still shows, unanimated, and the same when asked not to
    // animate.
    scene::Scene still{DefaultAllocator(), u8"still"};
    still.AddSystem<engine::render::MeshComponentManager>();
    const engine::model::ModelInstance plain = engine::model::InstantiateModel(still, *open.Model(), u8"Fox");
    CHECK(still.IsValid(plain.root));
    CHECK_FALSE(plain.animated);
    engine::model::InstantiateOptions frozen;
    frozen.animate = false;
    CHECK_FALSE(engine::model::InstantiateModel(level, *open.Model(), u8"Fox2", frozen).animated);
}
TEST_CASE("model preview: the animation bar picks a clip, pauses, steps, scrubs, loops and speeds up")
{
    const String fox = TestModel(u8"Tests/Models/FoxNested/glTF/Fox.gltf");
    const String cache = FreshCache(u8"scratch_model_viewer_playback");
    editor::OpenModel open(DefaultAllocator());
    REQUIRE(open.Open(fox.AsView(), cache.AsView(), nullptr) == editor::OpenModel::Outcome::Opened);
    scene::Scene level{DefaultAllocator(), u8"viewer"};
    level.AddSystem<engine::render::MeshComponentManager>();
    engine::animation::AddAnimationSceneManagers(level);
    const engine::model::ModelInstance instance = engine::model::InstantiateModel(level, *open.Model(), u8"Fox");
    editor::ClipPlayback playback(level, instance.root, *open.Model());
    REQUIRE(playback.HasClips());
    CHECK(playback.ClipCount() == 3);
    CHECK(playback.Selected() == 0); // InstantiateModel plays the first
    CHECK(playback.ClipName(1) == u8"Walk");
    level.Update(0.1f); // the manager builds the player and starts the clip
    REQUIRE(playback.Player() != nullptr);
    CHECK(playback.Playing());

    // A selected clip plays from its start on the next update.
    playback.Select(1);
    level.Update(0.0f);
    CHECK(playback.Player()->CurrentClip() == open.Model()->animations[1].Get());
    CHECK(playback.Time() == doctest::Approx(0.0f));

    // While paused, time stands still; a scrub and a step move it and stay paused.
    playback.SetPlaying(false);
    level.Update(0.5f);
    CHECK(playback.Time() == doctest::Approx(0.0f));
    playback.Scrub(0.4f);
    CHECK(playback.Time() == doctest::Approx(0.4f));
    playback.Step(1);
    CHECK(playback.Time() == doctest::Approx(0.4f + 1.0f / editor::ClipPlayback::kStepFps));
    CHECK_FALSE(playback.Playing());
    playback.Scrub(1000.0f);
    CHECK(playback.Time() == doctest::Approx(playback.Duration())); // clamped to the clip
    playback.Stop();
    CHECK(playback.Time() == doctest::Approx(0.0f));
    CHECK_FALSE(playback.Playing());

    // Twice the speed advances twice the time.
    playback.SetSpeed(2.0f);
    playback.SetPlaying(true);
    level.Update(0.1f);
    CHECK(playback.Time() == doctest::Approx(0.2f));
    playback.SetSpeed(1.0f);

    // Without looping it stops at the end, and playing again starts it over.
    playback.SetLooping(false);
    level.Update(playback.Duration() + 1.0f);
    CHECK_FALSE(playback.Playing());
    CHECK(playback.Time() == doctest::Approx(playback.Duration()));
    playback.SetPlaying(true);
    CHECK(playback.Playing());
    CHECK(playback.Time() == doctest::Approx(0.0f));
    // With looping it starts over past the end.
    playback.SetLooping(true);
    level.Update(playback.Duration() + 0.25f);
    CHECK(playback.Playing());
    CHECK(playback.Time() == doctest::Approx(0.25f).epsilon(0.01));
}
TEST_CASE("model preview: a material without its texture takes the image beside the model")
{
    const String folder = FreshCache(u8"scratch_model_viewer_loose");
    foundation::image::Image pixel(2, 2, foundation::image::PixelFormat::RGBA8);
    const StringView names[] = {u8"Body.png", u8"Knight.png", u8"aaa.png"};
    for (const StringView name : names)
        REQUIRE(foundation::image::io::SaveImage(pixel, PathJoin(folder.AsView(), name).AsView(), foundation::image::io::ImageFileFormat::PNG).IsOk());
    const String path = PathJoin(folder.AsView(), u8"Knight.fbx");

    foundation::model::Model model;
    auto material = [&](StringView name, i32 texture)
    {
        auto* m = new foundation::model::ModelMaterial();
        m->setName(name);
        m->baseColorTextureIndex = texture;
        return model.addMaterial(m);
    };
    auto* linked = new foundation::model::ModelTexture();
    const i32 own = model.addTexture(linked);
    const i32 body = material(u8"body", -1);   // takes the image named like it (ignoring case)
    const i32 cloak = material(u8"Cloak", -1); // no image named like it: takes the model's
    const i32 kept = material(u8"Sword", own); // already linked: left alone
    CHECK(editor::ApplyLooseTextures(model, path.AsView()) == 2u);
    const auto texture = [&](i32 m) { return model.textures()[static_cast<usize>(model.materials()[static_cast<usize>(m)]->baseColorTextureIndex)]; };
    CHECK(texture(body)->uri() == u8"Body.png");
    CHECK(texture(cloak)->uri() == u8"Knight.png");
    CHECK(model.materials()[static_cast<usize>(kept)]->baseColorTextureIndex == own);

    // With neither name's image present, nothing: the other images beside it are not its, and the
    // material keeps its own colour (a model of plain colours among a game's sources).
    foundation::model::Model other;
    auto* m = new foundation::model::ModelMaterial();
    m->setName(u8"Jacket");
    m->baseColorFactor = Float4(0.95f, 0.4f, 0.1f, 1.0f);
    other.addMaterial(m);
    CHECK(editor::ApplyLooseTextures(other, PathJoin(folder.AsView(), u8"Rider.glb").AsView()) == 0u);
    CHECK(other.materials()[0]->baseColorTextureIndex == -1);
    CHECK(other.textures().IsEmpty());
    CHECK(other.materials()[0]->baseColorFactor.x == doctest::Approx(0.95f));
    // A folder without images: nothing is applied.
    foundation::model::Model bare;
    auto* n = new foundation::model::ModelMaterial();
    bare.addMaterial(n);
    CHECK(editor::ApplyLooseTextures(bare, u8"/no/such/folder/x.fbx") == 0u);
    CHECK(bare.materials()[0]->baseColorTextureIndex == -1);
}

// The materials and textures listed in the side panel and details panes: they round-trip through
// the record (names with spaces, a texture inside the file with no file name) and are counted from
// a known model (the Fox: one PBR material whose base color is its one texture, an image beside the
// file).
TEST_CASE("model preview: the materials and textures are listed, kept in the record, counted from the file")
{
    editor::ModelStats stats;
    stats.textureList.PushBack(editor::ModelTextureInfo{String(u8"Body Skin"), 1024, 512, String(u8"skin.png")});
    stats.textureList.PushBack(editor::ModelTextureInfo{String(u8"inside"), 64, 64, String{}});
    editor::ModelMaterialInfo coat;
    coat.name = String(u8"Fur coat");
    coat.shading = String(u8"Phong");
    coat.slots.PushBack(editor::ModelMaterialInfo::Slot{String(u8"base color"), 0});
    coat.slots.PushBack(editor::ModelMaterialInfo::Slot{String(u8"normal"), 1});
    stats.materialList.PushBack(coat);
    editor::ModelMaterialInfo bare;
    bare.name = String(u8"Bare");
    bare.shading = String(u8"PBR");
    stats.materialList.PushBack(bare);

    Guid id;
    editor::ModelStats read;
    REQUIRE(editor::ReadRecord(editor::WriteRecord(Guid(1, 2), stats).AsView(), id, read));
    REQUIRE(read.textureList.Size() == 2u);
    CHECK(read.textureList[0].name == u8"Body Skin");
    CHECK(read.textureList[0].width == 1024u);
    CHECK(read.textureList[0].height == 512u);
    CHECK(read.textureList[0].file == u8"skin.png");
    CHECK(read.textureList[1].file.IsEmpty());
    REQUIRE(read.materialList.Size() == 2u);
    CHECK(read.materialList[0].name == u8"Fur coat");
    CHECK(read.materialList[0].shading == u8"Phong");
    REQUIRE(read.materialList[0].slots.Size() == 2u);
    CHECK(read.materialList[0].slots[1].slot == u8"normal");
    CHECK(read.materialList[0].slots[1].texture == 1u);
    CHECK(read.materialList[1].slots.IsEmpty());

    // Counted from the Fox.
    const String fox = TestModel(u8"Tests/Models/FoxNested/glTF/Fox.gltf");
    foundation::model::Model model;
    editor::ModelStats counted;
    REQUIRE(editor::OpenModel::LoadFile(fox.AsView(), true, model, counted) == editor::OpenModel::Outcome::Opened);
    REQUIRE(counted.materialList.Size() == 1u);
    CHECK(counted.materialList[0].name == u8"fox_material");
    CHECK(counted.materialList[0].shading == u8"PBR");
    REQUIRE(counted.materialList[0].slots.Size() == 1u);
    CHECK(counted.materialList[0].slots[0].slot == u8"base color");
    REQUIRE(counted.textureList.Size() == 1u);
    CHECK(counted.textureList[0].file == u8"Texture.png");
    CHECK(counted.textureList[0].width > 0u);
}

TEST_CASE("model preview: a cook told to stop says so and clears its folder, so the next open cooks afresh; Cook and Bind are Finish's two halves")
{
    const String fox = TestModel(u8"Tests/Models/FoxNested/glTF/Fox.gltf");
    const String cache = FreshCache(u8"scratch_model_viewer_cook_cancel");
    foundation::model::Model model;
    editor::ModelStats stats;
    REQUIRE(editor::OpenModel::LoadFile(fox.AsView(), true, model, stats) == editor::OpenModel::Outcome::Opened);

    String folder;
    {
        editor::OpenModel open(DefaultAllocator());
        editor::OpenModel::Outcome outcome = editor::OpenModel::Outcome::NotFound;
        REQUIRE(open.Begin(fox.AsView(), cache.AsView(), nullptr, outcome) == editor::OpenModel::Begun::NeedsFile);
        folder = String(open.CacheFolder());
        RefPtr<editor::ModelCache> kept = open.Cache();
        REQUIRE(kept);
        CancelSource cancel(DefaultAllocator());
        cancel.Cancel();
        Guid guid;
        CHECK(editor::OpenModel::Cook(*kept, model, stats, guid, cancel.Token()) == editor::OpenModel::Outcome::Cancelled);
        CHECK_FALSE(DirectoryExists(folder.AsView())); // nothing half-made is left for the next open
        CHECK_FALSE(open.IsOpen());
    }

    // The next open finds no record and cooks; split, as a window runs it, it binds what Cook made.
    editor::OpenModel open(DefaultAllocator());
    editor::OpenModel::Outcome outcome = editor::OpenModel::Outcome::NotFound;
    REQUIRE(open.Begin(fox.AsView(), cache.AsView(), nullptr, outcome) == editor::OpenModel::Begun::NeedsFile);
    Guid guid;
    REQUIRE(editor::OpenModel::Cook(*open.Cache(), model, stats, guid) == editor::OpenModel::Outcome::Opened);
    CHECK(FileExists(PathJoin(folder.AsView(), StringView(editor::OpenModel::kRecordName)).AsView()));
    CHECK(open.Bind(guid, stats) == editor::OpenModel::Outcome::Opened);
    CHECK(open.IsOpen());
    CHECK(open.Stats().triangles == stats.triangles);
}
