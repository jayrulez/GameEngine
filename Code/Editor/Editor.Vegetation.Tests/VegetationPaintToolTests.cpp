// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// VegetationPaintTool tests (headless): a paint gesture scripted through ViewportToolInput over an
// in-memory terrain with a vegetation component and a two-plane mask. The selected plane rises
// along the drag, ONE command undoes/redoes the stroke, the eraser fades, the brush is unavailable
// without a mask and refuses edits under Simulate, a stroke regrows ONLY the chunks it touched,
// the persist closure converts the source to embedded so the paint survives a re-cook, and the
// panel provider builds for the tool id.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.content;
import foundation.vfs;
import foundation.resource;
import foundation.scene;
import foundation.geometry;
import foundation.heightfield;
import foundation.terrain.resource;
import foundation.vegetation.resource;
import foundation.vegetation;
import foundation.render;
import foundation.gui;
import engine.terrain;
import engine.vegetation;
import pipeline.core;
import vegetation.pipeline;
import editor.core;
import editor.app;
import editor.viewporttools;
import editor.vegetation;

using namespace foundation::core;
namespace scene = foundation::scene;
namespace hf = foundation::heightfield;
namespace terrain = foundation::terrain;
namespace veg = foundation::vegetation;
namespace render = foundation::render;

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
            lastPersist = Move(persist);
        }
        i32 count = 0;
        Guid lastId;
        Function<Status(foundation::content::ContentDatabase&)> lastPersist;
    };

    // A straight-down ray at world (x, z); the 128 m fixture spans -64..64, so x = 0 is the
    // raster centre.
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

    struct Fixture
    {
        scene::Scene scene{DefaultAllocator()};
        RefPtr<hf::Heightfield> grid;
        RefPtr<terrain::TerrainResource> res;
        RefPtr<veg::VegetationMask> mask;
        RefPtr<foundation::geometry::StaticMesh> mesh;
        scene::EntityHandle terrain{};
        engine::vegetation::TerrainVegetationComponentManager* vegetation = nullptr;

        explicit Fixture(bool withMask = true)
        {
            engine::terrain::AddTerrainSceneManagers(scene);
            engine::vegetation::AddVegetationSceneManagers(scene);
            vegetation = scene.GetSystem<engine::vegetation::TerrainVegetationComponentManager>();
            vegetation->SetBuildBudget(100);
            grid = MakeRef<hf::Heightfield>(DefaultAllocator(), 129, Float2{128.0f, 128.0f}, 0.0f,
                                            10.0f); // 2 x 2 chunks
            res = MakeRef<terrain::TerrainResource>(DefaultAllocator());
            res->heightfield = grid.Get();
            terrain = scene.CreateEntity(u8"terrain");
            scene.GetSystem<engine::terrain::TerrainComponentManager>()->Add(terrain).terrain =
                res.Get();
            mesh = foundation::geometry::Primitives::Cube(DefaultAllocator(), 0.5f);
            engine::vegetation::TerrainVegetationComponent& c = vegetation->Add(terrain);
            engine::vegetation::ProceduralVegetationLayer flowers;
            flowers.name = String(u8"Flowers");
            flowers.mesh = mesh.Get();
            flowers.placement = veg::VegetationPlacement::Mask;
            flowers.maskPlane = 0;
            flowers.density = 0.25f;
            flowers.maxSlopeDegrees = 90.0f;
            c.proceduralLayers.PushBack(flowers);
            if (withMask)
            {
                mask = MakeRef<veg::VegetationMask>(DefaultAllocator(), 32, 32, 2);
                c.mask = mask.Get();
                c.mask.SetId(Guid{7, 77}); // the source asset guid the tool persists back to
            }
            scene.Start();
        }

        usize Extract()
        {
            render::ExtractedScene snapshot{DefaultAllocator()};
            vegetation->ExtractRenderData(snapshot);
            return snapshot.Size();
        }
    };

    void Stroke(editor::VegetationPaintTool& tool, f32 x0, f32 z0, f32 x1, f32 z1, i32 steps = 8)
    {
        editor::ViewportToolInput press = RayAt(x0, z0);
        press.leftPressed = true;
        press.leftDown = true;
        (void)tool.Update(press);
        for (i32 i = 1; i <= steps; ++i)
        {
            const f32 t = static_cast<f32>(i) / static_cast<f32>(steps);
            editor::ViewportToolInput drag = RayAt(x0 + (x1 - x0) * t, z0 + (z1 - z0) * t);
            drag.leftDown = true;
            (void)tool.Update(drag);
        }
        editor::ViewportToolInput release = RayAt(x1, z1);
        release.leftReleased = true;
        (void)tool.Update(release);
    }
}

