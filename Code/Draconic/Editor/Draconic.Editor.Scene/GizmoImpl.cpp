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

module draconic.editor.scene;

import draconic.foundation;
import draconic.scene;
import draconic.render;
import draconic.editor.core;
import :edit;

using namespace draconic::foundation;

namespace draconic::editor
{
    void GizmoController::SetMode(GizmoMode mode) noexcept
    {
        if (!m_gizmo.IsDragging())
        {
            m_mode = mode;
        }
    }

    void GizmoController::SetSpace(GizmoSpace space) noexcept
    {
        if (!m_gizmo.IsDragging())
        {
            m_space = space;
        }
    }

    bool GizmoController::Update(const GizmoFrameInput& in)
    {
        if (!m_gizmo.IsDragging() && in.pointerValid)
        {
            if (in.keyTranslate)
            {
                m_mode = GizmoMode::Translate;
            }
            if (in.keyRotate)
            {
                m_mode = GizmoMode::Rotate;
            }
            if (in.keyScale)
            {
                m_mode = GizmoMode::Scale;
            }
            if (in.keyToggleSpace)
            {
                m_space = (m_space == GizmoSpace::World) ? GizmoSpace::Local : GizmoSpace::World;
            }
        }

        const Guid* primary = m_edit->EntitySelection().Primary();
        const scene::EntityHandle entity =
            (primary != nullptr) ? m_edit->Resolve(*primary) : scene::EntityHandle{};
        if (!entity.IsAssigned())
        {
            AbortDrag();
            m_active = false;
            return false;
        }

        scene::Scene& scene = m_edit->Scene();
        m_gizmo.SetCamera(in.cameraPosition, in.cameraForward);

        // Pose: position always at the entity's world origin. Orientation = entity WORLD
        // rotation in Local space (and always for Scale - non-uniform world scale on a
        // rotated entity would be skew); identity in World space. Composed FRESH from the
        // local chain (GetWorldMatrix is the cached value from the last UpdateTransforms -
        // stale right after undo/redo and in headless tests).
        const Float4x4 world = scene.ComposeWorldMatrix(entity);
        m_gizmo.position = Float3{world.m[3][0], world.m[3][1], world.m[3][2]};
        if (!m_gizmo
                 .IsDragging()) // rotation frame is captured at BeginDrag; don't drift the handles
        {
            m_gizmo.orientation = Quaternion::Identity;
            if (m_space == GizmoSpace::Local || m_mode == GizmoMode::Scale)
            {
                Float3 t, s;
                Quaternion r;
                if (Decompose(world, t, r, s))
                {
                    m_gizmo.orientation = r;
                }
            }
        }

        const f32 scale = TransformGizmo::ScreenScale(in.cameraPosition, in.cameraForward, in.fovY,
                                                      m_gizmo.position);
        if (scale <= 0.0001f) // at/behind the camera plane
        {
            AbortDrag();
            m_active = false;
            return false;
        }
        m_gizmo.size = scale;
        m_active = true;

        // === Drag session ===
        if (m_gizmo.IsDragging())
        {
            if (!in.pointerValid)
            {
                FinishDrag();
                return true;
            } // pointer lost mid-drag
            if (in.leftDown)
            {
                UpdateDrag(in);
            }
            if (in.leftReleased || !in.leftDown)
            {
                FinishDrag();
            }
            return true;
        }

        // Pointer elsewhere: the pose above stays synced, but nothing is interactive.
        if (!in.pointerValid)
        {
            m_gizmo.ClearHover();
            return false;
        }

        m_gizmo.UpdateHover(in.ray, m_mode);
        if (in.leftPressed && m_gizmo.Hovered() != GizmoAxis::None &&
            m_gizmo.BeginDrag(in.ray, m_mode))
        {
            m_dragEntity = *primary;
            m_dragStartLocal = scene.GetLocalTransform(entity);

            // Parent frame captured once: world deltas convert into parent space.
            const scene::EntityHandle parent = scene.GetParent(entity);
            m_parentInverseWorld = Float4x4::Identity();
            m_parentRotation = Quaternion::Identity;
            if (parent.IsAssigned())
            {
                const Float4x4 parentWorld = scene.ComposeWorldMatrix(parent);
                m_parentInverseWorld = Inverse(parentWorld);
                Float3 t, s;
                Quaternion r;
                if (Decompose(parentWorld, t, r, s))
                {
                    m_parentRotation = r;
                }
            }

            m_edit->Commands().BeginGroup(u8"gizmo_drag");
            m_inGroup = true;
            return true;
        }
        return m_gizmo.Hovered() != GizmoAxis::None;
    }

