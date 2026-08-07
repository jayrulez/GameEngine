// Draconic::EditorScene - :camera partition.
//
// EditorCamera: the scene page's free-fly viewport camera - the samples' FlyCamera
// (Code/Samples/Common/FlyCamera.h) adopted into the editor with editor-scale defaults.
// WASD/QE move (Shift fast), RMB free-look, Alt+LMB turntable orbit about the focus point,
// MMB pan, wheel dolly. Driven from a ViewportView's GATED devices so it only moves while its
// own viewport is hovered/focused (each scene page owns one - multi-scene rule, never global).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.editor.scene:camera;

import draconic.foundation;
import draconic.shell;

using namespace draconic::foundation;

export namespace draconic::editor
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

        // Apply this frame's input from explicit (gated) devices.
        void Update(draconic::shell::IKeyboard* kb, draconic::shell::IMouse* mouse, f32 dt)
        {
            namespace shell = draconic::shell;
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
                const bool alt = kb->IsKeyDown(shell::KeyCode::LeftAlt) ||
                                 kb->IsKeyDown(shell::KeyCode::RightAlt);

                if (alt && mouse->IsButtonDown(shell::MouseButton::Left))
                {
                    // Turntable orbit: rotate about the focus point ahead, keeping it fixed.
                    const Float3 focus = position + Forward() * focusDistance;
                    yaw -= mouse->DeltaX() * lookSensitivity;
                    pitch -= mouse->DeltaY() * lookSensitivity;
                    pitch = Clamp(pitch, -1.55f, 1.55f);
                    position = focus - Forward() * focusDistance;
                }
                else if (mouseCaptured || mouse->IsButtonDown(shell::MouseButton::Right))
                {
                    yaw -= mouse->DeltaX() * lookSensitivity;
                    pitch -= mouse->DeltaY() * lookSensitivity;
                    pitch = Clamp(pitch, -1.55f, 1.55f);
                }

                if (mouse->IsButtonDown(shell::MouseButton::Middle))
                {
                    const f32 s = panSensitivity * focusDistance;
                    position =
                        position - Right() * (mouse->DeltaX() * s) + Up() * (mouse->DeltaY() * s);
                }

                const f32 scroll = mouse->ScrollY();
                if (scroll != 0.0f)
                {
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
    };
}
