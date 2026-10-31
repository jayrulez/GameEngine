// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Headless TerrainSplatTool tests (top-K model), with whole paint gestures scripted through
// ViewportToolInput. They drive a press, drag and release over an in-memory terrain (heightfield
// and SplatWeights) and check that the selected palette layer rises out of the base, that one
// command over both rasters undoes and redoes it, that the eraser reveals the base, that the brush
// is unavailable with no weights and refuses edits under Simulate, and that the save closure
// converts the source to embedded (both sidecars) so the paint survives a re-cook.

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.content;
import foundation.vfs;      // NativeFileSystem (the save round-trip test)
import foundation.resource;
import foundation.scene;
import foundation.heightfield;
import foundation.terrain.resource;
import engine.terrain;
import pipeline.core;
import terrain.pipeline;    // SplatmapAsset and its builder (the source envelope the save rewrites)
import editor.core;
import editor.viewporttools;
import editor.terrain;

using namespace foundation::core;
namespace scene = foundation::scene;
namespace hf = foundation::heightfield;
namespace terrain = foundation::terrain;

namespace
{
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

    // A straight-down ray at world (x, z); stamps land where it hits. The 64x64 fixture spans
    // -32..32, so x=0 is the raster's centre texel, 16 of 32.
    editor::ViewportToolInput RayAt(f32 x, f32 z)
    {
        editor::ViewportToolInput in;
        in.ray.origin = Float3{x, 100.0f, z != 0.0f ? z : 0.001f};
        in.ray.direction = Float3{0.0f, -1.0f, 0.0f};
        in.pointerValid = true;
        in.pointerOver = true;
        in.deltaSeconds = 1.0f / 60.0f;
        return in;
    }
    editor::ViewportToolInput CenterRay(f32) { return RayAt(0.0f, 0.0f); }

    struct Fixture
    {
        scene::Scene scene{DefaultAllocator()};
        RefPtr<hf::Heightfield> grid;
        RefPtr<terrain::SplatWeights> weights;
        RefPtr<terrain::TerrainResource> res;

        Fixture()
        {
            engine::terrain::AddTerrainSceneManagers(scene);
            auto* mgr = scene.GetSystem<engine::terrain::TerrainComponentManager>();
            grid = MakeRef<hf::Heightfield>(DefaultAllocator(), 65, Float2{64.0f, 64.0f}, 0.0f, 10.0f);
            weights = MakeRef<terrain::SplatWeights>(DefaultAllocator(), 32, 32);
            res = MakeRef<terrain::TerrainResource>(DefaultAllocator());
            res->heightfield = grid.Get();
            res->weights = weights.Get();
            res->weights.SetId(Guid{5, 55}); // the source asset guid the tool saves back to
            const scene::EntityHandle e = scene.CreateEntity(u8"terrain");
            engine::terrain::TerrainComponent& c = mgr->Add(e);
            c.terrain = res.Get();
            scene.Start();
        }
    };
}

TEST_CASE("terrain splat: a stroke stamps the selected palette layer along the drag")
{
    Fixture fx;
    editor::EditorCommandStack commands;
    FakeAssetEditSink sink;
    editor::TerrainSplatTool tool(fx.scene, commands, &sink);
    tool.SetPaletteIndex(1); // paint palette layer 1 over the (implicit) base

    CHECK(tool.IsAvailable());
    CHECK(fx.weights->BaseWeight(16, 16) == 255); // pure base before painting
    CHECK(fx.weights->WeightOfLayer(16, 16, 1) == 0);
    const u64 v0 = fx.weights->Version();

    // The press deposits one full stamp. In the distance-spaced stamp model the default strength
    // of 1 reaches one-hot at the centre at once, with no time-based trickle.
    editor::ViewportToolInput press = CenterRay(0.0f);
    press.leftPressed = true;
    press.leftDown = true;
    CHECK(tool.Update(press)); // consumed, since the brush owns the click
    CHECK(fx.weights->WeightOfLayer(16, 16, 1) == 255);
    CHECK(fx.weights->BaseWeight(16, 16) == 0);
    CHECK(fx.weights->Version() > v0); // paint bumped the version (GPU re-upload)

    // Dragging walks stamps along the world-space travel, so the swath has no gaps: texels between
    // the press point and the drag target are all painted hard.
    editor::ViewportToolInput drag = RayAt(8.0f, 0.0f); // 8 world units right, about texel 20
    drag.leftDown = true;
    CHECK(tool.Update(drag));
    CHECK(fx.weights->WeightOfLayer(18, 16, 1) > 200); // mid-path
    CHECK(fx.weights->WeightOfLayer(19, 16, 1) > 200);

    editor::ViewportToolInput release = RayAt(8.0f, 0.0f);
    release.leftReleased = true;
    (void)tool.Update(release);

    CHECK(commands.CanUndo());
    CHECK(sink.count == 1);
    CHECK(sink.lastId == Guid{5, 55});
    CHECK(sink.hasPersist);
}

