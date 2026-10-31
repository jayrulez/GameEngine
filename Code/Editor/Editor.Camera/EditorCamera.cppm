// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Camera: `editor.camera`, the free-fly viewport camera, in a lean module of its own
// (foundation only) so every editor page library can share it: the scene pages, Editor.Physics and
// PreviewViewport (editor.preview). It is kept out of the heavy editor.preview interface so pages
// that import it never pull in the render, scene and viewport graph (GCC hit an internal compiler
// error merging modules when ScenePage imported the combined interface).
//
// EditorCamera: the scene page's free-fly viewport camera, the samples' FlyCamera
// (Code/Samples/Common/FlyCamera.h) with editor-scale defaults. WASD and QE move (Shift for fast),
// the right mouse button looks around, Alt with the left button orbits about the focus point, the
// middle button pans and the wheel dollies. It is driven from a ViewportView's gated devices, so
// it moves only while its own viewport is hovered or focused; each scene page owns one, never a
// global one.

module;
#include "Core/Prelude.h"

export module editor.camera;

import foundation.core;
import foundation.shell;

using namespace foundation::core;

export namespace editor
{
    /// EditorCamera::kStraightPitch, at namespace scope so another library can read it (MSVC does not
    /// export a class's static constexpr member from a shared library).
    inline constexpr f32 kEditorCameraStraightPitch = 1.5707963f;

    struct EditorCamera
    {
        Float3 position{6.0f, 5.0f, 10.0f};
        f32 yaw = 0.54f; // 0 looks down -Z; the defaults aim at the origin (see LookAt)
        f32 pitch = -0.41f;
        bool mouseCaptured = false;
        f32 moveSpeed = 8.0f, fastSpeed = 30.0f, lookSensitivity = 0.003f;
        f32 zoomFraction = 0.12f;  // fraction of the pivot distance per wheel notch
        f32 focusDistance = 12.0f; // pivot distance ahead (Alt+LMB turntable orbit)
        f32 panSensitivity = 0.0015f;
        bool zoomedThisUpdate = false; // the last Update dollied on the wheel (the zoom readout)
        f32 glideSeconds = 0.3f;       // framing eases over this long (0 jumps)
        f32 fovY = 1.0472f;            // the vertical field of view, radians (60 degrees)
        f32 nearZ = 0.1f, farZ = 1000.0f;
        // Orthographic: the view keeps the camera's eye, pivot and framing, and draws parallel.
        // Its height is what the field of view spans at the pivot (OrthoHeight), so zoom, frame,
        // pan and orbit work as they do in perspective and switching keeps what is in view.
        bool orthographic = false;
        // Snapping to an axis view (SnapToAxis) switches to orthographic, and orbiting away switches
        // back. This is the editor's preference, set by the page.
        bool autoOrthographic = true;

        /// An orthographic view's height in world units: what the field of view spans at the
        /// pivot, so it follows the zoom (focusDistance).
        [[nodiscard]] f32 OrthoHeight() const
        {
            return 2.0f * focusDistance * Tan(0.5f * fovY);
        }

        /// The projection for a view `aspect` wide over tall, perspective or orthographic.
        [[nodiscard]] Float4x4 Projection(f32 aspect) const
        {
            if (orthographic)
            {
                const f32 height = OrthoHeight();
                return Float4x4::OrthographicRH(height * aspect, height, nearZ, DrawFar());
            }
            return Float4x4::PerspectiveFovRH(fovY, aspect, nearZ, farZ);
        }

        /// How far behind the eye the view draws from: one view height under orthographic, none in
        /// perspective. In a view tipped down, an orthographic volume starting at the eye's plane
        /// would cut away the ground below the eye; drawing from further back along the view changes
        /// nothing on screen except what is clipped. One view height shows that ground at the usual
        /// tilts and follows the zoom, while anything further behind the eye stays clipped (otherwise
        /// a wall far behind it would cover the view). The eye itself stays put, so whatever is
        /// measured from it (orbit, zoom, the renderer's distance fades) behaves as in perspective.
        [[nodiscard]] f32 DrawPullBack() const { return orthographic ? OrthoHeight() : 0.0f; }
        /// Where the view draws from: the eye, or under orthographic the point pulled back along
        /// the view behind it (the view matrix's origin; the eye stays `position`).
        [[nodiscard]] Float3 DrawEye() const { return position - Forward() * DrawPullBack(); }
        /// The far plane measured from the draw eye, so it reaches as far past the eye as in
        /// perspective.
        [[nodiscard]] f32 DrawFar() const { return farZ + DrawPullBack(); }

