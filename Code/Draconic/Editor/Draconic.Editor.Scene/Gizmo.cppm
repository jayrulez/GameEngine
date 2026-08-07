// Draconic::EditorScene - :gizmo partition.
//
// TransformGizmo + GizmoController: viewport transform manipulation (design doc §8). The
// Sedulous gizmo skeleton (debug-draw ribbons/rings, plane-projected drag math, atan2 rotation
// on a drag-start-captured basis) improved with the PlayCanvas interaction model:
//   - translate adds plane quads (flipped into the camera-facing quadrant) + a center free-move
//     handle; rotate adds a screen-space ring; scale adds a center uniform handle;
//   - picking has priorities (center > planes > axes) and inflated grab zones; handles disable
//     and fade at grazing angles (axis pointing at the camera / plane edge-on);
//   - rotate rings draw + pick only their camera-facing half (full ring while dragging), with an
//     angle guide (start reference line + current line + degree readout);
//   - snapping (held Ctrl) quantizes the drag DELTA (relative to the drag start, not a world
//     grid): translate 1.0, rotate 15 deg, scale 0.25;
//   - screen-constant size: tan(fovY/2) * viewDepth * 0.3 (view depth, not radial distance);
//   - scale always operates in Local space (world non-uniform scale on a rotated entity = skew).
//
// Parent-aware application (Sedulous ignored parents): world-space deltas convert into the
// entity's parent space - translate through the inverse parent world matrix, rotate conjugated
// (local' = inv(parentQ) * worldDelta * parentQ * localStart).
//
// Commands: a drag session brackets BeginGroup("gizmo_drag") -> per-frame SetLocalTransform
// (merges inside the group) -> EndGroup + LockGroup, so a whole drag is exactly ONE undo entry
// restoring the exact start transform, and can never merge with adjacent gestures.
//
// TransformGizmo is pure math + debug-draw output; GizmoController is driven by a plain
// GizmoFrameInput struct - both fully scriptable in headless tests.

module;
#include "Draconic.Foundation/Prelude.h"
#include <initializer_list>

export module draconic.editor.scene:gizmo;

import draconic.foundation;
import draconic.scene;
import draconic.render;
import draconic.editor.core;
import :edit;

using namespace draconic::foundation;

export namespace draconic::editor
{
    namespace scene = draconic::scene;
    namespace render = draconic::render;

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

    /// A handle on the gizmo. PlaneX = the YZ plane quad (normal X), etc. View = the center
    /// handle (free move / uniform scale) or the screen-space rotation ring.
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
        // Pose (world) + screen-constant size, set by the controller each frame.
        Float3 position{};
        Quaternion orientation = Quaternion::Identity;
        f32 size = 1.0f;

        // Snap increments (PlayCanvas-style relative-delta quantization).
        f32 translateSnap = 1.0f;
        f32 rotateSnapDegrees = 15.0f;
        f32 scaleSnap = 0.25f;

        [[nodiscard]] GizmoAxis Hovered() const noexcept { return m_hovered; }
        [[nodiscard]] GizmoAxis Selected() const noexcept { return m_selected; }
        [[nodiscard]] bool IsDragging() const noexcept { return m_dragging; }

        /// Camera facts for grazing tests, quadrant flips, half-ring culling and sizing.
        void SetCamera(Float3 cameraPosition, Float3 cameraForward);

        /// PlayCanvas screen-constant scale: tan(fovY/2) * view depth * ratio. Returns <= 0
        /// when the point is at/behind the camera plane (caller should hide the gizmo).
        [[nodiscard]] static f32 ScreenScale(Float3 cameraPos, Float3 cameraForward, f32 fovY,
                                             Float3 point, f32 ratio = 0.3f);

        // === Handle enablement (grazing angles; PlayCanvas GLANCE_EPSILON) ===

        [[nodiscard]] Float3 AxisDirection(GizmoAxis axis) const;

        /// Whether a handle is usable from the current view. Axis handles die when the axis
        /// points at the camera; plane quads die when seen edge-on.
        [[nodiscard]] bool IsAxisEnabled(GizmoAxis axis, GizmoMode mode) const;

        // === Hover ===

        /// Priority pick (PlayCanvas): center (2) beats planes / screen ring (1) beats axis
        /// handles (0); distance only breaks ties inside a priority class.
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

        /// Angle delta around the drag-start axis, unwrapped across the atan2 seam so sweeping
        /// past +/-pi stays continuous. `snap` quantizes to rotateSnapDegrees.
        [[nodiscard]] RotateDelta UpdateRotateDrag(const GizmoRay& ray, bool snap = false);

        /// Per-local-axis scale delta; the center handle scales uniformly by the camera-plane
        /// drag projected onto the screen diagonal (PlayCanvas). `snap` quantizes to scaleSnap.
        [[nodiscard]] Float3 UpdateScaleDrag(const GizmoRay& ray, bool snap = false);

        void EndDrag();

        void ClearHover();

        // === Drawing (debug-draw overlay; the gizmo stays visible through geometry) ===

