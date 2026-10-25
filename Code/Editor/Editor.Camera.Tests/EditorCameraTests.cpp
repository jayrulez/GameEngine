// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// EditorCamera pure-geometry + input-drive tests. EditorCamera is the algorithmic content of
// editor.preview (LookAt solves yaw/pitch with a level horizon; the free-fly/orbit Update maps
// gated devices to motion). PreviewViewport itself is glue over a live IApplicationHost + UIHost
// + render/scene subsystems - not unit-constructible with the codebase's headless precedents (the
// editor pages are not unit-constructed either); it is proven by the six pages built on it + on-screen
// verify.
#include "Core/Prelude.h"
#include <doctest/doctest.h>

import foundation.core;
import foundation.shell;
import editor.camera;

using namespace foundation::core;
namespace shell = foundation::shell;

namespace
{
    bool Near(f32 a, f32 b) { return Abs(a - b) < 1e-3f; }

    // Minimal gated-device stubs: just enough state for the camera's Update to read.
    class StubKeyboard final : public shell::IKeyboard
    {
    public:
        bool wDown = false;
        bool shiftDown = false;
        [[nodiscard]] bool IsKeyDown(shell::KeyCode key) const override
        {
            return (key == shell::KeyCode::W && wDown) ||
                   (key == shell::KeyCode::LeftShift && shiftDown);
        }
        [[nodiscard]] bool IsKeyPressed(shell::KeyCode) const override { return false; }
        [[nodiscard]] bool IsKeyReleased(shell::KeyCode) const override { return false; }
        [[nodiscard]] shell::KeyModifiers Modifiers() const override { return {}; }
    };

    class StubMouse final : public shell::IMouse
    {
    public:
        bool rmb = false;
        f32 dx = 0.0f, dy = 0.0f, scroll = 0.0f;
        int relativeCalls = 0; ///< the camera never sets the window's mouse for a drag itself

        [[nodiscard]] f32 X() const override { return 0.0f; }
        [[nodiscard]] f32 Y() const override { return 0.0f; }
        [[nodiscard]] f32 GlobalX() const override { return 0.0f; }
        [[nodiscard]] f32 GlobalY() const override { return 0.0f; }
        [[nodiscard]] f32 DeltaX() const override { return dx; }
        [[nodiscard]] f32 DeltaY() const override { return dy; }
        [[nodiscard]] f32 ScrollX() const override { return 0.0f; }
        [[nodiscard]] f32 ScrollY() const override { return scroll; }
        [[nodiscard]] bool IsButtonDown(shell::MouseButton button) const override
        {
            return button == shell::MouseButton::Right && rmb;
        }
        [[nodiscard]] bool IsButtonPressed(shell::MouseButton) const override { return false; }
        [[nodiscard]] bool IsButtonReleased(shell::MouseButton) const override { return false; }
        [[nodiscard]] bool RelativeMode() const override { return false; }
        void SetRelativeMode(bool) override { ++relativeCalls; }
        [[nodiscard]] bool CursorVisible() const override { return true; }
        void SetCursorVisible(bool) override {}
        void SetCursor(shell::CursorType) override {}
        void SetGlobalCapture(bool) override {}
    };
}

TEST_CASE("EditorCamera::LookAt aims Forward at the target with a level horizon")
{
    editor::EditorCamera cam;
    cam.position = Float3{4.0f, 3.0f, 6.0f};
    cam.LookAt(Float3{0.0f, 0.0f, 0.0f});

    const Float3 want = Normalized(Float3{0.0f, 0.0f, 0.0f} - cam.position);
    const Float3 fwd = cam.Forward();
    CHECK(Near(fwd.x, want.x));
    CHECK(Near(fwd.y, want.y));
    CHECK(Near(fwd.z, want.z));

    // focusDistance becomes the range to the target; the horizon stays level (no roll).
    CHECK(Near(cam.focusDistance, Sqrt(16.0f + 9.0f + 36.0f)));
    CHECK(Near(cam.Right().y, 0.0f));
}

