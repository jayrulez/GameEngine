// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :scene_overlays partition.
//
// What the scene page draws over its scene, into its viewport's own debug list (so never into a
// second view of the same scene): the grid on its plane and the origin's axes, a marker at each
// entity and a box round each selected one, the tools' and the components' gizmos with the active
// tool's status, the frame rate, the measurement while zooming and the orientation gizmo. Its
// toggles are SceneOverlaySettings, which the page keeps per scene. Out of the page's class as it
// was (Documentation/Specs/editor-gui.md, 5.3, layer 2); what it reads each frame is handed in.

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
import :zoom_readout;
import :view_gizmo;
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
        bool showFps = false;
    };

    /// One frame of what the overlays read.
    struct SceneOverlayFrame
    {
        foundation::scene::Scene* scene = nullptr;
        Selection<Guid>* selection = nullptr;
        const EditorCamera* camera = nullptr;
        u32 width = 0; // the viewport's render size
        u32 height = 0;
        bool pointerOver = false; // the pointer is over the viewport, at:
        f32 pointerX = 0.0f;
        f32 pointerY = 0.0f;
        ViewportToolManager* tools = nullptr;
        GizmoRendererRegistry* componentGizmos = nullptr;
        f32 deltaSeconds = 0.0f;
    };

    class SceneOverlays
    {
    public:
        SceneOverlaySettings settings;

        /// A wheel zoom happened: the measurement shows for a moment.
        void Zoomed() { m_zoomReadout.Zoomed(); }

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
            // The FPS readout: top-right, clear of the tool status text in the top-left. Sampled
            // over half-second windows of the page's update time.
            if (settings.showFps)
            {
                m_fpsWindowSeconds += static_cast<f64>(frame.deltaSeconds);
                m_fpsWindowFrames += 1;
                if (m_fpsWindowSeconds >= 0.5 || m_fpsText.IsEmpty())
                {
                    m_fpsText = FrameRateOverlayText(m_fpsWindowSeconds, m_fpsWindowFrames);
                    m_fpsWindowSeconds = 0.0;
                    m_fpsWindowFrames = 0;
                }
                dd.DrawScreenTextRight(12.0f, 12.0f, m_fpsText.AsView(), Color{0.85f, 0.85f, 0.85f, 1.0f});
            }
            else
            {
                m_fpsWindowSeconds = 0.0;
                m_fpsWindowFrames = 0;
                m_fpsText.Clear();
            }
            m_zoomReadout.Advance(frame.deltaSeconds);
            if (const f32 opacity = m_zoomReadout.Opacity(); opacity > 0.0f)
                DrawZoomReadout(dd, frame, opacity);
            DrawViewGizmo(dd, frame);
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
                if (IViewportTool* active = frame.tools->ActiveTool(); active != nullptr)
                {
                    const StringView status = active->StatusText();
                    if (!status.IsEmpty())
                        dd.DrawScreenText(12.0f, 12.0f, status, Color{0.85f, 0.85f, 0.85f, 1.0f});
                }
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

        // The measurement overlay while zooming, bottom-left: the focus distance, the grid cell
        // and a scale bar, at `opacity`.
        void DrawZoomReadout(foundation::render::debug::DebugDraw& dd, const SceneOverlayFrame& frame, f32 opacity) const
        {
            if (frame.height == 0)
                return;
            const EditorCamera& camera = *frame.camera;
            const f32 height = static_cast<f32>(frame.height);
            const ScaleBar bar = ScaleBarAt(camera.focusDistance, camera.fovY, height);
            const String focusText = Format(u8"{} to focus", FormatMetres(camera.focusDistance));
            // The cell of the grid as drawn; whether the grid shows is the page's toggle.
            const String cellText = Format(u8"grid cell {}", FormatMetres(CurrentGridSpacing(camera).spacing));
            const String barText = FormatMetres(bar.metres);

            // Bottom-left, clear of the tool status (top-left) and the FPS readout (top-right):
            // two lines of text over the bar, on a translucent backing.
            constexpr f32 kMargin = 12.0f;
            constexpr f32 kPad = 6.0f;
            constexpr f32 kLine = static_cast<f32>(foundation::render::debug::kCharHeight) + 6.0f;
            constexpr f32 kGlyph = static_cast<f32>(foundation::render::debug::kCharWidth);
            const f32 textWidth = kGlyph * static_cast<f32>(Max(focusText.Size(), cellText.Size()));
            const f32 barWidth = bar.pixels + 6.0f + kGlyph * static_cast<f32>(barText.Size());
            const f32 width = Max(textWidth, barWidth) + 2.0f * kPad;
            const f32 boxHeight = 2.0f * kLine + 10.0f + 2.0f * kPad;
            const f32 left = kMargin;
            const f32 top = height - kMargin - boxHeight;
            dd.DrawScreenRect(left, top, width, boxHeight, Color{0.0f, 0.0f, 0.0f, 0.45f * opacity});

            const Color ink{0.85f, 0.85f, 0.85f, opacity};
            const f32 x = left + kPad;
            dd.DrawScreenText(x, top + kPad, focusText.AsView(), ink);
            dd.DrawScreenText(x, top + kPad + kLine, cellText.AsView(), ink);
            // The bar: a line the length of its round measure, with end ticks, its length beside it.
            const f32 barY = top + kPad + 2.0f * kLine + 4.0f;
            dd.DrawScreenRect(x, barY, bar.pixels, 2.0f, ink);
            dd.DrawScreenRect(x, barY - 3.0f, 1.0f, 8.0f, ink);
            dd.DrawScreenRect(x + bar.pixels - 1.0f, barY - 3.0f, 1.0f, 8.0f, ink);
            dd.DrawScreenText(x + bar.pixels + 6.0f, barY - 3.0f, barText.AsView(), ink);
        }

        // The orientation gizmo: the axis tripod turning with the camera, the knob under the
        // pointer lit.
        void DrawViewGizmo(foundation::render::debug::DebugDraw& dd, const SceneOverlayFrame& frame) const
        {
            if (frame.width == 0)
                return;
            const EditorCamera& camera = *frame.camera;
            const ViewGizmoLayout gizmo = ViewGizmo::LayoutFor(camera.Right(), camera.Up(), camera.Forward(), static_cast<f32>(frame.width));
            const i32 hovered = frame.pointerOver ? ViewGizmo::Hit(gizmo, frame.pointerX, frame.pointerY) : -1;
            // Back to front, so a knob pointing at the viewer covers the arms and knobs behind it; the
            // positive axes have an arm from the centre, the negative ones a dimmer knob alone.
            constexpr f32 kGlyph = static_cast<f32>(foundation::render::debug::kCharWidth);
            constexpr f32 kGlyphHeight = static_cast<f32>(foundation::render::debug::kCharHeight);
            const f32 half = 0.5f * ViewGizmo::kKnob;
            for (i32 i = 0; i < 6; ++i)
            {
                const ViewGizmoPole& pole = gizmo.poles[i];
                const Color color = ViewGizmo::ColorOf(pole, i == hovered);
                if (pole.positive)
                    dd.DrawScreenLine(gizmo.center.x, gizmo.center.y, pole.screen.x, pole.screen.y, color, 2.0f);
                dd.DrawScreenRect(pole.screen.x - half, pole.screen.y - half, ViewGizmo::kKnob, ViewGizmo::kKnob, color);
                const StringView label = ViewGizmo::Label(pole);
                const Color ink = pole.positive ? Color{0.08f, 0.08f, 0.08f, 1.0f} : Color{0.9f, 0.9f, 0.9f, 1.0f};
                dd.DrawScreenText(pole.screen.x - 0.5f * kGlyph * static_cast<f32>(label.Size()), pole.screen.y - 0.5f * kGlyphHeight, label, ink);
            }
            // The centre: a click switches the projection; under the tripod, which one is on.
            const bool overCentre = frame.pointerOver && ViewGizmo::HitCentre(gizmo, frame.pointerX, frame.pointerY);
            const f32 c = 0.5f * ViewGizmo::kCentre;
            const Color centre = overCentre ? Color{1.0f, 1.0f, 1.0f, 1.0f} : Color{0.75f, 0.75f, 0.75f, 1.0f};
            dd.DrawScreenRect(gizmo.center.x - c, gizmo.center.y - c, ViewGizmo::kCentre, ViewGizmo::kCentre, centre);
            const StringView projection = ViewGizmo::ProjectionLabel(camera.orthographic);
            dd.DrawScreenText(gizmo.center.x - 0.5f * kGlyph * static_cast<f32>(projection.Size()), gizmo.center.y + ViewGizmo::kArm + ViewGizmo::kKnob,
                              projection, Color{0.85f, 0.85f, 0.85f, 1.0f});
        }

        ZoomReadout m_zoomReadout; // shown for a moment after each wheel zoom
        // The FPS overlay samples half-second windows so the readout is legible, not a blur.
        f64 m_fpsWindowSeconds = 0.0;
        u32 m_fpsWindowFrames = 0;
        String m_fpsText;
    };
}