        void Draw(render::debug::DebugDraw& dd, GizmoMode mode);

        // === Math utilities (public: unit-tested) ===

        /// Closest distance between a ray and an axis SEGMENT [origin, origin + dir * length].
        [[nodiscard]] static f32 RayAxisDistance(const GizmoRay& ray, Float3 axisOrigin,
                                                 Float3 axisDir, f32 axisLength);

        /// Distance from a ray to a ring circumference; optionally outputs the plane hit point.
        /// kFloatMax when the ring plane is behind the ray.
        [[nodiscard]] static f32 RayRingDistance(const GizmoRay& ray, Float3 center, Float3 normal,
                                                 f32 radius, Float3* outHit);

        [[nodiscard]] static f32 RayPointDistance(const GizmoRay& ray, Float3 point);

    private:
        [[nodiscard]] Float3 ViewDir() const { return Normalized(position - m_cameraPos); }
        [[nodiscard]] Float3 CameraRight() const;

        [[nodiscard]] static f32 Snap(f32 value, f32 increment);

        /// In-plane basis of a plane handle (the two axes spanning it).
        void PlaneBasis(GizmoAxis plane, Float3& u, Float3& v) const;

        /// Camera-facing quadrant signs for a plane quad (PlayCanvas flipPlanes).
        void PlaneQuadSigns(GizmoAxis plane, f32& su, f32& sv) const;

        /// Ray hit inside the plane handle's quad ([0.25, 0.55] * size on both in-plane axes,
        /// in the camera-facing quadrant). Optionally outputs the plane hit point.
        [[nodiscard]] bool PlaneQuadHit(const GizmoRay& ray, GizmoAxis plane, Float3* outHit) const;

        /// Hit point on the drag plane for the selected handle. Axis handles use the plane that
        /// contains the axis and is most perpendicular to the view (Sedulous); plane handles use
        /// their own plane; the center handle uses the camera plane.
        [[nodiscard]] Float3 DragHitPoint(const GizmoRay& ray, GizmoAxis axis,
                                          Float3 planeOrigin) const;

        /// Polar angle of the ray hit on the captured rotation plane. Behind-camera hits retry
        /// the reversed ray (PlayCanvas guard) so grazing rings stay continuous.
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

    /// One frame of viewport input for the controller - a plain struct so tests can script
    /// whole drag sessions headlessly. The page masks buttons while the camera owns the mouse
    /// (Alt orbit / RMB fly).
    struct GizmoFrameInput
    {
        GizmoRay ray{};
        Float3 cameraPosition{};
        Float3 cameraForward{0, 0, -1};
        f32 fovY = 1.0472f;
        bool leftPressed = false;
        bool leftDown = false;
        bool leftReleased = false;
        bool snap = false;           // held Ctrl
        bool keyTranslate = false;   // W (edge)
        bool keyRotate = false;      // E (edge)
        bool keyScale = false;       // R (edge)
        bool keyToggleSpace = false; // X (edge)

        /// False when the pointer isn't over the viewport this frame: the controller still
        /// syncs the gizmo pose/size to the selection (so it tracks tree selections and
        /// undo/redo while the mouse is elsewhere) but clears hover, ignores buttons/keys,
        /// and finishes any in-flight drag.
        bool pointerValid = true;
    };

    /// Owns the gizmo + the drag session against a SceneEditContext. Update() returns true when
    /// the gizmo consumed the mouse (hot handle or active drag) so the page skips click-picking.
    class GizmoController
    {
    public:
        explicit GizmoController(SceneEditContext& edit) : m_edit(&edit) {}

        [[nodiscard]] GizmoMode Mode() const noexcept { return m_mode; }
        void SetMode(GizmoMode mode) noexcept;
        [[nodiscard]] GizmoSpace Space() const noexcept { return m_space; }
        void SetSpace(GizmoSpace space) noexcept;
        [[nodiscard]] TransformGizmo& Gizmo() noexcept { return m_gizmo; }
        [[nodiscard]] bool IsActive() const noexcept { return m_active; }

        bool Update(const GizmoFrameInput& in);

        /// Draw the gizmo + a mode/space/snap readout into the page's debug-draw list.
        void Draw(render::debug::DebugDraw& dd);

        [[nodiscard]] StringView StatusText() const;

    private:
        void UpdateDrag(const GizmoFrameInput& in);

        void FinishDrag();

        void AbortDrag();

        SceneEditContext* m_edit; // borrowed (the page owns it)
        TransformGizmo m_gizmo;
        GizmoMode m_mode = GizmoMode::Translate;
        GizmoSpace m_space = GizmoSpace::World;
        bool m_active = false; // false while nothing is selected / behind the camera
        bool m_inGroup = false;

        // Drag session capture.
        Guid m_dragEntity;
        foundation::Transform m_dragStartLocal;
        Float4x4 m_parentInverseWorld = Float4x4::Identity();
        Quaternion m_parentRotation = Quaternion::Identity;
    };
}
