// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The spline tool implementation (see SplineTool.cppm). Heavy imports live here.

module;
#include "Core/Prelude.h"

module editor.spline;

import foundation.core;
import foundation.scene;
import foundation.render;
import foundation.shell;
import foundation.spline;
import engine.spline;
import editor.core;
import editor.viewporttools;

using namespace foundation::core;

namespace editor
{
    namespace scene = foundation::scene;
    namespace fspline = foundation::spline;
    using engine::spline::SplineComponent;
    using engine::spline::SplineComponentManager;

    namespace
    {
        constexpr f32 kPickScale = 0.02f;  // screen-constant pick radius per unit of distance
        constexpr f32 kMarkerScale = 0.012f;

        // One gesture = one command: the whole point set before/after (points are small; a
        // positional diff would buy nothing).
        class SplineEditCommand final : public IEditorCommand
        {
        public:
            SplineEditCommand(scene::Scene& sceneRef, const Guid& entityId,
                              Array<fspline::SplinePoint> before, bool beforeClosed,
                              Array<fspline::SplinePoint> after, bool afterClosed)
                : m_scene(&sceneRef), m_entity(entityId), m_before(Move(before)),
                  m_beforeClosed(beforeClosed), m_after(Move(after)), m_afterClosed(afterClosed)
            {
            }

            [[nodiscard]] bool Execute() override { return Apply(m_after, m_afterClosed); }
            void Undo() override { (void)Apply(m_before, m_beforeClosed); }
            [[nodiscard]] StringView TypeId() const override { return u8"spline.edit"; }

        private:
            [[nodiscard]] bool Apply(const Array<fspline::SplinePoint>& points, bool closed)
            {
                auto* manager = m_scene->GetSystem<SplineComponentManager>();
                if (manager == nullptr)
                {
                    return false;
                }
                SplineComponent* component = manager->Get(m_scene->FindEntity(m_entity));
                if (component == nullptr)
                {
                    return false;
                }
                component->curve.points = points;
                component->curve.closed = closed;
                component->curve.UpdateAutoHandles();
                component->curve.RebuildArcLength();
                return true;
            }

            scene::Scene* m_scene;
            Guid m_entity;
            Array<fspline::SplinePoint> m_before;
            bool m_beforeClosed;
            Array<fspline::SplinePoint> m_after;
            bool m_afterClosed;
        };

        // Distance from a ray to a point, and the along-ray parameter of the closest approach.
        [[nodiscard]] f32 RayPointDistance(const ViewportRay& ray, Float3 point, f32& outAlong)
        {
            const Float3 toPoint = point - ray.origin;
            outAlong = Max(Dot(toPoint, ray.direction), 0.0f);
            return Length(toPoint - ray.direction * outAlong);
        }

        class SplineEditTool final : public IViewportTool
        {
        public:
            explicit SplineEditTool(const ViewportToolHostContext& context)
                : m_scene(context.scene), m_commands(context.commands),
                  m_selection(context.entitySelection)
            {
            }

            [[nodiscard]] StringView Id() const override { return u8"spline.edit"; }
            [[nodiscard]] StringView DisplayName() const override { return u8"Edit Spline"; } // as its refusal names it
            [[nodiscard]] StringView Icon() const override { return u8"Editor/Icons/Types/SplineComponent.svg"; }
            [[nodiscard]] SplineEditToolState State() const noexcept
            {
                return SplineEditToolState{m_hoverPoint, m_selectedPoint, m_dragging,
                                           m_hasInsertPreview, m_hasPlacePreview};
            }

            [[nodiscard]] bool IsAvailable() const override
            {
                return TargetComponent() != nullptr;
            }
            [[nodiscard]] StringView UnavailableReason() const override
            {
                return u8"Edit Spline needs a selected entity with a Spline component.";
            }

            void OnDeactivate() override { EndDrag(/*commit*/ true); }