        /// Switch between perspective and orthographic by hand (the toggle), keeping what is in view.
        void SetOrthographic(bool on)
        {
            orthographic = on;
            m_orthographicFromSnap = false;
        }

        /// The world ray through a viewport point given in NDC (x right, y up, each -1..1) for a
        /// view `aspect` wide over tall: from the eye fanning out in perspective; parallel to the
        /// view under orthographic, starting on the draw eye's plane (DrawEye).
        void RayThrough(f32 ndcX, f32 ndcY, f32 aspect, Float3& origin, Float3& direction) const
        {
            const f32 tanY = Tan(0.5f * fovY);
            if (orthographic)
            {
                // From the draw eye's plane, so what lies behind the eye's own plane is picked too.
                const f32 halfH = 0.5f * OrthoHeight();
                origin = DrawEye() + Right() * (ndcX * halfH * aspect) + Up() * (ndcY * halfH);
                direction = Forward();
                return;
            }
            origin = position;
            direction = Normalized(Forward() + Right() * (ndcX * tanY * aspect) + Up() * (ndcY * tanY));
        }

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
        /// stays level) and moves the orbit pivot there (focusDistance).
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
        /// sphere of `radius` about `center` fits, looking at the center. This is the shared framing
        /// helper for preview viewports; each page supplies bounds for its own content (mesh bounds,
        /// collision outline extents, skeleton reach). It matches the mesh preview's framing (a
        /// slightly high 3/4 view at about 2.8 times the radius).
        void FrameBounds(Float3 center, f32 radius)
        {
            radius = Max(0.25f, radius);
            position = center + Float3{0.0f, 0.4f, 1.0f} * (radius * 2.6f);
            LookAt(center);
        }

        /// Turn to `newYaw` / `newPitch` about the orbit pivot: what the camera looks at stays put
        /// and the camera swings round it. Stops a glide. A pitch may reach straight up or down
        /// (a top view), past the mouse-look limit.
        void TurnAboutPivot(f32 newYaw, f32 newPitch)
        {
            const Float3 pivot = position + Forward() * focusDistance;
            yaw = newYaw;
            pitch = Clamp(newPitch, -kStraightPitch, kStraightPitch);
            position = pivot - Forward() * focusDistance;
            m_gliding = false;
        }

        /// The orientation gizmo's snap to an axis view: TurnAboutPivot, and with
        /// autoOrthographic, orthographic until the camera is orbited or turned away.
        void SnapToAxis(f32 newYaw, f32 newPitch)
        {
            TurnAboutPivot(newYaw, newPitch);
            if (autoOrthographic && !orthographic)
            {
                orthographic = true;
                m_orthographicFromSnap = true;
            }
        }

        /// Whether the view is orthographic because a snap made it so (and turning away undoes it).
        [[nodiscard]] bool IsOrthographicFromSnap() const noexcept { return m_orthographicFromSnap; }

        static constexpr f32 kStraightPitch = kEditorCameraStraightPitch; // straight down or up
        static constexpr f32 kLookPitch = 1.55f;          // the mouse-look limit