TEST_CASE("terrain splat: stamps are distance-spaced - holding still adds nothing, scrubbing builds")
{
    Fixture fx;
    editor::EditorCommandStack commands;
    editor::TerrainSplatTool tool(fx.scene, commands, nullptr);
    tool.SetPaletteIndex(2);
    tool.SetStrength(0.5f); // below 1: one stamp gives half coverage, and building up needs movement

    editor::ViewportToolInput press = CenterRay(0.0f);
    press.leftPressed = true;
    press.leftDown = true;
    (void)tool.Update(press);
    const u8 afterPress = fx.weights->WeightOfLayer(16, 16, 2);
    CHECK(afterPress >= 126); // about half coverage from the press stamp
    CHECK(afterPress <= 129);

    // Holding the button still (many frames, no movement) deposits nothing more.
    for (i32 i = 0; i < 10; ++i)
    {
        editor::ViewportToolInput hold = CenterRay(0.0f);
        hold.leftDown = true;
        (void)tool.Update(hold);
    }
    CHECK(fx.weights->WeightOfLayer(16, 16, 2) == afterPress);

    // Scrubbing out and back re-stamps the centre: coverage builds toward one-hot.
    editor::ViewportToolInput out = RayAt(2.0f, 0.0f);
    out.leftDown = true;
    (void)tool.Update(out);
    editor::ViewportToolInput back = CenterRay(0.0f);
    back.leftDown = true;
    (void)tool.Update(back);
    CHECK(fx.weights->WeightOfLayer(16, 16, 2) > afterPress + 40);

    editor::ViewportToolInput release = CenterRay(0.0f);
    release.leftReleased = true;
    (void)tool.Update(release);
}

TEST_CASE("terrain splat: airbrush mode builds up while holding still (time-cadence stamps)")
{
    Fixture fx;
    editor::EditorCommandStack commands;
    editor::TerrainSplatTool tool(fx.scene, commands, nullptr);
    tool.SetPaletteIndex(4);
    tool.SetStrength(0.5f);
    tool.SetAirbrush(true);
    CHECK(tool.IsAirbrush());

    editor::ViewportToolInput press = CenterRay(0.0f);
    press.leftPressed = true;
    press.leftDown = true;
    (void)tool.Update(press);
    const u8 afterPress = fx.weights->WeightOfLayer(16, 16, 4);
    REQUIRE(afterPress >= 126); // the press stamp (about half coverage)

    // Airbrush is a gentle flow: stamps deposit strength times the period, so strength means
    // coverage per second of hover. A short hold (0.2 s) adds only a little...
    for (i32 i = 0; i < 12; ++i)
    {
        editor::ViewportToolInput hold = CenterRay(0.0f); // deltaSeconds = 1/60
        hold.leftDown = true;
        (void)tool.Update(hold);
    }
    const u8 shortHold = fx.weights->WeightOfLayer(16, 16, 4);
    CHECK(shortHold > afterPress);      // it does build while held...
    CHECK(shortHold <= afterPress + 20); // ...but gently

    // ...while a long hover (2s total) builds real coverage.
    for (i32 i = 0; i < 108; ++i)
    {
        editor::ViewportToolInput hold = CenterRay(0.0f);
        hold.leftDown = true;
        (void)tool.Update(hold);
    }
    CHECK(fx.weights->WeightOfLayer(16, 16, 4) > afterPress + 40);

    editor::ViewportToolInput release = CenterRay(0.0f);
    release.leftReleased = true;
    (void)tool.Update(release);
}

