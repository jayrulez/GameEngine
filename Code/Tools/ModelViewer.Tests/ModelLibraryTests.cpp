// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The model viewer's headless half (Documentation/Specs/model-viewer.md, P0): a model file opens
// into a cache folder of its own and binds, with its numbers counted from the file; opened again
// it binds what it cooked before, numbers and all, while a changed file gets a folder of its own;
// and the bound model goes into a scene as entities, a skinned one playing its first clip. P1: the
// numbers counted from a known model, the side panel's rows, and the tabs' open, reopen, move and
// close.
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
import foundation.model;
import foundation.image;
import foundation.image.io;

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

TEST_CASE("model viewer: a known model's numbers, as the side panel shows them")
{
    // Fox.gltf: one mesh of 1728 vertices with no index buffer (576 triangles), one material and
    // texture, one skin of 24 joints, three clips; its position accessor's bounds.
    const String fox = TestModel(u8"Tests/Models/FoxNested/glTF/Fox.gltf");
    const String cache = FreshCache(u8"scratch_model_viewer_stats");
    modelviewer::OpenModel open(DefaultAllocator());
    REQUIRE(open.Open(fox.AsView(), cache.AsView(), nullptr) == modelviewer::OpenModel::Outcome::Opened);
    const modelviewer::ModelStats& stats = open.Stats();
    CHECK(stats.meshes == 1u);
    CHECK(stats.vertices == 1728u);
    CHECK(stats.triangles == 576u);
    CHECK(stats.materials == 1u);
    CHECK(stats.textures == 1u);
    CHECK(stats.bones == 24u);
    CHECK(stats.skins == 1u);
    REQUIRE(stats.clips.Size() == 3u);
    CHECK(stats.clips[0] == u8"Survey");
    CHECK(stats.clips[2] == u8"Run");
    CHECK(stats.size.y == doctest::Approx(79.03f).epsilon(0.001));

    const Array<modelviewer::StatRow> rows = modelviewer::StatRows(stats);
    REQUIRE(rows.Size() == 8u);
    CHECK(rows[1].label == u8"Vertices");
    CHECK(rows[1].value == u8"1,728");
    CHECK(rows[2].value == u8"576");
    CHECK(rows[5].value == u8"24");
    CHECK(rows[6].value == u8"3");
    CHECK(rows[7].label == u8"Size");
    CHECK(modelviewer::GroupedCount(1234567) == u8"1,234,567");
    CHECK(modelviewer::GroupedCount(999) == u8"999");
}

TEST_CASE("model viewer: tabs open, reopen to the front, move and close to a neighbour")
{
    const String fox = TestModel(u8"Tests/Models/FoxNested/glTF/Fox.gltf");
    const String oozi = TestModel(u8"Tests/Models/KenneyPlatformerCharacters/GLB/character-oozi.glb");
    const String cache = FreshCache(u8"scratch_model_viewer_tabs");
    modelviewer::ModelTabs tabs(DefaultAllocator());
    CHECK(tabs.Active() == -1);

    const modelviewer::ModelTabs::Opened first = tabs.Open(fox.AsView(), cache.AsView(), nullptr);
    CHECK(first.index == 0);
    CHECK(first.added);
    CHECK(tabs.At(0).title == u8"Fox.gltf");
    const modelviewer::ModelTabs::Opened second = tabs.Open(oozi.AsView(), cache.AsView(), nullptr);
    CHECK(second.index == 1);
    CHECK(tabs.Active() == 1);

    // Opened again: its own tab comes to the front, no second one.
    const modelviewer::ModelTabs::Opened again = tabs.Open(fox.AsView(), cache.AsView(), nullptr);
    CHECK(again.index == 0);
    CHECK_FALSE(again.added);
    CHECK(tabs.Count() == 2);
    CHECK(tabs.Active() == 0);

    // A file that does not open adds no tab and leaves the front where it was.
    CHECK(tabs.Open(u8"/no/such/model.fbx", cache.AsView(), nullptr).index == -1);
    CHECK(tabs.Count() == 2);
    CHECK(tabs.Active() == 0);

    // Moved, the one in front stays in front.
    CHECK(tabs.Move(0, 1));
    CHECK(tabs.At(1).title == u8"Fox.gltf");
    CHECK(tabs.Active() == 1);

    // Closing the one in front hands it to the one before (none after); closing the last empties.
    CHECK(tabs.Close(1) == 0);
    CHECK(tabs.At(0).title == u8"character-oozi.glb");
    CHECK(tabs.Close(0) == -1);
    CHECK(tabs.Count() == 0);
    CHECK(tabs.ActiveTab() == nullptr);
}

