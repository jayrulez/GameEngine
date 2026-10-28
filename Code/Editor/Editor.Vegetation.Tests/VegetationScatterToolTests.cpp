// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// VegetationScatterTool tests (headless): a scripted stroke places a deterministic count of props
// inside its footprint into the selected prop layer (the same instances every run), none on a
// rejected slope and none where the collision query says a body is; erase removes the props under
// the brush; one command per stroke undoes and redoes; unavailable without a prop layer and
// refusing edits under Simulate; the panel provider builds for the tool id.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.scene;
import foundation.geometry;
import foundation.heightfield;
import foundation.terrain.resource;
import foundation.vegetation;
import foundation.gui;
import foundation.render; // ExtractedScene (the props draw as sets)
import engine.terrain;
import engine.vegetation;
import editor.core;
import editor.app;
import editor.viewporttools;
import editor.vegetation;

using namespace foundation::core;
namespace scene = foundation::scene;
namespace hf = foundation::heightfield;
namespace terrain = foundation::terrain;
namespace veg = foundation::vegetation;

namespace
{
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

    // A 128 m terrain (2 x 2 chunks); `rise` > 0 makes it a ramp rising with x.
    struct Fixture
    {
        scene::Scene scene{DefaultAllocator()};
        RefPtr<hf::Heightfield> grid;
        RefPtr<terrain::TerrainResource> res;
        RefPtr<foundation::geometry::StaticMesh> mesh;
        scene::EntityHandle terrain{};
        engine::vegetation::TerrainVegetationComponentManager* vegetation = nullptr;

        explicit Fixture(bool withScatteredLayer = true, f32 rise = 0.0f)
        {
            engine::terrain::AddTerrainSceneManagers(scene);
            engine::vegetation::AddVegetationSceneManagers(scene);
            vegetation = scene.GetSystem<engine::vegetation::TerrainVegetationComponentManager>();
            vegetation->SetBuildBudget(100);
            grid = MakeRef<hf::Heightfield>(DefaultAllocator(), 129, Float2{128.0f, 128.0f}, 0.0f,
                                            64.0f);
            for (i32 z = 0; z < 129; ++z)
            {
                for (i32 x = 0; x < 129; ++x)
                {
                    const f32 t = static_cast<f32>(x) / 128.0f;
                    grid->SetSample(x, z, grid->WorldYToSample(2.0f + rise * t));
                }
            }
            res = MakeRef<terrain::TerrainResource>(DefaultAllocator());
            res->heightfield = grid.Get();
            terrain = scene.CreateEntity(u8"terrain");
            scene.GetSystem<engine::terrain::TerrainComponentManager>()->Add(terrain).terrain =
                res.Get();
            mesh = foundation::geometry::Primitives::Cube(DefaultAllocator(), 1.0f);
            engine::vegetation::TerrainVegetationComponent& c = vegetation->Add(terrain);
            engine::vegetation::ProceduralVegetationLayer grass;
            grass.name = String(u8"Grass");
            grass.mesh = mesh.Get();
            grass.placement = veg::VegetationPlacement::Uniform;
            c.proceduralLayers.PushBack(grass);
            if (withScatteredLayer)
            {
                engine::vegetation::PropVegetationLayer rocks;
                rocks.name = String(u8"Rocks");
                rocks.mesh = mesh.Get();
                rocks.scaleRange = Float2{1.0f, 1.0f};
                rocks.maxSlopeDegrees = 35.0f;
                c.propLayers.PushBack(rocks);
            }
            scene.Start();
        }

        Array<Float4x4>& Rocks() { return vegetation->Get(terrain)->propLayers[0].instances; }
    };

    void Stroke(editor::VegetationScatterTool& tool, f32 x0, f32 z0, f32 x1, f32 z1, i32 steps = 8)
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

    bool SameInstances(const Array<Float4x4>& a, const Array<Float4x4>& b)
    {
        return a.Size() == b.Size() &&
               (a.IsEmpty() || MemCompare(a.Data(), b.Data(), a.Size() * sizeof(Float4x4)) == 0);
    }
}