    void GizmoController::Draw(render::debug::DebugDraw& dd)
    {
        if (!m_active)
        {
            return;
        }
        m_gizmo.Draw(dd, m_mode);
    }

    StringView GizmoController::StatusText() const
    {
        switch (m_mode)
        {
        case GizmoMode::Translate:
            return (m_space == GizmoSpace::World)
                       ? StringView(u8"Move [World]  (W/E/R mode, X space, Ctrl snap)")
                       : StringView(u8"Move [Local]  (W/E/R mode, X space, Ctrl snap)");
        case GizmoMode::Rotate:
            return (m_space == GizmoSpace::World)
                       ? StringView(u8"Rotate [World]  (W/E/R mode, X space, Ctrl snap)")
                       : StringView(u8"Rotate [Local]  (W/E/R mode, X space, Ctrl snap)");
        default:
            return StringView(u8"Scale [Local]  (W/E/R mode, X space, Ctrl snap)");
        }
    }

    void GizmoController::UpdateDrag(const GizmoFrameInput& in)
    {
        const scene::EntityHandle entity = m_edit->Resolve(m_dragEntity);
        if (!entity.IsAssigned())
        {
            AbortDrag();
            return;
        }

        foundation::Transform t = m_dragStartLocal;
        switch (m_mode)
        {
        case GizmoMode::Translate:
        {
            const Float3 worldDelta = m_gizmo.UpdateTranslateDrag(in.ray, in.snap);
            t.position =
                m_dragStartLocal.position + TransformDirection(worldDelta, m_parentInverseWorld);
            break;
        }
        case GizmoMode::Rotate:
        {
            const TransformGizmo::RotateDelta d = m_gizmo.UpdateRotateDrag(in.ray, in.snap);
            const Quaternion worldDelta = Quaternion::FromAxisAngle(d.axis, d.angle);
            // Conjugate the world delta into parent space, then compose with the start.
            t.rotation = Normalized(Inverse(m_parentRotation) * worldDelta * m_parentRotation *
                                    m_dragStartLocal.rotation);
            break;
        }
        case GizmoMode::Scale:
        {
            const Float3 d = m_gizmo.UpdateScaleDrag(in.ray, in.snap);
            t.scale.x = Max(m_dragStartLocal.scale.x + d.x, 0.001f);
            t.scale.y = Max(m_dragStartLocal.scale.y + d.y, 0.001f);
            t.scale.z = Max(m_dragStartLocal.scale.z + d.z, 0.001f);
            break;
        }
        }
        m_edit->SetLocalTransform(m_dragEntity, t);
    }

    void GizmoController::FinishDrag()
    {
        m_gizmo.EndDrag();
        if (m_inGroup)
        {
            m_edit->Commands().EndGroup();
            m_edit->Commands().LockGroup(); // the next drag is its own undo entry
            m_inGroup = false;
        }
        m_dragEntity = Guid{};
    }

    void GizmoController::AbortDrag()
    {
        if (m_gizmo.IsDragging())
        {
            FinishDrag();
        }
    }
    void TransformGizmo::SetCamera(Float3 cameraPosition, Float3 cameraForward)
    {
        m_cameraPos = cameraPosition;
        m_cameraForward = Normalized(cameraForward);
    }

    f32 TransformGizmo::ScreenScale(Float3 cameraPos, Float3 cameraForward, f32 fovY, Float3 point,
                                    f32 ratio)
    {
        const f32 depth = Dot(point - cameraPos, Normalized(cameraForward));
        return Tan(fovY * 0.5f) * depth * ratio;
    }

    Float3 TransformGizmo::AxisDirection(GizmoAxis axis) const
    {
        switch (axis)
        {
        case GizmoAxis::X:
        case GizmoAxis::PlaneX:
            return RotateVector(orientation, Float3{1, 0, 0});
        case GizmoAxis::Y:
        case GizmoAxis::PlaneY:
            return RotateVector(orientation, Float3{0, 1, 0});
        case GizmoAxis::Z:
        case GizmoAxis::PlaneZ:
            return RotateVector(orientation, Float3{0, 0, 1});
        default:
            return Float3{};
        }
    }