TEST_CASE("model viewer: closing a tab before the one in front keeps that one in front")
{
    const String fox = TestModel(u8"Tests/Models/FoxNested/glTF/Fox.gltf");
    const String oozi = TestModel(u8"Tests/Models/KenneyPlatformerCharacters/GLB/character-oozi.glb");
    const String oobi = TestModel(u8"Tests/Models/KenneyPlatformerCharacters/GLB/character-oobi.glb");
    const String cache = FreshCache(u8"scratch_model_viewer_tabs_close");
    modelviewer::ModelTabs tabs(DefaultAllocator());
    (void)tabs.Open(fox.AsView(), cache.AsView(), nullptr);
    (void)tabs.Open(oozi.AsView(), cache.AsView(), nullptr);
    (void)tabs.Open(oobi.AsView(), cache.AsView(), nullptr);
    tabs.SetActive(1);
    CHECK(tabs.Close(0) == 0); // oozi, now first, still in front
    CHECK(tabs.ActiveTab()->title == u8"character-oozi.glb");
    CHECK(tabs.Close(0) == 0); // its successor comes to the front
    CHECK(tabs.ActiveTab()->title == u8"character-oobi.glb");
}

TEST_CASE("model viewer: the animation bar picks a clip, pauses, steps, scrubs, loops and speeds up")
{
    const String fox = TestModel(u8"Tests/Models/FoxNested/glTF/Fox.gltf");
    const String cache = FreshCache(u8"scratch_model_viewer_playback");
    modelviewer::OpenModel open(DefaultAllocator());
    REQUIRE(open.Open(fox.AsView(), cache.AsView(), nullptr) == modelviewer::OpenModel::Outcome::Opened);
    scene::Scene level{DefaultAllocator(), u8"viewer"};
    level.AddSystem<engine::render::MeshComponentManager>();
    engine::animation::AddAnimationSceneManagers(level);
    const engine::model::ModelInstance instance = engine::model::InstantiateModel(level, *open.Model(), u8"Fox");
    modelviewer::ClipPlayback playback(level, instance.root, *open.Model());
    REQUIRE(playback.HasClips());
    CHECK(playback.ClipCount() == 3);
    CHECK(playback.Selected() == 0); // InstantiateModel plays the first
    CHECK(playback.ClipName(1) == u8"Walk");
    level.Update(0.1f); // the manager builds the player and starts the clip
    REQUIRE(playback.Player() != nullptr);
    CHECK(playback.Playing());

    // A choice plays from its start on the next update.
    playback.Select(1);
    level.Update(0.0f);
    CHECK(playback.Player()->CurrentClip() == open.Model()->animations[1].Get());
    CHECK(playback.Time() == doctest::Approx(0.0f));

    // Paused, time stands; a scrub and a step move it, paused.
    playback.SetPlaying(false);
    level.Update(0.5f);
    CHECK(playback.Time() == doctest::Approx(0.0f));
    playback.Scrub(0.4f);
    CHECK(playback.Time() == doctest::Approx(0.4f));
    playback.Step(1);
    CHECK(playback.Time() == doctest::Approx(0.4f + 1.0f / modelviewer::ClipPlayback::kStepFps));
    CHECK_FALSE(playback.Playing());
    playback.Scrub(1000.0f);
    CHECK(playback.Time() == doctest::Approx(playback.Duration())); // clamped to the clip
    playback.Stop();
    CHECK(playback.Time() == doctest::Approx(0.0f));
    CHECK_FALSE(playback.Playing());

    // Twice the speed, twice the time.
    playback.SetSpeed(2.0f);
    playback.SetPlaying(true);
    level.Update(0.1f);
    CHECK(playback.Time() == doctest::Approx(0.2f));
    playback.SetSpeed(1.0f);

    // Not looping, it stops at the end; play again starts it over.
    playback.SetLooping(false);
    level.Update(playback.Duration() + 1.0f);
    CHECK_FALSE(playback.Playing());
    CHECK(playback.Time() == doctest::Approx(playback.Duration()));
    playback.SetPlaying(true);
    CHECK(playback.Playing());
    CHECK(playback.Time() == doctest::Approx(0.0f));
    // Looping, it starts over past the end.
    playback.SetLooping(true);
    level.Update(playback.Duration() + 0.25f);
    CHECK(playback.Playing());
    CHECK(playback.Time() == doctest::Approx(0.25f).epsilon(0.01));
}

