#pragma once
// Shared free-fly camera for the dev samples: WASD/QE move, hold RMB (or Tab to capture) to look,
// Shift to move fast. Provides position + orientation; the sample consumes it as it likes (set a
// camera-entity transform, or build a ViewCamera). The including translation unit must have
// imported Draconic.Foundation + draconic.shell + draconic.runtime.client (this header uses their
// types via fully-qualified names).

namespace draconic::samples
{

    struct FlyCamera
    {
        draconic::foundation::Float3 position{0.0f, 14.0f, 30.0f};
        draconic::foundation::f32 yaw = 0.0f;    // 0 => looking down -Z
        draconic::foundation::f32 pitch = -0.3f; // tilt down a touch
        bool mouseCaptured = false;
        draconic::foundation::f32 moveSpeed = 50.0f, fastSpeed = 200.0f, lookSensitivity = 0.003f;
        draconic::foundation::f32 zoomSpeed = 3.0f; // world units per wheel notch (dolly along forward)
        draconic::foundation::f32 focusDistance = 20.0f; // pivot distance ahead (Alt+LMB turntable orbit)
        draconic::foundation::f32 panSensitivity = 0.0015f; // MMB pan speed (scaled by focus distance)

        [[nodiscard]] draconic::foundation::Float3 Up() const
        {
            return draconic::foundation::RotateVector(Rotation(),
                                                draconic::foundation::Float3{0.0f, 1.0f, 0.0f});
        }

        // Orientation as a quaternion (yaw about world Y, then pitch about local X). Default forward -Z.
        [[nodiscard]] draconic::foundation::Quaternion Rotation() const
        {
            return draconic::foundation::Quaternion::FromAxisAngle(
                       draconic::foundation::Float3{0.0f, 1.0f, 0.0f}, yaw) *
                   draconic::foundation::Quaternion::FromAxisAngle(
                       draconic::foundation::Float3{1.0f, 0.0f, 0.0f}, pitch);
        }
        [[nodiscard]] draconic::foundation::Float3 Forward() const
        {
            return draconic::foundation::RotateVector(Rotation(),
                                                draconic::foundation::Float3{0.0f, 0.0f, -1.0f});
        }
        [[nodiscard]] draconic::foundation::Float3 Right() const
        {
            return draconic::foundation::RotateVector(Rotation(),
                                                draconic::foundation::Float3{1.0f, 0.0f, 0.0f});
        }

        // Convenience overload: drive from the shell's global devices.
        void Update(draconic::runtime::IApplicationHost& host, draconic::foundation::f32 dt)
        {
            namespace runtime = draconic::runtime;
            namespace shell = draconic::shell;
            auto* input = (host.Shell() != nullptr) ? host.Shell()->Input() : nullptr;
            shell::IKeyboard* kb = (input != nullptr) ? input->Keyboard() : nullptr;
            shell::IMouse* mouse = (input != nullptr) ? input->Mouse() : nullptr;
            Update(kb, mouse, dt);
        }

        // Apply this frame's input from explicit devices - pass an InputSurface's gated Keyboard()/Mouse()
        // to confine the camera to one viewport. Mouse: RMB (or Tab-capture) = free look; Alt+LMB = turntable
        // orbit about the focus point (Maya-style); MMB = pan; wheel = dolly/zoom. Plus WASD/QE move + Shift.
        void Update(draconic::shell::IKeyboard* kb, draconic::shell::IMouse* mouse,
                    draconic::foundation::f32 dt)
        {
            using draconic::foundation::Float3;
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
                    pitch = draconic::foundation::Clamp(pitch, -1.55f, 1.55f);
                    position = focus - Forward() * focusDistance;
                }
                else if (mouseCaptured || mouse->IsButtonDown(shell::MouseButton::Right))
                {
                    // Free look (rotate in place).
                    yaw -= mouse->DeltaX() * lookSensitivity;
                    pitch -= mouse->DeltaY() * lookSensitivity;
                    pitch = draconic::foundation::Clamp(pitch, -1.55f, 1.55f);
                }

                // MMB pan: drag moves the view laterally (content follows the cursor). Scaled by the focus
                // distance so the pan feels consistent regardless of zoom.
                if (mouse->IsButtonDown(shell::MouseButton::Middle))
                {
                    const draconic::foundation::f32 s = panSensitivity * focusDistance;
                    position =
                        position - Right() * (mouse->DeltaX() * s) + Up() * (mouse->DeltaY() * s);
                }

                // Wheel dollies along the view forward (zoom) - scroll up = move in, down = move out - and
                // shrinks the orbit pivot distance so the turntable pivot tracks the zoom.
                const draconic::foundation::f32 scroll = mouse->ScrollY();
                if (scroll != 0.0f)
                {
                    position = position + Forward() * (scroll * zoomSpeed);
                    focusDistance = draconic::foundation::Max(1.0f, focusDistance - scroll * zoomSpeed);
                }
            }

            const Float3 fwd = Forward();
            const Float3 right = Right();
            const draconic::foundation::f32 speed =
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
            if (draconic::foundation::Dot(move, move) > 0.0f)
            {
                position = position + draconic::foundation::Normalized(move) * speed;
            }
        }
    };

} // namespace draconic::samples
