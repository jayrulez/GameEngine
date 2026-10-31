// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :gizmo partition.
//
// TransformGizmo and GizmoController: viewport transform manipulation. The gizmo draws with
// debug-draw ribbons and rings, projects drags onto planes, and measures rotation with atan2 on a
// basis captured at drag start. Its interaction model follows PlayCanvas's transform gizmo:
//   - translate adds plane quads (flipped into the camera-facing quadrant) and a center free-move
//     handle; rotate adds a screen-space ring; scale adds a center uniform handle;
//   - picking has priorities (center, then planes, then axes) and inflated grab zones; handles
//     disable and fade at grazing angles (an axis pointing at the camera, a plane seen edge-on);
//   - rotate rings draw and pick only their camera-facing half (the full ring while dragging),
//     with an angle guide (a start reference line, the current line and a degree readout);
//   - snapping (Ctrl held) quantizes the drag delta relative to the drag start, not to a world
//     grid: translate 1.0, rotate 15 degrees, scale 0.25;
//   - the screen-constant size is tan(fovY/2) * viewDepth * 0.3 (view depth, not radial
//     distance);
//   - scale always works in Local space (a world non-uniform scale on a rotated entity is a skew).
//
// World-space deltas convert into the entity's parent space: translation through the inverse
// parent world matrix, rotation by conjugation (local' = inv(parentQ) * worldDelta * parentQ *
// localStart).
//
// A drag session brackets BeginGroup("gizmo_drag"), then a SetLocalTransform per frame (merged
// inside the group), then EndGroup and LockGroup, so a whole drag is one undo entry that restores
// the exact start transform and never merges with adjacent gestures.
//
// TransformGizmo is pure math plus debug-draw output; GizmoController is driven by a plain
// GizmoFrameInput struct, so both can be scripted in headless tests.

module;
#include "Core/Prelude.h"
#include <initializer_list>

export module editor.scene:gizmo;

import foundation.core;
import foundation.scene;
import foundation.render;
import editor.core;
import :edit;
import foundation.materials;

using namespace foundation::core;
namespace core = foundation::core;

export namespace editor
{
    namespace scene = foundation::scene;
    namespace render = foundation::render;

    enum class GizmoMode : u8
    {
        Translate,
        Rotate,
        Scale
    };
    enum class GizmoSpace : u8
    {
        World,
        Local
    };

    /// A handle on the gizmo. PlaneX is the YZ plane quad (normal X), and so on. View is the center
    /// handle (free move or uniform scale) or the screen-space rotation ring.
    enum class GizmoAxis : u8
    {
        None,
        X,
        Y,
        Z,
        PlaneX,
        PlaneY,
        PlaneZ,
        View
    };

    struct GizmoRay
    {
        Float3 origin{};
        Float3 direction{0.0f, 0.0f, -1.0f};
    };

    class TransformGizmo
    {
    public:
        // Pose (world) and screen-constant size, set by the controller each frame.
        Float3 position{};
        Quaternion orientation = Quaternion::Identity;
        f32 size = 1.0f;

        // Snap increments, applied to the drag's delta relative to its start.
        f32 translateSnap = 1.0f;
        f32 rotateSnapDegrees = 15.0f;
        f32 scaleSnap = 0.25f;

        [[nodiscard]] GizmoAxis Hovered() const noexcept { return m_hovered; }
        [[nodiscard]] GizmoAxis Selected() const noexcept { return m_selected; }
        [[nodiscard]] bool IsDragging() const noexcept { return m_dragging; }

        /// Camera facts for grazing tests, quadrant flips, half-ring culling and sizing. Under an
        /// orthographic view every view ray runs along `cameraForward`.
        void SetCamera(Float3 cameraPosition, Float3 cameraForward, bool orthographic = false);

