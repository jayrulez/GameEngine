// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Model files read on a worker thread. A file read on the worker and finished on the main thread
// cooks into the same model as an open done entirely on the main thread; a missing file is
// reported; a path requested twice while queued is read once; and Cancel hands over nothing that
// was queued or being read, while requests made after it still arrive. A tab is added before its
// model is ready and finished from the worker's read.
#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.vfs;
import foundation.model.resource;
import modelviewer;

using namespace foundation::core;
namespace vfs = foundation::vfs;

namespace
{
    String TestModel(StringView relative)
    {
        static const String root = vfs::FindDataRoot();
        REQUIRE_FALSE(root.IsEmpty());
        return vfs::DataPath(root.AsView(), relative);
    }

    String FreshCache(StringView name)
    {
        const String path = PathJoin(GetCurrentDirectory().AsView(), name);
        (void)RemoveDirectoryRecursive(path.AsView());
        REQUIRE(CreateDirectories(path.AsView()));
        return path;
    }

    // Waits, a few seconds at most, until the worker has nothing queued or under way.
    void Settle(const modelviewer::FileLoads& loads)
    {
        for (u32 i = 0; i < 2000 && loads.Pending() > 0; ++i)
            SleepMilliseconds(5);
        REQUIRE(loads.Pending() == 0);
    }

    Array<String> FilesIn(StringView folder)
    {
        Array<String> names;
        (void)ListDirectory(folder,
                            [](void* ctx, StringView name, bool) { static_cast<Array<String>*>(ctx)->PushBack(String(name)); },
                            &names);
        return names;
    }
}

TEST_CASE("model viewer loads: read on the worker and finished on the main thread, the same cooked model")
{
    const String fox = TestModel(u8"Tests/Models/FoxNested/glTF/Fox.gltf");
    const String direct = FreshCache(u8"scratch_viewer_loads_direct");
    const String worked = FreshCache(u8"scratch_viewer_loads_worker");

    editor::OpenModel once(DefaultAllocator());
    REQUIRE(once.Open(fox.AsView(), direct.AsView(), nullptr) == editor::OpenModel::Outcome::Opened);

    modelviewer::FileLoads loads(DefaultAllocator());
    loads.Request(fox.AsView(), true);
    Settle(loads);
    modelviewer::FileRead read;
    REQUIRE(loads.Take(read));
    CHECK(read.path == fox);
    REQUIRE(read.outcome == editor::OpenModel::Outcome::Opened);

    editor::OpenModel split(DefaultAllocator());
    editor::OpenModel::Outcome outcome = editor::OpenModel::Outcome::NotFound;
    REQUIRE(split.Begin(fox.AsView(), worked.AsView(), nullptr, outcome) == editor::OpenModel::Begun::NeedsFile);
    REQUIRE(split.Finish(read.model, read.stats) == editor::OpenModel::Outcome::Opened);
    REQUIRE(split.IsOpen());

    // The same numbers, the same model bound, the same files cooked.
    CHECK(split.Stats().meshes == once.Stats().meshes);
    CHECK(split.Stats().vertices == once.Stats().vertices);
    CHECK(split.Stats().triangles == once.Stats().triangles);
    CHECK(split.Stats().bones == once.Stats().bones);
    CHECK(split.Stats().clips.Size() == once.Stats().clips.Size());
    CHECK(split.Model()->meshes.Size() == once.Model()->meshes.Size());
    CHECK(split.Model()->animations.Size() == once.Model()->animations.Size());
    const Array<String> a = FilesIn(once.CacheFolder());
    const Array<String> b = FilesIn(split.CacheFolder());
    REQUIRE(a.Size() == b.Size());
    for (const String& name : a)
    {
        bool found = false;
        for (const String& other : b)
            found = found || other == name;
        CHECK(found);
    }

    // Begun again over that cache: bound, with no file to read.
    editor::OpenModel again(DefaultAllocator());
    CHECK(again.Begin(fox.AsView(), worked.AsView(), nullptr, outcome) == editor::OpenModel::Begun::Bound);
    CHECK(outcome == editor::OpenModel::Outcome::FromCache);
}

TEST_CASE("model viewer loads: a missing file is said so; a path asked twice while queued is read once")
{
    modelviewer::FileLoads loads(DefaultAllocator());
    loads.Request(u8"/no/such/model.fbx", true);
    loads.Request(u8"/no/such/model.fbx", true);
    Settle(loads);
    modelviewer::FileRead read;
    REQUIRE(loads.Take(read));
    CHECK(read.outcome == editor::OpenModel::Outcome::NotFound);
    // Requested twice, it is read once, or twice if the first read was already under way; never
    // more, and the second request is never queued beside the first.
    u32 more = 0;
    while (loads.Take(read))
        ++more;
    CHECK(more <= 1u);
}

