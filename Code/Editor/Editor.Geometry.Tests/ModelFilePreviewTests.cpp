// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The import window's preview of a model file, without a host (no view): the file is read on the
// editor's task pool, cooked into its cache folder and bound, and its facts and clips show; opened
// again it binds from the cache at once. It publishes the frame hooks the window drives. A file
// the loaders cannot read opens nothing and says so.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.vfs;
import editor.core;
import editor.app;
import editor.geometry;
import modelimporter; // the importer's own read

using namespace foundation::core;
namespace vfs = foundation::vfs;

namespace
{
    String Fox()
    {
        static const String root = vfs::FindDataRoot();
        REQUIRE_FALSE(root.IsEmpty());
        return vfs::DataPath(root.AsView(), u8"Tests/Models/FoxNested/glTF/Fox.gltf");
    }
}

TEST_CASE("model file preview: read off the main thread, cooked and bound, its facts and clips shown; opened again from the cache")
{
    const String cache = PathJoin(GetCurrentDirectory().AsView(), u8"scratch_model_file_preview");
    (void)RemoveDirectoryRecursive(cache.AsView());
    TaskPool tasks(DefaultAllocator(), 1u);
    const editor::app::AssetPageServices services{}; // no host: no view
    const String fox = Fox();
    {
        editor::ModelFilePreview preview(fox.AsView(), cache.AsView(), tasks, DefaultAllocator(), services);
        CHECK(preview.Service<editor::app::IImportPreviewFrames>() != nullptr); // the window drives its frames
        u32 changed = 0;
        preview.onChanged = [&changed]() { ++changed; };
        CHECK_FALSE(preview.IsOpen()); // still reading
        CHECK(preview.Facts().IsEmpty());
        tasks.Finish();
        CHECK(changed == 1u);
        REQUIRE(preview.IsOpen());
        // Fox.gltf: one mesh of 576 triangles, one material and texture, 24 bones, three clips.
        const Span<const String> facts = preview.Facts();
        REQUIRE(facts.Size() == 6u);
        CHECK(facts[0] == StringView(u8"Meshes 1"));
        CHECK(facts[1] == StringView(u8"Triangles 576"));
        CHECK(facts[2] == StringView(u8"Materials 1"));
        CHECK(facts[3] == StringView(u8"Textures 1"));
        CHECK(facts[4] == StringView(u8"Bones 24"));
        CHECK(facts[5] == StringView(u8"Clips 3"));
        REQUIRE(preview.Clips().Size() == 3u);
        CHECK(preview.Clips()[0] == StringView(u8"Survey"));
    }

    // Opened again, it binds what the first open cooked, with no read.
    {
        editor::ModelFilePreview again(fox.AsView(), cache.AsView(), tasks, DefaultAllocator(), services);
        CHECK(again.IsOpen());
        CHECK(tasks.InFlight() == 0u);
        CHECK(again.Facts().Size() == 6u);
    }

    // A file the loaders cannot read: nothing opens, no facts.
    const String junk = PathJoin(cache.AsView(), u8"junk.glb");
    const char8_t bytes[] = u8"not a model";
    REQUIRE(WriteFile(junk.AsView(), Span<const byte>(reinterpret_cast<const byte*>(bytes), sizeof(bytes))).IsOk());
    editor::ModelFilePreview broken(junk.AsView(), cache.AsView(), tasks, DefaultAllocator(), services);
    tasks.Finish();
    CHECK_FALSE(broken.IsOpen());
    CHECK(broken.Facts().IsEmpty());
    (void)RemoveDirectoryRecursive(cache.AsView());
}

TEST_CASE("model file preview: given the importer's read, it cooks that and reads nothing itself")
{
    const String cache = PathJoin(GetCurrentDirectory().AsView(), u8"scratch_model_file_preview_read");
    (void)RemoveDirectoryRecursive(cache.AsView());
    TaskPool tasks(DefaultAllocator(), 1u);
    const editor::app::AssetPageServices services{};
    const String fox = Fox();
    pipeline::ModelFileImporter importer;
    RefPtr<Object> prepared = importer.PrepareOnWorker(fox.AsView(), DefaultAllocator());
    auto* loaded = Cast<pipeline::LoadedModel>(prepared.Get());
    REQUIRE(loaded != nullptr);
    editor::ModelFilePreview preview(fox.AsView(), cache.AsView(), tasks, DefaultAllocator(), services, &loaded->model);
    CHECK(preview.IsOpen()); // at once
    CHECK(tasks.InFlight() == 0u); // no read of its own
    CHECK(preview.Facts().Size() == 6u);
    (void)RemoveDirectoryRecursive(cache.AsView());
}