TEST_CASE("vegetation paint: a stroke raises the selected plane along the drag; one command undoes and redoes it")
{
    Fixture fx;
    editor::EditorCommandStack commands;
    editor::VegetationPaintTool tool(fx.scene, commands, nullptr);
    CHECK(tool.IsAvailable());
    CHECK(tool.Id() == u8"vegetation.paint");
    tool.SetPlane(1);
    tool.SetRadius(8.0f);
    const u64 v0 = fx.mask->Version();

    Stroke(tool, -20.0f, 0.0f, 20.0f, 0.0f); // a 40 m drag through the centre
    // Plane 1 rose along the path (texel 16 = x 0, texel 11 = x -20), plane 0 did not.
    CHECK(fx.mask->DensityAt(1, 16, 16) == 255);
    CHECK(fx.mask->DensityAt(1, 11, 16) > 0);
    CHECK(fx.mask->DensityAt(1, 21, 16) > 0);
    CHECK(fx.mask->DensityAt(1, 16, 2) == 0); // far off the path
    CHECK(fx.mask->DensityAt(0, 16, 16) == 0);
    CHECK(fx.mask->Version() > v0);
    CHECK(commands.CanUndo());
    CHECK(tool.StatusText().Size() > 0);

    commands.Undo(); // ONE undo clears the whole stroke
    CHECK(fx.mask->DensityAt(1, 16, 16) == 0);
    CHECK(fx.mask->DensityAt(1, 11, 16) == 0);
    CHECK(!commands.CanUndo());
    commands.Redo();
    CHECK(fx.mask->DensityAt(1, 16, 16) == 255);

    // The eraser fades it back; smooth feathers.
    tool.SetEraser(true);
    CHECK(tool.IsEraser());
    Stroke(tool, -20.0f, 0.0f, 20.0f, 0.0f);
    CHECK(fx.mask->DensityAt(1, 16, 16) == 0);
    commands.Undo();
    CHECK(fx.mask->DensityAt(1, 16, 16) == 255);
    // The plane and the mode are separate choices: picking a plane while erasing keeps
    // erasing (that plane), and the status names both.
    tool.SetPlane(0);
    CHECK(tool.IsEraser());
    CHECK(tool.Plane() == 0u);
    {
        const StringView status = tool.StatusText();
        bool named = false;
        for (usize i = 0; i + 13 <= status.Size(); ++i)
        {
            named |= status.SubStr(i, 13) == StringView(u8"ERASE plane 1");
        }
        CHECK(named);
    }
    tool.SetEraser(false);
    CHECK(!tool.IsEraser());
    tool.SetSmooth(true);
    CHECK(tool.IsSmooth());
    CHECK(!tool.IsEraser());
    tool.SetEraser(true); // erase and smooth exclude each other
    CHECK(!tool.IsSmooth());
}

TEST_CASE("vegetation paint: unavailable with no mask, and refuses edits while editingLocked")
{
    Fixture bare(/*withMask*/ false);
    editor::EditorCommandStack commands;
    editor::VegetationPaintTool none(bare.scene, commands, nullptr);
    CHECK(!none.IsAvailable());
    CHECK(none.UnavailableReason().Size() > 0); // the toolbar's refusal notice names the lack
    editor::ViewportToolInput press = RayAt(0.0f, 0.0f);
    press.leftPressed = true;
    press.leftDown = true;
    CHECK(!none.Update(press)); // no pick: nothing consumed
    CHECK(!commands.CanUndo());

    Fixture fx;
    editor::VegetationPaintTool tool(fx.scene, commands, nullptr);
    editor::ViewportToolInput locked = RayAt(0.0f, 0.0f);
    locked.leftPressed = true;
    locked.leftDown = true;
    locked.editingLocked = true; // Simulate: the mask feeds the live scatter
    (void)tool.Update(locked);
    CHECK(fx.mask->DensityAt(0, 16, 16) == 0);
    CHECK(!commands.CanUndo());
    // Deactivating mid-stroke closes the gesture cleanly.
    editor::ViewportToolInput press2 = RayAt(0.0f, 0.0f);
    press2.leftPressed = true;
    press2.leftDown = true;
    (void)tool.Update(press2);
    tool.OnDeactivate();
    CHECK(commands.CanUndo());
    CHECK(fx.mask->DensityAt(0, 16, 16) == 255);
}