TEST_CASE("vegetation scatter: a scripted stroke places a deterministic count inside its footprint; one command undoes and redoes it")
{
    Fixture fx;
    editor::EditorCommandStack commands;
    editor::VegetationScatterTool tool(fx.scene, commands);
    CHECK(tool.IsAvailable());
    CHECK(tool.Id() == u8"vegetation.scatter");
    tool.SetLayer(0);
    tool.SetRadius(6.0f);
    tool.SetDensity(0.5f);
    tool.SetSpacing(0.0f);

    Stroke(tool, -20.0f, 10.0f, 20.0f, 10.0f);
    const Array<Float4x4> placed = fx.Rocks();
    REQUIRE(placed.Size() > 50u);
    for (const Float4x4& m : placed)
    {
        // Inside the swept footprint (the drag plus the radius), on the surface.
        CHECK(m.m[3][0] >= -26.0f);
        CHECK(m.m[3][0] <= 26.0f);
        CHECK(Abs(m.m[3][2] - 10.0f) <= 6.0f + 1e-3f);
        CHECK(m.m[3][1] == doctest::Approx(2.0f).epsilon(0.01));
    }
    CHECK(commands.CanUndo());
    CHECK(tool.StatusText().Size() > 0);
    // The instances draw as per-chunk sets on the next extraction.
    {
        foundation::render::ExtractedScene snapshot{DefaultAllocator()};
        fx.vegetation->ExtractRenderData(snapshot);
        CHECK(snapshot.Size() >= 1u);
    }

    // The same scripted stroke on a fresh fixture places the same instances.
    Fixture again;
    editor::EditorCommandStack commands2;
    editor::VegetationScatterTool tool2(again.scene, commands2);
    tool2.SetLayer(0);
    tool2.SetRadius(6.0f);
    tool2.SetDensity(0.5f);
    tool2.SetSpacing(0.0f);
    Stroke(tool2, -20.0f, 10.0f, 20.0f, 10.0f);
    CHECK(SameInstances(again.Rocks(), placed));

    // One undo empties the layer; redo restores every prop.
    commands.Undo();
    CHECK(fx.Rocks().IsEmpty());
    commands.Redo();
    CHECK(SameInstances(fx.Rocks(), placed));

    // The eraser removes the props under the brush and nothing else; a stroke that changes
    // nothing pushes no command. The layer stays selected in erase mode (erase is per layer)
    // and the status names both.
    tool.SetEraser(true);
    tool.SetLayer(0);
    CHECK(tool.IsEraser());
    {
        const StringView status = tool.StatusText();
        (void)tool.Update(RayAt(0.0f, 0.0f));
        bool named = false;
        for (usize i = 0; i + 13 <= tool.StatusText().Size(); ++i)
        {
            named |= tool.StatusText().SubStr(i, 13) == StringView(u8"ERASE layer 1");
        }
        CHECK(named);
        (void)status;
    }
    Stroke(tool, -20.0f, 10.0f, -10.0f, 10.0f, 4);
    REQUIRE(fx.Rocks().Size() < placed.Size());
    for (const Float4x4& m : fx.Rocks())
    {
        CHECK(m.m[3][0] > -10.0f - 6.0f);
    }
    commands.Undo();
    CHECK(SameInstances(fx.Rocks(), placed));
    Stroke(tool, 60.0f, -60.0f, 60.0f, -60.0f, 1); // erasing empty ground
    CHECK(SameInstances(fx.Rocks(), placed));
    commands.Undo(); // still the stroke before: the no-op stroke pushed nothing
    CHECK(fx.Rocks().IsEmpty());
}