TEST_CASE("model viewer loads: a cancelled queue hands nothing over; what is asked after it comes")
{
    const String fox = TestModel(u8"Tests/Models/FoxNested/glTF/Fox.gltf");
    const String oozi = TestModel(u8"Tests/Models/KenneyPlatformerCharacters/GLB/character-oozi.glb");
    const String oobi = TestModel(u8"Tests/Models/KenneyPlatformerCharacters/GLB/character-oobi.glb");
    modelviewer::FileLoads loads(DefaultAllocator());
    loads.Request(fox.AsView(), true);
    loads.Request(oozi.AsView(), true);
    loads.Request(oobi.AsView(), true);
    CHECK(loads.IsPending(oobi.AsView()));
    loads.Cancel(); // the fox may be under way: it finishes and is dropped
    CHECK_FALSE(loads.IsPending(oobi.AsView()));
    Settle(loads);
    modelviewer::FileRead read;
    CHECK_FALSE(loads.Take(read));

    loads.Request(oobi.AsView(), true);
    Settle(loads);
    REQUIRE(loads.Take(read));
    CHECK(read.path == oobi);
    CHECK(read.outcome == editor::OpenModel::Outcome::Opened);
    CHECK_FALSE(loads.Take(read));
}

TEST_CASE("model viewer loads: a tab added before its model is ready, finished from the worker's read")
{
    const String fox = TestModel(u8"Tests/Models/FoxNested/glTF/Fox.gltf");
    const String cache = FreshCache(u8"scratch_viewer_loads_tabs");
    modelviewer::ModelTabs tabs(DefaultAllocator());
    const modelviewer::ModelTabs::Opened begun = tabs.Begin(fox.AsView(), cache.AsView(), nullptr);
    REQUIRE(begun.added);
    REQUIRE(begun.needsFile);
    CHECK_FALSE(tabs.At(begun.index).ready);
    CHECK(tabs.Active() == begun.index);

    modelviewer::FileLoads loads(DefaultAllocator());
    loads.Request(fox.AsView(), true);
    Settle(loads);
    modelviewer::FileRead read;
    REQUIRE(loads.Take(read));
    REQUIRE(tabs.Finish(tabs.Find(read.path.AsView()), read) == editor::OpenModel::Outcome::Opened);
    CHECK(tabs.At(begun.index).ready);
    CHECK(tabs.At(begun.index).open->Model() != nullptr);

    // Opened again in a second set of tabs: bound from the cache and ready at once.
    modelviewer::ModelTabs other(DefaultAllocator());
    const modelviewer::ModelTabs::Opened cached = other.Begin(fox.AsView(), cache.AsView(), nullptr);
    CHECK_FALSE(cached.needsFile);
    CHECK(other.At(cached.index).ready);

    // A read that failed leaves the tab not ready, for the window to close.
    modelviewer::ModelTabs failing(DefaultAllocator());
    const String fresh = FreshCache(u8"scratch_viewer_loads_tabs_fail");
    const modelviewer::ModelTabs::Opened waiting = failing.Begin(fox.AsView(), fresh.AsView(), nullptr);
    modelviewer::FileRead bad;
    bad.path = fox;
    bad.outcome = editor::OpenModel::Outcome::LoadFailed;
    CHECK(failing.Finish(waiting.index, bad) == editor::OpenModel::Outcome::LoadFailed);
    CHECK_FALSE(failing.At(waiting.index).ready);
}

TEST_CASE("model viewer loads: given the open model's cache, the worker cooks what it read, and only the bind is left; two loads share one pool")
{
    const String fox = TestModel(u8"Tests/Models/FoxNested/glTF/Fox.gltf");
    const String oozi = TestModel(u8"Tests/Models/KenneyPlatformerCharacters/GLB/character-oozi.glb");
    const String cache = FreshCache(u8"scratch_viewer_loads_cook");
    TaskPool reads(DefaultAllocator(), 1u);
    modelviewer::FileLoads tabs(DefaultAllocator(), reads);
    modelviewer::FileLoads thumbnails(DefaultAllocator(), reads);

    editor::OpenModel open(DefaultAllocator());
    editor::OpenModel::Outcome outcome = editor::OpenModel::Outcome::NotFound;
    REQUIRE(open.Begin(fox.AsView(), cache.AsView(), nullptr, outcome) == editor::OpenModel::Begun::NeedsFile);
    tabs.Request(fox.AsView(), true, open.Cache());
    thumbnails.Request(oozi.AsView(), true); // read only: no cache given
    Settle(tabs);
    Settle(thumbnails);

    modelviewer::FileRead read;
    REQUIRE(tabs.Take(read));
    CHECK(read.path == fox);
    REQUIRE(read.outcome == editor::OpenModel::Outcome::Opened);
    REQUIRE_FALSE(read.cooked.IsNil()); // cooked on the worker
    CHECK(FileExists(PathJoin(open.CacheFolder(), StringView(editor::OpenModel::kRecordName)).AsView()));
    CHECK(open.Bind(read.cooked, read.stats) == editor::OpenModel::Outcome::Opened);
    CHECK(open.IsOpen());
    CHECK_FALSE(tabs.Take(read)); // each load hands over only its own

    REQUIRE(thumbnails.Take(read));
    CHECK(read.path == oozi);
    CHECK(read.outcome == editor::OpenModel::Outcome::Opened);
    CHECK(read.cooked.IsNil());
}
