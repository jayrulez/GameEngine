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
        void SetRelativeMode(bool) override {}
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
