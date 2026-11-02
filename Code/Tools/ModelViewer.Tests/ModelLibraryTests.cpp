// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The model viewer's headless logic over an opened model (the opening itself is
// Editor.ModelPreview's, tested there): the side panel's rows from a known model's numbers, the
// tabs' open, reopen, move and close, browsing a folder, thumbnails' file names, the grid's
// filters, and the shortlist.
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





TEST_CASE("model viewer: a known model's numbers, as the side panel shows them")
{
    // Fox.gltf: one mesh of 1728 vertices with no index buffer (576 triangles), one material and
    // texture, one skin of 24 joints, three clips; its position accessor's bounds.
    const String fox = TestModel(u8"Tests/Models/FoxNested/glTF/Fox.gltf");
    const String cache = FreshCache(u8"scratch_model_viewer_stats");
    editor::OpenModel open(DefaultAllocator());
    REQUIRE(open.Open(fox.AsView(), cache.AsView(), nullptr) == editor::OpenModel::Outcome::Opened);
    const editor::ModelStats& stats = open.Stats();
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

    // Opened again: its existing tab comes to the front, and no second tab is added.
    const modelviewer::ModelTabs::Opened again = tabs.Open(fox.AsView(), cache.AsView(), nullptr);
    CHECK(again.index == 0);
    CHECK_FALSE(again.added);
    CHECK(tabs.Count() == 2);
    CHECK(tabs.Active() == 0);

    // A file that fails to open adds no tab and leaves the front tab unchanged.
    CHECK(tabs.Open(u8"/no/such/model.fbx", cache.AsView(), nullptr).index == -1);
    CHECK(tabs.Count() == 2);
    CHECK(tabs.Active() == 0);

    // After a move, the front tab stays in front.
    CHECK(tabs.Move(0, 1));
    CHECK(tabs.At(1).title == u8"Fox.gltf");
    CHECK(tabs.Active() == 1);

    // Closing the front tab with none after it brings the one before to the front; closing the
    // last one leaves none.
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
    CHECK(tabs.Close(0) == 0); // the tab after it comes to the front
    CHECK(tabs.ActiveTab()->title == u8"character-oobi.glb");
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
    CHECK(editor::FileTitle(listing.folders[0].AsView()) == u8"beasts");
    CHECK(editor::FileTitle(listing.folders[1].AsView()) == u8"Knights");
    REQUIRE(listing.models.Size() == 2u); // not the text file
    CHECK(editor::FileTitle(listing.models[0].AsView()) == u8"Alpha.glb");
    CHECK(editor::FileTitle(listing.models[1].AsView()) == u8"zeta.FBX");

    Array<String> all;
    modelviewer::CollectModels(root.AsView(), all);
    REQUIRE(all.Size() == 4u); // this folder's models, then each subfolder's in order
    CHECK(editor::FileTitle(all[2].AsView()) == u8"knight.gltf");
    CHECK(editor::FileTitle(all[3].AsView()) == u8"helm.obj");
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
    CHECK(modelviewer::ThumbnailPathFor(thumbs.AsView(), model.AsView()) == first); // an unchanged file keeps the same name
    Touch(model.AsView(), u8"v 0 0 0\nv 1 0 0"); // a changed size gives a new name
    CHECK(modelviewer::ThumbnailPathFor(thumbs.AsView(), model.AsView()) != first);
    CHECK(modelviewer::ThumbnailPathFor(thumbs.AsView(), u8"/no/such/model.obj").IsEmpty());
}

TEST_CASE("model viewer: the grid's filters by name, rigged and animated; numbers read from the cache")
{
    editor::ModelStats rigged;
    rigged.skins = 1;
    rigged.clips.PushBack(String(u8"Idle"));
    rigged.clipSeconds.PushBack(1.0f);
    editor::ModelStats prop;

    modelviewer::BrowseFilter filter;
    CHECK(modelviewer::Matches(filter, u8"/m/Knight.fbx", nullptr)); // no filter: everything
    filter.name = String(u8"knig");
    CHECK(modelviewer::Matches(filter, u8"/m/Knight.fbx", nullptr)); // any case
    CHECK_FALSE(modelviewer::Matches(filter, u8"/knights/crate.fbx", nullptr)); // matches the file's name, not its folder
    filter.name = {};
    filter.riggedOnly = true;
    CHECK(modelviewer::Matches(filter, u8"/m/Knight.fbx", &rigged));
    CHECK_FALSE(modelviewer::Matches(filter, u8"/m/crate.fbx", &prop));
    CHECK_FALSE(modelviewer::Matches(filter, u8"/m/unknown.fbx", nullptr)); // not cooked yet, so not known to be rigged
    filter.riggedOnly = false;
    filter.animatedOnly = true;
    CHECK(modelviewer::Matches(filter, u8"/m/Knight.fbx", &rigged));
    CHECK_FALSE(modelviewer::Matches(filter, u8"/m/crate.fbx", &prop));
    filter.animatedOnly = false;
    filter.inLibraryOnly = true;
    CHECK(modelviewer::Matches(filter, u8"/m/crate.fbx", &prop, /*inLibrary*/ true));
    CHECK_FALSE(modelviewer::Matches(filter, u8"/m/Knight.fbx", &rigged, false));

    // A model cooked once has its stats in the cache; one never opened has none.
    const String fox = TestModel(u8"Tests/Models/FoxNested/glTF/Fox.gltf");
    const String cache = FreshCache(u8"scratch_model_viewer_cached_stats");
    editor::ModelStats read;
    CHECK_FALSE(modelviewer::ReadCachedStats(cache.AsView(), fox.AsView(), read));
    {
        editor::OpenModel open(DefaultAllocator());
        REQUIRE(open.Open(fox.AsView(), cache.AsView(), nullptr) == editor::OpenModel::Outcome::Opened);
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
    list.SetStarred(u8"/m/Knight.fbx", true); // starring twice keeps one entry
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

    read.SetStarred(u8"/m/Knight.fbx", false); // unstarring drops its note too
    CHECK_FALSE(read.IsStarred(u8"/m/Knight.fbx"));
    CHECK(read.Note(u8"/m/Knight.fbx").IsEmpty());
    CHECK(read.Count() == 1u);
}

