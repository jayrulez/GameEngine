// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Headless TerrainHoleTool tests. A scripted stroke cuts the shared heightfield's hole plane with a
// hard edge, and one region-delta command undoes and redoes it. Fill restores the surface. The
// brush is unavailable with no terrain and refuses edits while editingLocked. The save closure
// writes both sidecar streams, so the cut survives a re-cook and an imported heightfield keeps its
// heights.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.content;
import foundation.vfs;
import foundation.resource;
import foundation.scene;
import foundation.heightfield;
import foundation.heightfield.resource;
import foundation.terrain.resource;
import engine.terrain;
import pipeline.core;
import heightfield.pipeline;
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
            lastPersist = Move(persist);
        }
        i32 count = 0;
        Guid lastId;
        Function<Status(foundation::content::ContentDatabase&)> lastPersist;
    };

    editor::ViewportToolInput RayAt(f32 x, f32 z)
    {
        editor::ViewportToolInput in;
        in.ray.origin = Float3{x, 100.0f, z};
        in.ray.direction = Float3{0.0f, -1.0f, 0.0f};
        in.pointerValid = true;
        in.pointerOver = true;
        in.deltaSeconds = 0.1f;
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
            for (i32 z = 0; z < 65; ++z)
            {
                for (i32 x = 0; x < 65; ++x)
                {
                    grid->SetSample(x, z, 32768); // a flat field at about 5 m, so a ray has a surface to hit
                }
            }
            res = MakeRef<terrain::TerrainResource>(DefaultAllocator());
            res->heightfield = grid.Get();
            res->heightfield.SetId(Guid{7, 99});
            const scene::EntityHandle e = scene.CreateEntity(u8"terrain");
            engine::terrain::TerrainComponent& c = mgr->Add(e);
            c.terrain = res.Get();
            scene.Start();
        }
    };


    // Substring search on a status line (the view has no Contains).
    bool Mentions(StringView text, StringView needle)
    {
        if (needle.Size() > text.Size())
        {
            return false;
        }
        for (usize i = 0; i + needle.Size() <= text.Size(); ++i)
        {
            if (text.SubStr(i, needle.Size()) == needle)
            {
                return true;
            }
        }
        return false;
    }

    // Press, drag one frame and release, at (x, z).
    void Stroke(editor::TerrainHoleTool& tool, f32 x, f32 z)
    {
        editor::ViewportToolInput press = RayAt(x, z);
        press.leftPressed = true;
        press.leftDown = true;
        (void)tool.Update(press);
        editor::ViewportToolInput release = RayAt(x, z);
        release.leftReleased = true;
        (void)tool.Update(release);
    }
}

TEST_CASE("terrain holes: a stroke cuts a hard-edged disc, bumps the version, and one command undoes / redoes it")
{
    Fixture fx;
    editor::EditorCommandStack commands;
    FakeAssetEditSink sink;
    editor::TerrainHoleTool tool(fx.scene, commands, &sink);
    CHECK(tool.IsAvailable());
    CHECK(tool.Id() == StringView(u8"terrain.hole"));
    tool.SetRadius(3.0f);
    const u64 v0 = fx.grid->Version();
    editor::ViewportToolInput press = RayAt(0.0f, 0.0f);
    press.leftPressed = true;
    press.leftDown = true;
    CHECK(tool.Update(press)); // consumed, since the brush owns the click
    CHECK(fx.grid->IsHole(32, 32));
    CHECK(fx.grid->IsHole(34, 32));      // 2 m out: inside
    CHECK_FALSE(fx.grid->IsHole(35, 32)); // 3 m out: the rim, not cut (hard edge)
    CHECK(fx.grid->Version() > v0);
    editor::ViewportToolInput release = RayAt(0.0f, 0.0f);
    release.leftReleased = true;
    (void)tool.Update(release);
    REQUIRE(commands.CanUndo());
    CHECK(sink.count == 1);
    CHECK(sink.lastId == Guid{7, 99});
    const u32 cut = fx.grid->HoleCount();
    REQUIRE(cut > 0u);
    const u64 vStroke = fx.grid->Version();
    commands.Undo();
    CHECK(fx.grid->HoleCount() == 0u);
    CHECK(fx.grid->Version() > vStroke); // undo bumps the version too, so consumers re-read
    commands.Redo();
    CHECK(fx.grid->HoleCount() == cut);
    CHECK(Mentions(tool.StatusText(), u8"CUT"));
}

