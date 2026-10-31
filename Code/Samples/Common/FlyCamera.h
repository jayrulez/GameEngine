// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

#pragma once
// Shared free-fly camera for the dev samples: WASD/QE move, hold RMB (or Tab to capture) to look,
// Shift to move fast. Provides position and orientation; the sample consumes it as it likes (set a
// camera-entity transform, or build a ViewCamera). The including translation unit must have
// imported foundation.core, foundation.shell and foundation.runtime.client (this header uses their
// types via fully-qualified names).

namespace samples
{
    namespace core = foundation::core;
    namespace shell = foundation::shell;
    namespace runtime = foundation::runtime;

    struct FlyCamera
    {
        foundation::core::Float3 position{0.0f, 14.0f, 30.0f};
        foundation::core::f32 yaw = 0.0f;    // 0 looks down -Z
        foundation::core::f32 pitch = -0.3f; // tilt down a touch
        bool mouseCaptured = false;
        foundation::core::f32 moveSpeed = 50.0f, fastSpeed = 200.0f, lookSensitivity = 0.003f;
        foundation::core::f32 zoomSpeed = 3.0f; // world units per wheel notch (dolly along forward)
        foundation::core::f32 focusDistance = 20.0f; // pivot distance ahead (Alt+LMB turntable orbit)
        foundation::core::f32 panSensitivity = 0.0015f; // MMB pan speed (scaled by focus distance)

        [[nodiscard]] foundation::core::Float3 Up() const
        {
            return foundation::core::RotateVector(Rotation(),
                                                foundation::core::Float3{0.0f, 1.0f, 0.0f});
        }

        // Orientation as a quaternion (yaw about world Y, then pitch about local X). Default forward -Z.
        [[nodiscard]] foundation::core::Quaternion Rotation() const
        {
            return foundation::core::Quaternion::FromAxisAngle(
                       foundation::core::Float3{0.0f, 1.0f, 0.0f}, yaw) *
                   foundation::core::Quaternion::FromAxisAngle(
                       foundation::core::Float3{1.0f, 0.0f, 0.0f}, pitch);
        }
        [[nodiscard]] foundation::core::Float3 Forward() const
        {
            return foundation::core::RotateVector(Rotation(),
                                                foundation::core::Float3{0.0f, 0.0f, -1.0f});
        }
        [[nodiscard]] foundation::core::Float3 Right() const
        {
            return foundation::core::RotateVector(Rotation(),
                                                foundation::core::Float3{1.0f, 0.0f, 0.0f});
        }

        // Convenience overload: drive from the shell's global devices.
        void Update(foundation::runtime::IApplicationHost& host, foundation::core::f32 dt)
        {
            auto* input = (host.Shell() != nullptr) ? host.Shell()->Input() : nullptr;
            shell::IKeyboard* kb = (input != nullptr) ? input->Keyboard() : nullptr;
            shell::IMouse* mouse = (input != nullptr) ? input->Mouse() : nullptr;
            Update(kb, mouse, dt);
        }

        // Apply this frame's input from explicit devices. Pass an InputSurface's gated Keyboard()/Mouse()
        // to confine the camera to one viewport. RMB (or Tab capture) looks around freely; Alt+LMB orbits
        // the focus point like a turntable (Maya-style); MMB pans; the wheel dollies. WASD/QE move, and
        // Shift moves fast.
        void Update(foundation::shell::IKeyboard* kb, foundation::shell::IMouse* mouse,
                    foundation::core::f32 dt)
        {
            using foundation::core::Float3;
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
                    pitch = foundation::core::Clamp(pitch, -1.55f, 1.55f);
                    position = focus - Forward() * focusDistance;
                }
                else if (mouseCaptured || mouse->IsButtonDown(shell::MouseButton::Right))
                {
                    // Free look (rotate in place).
                    yaw -= mouse->DeltaX() * lookSensitivity;
                    pitch -= mouse->DeltaY() * lookSensitivity;
                    pitch = foundation::core::Clamp(pitch, -1.55f, 1.55f);
                }

                // MMB pan: drag moves the view laterally (content follows the cursor). Scaled by the focus
                // distance so the pan feels consistent regardless of zoom.
                if (mouse->IsButtonDown(shell::MouseButton::Middle))
                {
                    const foundation::core::f32 s = panSensitivity * focusDistance;
                    position =
                        position - Right() * (mouse->DeltaX() * s) + Up() * (mouse->DeltaY() * s);
                }

                // The wheel dollies along the view's forward axis (scrolling up moves in, down moves out)
                // and shrinks the orbit pivot distance so the turntable pivot tracks the zoom.
                const foundation::core::f32 scroll = mouse->ScrollY();
                if (scroll != 0.0f)
                {
                    position = position + Forward() * (scroll * zoomSpeed);
                    focusDistance = foundation::core::Max(1.0f, focusDistance - scroll * zoomSpeed);
                }
            }

            const Float3 fwd = Forward();
            const Float3 right = Right();
            const foundation::core::f32 speed =
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
            if (foundation::core::Dot(move, move) > 0.0f)
            {
                position = position + foundation::core::Normalized(move) * speed;
            }
        }
    };

} // namespace samples