        /// Screen-constant scale: tan(fovY/2) * view depth * ratio. Under an orthographic view
        /// `orthoHeight` tall (> 0), it is half that height * ratio, the same at every depth.
        /// Returns <= 0 when the point is at or behind the camera plane (the caller should hide the
        /// gizmo).
        [[nodiscard]] static f32 ScreenScale(Float3 cameraPos, Float3 cameraForward, f32 fovY,
                                             Float3 point, f32 ratio = 0.3f, f32 orthoHeight = 0.0f);

        // === Handle enablement (grazing angles) ===

        [[nodiscard]] Float3 AxisDirection(GizmoAxis axis) const;

        /// Whether a handle is usable from the current view. Axis handles are disabled when the
        /// axis points at the camera, plane quads when seen edge-on.
        [[nodiscard]] bool IsAxisEnabled(GizmoAxis axis, GizmoMode mode) const;

        // === Hover ===

        /// Priority pick: the center (2) beats planes and the screen ring (1), which beat axis
        /// handles (0); distance only breaks ties within a priority class.
        GizmoAxis UpdateHover(const GizmoRay& ray, GizmoMode mode);

        // === Drag ===

        [[nodiscard]] bool BeginDrag(const GizmoRay& ray, GizmoMode mode);

        /// World-space position delta. Axis handles project on the axis; plane handles move
        /// freely in the plane; the center handle moves on the camera plane. `snap` quantizes
        /// the delta (per axis scalar) to translateSnap.
        [[nodiscard]] Float3 UpdateTranslateDrag(const GizmoRay& ray, bool snap = false);

        struct RotateDelta
        {
            Float3 axis{};
            f32 angle = 0.0f;
        };

        /// Angle delta around the drag-start axis, unwrapped across atan2's wrap-around so sweeping
        /// past +/-pi stays continuous. `snap` quantizes to rotateSnapDegrees.
        [[nodiscard]] RotateDelta UpdateRotateDrag(const GizmoRay& ray, bool snap = false);

        /// Per-local-axis scale delta; the center handle scales uniformly by the camera-plane drag
        /// projected onto the screen diagonal. `snap` quantizes to scaleSnap.
        [[nodiscard]] Float3 UpdateScaleDrag(const GizmoRay& ray, bool snap = false);

        void EndDrag();

        void ClearHover();

        // === Drawing (debug-draw overlay; the gizmo stays visible through geometry) ===

        void Draw(render::debug::DebugDraw& dd, GizmoMode mode);

        // === Math utilities (public: unit-tested) ===

        /// Closest distance between a ray and an axis segment [origin, origin + dir * length].
        [[nodiscard]] static f32 RayAxisDistance(const GizmoRay& ray, Float3 axisOrigin,
                                                 Float3 axisDir, f32 axisLength);

        /// Distance from a ray to a ring circumference; optionally outputs the plane hit point.
        /// kFloatMax when the ring plane is behind the ray.
        [[nodiscard]] static f32 RayRingDistance(const GizmoRay& ray, Float3 center, Float3 normal,
                                                 f32 radius, Float3* outHit);

        [[nodiscard]] static f32 RayPointDistance(const GizmoRay& ray, Float3 point);

    private:
        [[nodiscard]] Float3 ViewDir() const { return ViewDirAt(position); }
        // The view ray's direction at `point`: from the eye, or along the view when orthographic.
        [[nodiscard]] Float3 ViewDirAt(Float3 point) const
        {
            return m_orthographic ? m_cameraForward : Normalized(point - m_cameraPos);
        }
        [[nodiscard]] Float3 CameraRight() const;

        [[nodiscard]] static f32 Snap(f32 value, f32 increment);

        /// In-plane basis of a plane handle (the two axes spanning it).
        void PlaneBasis(GizmoAxis plane, Float3& u, Float3& v) const;

        /// Camera-facing quadrant signs for a plane quad.
        void PlaneQuadSigns(GizmoAxis plane, f32& su, f32& sv) const;

        /// Ray hit inside the plane handle's quad ([0.25, 0.55] * size on both in-plane axes,
        /// in the camera-facing quadrant). Optionally outputs the plane hit point.
        [[nodiscard]] bool PlaneQuadHit(const GizmoRay& ray, GizmoAxis plane, Float3* outHit) const;