TEST_CASE("terrain splat: spacing controls stamp density along the stroke")
{
    // The same drag at low strength: wide spacing lays few stamps along the path, while tight
    // spacing lays many overlapping ones and builds far more coverage at the midpoint.
    const auto strokeAndMeasure = [](f32 spacing) -> u8
    {
        Fixture fx;
        editor::EditorCommandStack commands;
        editor::TerrainSplatTool tool(fx.scene, commands, nullptr);
        tool.SetPaletteIndex(6);
        tool.SetStrength(0.3f);
        tool.SetSpacing(spacing);
        CHECK(tool.Spacing() == doctest::Approx(spacing));

        editor::ViewportToolInput press = CenterRay(0.0f);
        press.leftPressed = true;
        press.leftDown = true;
        (void)tool.Update(press);
        editor::ViewportToolInput drag = RayAt(8.0f, 0.0f);
        drag.leftDown = true;
        (void)tool.Update(drag);
        editor::ViewportToolInput release = RayAt(8.0f, 0.0f);
        release.leftReleased = true;
        (void)tool.Update(release);
        return fx.weights->WeightOfLayer(18, 16, 6); // mid-path texel (world x = +4)
    };

    const u8 tight = strokeAndMeasure(0.05f);
    const u8 wide = strokeAndMeasure(1.0f);
    CHECK(tight > wide + 30); // denser stamps build up visibly more on the same path
    CHECK(wide > 0);          // but the wide stroke still covers the path (the walk leaves no gap)
}

TEST_CASE("terrain splat: one command per stroke undoes/redoes BOTH rasters")
{
    Fixture fx;
    editor::EditorCommandStack commands;
    FakeAssetEditSink sink;
    editor::TerrainSplatTool tool(fx.scene, commands, &sink);
    tool.SetPaletteIndex(2);

    editor::ViewportToolInput press = CenterRay(1.0f);
    press.leftPressed = true;
    press.leftDown = true;
    (void)tool.Update(press);
    editor::ViewportToolInput release = CenterRay(1.0f);
    release.leftReleased = true;
    (void)tool.Update(release);

    const u8 painted = fx.weights->WeightOfLayer(16, 16, 2);
    REQUIRE(painted > 0);
    // The paint claimed a slot whose index byte is now 2, so undo has to restore that raster too.
    bool hasIndex2 = false;
    for (u32 k = 0; k < terrain::kSplatSlotCount; ++k)
    {
        hasIndex2 = hasIndex2 || (fx.weights->SlotIndex(16, 16, k) == 2);
    }
    REQUIRE(hasIndex2);

    commands.Undo();
    CHECK(fx.weights->WeightOfLayer(16, 16, 2) == 0); // weights restored
    CHECK(fx.weights->BaseWeight(16, 16) == 255);     // pure base again
    for (u32 k = 0; k < terrain::kSplatSlotCount; ++k)
    {
        CHECK(fx.weights->SlotIndex(16, 16, k) == 0); // the index raster restored too
    }

    commands.Redo();
    CHECK(fx.weights->WeightOfLayer(16, 16, 2) == painted); // redo replays the after-state
}