            bool Update(const ViewportToolInput& input) override
            {
                m_hoverPoint = -1;
                m_hasInsertPreview = false;
                m_hasPlacePreview = false;
                SplineComponent* component = TargetComponent(&m_entity);
                if (component == nullptr || m_scene == nullptr)
                {
                    EndDrag(true);
                    return false;
                }
                m_world = m_scene->GetWorldMatrix(m_scene->FindEntity(m_entity));
                fspline::SplineCurve& curve = component->curve;

                if (!input.pointerValid)
                {
                    EndDrag(true);
                    return m_dragging;
                }

                // Hover: nearest control point within the screen-constant radius; the
                // SELECTED point's tangent handles pick first (they sit near the point).
                m_hoverHandle = -1;
                f32 bestDistance = kFloatMax;
                if (m_selectedPoint >= 0 &&
                    m_selectedPoint < static_cast<i32>(curve.points.Size()))
                {
                    const fspline::SplinePoint& selected =
                        curve.points[static_cast<usize>(m_selectedPoint)];
                    const Float3 ends[2] = {
                        TransformPoint(selected.position + selected.inHandle, m_world),
                        TransformPoint(selected.position + selected.outHandle, m_world)};
                    for (i32 h = 0; h < 2; ++h)
                    {
                        f32 along = 0.0f;
                        const f32 d = RayPointDistance(input.ray, ends[h], along);
                        if (d < along * kPickScale && d < bestDistance)
                        {
                            bestDistance = d;
                            m_hoverHandle = h;
                        }
                    }
                }
                if (m_hoverHandle < 0)
                {
                    for (usize i = 0; i < curve.points.Size(); ++i)
                    {
                        const Float3 world = TransformPoint(curve.points[i].position, m_world);
                        f32 along = 0.0f;
                        const f32 d = RayPointDistance(input.ray, world, along);
                        if (d < along * kPickScale && d < bestDistance)
                        {
                            bestDistance = d;
                            m_hoverPoint = static_cast<i32>(i);
                        }
                    }
                }

                // Insert preview: closest curve position to the ray (only meaningful with Ctrl
                // and no point under the pointer).
                if (input.ctrl && m_hoverPoint < 0 && curve.SegmentCount() > 0)
                {
                    FindRayClosest(curve, input.ray);
                }
                // Place preview: a curve with no segment yet (a component added bare, or cut
                // down to one point) takes its points from Ctrl-clicks, on the camera-facing
                // plane through the entity (through its last point once it has one), so a
                // spline can be built from nothing.
                if (input.ctrl && m_hoverPoint < 0 && curve.SegmentCount() == 0 && !m_dragging)
                {
                    const Float3 through =
                        curve.points.IsEmpty()
                            ? TransformPoint(Float3{}, m_world)
                            : TransformPoint(curve.points[curve.points.Size() - 1].position, m_world);
                    const Float3 normal = input.cameraForward * -1.0f;
                    const f32 denominator = Dot(input.ray.direction, normal);
                    if (Abs(denominator) > 0.0001f)
                    {
                        const f32 t = Dot(through - input.ray.origin, normal) / denominator;
                        if (t > 0.0f)
                        {
                            const Float3 world = input.ray.origin + input.ray.direction * t;
                            m_placeLocal = TransformPoint(world, Inverse(m_world));
                            m_hasPlacePreview = true;
                        }
                    }
                }

                if (m_dragging)
                {
                    if (input.editingLocked || !input.leftDown)
                    {
                        EndDrag(!input.editingLocked);
                        return true;
                    }
                    // Drag on the camera-facing plane through the grab point.
                    const f32 denominator = Dot(input.ray.direction, m_dragPlaneNormal);
                    if (Abs(denominator) > 0.0001f &&
                        m_dragPoint < static_cast<i32>(curve.points.Size()))
                    {
                        const f32 t =
                            Dot(m_dragPlaneOrigin - input.ray.origin, m_dragPlaneNormal) /
                            denominator;
                        if (t > 0.0f)
                        {
                            const Float3 world = input.ray.origin + input.ray.direction * t;
                            const Float3 local = TransformPoint(world, Inverse(m_world));
                            fspline::SplinePoint& point =
                                curve.points[static_cast<usize>(m_dragPoint)];
                            if (m_dragHandle < 0)
                            {
                                point.position = local;
                            }
                            else
                            {
                                // Editing a handle promotes Auto; Shift breaks the pair.
                                if (point.mode == fspline::SplineHandleMode::Auto)
                                {
                                    point.mode = fspline::SplineHandleMode::Smooth;
                                }
                                if (input.shift)
                                {
                                    point.mode = fspline::SplineHandleMode::Broken;
                                }
                                Float3& dragged =
                                    (m_dragHandle == 0) ? point.inHandle : point.outHandle;
                                Float3& other =
                                    (m_dragHandle == 0) ? point.outHandle : point.inHandle;
                                dragged = local - point.position;
                                if (point.mode == fspline::SplineHandleMode::Smooth)
                                {
                                    // Collinear: the other handle keeps its own magnitude.
                                    const f32 draggedLength = Length(dragged);
                                    if (draggedLength > 0.0001f)
                                    {
                                        other = dragged * (-Length(other) / draggedLength);
                                    }
                                }
                            }
                            curve.UpdateAutoHandles();
                            curve.RebuildArcLength();
                        }
                    }
                    return true;
                }

                if (input.editingLocked)
                {
                    return m_hoverPoint >= 0;
                }

                // Delete the hovered point (two-point floor keeps the curve a curve).
                if (m_hoverPoint >= 0 && input.keyboard != nullptr &&
                    (input.keyboard->IsKeyPressed(foundation::shell::KeyCode::Delete) ||
                     input.keyboard->IsKeyPressed(foundation::shell::KeyCode::X)) &&
                    curve.points.Size() > 2)
                {
                    BeginSnapshot(curve);
                    curve.points.RemoveAt(static_cast<usize>(m_hoverPoint));
                    curve.UpdateAutoHandles();
                    curve.RebuildArcLength();
                    CommitSnapshot(curve);
                    m_hoverPoint = -1;
                    m_selectedPoint = -1;
                    return true;
                }

                if (input.leftPressed && input.pointerOver)
                {
                    if (m_hoverHandle >= 0 && m_selectedPoint >= 0)
                    {
                        const fspline::SplinePoint& selected =
                            curve.points[static_cast<usize>(m_selectedPoint)];
                        BeginSnapshot(curve);
                        m_dragging = true;
                        m_dragPoint = m_selectedPoint;
                        m_dragHandle = m_hoverHandle;
                        m_dragPlaneOrigin = TransformPoint(
                            selected.position +
                                (m_hoverHandle == 0 ? selected.inHandle : selected.outHandle),
                            m_world);
                        m_dragPlaneNormal = input.cameraForward * -1.0f;
                        return true;
                    }
                    if (m_hoverPoint >= 0)
                    {
                        // Grab: gesture snapshot + a camera-facing drag plane.
                        BeginSnapshot(curve);
                        m_dragging = true;
                        m_dragPoint = m_hoverPoint;
                        m_dragHandle = -1;
                        m_selectedPoint = m_hoverPoint;
                        m_dragPlaneOrigin = TransformPoint(
                            curve.points[static_cast<usize>(m_dragPoint)].position, m_world);
                        m_dragPlaneNormal = input.cameraForward * -1.0f;
                        return true;
                    }
                    if (input.ctrl && m_hasInsertPreview)
                    {
                        BeginSnapshot(curve);
                        fspline::SplinePoint point;
                        point.position = m_insertLocal;
                        const usize after = static_cast<usize>(m_insertT) + 1;
                        curve.points.Insert(Min(after, curve.points.Size()), point);
                        curve.UpdateAutoHandles();
                        curve.RebuildArcLength();
                        CommitSnapshot(curve);
                        return true;
                    }
                    if (input.ctrl && m_hasPlacePreview)
                    {
                        // Append (one undo step each): the first two clicks make the curve a
                        // curve, after which Ctrl-click inserts on the segment under the pointer.
                        BeginSnapshot(curve);
                        fspline::SplinePoint point;
                        point.position = m_placeLocal;
                        curve.points.PushBack(point);
                        curve.UpdateAutoHandles();
                        curve.RebuildArcLength();
                        CommitSnapshot(curve);
                        m_selectedPoint = static_cast<i32>(curve.points.Size()) - 1;
                        m_hasPlacePreview = false;
                        return true;
                    }
                }
                return m_hoverPoint >= 0;
            }