    bool TransformGizmo::IsAxisEnabled(GizmoAxis axis, GizmoMode mode) const
    {
        const Float3 viewDir = ViewDir();
        switch (axis)
        {
        case GizmoAxis::X:
        case GizmoAxis::Y:
        case GizmoAxis::Z:
            if (mode == GizmoMode::Rotate)
            {
                return true;
            } // rings stay pickable
            return 1.0f - Abs(Dot(AxisDirection(axis), viewDir)) > 0.01f;
        case GizmoAxis::PlaneX:
        case GizmoAxis::PlaneY:
        case GizmoAxis::PlaneZ:
            return Abs(Dot(AxisDirection(axis), viewDir)) > 0.05f;
        default:
            return true;
        }
    }

    GizmoAxis TransformGizmo::UpdateHover(const GizmoRay& ray, GizmoMode mode)
    {
        if (m_dragging)
        {
            return m_hovered;
        }
        m_hovered = GizmoAxis::None;

        i32 bestPriority = -1;
        f32 bestDist = kFloatMax;
        auto consider = [&](GizmoAxis axis, i32 priority, f32 dist, f32 threshold)
        {
            if (dist >= threshold)
            {
                return;
            }
            if (priority > bestPriority || (priority == bestPriority && dist < bestDist))
            {
                bestPriority = priority;
                bestDist = dist;
                m_hovered = axis;
            }
        };

        const f32 axisThreshold = size * 0.12f; // inflated vs the drawn ribbon
        if (mode == GizmoMode::Rotate)
        {
            const f32 radius = size * 0.8f;
            for (GizmoAxis a : {GizmoAxis::X, GizmoAxis::Y, GizmoAxis::Z})
            {
                // Only the camera-facing half of a ring is pickable (the far half isn't drawn).
                Float3 hit;
                const f32 d = RayRingDistance(ray, position, AxisDirection(a), radius, &hit);
                if (d < kFloatMax && Dot(hit - position, ViewDir()) > 0.0f)
                {
                    continue;
                }
                consider(a, 0, d, axisThreshold);
            }
            // Screen-space ring (outer).
            const f32 dv = RayRingDistance(ray, position, m_cameraForward, size * 1.0f, nullptr);
            consider(GizmoAxis::View, 1, dv, axisThreshold);
            return m_hovered;
        }

        // Translate / Scale: axes + (translate) planes + center. A foreshortening penalty
        // biases overlaps toward the axis more perpendicular to the ray: an axis nearly
        // parallel to the ray shadows a whole screen region at ~zero 3D distance (its shaft
        // overlaps others on screen) while being the worst one to drag (degenerate plane).
        for (GizmoAxis a : {GizmoAxis::X, GizmoAxis::Y, GizmoAxis::Z})
        {
            if (!IsAxisEnabled(a, mode))
            {
                continue;
            }
            const f32 dist = RayAxisDistance(ray, position, AxisDirection(a), size);
            const f32 penalty = Abs(Dot(ray.direction, AxisDirection(a))) * axisThreshold * 0.5f;
            consider(a, 0, dist + penalty, axisThreshold);
        }
        if (mode == GizmoMode::Translate)
        {
            for (GizmoAxis p : {GizmoAxis::PlaneX, GizmoAxis::PlaneY, GizmoAxis::PlaneZ})
            {
                if (!IsAxisEnabled(p, mode))
                {
                    continue;
                }
                if (PlaneQuadHit(ray, p, nullptr))
                {
                    consider(p, 1, 0.0f, 1.0f);
                }
            }
        }
        // Center handle: free move (translate) / uniform scale (scale).
        const f32 dc = RayPointDistance(ray, position);
        consider(GizmoAxis::View, 2, dc, size * 0.15f);
        return m_hovered;
    }

    bool TransformGizmo::BeginDrag(const GizmoRay& ray, GizmoMode mode)
    {
        if (m_hovered == GizmoAxis::None)
        {
            return false;
        }
        m_selected = m_hovered;
        m_dragging = true;
        m_dragStartPosition = position;

        if (mode == GizmoMode::Rotate)
        {
            // Capture the rotation frame NOW: the orientation tracks the entity each frame
            // (Local space), so re-deriving mid-drag would let the plane drift.
            if (m_selected == GizmoAxis::View)
            {
                m_dragRotationAxis = m_cameraForward;
                m_dragRotationU = CameraRight();
                m_dragRotationV = Cross(CameraRight(), m_cameraForward);
            }
            else
            {
                m_dragRotationAxis = AxisDirection(m_selected);
                switch (m_selected)
                {
                case GizmoAxis::X:
                    m_dragRotationU = AxisDirection(GizmoAxis::Y);
                    m_dragRotationV = AxisDirection(GizmoAxis::Z);
                    break;
                case GizmoAxis::Y:
                    m_dragRotationU = AxisDirection(GizmoAxis::Z);
                    m_dragRotationV = AxisDirection(GizmoAxis::X);
                    break;
                default:
                    m_dragRotationU = AxisDirection(GizmoAxis::X);
                    m_dragRotationV = AxisDirection(GizmoAxis::Y);
                    break;
                }
            }
            m_dragStartAngle = ComputeRotateAngle(ray, m_dragStartPosition, m_dragRotationAxis,
                                                  m_dragRotationU, m_dragRotationV);
            m_currentAngleDelta = 0.0f;
        }
        else
        {
            m_dragStartHitPoint = DragHitPoint(ray, m_selected, m_dragStartPosition);
        }
        return true;
    }