TEST_CASE("EditorCamera basis is orthonormal")
{
    editor::EditorCamera cam;
    cam.position = Float3{2.0f, -1.0f, 5.0f};
    cam.LookAt(Float3{0.0f, 1.0f, 0.0f});
    const Float3 f = cam.Forward();
    const Float3 r = cam.Right();
    const Float3 u = cam.Up();
    CHECK(Near(Dot(f, f), 1.0f));
    CHECK(Near(Dot(r, r), 1.0f));
    CHECK(Near(Dot(u, u), 1.0f));
    CHECK(Near(Dot(f, r), 0.0f));
    CHECK(Near(Dot(f, u), 0.0f));
    CHECK(Near(Dot(r, u), 0.0f));
}

TEST_CASE("EditorCamera::FrameBounds looks at the center from outside the sphere")
{
    editor::EditorCamera cam;
    const Float3 center{5.0f, 2.0f, -3.0f};
    const f32 radius = 4.0f;
    cam.FrameBounds(center, radius);

    // Camera sits outside the sphere and aims back at its center.
    const f32 dist = Length(cam.position - center);
    CHECK(dist > radius);
    const Float3 toCenter = Normalized(center - cam.position);
    const Float3 fwd = cam.Forward();
    CHECK(Near(fwd.x, toCenter.x));
    CHECK(Near(fwd.y, toCenter.y));
    CHECK(Near(fwd.z, toCenter.z));
    CHECK(Near(cam.focusDistance, dist)); // pivot is the framed center

    // A tiny radius is floored so degenerate content still frames sanely (no zero distance).
    editor::EditorCamera tiny;
    tiny.FrameBounds(Float3{0.0f, 0.0f, 0.0f}, 0.0f);
    CHECK(Length(tiny.position) > 0.1f);
}

TEST_CASE("EditorCamera RMB free-look turns yaw/pitch by mouse delta * sensitivity")
{
    editor::EditorCamera cam;
    const f32 yaw0 = cam.yaw;
    const f32 pitch0 = cam.pitch;

    StubKeyboard kb;
    StubMouse mouse;
    mouse.rmb = true;
    mouse.dx = 100.0f;
    mouse.dy = 50.0f;
    cam.Update(&kb, &mouse, 0.016f);

    CHECK(Near(cam.yaw, yaw0 - 100.0f * cam.lookSensitivity));
    CHECK(Near(cam.pitch, pitch0 - 50.0f * cam.lookSensitivity));
}

TEST_CASE("EditorCamera flies along Forward for W while RMB is held")
{
    editor::EditorCamera cam;
    cam.position = Float3{0.0f, 0.0f, 0.0f};
    const Float3 fwd = cam.Forward();

    StubKeyboard kb;
    kb.wDown = true;
    StubMouse mouse; // RMB held so the camera owns input, but zero delta so look does not move
    mouse.rmb = true;
    cam.Update(&kb, &mouse, 0.5f);

    const Float3 expect = fwd * (cam.moveSpeed * 0.5f);
    CHECK(Near(cam.position.x, expect.x));
    CHECK(Near(cam.position.y, expect.y));
    CHECK(Near(cam.position.z, expect.z));
}

TEST_CASE("EditorCamera does not fly when RMB is up (W belongs to the gizmo shortcuts)")
{
    editor::EditorCamera cam;
    cam.position = Float3{1.0f, 2.0f, 3.0f};

    StubKeyboard kb;
    kb.wDown = true;
    StubMouse mouse; // RMB up => not flying
    cam.Update(&kb, &mouse, 0.5f);

    CHECK(Near(cam.position.x, 1.0f));
    CHECK(Near(cam.position.y, 2.0f));
    CHECK(Near(cam.position.z, 3.0f));
}