        /// Hit point on the drag plane for the selected handle. Axis handles use the plane that
        /// contains the axis and is most perpendicular to the view; plane handles use their own
        /// plane; the center handle uses the camera plane.
        [[nodiscard]] Float3 DragHitPoint(const GizmoRay& ray, GizmoAxis axis,
                                          Float3 planeOrigin) const;

        /// Polar angle of the ray hit on the captured rotation plane. A hit behind the camera
        /// retries with the reversed ray, so grazing rings stay continuous.
        [[nodiscard]] static f32 ComputeRotateAngle(const GizmoRay& ray, Float3 center,
                                                    Float3 normal, Float3 u, Float3 v);

        // === Draw helpers ===

        [[nodiscard]] Color AxisColor(GizmoAxis axis, GizmoMode mode) const;

        /// A line as a screen-facing overlay quad (ribbon) so it has constant apparent width.
        void DrawThickLine(render::debug::DebugDraw& dd, Float3 from, Float3 to, Color color,
                           f32 thickness) const;

        void DrawAxisArrow(render::debug::DebugDraw& dd, GizmoAxis axis, GizmoMode mode) const;

        void DrawSpanLine(render::debug::DebugDraw& dd) const;

        void DrawTranslate(render::debug::DebugDraw& dd);

        void DrawScale(render::debug::DebugDraw& dd);

        void DrawRotate(render::debug::DebugDraw& dd);

        /// Ring from thick segments; cullBackHalf skips segments on the far side of the camera.
        void DrawRing(render::debug::DebugDraw& dd, Float3 u, Float3 v, f32 radius, Color color,
                      f32 thickness, i32 segments, bool cullBackHalf) const;

        void DrawAngleGuide(render::debug::DebugDraw& dd, f32 radius);

        // Camera facts for the frame.
        Float3 m_cameraPos{};
        Float3 m_cameraForward{0, 0, -1};
        bool m_orthographic = false;

        GizmoAxis m_hovered = GizmoAxis::None;
        GizmoAxis m_selected = GizmoAxis::None;
        bool m_dragging = false;

        // Drag capture.
        Float3 m_dragStartPosition{};
        Float3 m_dragStartHitPoint{};
        f32 m_dragStartAngle = 0.0f;
        f32 m_currentAngleDelta = 0.0f;
        Float3 m_dragRotationAxis{};
        Float3 m_dragRotationU{};
        Float3 m_dragRotationV{};
    };

    /// One frame of viewport input for the controller: a plain struct, so tests can script whole
    /// drag sessions headlessly. The page masks buttons while the camera owns the mouse (Alt orbit,
    /// RMB fly).
    struct GizmoFrameInput
    {
        GizmoRay ray{};
        Float3 cameraPosition{};
        Float3 cameraForward{0, 0, -1};
        f32 fovY = 1.0472f;
        f32 orthoHeight = 0.0f; // > 0: an orthographic view this tall, in world units
        bool leftPressed = false;
        bool leftDown = false;
        bool leftReleased = false;
        bool snap = false;           // held Ctrl
        /// Vertex snap (V held, or the controller's VertexSnapMode on): a translate drag puts the
        /// pivot on the vertex nearest where the cursor meets the scene's drawn surfaces, whichever
        /// handle it holds. This places the pivot absolutely, where Ctrl's snap steps the drag's
        /// distance.
        bool vertexSnap = false;
        bool keySelect = false;      // Q (edge): no handles, a click only picks
        bool keyTranslate = false;   // W (edge)
        bool keyRotate = false;      // E (edge)
        bool keyScale = false;       // R (edge)
        bool keyToggleSpace = false; // X (edge)

        /// False when the pointer is not over the viewport this frame. The controller still syncs
        /// the gizmo's pose and size to the selection (so it tracks tree selections and undo or
        /// redo while the mouse is elsewhere) but clears hover, ignores buttons and keys, and
        /// finishes any drag in flight.
        bool pointerValid = true;
    };