            void Draw(foundation::render::debug::DebugDraw& drawList) override
            {
                SplineComponent* component = TargetComponent();
                if (component == nullptr)
                {
                    return;
                }
                const fspline::SplineCurve& curve = component->curve;
                const Color curveColor{0.35f, 0.85f, 1.0f, 1.0f};
                const Color pointColor{1.0f, 0.85f, 0.2f, 1.0f};
                const Color hoverColor{1.0f, 0.4f, 0.2f, 1.0f};

                // The curve as a polyline (samples per segment match the arc table density).
                const u32 segments = curve.SegmentCount();
                if (segments > 0)
                {
                    const u32 steps = segments * fspline::SplineCurve::kSamplesPerSegment;
                    Float3 previous = TransformPoint(curve.Evaluate(0.0f), m_world);
                    for (u32 i = 1; i <= steps; ++i)
                    {
                        const f32 t = curve.MaxT() * static_cast<f32>(i) / static_cast<f32>(steps);
                        const Float3 position = TransformPoint(curve.Evaluate(t), m_world);
                        drawList.DrawLine(previous, position, curveColor);
                        previous = position;
                    }
                }
                for (usize i = 0; i < curve.points.Size(); ++i)
                {
                    const Float3 world = TransformPoint(curve.points[i].position, m_world);
                    const bool hovered = static_cast<i32>(i) == m_hoverPoint;
                    const f32 size = kMarkerScale * Max(Length(world - m_dragPlaneOrigin), 1.0f);
                    drawList.DrawWireSphere(world, hovered ? 0.14f : 0.1f,
                                            hovered ? hoverColor : pointColor);
                    (void)size;
                }
                if (m_selectedPoint >= 0 &&
                    m_selectedPoint < static_cast<i32>(curve.points.Size()))
                {
                    const fspline::SplinePoint& selected =
                        curve.points[static_cast<usize>(m_selectedPoint)];
                    const Float3 anchor = TransformPoint(selected.position, m_world);
                    const Color handleColor{0.75f, 0.6f, 1.0f, 1.0f};
                    const Float3 ends[2] = {
                        TransformPoint(selected.position + selected.inHandle, m_world),
                        TransformPoint(selected.position + selected.outHandle, m_world)};
                    for (i32 h = 0; h < 2; ++h)
                    {
                        drawList.DrawLine(anchor, ends[h], handleColor);
                        drawList.DrawWireSphere(ends[h], m_hoverHandle == h ? 0.09f : 0.06f,
                                                m_hoverHandle == h ? hoverColor : handleColor,
                                                10);
                    }
                }
                if (m_hasPlacePreview)
                {
                    drawList.DrawWireSphere(TransformPoint(m_placeLocal, m_world), 0.08f,
                                            Color{0.6f, 1.0f, 0.6f, 1.0f});
                }
                if (m_hasInsertPreview)
                {
                    drawList.DrawWireSphere(TransformPoint(m_insertLocal, m_world), 0.08f,
                                            Color{0.4f, 1.0f, 0.4f, 1.0f});
                }
            }