TEST_CASE("vegetation scatter: the layer's slope rule and the collision query reject; spacing keeps props apart")
{
    // A ramp rising 64 m over 128 m (26.6 degrees) under a 10-degree limit: nothing lands.
    Fixture steep(/*withScatteredLayer*/ true, /*rise*/ 64.0f);
    steep.vegetation->Get(steep.terrain)->propLayers[0].maxSlopeDegrees = 10.0f;
    editor::EditorCommandStack commands;
    editor::VegetationScatterTool tool(steep.scene, commands);
    tool.SetLayer(0);
    tool.SetRadius(6.0f);
    tool.SetDensity(1.0f);
    Stroke(tool, -10.0f, 0.0f, 10.0f, 0.0f);
    CHECK(steep.Rocks().IsEmpty());
    CHECK(!commands.CanUndo()); // nothing changed: no command

    // The collision query (the physics world's overlap in the editor): nothing right of x = 0.
    Fixture flat;
    editor::EditorCommandStack commands2;
    editor::VegetationScatterTool blocked(flat.scene, commands2);
    blocked.SetLayer(0);
    blocked.SetRadius(6.0f);
    blocked.SetDensity(1.0f);
    blocked.SetSpacing(0.0f);
    blocked.SetBlockedQuery([](Float3 world, f32) { return world.x > 0.0f; });
    Stroke(blocked, -10.0f, 0.0f, 10.0f, 0.0f);
    REQUIRE(!flat.Rocks().IsEmpty());
    for (const Float4x4& m : flat.Rocks())
    {
        CHECK(m.m[3][0] <= 0.0f);
    }

    // Spacing: with a 1 m cube (radius ~0.87) and spacing 2, no two props sit within ~1.7 m.
    Fixture spaced;
    editor::EditorCommandStack commands3;
    editor::VegetationScatterTool sparse(spaced.scene, commands3);
    sparse.SetLayer(0);
    sparse.SetRadius(8.0f);
    sparse.SetDensity(4.0f);
    sparse.SetSpacing(2.0f);
    Stroke(sparse, 0.0f, 0.0f, 0.0f, 0.0f, 1);
    const Array<Float4x4>& props = spaced.Rocks();
    REQUIRE(props.Size() > 3u);
    const f32 reach = 2.0f * Length(spaced.mesh->bounds.Extents());
    for (usize i = 0; i < props.Size(); ++i)
    {
        for (usize j = i + 1; j < props.Size(); ++j)
        {
            const f32 dx = props[i].m[3][0] - props[j].m[3][0];
            const f32 dz = props[i].m[3][2] - props[j].m[3][2];
            CHECK(dx * dx + dz * dz >= reach * reach - 1e-3f);
        }
    }
}

TEST_CASE("vegetation scatter: unavailable without a prop layer; refuses a missing layer and edits under Simulate")
{
    Fixture bare(/*withScatteredLayer*/ false);
    editor::EditorCommandStack commands;
    editor::VegetationScatterTool none(bare.scene, commands);
    CHECK(!none.IsAvailable());
    CHECK(none.UnavailableReason().Size() > 0); // the toolbar's refusal notice names the lack

    Fixture fx;
    editor::VegetationScatterTool tool(fx.scene, commands);
    tool.SetLayer(5); // no such prop layer: not paintable
    Stroke(tool, 0.0f, 0.0f, 5.0f, 0.0f, 2);
    CHECK(!commands.CanUndo());
    tool.SetLayer(0);
    editor::ViewportToolInput locked = RayAt(0.0f, 0.0f);
    locked.leftPressed = true;
    locked.leftDown = true;
    locked.editingLocked = true;
    (void)tool.Update(locked);
    CHECK(fx.Rocks().IsEmpty());
    CHECK(!commands.CanUndo());
    // A deactivation mid-stroke closes the gesture with its command.
    editor::ViewportToolInput press = RayAt(0.0f, 0.0f);
    press.leftPressed = true;
    press.leftDown = true;
    (void)tool.Update(press);
    tool.OnDeactivate();
    CHECK(commands.CanUndo());
    CHECK(!fx.Rocks().IsEmpty());
}

TEST_CASE("vegetation scatter panel: the provider registers for the brush id and builds its panel")
{
    editor::RegisterVegetationToolPanels();
    editor::IViewportToolPanelProvider* provider =
        editor::ViewportToolPanelRegistry::Get().FindByToolId(u8"vegetation.scatter");
    REQUIRE(provider != nullptr);
    Fixture fx;
    editor::EditorCommandStack commands;
    editor::VegetationScatterTool tool(fx.scene, commands);
    tool.SetLayer(0);
    editor::ViewportToolHostContext ctx;
    ctx.scene = &fx.scene;
    ctx.commands = &commands;
    RefPtr<foundation::gui::Node> node = provider->CreatePanel(tool, ctx);
    auto* panel = Cast<editor::app::ToolPanelBody>(node.Get());
    REQUIRE(panel != nullptr);
    // The layers by name, the chosen one checked; a wheel resize shows after a Sync.
    REQUIRE(panel->ChoiceCount(0) >= 1u);
    CHECK(panel->ChoiceAt(0, 0)->IsChecked());
    tool.SetRadius(9.0f);
    panel->Sync();
    CHECK(panel->NumberAt(0)->GetValue() == doctest::Approx(9.0));
    panel->ChoiceAt(1, 1)->HandleActivate(); // Erase
    CHECK(tool.IsEraser());
}