    /// Owns the gizmo and the drag session against a SceneEditContext. Update() returns true when
    /// the gizmo consumed the mouse (a hot handle or an active drag), so the page skips
    /// click-picking.
    class GizmoController
    {
    public:
        explicit GizmoController(SceneEditContext& edit) : m_edit(&edit) {}

        [[nodiscard]] GizmoMode Mode() const noexcept { return m_mode; }
        /// Sets the mode and shows its handles (a mode is chosen to transform with it).
        void SetMode(GizmoMode mode) noexcept;
        /// Whether the handles show (off: Select, where a click only picks; the mode stays for
        /// when they show again).
        [[nodiscard]] bool HandlesShown() const noexcept { return m_handlesShown; }
        void SetHandlesShown(bool shown) noexcept;
        /// While on (the bar's Snap), every drag snaps to the increments, as holding Ctrl does for
        /// one.
        [[nodiscard]] bool SnapMode() const noexcept { return m_snapMode; }
        void SetSnapMode(bool on) noexcept { m_snapMode = on; }
        /// A move snaps by the grid's cell where the camera is (the page sets the gizmo's
        /// translateSnap to it each frame) rather than by its own increment.
        [[nodiscard]] bool MoveSnapsToGrid() const noexcept { return m_moveSnapsToGrid; }
        void SetMoveSnapsToGrid(bool on) noexcept { m_moveSnapsToGrid = on; }
        [[nodiscard]] GizmoSpace Space() const noexcept { return m_space; }
        void SetSpace(GizmoSpace space) noexcept;
        /// While on (the toolbar's toggle), every translate drag snaps to vertices, as holding V
        /// does for one.
        [[nodiscard]] bool VertexSnapMode() const noexcept { return m_vertexSnapMode; }
        void SetVertexSnapMode(bool on) noexcept { m_vertexSnapMode = on; }
        [[nodiscard]] TransformGizmo& Gizmo() noexcept { return m_gizmo; }
        [[nodiscard]] bool IsActive() const noexcept { return m_active; }

        bool Update(const GizmoFrameInput& in);

        /// Draw the gizmo and a mode, space and snap readout into the page's debug-draw list.
        void Draw(render::debug::DebugDraw& dd);

        [[nodiscard]] StringView StatusText() const;

        /// The vertex a vertex-snapping drag holds the pivot on this frame (world); none when
        /// not snapping, or when the cursor is over no surface.
        [[nodiscard]] const Optional<Float3>& SnapTarget() const noexcept { return m_snapTarget; }

    private:
        void UpdateDrag(const GizmoFrameInput& in);
        /// The vertex under `ray` to snap the dragged entity onto: on any drawn surface except
        /// those of the dragged entity and its descendants.
        [[nodiscard]] Optional<Float3> FindSnapVertex(const GizmoRay& ray, scene::EntityHandle dragged);

        void FinishDrag();

        void AbortDrag();

        SceneEditContext* m_edit; // borrowed (the page owns it)
        TransformGizmo m_gizmo;
        GizmoMode m_mode = GizmoMode::Translate;
        GizmoSpace m_space = GizmoSpace::World;
        bool m_active = false; // false while nothing is selected or the gizmo is behind the camera
        bool m_inGroup = false;

        // Drag session capture.
        Guid m_dragEntity;
        core::Transform m_dragStartLocal;
        Float4x4 m_parentInverseWorld = Float4x4::Identity();
        Quaternion m_parentRotation = Quaternion::Identity;
        bool m_vertexSnapMode = false;
        bool m_handlesShown = true;
        bool m_snapMode = false;
        bool m_moveSnapsToGrid = false;
        Optional<Float3> m_snapTarget; // the vertex the pivot is snapped to (vertex snap)
        static constexpr f32 kSnapReach = 10000.0f; // how far along the ray a snap target may be
    };
}