namespace
{
    void Touch(StringView path, StringView text = u8"x")
    {
        REQUIRE(WriteFile(path, Span<const byte>(reinterpret_cast<const byte*>(text.Data()), text.Size())));
    }
}

TEST_CASE("model viewer: a folder lists its folders and models in name order; the models under it collect")
{
    const String root = FreshCache(u8"scratch_model_viewer_browse");
    REQUIRE(CreateDirectories(PathJoin(root.AsView(), u8"Knights/Armour").AsView()));
    REQUIRE(CreateDirectories(PathJoin(root.AsView(), u8"beasts").AsView()));
    REQUIRE(CreateDirectories(PathJoin(root.AsView(), u8".hidden").AsView()));
    Touch(PathJoin(root.AsView(), u8"zeta.FBX").AsView());
    Touch(PathJoin(root.AsView(), u8"Alpha.glb").AsView());
    Touch(PathJoin(root.AsView(), u8"readme.txt").AsView());
    Touch(PathJoin(root.AsView(), u8"Knights/knight.gltf").AsView());
    Touch(PathJoin(root.AsView(), u8"Knights/Armour/helm.obj").AsView());
    Touch(PathJoin(root.AsView(), u8".hidden/secret.fbx").AsView());

    const modelviewer::FolderListing listing = modelviewer::ListFolder(root.AsView());
    REQUIRE(listing.folders.Size() == 2u); // not the hidden one
    CHECK(modelviewer::FileTitle(listing.folders[0].AsView()) == u8"beasts");
    CHECK(modelviewer::FileTitle(listing.folders[1].AsView()) == u8"Knights");
    REQUIRE(listing.models.Size() == 2u); // not the text file
    CHECK(modelviewer::FileTitle(listing.models[0].AsView()) == u8"Alpha.glb");
    CHECK(modelviewer::FileTitle(listing.models[1].AsView()) == u8"zeta.FBX");

    Array<String> all;
    modelviewer::CollectModels(root.AsView(), all);
    REQUIRE(all.Size() == 4u); // this folder's, then each subfolder's in order
    CHECK(modelviewer::FileTitle(all[2].AsView()) == u8"knight.gltf");
    CHECK(modelviewer::FileTitle(all[3].AsView()) == u8"helm.obj");
    Array<String> bounded;
    modelviewer::CollectModels(root.AsView(), bounded, 3);
    CHECK(bounded.Size() == 3u);
}

TEST_CASE("model viewer: a thumbnail's file is named by the model file, and changes when it does")
{
    const String root = FreshCache(u8"scratch_model_viewer_thumbs");
    const String model = PathJoin(root.AsView(), u8"crate.obj");
    Touch(model.AsView(), u8"v 0 0 0");
    const String thumbs = PathJoin(root.AsView(), u8"thumbs");
    const String first = modelviewer::ThumbnailPathFor(thumbs.AsView(), model.AsView());
    REQUIRE_FALSE(first.IsEmpty());
    CHECK(first.AsView().StartsWith(thumbs.AsView()));
    CHECK(first.AsView().EndsWith(u8".png"));
    CHECK(modelviewer::ThumbnailPathFor(thumbs.AsView(), model.AsView()) == first); // the same file, the same name
    Touch(model.AsView(), u8"v 0 0 0\nv 1 0 0"); // changed: another size
    CHECK(modelviewer::ThumbnailPathFor(thumbs.AsView(), model.AsView()) != first);
    CHECK(modelviewer::ThumbnailPathFor(thumbs.AsView(), u8"/no/such/model.obj").IsEmpty());
}

