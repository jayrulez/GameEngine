// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :scene_overlays partition.
//
// What the scene page draws into its scene, into its viewport's own debug list (so never into a
// second view of the same scene): the grid on its plane and the origin's axes, a marker at each
// entity and a box round each selected one, the tools' and the components' gizmos. Its toggles are
// SceneOverlaySettings, which the page keeps per scene. Out of the page's class as it was
// (Documentation/Specs/editor-gui.md, 5.3, layer 2); what it reads each frame is handed in. What
// is written rather than drawn in the scene (the stats, the tool's status, the zoom readout, the
// view gizmo) is the HUD's, on the GUI (layer 5).

module;
#include "Core/Prelude.h"

export module editor.scene:scene_overlays;

import foundation.core;
import foundation.scene;
import foundation.render;
import editor.core;
import editor.camera;
import editor.viewporttools;
import :scene_grid;
import :view_settings;
import :component_gizmos;

using namespace foundation::core;

export namespace editor
{
    /// The overlays a scene page shows, kept per scene (SceneViewPref).
    struct SceneOverlaySettings
    {
        bool showGrid = true;
        GridPlane gridPlane = GridPlane::XZ; // the plane the grid lies on
        bool gridLines = false;              // debug lines instead of the shader grid, to compare
        bool showLodOverlay = false;
        bool showColliders = false;
        bool showMarkers = false; // every entity's origin cross (the selection keeps its own)
        bool showFps = false; // the HUD's stats
    };

    /// One frame of what the overlays read.
    struct SceneOverlayFrame
    {
        foundation::scene::Scene* scene = nullptr;
        Selection<Guid>* selection = nullptr;
        const EditorCamera* camera = nullptr;
        u32 width = 0; // the viewport's render size
        u32 height = 0;
        ViewportToolManager* tools = nullptr;
        GizmoRendererRegistry* componentGizmos = nullptr;
        Guid previewed; // the entity the animation panel's preview poses; nil: none
    };

    class SceneOverlays
    {
    public:
        SceneOverlaySettings settings;

        /// Everything, in order, into `dd`.
        void Draw(foundation::render::debug::DebugDraw& dd, const SceneOverlayFrame& frame)
        {
            if (frame.scene == nullptr || frame.camera == nullptr)
                return;
            if (settings.showGrid)
            {
                DrawGrid(dd, *frame.camera);
                dd.DrawLine(Float3{0, 0, 0}, Float3{1, 0, 0}, Color{0.9f, 0.2f, 0.2f, 1.0f});
                dd.DrawLine(Float3{0, 0, 0}, Float3{0, 1, 0}, Color{0.2f, 0.9f, 0.2f, 1.0f});
                dd.DrawLine(Float3{0, 0, 0}, Float3{0, 0, 1}, Color{0.2f, 0.4f, 0.95f, 1.0f});
            }
            DrawEntityMarkers(dd, frame);
            DrawGizmos(dd, frame);
            DrawPreviewMarker(dd, frame);
        }

        /// The grid's spacing where the camera is now: what the grid draws and the zoom readout says.
        [[nodiscard]] GridSpacing CurrentGridSpacing(const EditorCamera& camera) const { return GridSpacingFor(GridViewHeight(camera)); }

    private:
        // How high the camera is over the grid's plane, for its spacing: the eye's height in
        // perspective; under orthographic, the pivot's distance (what the view spans), since an
        // orthographic eye may stand level with the plane and still see across it.
        [[nodiscard]] f32 GridViewHeight(const EditorCamera& camera) const
        {
            if (camera.orthographic)
                return camera.focusDistance;
            return Abs(Dot(camera.position, AxesOf(settings.gridPlane).normal));
        }

        void DrawGrid(foundation::render::debug::DebugDraw& dd, const EditorCamera& camera) const
        {
            const GridPlaneAxes axes = AxesOf(settings.gridPlane);
            const f32 height = GridViewHeight(camera);
            const GridSpacing step = GridSpacingFor(height);
            // The camera's foot on the plane: the grid is centred under it, wherever it flies.
            const Float3 foot = camera.position - axes.normal * Dot(camera.position, axes.normal);
            if (!settings.gridLines)
            {
                foundation::render::debug::GridPlaneDesc grid;
                grid.origin = foot;
                grid.fadeDistance = GridFadeDistance(height);
                grid.extent = grid.fadeDistance;
                grid.axisU = axes.u;
                grid.axisV = axes.v;
                grid.spacing = step.spacing;
                grid.blend = step.blend;
                grid.cameraPosition = camera.position;
                dd.DrawGridPlane(grid);
                return;
            }
            // The line grid, to compare: the same spacing, every tenth line brighter, a square 100
            // cells each way snapped to the coarse spacing under the camera (so it does not swim), no
            // fade and no blend between decades.
            constexpr i32 kHalfCells = 100;
            const f32 coarse = step.spacing * 10.0f;
            const f32 cu = Floor(Dot(foot, axes.u) / coarse) * coarse;
            const f32 cv = Floor(Dot(foot, axes.v) / coarse) * coarse;
            const f32 half = step.spacing * static_cast<f32>(kHalfCells);
            // sRGB, like every colour: a light grey that stays clear of the backdrop.
            const Color minor{0.63f, 0.63f, 0.65f, 0.35f};
            const Color major{0.63f, 0.63f, 0.65f, 1.0f};
            for (i32 i = -kHalfCells; i <= kHalfCells; ++i)
            {
                const f32 t = static_cast<f32>(i) * step.spacing;
                const Color color = (i % 10 == 0) ? major : minor;
                const Float3 alongV = axes.u * (cu + t);
                const Float3 alongU = axes.v * (cv + t);
                dd.DrawLine(alongV + axes.v * (cv - half), alongV + axes.v * (cv + half), color);
                dd.DrawLine(alongU + axes.u * (cu - half), alongU + axes.u * (cu + half), color);
            }
        }