    Float3 TransformGizmo::UpdateTranslateDrag(const GizmoRay& ray, bool snap)
    {
        if (!m_dragging || m_selected == GizmoAxis::None)
        {
            return Float3{};
        }
        const Float3 delta =
            DragHitPoint(ray, m_selected, m_dragStartPosition) - m_dragStartHitPoint;

        switch (m_selected)
        {
        case GizmoAxis::X:
        case GizmoAxis::Y:
        case GizmoAxis::Z:
        {
            const Float3 axis = AxisDirection(m_selected);
            return axis * Snap(Dot(delta, axis), snap ? translateSnap : 0.0f);
        }
        case GizmoAxis::PlaneX:
        case GizmoAxis::PlaneY:
        case GizmoAxis::PlaneZ:
        {
            Float3 u, v;
            PlaneBasis(m_selected, u, v);
            return u * Snap(Dot(delta, u), snap ? translateSnap : 0.0f) +
                   v * Snap(Dot(delta, v), snap ? translateSnap : 0.0f);
        }
        case GizmoAxis::View:
        {
            const Float3 r = CameraRight();
            const Float3 up = Cross(r, m_cameraForward);
            return r * Snap(Dot(delta, r), snap ? translateSnap : 0.0f) +
                   up * Snap(Dot(delta, up), snap ? translateSnap : 0.0f);
        }
        default:
            return Float3{};
        }
    }

    TransformGizmo::RotateDelta TransformGizmo::UpdateRotateDrag(const GizmoRay& ray, bool snap)
    {
        if (!m_dragging || m_selected == GizmoAxis::None)
        {
            return {};
        }
        const f32 current = ComputeRotateAngle(ray, m_dragStartPosition, m_dragRotationAxis,
                                               m_dragRotationU, m_dragRotationV);
        f32 delta = current - m_dragStartAngle;
        if (delta > kPi)
        {
            delta -= kTwoPi;
        }
        if (delta < -kPi)
        {
            delta += kTwoPi;
        }
        delta = Snap(delta, snap ? DegreesToRadians(rotateSnapDegrees) : 0.0f);
        m_currentAngleDelta = delta; // angle-guide readout
        return RotateDelta{m_dragRotationAxis, delta};
    }

    Float3 TransformGizmo::UpdateScaleDrag(const GizmoRay& ray, bool snap)
    {
        if (!m_dragging || m_selected == GizmoAxis::None)
        {
            return Float3{};
        }
        const Float3 delta =
            DragHitPoint(ray, m_selected, m_dragStartPosition) - m_dragStartHitPoint;

        if (m_selected == GizmoAxis::View)
        {
            const Float3 r = CameraRight();
            const Float3 up = Cross(r, m_cameraForward);
            const f32 s = Snap(Dot(delta, Normalized(r + up)) / size, snap ? scaleSnap : 0.0f);
            return Float3{s, s, s};
        }

        const Float3 axis = AxisDirection(m_selected);
        const f32 s = Snap(Dot(delta, axis) / size, snap ? scaleSnap : 0.0f);
        switch (m_selected)
        {
        case GizmoAxis::X:
            return Float3{s, 0, 0};
        case GizmoAxis::Y:
            return Float3{0, s, 0};
        case GizmoAxis::Z:
            return Float3{0, 0, s};
        default:
            return Float3{};
        }
    }

    void TransformGizmo::EndDrag()
    {
        m_dragging = false;
        m_selected = GizmoAxis::None;
    }

    void TransformGizmo::ClearHover()
    {
        if (!m_dragging)
        {
            m_hovered = GizmoAxis::None;
        }
    }