TEST_CASE("EditorCamera says when an Update zoomed on the wheel, and only then")
{
    editor::EditorCamera cam;
    StubKeyboard kb;
    StubMouse mouse;
    const f32 before = cam.focusDistance;
    mouse.scroll = 1.0f;
    cam.Update(&kb, &mouse, 0.016f);
    CHECK(cam.zoomedThisUpdate);
    CHECK(cam.focusDistance < before);

    // The next frame without a notch clears it.
    mouse.scroll = 0.0f;
    cam.Update(&kb, &mouse, 0.016f);
    CHECK_FALSE(cam.zoomedThisUpdate);

    // A wheel a modal tool owns (allowZoom false) is no zoom.
    mouse.scroll = 1.0f;
    cam.Update(&kb, &mouse, 0.016f, /*allowZoom*/ false);
    CHECK_FALSE(cam.zoomedThisUpdate);

    // Turning the camera is no zoom either.
    mouse.scroll = 0.0f;
    mouse.rmb = true;
    mouse.dx = 40.0f;
    cam.Update(&kb, &mouse, 0.016f);
    CHECK_FALSE(cam.zoomedThisUpdate);
}

TEST_CASE("EditorCamera::FrameSphere stands back along its view until the sphere fits, pivoting on it")
{
    editor::EditorCamera cam;
    cam.position = Float3{0.0f, 0.0f, 0.0f};
    cam.LookAt(Float3{0.0f, 0.0f, -1.0f}); // looking down -Z
    const Float3 forward = cam.Forward();
    const f32 fovY = 1.0472f;
    const Float3 center{10.0f, 2.0f, -30.0f};

    // At once: the direction kept, the pivot on the centre, the sphere inside the view.
    cam.FrameSphere(center, 2.0f, fovY, /*ease*/ false);
    CHECK_FALSE(cam.IsGliding());
    CHECK(Near(Dot(cam.Forward(), forward), 1.0f));
    const Float3 pivot = cam.position + cam.Forward() * cam.focusDistance;
    CHECK(Near(pivot.x, center.x));
    CHECK(Near(pivot.y, center.y));
    CHECK(Near(pivot.z, center.z));
    CHECK(2.0f / cam.focusDistance <= Sin(0.5f * fovY)); // the sphere's angle fits the half view

    // Eased: there over glideSeconds, not before, wherever it started.
    cam.position = Float3{0.0f, 0.0f, 0.0f};
    cam.FrameSphere(center, 2.0f, fovY);
    CHECK(cam.IsGliding());
    cam.Advance(cam.glideSeconds * 0.5f);
    CHECK(cam.IsGliding());
    const Float3 halfway = cam.position;
    cam.Advance(cam.glideSeconds);
    CHECK_FALSE(cam.IsGliding());
    CHECK(Length(cam.position - halfway) > 0.01f); // half way was not there yet
    const Float3 settled = cam.position + cam.Forward() * cam.focusDistance;
    CHECK(Near(settled.x, center.x));
    CHECK(Near(settled.z, center.z));

    // The user taking the camera (a wheel notch) stops a glide where it is.
    cam.FrameSphere(Float3{-50.0f, 0.0f, 0.0f}, 1.0f, fovY);
    StubKeyboard kb;
    StubMouse mouse;
    mouse.scroll = 1.0f;
    cam.Update(&kb, &mouse, 0.016f);
    CHECK_FALSE(cam.IsGliding());
}

TEST_CASE("EditorCamera orthographic: the height is what the view spans at the pivot, and follows the zoom")
{
    editor::EditorCamera cam;
    cam.focusDistance = 10.0f;
    CHECK(Near(cam.OrthoHeight(), 2.0f * 10.0f * Tan(0.5f * cam.fovY)));
    cam.focusDistance = 5.0f; // a zoom in halves it
    CHECK(Near(cam.OrthoHeight(), 2.0f * 5.0f * Tan(0.5f * cam.fovY)));

    // Switching keeps the eye and the pivot; only the projection changes, to that height.
    const Float3 eye = cam.position;
    cam.SetOrthographic(true);
    CHECK(Near(cam.position.x, eye.x));
    CHECK(Near(cam.position.z, eye.z));
    const Float4x4 ortho = cam.Projection(2.0f);
    CHECK(projection::IsOrthographic(ortho));
    CHECK(Near(2.0f / ortho.m[1][1], cam.OrthoHeight()));
    CHECK(Near(2.0f / ortho.m[0][0], cam.OrthoHeight() * 2.0f));
    cam.SetOrthographic(false);
    CHECK_FALSE(projection::IsOrthographic(cam.Projection(2.0f)));
}

