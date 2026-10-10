// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Camera - `editor.camera`: the free-fly viewport camera, graduated out of Editor::Scene
// into its own LEAN module (foundation-only) so every editor page lib can share it: the scene
// pages, Editor.Physics, and PreviewViewport (editor.preview) alike. Kept separate from the heavy
// editor.preview interface so widely-importing pages never pull the render/scene/viewport graph
// (a GCC module-merge ICE when ScenePage imported the combined interface).
//
// EditorCamera: the scene page's free-fly viewport camera - the samples' FlyCamera
// (Code/Samples/Common/FlyCamera.h) adopted into the editor with editor-scale defaults.
// WASD/QE move (Shift fast), RMB free-look, Alt+LMB turntable orbit about the focus point,
// MMB pan, wheel dolly. Driven from a ViewportView's GATED devices so it only moves while its
// own viewport is hovered/focused (each scene page owns one - multi-scene rule, never global).

module;
#include "Core/Prelude.h"

export module editor.camera;

import foundation.core;
import foundation.shell;

using namespace foundation::core;

export namespace editor
{
    struct EditorCamera
    {
        Float3 position{6.0f, 5.0f, 10.0f};
        f32 yaw = 0.54f; // 0 => looking down -Z; defaults aim at the origin (see LookAt)
        f32 pitch = -0.41f;
        bool mouseCaptured = false;
        f32 moveSpeed = 8.0f, fastSpeed = 30.0f, lookSensitivity = 0.003f;
        f32 zoomFraction = 0.12f;  // fraction of the pivot distance per wheel notch
        f32 focusDistance = 12.0f; // pivot distance ahead (Alt+LMB turntable orbit)
        f32 panSensitivity = 0.0015f;
        bool zoomedThisUpdate = false; // the last Update dollied on the wheel (the zoom readout)
        f32 glideSeconds = 0.3f;       // a frame eases over this long (0 jumps)

        [[nodiscard]] Quaternion Rotation() const
        {
            return Quaternion::FromAxisAngle(Float3{0.0f, 1.0f, 0.0f}, yaw) *
                   Quaternion::FromAxisAngle(Float3{1.0f, 0.0f, 0.0f}, pitch);
        }
        [[nodiscard]] Float3 Forward() const
        {
            return RotateVector(Rotation(), Float3{0.0f, 0.0f, -1.0f});
        }
        [[nodiscard]] Float3 Right() const
        {
            return RotateVector(Rotation(), Float3{1.0f, 0.0f, 0.0f});
        }
        [[nodiscard]] Float3 Up() const
        {
            return RotateVector(Rotation(), Float3{0.0f, 1.0f, 0.0f});
        }

        /// Aim at `target` from the current position: solves yaw/pitch (no roll, so the horizon
        /// stays level) and moves the orbit pivot there (focusDistance). Also the future
        /// frame-selection seam.
        void LookAt(Float3 target)
        {
            const Float3 delta = target - position;
            const f32 length = Sqrt(Dot(delta, delta));
            if (length < 0.0001f)
            {
                return;
            }
            const Float3 dir = delta * (1.0f / length);
            // forward = (-cos(pitch)*sin(yaw), sin(pitch), -cos(pitch)*cos(yaw))
            pitch = Asin(Clamp(dir.y, -1.0f, 1.0f));
            yaw = Atan2(-dir.x, -dir.z);
            focusDistance = length;
        }

        /// Frame a bounding sphere in view: place the camera at a 3/4 vantage far enough that a
        /// sphere of `radius` about `center` fits, looking at the center. The shared "frame the
        /// object" helper for preview viewports - each page supplies bounds for its own content
        /// (mesh bounds, collision-outline extents, skeleton reach, ...). Matches the framing the
        /// mesh preview uses (a slightly high 3/4 view, distance ~2.8x the radius).
        void FrameBounds(Float3 center, f32 radius)
        {
            radius = Max(0.25f, radius);
            position = center + Float3{0.0f, 0.4f, 1.0f} * (radius * 2.6f);
            LookAt(center);
        }