        /// Frame a sphere in a view of vertical field of view `viewFovY` (radians): keep the view
        /// direction, stand back far enough that the sphere fits with a margin, and put the orbit
        /// pivot on its centre, so Alt+drag orbits it afterwards. Eases there over glideSeconds
        /// when `ease` (Advance moves the glide on); any input that moves the camera stops the
        /// glide where it is. The scene view frames the selection with this.
        void FrameSphere(Float3 center, f32 radius, f32 viewFovY, bool ease = true)
        {
            radius = Max(0.25f, radius);
            const f32 distance = 1.1f * radius / Max(0.05f, Sin(0.5f * viewFovY));
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

        /// Whether the last Update's mouse drags the camera (an Alt+LMB orbit, an RMB look, an MMB
        /// pan): its viewport asks for the pointer locked meanwhile (InputSurface::SetPointerLock),
        /// so the drag never stops at the screen's edge. False while Tab holds the mouse, which
        /// already locks it.
        [[nodiscard]] bool WantsPointerLock() const noexcept { return m_wantsLock; }

        /// Force-exit Tab capture, restoring the OS cursor. Update() is the only place Tab turns
        /// capture off, and it runs only while the viewport is hovered or focused; relative mode
        /// makes losing that state easy, which would leave the OS grab stuck. The page calls this
        /// whenever the viewport is inactive or closing, so capture cannot outlive the ability to
        /// release it.
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

        /// Applies this frame's input from explicit (gated) devices.
        /// `allowZoom` is false while a modal viewport tool owns the wheel (a brush resizes on
        /// Shift and the wheel): the first consumer takes the scroll, so it does not also dolly the
        /// camera.
        /// `kb` is null while the view has no keyboard focus: the mouse still works it (the wheel
        /// over a view that was not clicked into zooms it), the keys wait for the focus.
        void Update(foundation::shell::IKeyboard* kb, foundation::shell::IMouse* mouse, f32 dt,
                    bool allowZoom = true)
        {
            namespace shell = foundation::shell;
            zoomedThisUpdate = false;
            m_wantsLock = false;
            const auto down = [kb](shell::KeyCode key) { return kb != nullptr && kb->IsKeyDown(key); };
            const auto pressed = [kb](shell::KeyCode key) { return kb != nullptr && kb->IsKeyPressed(key); };

            if (mouse != nullptr)
            {
                if (pressed(shell::KeyCode::Tab))
                {
                    mouseCaptured = !mouseCaptured;
                    mouse->SetRelativeMode(mouseCaptured);
                    mouse->SetCursorVisible(!mouseCaptured);
                }
                if (mouseCaptured && pressed(shell::KeyCode::Escape))
                {
                    ReleaseCapture(mouse); // Escape releases a capture mid-flight
                }
                const bool alt = down(shell::KeyCode::LeftAlt) || down(shell::KeyCode::RightAlt);
                m_wantsLock = !mouseCaptured && ((alt && mouse->IsButtonDown(shell::MouseButton::Left)) ||
                                                 mouse->IsButtonDown(shell::MouseButton::Right) ||
                                                 mouse->IsButtonDown(shell::MouseButton::Middle));

                if (alt && mouse->IsButtonDown(shell::MouseButton::Left))
                {
                    m_gliding = false; // the user takes the camera
                    // Turntable orbit: rotate about the focus point ahead, keeping it fixed.
                    const Float3 focus = position + Forward() * focusDistance;
                    yaw -= mouse->DeltaX() * lookSensitivity;
                    pitch -= mouse->DeltaY() * lookSensitivity;
                    pitch = Clamp(pitch, -kLookPitch, kLookPitch);
                    position = focus - Forward() * focusDistance;
                    LeaveSnapView(mouse);
                }
                else if (mouseCaptured || mouse->IsButtonDown(shell::MouseButton::Right))
                {
                    m_gliding = false;
                    yaw -= mouse->DeltaX() * lookSensitivity;
                    pitch -= mouse->DeltaY() * lookSensitivity;
                    pitch = Clamp(pitch, -kLookPitch, kLookPitch);
                    LeaveSnapView(mouse);
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
                    // pivot, so Alt and the left button stay a turntable orbit at any zoom.
                    // A fixed step would have to clamp the distance and drag the pivot along,
                    // which turns orbit into head-turning on small meshes.
                    const Float3 focus = position + Forward() * focusDistance;
                    const f32 factor = Clamp(1.0f - zoomFraction * scroll, 0.2f, 5.0f);
                    focusDistance = Max(0.05f, focusDistance * factor);
                    position = focus - Forward() * focusDistance;
                    zoomedThisUpdate = true;
                }
            }

            // WASD and QE fly only while the camera owns the input: the right button held, or Tab
            // capture. With the right button up, W/E/R/X belong to the gizmo shortcuts.
            const bool flying = mouseCaptured || (mouse != nullptr &&
                                                  mouse->IsButtonDown(shell::MouseButton::Right));
            if (!flying || kb == nullptr)
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
        // Turning with the mouse leaves an axis view: orthographic from a snap goes back to
        // perspective.
        void LeaveSnapView(foundation::shell::IMouse* mouse)
        {
            if (m_orthographicFromSnap && (mouse->DeltaX() != 0.0f || mouse->DeltaY() != 0.0f))
            {
                orthographic = false;
                m_orthographicFromSnap = false;
            }
        }

        bool m_orthographicFromSnap = false;
        bool m_gliding = false;
        bool m_wantsLock = false; // the last Update's mouse drags the camera
        f32 m_glideElapsed = 0.0f;
        Float3 m_glideFrom{};
        Float3 m_glideTo{};
        f32 m_glideFromFocus = 0.0f;
        f32 m_glideToFocus = 0.0f;
    };
}
