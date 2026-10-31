// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Headless TerrainSculptTool tests. A whole sculpt gesture can be scripted through
// ViewportToolInput without a viewport, so these drive a press, drag and release over an in-memory
// terrain. They check that the shared heightfield is raised, that one region-delta command undoes
// and redoes it, that the brush is unavailable with no terrain and refuses edits while
// editingLocked, and that the closure writing back to the source is registered on the asset-edit
// sink with the heightfield's source guid.

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.content; // ContentDatabase (the IAssetEditSink save closure's signature)
import foundation.vfs;    // NativeFileSystem (the save round-trip test)
import foundation.resource;
import foundation.scene;
import foundation.heightfield;
import foundation.heightfield.resource; // HeightfieldFactory, kHeightStream (the re-cook test)
import foundation.terrain.resource;
import engine.terrain;
import pipeline.core;
import heightfield.pipeline; // HeightfieldAsset and its builder (the source envelope the save rewrites)
import editor.core;
import editor.viewporttools;
import editor.terrain;

using namespace foundation::core;
namespace scene = foundation::scene;
namespace hf = foundation::heightfield;
namespace terrain = foundation::terrain;

namespace
{
    // Captures the last registered (guid, closure) pair so a test can check the registration and
    // run the closure.
    class FakeAssetEditSink final : public editor::IAssetEditSink
    {
    public:
        void RegisterAssetEdit(const Guid& assetId,
                               Function<Status(foundation::content::ContentDatabase&)> persist) override
        {
            ++count;
            lastId = assetId;
            hasPersist = static_cast<bool>(persist);
            lastPersist = Move(persist); // captured so tests can run it against a real database
        }

        i32 count = 0;
        Guid lastId;
        bool hasPersist = false;
        Function<Status(foundation::content::ContentDatabase&)> lastPersist;
    };

    // A straight-down ray at the terrain center (the entity transform is identity, so local
    // equals world).
    editor::ViewportToolInput CenterRay(f32 deltaSeconds)
    {
        editor::ViewportToolInput in;
        in.ray.origin = Float3{0.0f, 100.0f, 0.0f};
        in.ray.direction = Float3{0.0f, -1.0f, 0.0f};
        in.pointerValid = true;
        in.pointerOver = true;
        in.deltaSeconds = deltaSeconds;
        return in;
    }

    struct Fixture
    {
        scene::Scene scene{DefaultAllocator()};
        RefPtr<hf::Heightfield> grid;
        RefPtr<terrain::TerrainResource> res;

        Fixture()
        {
            engine::terrain::AddTerrainSceneManagers(scene);
            auto* mgr = scene.GetSystem<engine::terrain::TerrainComponentManager>();
            grid = MakeRef<hf::Heightfield>(DefaultAllocator(), 65, Float2{64.0f, 64.0f}, 0.0f, 10.0f);
            res = MakeRef<terrain::TerrainResource>(DefaultAllocator());
            res->heightfield = grid.Get();
            res->heightfield.SetId(Guid{7, 99}); // the source asset guid the tool saves back to
            const scene::EntityHandle e = scene.CreateEntity(u8"terrain");
            engine::terrain::TerrainComponent& c = mgr->Add(e);
            c.terrain = res.Get();
            scene.Start();
        }
    };
}

TEST_CASE("terrain sculpt: a press-drag-release stroke raises the shared heightfield")
{
    Fixture fx;
    editor::EditorCommandStack commands;
    FakeAssetEditSink sink;
    editor::TerrainSculptTool tool(fx.scene, commands, &sink);

    CHECK(tool.IsAvailable());
    const hf::Height before = fx.grid->GetSample(32, 32);
    CHECK(before == 0);

    // The press begins the stroke and deposits the first dab.
    editor::ViewportToolInput press = CenterRay(0.1f);
    press.leftPressed = true;
    press.leftDown = true;
    CHECK(tool.Update(press)); // consumed, since the brush owns the click

    const hf::Height afterDab = fx.grid->GetSample(32, 32);
    CHECK(afterDab > before);
    CHECK(fx.grid->Version() > 1u); // sculpt bumped the version (GPU re-upload)

    // A held drag frame raises further.
    editor::ViewportToolInput drag = CenterRay(0.1f);
    drag.leftDown = true;
    CHECK(tool.Update(drag));
    CHECK(fx.grid->GetSample(32, 32) > afterDab);

    // Release commits one command and registers the save closure.
    editor::ViewportToolInput release = CenterRay(0.1f);
    release.leftReleased = true;
    (void)tool.Update(release);

    CHECK(commands.CanUndo());
    CHECK(sink.count == 1);
    CHECK(sink.lastId == Guid{7, 99});
    CHECK(sink.hasPersist);
}