TEST_CASE("vegetation paint: a stroke regrows only the chunks it touched")
{
    Fixture fx;
    editor::EditorCommandStack commands;
    editor::VegetationPaintTool tool(fx.scene, commands, nullptr);
    tool.SetPlane(0);
    tool.SetRadius(4.0f);
    // Warm: every chunk scattered once (all empty - the mask is blank).
    CHECK(fx.Extract() == 0u);
    CHECK(fx.vegetation->BuildCount() == 4u);

    // A stroke inside the (-x, -z) chunk only: one chunk regrows and now draws.
    Stroke(tool, -40.0f, -40.0f, -30.0f, -40.0f);
    CHECK(fx.Extract() == 1u);
    CHECK(fx.vegetation->BuildCount() == 5u);

    // Undo regrows that chunk again (the command notifies its rect), nothing else.
    commands.Undo();
    CHECK(fx.Extract() == 0u);
    CHECK(fx.vegetation->BuildCount() == 6u);
    commands.Redo();
    CHECK(fx.Extract() == 1u);
    CHECK(fx.vegetation->BuildCount() == 7u);
}

TEST_CASE("vegetation paint: a save converts an imported mask to embedded and survives a re-cook")
{
    using foundation::vfs::NativeFileSystem;
    namespace content = foundation::content;
    const StringView dbDir = u8"scratch_vegpaint_db";
    FileDelete(u8"scratch_vegpaint_db/mask.rasset");
    FileDelete(u8"scratch_vegpaint_db/mask.densities.bin");
    RemoveDirectory(dbDir);
    NativeFileSystem mount(dbDir, DefaultAllocator());

    // The SOURCE asset: imported-style (fileName set) with stale dims and planes.
    Guid id;
    {
        content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");
        pipeline::RegisterVegetationMaskAsset();
        veg::RegisterVegetationMaskResourceTypes();
        auto* inst = db.RootGroup()->CreateInstance(u8"mask", pipeline::VegetationMaskAsset::StaticType());
        REQUIRE(inst != nullptr);
        id = inst->Id();
        pipeline::VegetationMaskAsset src;
        src.width = 8;
        src.height = 8;
        src.planeCount = 4;
        src.fileName = foundation::vfs::SourcePath(u8"mask.png");
        REQUIRE(inst->WriteObject(src).IsOk());
    }
    // Paint a stroke on the runtime mask bound to that source id.
    Fixture fx;
    fx.vegetation->Get(fx.terrain)->mask.SetId(id);
    FakeAssetEditSink sink;
    editor::EditorCommandStack commands;
    editor::VegetationPaintTool tool(fx.scene, commands, &sink);
    tool.SetPlane(1);
    Stroke(tool, -10.0f, 0.0f, 10.0f, 0.0f);
    REQUIRE(sink.lastPersist);
    CHECK(sink.count == 1);
    CHECK(sink.lastId == id);
    REQUIRE(fx.mask->DensityAt(1, 16, 16) == 255);

    // Drain: the envelope converts to embedded with the raster's true shape, then re-cook into a
    // separate cooked DB under the SAME guid; the paint survives the round trip.
    const StringView cookedDir = u8"scratch_vegpaint_cooked";
    FileDelete(u8"scratch_vegpaint_cooked/mask.rasset");
    FileDelete(u8"scratch_vegpaint_cooked/mask.densities.bin");
    RemoveDirectory(cookedDir);
    NativeFileSystem cookedMount(cookedDir, DefaultAllocator());
    {
        content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");
        REQUIRE(sink.lastPersist(db).IsOk());
        content::Instance* inst = db.GetInstance(id);
        REQUIRE(inst != nullptr);
        RefPtr<ISerializable> object = inst->ReadObject();
        auto* asset = Cast<pipeline::VegetationMaskAsset>(object.Get());
        REQUIRE(asset != nullptr);
        CHECK(asset->fileName.View().IsEmpty()); // embedded now (the sidecar is the truth)
        CHECK(asset->width == 32);
        CHECK(asset->height == 32);
        CHECK(asset->planeCount == 2u);
        content::ContentDatabase cookedDb(DefaultAllocator(), cookedMount, BinarySerializerFactory(),
                                          u8".rasset");
        content::Instance* cookedInst = cookedDb.RootGroup()->CreateInstanceWithId(
            id, u8"mask", veg::VegetationMaskSource::StaticType());
        REQUIRE(cookedInst != nullptr);
        pipeline::VegetationMaskAssetBuilder builder;
        NativeFileSystem srcMount(u8".", DefaultAllocator());
        pipeline::AssetBuildContext ctx{editor::EditorRootAllocator()};
        ctx.sources = &srcMount;
        ctx.source = inst;
        ctx.output = cookedInst;
        REQUIRE(builder.Build(*asset, ctx).IsOk());
    }
    {
        content::ContentDatabase cookedDb(DefaultAllocator(), cookedMount, BinarySerializerFactory(),
                                          u8".rasset");
        veg::VegetationMaskFactory factory(DefaultAllocator());
        foundation::resource::ResourceManager manager(DefaultAllocator(), cookedDb);
        manager.AddFactory(&factory);
        foundation::resource::Proxy<veg::VegetationMask> loaded =
            manager.Bind<veg::VegetationMask>(id);
        REQUIRE(loaded);
        CHECK(loaded->PlaneCount() == 2u);
        CHECK(loaded->DensityAt(1, 16, 16) == 255);
        CHECK(loaded->DensityAt(0, 16, 16) == 0);
    }
    FileDelete(u8"scratch_vegpaint_cooked/mask.rasset");
    FileDelete(u8"scratch_vegpaint_cooked/mask.densities.bin");
    RemoveDirectory(cookedDir);
    FileDelete(u8"scratch_vegpaint_db/mask.rasset");
    FileDelete(u8"scratch_vegpaint_db/mask.densities.bin");
    RemoveDirectory(dbDir);
}