TEST_CASE("EditorCamera orthographic: a ray runs along the view, from a point that moves with the pixel")
{
    editor::EditorCamera cam;
    cam.LookAt(Float3{0.0f, 0.0f, 0.0f});
    cam.SetOrthographic(true);
    Float3 originA, dirA, originB, dirB;
    cam.RayThrough(0.0f, 0.0f, 1.5f, originA, dirA);
    cam.RayThrough(1.0f, 1.0f, 1.5f, originB, dirB);
    CHECK(Near(Dot(dirA, cam.Forward()), 1.0f));
    CHECK(Near(Dot(dirB, cam.Forward()), 1.0f));
    // The corner's ray starts half the view up and half its width across from the centre's.
    const Float3 offset = originB - originA;
    CHECK(Near(Dot(offset, cam.Up()), 0.5f * cam.OrthoHeight()));
    CHECK(Near(Dot(offset, cam.Right()), 0.5f * cam.OrthoHeight() * 1.5f));

    // In perspective every ray starts at the eye and fans out.
    cam.SetOrthographic(false);
    cam.RayThrough(1.0f, 1.0f, 1.5f, originB, dirB);
    CHECK(Near(originB.x, cam.position.x));
    CHECK(Dot(dirB, cam.Forward()) < 0.95f);
}

TEST_CASE("EditorCamera orthographic: it draws from a view height back, showing the ground below the eye")
{
    // An eye 12 m up, tipped 35 degrees down, zoomed out: the view is 34.6 m tall, so at the eye's
    // plane its bottom edge lies under the ground, and the ground there is nearer than that plane,
    // which a volume starting at it cut away.
    editor::EditorCamera cam;
    cam.position = Float3{0.0f, 12.0f, 10.0f};
    cam.yaw = 0.0f;
    cam.pitch = -0.61f;
    cam.focusDistance = 30.0f;

    // Perspective draws from the eye.
    CHECK(cam.DrawPullBack() == doctest::Approx(0.0f));
    CHECK(Near(cam.DrawEye().z, cam.position.z));
    CHECK(cam.DrawFar() == doctest::Approx(cam.farZ));

    cam.SetOrthographic(true);
    CHECK(cam.DrawPullBack() == doctest::Approx(cam.OrthoHeight()));
    const Float3 drawEye = cam.DrawEye();
    CHECK(Near(Dot(cam.position - drawEye, cam.Forward()), cam.OrthoHeight()));
    // The eye stays put, the far plane keeps its reach past it, and the projection says so.
    CHECK(Near(cam.position.y, 12.0f));
    CHECK(cam.DrawFar() == doctest::Approx(cam.farZ + cam.OrthoHeight()));
    CHECK(projection::FarPlane(cam.Projection(1.5f)) == doctest::Approx(cam.DrawFar()).epsilon(0.001));

    // A ray through the bottom of the view starts on the draw eye's plane, so it finds the ground
    // below the eye, behind the eye's own plane.
    Float3 origin, direction;
    cam.RayThrough(0.0f, -1.0f, 1.5f, origin, direction);
    REQUIRE(direction.y < 0.0f);
    // On the draw eye's plane, not the eye's: the line is the same either way, but a pick takes
    // the first hit past the origin, so from the eye's plane it missed the ground behind it.
    CHECK(Near(Dot(origin - drawEye, cam.Forward()), 0.0f));
    const Float3 hit = origin + direction * (-origin.y / direction.y);
    CHECK(Dot(hit - cam.position, cam.Forward()) < 0.0f);       // behind the eye's plane
    CHECK(Dot(hit - drawEye, cam.Forward()) >= cam.nearZ);      // past the near plane
    // What stands two view heights behind the eye stays clipped.
    const Float3 farBehind = cam.position - cam.Forward() * (2.0f * cam.OrthoHeight());
    CHECK(Dot(farBehind - drawEye, cam.Forward()) < cam.nearZ);

    // Zooming in brings it closer: the reach follows the view height.
    cam.focusDistance = 10.0f;
    CHECK(cam.DrawPullBack() == doctest::Approx(cam.OrthoHeight()));
}