TEST_CASE("terrain sculpt: one command per stroke undoes/redoes the whole region")
{
    Fixture fx;
    editor::EditorCommandStack commands;
    FakeAssetEditSink sink;
    editor::TerrainSculptTool tool(fx.scene, commands, &sink);

    editor::ViewportToolInput press = CenterRay(0.2f);
    press.leftPressed = true;
    press.leftDown = true;
    (void)tool.Update(press);
    editor::ViewportToolInput release = CenterRay(0.2f);
    release.leftReleased = true;
    (void)tool.Update(release);

    const hf::Height raised = fx.grid->GetSample(32, 32);
    REQUIRE(raised > 0);
    const u64 versionAfterStroke = fx.grid->Version();

    commands.Undo();
    CHECK(fx.grid->GetSample(32, 32) == 0);          // region restored to the before-state
    CHECK(fx.grid->Version() > versionAfterStroke);  // undo bumps the version too

    commands.Redo();
    CHECK(fx.grid->GetSample(32, 32) == raised);     // redo replays the after-state
}

TEST_CASE("terrain sculpt: unavailable with no terrain, and refuses edits while editingLocked")
{
    // With no terrain in the scene the tool is unavailable.
    scene::Scene empty{DefaultAllocator()};
    engine::terrain::AddTerrainSceneManagers(empty);
    empty.Start();
    editor::EditorCommandStack commandsA;
    editor::TerrainSculptTool bare(empty, commandsA, nullptr);
    CHECK_FALSE(bare.IsAvailable());

    // With a terrain but editingLocked (Simulate), a press does nothing, since the collider is
    // shared.
    Fixture fx;
    editor::EditorCommandStack commands;
    editor::TerrainSculptTool tool(fx.scene, commands, nullptr);
    editor::ViewportToolInput locked = CenterRay(0.1f);
    locked.leftPressed = true;
    locked.leftDown = true;
    locked.editingLocked = true;
    (void)tool.Update(locked);
    CHECK(fx.grid->GetSample(32, 32) == 0); // no edit under lock
    CHECK_FALSE(commands.CanUndo());
}

