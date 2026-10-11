// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :tools implementation (SelectTransformTool).

module;
#include "Core/Prelude.h"

module editor.scene;

import foundation.core;
import foundation.scene;
import foundation.shell;
import foundation.render; // debug::DebugDraw (the marquee rect)
import editor.viewporttools;

using namespace foundation::core;

namespace editor
{
    namespace scene = foundation::scene;

    bool SelectTransformTool::Update(const ViewportToolInput& input)
    {
        GizmoFrameInput gizmoInput;
        gizmoInput.ray = GizmoRay{input.ray.origin, input.ray.direction};
        gizmoInput.cameraPosition = input.cameraPosition;
        gizmoInput.cameraForward = input.cameraForward;
        gizmoInput.fovY = input.fovY;
        // editingLocked (Simulate) maps to the controller's pointer-less update: the gizmo pose
        // keeps tracking what physics/animation move, but no hover, no drags, no mode hotkeys -
        // exactly the pre-framework Simulate branch.
        gizmoInput.pointerValid = input.pointerValid && !input.editingLocked;
        gizmoInput.leftPressed = input.leftPressed;
        gizmoInput.leftDown = input.leftDown;
        gizmoInput.leftReleased = input.leftReleased;
        gizmoInput.snap = input.ctrl;
        if (foundation::shell::IKeyboard* keyboard = input.keyboard;
            keyboard != nullptr && gizmoInput.pointerValid)
        {
            gizmoInput.keyTranslate = keyboard->IsKeyPressed(foundation::shell::KeyCode::W);
            gizmoInput.keyRotate = keyboard->IsKeyPressed(foundation::shell::KeyCode::E);
            gizmoInput.keyScale = keyboard->IsKeyPressed(foundation::shell::KeyCode::R);
            gizmoInput.keyToggleSpace = keyboard->IsKeyPressed(foundation::shell::KeyCode::X);
            gizmoInput.vertexSnap = keyboard->IsKeyDown(foundation::shell::KeyCode::V); // held
        }
        const bool consumed = m_gizmos.Update(gizmoInput);

        PollPick(); // a GPU answer from an earlier click / marquee lands here
        if (!consumed && input.pointerOver && input.leftPressed)
        {
            // Arm the marquee BEFORE the click picks: if this press drags past the threshold it
            // becomes a rect select and the click is undone back to this snapshot.
            m_marquee.armed = input.pointerValid && input.viewportWidth > 0 &&
                              input.viewportHeight > 0;
            m_marquee.active = false;
            m_marquee.ctrl = input.ctrl;
            m_marquee.pressX = input.pointerX;
            m_marquee.pressY = input.pointerY;
            m_marquee.currentX = input.pointerX;
            m_marquee.currentY = input.pointerY;
            m_marquee.selectionAtPress.Clear();
            for (const Guid& id : m_edit->EntitySelection().Items())
            {
                m_marquee.selectionAtPress.PushBack(id);
            }
            PickOnClick(input);
        }
        else if (m_marquee.armed)
        {
            UpdateMarquee(input);
        }
        return consumed || m_marquee.active;
    }

    void SelectTransformTool::UpdateMarquee(const ViewportToolInput& input)
    {
        if (input.pointerValid)
        {
            m_marquee.currentX = input.pointerX; // the release frame's position counts too
            m_marquee.currentY = input.pointerY;
        }
        if (!input.leftDown || input.leftReleased || !input.pointerValid)
        {
            FinishMarquee(input);
            return;
        }
        if (!m_marquee.active)
        {
            const i32 dx = Abs(m_marquee.currentX - m_marquee.pressX);
            const i32 dy = Abs(m_marquee.currentY - m_marquee.pressY);
            if (dx < kMarqueeThreshold && dy < kMarqueeThreshold)
            {
                return;
            }
            m_marquee.active = true;
            // The press was a click until now: drop its pick (in flight or already applied)
            // and put the selection back the way it was.
            m_pendingPick = 0;
            m_edit->EntitySelection().Set(Span<const Guid>{m_marquee.selectionAtPress.Data(),
                                                           m_marquee.selectionAtPress.Size()});
        }
    }