        /// Frame a sphere in a view of vertical field of view `fovY` (radians): keep the view
        /// direction, stand back far enough that the sphere fits with a margin, and put the orbit
        /// pivot on its centre, so Alt+drag orbits it afterwards. Eases there over glideSeconds
        /// when `ease` (Advance moves the glide on); any input that moves the camera stops the
        /// glide where it is. The scene view's frame-the-selection.
        void FrameSphere(Float3 center, f32 radius, f32 fovY, bool ease = true)
        {
            radius = Max(0.25f, radius);
            const f32 distance = 1.1f * radius / Max(0.05f, Sin(0.5f * fovY));
            const Float3 target = center - Forward() * distance;
            if (!ease || glideSeconds <= 0.0f)
            {
                position = target;
                focusDistance = distance;
                m_gliding = false;
                return;
            }
            m_glideFrom = position;
            m_glideFromFocus = focusDistance;
            m_glideTo = target;
            m_glideToFocus = distance;
            m_glideElapsed = 0.0f;
            m_gliding = true;
        }

        /// Moves a FrameSphere glide on by `dt`, eased in and out; a no-op when none runs. Called
        /// every frame whether or not the viewport has the input (a frame from the hierarchy
        /// glides while the mouse is over the hierarchy).
        void Advance(f32 dt)
        {
            if (!m_gliding)
            {
                return;
            }
            m_glideElapsed += dt;
            f32 t = Clamp(m_glideElapsed / glideSeconds, 0.0f, 1.0f);
            const bool done = t >= 1.0f;
            t = t * t * (3.0f - 2.0f * t);
            position = m_glideFrom + (m_glideTo - m_glideFrom) * t;
            focusDistance = m_glideFromFocus + (m_glideToFocus - m_glideFromFocus) * t;
            m_gliding = !done;
        }

        [[nodiscard]] bool IsGliding() const noexcept { return m_gliding; }

        /// Force-exit Tab-capture, restoring the OS cursor. The I2 stuck-mouse bug: Update()
        /// (the only place Tab toggles capture OFF) runs only while the viewport is
        /// hovered/focused - but relative mode makes LOSING that state easy, wedging the OS
        /// grab forever. The page calls this whenever the viewport is inactive or closing, so
        /// capture structurally cannot outlive the ability to release it.
        void ReleaseCapture(foundation::shell::IMouse* mouse)
        {
            if (!mouseCaptured)
            {
                return;
            }
            mouseCaptured = false;
            if (mouse != nullptr)
            {
                mouse->SetRelativeMode(false);
                mouse->SetCursorVisible(true);
            }
        }