TEST_CASE("terrain splat: the eraser fades paint back to the base")
{
    Fixture fx;
    editor::EditorCommandStack commands;
    editor::TerrainSplatTool tool(fx.scene, commands, nullptr);
    tool.SetPaletteIndex(3);
    tool.SetStrength(1.0f);

    // One full-strength press stamp gives one-hot paint at the centre.
    editor::ViewportToolInput press = CenterRay(0.0f);
    press.leftPressed = true;
    press.leftDown = true;
    (void)tool.Update(press);
    editor::ViewportToolInput release = CenterRay(0.0f);
    release.leftReleased = true;
    (void)tool.Update(release);
    const u8 painted = fx.weights->WeightOfLayer(16, 16, 3);
    REQUIRE(painted == 255);

    // The eraser at strength 1 is a hard eraser: one press stamp restores pure base.
    tool.SetEraser(true);
    CHECK(tool.IsEraser());
    editor::ViewportToolInput epress = CenterRay(0.0f);
    epress.leftPressed = true;
    epress.leftDown = true;
    (void)tool.Update(epress);
    editor::ViewportToolInput erelease = CenterRay(0.0f);
    erelease.leftReleased = true;
    (void)tool.Update(erelease);

    CHECK(fx.weights->WeightOfLayer(16, 16, 3) == 0);
    CHECK(fx.weights->BaseWeight(16, 16) == 255);

    // A soft eraser (strength below 1) only fades the paint per stamp.
    tool.SetEraser(false);
    editor::ViewportToolInput rp = CenterRay(0.0f);
    rp.leftPressed = true;
    rp.leftDown = true;
    (void)tool.Update(rp);
    editor::ViewportToolInput rr = CenterRay(0.0f);
    rr.leftReleased = true;
    (void)tool.Update(rr);
    REQUIRE(fx.weights->WeightOfLayer(16, 16, 3) == 255);
    tool.SetEraser(true);
    tool.SetStrength(0.5f);
    editor::ViewportToolInput sp = CenterRay(0.0f);
    sp.leftPressed = true;
    sp.leftDown = true;
    (void)tool.Update(sp);
    editor::ViewportToolInput sr = CenterRay(0.0f);
    sr.leftReleased = true;
    (void)tool.Update(sr);
    const u8 faded = fx.weights->WeightOfLayer(16, 16, 3);
    CHECK(faded > 100); // half remains
    CHECK(faded < 160);
    CHECK(fx.weights->BaseWeight(16, 16) > 90);
}

TEST_CASE("terrain splat: smooth mode feathers a hard seam and rides the stroke undo")
{
    Fixture fx;
    editor::EditorCommandStack commands;
    editor::TerrainSplatTool tool(fx.scene, commands, nullptr);

    // Author a hard paint-vs-base edge down the raster middle (left half one-hot layer 1).
    for (i32 y = 0; y < 32; ++y)
    {
        for (i32 x = 0; x < 16; ++x)
        {
            const usize at = fx.weights->TexelOffset(x, y);
            fx.weights->Indices()[at] = 1;
            fx.weights->Weights()[at] = 255;
        }
    }

    // Mode selection is mutually exclusive across paint/erase/smooth.
    tool.SetSmooth(true);
    CHECK(tool.IsSmooth());
    CHECK(!tool.IsEraser());
    tool.SetEraser(true);
    CHECK(!tool.IsSmooth());
    tool.SetSmooth(true);
    tool.SetStrength(1.0f);

    editor::ViewportToolInput press = CenterRay(0.0f);
    press.leftPressed = true;
    press.leftDown = true;
    (void)tool.Update(press);
    editor::ViewportToolInput release = CenterRay(0.0f);
    release.leftReleased = true;
    (void)tool.Update(release);

    // The boundary column faded and the first base column gained paint, so the edge is graded.
    const u8 edge = fx.weights->WeightOfLayer(15, 16, 1);
    const u8 bled = fx.weights->WeightOfLayer(16, 16, 1);
    CHECK(edge < 255);
    CHECK(edge > 0);
    CHECK(bled > 0);
    CHECK(bled < edge);

    // One command per stroke: undo restores the hard edge exactly.
    commands.Undo();
    CHECK(fx.weights->WeightOfLayer(15, 16, 1) == 255);
    CHECK(fx.weights->WeightOfLayer(16, 16, 1) == 0);

    // Selecting a palette layer leaves smooth mode.
    tool.SetPaletteIndex(2);
    CHECK(!tool.IsSmooth());
}

