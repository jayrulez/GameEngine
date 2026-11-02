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
import pipeline.importer; // ImportPlan, a texture part to show
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
        CHECK(preview.IsLoading()); // cooked; its parts now load on the pool, a few a frame
        CHECK_FALSE(preview.IsOpen());
        preview.FinishLoading();
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

    // Opened again, it loads what the first open cooked, with no read and no cook: its parts
    // load on the pool, never on the window's thread.
    {
        editor::ModelFilePreview again(fox.AsView(), cache.AsView(), tasks, DefaultAllocator(), services);
        CHECK(again.IsLoading());
        CHECK_FALSE(again.IsOpen());
        again.FinishLoading();
        CHECK(again.IsOpen());
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

TEST_CASE("model file preview: given the importer's read, it cooks that on the pool, never on the window's thread, and reads nothing itself")
{
    const String cache = PathJoin(GetCurrentDirectory().AsView(), u8"scratch_model_file_preview_read");
    (void)RemoveDirectoryRecursive(cache.AsView());
    TaskPool tasks(DefaultAllocator(), 1u);
    const editor::app::AssetPageServices services{};
    const String fox = Fox();
    pipeline::ModelFileImporter importer;
    RefPtr<Object> prepared = importer.PrepareOnWorker(fox.AsView(), DefaultAllocator());
    REQUIRE(Cast<pipeline::LoadedModel>(prepared.Get()) != nullptr);
    {
        editor::ModelFilePreview preview(fox.AsView(), cache.AsView(), tasks, DefaultAllocator(), services, prepared.Get());
        CHECK_FALSE(preview.IsOpen()); // its cook is a task: the window's thread goes on
        CHECK(tasks.InFlight() == 1u);  // the cook alone, no read of its own
        tasks.Finish();
        preview.FinishLoading(); // its parts, then the bind
        CHECK(preview.IsOpen());
        CHECK(preview.Facts().Size() == 6u);
    }
    (void)RemoveDirectoryRecursive(cache.AsView());
}

TEST_CASE("model file preview: StopReading ends its cook before the import may change the importer's model; it then never opens")
{
    const String cache = PathJoin(GetCurrentDirectory().AsView(), u8"scratch_model_file_preview_stop");
    (void)RemoveDirectoryRecursive(cache.AsView());
    TaskPool tasks(DefaultAllocator(), 1u);
    const editor::app::AssetPageServices services{};
    const String fox = Fox();
    pipeline::ModelFileImporter importer;
    RefPtr<Object> prepared = importer.PrepareOnWorker(fox.AsView(), DefaultAllocator());
    {
        editor::ModelFilePreview preview(fox.AsView(), cache.AsView(), tasks, DefaultAllocator(), services, prepared.Get());
        preview.StopReading();
        CHECK(tasks.InFlight() <= 1u); // ended (cancelled, or finished just before), only its hand-back left
        tasks.Finish();
        CHECK_FALSE(preview.IsOpen()); // a stopped preview's completion does not run
    }
    (void)RemoveDirectoryRecursive(cache.AsView());
}

TEST_CASE("model file preview: a texture part shows its picture in place of the model, from the importer's read; the whole file brings the model back")
{
    const String cache = PathJoin(GetCurrentDirectory().AsView(), u8"scratch_model_file_preview_texture");
    (void)RemoveDirectoryRecursive(cache.AsView());
    TaskPool tasks(DefaultAllocator(), 1u);
    const editor::app::AssetPageServices services{};
    const String fox = Fox();
    pipeline::ModelFileImporter importer;
    RefPtr<Object> prepared = importer.PrepareOnWorker(fox.AsView(), DefaultAllocator());
    RefPtr<pipeline::ImportOptions> options = importer.CreateOptions(DefaultAllocator());
    const pipeline::ImportPlan plan = importer.DescribeImport(fox.AsView(), options.Get(), prepared.Get());
    const pipeline::ImportPlanEntry* texture = plan.Find(pipeline::ImportResourceKind::Texture, StringView{});
    for (const pipeline::ImportPlanEntry& entry : plan.entries)
        if (entry.kind == pipeline::ImportResourceKind::Texture)
            texture = &entry;
    REQUIRE(texture != nullptr);
    {
        editor::ModelFilePreview preview(fox.AsView(), cache.AsView(), tasks, DefaultAllocator(), services, prepared.Get());
        CHECK(preview.ShowPart(*texture)); // the importer read it: its pixels are here
        CHECK(preview.ShowsPicture());
        preview.ShowWhole();
        CHECK_FALSE(preview.ShowsPicture());
        pipeline::ImportPlanEntry missing = *texture;
        missing.sourceName = String(u8"no_such_texture");
        CHECK_FALSE(preview.ShowPart(missing)); // no picture of that name: the model stays
        CHECK_FALSE(preview.ShowsPicture());
        tasks.Finish();
    }
    (void)RemoveDirectoryRecursive(cache.AsView());
}

TEST_CASE("model file preview: with its materials left out of the import it draws the default material, and its own again when they are back")
{
    const String cache = PathJoin(GetCurrentDirectory().AsView(), u8"scratch_model_file_preview_default_material");
    (void)RemoveDirectoryRecursive(cache.AsView());
    TaskPool tasks(DefaultAllocator(), 1u);
    const editor::app::AssetPageServices services{};
    const String fox = Fox();
    pipeline::ModelFileImporter importer;
    RefPtr<Object> prepared = importer.PrepareOnWorker(fox.AsView(), DefaultAllocator());
    RefPtr<pipeline::ImportOptions> options = importer.CreateOptions(DefaultAllocator());
    auto* model = Cast<pipeline::ModelImportOptions>(options.Get());
    REQUIRE(model != nullptr);
    model->importMaterials = false;
    {
        editor::ModelFilePreview preview(fox.AsView(), cache.AsView(), tasks, DefaultAllocator(), services, prepared.Get(), options.Get());
        tasks.Finish();
        preview.FinishLoading();
        REQUIRE(preview.IsOpen());
        CHECK(preview.DrawsDefaultMaterial()); // as it will import: no materials
        model->importMaterials = true;
        preview.OnSettingsChanged();
        CHECK_FALSE(preview.DrawsDefaultMaterial());
        model->importMaterials = false;
        preview.OnSettingsChanged();
        CHECK(preview.DrawsDefaultMaterial());
    }
    (void)RemoveDirectoryRecursive(cache.AsView());
}