        // Apply this frame's input from explicit (gated) devices.
        /// `allowZoom` is false while a modal viewport tool owns the wheel (a brush resizes on
        /// Shift + wheel): the first-consumer rule keeps that scroll from ALSO dollying the camera.
        void Update(foundation::shell::IKeyboard* kb, foundation::shell::IMouse* mouse, f32 dt,
                    bool allowZoom = true)
        {
            namespace shell = foundation::shell;
            zoomedThisUpdate = false;
            if (kb == nullptr)
            {
                return;
            }

            if (mouse != nullptr)
            {
                if (kb->IsKeyPressed(shell::KeyCode::Tab))
                {
                    mouseCaptured = !mouseCaptured;
                    mouse->SetRelativeMode(mouseCaptured);
                    mouse->SetCursorVisible(!mouseCaptured);
                }
                if (mouseCaptured && kb->IsKeyPressed(shell::KeyCode::Escape))
                {
                    ReleaseCapture(mouse); // in-flight escape hatch (I2)
                }
                const bool alt = kb->IsKeyDown(shell::KeyCode::LeftAlt) ||
                                 kb->IsKeyDown(shell::KeyCode::RightAlt);

                if (alt && mouse->IsButtonDown(shell::MouseButton::Left))
                {
                    m_gliding = false; // the user takes the camera
                    // Turntable orbit: rotate about the focus point ahead, keeping it fixed.
                    const Float3 focus = position + Forward() * focusDistance;
                    yaw -= mouse->DeltaX() * lookSensitivity;
                    pitch -= mouse->DeltaY() * lookSensitivity;
                    pitch = Clamp(pitch, -1.55f, 1.55f);
                    position = focus - Forward() * focusDistance;
                }
                else if (mouseCaptured || mouse->IsButtonDown(shell::MouseButton::Right))
                {
                    m_gliding = false;
                    yaw -= mouse->DeltaX() * lookSensitivity;
                    pitch -= mouse->DeltaY() * lookSensitivity;
                    pitch = Clamp(pitch, -1.55f, 1.55f);
                }

                if (mouse->IsButtonDown(shell::MouseButton::Middle))
                {
                    m_gliding = false;
                    const f32 s = panSensitivity * focusDistance;
                    position =
                        position - Right() * (mouse->DeltaX() * s) + Up() * (mouse->DeltaY() * s);
                }

                const f32 scroll = allowZoom ? mouse->ScrollY() : 0.0f;
                if (scroll != 0.0f)
                {
                    m_gliding = false;
                    // Exponential dolly toward the orbit pivot: the step scales with the
                    // pivot distance, so zoom feels the same on a 100-unit scene and a
                    // 0.5-unit mesh, and the camera approaches but never crosses the
                    // pivot - Alt+LMB stays a turntable at ANY zoom. (The old fixed-step
                    // dolly clamped focusDistance at 1 and dragged the pivot along with
                    // the camera, which turned orbit into head-turning on small meshes.)
                    const Float3 focus = position + Forward() * focusDistance;
                    const f32 factor = Clamp(1.0f - zoomFraction * scroll, 0.2f, 5.0f);
                    focusDistance = Max(0.05f, focusDistance * factor);
                    position = focus - Forward() * focusDistance;
                    zoomedThisUpdate = true;
                }
            }

            // WASD/QE fly ONLY while the camera owns the input - RMB held or Tab-captured
            // (the editor convention: with RMB up, W/E/R/X belong to the gizmo shortcuts).
            const bool flying = mouseCaptured || (mouse != nullptr &&
                                                  mouse->IsButtonDown(shell::MouseButton::Right));
            if (!flying)
            {
                return;
            }

            const Float3 fwd = Forward();
            const Float3 right = Right();
            const f32 speed =
                (kb->IsKeyDown(shell::KeyCode::LeftShift) ? fastSpeed : moveSpeed) * dt;
            Float3 move{0.0f, 0.0f, 0.0f};
            if (kb->IsKeyDown(shell::KeyCode::W))
            {
                move = move + fwd;
            }
            if (kb->IsKeyDown(shell::KeyCode::S))
            {
                move = move - fwd;
            }
            if (kb->IsKeyDown(shell::KeyCode::D))
            {
                move = move + right;
            }
            if (kb->IsKeyDown(shell::KeyCode::A))
            {
                move = move - right;
            }
            if (kb->IsKeyDown(shell::KeyCode::E))
            {
                move = move + Float3{0.0f, 1.0f, 0.0f};
            }
            if (kb->IsKeyDown(shell::KeyCode::Q))
            {
                move = move - Float3{0.0f, 1.0f, 0.0f};
            }
            if (Dot(move, move) > 0.0f)
            {
                position = position + Normalized(move) * speed;
            }
        }

    private:
        bool m_gliding = false;
        f32 m_glideElapsed = 0.0f;
        Float3 m_glideFrom{};
        Float3 m_glideTo{};
        f32 m_glideFromFocus = 0.0f;
        f32 m_glideToFocus = 0.0f;
    };
}