TEST_CASE("vegetation tool panel: the provider registers for the brush id and builds its panel")
{
    editor::RegisterVegetationToolPanels(); // idempotent (first-wins per tool id)
    editor::RegisterVegetationViewportTools();
    editor::ViewportToolPanelRegistry& reg = editor::ViewportToolPanelRegistry::Get();
    editor::IViewportToolPanelProvider* provider = reg.FindByToolId(u8"vegetation.paint");
    REQUIRE(provider != nullptr);

    Fixture fx;
    editor::EditorCommandStack commands;
    editor::VegetationPaintTool tool(fx.scene, commands, nullptr);
    tool.SetPlane(1);
    editor::ViewportToolHostContext ctx;
    ctx.scene = &fx.scene;
    ctx.commands = &commands;
    RefPtr<foundation::gui::Node> node = provider->CreatePanel(tool, ctx);
    auto* panel = Cast<editor::app::ToolPanelBody>(node.Get());
    REQUIRE(panel != nullptr);
    // The planes from 1, the chosen one checked; a wheel resize shows after a Sync.
    REQUIRE(panel->ChoiceCount(0) >= 2u);
    CHECK(panel->ChoiceAt(0, 0)->GetText() == StringView(u8"1"));
    CHECK(panel->ChoiceAt(0, 1)->IsChecked());
    tool.SetRadius(12.0f);
    panel->Sync();
    CHECK(panel->NumberAt(0)->GetValue() == doctest::Approx(12.0));
    // The mode: Erase, then Smooth, each one at a time.
    REQUIRE(panel->ChoiceCount(1) == 3u);
    panel->ChoiceAt(1, 1)->HandleActivate();
    CHECK(tool.IsEraser());
    panel->ChoiceAt(1, 2)->HandleActivate();
    CHECK(tool.IsSmooth());
    CHECK_FALSE(tool.IsEraser());
}

TEST_CASE("vegetation paint: the wheel resizes the brush only with Shift (the bare wheel is the camera's)")
{
    Fixture fx;
    editor::EditorCommandStack commands;
    editor::VegetationPaintTool tool(fx.scene, commands, nullptr);
    const f32 before = tool.Radius();
    editor::ViewportToolInput wheel = RayAt(0.0f, 0.0f);
    wheel.wheelDelta = 1.0f;
    (void)tool.Update(wheel);
    CHECK(tool.Radius() == before); // the camera's scroll
    wheel.shift = true;
    (void)tool.Update(wheel);
    CHECK(tool.Radius() > before);
    wheel.wheelDelta = -1.0f;
    (void)tool.Update(wheel);
    CHECK(tool.Radius() < before * 1.13f);
}
