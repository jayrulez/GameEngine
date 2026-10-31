// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The spline tool over a headless scene: availability, picking, a point drag as one undo step,
// a Ctrl-click insert, and the provider. The tool is driven through the framework interface and
// read through the exported state probe.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include <initializer_list>

import foundation.core;
import foundation.scene;
import foundation.spline;
import engine.spline;
import editor.core;
import editor.viewporttools;
import editor.spline;

using namespace foundation::core;
namespace scene = foundation::scene;
namespace fspline = foundation::spline;

namespace
{
    constexpr Float3 kCamera{0.0f, 0.0f, 10.0f};

    editor::ViewportToolInput Frame(Float3 through, bool pressed, bool down, bool released,
                                    bool ctrl = false)
    {
        editor::ViewportToolInput input;
        input.ray.origin = kCamera;
        input.ray.direction = Normalized(through - kCamera);
        input.cameraPosition = kCamera;
        input.cameraForward = Float3{0.0f, 0.0f, -1.0f};
        input.leftPressed = pressed;
        input.leftDown = down;
        input.leftReleased = released;
        input.ctrl = ctrl;
        return input;
    }

    // A three-point spline along X on one entity, selected.
    engine::spline::SplineComponent& Rig(scene::Scene& scene, editor::Selection<Guid>& selection)
    {
        auto* splines = scene.AddSystem<engine::spline::SplineComponentManager>();
        const scene::EntityHandle entity = scene.CreateEntity(u8"Path");
        engine::spline::SplineComponent& component = splines->Add(entity);
        for (const f32 x : {-2.0f, 0.0f, 2.0f})
        {
            fspline::SplinePoint point;
            point.position = Float3{x, 0.0f, 0.0f};
            component.curve.points.PushBack(point);
        }
        component.curve.UpdateAutoHandles();
        component.curve.RebuildArcLength();
        selection.Set(scene.GetEntityId(entity));
        return component;
    }

    editor::ViewportToolHostContext Host(scene::Scene& scene, editor::EditorCommandStack& commands,
                                         editor::Selection<Guid>& selection)
    {
        editor::ViewportToolHostContext host;
        host.scene = &scene;
        host.commands = &commands;
        host.entitySelection = &selection;
        return host;
    }

    bool Near(f32 a, f32 b, f32 tolerance = 0.01f) { return Abs(a - b) < tolerance; }
}

TEST_CASE("spline tool: available only while a selected entity carries a spline")
{
    scene::Scene scene(DefaultAllocator(), u8"t");
    editor::EditorCommandStack commands;
    editor::Selection<Guid> selection;
    UniquePtr<editor::IViewportTool> tool = editor::CreateSplineEditTool(Host(scene, commands, selection));
    REQUIRE(tool);
    CHECK(tool->Id() == StringView(u8"spline.edit"));
    CHECK_FALSE(tool->IsAvailable());
    CHECK(tool->UnavailableReason().Size() > 0); // the toolbar's refusal notice says what is missing
    (void)Rig(scene, selection);
    CHECK(tool->IsAvailable());
    selection.Clear();
    CHECK_FALSE(tool->IsAvailable());
    CHECK_FALSE(tool->Update(Frame(Float3{}, false, false, false)));
    // A tool that is not the spline tool reads as the default state.
    CHECK(editor::SplineEditToolStateOf(*tool).hoverPoint == -1);
}

TEST_CASE("spline tool: dragging a point moves it as one undo step, and a lock reverts it")
{
    scene::Scene scene(DefaultAllocator(), u8"t");
    editor::EditorCommandStack commands;
    editor::Selection<Guid> selection;
    engine::spline::SplineComponent& component = Rig(scene, selection);
    UniquePtr<editor::IViewportTool> tool = editor::CreateSplineEditTool(Host(scene, commands, selection));

    // Hover the middle point, press, drag it up two units, release.
    CHECK(tool->Update(Frame(Float3{}, false, false, false)));
    CHECK(editor::SplineEditToolStateOf(*tool).hoverPoint == 1);
    CHECK(tool->Update(Frame(Float3{}, true, true, false)));
    CHECK(editor::SplineEditToolStateOf(*tool).dragging);
    CHECK(editor::SplineEditToolStateOf(*tool).selectedPoint == 1);
    CHECK(tool->Update(Frame(Float3{0.0f, 2.0f, 0.0f}, false, true, false)));
    CHECK(Near(component.curve.points[1].position.y, 2.0f));
    CHECK(tool->Update(Frame(Float3{0.0f, 2.0f, 0.0f}, false, false, true)));
    CHECK_FALSE(editor::SplineEditToolStateOf(*tool).dragging);
    REQUIRE(commands.CanUndo());

    // Undo restores the whole table; redo brings the move back.
    commands.Undo();
    CHECK(Near(component.curve.points[1].position.y, 0.0f));
    commands.Redo();
    CHECK(Near(component.curve.points[1].position.y, 2.0f));

    // A drag interrupted by an editing lock reverts without a command.
    const usize before = commands.Size();
    (void)tool->Update(Frame(Float3{}, false, false, false));
    CHECK(tool->Update(Frame(Float3{-2.0f, 0.0f, 0.0f}, true, true, false)));
    CHECK(tool->Update(Frame(Float3{-2.0f, 1.0f, 0.0f}, false, true, false)));
    editor::ViewportToolInput locked = Frame(Float3{-2.0f, 1.0f, 0.0f}, false, true, false);
    locked.editingLocked = true;
    (void)tool->Update(locked);
    CHECK_FALSE(editor::SplineEditToolStateOf(*tool).dragging);
    CHECK(Near(component.curve.points[0].position.y, 0.0f));
    CHECK(commands.Size() == before);
}

