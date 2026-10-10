// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :view_gizmo partition.
//
// The scene view's orientation gizmo: an axis tripod in the viewport's top-right that turns with
// the camera - the world's X, Y and Z and their negatives as labelled knobs, the positive ones on
// arms from the centre - and a click on a knob looks along that axis. The pure parts live here
// (where the knobs sit for a camera, which one a click lands on, the angles a click snaps to) so
// they are testable without a viewport; the page draws it and routes the click.

module;
#include "Core/Prelude.h"

export module editor.scene:view_gizmo;

import foundation.core;

using namespace foundation::core;

export namespace editor
{
    /// One of the six knobs: a world axis, where its knob sits on screen, and how far it points
    /// away from the viewer (into the screen is positive).
    struct ViewGizmoPole
    {
        Float3 axis{};
        bool positive = true;
        Float2 screen{};
        f32 depth = 0.0f;
    };

    /// The six knobs for one camera, around the gizmo's centre, ordered back to front (draw in
    /// order; hit-test in reverse).
    struct ViewGizmoLayout
    {
        Float2 center{};
        ViewGizmoPole poles[6];
    };

    struct ViewGizmo
    {
        static constexpr f32 kArm = 34.0f;    // pixels from the centre to a knob
        static constexpr f32 kKnob = 18.0f;   // a knob's side, pixels
        static constexpr f32 kMargin = 12.0f; // from the viewport's right edge
        static constexpr f32 kTop = 28.0f;    // from the top edge, below the FPS readout's line
        static constexpr f32 kMaxPitch = 1.55f; // the camera's own pitch limit (EditorCamera)

        /// Where the knobs sit for a camera whose basis is `right`, `up`, `forward`, in a viewport
        /// `viewportWidth` pixels wide: each axis seen along the view, as the tripod shows it.
        [[nodiscard]] static ViewGizmoLayout LayoutFor(Float3 right, Float3 up, Float3 forward,
                                                       f32 viewportWidth)
        {
            ViewGizmoLayout layout;
            layout.center =
                Float2{viewportWidth - kMargin - kArm - 0.5f * kKnob, kTop + kArm + 0.5f * kKnob};
            const Float3 axes[6] = {Float3{1, 0, 0},  Float3{0, 1, 0},  Float3{0, 0, 1},
                                    Float3{-1, 0, 0}, Float3{0, -1, 0}, Float3{0, 0, -1}};
            for (u32 i = 0; i < 6; ++i)
            {
                ViewGizmoPole& pole = layout.poles[i];
                pole.axis = axes[i];
                pole.positive = i < 3;
                pole.screen = Float2{layout.center.x + Dot(axes[i], right) * kArm,
                                     layout.center.y - Dot(axes[i], up) * kArm}; // screen y runs down
                pole.depth = Dot(axes[i], forward);
            }
            // Back to front: the knob pointing furthest into the screen first.
            for (u32 i = 1; i < 6; ++i)
            {
                for (u32 j = i; j > 0 && layout.poles[j].depth > layout.poles[j - 1].depth; --j)
                {
                    const ViewGizmoPole swap = layout.poles[j];
                    layout.poles[j] = layout.poles[j - 1];
                    layout.poles[j - 1] = swap;
                }
            }
            return layout;
        }

        /// The knob under pixel (x, y), the front-most where two overlap; -1 when none.
        [[nodiscard]] static i32 Hit(const ViewGizmoLayout& layout, f32 x, f32 y)
        {
            const f32 half = 0.5f * kKnob;
            for (i32 i = 5; i >= 0; --i)
            {
                const Float2 c = layout.poles[i].screen;
                if (x >= c.x - half && x <= c.x + half && y >= c.y - half && y <= c.y + half)
                {
                    return i;
                }
            }
            return -1;
        }

        /// The camera angles that look along `axis` from its side toward the centre: +X looks
        /// down -X (yaw 90 degrees), +Z down -Z (yaw 0). Y keeps `currentYaw` and pitches to the
        /// camera's limit (straight down from +Y, up from -Y).
        static void SnapAngles(Float3 axis, f32 currentYaw, f32& yaw, f32& pitch)
        {
            if (axis.y > 0.5f || axis.y < -0.5f)
            {
                yaw = currentYaw;
                pitch = axis.y > 0.0f ? -kMaxPitch : kMaxPitch;
                return;
            }
            // forward = -axis; forward = (-cos(p) sin(y), sin(p), -cos(p) cos(y)) at pitch 0.
            yaw = Atan2(axis.x, axis.z);
            pitch = 0.0f;
        }

        /// The knob's label: "X", "-Y", ...
        [[nodiscard]] static StringView Label(const ViewGizmoPole& pole)
        {
            const u32 which = pole.axis.x != 0.0f ? 0u : (pole.axis.y != 0.0f ? 1u : 2u);
            constexpr StringView positive[3] = {u8"X", u8"Y", u8"Z"};
            constexpr StringView negative[3] = {u8"-X", u8"-Y", u8"-Z"};
            return pole.positive ? positive[which] : negative[which];
        }

        /// The axis colours of the scene's origin axes (red X, green Y, blue Z), the negative
        /// knobs dimmer; `hovered` brightens a knob under the pointer.
        [[nodiscard]] static Color ColorOf(const ViewGizmoPole& pole, bool hovered)
        {
            const Color axes[3] = {Color{0.9f, 0.2f, 0.2f, 1.0f}, Color{0.2f, 0.9f, 0.2f, 1.0f},
                                   Color{0.2f, 0.4f, 0.95f, 1.0f}};
            const u32 which = pole.axis.x != 0.0f ? 0u : (pole.axis.y != 0.0f ? 1u : 2u);
            Color c = axes[which];
            const f32 scale = (pole.positive ? 1.0f : 0.45f) * (hovered ? 1.25f : 1.0f);
            return Color{Min(1.0f, c.r * scale), Min(1.0f, c.g * scale), Min(1.0f, c.b * scale),
                         1.0f};
        }
    };
}
