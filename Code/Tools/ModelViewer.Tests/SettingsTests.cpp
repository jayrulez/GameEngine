// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The model viewer's preferences and recents (Documentation/Specs/model-viewer.md, P9): the
// settings file's round trip keeps a line it does not know; recents come newest first, once each,
// as many as the preference keeps; settings.set takes what it names and refuses what is out of
// range, changing nothing; a screenshot's name from its pattern; cache.clear empties the cache
// folders and is refused while the window says it is busy.
#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.json;
import modelviewer;

using namespace foundation::core;
using foundation::json::JsonValue;

namespace
{
    String Folder(StringView name)
    {
        const String path = PathJoin(GetCurrentDirectory().AsView(), name);
        (void)RemoveDirectoryRecursive(path.AsView());
        REQUIRE(CreateDirectories(path.AsView()));
        return path;
    }

    void Write(StringView path, StringView text)
    {
        REQUIRE(WriteFile(path, Span<const byte>(reinterpret_cast<const byte*>(text.Data()), text.Size())).IsOk());
    }
}

TEST_CASE("model viewer settings: the file's round trip keeps a line it does not know")
{
    const StringView text = u8"folder /models\nloose 0\nthumbnail-size 192\nexposure -1.5\nfuture-thing on\nrecent-model /a.glb\nrecent-model /b.fbx\n"
                            u8"recent-folder /models\n";
    const modelviewer::ViewerSettings read = modelviewer::ReadSettings(text);
    CHECK(read.folder == u8"/models");
    CHECK_FALSE(read.looseTextures);
    CHECK(read.thumbnailSize == 192);
    CHECK(read.exposure == doctest::Approx(-1.5f));
    CHECK(read.loop); // not in the file: the default
    REQUIRE(read.unknown.Size() == 1);
    CHECK(read.unknown[0] == u8"future-thing on");
    REQUIRE(read.recentModels.Size() == 2);
    CHECK(read.recentModels[0] == u8"/a.glb");

    // Written and read again: the same, the unknown line still there.
    const String written = modelviewer::WriteSettings(read);
    CHECK(written.AsView().ContainsIgnoreCase(u8"future-thing on\n"));
    const modelviewer::ViewerSettings again = modelviewer::ReadSettings(written.AsView());
    CHECK(again.folder == read.folder);
    CHECK(again.looseTextures == read.looseTextures);
    CHECK(again.thumbnailSize == read.thumbnailSize);
    CHECK(again.exposure == doctest::Approx(read.exposure));
    CHECK(again.unknown.Size() == 1);
    CHECK(again.recentModels.Size() == 2);
    CHECK(again.recentModels[1] == u8"/b.fbx");
    CHECK(again.recentFolders.Size() == 1);
    CHECK(modelviewer::WriteSettings(again) == written);

    // The viewer before P9 wrote two lines; they read as before.
    const modelviewer::ViewerSettings old = modelviewer::ReadSettings(u8"folder /old\nloose 1\n");
    CHECK(old.folder == u8"/old");
    CHECK(old.looseTextures);
    CHECK(old.unknown.IsEmpty());
}

TEST_CASE("model viewer settings: recents are newest first, once each, as many as kept")
{
    Array<String> list;
    modelviewer::PushRecent(list, u8"/a", 3);
    modelviewer::PushRecent(list, u8"/b", 3);
    modelviewer::PushRecent(list, u8"/c", 3);
    modelviewer::PushRecent(list, u8"/a", 3); // again: to the front, not twice
    REQUIRE(list.Size() == 3);
    CHECK(list[0] == u8"/a");
    CHECK(list[1] == u8"/c");
    CHECK(list[2] == u8"/b");
    modelviewer::PushRecent(list, u8"/d", 3); // over the limit: the oldest goes
    REQUIRE(list.Size() == 3);
    CHECK(list[0] == u8"/d");
    CHECK(list[2] == u8"/c");

    // Through the store and its commands: kept in the file, listed newest first.
    const String folder = Folder(u8"scratch_viewer_recents");
    const String path = PathJoin(folder.AsView(), u8"settings.txt");
    {
        modelviewer::SettingsStore store;
        store.Open(path.AsView());
        store.Recent(false, u8"/m1.glb");
        store.Recent(false, u8"/m2.glb");
        store.Recent(true, u8"/models");
    }
    modelviewer::SettingsStore store;
    store.Open(path.AsView());
    modelviewer::CommandSet set(DefaultAllocator());
    modelviewer::RegisterSettingsCommands(set, store, Span<const String>{});
    const modelviewer::CommandResult recents = set.Execute(u8"recents.list");
    REQUIRE(recents.ok);
    REQUIRE(recents.value.Get(String(u8"models")).Count() == 2);
    CHECK(recents.value.Get(String(u8"models")).At(0).AsString() == u8"/m2.glb");
    CHECK(recents.value.Get(String(u8"folders")).At(0).AsString() == u8"/models");

    // Fewer kept: the lists cut at once.
    JsonValue fewer = JsonValue::MakeObject();
    fewer.Set(String(u8"recentCount"), JsonValue::MakeNumber(1));
    REQUIRE(set.Execute(u8"settings.set", fewer).ok);
    CHECK(set.Execute(u8"recents.list").value.Get(String(u8"models")).Count() == 1);
}