        // Position markers for every entity (small cross; selected = brighter + boxed): empty
        // entities have no renderable, so the editor gives them a visual anchor. The selection box
        // is what the scene's systems measure the entity as (meshes, colliders), the box F frames;
        // a small cube at its origin when nothing does.
        // The entity the animation panel poses: a sphere and "preview" over it, drawn over the
        // scene so it reads through what stands in front.
        void DrawPreviewMarker(foundation::render::debug::DebugDraw& dd, const SceneOverlayFrame& frame) const
        {
            if (frame.previewed.IsNil())
                return;
            const foundation::scene::EntityHandle entity = frame.scene->FindEntity(frame.previewed);
            if (!entity.IsAssigned())
                return;
            const Float4x4 world = frame.scene->GetWorldMatrix(entity);
            const Float3 at{world.m[3][0], world.m[3][1], world.m[3][2]};
            const Color marker{1.0f, 0.85f, 0.2f, 1.0f};
            dd.DrawWireSphereOverlay(at, 0.35f, marker);
            dd.DrawText3D(at, u8"preview", marker);
        }
        void DrawEntityMarkers(foundation::render::debug::DebugDraw& dd, const SceneOverlayFrame& frame) const
        {
            if (frame.selection == nullptr)
                return;
            Selection<Guid>& selection = *frame.selection;
            foundation::scene::Scene& scene = *frame.scene;
            scene.ForEachEntity(
                [&](foundation::scene::EntityHandle e)
                {
                    const Float4x4 world = scene.GetWorldMatrix(e);
                    const Float3 p{world.m[3][0], world.m[3][1], world.m[3][2]};
                    const bool selected = selection.Contains(scene.GetEntityId(e));
                    if (!selected && !settings.showMarkers)
                        return; // the "Markers" toggle: unselected origins draw nothing
                    const f32 s = 0.25f;
                    const Color color = selected ? Color{1.0f, 0.85f, 0.25f, 1.0f} : Color{0.75f, 0.75f, 0.80f, 1.0f};
                    dd.DrawLine(p - Float3{s, 0, 0}, p + Float3{s, 0, 0}, color);
                    dd.DrawLine(p - Float3{0, s, 0}, p + Float3{0, s, 0}, color);
                    dd.DrawLine(p - Float3{0, 0, s}, p + Float3{0, 0, s}, color);
                    if (!selected)
                        return;
                    AABB bounds;
                    if (foundation::scene::EntityWorldBounds(scene, e, bounds))
                    {
                        dd.DrawWireBoxCenter(bounds.Center(), bounds.Extents(), color);
                        return;
                    }
                    dd.DrawWireBoxCenter(p, Float3{0.35f, 0.35f, 0.35f}, color);
                });
        }

        void DrawGizmos(foundation::render::debug::DebugDraw& dd, const SceneOverlayFrame& frame) const
        {
            if (frame.tools != nullptr)
            {
                frame.tools->Draw(dd);
            }
            // Component gizmos: full set for the selected entities; opted-in renderers for the rest.
            if (frame.componentGizmos == nullptr || frame.selection == nullptr)
                return;
            const EditorCamera& camera = *frame.camera;
            GizmoContext ctx;
            ctx.debug = &dd;
            ctx.scene = frame.scene;
            ctx.cameraPosition = camera.position;
            // The viewport's camera for the LOD overlay's per-view pick: the same matrices the
            // render path builds (aspect from the render target when it has a size).
            foundation::render::ViewCamera gizmoCamera;
            gizmoCamera.view = Float4x4::LookAtRH(camera.DrawEye(), camera.DrawEye() + camera.Forward(), camera.Up());
            const f32 aspect = frame.height > 0 ? static_cast<f32>(frame.width) / static_cast<f32>(frame.height) : 16.0f / 9.0f;
            gizmoCamera.projection = camera.Projection(aspect);
            gizmoCamera.position = camera.position;
            gizmoCamera.farZ = camera.DrawFar();
            gizmoCamera.pullBack = camera.DrawPullBack();
            ctx.viewCamera = &gizmoCamera;
            ctx.lodOverlay = settings.showLodOverlay;
            ctx.showColliders = settings.showColliders;
            Selection<Guid>& selection = *frame.selection;
            foundation::scene::Scene& scene = *frame.scene;
            scene.ForEachEntity([&](foundation::scene::EntityHandle e)
                                { frame.componentGizmos->DrawEntity(e, selection.Contains(scene.GetEntityId(e)), ctx); });
        }
    };
}