TEST_CASE("terrain splat: unavailable with no weights, and refuses edits while editingLocked")
{
    // A terrain with a heightfield but no weights raster: the paint tool is unavailable.
    scene::Scene bare{DefaultAllocator()};
    engine::terrain::AddTerrainSceneManagers(bare);
    auto* mgr = bare.GetSystem<engine::terrain::TerrainComponentManager>();
    RefPtr<hf::Heightfield> grid =
        MakeRef<hf::Heightfield>(DefaultAllocator(), 65, Float2{64.0f, 64.0f}, 0.0f, 10.0f);
    auto res = MakeRef<terrain::TerrainResource>(DefaultAllocator());
    res->heightfield = grid.Get(); // no weights
    const scene::EntityHandle e = bare.CreateEntity(u8"terrain");
    mgr->Add(e).terrain = res.Get();
    bare.Start();
    editor::EditorCommandStack commandsA;
    editor::TerrainSplatTool bareTool(bare, commandsA, nullptr);
    CHECK_FALSE(bareTool.IsAvailable());

    // With weights but editingLocked (Simulate), a press does nothing.
    Fixture fx;
    editor::EditorCommandStack commands;
    editor::TerrainSplatTool tool(fx.scene, commands, nullptr);
    tool.SetPaletteIndex(1);
    editor::ViewportToolInput locked = CenterRay(0.5f);
    locked.leftPressed = true;
    locked.leftDown = true;
    locked.editingLocked = true;
    (void)tool.Update(locked);
    CHECK(fx.weights->WeightOfLayer(16, 16, 1) == 0); // no edit under lock
    CHECK_FALSE(commands.CanUndo());
}