TEST_CASE("terrain holes: Fill restores a cut from inside it; a fill over solid ground is no command")
{
    Fixture fx;
    editor::EditorCommandStack commands;
    editor::TerrainHoleTool tool(fx.scene, commands, nullptr);
    tool.SetRadius(3.0f);
    Stroke(tool, 0.0f, 0.0f);
    REQUIRE(fx.grid->HasHoles());
    // The brush picks the hole plane (QueryRayIgnoringHoles), so a fill lands inside the cut
    // with the same radius, without having to aim at the rim.
    tool.SetMode(editor::TerrainHoleTool::Mode::Fill);
    CHECK(Mentions(tool.StatusText(), u8"FILL"));
    Stroke(tool, 0.0f, 0.0f);
    CHECK_FALSE(fx.grid->HasHoles());
    CHECK(commands.CanUndo());
    // A fill over solid ground changes nothing and pushes nothing.
    editor::EditorCommandStack quiet;
    editor::TerrainHoleTool still(fx.scene, quiet, nullptr);
    still.SetMode(editor::TerrainHoleTool::Mode::Fill);
    Stroke(still, 10.0f, 10.0f);
    CHECK_FALSE(quiet.CanUndo());
    // A ray straight into a cut still finds the plane, so the brush works over a hole, and a
    // cut over a cut changes nothing: the input is consumed but no command is pushed.
    editor::EditorCommandStack blind;
    editor::TerrainHoleTool cutter(fx.scene, blind, nullptr);
    cutter.SetRadius(3.0f);
    Stroke(cutter, 0.0f, 0.0f);
    REQUIRE(fx.grid->HasHoles());
    const u32 cutCount = fx.grid->HoleCount();
    editor::EditorCommandStack again;
    editor::TerrainHoleTool over(fx.scene, again, nullptr);
    over.SetRadius(1.0f);
    editor::ViewportToolInput press = RayAt(0.0f, 0.0f);
    press.leftPressed = true;
    press.leftDown = true;
    CHECK(over.Update(press)); // the plane is under the cursor, so the stroke begins
    editor::ViewportToolInput release = RayAt(0.0f, 0.0f);
    release.leftReleased = true;
    (void)over.Update(release);
    CHECK(fx.grid->HoleCount() == cutCount);
    CHECK_FALSE(again.CanUndo());
}

TEST_CASE("terrain holes: unavailable with no terrain, and refuses edits while editingLocked")
{
    scene::Scene empty{DefaultAllocator()};
    engine::terrain::AddTerrainSceneManagers(empty);
    empty.Start();
    editor::EditorCommandStack commandsA;
    editor::TerrainHoleTool bare(empty, commandsA, nullptr);
    CHECK_FALSE(bare.IsAvailable());
    CHECK_FALSE(bare.UnavailableReason().IsEmpty());

    Fixture fx;
    editor::EditorCommandStack commands;
    editor::TerrainHoleTool tool(fx.scene, commands, nullptr);
    editor::ViewportToolInput locked = RayAt(0.0f, 0.0f);
    locked.leftPressed = true;
    locked.leftDown = true;
    locked.editingLocked = true;
    (void)tool.Update(locked);
    CHECK_FALSE(fx.grid->HasHoles());
    CHECK_FALSE(commands.CanUndo());
}