TEST_CASE("spline tool: Ctrl-click on a segment inserts a point, undoably")
{
    scene::Scene scene(DefaultAllocator(), u8"t");
    editor::EditorCommandStack commands;
    editor::Selection<Guid> selection;
    engine::spline::SplineComponent& component = Rig(scene, selection);
    UniquePtr<editor::IViewportTool> tool = editor::CreateSplineEditTool(Host(scene, commands, selection));

    // Between the first two points, off any point: the preview appears only with Ctrl held.
    CHECK_FALSE(tool->Update(Frame(Float3{-1.0f, 0.0f, 0.0f}, false, false, false)));
    CHECK_FALSE(editor::SplineEditToolStateOf(*tool).insertPreview);
    (void)tool->Update(Frame(Float3{-1.0f, 0.0f, 0.0f}, false, false, false, true));
    CHECK(editor::SplineEditToolStateOf(*tool).insertPreview);
    CHECK(tool->Update(Frame(Float3{-1.0f, 0.0f, 0.0f}, true, true, false, true)));
    REQUIRE(component.curve.points.Size() == 4u);
    CHECK(Near(component.curve.points[1].position.x, -1.0f, 0.15f));
    REQUIRE(commands.CanUndo());
    commands.Undo();
    CHECK(component.curve.points.Size() == 3u);
}

TEST_CASE("spline tool: an empty spline takes its first points from Ctrl-clicks, undoably")
{
    scene::Scene scene(DefaultAllocator(), u8"t");
    editor::EditorCommandStack commands;
    editor::Selection<Guid> selection;
    // A component added empty, before its Initialize phase seeded it, has no points at all.
    auto* splines = scene.AddSystem<engine::spline::SplineComponentManager>();
    const scene::EntityHandle entity = scene.CreateEntity(u8"Path");
    engine::spline::SplineComponent& component = splines->Add(entity);
    selection.Set(scene.GetEntityId(entity));
    UniquePtr<editor::IViewportTool> tool = editor::CreateSplineEditTool(Host(scene, commands, selection));
    CHECK(tool->IsAvailable()); // the component is there, even with nothing to draw

    // Without Ctrl nothing happens; with Ctrl the place preview appears where the ray meets the
    // camera-facing plane through the entity.
    CHECK_FALSE(tool->Update(Frame(Float3{-1.0f, 0.0f, 0.0f}, false, false, false)));
    CHECK_FALSE(editor::SplineEditToolStateOf(*tool).placePreview);
    (void)tool->Update(Frame(Float3{-1.0f, 0.0f, 0.0f}, false, false, false, true));
    CHECK(editor::SplineEditToolStateOf(*tool).placePreview);
    CHECK_FALSE(editor::SplineEditToolStateOf(*tool).insertPreview);

    // Two Ctrl-clicks make a two-point curve, each click one undo step, with the last point
    // selected.
    CHECK(tool->Update(Frame(Float3{-1.0f, 0.0f, 0.0f}, true, true, false, true)));
    REQUIRE(component.curve.points.Size() == 1u);
    CHECK(Near(component.curve.points[0].position.x, -1.0f));
    CHECK(editor::SplineEditToolStateOf(*tool).selectedPoint == 0);
    (void)tool->Update(Frame(Float3{1.0f, 0.0f, 0.0f}, false, false, true, true)); // release
    CHECK(tool->Update(Frame(Float3{1.0f, 0.0f, 0.0f}, true, true, false, true)));
    REQUIRE(component.curve.points.Size() == 2u);
    CHECK(Near(component.curve.points[1].position.x, 1.0f));
    CHECK(component.curve.SegmentCount() == 1u);
    (void)tool->Update(Frame(Float3{1.0f, 0.0f, 0.0f}, false, false, true, true));

    // From here Ctrl-click is the insert-on-segment gesture, not placement.
    (void)tool->Update(Frame(Float3{0.0f, 0.0f, 0.0f}, false, false, false, true));
    CHECK(editor::SplineEditToolStateOf(*tool).insertPreview);
    CHECK_FALSE(editor::SplineEditToolStateOf(*tool).placePreview);

    commands.Undo();
    CHECK(component.curve.points.Size() == 1u);
    commands.Undo();
    CHECK(component.curve.points.IsEmpty());
    commands.Redo();
    CHECK(component.curve.points.Size() == 1u);
}

TEST_CASE("spline tool: the provider adds the tool once, however often it registers")
{
    scene::Scene scene(DefaultAllocator(), u8"t");
    editor::EditorCommandStack commands;
    editor::Selection<Guid> selection;
    const usize before = editor::ViewportToolProviderRegistry::Get().Count();
    editor::RegisterSplineViewportTools();
    editor::RegisterSplineViewportTools();
    CHECK(editor::ViewportToolProviderRegistry::Get().Count() == before + 1);

    editor::ViewportToolManager manager;
    editor::ViewportToolProviderRegistry::Get().CreateAll(manager, Host(scene, commands, selection));
    bool found = false;
    for (usize i = 0; i < manager.Count(); ++i)
    {
        found = found || manager.ToolAt(i)->Id() == StringView(u8"spline.edit");
    }
    CHECK(found);
}