TEST_CASE("model viewer settings: settings.set takes what it names, and refuses what is out of range")
{
    const String folder = Folder(u8"scratch_viewer_settings_set");
    const String path = PathJoin(folder.AsView(), u8"settings.txt");
    Write(path.AsView(), u8"folder /kept\nhand-written yes\n");
    modelviewer::SettingsStore store;
    store.Open(path.AsView());
    i32 heard = 0;
    store.onChanged = [&heard]() { ++heard; };
    modelviewer::CommandSet set(DefaultAllocator());
    modelviewer::RegisterSettingsCommands(set, store, Span<const String>{});

    JsonValue change = JsonValue::MakeObject();
    change.Set(String(u8"thumbnailSize"), JsonValue::MakeNumber(96));
    change.Set(String(u8"loop"), JsonValue::MakeBool(false));
    change.Set(String(u8"density"), JsonValue::MakeString(String(u8"comfortable")));
    const modelviewer::CommandResult set1 = set.Execute(u8"settings.set", change);
    REQUIRE(set1.ok);
    CHECK(set1.value.Get(String(u8"thumbnailSize")).AsInt() == 96);
    CHECK(heard == 1);
    // In the file, with what it did not name as it was.
    modelviewer::SettingsStore reread;
    reread.Open(path.AsView());
    CHECK(reread.value.thumbnailSize == 96);
    CHECK_FALSE(reread.value.loop);
    CHECK(reread.value.density == u8"comfortable");
    CHECK(reread.value.folder == u8"/kept");
    REQUIRE(reread.value.unknown.Size() == 1);
    CHECK(reread.value.unknown[0] == u8"hand-written yes");

    // Out of range: refused, nothing changed (the good value beside it neither).
    JsonValue bad = JsonValue::MakeObject();
    bad.Set(String(u8"loop"), JsonValue::MakeBool(true));
    bad.Set(String(u8"thumbnailSize"), JsonValue::MakeNumber(100));
    const modelviewer::CommandResult refused = set.Execute(u8"settings.set", bad);
    CHECK_FALSE(refused.ok);
    CHECK(refused.error.AsView().ContainsIgnoreCase(u8"96, 128 or 192"));
    CHECK_FALSE(store.value.loop);
    CHECK(heard == 1);
    JsonValue dark = JsonValue::MakeObject();
    dark.Set(String(u8"exposure"), JsonValue::MakeNumber(9));
    CHECK_FALSE(set.Execute(u8"settings.set", dark).ok);
    JsonValue blank = JsonValue::MakeObject();
    blank.Set(String(u8"screenshotPattern"), JsonValue::MakeString(String(u8"")));
    CHECK_FALSE(set.Execute(u8"settings.set", blank).ok);

    // settings.get: the preferences, and where screenshots go when the folder is left empty.
    const modelviewer::CommandResult got = set.Execute(u8"settings.get");
    REQUIRE(got.ok);
    CHECK(got.value.Get(String(u8"density")).AsString() == u8"comfortable");
    CHECK(got.value.Get(String(u8"screenshotFolderUsed")).AsString() == modelviewer::DefaultScreenshotFolder());
}

TEST_CASE("model viewer settings: a screenshot's name from its pattern")
{
    CHECK(modelviewer::ScreenshotName(u8"{model}_{time}", u8"/models/Knight.glb", u8"", 42) == u8"Knight_42.png");
    CHECK(modelviewer::ScreenshotName(u8"{model} {clip}", u8"/m/hero.fbx", u8"Run", 0) == u8"hero Run.png");
    // What a file name cannot hold becomes _.
    CHECK(modelviewer::ScreenshotName(u8"{clip}", u8"/m/a.glb", u8"Armature|Walk", 0) == u8"Armature_Walk.png");
    CHECK(modelviewer::ScreenshotName(u8"{clip}", u8"/m/a.glb", u8"", 0) == u8"screenshot.png");
}

TEST_CASE("model viewer settings: cache.clear empties the cache folders, refused while busy")
{
    const String folder = Folder(u8"scratch_viewer_cache_clear");
    const String cache = PathJoin(folder.AsView(), u8"cache");
    const String thumbs = PathJoin(folder.AsView(), u8"thumbnails");
    REQUIRE(CreateDirectories(PathJoin(cache.AsView(), u8"ab12").AsView()));
    REQUIRE(CreateDirectories(thumbs.AsView()));
    Write(PathJoin(cache.AsView(), u8"ab12/model.bin").AsView(), u8"0123456789");
    Write(PathJoin(thumbs.AsView(), u8"ab12.png").AsView(), u8"png!");
    CHECK(modelviewer::FolderBytes(folder.AsView()) == 14);

    modelviewer::SettingsStore store;
    String busy = String(u8"close the model tabs first");
    modelviewer::CommandSet set(DefaultAllocator());
    const String folders[] = {cache, thumbs};
    modelviewer::RegisterSettingsCommands(set, store, Span<const String>(folders, 2), [&busy]() { return busy; });
    CHECK(set.Execute(u8"settings.get").value.Get(String(u8"cacheBytes")).AsInt() == 14);

    const modelviewer::CommandResult refused = set.Execute(u8"cache.clear");
    CHECK_FALSE(refused.ok);
    CHECK(refused.error == u8"close the model tabs first");
    CHECK(FileExists(PathJoin(cache.AsView(), u8"ab12/model.bin").AsView()));

    busy = {};
    const modelviewer::CommandResult cleared = set.Execute(u8"cache.clear");
    REQUIRE(cleared.ok);
    CHECK(cleared.value.Get(String(u8"freed")).AsInt() == 14);
    CHECK(DirectoryExists(cache.AsView())); // there, empty, for what is cooked next
    CHECK(modelviewer::FolderBytes(folder.AsView()) == 0);
}