// Painting an imported (PNG-backed) splatmap is saved durably. The closure rewrites the source
// envelope (dimensions synced to the raster, fileName cleared so it becomes embedded) and both
// sidecars (the "pixels" weights and the "indices"), so a re-cook keeps the paint instead of
// regenerating it from the file. A re-import resets it explicitly.
TEST_CASE("terrain splat: a save converts an imported splatmap to embedded and survives a re-cook")
{
    using foundation::vfs::NativeFileSystem;
    namespace content = foundation::content;

    const StringView dbDir = u8"scratch_splat_persist_db";
    FileDelete(u8"scratch_splat_persist_db/splat.rasset");
    FileDelete(u8"scratch_splat_persist_db/splat.pixels.bin");
    FileDelete(u8"scratch_splat_persist_db/splat.indices.bin");
    RemoveDirectory(dbDir);
    NativeFileSystem mount(dbDir, DefaultAllocator());

    // The source asset looks imported (fileName set) and has stale dimensions; the raster is
    // 32x32.
    Guid id;
    {
        content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                    u8".rasset");
        pipeline::RegisterSplatmapAsset();
        terrain::RegisterSplatmapResourceTypes();
        auto* inst =
            db.RootGroup()->CreateInstance(u8"splat", pipeline::SplatmapAsset::StaticType());
        REQUIRE(inst != nullptr);
        id = inst->Id();
        pipeline::SplatmapAsset src;
        src.width = 8; // stale: the import decoded at native size without updating the envelope
        src.height = 8;
        src.fileName = foundation::vfs::SourcePath(u8"weights.png"); // as if imported
        REQUIRE(inst->WriteObject(src).IsOk());
    }

    // Paint a stroke on the shared runtime raster bound to that source id.
    Fixture fx;
    fx.res->weights.SetId(id);
    FakeAssetEditSink sink;
    editor::EditorCommandStack commands;
    editor::TerrainSplatTool tool(fx.scene, commands, &sink);
    tool.SetPaletteIndex(1);

    editor::ViewportToolInput press = CenterRay(0.5f);
    press.leftPressed = true;
    press.leftDown = true;
    (void)tool.Update(press);
    editor::ViewportToolInput release = CenterRay(0.0f);
    release.leftReleased = true;
    (void)tool.Update(release);
    REQUIRE(sink.lastPersist);
    CHECK(sink.lastId == id);
    const u8 painted = fx.weights->WeightOfLayer(16, 16, 1);
    REQUIRE(painted > 0); // the stroke raised palette 1

    // Drain: the envelope converts to embedded with the raster's true dimensions, then the test
    // re-cooks into a separate cooked database under the same guid. The product guid equals the
    // source guid, as in production, and the cooked instance carries the product-source type the
    // factory reads.
    const StringView cookedDir = u8"scratch_splat_persist_cooked";
    FileDelete(u8"scratch_splat_persist_cooked/splat.rasset");
    FileDelete(u8"scratch_splat_persist_cooked/splat.pixels.bin");
    FileDelete(u8"scratch_splat_persist_cooked/splat.indices.bin");
    RemoveDirectory(cookedDir);
    NativeFileSystem cookedMount(cookedDir, DefaultAllocator());
    {
        content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                    u8".rasset");
        REQUIRE(sink.lastPersist(db).IsOk());
        content::Instance* inst = db.GetInstance(id);
        REQUIRE(inst != nullptr);
        RefPtr<ISerializable> object = inst->ReadObject();
        auto* asset = Cast<pipeline::SplatmapAsset>(object.Get());
        REQUIRE(asset != nullptr);               // the envelope is still a SplatmapAsset
        CHECK(asset->fileName.View().IsEmpty()); // converted to embedded (the sidecars are authoritative)
        CHECK(asset->width == 32);               // dimensions synced to the painted raster
        CHECK(asset->height == 32);

        content::ContentDatabase cookedDb(foundation::core::DefaultAllocator(), cookedMount,
                                          foundation::core::BinarySerializerFactory(),
                                          u8".rasset");
        content::Instance* cookedInst = cookedDb.RootGroup()->CreateInstanceWithId(
            id, u8"splat", terrain::SplatWeightsSource::StaticType());
        REQUIRE(cookedInst != nullptr);

        pipeline::SplatmapAssetBuilder builder;
        NativeFileSystem srcMount(u8".", DefaultAllocator());
        pipeline::AssetBuildContext ctx{editor::EditorRootAllocator()};
        ctx.sources = &srcMount;
        ctx.source = inst;
        ctx.output = cookedInst;
        REQUIRE(builder.Build(*asset, ctx).IsOk());
    }

    // Bind the cooked product through a fresh database: the paint (weights and indices) survived.
    {
        content::ContentDatabase cookedDb(foundation::core::DefaultAllocator(), cookedMount,
                                          foundation::core::BinarySerializerFactory(),
                                          u8".rasset");
        terrain::SplatWeightsFactory factory(DefaultAllocator());
        foundation::resource::ResourceManager manager(foundation::core::DefaultAllocator(), cookedDb);
        manager.AddFactory(&factory);
        foundation::resource::Proxy<terrain::SplatWeights> cooked =
            manager.Bind<terrain::SplatWeights>(id);
        REQUIRE(cooked);
        CHECK(cooked->WeightOfLayer(16, 16, 1) == painted);
    }
    FileDelete(u8"scratch_splat_persist_cooked/splat.rasset");
    FileDelete(u8"scratch_splat_persist_cooked/splat.pixels.bin");
    FileDelete(u8"scratch_splat_persist_cooked/splat.indices.bin");
    RemoveDirectory(cookedDir);

    FileDelete(u8"scratch_splat_persist_db/splat.rasset");
    FileDelete(u8"scratch_splat_persist_db/splat.pixels.bin");
    FileDelete(u8"scratch_splat_persist_db/splat.indices.bin");
    RemoveDirectory(dbDir);
}

TEST_CASE("terrain splat: the wheel resizes the brush only with Shift (the bare wheel is the camera's)")
{
    Fixture fx;
    editor::EditorCommandStack commands;
    editor::TerrainSplatTool tool(fx.scene, commands, nullptr);
    const f32 before = tool.Radius();
    editor::ViewportToolInput wheel = RayAt(0.0f, 0.0f);
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