TEST_CASE("vegetation scatter: a layer whose mesh does not resolve is named in the status (nothing would draw)")
{
    Fixture fx;
    fx.vegetation->Get(fx.terrain)->propLayers[0].mesh = nullptr; // the reference resolves to nothing
    editor::EditorCommandStack commands;
    editor::VegetationScatterTool tool(fx.scene, commands);
    tool.SetLayer(0);
    (void)tool.Update(RayAt(0.0f, 0.0f)); // a hover resolves the layer's mesh state
    const StringView status = tool.StatusText();
    bool named = false;
    for (usize i = 0; i + 7 <= status.Size(); ++i)
    {
        named |= status.SubStr(i, 7) == StringView(u8"no mesh");
    }
    CHECK(named);
    fx.vegetation->Get(fx.terrain)->propLayers[0].mesh = fx.mesh.Get();
    (void)tool.Update(RayAt(0.0f, 0.0f));
    named = false;
    for (usize i = 0; i + 7 <= tool.StatusText().Size(); ++i)
    {
        named |= tool.StatusText().SubStr(i, 7) == StringView(u8"no mesh");
    }
    CHECK(!named);
}

TEST_CASE("vegetation scatter: the eraser reaches the props left standing over a cut (the brush picks through the hole)")
{
    // Cut a hole under placed props: they stay in the layer (hidden by the manager) until the
    // eraser removes them, which needs the brush to pick the plane INSIDE the cut - QueryRay
    // alone passes through and the brush would never activate there.
    Fixture fx;
    editor::EditorCommandStack commands;
    editor::VegetationScatterTool tool(fx.scene, commands);
    tool.SetLayer(0);
    tool.SetRadius(6.0f);
    tool.SetDensity(0.5f);
    tool.SetSpacing(0.0f);
    tool.SetBlockedQuery([](Float3, f32) { return false; });
    Stroke(tool, -20.0f, 10.0f, 20.0f, 10.0f);
    const Array<Float4x4> placed = fx.Rocks();
    REQUIRE(placed.Size() > 4u);
    u32 underCut = 0;
    for (const Float4x4& m : placed)
    {
        const f32 dx = m.m[3][0];
        const f32 dz = m.m[3][2] - 10.0f;
        underCut += (dx * dx + dz * dz < 6.0f * 6.0f) ? 1u : 0u; // inside the eraser's disc
    }
    REQUIRE(underCut > 0u);

    (void)hf::CutHoles(*fx.grid, 0.0f, 10.0f, 12.0f); // a 24 m cut under the middle of the stroke
    REQUIRE(fx.grid->HasHoles());
    CHECK(fx.Rocks().Size() == placed.Size()); // the cut deletes nothing

    tool.SetEraser(true);
    Stroke(tool, 0.0f, 10.0f, 0.0f, 10.0f, 1); // a press inside the hole
    CHECK(fx.Rocks().Size() + underCut == placed.Size()); // exactly those, no other
    for (const Float4x4& m : fx.Rocks())
    {
        const f32 dx = m.m[3][0];
        const f32 dz = m.m[3][2] - 10.0f;
        CHECK(dx * dx + dz * dz >= 6.0f * 6.0f); // nothing left under the eraser's disc
    }
    commands.Undo();
    CHECK(SameInstances(fx.Rocks(), placed));
}

TEST_CASE("vegetation scatter: the wheel resizes the brush only with Shift (the bare wheel is the camera's)")
{
    Fixture fx;
    editor::EditorCommandStack commands;
    editor::VegetationScatterTool tool(fx.scene, commands);
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