TEST_CASE("terrain holes: a save writes both sidecars to the source asset; the cut and the heights survive a re-cook")
{
    using foundation::vfs::NativeFileSystem;
    namespace content = foundation::content;
    const StringView dbDir = u8"scratch_hole_persist_db";
    const StringView cookedDir = u8"scratch_hole_persist_cooked";
    (void)RemoveDirectoryRecursive(dbDir);
    (void)RemoveDirectoryRecursive(cookedDir);
    NativeFileSystem mount(dbDir, DefaultAllocator());
    NativeFileSystem cookedMount(cookedDir, DefaultAllocator());
    Guid id;
    {
        content::ContentDatabase db(foundation::core::DefaultAllocator(), mount,
                                    foundation::core::BinarySerializerFactory(), u8".rasset");
        pipeline::RegisterHeightfieldAsset();
        foundation::heightfield::RegisterHeightfieldResourceTypes();
        auto* inst = db.RootGroup()->CreateInstance(u8"hf", pipeline::HeightfieldAsset::StaticType());
        REQUIRE(inst != nullptr);
        id = inst->Id();
        pipeline::HeightfieldAsset src;
        src.size = 65;
        src.worldSize = Float2{64.0f, 64.0f};
        src.minY = 0.0f;
        src.maxY = 10.0f;
        src.fileName = foundation::vfs::SourcePath(u8"legacy.png"); // an imported heightfield
        REQUIRE(inst->WriteObject(src).IsOk());
    }
    Fixture fx; // the runtime grid is flat at 32768, standing in for imported heights the save must keep
    fx.res->heightfield.SetId(id);
    FakeAssetEditSink sink;
    editor::EditorCommandStack commands;
    editor::TerrainHoleTool tool(fx.scene, commands, &sink);
    tool.SetRadius(3.0f);
    Stroke(tool, 0.0f, 0.0f);
    REQUIRE(sink.lastPersist);
    CHECK(sink.lastId == id);
    const u32 cut = fx.grid->HoleCount();
    REQUIRE(cut > 0u);
    {
        content::ContentDatabase db(foundation::core::DefaultAllocator(), mount,
                                    foundation::core::BinarySerializerFactory(), u8".rasset");
        REQUIRE(sink.lastPersist(db).IsOk());
        content::Instance* inst = db.GetInstance(id);
        REQUIRE(inst != nullptr);
        RefPtr<ISerializable> object = inst->ReadObject();
        auto* asset = Cast<pipeline::HeightfieldAsset>(object.Get());
        REQUIRE(asset != nullptr);
        CHECK(asset->fileName.View().IsEmpty()); // the sidecars are now authoritative
        content::ContentDatabase cookedDb(foundation::core::DefaultAllocator(), cookedMount,
                                          foundation::core::BinarySerializerFactory(), u8".rasset");
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
    {
        content::ContentDatabase cookedDb(foundation::core::DefaultAllocator(), cookedMount,
                                          foundation::core::BinarySerializerFactory(), u8".rasset");
        foundation::heightfield::HeightfieldFactory factory(DefaultAllocator());
        foundation::resource::ResourceManager manager(foundation::core::DefaultAllocator(), cookedDb);
        manager.AddFactory(&factory);
        foundation::resource::Proxy<hf::Heightfield> cooked = manager.Bind<hf::Heightfield>(id);
        REQUIRE(cooked);
        CHECK(cooked->HoleCount() == cut);      // the cut survived
        CHECK(cooked->IsHole(32, 32));
        CHECK(cooked->GetSample(10, 10) == 32768); // and the heights did too
    }
    (void)RemoveDirectoryRecursive(dbDir);
    (void)RemoveDirectoryRecursive(cookedDir);
}

TEST_CASE("terrain holes: the wheel resizes the brush only with Shift (the bare wheel is the camera's)")
{
    Fixture fx;
    editor::EditorCommandStack commands;
    editor::TerrainHoleTool tool(fx.scene, commands, nullptr);
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