// The save closure writes the source HeightfieldAsset envelope (never the cooked
// HeightfieldSource type, which would clobber the source asset) and clears fileName, so the
// authored "heights" sidecar becomes authoritative. The sculpt survives a re-cook through the
// builder's embedded path.
TEST_CASE("terrain sculpt: a save persists to the source asset and survives a re-cook")
{
    using foundation::vfs::NativeFileSystem;
    namespace content = foundation::content;

    const StringView dbDir = u8"scratch_sculpt_persist_db";
    FileDelete(u8"scratch_sculpt_persist_db/hf.rasset");
    FileDelete(u8"scratch_sculpt_persist_db/hf.heights.bin");
    RemoveDirectory(dbDir);
    NativeFileSystem mount(dbDir, DefaultAllocator());

    // The source asset: an envelope that looks imported (fileName set), to show the save converts
    // it.
    Guid id;
    {
        content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                    u8".rasset");
        pipeline::RegisterHeightfieldAsset();
        foundation::heightfield::RegisterHeightfieldResourceTypes();
        auto* inst =
            db.RootGroup()->CreateInstance(u8"hf", pipeline::HeightfieldAsset::StaticType());
        REQUIRE(inst != nullptr);
        id = inst->Id();
        pipeline::HeightfieldAsset src;
        src.size = 65;
        src.worldSize = Float2{64.0f, 64.0f};
        src.minY = 0.0f;
        src.maxY = 10.0f;
        src.fileName = foundation::vfs::SourcePath(u8"legacy.png"); // as if imported
        REQUIRE(inst->WriteObject(src).IsOk());
    }

    // Sculpt a stroke against the shared runtime grid bound to that source id.
    Fixture fx;
    fx.res->heightfield.SetId(id);
    FakeAssetEditSink sink;
    editor::EditorCommandStack commands;
    editor::TerrainSculptTool tool(fx.scene, commands, &sink);

    editor::ViewportToolInput press = CenterRay(0.1f);
    press.leftPressed = true;
    press.leftDown = true;
    (void)tool.Update(press);
    editor::ViewportToolInput release = CenterRay(0.0f);
    release.leftReleased = true;
    (void)tool.Update(release);
    REQUIRE(sink.lastPersist);
    CHECK(sink.lastId == id);
    const hf::Height sculpted = fx.grid->GetSample(32, 32);
    REQUIRE(sculpted > 0); // the stroke raised the grid

    // Drain: the closure writes the envelope and sidecar into the source database, then the test
    // re-cooks into a separate cooked database under the same guid. The product guid equals the
    // source guid, as in production, and the cooked instance carries the product-source type the
    // factory reads.
    const StringView cookedDir = u8"scratch_sculpt_persist_cooked";
    FileDelete(u8"scratch_sculpt_persist_cooked/hf.rasset");
    FileDelete(u8"scratch_sculpt_persist_cooked/hf.heights.bin");
    FileDelete(u8"scratch_sculpt_persist_cooked/hf.holes.bin");
    RemoveDirectory(cookedDir);
    NativeFileSystem cookedMount(cookedDir, DefaultAllocator());
    {
        content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                    u8".rasset");
        REQUIRE(sink.lastPersist(db).IsOk());
        content::Instance* inst = db.GetInstance(id);
        REQUIRE(inst != nullptr);
        RefPtr<ISerializable> object = inst->ReadObject();
        auto* asset = Cast<pipeline::HeightfieldAsset>(object.Get());
        REQUIRE(asset != nullptr);               // the envelope is still a HeightfieldAsset
        CHECK(asset->fileName.View().IsEmpty()); // converted to embedded (the sidecar is authoritative)
        CHECK(asset->size == 65);

        content::ContentDatabase cookedDb(foundation::core::DefaultAllocator(), cookedMount,
                                          foundation::core::BinarySerializerFactory(),
                                          u8".rasset");
        content::Instance* cookedInst = cookedDb.RootGroup()->CreateInstanceWithId(
            id, u8"hf", foundation::heightfield::HeightfieldSource::StaticType());
        REQUIRE(cookedInst != nullptr);

        pipeline::HeightfieldAssetBuilder builder;
        NativeFileSystem srcMount(u8".", DefaultAllocator());
        pipeline::AssetBuildContext ctx{editor::EditorRootAllocator()};
        ctx.sources = &srcMount;
        ctx.source = inst;
        ctx.output = cookedInst;
        REQUIRE(builder.Build(*asset, ctx).IsOk());
    }

    // Bind the cooked product through a fresh database: the sculpt survived the re-cook.
    {
        content::ContentDatabase cookedDb(foundation::core::DefaultAllocator(), cookedMount,
                                          foundation::core::BinarySerializerFactory(),
                                          u8".rasset");
        foundation::heightfield::HeightfieldFactory factory(DefaultAllocator());
        foundation::resource::ResourceManager manager(foundation::core::DefaultAllocator(), cookedDb);
        manager.AddFactory(&factory);
        foundation::resource::Proxy<hf::Heightfield> cooked = manager.Bind<hf::Heightfield>(id);
        REQUIRE(cooked);
        CHECK(cooked->GetSample(32, 32) == sculpted);
    }
    FileDelete(u8"scratch_sculpt_persist_cooked/hf.rasset");
    FileDelete(u8"scratch_sculpt_persist_cooked/hf.heights.bin");
    FileDelete(u8"scratch_sculpt_persist_cooked/hf.holes.bin");
    RemoveDirectory(cookedDir);

    FileDelete(u8"scratch_sculpt_persist_db/hf.rasset");
    FileDelete(u8"scratch_sculpt_persist_db/hf.heights.bin");
    RemoveDirectory(dbDir);
}

TEST_CASE("terrain sculpt: the wheel resizes the brush only with Shift (the bare wheel is the camera's)")
{
    Fixture fx;
    editor::EditorCommandStack commands;
    editor::TerrainSculptTool tool(fx.scene, commands, nullptr);
    const f32 before = tool.Radius();
    editor::ViewportToolInput wheel = []() {
        editor::ViewportToolInput in;
        in.ray.origin = Float3{0.0f, 100.0f, 0.0f};
        in.ray.direction = Float3{0.0f, -1.0f, 0.0f};
        in.pointerValid = true;
        in.pointerOver = true;
        in.deltaSeconds = 0.1f;
        return in;
    }();
    wheel.wheelDelta = 1.0f;
    (void)tool.Update(wheel);
    CHECK(tool.Radius() == before); // the wheel alone scrolls the camera
    wheel.shift = true;
    (void)tool.Update(wheel);
    CHECK(tool.Radius() > before);
    wheel.wheelDelta = -1.0f;
    (void)tool.Update(wheel);
    CHECK(tool.Radius() < before * 1.13f);
}