    void TransformGizmo::Draw(render::debug::DebugDraw& dd, GizmoMode mode)
    {
        switch (mode)
        {
        case GizmoMode::Translate:
            DrawTranslate(dd);
            break;
        case GizmoMode::Rotate:
            DrawRotate(dd);
            break;
        case GizmoMode::Scale:
            DrawScale(dd);
            break;
        }
    }

    f32 TransformGizmo::RayAxisDistance(const GizmoRay& ray, Float3 axisOrigin, Float3 axisDir,
                                        f32 axisLength)
    {
        const Float3 d1 = ray.direction;
        const Float3 d2 = axisDir;
        const Float3 r = ray.origin - axisOrigin;
        const f32 a = Dot(d1, d1);
        const f32 b = Dot(d1, d2);
        const f32 c = Dot(d2, d2);
        const f32 d = Dot(d1, r);
        const f32 e = Dot(d2, r);
        const f32 denom = a * c - b * b;

        f32 t1, t2;
        if (Abs(denom) < 0.0001f)
        {
            t1 = 0.0f;
            t2 = e / c;
        }
        else
        {
            t1 = (b * e - c * d) / denom;
            t2 = (a * e - b * d) / denom;
        }
        t2 = Clamp(t2 / axisLength, 0.0f, 1.0f) * axisLength;
        t1 = Max(t1, 0.0f);
        return Distance(ray.origin + d1 * t1, axisOrigin + d2 * t2);
    }

    f32 TransformGizmo::RayRingDistance(const GizmoRay& ray, Float3 center, Float3 normal,
                                        f32 radius, Float3* outHit)
    {
        const f32 denom = Dot(normal, ray.direction);
        if (Abs(denom) < 0.0001f)
        {
            // Ray parallel to the ring plane: distance at the ray's closest approach to the
            // center, split into out-of-plane height + in-plane distance to the circle.
            const f32 t = Max(Dot(center - ray.origin, ray.direction), 0.0f);
            const Float3 offset = ray.origin + ray.direction * t - center;
            const f32 h = Dot(offset, normal);
            const Float3 inPlane = offset - normal * h;
            const f32 dr = Length(inPlane) - radius;
            return Sqrt(h * h + dr * dr);
        }
        const f32 t = Dot(normal, center - ray.origin) / denom;
        if (t < 0.0f)
        {
            return kFloatMax;
        }
        const Float3 hit = ray.origin + ray.direction * t;
        if (outHit != nullptr)
        {
            *outHit = hit;
        }
        return Abs(Length(hit - center) - radius);
    }

    f32 TransformGizmo::RayPointDistance(const GizmoRay& ray, Float3 point)
    {
        const f32 t = Max(Dot(point - ray.origin, ray.direction), 0.0f);
        return Distance(ray.origin + ray.direction * t, point);
    }

    Float3 TransformGizmo::CameraRight() const
    {
        const Float3 worldUp = (Abs(m_cameraForward.y) < 0.99f) ? Float3{0, 1, 0} : Float3{1, 0, 0};
        return Normalized(Cross(m_cameraForward, worldUp));
    }

    f32 TransformGizmo::Snap(f32 value, f32 increment)
    {
        if (increment <= 0.0f)
        {
            return value;
        }
        return Round(value / increment) * increment;
    }

    void TransformGizmo::PlaneBasis(GizmoAxis plane, Float3& u, Float3& v) const
    {
        switch (plane)
        {
        case GizmoAxis::PlaneX:
            u = AxisDirection(GizmoAxis::Y);
            v = AxisDirection(GizmoAxis::Z);
            break;
        case GizmoAxis::PlaneY:
            u = AxisDirection(GizmoAxis::X);
            v = AxisDirection(GizmoAxis::Z);
            break;
        default:
            u = AxisDirection(GizmoAxis::X);
            v = AxisDirection(GizmoAxis::Y);
            break;
        }
    }

    void TransformGizmo::PlaneQuadSigns(GizmoAxis plane, f32& su, f32& sv) const
    {
        Float3 u, v;
        PlaneBasis(plane, u, v);
        const Float3 toCam = m_cameraPos - position;
        su = (Dot(u, toCam) >= 0.0f) ? 1.0f : -1.0f;
        sv = (Dot(v, toCam) >= 0.0f) ? 1.0f : -1.0f;
    }