TEST_CASE("model viewer: the grid's filters by name, rigged and animated; numbers read from the cache")
{
    modelviewer::ModelStats rigged;
    rigged.skins = 1;
    rigged.clips.PushBack(String(u8"Idle"));
    rigged.clipSeconds.PushBack(1.0f);
    modelviewer::ModelStats prop;

    modelviewer::BrowseFilter filter;
    CHECK(modelviewer::Matches(filter, u8"/m/Knight.fbx", nullptr)); // no filter: everything
    filter.name = String(u8"knig");
    CHECK(modelviewer::Matches(filter, u8"/m/Knight.fbx", nullptr)); // any case
    CHECK_FALSE(modelviewer::Matches(filter, u8"/knights/crate.fbx", nullptr)); // the file's name, not its folder
    filter.name = {};
    filter.riggedOnly = true;
    CHECK(modelviewer::Matches(filter, u8"/m/Knight.fbx", &rigged));
    CHECK_FALSE(modelviewer::Matches(filter, u8"/m/crate.fbx", &prop));
    CHECK_FALSE(modelviewer::Matches(filter, u8"/m/unknown.fbx", nullptr)); // not cooked yet: not known rigged
    filter.riggedOnly = false;
    filter.animatedOnly = true;
    CHECK(modelviewer::Matches(filter, u8"/m/Knight.fbx", &rigged));
    CHECK_FALSE(modelviewer::Matches(filter, u8"/m/crate.fbx", &prop));
    filter.animatedOnly = false;
    filter.inLibraryOnly = true;
    CHECK(modelviewer::Matches(filter, u8"/m/crate.fbx", &prop, /*inLibrary*/ true));
    CHECK_FALSE(modelviewer::Matches(filter, u8"/m/Knight.fbx", &rigged, false));

    // A model cooked once has its numbers in the cache; one never opened has none.
    const String fox = TestModel(u8"Tests/Models/FoxNested/glTF/Fox.gltf");
    const String cache = FreshCache(u8"scratch_model_viewer_cached_stats");
    modelviewer::ModelStats read;
    CHECK_FALSE(modelviewer::ReadCachedStats(cache.AsView(), fox.AsView(), read));
    {
        modelviewer::OpenModel open(DefaultAllocator());
        REQUIRE(open.Open(fox.AsView(), cache.AsView(), nullptr) == modelviewer::OpenModel::Outcome::Opened);
    }
    REQUIRE(modelviewer::ReadCachedStats(cache.AsView(), fox.AsView(), read));
    CHECK(read.skins == 1u);
    CHECK(read.clips.Size() == 3u);
}

TEST_CASE("model viewer: the shortlist stars, notes, writes and reads back, and exports CSV")
{
    modelviewer::Shortlist list(DefaultAllocator());
    list.SetStarred(u8"/m/Knight.fbx", true);
    list.SetNote(u8"/m/Crate, wooden.obj", u8"too \"plain\",\nmaybe"); // a note stars it
    CHECK(list.IsStarred(u8"/m/Knight.fbx"));
    CHECK(list.IsStarred(u8"/m/Crate, wooden.obj"));
    CHECK(list.Count() == 2u);
    list.SetStarred(u8"/m/Knight.fbx", true); // starred twice: once
    CHECK(list.Count() == 2u);
    list.SetNote(u8"/m/Knight.fbx", u8"a\ttab and a \\ backslash");

    modelviewer::Shortlist read(DefaultAllocator());
    read.Read(list.Write().AsView());
    REQUIRE(read.Count() == 2u);
    CHECK(read.At(0).path == u8"/m/Knight.fbx");
    CHECK(read.Note(u8"/m/Knight.fbx") == u8"a\ttab and a \\ backslash");
    CHECK(read.Note(u8"/m/Crate, wooden.obj") == u8"too \"plain\",\nmaybe");

    const String csv = read.ToCsv();
    CHECK(csv.AsView().StartsWith(u8"name,path,note\nKnight.fbx,/m/Knight.fbx,"));
    CHECK(csv.AsView().EndsWith(u8"\"Crate, wooden.obj\",\"/m/Crate, wooden.obj\",\"too \"\"plain\"\",\nmaybe\"\n"));

    read.SetStarred(u8"/m/Knight.fbx", false); // off the list, its note with it
    CHECK_FALSE(read.IsStarred(u8"/m/Knight.fbx"));
    CHECK(read.Note(u8"/m/Knight.fbx").IsEmpty());
    CHECK(read.Count() == 1u);
}