TEST_CASE("EditorCamera orthographic: a snap to an axis turns it on, and turning away turns it off")
{
    editor::EditorCamera cam;
    cam.SnapToAxis(0.0f, -editor::EditorCamera::kStraightPitch); // the top view
    CHECK(cam.orthographic);
    CHECK(cam.IsOrthographicFromSnap());
    CHECK(Near(cam.Forward().y, -1.0f)); // straight down, past the mouse-look limit

    StubKeyboard kb;
    StubMouse mouse;
    mouse.rmb = true;
    cam.Update(&kb, &mouse, 0.016f); // held still: stays
    CHECK(cam.orthographic);
    mouse.dx = 20.0f;
    cam.Update(&kb, &mouse, 0.016f); // a look away: back to perspective
    CHECK_FALSE(cam.orthographic);

    // Chosen by hand, it stays through a look; with the preference off, a snap leaves it alone.
    cam.SetOrthographic(true);
    cam.Update(&kb, &mouse, 0.016f);
    CHECK(cam.orthographic);
    editor::EditorCamera manual;
    manual.autoOrthographic = false;
    manual.SnapToAxis(0.0f, 0.0f);
    CHECK_FALSE(manual.orthographic);
}


TEST_CASE("EditorCamera asks for the pointer locked while its mouse drags it, and never locks the mouse itself")
{
    editor::EditorCamera cam;
    StubKeyboard kb;
    StubMouse mouse;
    cam.Update(&kb, &mouse, 0.016f);
    CHECK_FALSE(cam.WantsPointerLock());

    mouse.rmb = true; // a look drag
    cam.Update(&kb, &mouse, 0.016f);
    CHECK(cam.WantsPointerLock());
    cam.Update(&kb, &mouse, 0.016f);
    CHECK(cam.WantsPointerLock());
    CHECK(mouse.relativeCalls == 0); // the viewport's router locks it, for this surface alone

    mouse.rmb = false; // let go
    cam.Update(&kb, &mouse, 0.016f);
    CHECK_FALSE(cam.WantsPointerLock());

    // Held by Tab, the mouse is locked already: no ask for a drag.
    cam.mouseCaptured = true;
    mouse.rmb = true;
    cam.Update(&kb, &mouse, 0.016f);
    CHECK_FALSE(cam.WantsPointerLock());
    // The drag let go with no keyboard (the view never had the focus): no ask left over.
    cam.mouseCaptured = false;
    cam.Update(&kb, &mouse, 0.016f);
    REQUIRE(cam.WantsPointerLock());
    mouse.rmb = false;
    cam.Update(nullptr, &mouse, 0.016f);
    CHECK_FALSE(cam.WantsPointerLock());
}

TEST_CASE("EditorCamera: the mouse works it with no keyboard (a view not clicked into): the wheel zooms; the keys wait")
{
    editor::EditorCamera cam;
    StubMouse mouse;
    const f32 before = cam.focusDistance;
    mouse.scroll = 1.0f;
    cam.Update(nullptr, &mouse, 0.016f);
    CHECK(cam.focusDistance < before);
    CHECK(cam.zoomedThisUpdate);
    mouse.scroll = 0.0f;
    // A right-button look turns it; with no keyboard there is no fly.
    const Float3 at = cam.position;
    const f32 yaw = cam.yaw;
    mouse.rmb = true;
    mouse.dx = 10.0f;
    cam.Update(nullptr, &mouse, 0.5f);
    CHECK(cam.yaw != yaw);
    CHECK(cam.position.x == doctest::Approx(at.x));
    CHECK(cam.position.z == doctest::Approx(at.z));
}