    bool TransformGizmo::PlaneQuadHit(const GizmoRay& ray, GizmoAxis plane, Float3* outHit) const
    {
        const Float3 normal = AxisDirection(plane);
        const f32 denom = Dot(normal, ray.direction);
        if (Abs(denom) < 0.0001f)
        {
            return false;
        }
        const f32 t = Dot(normal, position - ray.origin) / denom;
        if (t < 0.0f)
        {
            return false;
        }
        const Float3 hit = ray.origin + ray.direction * t;
        if (outHit != nullptr)
        {
            *outHit = hit;
        }

        Float3 u, v;
        f32 su, sv;
        PlaneBasis(plane, u, v);
        PlaneQuadSigns(plane, su, sv);
        const Float3 offset = hit - position;
        const f32 cu = Dot(offset, u) * su;
        const f32 cv = Dot(offset, v) * sv;
        return cu >= size * 0.25f && cu <= size * 0.55f && cv >= size * 0.25f && cv <= size * 0.55f;
    }

    Float3 TransformGizmo::DragHitPoint(const GizmoRay& ray, GizmoAxis axis,
                                        Float3 planeOrigin) const
    {
        Float3 planeNormal;
        switch (axis)
        {
        case GizmoAxis::X:
        case GizmoAxis::Y:
        case GizmoAxis::Z:
        {
            Float3 otherA, otherB;
            switch (axis)
            {
            case GizmoAxis::X:
                otherA = AxisDirection(GizmoAxis::Y);
                otherB = AxisDirection(GizmoAxis::Z);
                break;
            case GizmoAxis::Y:
                otherA = AxisDirection(GizmoAxis::X);
                otherB = AxisDirection(GizmoAxis::Z);
                break;
            default:
                otherA = AxisDirection(GizmoAxis::X);
                otherB = AxisDirection(GizmoAxis::Y);
                break;
            }
            planeNormal = (Abs(Dot(ray.direction, otherA)) > Abs(Dot(ray.direction, otherB)))
                              ? otherA
                              : otherB;
            break;
        }
        case GizmoAxis::PlaneX:
        case GizmoAxis::PlaneY:
        case GizmoAxis::PlaneZ:
            planeNormal = AxisDirection(axis);
            break;
        default:
            planeNormal = m_cameraForward;
            break;
        }

        const f32 denom = Dot(planeNormal, ray.direction);
        if (Abs(denom) < 0.0001f)
        {
            return planeOrigin;
        }
        const f32 t = Dot(planeNormal, planeOrigin - ray.origin) / denom;
        return ray.origin + ray.direction * t;
    }

    f32 TransformGizmo::ComputeRotateAngle(const GizmoRay& ray, Float3 center, Float3 normal,
                                           Float3 u, Float3 v)
    {
        const f32 denom = Dot(normal, ray.direction);
        if (Abs(denom) < 0.0001f)
        {
            return 0.0f;
        }
        f32 t = Dot(normal, center - ray.origin) / denom;
        if (t < 0.0f)
        {
            t = -t;
        } // reversed-ray retry
        const Float3 offset = ray.origin + ray.direction * t - center;
        return Atan2(Dot(offset, v), Dot(offset, u));
    }

    Color TransformGizmo::AxisColor(GizmoAxis axis, GizmoMode mode) const
    {
        static constexpr Color kSelected{1.0f, 1.0f, 0.4f, 1.0f};
        if (m_selected == axis)
        {
            return kSelected;
        }

        Color base;
        switch (axis)
        {
        case GizmoAxis::X:
        case GizmoAxis::PlaneX:
            base = Color{0.86f, 0.20f, 0.20f, 1.0f};
            break;
        case GizmoAxis::Y:
        case GizmoAxis::PlaneY:
            base = Color{0.20f, 0.86f, 0.20f, 1.0f};
            break;
        case GizmoAxis::Z:
        case GizmoAxis::PlaneZ:
            base = Color{0.20f, 0.40f, 0.86f, 1.0f};
            break;
        default:
            base = Color{0.8f, 0.8f, 0.8f, 1.0f};
            break;
        }
        if (!IsAxisEnabled(axis, mode)) // grazing fade
        {
            return Color{base.r * 0.5f, base.g * 0.5f, base.b * 0.5f, 0.35f};
        }
        if (m_hovered == axis) // 75% lerp toward white (PlayCanvas)
        {
            return Color{base.r + (1.0f - base.r) * 0.75f, base.g + (1.0f - base.g) * 0.75f,
                         base.b + (1.0f - base.b) * 0.75f, 1.0f};
        }
        return base;
    }