            [[nodiscard]] StringView StatusText() const override
            {
                return u8"drag point/handle (Shift: break pair) | Ctrl+click segment: insert | Del/X: remove";
            }

        private:
            [[nodiscard]] SplineComponent* TargetComponent(Guid* outEntity = nullptr) const
            {
                if (m_scene == nullptr || m_selection == nullptr || m_selection->Items().IsEmpty())
                {
                    return nullptr;
                }
                auto* manager = m_scene->GetSystem<SplineComponentManager>();
                if (manager == nullptr)
                {
                    return nullptr;
                }
                for (const Guid& id : m_selection->Items())
                {
                    if (SplineComponent* component = manager->Get(m_scene->FindEntity(id)))
                    {
                        if (outEntity != nullptr)
                        {
                            *outEntity = id;
                        }
                        return component;
                    }
                }
                return nullptr;
            }

            void FindRayClosest(const fspline::SplineCurve& curve, const ViewportRay& ray)
            {
                const u32 steps = curve.SegmentCount() * fspline::SplineCurve::kSamplesPerSegment;
                f32 bestDistance = kFloatMax;
                for (u32 i = 0; i <= steps; ++i)
                {
                    const f32 t = curve.MaxT() * static_cast<f32>(i) / static_cast<f32>(steps);
                    const Float3 world = TransformPoint(curve.Evaluate(t), m_world);
                    f32 along = 0.0f;
                    const f32 d = RayPointDistance(ray, world, along);
                    if (d < along * kPickScale && d < bestDistance)
                    {
                        bestDistance = d;
                        m_insertT = t;
                        m_insertLocal = curve.Evaluate(t);
                        m_hasInsertPreview = true;
                    }
                }
            }

