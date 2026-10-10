// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::Scene tests - the orientation gizmo's pure parts: where the knobs sit for a camera (and
// in which order they draw), which knob a click lands on, and the camera a click snaps to.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import editor.camera;
import editor.scene;

using namespace foundation::core;

namespace
{
    bool Near(f32 a, f32 b) { return Abs(a - b) < 1e-3f; }

    const editor::ViewGizmoPole* PoleOf(const editor::ViewGizmoLayout& layout, Float3 axis)
    {
        for (const editor::ViewGizmoPole& pole : layout.poles)
        {
            if (Near(pole.axis.x, axis.x) && Near(pole.axis.y, axis.y) && Near(pole.axis.z, axis.z))
            {
                return &pole;
            }
        }
        return nullptr;
    }
}

TEST_CASE("view gizmo: the knobs sit where the axes point for the camera, drawn back to front")
{
    // Looking down -Z: X to the right, Y up, Z toward the viewer.
    editor::EditorCamera cam;
    cam.yaw = 0.0f;
    cam.pitch = 0.0f;
    const editor::ViewGizmoLayout layout =
        editor::ViewGizmo::LayoutFor(cam.Right(), cam.Up(), cam.Forward(), 1000.0f);
    CHECK(layout.center.x < 1000.0f);
    CHECK(layout.center.x > 1000.0f - 2.0f * editor::ViewGizmo::kArm - editor::ViewGizmo::kKnob -
                                editor::ViewGizmo::kMargin - 1.0f);

    const editor::ViewGizmoPole* x = PoleOf(layout, Float3{1, 0, 0});
    const editor::ViewGizmoPole* y = PoleOf(layout, Float3{0, 1, 0});
    REQUIRE(x != nullptr);
    REQUIRE(y != nullptr);
    CHECK(Near(x->screen.x, layout.center.x + editor::ViewGizmo::kArm)); // right of the centre
    CHECK(Near(x->screen.y, layout.center.y));
    CHECK(Near(y->screen.y, layout.center.y - editor::ViewGizmo::kArm)); // above (screen y runs down)

    // -Z points away from the viewer (drawn first), +Z toward it (drawn last, over the rest).
    CHECK(Near(layout.poles[0].axis.z, -1.0f));
    CHECK(Near(layout.poles[5].axis.z, 1.0f));
    for (u32 i = 1; i < 6; ++i)
    {
        CHECK(layout.poles[i - 1].depth >= layout.poles[i].depth);
    }

    // Turned a quarter to look down -X, the camera's right is -Z, so +Z is on the left.
    cam.yaw = kHalfPi;
    const editor::ViewGizmoLayout turned =
        editor::ViewGizmo::LayoutFor(cam.Right(), cam.Up(), cam.Forward(), 1000.0f);
    const editor::ViewGizmoPole* z = PoleOf(turned, Float3{0, 0, 1});
    REQUIRE(z != nullptr);
    CHECK(Near(z->screen.x, turned.center.x - editor::ViewGizmo::kArm));
}

TEST_CASE("view gizmo: a click lands on the front-most knob under it, and nowhere else")
{
    editor::EditorCamera cam;
    cam.yaw = 0.0f;
    cam.pitch = 0.0f;
    const editor::ViewGizmoLayout layout =
        editor::ViewGizmo::LayoutFor(cam.Right(), cam.Up(), cam.Forward(), 1000.0f);
    const editor::ViewGizmoPole* x = PoleOf(layout, Float3{1, 0, 0});
    REQUIRE(x != nullptr);
    const i32 onX = editor::ViewGizmo::Hit(layout, x->screen.x + 3.0f, x->screen.y - 3.0f);
    REQUIRE(onX >= 0);
    CHECK(Near(layout.poles[onX].axis.x, 1.0f));

    // +Z and -Z share the centre seen down Z: the one pointing at the viewer wins.
    const i32 onCentre = editor::ViewGizmo::Hit(layout, layout.center.x, layout.center.y);
    REQUIRE(onCentre >= 0);
    CHECK(Near(layout.poles[onCentre].axis.z, 1.0f));

    // Between knobs, and far from the gizmo: nothing.
    CHECK(editor::ViewGizmo::Hit(layout, layout.center.x + 0.5f * editor::ViewGizmo::kArm,
                                 layout.center.y - 0.5f * editor::ViewGizmo::kArm) == -1);
    CHECK(editor::ViewGizmo::Hit(layout, 10.0f, 500.0f) == -1);
}

TEST_CASE("view gizmo: a knob snaps the camera to look along its axis, about the same pivot")
{
    const Float3 sides[4] = {Float3{1, 0, 0}, Float3{-1, 0, 0}, Float3{0, 0, 1}, Float3{0, 0, -1}};
    for (const Float3 axis : sides)
    {
        CAPTURE(axis.x);
        CAPTURE(axis.z);
        editor::EditorCamera cam;
        cam.position = Float3{3.0f, 4.0f, 5.0f};
        cam.yaw = 0.3f;
        cam.pitch = -0.2f;
        const Float3 pivot = cam.position + cam.Forward() * cam.focusDistance;
        f32 yaw = 0.0f;
        f32 pitch = 0.0f;
        editor::ViewGizmo::SnapAngles(axis, cam.yaw, yaw, pitch);
        cam.TurnAboutPivot(yaw, pitch);
        // Looking from the knob's side toward the centre: forward is the negated axis.
        CHECK(Near(cam.Forward().x, -axis.x));
        CHECK(Near(cam.Forward().y, 0.0f));
        CHECK(Near(cam.Forward().z, -axis.z));
        const Float3 after = cam.position + cam.Forward() * cam.focusDistance;
        CHECK(Near(after.x, pivot.x));
        CHECK(Near(after.y, pivot.y));
        CHECK(Near(after.z, pivot.z));
    }

    // Top: straight down as far as the camera pitches, keeping the yaw; bottom: up.
    f32 yaw = 0.0f;
    f32 pitch = 0.0f;
    editor::ViewGizmo::SnapAngles(Float3{0, 1, 0}, 0.7f, yaw, pitch);
    CHECK(Near(yaw, 0.7f));
    CHECK(Near(pitch, -editor::ViewGizmo::kMaxPitch));
    editor::ViewGizmo::SnapAngles(Float3{0, -1, 0}, 0.7f, yaw, pitch);
    CHECK(Near(pitch, editor::ViewGizmo::kMaxPitch));
}

TEST_CASE("view gizmo: knobs are labelled by axis, coloured by axis, the negatives dimmer")
{
    editor::ViewGizmoPole x;
    x.axis = Float3{1, 0, 0};
    x.positive = true;
    editor::ViewGizmoPole minusY;
    minusY.axis = Float3{0, -1, 0};
    minusY.positive = false;
    CHECK(editor::ViewGizmo::Label(x) == StringView(u8"X"));
    CHECK(editor::ViewGizmo::Label(minusY) == StringView(u8"-Y"));
    const Color red = editor::ViewGizmo::ColorOf(x, false);
    CHECK(red.r > red.g);
    const Color dimGreen = editor::ViewGizmo::ColorOf(minusY, false);
    CHECK(dimGreen.g > dimGreen.r);
    const editor::ViewGizmoPole y{Float3{0, 1, 0}, true, {}, 0.0f};
    CHECK(dimGreen.g < editor::ViewGizmo::ColorOf(y, false).g);
    CHECK(editor::ViewGizmo::ColorOf(x, true).r >= red.r); // hovered: no dimmer
}