    void TransformGizmo::DrawThickLine(render::debug::DebugDraw& dd, Float3 from, Float3 to,
                                       Color color, f32 thickness) const
    {
        const Float3 lineDir = to - from;
        if (Dot(lineDir, lineDir) < 0.0001f)
        {
            return;
        }
        const Float3 mid = (from + to) * 0.5f;
        Float3 side = Cross(lineDir, mid - m_cameraPos);
        const f32 lenSq = Dot(side, side);
        if (lenSq < 0.0001f)
        {
            return;
        } // pointing at the camera: zero apparent width
        side = side * (thickness * 0.5f / Sqrt(lenSq));
        dd.DrawQuad(from - side, from + side, to + side, to - side, color, true);
    }

    void TransformGizmo::DrawAxisArrow(render::debug::DebugDraw& dd, GizmoAxis axis,
                                       GizmoMode mode) const
    {
        const Float3 dir = AxisDirection(axis);
        const Color color = AxisColor(axis, mode);
        const f32 thickness = size * 0.02f;
        const Float3 end = position + dir * size;
        DrawThickLine(dd, position, end, color, thickness);

        // Screen-facing arrowhead: two barbs in the ribbon plane.
        const Float3 viewDir = Normalized(end - m_cameraPos);
        Float3 side = Cross(dir, viewDir);
        const f32 lenSq = Dot(side, side);
        if (lenSq > 0.0001f)
        {
            side = side / Sqrt(lenSq);
            const f32 h = size * 0.1f;
            DrawThickLine(dd, end, end - dir * h + side * h, color, thickness);
            DrawThickLine(dd, end, end - dir * h - side * h, color, thickness);
        }
    }

    void TransformGizmo::DrawSpanLine(render::debug::DebugDraw& dd) const
    {
        // Full-length reference line across the scene during an axis drag: depth-tested
        // solid + faint overlay so the occluded part still reads (PlayCanvas span line).
        if (m_selected != GizmoAxis::X && m_selected != GizmoAxis::Y && m_selected != GizmoAxis::Z)
        {
            return;
        }
        const Float3 dir = AxisDirection(m_selected);
        const Color c = AxisColor(m_selected, GizmoMode::Translate);
        const f32 span = size * 1000.0f;
        dd.DrawLine(position - dir * span, position + dir * span, c, false);
        dd.DrawLine(position - dir * span, position + dir * span, Color{c.r, c.g, c.b, 0.25f},
                    true);
    }

    void TransformGizmo::DrawTranslate(render::debug::DebugDraw& dd)
    {
        for (GizmoAxis a : {GizmoAxis::X, GizmoAxis::Y, GizmoAxis::Z})
        {
            DrawAxisArrow(dd, a, GizmoMode::Translate);
        }

        // Plane quads in the camera-facing quadrant.
        for (GizmoAxis p : {GizmoAxis::PlaneX, GizmoAxis::PlaneY, GizmoAxis::PlaneZ})
        {
            if (!IsAxisEnabled(p, GizmoMode::Translate))
            {
                continue;
            }
            Float3 u, v;
            f32 su, sv;
            PlaneBasis(p, u, v);
            PlaneQuadSigns(p, su, sv);
            const Float3 a = position + u * (su * size * 0.25f) + v * (sv * size * 0.25f);
            const Float3 b = position + u * (su * size * 0.55f) + v * (sv * size * 0.25f);
            const Float3 c = position + u * (su * size * 0.55f) + v * (sv * size * 0.55f);
            const Float3 d = position + u * (su * size * 0.25f) + v * (sv * size * 0.55f);
            Color color = AxisColor(p, GizmoMode::Translate);
            color.a *= (m_hovered == p || m_selected == p) ? 0.6f : 0.35f;
            dd.DrawQuad(a, b, c, d, color, true);
        }

        const Color center = AxisColor(GizmoAxis::View, GizmoMode::Translate);
        dd.DrawWireSphereOverlay(position, size * 0.1f, center, 16);
        if (m_dragging)
        {
            DrawSpanLine(dd);
        }
    }

    void TransformGizmo::DrawScale(render::debug::DebugDraw& dd)
    {
        const f32 thickness = size * 0.02f;
        for (GizmoAxis a : {GizmoAxis::X, GizmoAxis::Y, GizmoAxis::Z})
        {
            const Float3 dir = AxisDirection(a);
            const Color color = AxisColor(a, GizmoMode::Scale);
            DrawThickLine(dd, position, position + dir * size, color, thickness);
            dd.DrawFilledBoxCenter(position + dir * size,
                                   Float3{size * 0.05f, size * 0.05f, size * 0.05f}, color, true);
        }
        const Color center = AxisColor(GizmoAxis::View, GizmoMode::Scale);
        dd.DrawFilledBoxCenter(position, Float3{size * 0.07f, size * 0.07f, size * 0.07f}, center,
                               true);
        if (m_dragging)
        {
            DrawSpanLine(dd);
        }
    }