            void BeginSnapshot(const fspline::SplineCurve& curve)
            {
                m_snapshotPoints = curve.points;
                m_snapshotClosed = curve.closed;
            }

            void CommitSnapshot(const fspline::SplineCurve& curve)
            {
                if (m_commands == nullptr)
                {
                    return;
                }
                (void)m_commands->Execute(UniquePtr<IEditorCommand>(
                    editor::EditorRootAllocator().New<SplineEditCommand>(*m_scene, m_entity,
                                                              Move(m_snapshotPoints),
                                                              m_snapshotClosed, curve.points,
                                                              curve.closed),
                    editor::EditorRootAllocator()));
            }

            void EndDrag(bool commit)
            {
                if (!m_dragging)
                {
                    return;
                }
                m_dragging = false;
                SplineComponent* component = TargetComponent();
                if (component == nullptr)
                {
                    return;
                }
                if (commit)
                {
                    CommitSnapshot(component->curve);
                }
                else
                {
                    // Aborted (edit lock landed mid-drag): restore the snapshot.
                    component->curve.points = Move(m_snapshotPoints);
                    component->curve.closed = m_snapshotClosed;
                    component->curve.UpdateAutoHandles();
                    component->curve.RebuildArcLength();
                }
            }

            scene::Scene* m_scene;
            EditorCommandStack* m_commands;
            Selection<Guid>* m_selection;

            Guid m_entity{};
            Float4x4 m_world = Float4x4::Identity();
            i32 m_hoverPoint = -1;

            bool m_dragging = false;
            i32 m_dragPoint = -1;
            i32 m_dragHandle = -1; // -1 = the point itself; 0 = inHandle, 1 = outHandle
            i32 m_selectedPoint = -1;
            i32 m_hoverHandle = -1;
            Float3 m_dragPlaneOrigin{};
            Float3 m_dragPlaneNormal{0, 0, 1};

            bool m_hasInsertPreview = false;
            bool m_hasPlacePreview = false; // Ctrl over an empty curve: the next point's spot
            Float3 m_placeLocal{};
            f32 m_insertT = 0.0f;
            Float3 m_insertLocal{};

            Array<fspline::SplinePoint> m_snapshotPoints;
            bool m_snapshotClosed = false;
        };
    }

    UniquePtr<IViewportTool> CreateSplineEditTool(const ViewportToolHostContext& context)
    {
        return UniquePtr<IViewportTool>(editor::EditorRootAllocator().New<SplineEditTool>(context),
                                        editor::EditorRootAllocator());
    }

    SplineEditToolState SplineEditToolStateOf(const IViewportTool& tool)
    {
        // The Id is the type check (no RTTI): only the spline tool answers "spline.edit".
        if (tool.Id() != StringView(u8"spline.edit"))
        {
            return SplineEditToolState{};
        }
        return static_cast<const SplineEditTool&>(tool).State();
    }

    void SplineViewportToolProvider::CreateTools(ViewportToolManager& manager,
                                                 const ViewportToolHostContext& context)
    {
        manager.Add(CreateSplineEditTool(context));
    }

    void RegisterSplineViewportTools()
    {
        static SplineViewportToolProvider provider;
        ViewportToolProviderRegistry::Get().Register(&provider);
    }
}