    void SelectTransformTool::FinishMarquee(const ViewportToolInput& input)
    {
        const bool wasActive = m_marquee.active;
        m_marquee.armed = false;
        m_marquee.active = false;
        if (!wasActive)
        {
            return; // a click: PickOnClick handled it
        }
        const i32 x0 = Min(m_marquee.pressX, m_marquee.currentX);
        const i32 y0 = Min(m_marquee.pressY, m_marquee.currentY);
        const i32 x1 = Max(m_marquee.pressX, m_marquee.currentX);
        const i32 y1 = Max(m_marquee.pressY, m_marquee.currentY);
        if (m_picker != nullptr)
        {
            const u32 request = m_picker->RequestPick(
                x0, y0, static_cast<u32>(x1 - x0 + 1), static_cast<u32>(y1 - y0 + 1));
            if (request != 0)
            {
                m_pendingMarquee = request; // the newest rect wins over one still in flight
                m_marqueeCtrl = m_marquee.ctrl;
                return;
            }
        }
        Array<Guid> picked;
        CpuMarquee(input, x0, y0, x1, y1, picked);
        ApplyMarquee(Span<const Guid>{picked.Data(), picked.Size()}, m_marquee.ctrl);
    }

    void SelectTransformTool::ApplyMarquee(Span<const Guid> picked, bool ctrl)
    {
        Selection<Guid>& selection = m_edit->EntitySelection();
        if (ctrl)
        {
            for (const Guid& id : picked)
            {
                if (!selection.Contains(id))
                {
                    selection.Add(id);
                }
            }
            return;
        }
        if (picked.IsEmpty())
        {
            selection.Clear();
        }
        else
        {
            selection.Set(picked);
        }
    }

    void SelectTransformTool::CpuMarquee(const ViewportToolInput& input, i32 x0, i32 y0, i32 x1,
                                         i32 y1, Array<Guid>& out) const
    {
        out.Clear();
        if (input.viewportWidth == 0 || input.viewportHeight == 0)
        {
            return;
        }
        // The view's basis from the camera forward (roll-free, like the editor camera) and its
        // frustum tangents from fovY + the viewport aspect: enough to project an origin to a pixel.
        const Float3 forward = Normalized(input.cameraForward);
        Float3 right = Cross(forward, Float3{0.0f, 1.0f, 0.0f});
        if (Dot(right, right) < 1e-6f)
        {
            right = Float3{1.0f, 0.0f, 0.0f}; // looking straight up/down
        }
        right = Normalized(right);
        const Float3 up = Cross(right, forward);
        const f32 tanY = Tan(input.fovY * 0.5f);
        const f32 tanX = tanY * (static_cast<f32>(input.viewportWidth) /
                                 static_cast<f32>(input.viewportHeight));
        const f32 w = static_cast<f32>(input.viewportWidth);
        const f32 h = static_cast<f32>(input.viewportHeight);
        scene::Scene& scene = m_edit->Scene();
        scene.ForEachEntity(
            [&](scene::EntityHandle e)
            {
                const Float4x4 world = scene.GetWorldMatrix(e);
                const Float3 d = Float3{world.m[3][0], world.m[3][1], world.m[3][2]} -
                                 input.cameraPosition;
                const f32 z = Dot(d, forward);
                if (z <= 1e-4f)
                {
                    return; // behind the camera
                }
                const f32 nx = Dot(d, right) / (z * tanX);
                const f32 ny = Dot(d, up) / (z * tanY);
                const f32 px = (nx * 0.5f + 0.5f) * w;
                const f32 py = (0.5f - ny * 0.5f) * h;
                if (px >= static_cast<f32>(x0) && px < static_cast<f32>(x1 + 1) &&
                    py >= static_cast<f32>(y0) && py < static_cast<f32>(y1 + 1))
                {
                    out.PushBack(scene.GetEntityId(e));
                }
            });
    }

    void SelectTransformTool::Draw(render::debug::DebugDraw& drawList)
    {
        m_gizmos.Draw(drawList);
        if (!m_marquee.active)
        {
            return;
        }
        const f32 x0 = static_cast<f32>(Min(m_marquee.pressX, m_marquee.currentX));
        const f32 y0 = static_cast<f32>(Min(m_marquee.pressY, m_marquee.currentY));
        const f32 x1 = static_cast<f32>(Max(m_marquee.pressX, m_marquee.currentX)) + 1.0f;
        const f32 y1 = static_cast<f32>(Max(m_marquee.pressY, m_marquee.currentY)) + 1.0f;
        const Color fill{0.35f, 0.6f, 1.0f, 0.18f};
        const Color edge{0.55f, 0.75f, 1.0f, 0.9f};
        drawList.DrawScreenRect(x0, y0, x1 - x0, y1 - y0, fill);
        drawList.DrawScreenRect(x0, y0, x1 - x0, 1.0f, edge);
        drawList.DrawScreenRect(x0, y1 - 1.0f, x1 - x0, 1.0f, edge);
        drawList.DrawScreenRect(x0, y0, 1.0f, y1 - y0, edge);
        drawList.DrawScreenRect(x1 - 1.0f, y0, 1.0f, y1 - y0, edge);
    }