    void TransformGizmo::DrawRotate(render::debug::DebugDraw& dd)
    {
        const f32 radius = size * 0.8f;
        const f32 thickness = size * 0.02f;
        constexpr i32 kSegments = 48;

        for (GizmoAxis a : {GizmoAxis::X, GizmoAxis::Y, GizmoAxis::Z})
        {
            Float3 u, v;
            switch (a)
            {
            case GizmoAxis::X:
                u = AxisDirection(GizmoAxis::Y);
                v = AxisDirection(GizmoAxis::Z);
                break;
            case GizmoAxis::Y:
                u = AxisDirection(GizmoAxis::X);
                v = AxisDirection(GizmoAxis::Z);
                break;
            default:
                u = AxisDirection(GizmoAxis::X);
                v = AxisDirection(GizmoAxis::Y);
                break;
            }
            const Color color = AxisColor(a, GizmoMode::Rotate);
            const bool fullRing = m_dragging && m_selected == a; // active axis shows the whole ring
            DrawRing(dd, u, v, radius, color, thickness, kSegments, !fullRing);
        }

        // Screen-space ring (always full; it faces the camera by construction).
        const Float3 r = CameraRight();
        const Float3 up = Cross(r, m_cameraForward);
        DrawRing(dd, r, up, size * 1.0f, AxisColor(GizmoAxis::View, GizmoMode::Rotate), thickness,
                 kSegments, false);

        if (m_dragging)
        {
            DrawAngleGuide(dd, radius);
        }
    }

    void TransformGizmo::DrawRing(render::debug::DebugDraw& dd, Float3 u, Float3 v, f32 radius,
                                  Color color, f32 thickness, i32 segments, bool cullBackHalf) const
    {
        const Float3 uN = Normalized(u);
        const Float3 vN = Normalized(v);
        const Float3 viewDir = ViewDir();
        Float3 prev = position + uN * radius;
        for (i32 i = 1; i <= segments; ++i)
        {
            const f32 t = static_cast<f32>(i) / static_cast<f32>(segments) * kTwoPi;
            const Float3 point = position + uN * (radius * Cos(t)) + vN * (radius * Sin(t));
            const Float3 mid = (prev + point) * 0.5f;
            if (!cullBackHalf || Dot(mid - position, viewDir) <= 0.0f)
            {
                DrawThickLine(dd, prev, point, color, thickness);
            }
            prev = point;
        }
    }

    void TransformGizmo::DrawAngleGuide(render::debug::DebugDraw& dd, f32 radius)
    {
        // Faint start-reference line + solid current line + degree readout (PlayCanvas).
        const Float3 startDir =
            m_dragRotationU * Cos(m_dragStartAngle) + m_dragRotationV * Sin(m_dragStartAngle);
        const f32 current = m_dragStartAngle + m_currentAngleDelta;
        const Float3 currentDir = m_dragRotationU * Cos(current) + m_dragRotationV * Sin(current);
        const Color solid{1.0f, 1.0f, 0.4f, 1.0f};
        dd.DrawLine(position, position + startDir * radius, Color{1.0f, 1.0f, 0.4f, 0.35f}, true);
        dd.DrawLine(position, position + currentDir * radius, solid, true);

        // Integer-degree readout past the current line's tip.
        i32 degrees = static_cast<i32>(RadiansToDegrees(m_currentAngleDelta) +
                                       (m_currentAngleDelta >= 0 ? 0.5f : -0.5f));
        utf8char text[16];
        i32 n = 0;
        if (degrees < 0)
        {
            text[n++] = utf8char('-');
            degrees = -degrees;
        }
        utf8char digits[8];
        i32 d = 0;
        do
        {
            digits[d++] = static_cast<utf8char>('0' + degrees % 10);
            degrees /= 10;
        } while (degrees > 0 && d < 8);
        while (d > 0)
        {
            text[n++] = digits[--d];
        }
        text[n++] = utf8char(0xC2);
        text[n++] = utf8char(0xB0); // degree sign U+00B0
        dd.DrawText3D(position + currentDir * (radius * 1.15f),
                      StringView(text, static_cast<usize>(n)), solid);
    }
}