TEST_CASE("model viewer: a material without its texture takes the image beside the model")
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
    const i32 body = material(u8"body", -1);   // its own name's image (any case)
    const i32 cloak = material(u8"Cloak", -1); // none of its name: the model's
    const i32 kept = material(u8"Sword", own); // linked: left alone
    CHECK(modelviewer::ApplyLooseTextures(model, path.AsView()) == 2u);
    const auto texture = [&](i32 m) { return model.textures()[static_cast<usize>(model.materials()[static_cast<usize>(m)]->baseColorTextureIndex)]; };
    CHECK(texture(body)->uri() == u8"Body.png");
    CHECK(texture(cloak)->uri() == u8"Knight.png");
    CHECK(model.materials()[static_cast<usize>(kept)]->baseColorTextureIndex == own);

    // With neither name's image there, the first in name order.
    foundation::model::Model other;
    auto* m = new foundation::model::ModelMaterial();
    m->setName(u8"Helm");
    other.addMaterial(m);
    CHECK(modelviewer::ApplyLooseTextures(other, PathJoin(folder.AsView(), u8"Squire.fbx").AsView()) == 1u);
    CHECK(other.textures()[0]->uri() == u8"Body.png");
    // A folder without images: nothing taken.
    foundation::model::Model bare;
    auto* n = new foundation::model::ModelMaterial();
    bare.addMaterial(n);
    CHECK(modelviewer::ApplyLooseTextures(bare, u8"/no/such/folder/x.fbx") == 0u);
    CHECK(bare.materials()[0]->baseColorTextureIndex == -1);
}

// The materials and textures, listed for the side panel and the details panes: kept in the record
// (names with spaces, a texture inside the file with no file name) and counted from a known model
// (the Fox: one PBR material whose base color is its one texture, an image beside the file).
TEST_CASE("model viewer: the materials and textures are listed, kept in the record, counted from the file")
{
    modelviewer::ModelStats stats;
    stats.textureList.PushBack(modelviewer::ModelTextureInfo{String(u8"Body Skin"), 1024, 512, String(u8"skin.png")});
    stats.textureList.PushBack(modelviewer::ModelTextureInfo{String(u8"inside"), 64, 64, String{}});
    modelviewer::ModelMaterialInfo coat;
    coat.name = String(u8"Fur coat");
    coat.shading = String(u8"Phong");
    coat.slots.PushBack(modelviewer::ModelMaterialInfo::Slot{String(u8"base color"), 0});
    coat.slots.PushBack(modelviewer::ModelMaterialInfo::Slot{String(u8"normal"), 1});
    stats.materialList.PushBack(coat);
    modelviewer::ModelMaterialInfo bare;
    bare.name = String(u8"Bare");
    bare.shading = String(u8"PBR");
    stats.materialList.PushBack(bare);

    Guid id;
    modelviewer::ModelStats read;
    REQUIRE(modelviewer::ReadRecord(modelviewer::WriteRecord(Guid(1, 2), stats).AsView(), id, read));
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
    modelviewer::ModelStats counted;
    REQUIRE(modelviewer::OpenModel::LoadFile(fox.AsView(), true, model, counted) == modelviewer::OpenModel::Outcome::Opened);
    REQUIRE(counted.materialList.Size() == 1u);
    CHECK(counted.materialList[0].name == u8"fox_material");
    CHECK(counted.materialList[0].shading == u8"PBR");
    REQUIRE(counted.materialList[0].slots.Size() == 1u);
    CHECK(counted.materialList[0].slots[0].slot == u8"base color");
    REQUIRE(counted.textureList.Size() == 1u);
    CHECK(counted.textureList[0].file == u8"Texture.png");
    CHECK(counted.textureList[0].width > 0u);
}