    void SelectTransformTool::OnDeactivate()
    {
        // Gesture-end guarantee: a pointer-less update finishes/aborts any in-flight drag so no
        // half-applied command group survives a tool switch.
        GizmoFrameInput gizmoInput;
        gizmoInput.pointerValid = false;
        (void)m_gizmos.Update(gizmoInput);
        m_marquee.armed = false; // a marquee mid-drag is dropped, never applied
        m_marquee.active = false;
    }

    void SelectTransformTool::PickOnClick(const ViewportToolInput& input)
    {
        const Guid cpu = CpuPick(input);
        const bool pixelValid = input.viewportWidth > 0 && input.viewportHeight > 0 &&
                                input.pointerX >= 0 && input.pointerY >= 0 &&
                                static_cast<u32>(input.pointerX) < input.viewportWidth &&
                                static_cast<u32>(input.pointerY) < input.viewportHeight;
        if (m_picker != nullptr && pixelValid)
        {
            const u32 request = m_picker->RequestPick(input.pointerX, input.pointerY, 1, 1);
            if (request != 0)
            {
                // The newest click wins: an older answer still in flight is ignored when it lands.
                m_pendingPick = request;
                m_pendingCpuPick = cpu;
                m_pendingCtrl = input.ctrl;
                return;
            }
        }
        ApplyPick(cpu, input.ctrl);
    }

    void SelectTransformTool::PollPick()
    {
        if (m_picker == nullptr)
        {
            return;
        }
        if (m_pendingMarquee != 0)
        {
            Array<scene::EntityHandle> hits;
            if (m_picker->TryTakePick(m_pendingMarquee, hits))
            {
                m_pendingMarquee = 0;
                scene::Scene& scene = m_edit->Scene();
                Array<Guid> picked;
                for (const scene::EntityHandle& h : hits)
                {
                    if (scene.IsValid(h))
                    {
                        picked.PushBack(scene.GetEntityId(h));
                    }
                }
                ApplyMarquee(Span<const Guid>{picked.Data(), picked.Size()}, m_marqueeCtrl);
            }
        }
        if (m_pendingPick == 0)
        {
            return;
        }
        Array<scene::EntityHandle> hits;
        if (!m_picker->TryTakePick(m_pendingPick, hits))
        {
            return;
        }
        m_pendingPick = 0;
        scene::Scene& scene = m_edit->Scene();
        Guid picked = m_pendingCpuPick; // the GPU saw nothing drawn there: the CPU answer stands
        for (const scene::EntityHandle& h : hits)
        {
            if (scene.IsValid(h))
            {
                picked = scene.GetEntityId(h);
                break;
            }
        }
        ApplyPick(picked, m_pendingCtrl);
    }

    void SelectTransformTool::ApplyPick(const Guid& picked, bool ctrl)
    {
        Selection<Guid>& selection = m_edit->EntitySelection();
        if (picked != Guid{})
        {
            if (ctrl)
            {
                selection.Toggle(picked);
            }
            else
            {
                selection.Set(picked);
            }
        }
        else if (!ctrl)
        {
            selection.Clear();
        }
    }

    Guid SelectTransformTool::CpuPick(const ViewportToolInput& input) const
    {
        scene::Scene& scene = m_edit->Scene();
        const Float3 origin = input.ray.origin;
        const Float3 dir = input.ray.direction;

        Guid best;
        f32 bestT = kFloatMax;
        scene.ForEachEntity(
            [&](scene::EntityHandle e)
            {
                const Float4x4 world = scene.GetWorldMatrix(e);
                const Float3 p{world.m[3][0], world.m[3][1], world.m[3][2]};
                const Float3 toCenter = p - origin;
                const f32 t = Dot(toCenter, dir);
                if (t <= 0.0f || t >= bestT)
                {
                    return;
                }
                const Float3 closest = origin + dir * t;
                const Float3 d = p - closest;
                // Screen-constant-ish pick radius: grows with distance, floors for close-ups.
                const f32 radius = Max(0.15f, t * 0.02f);
                if (Dot(d, d) <= radius * radius)
                {
                    bestT = t;
                    best = scene.GetEntityId(e);
                }
            });
        return best;
    }
}
