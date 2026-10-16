// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :node_box partition
//
// NodeBox: a node's box as values, resolved to pixels (G2 of Specs/vg-and-gui-lab.md): background
// colour and layers (gradients, images), a border width and colour per side, corner radii, and box
// shadows. A sheet's box properties write it and native code may set it directly; the node draws it
// through VG (analytic boxes, box shadows, gradient fills) with nothing allocated per draw or per
// restyle, and G3's transitions interpolate its fields.

module;
#include "Core/Prelude.h"

export module experimental.gui:node_box;

import foundation.core;
import foundation.image;
import foundation.vg;
import :rect;
import :draw_context;

using namespace foundation::core;
namespace core = foundation::core;
namespace vg = foundation::vg;
namespace image = foundation::image;

export namespace experimental::gui
{
    enum class BoxGradientKind : u8
    {
        Linear,
        Radial,
        Conic,
    };

    /// A gradient layer, its stops settled to positions in [0, 1].
    struct BoxGradient
    {
        BoxGradientKind Kind = BoxGradientKind::Linear;
        f32 Angle = 3.14159265358979f; ///< Linear: CSS's angle (0 up, clockwise). Conic: its start.
        bool Repeating = false;
        Array<vg::GradientStop> Stops;
    };

    /// One background layer over the background colour: a gradient or an image.
    struct BoxLayer
    {
        bool IsImage = false;
        BoxGradient Gradient;
        const image::ImageData* Image = nullptr; ///< Not owned (the resource provider's).
    };

    /// One box shadow, in pixels.
    struct BoxShadow
    {
        core::Float2 Offset{};
        f32 Blur = 0.0f;
        f32 Spread = 0.0f;
        Color ShadowColor{0.0f, 0.0f, 0.0f, 0.5f};
        bool Inset = false;

        [[nodiscard]] bool operator==(const BoxShadow&) const noexcept = default;
    };

    struct NodeBox
    {
        Color BackgroundColor{0.0f, 0.0f, 0.0f, 0.0f};
        Array<BoxLayer> Layers; ///< First on top, as CSS lists them.
        vg::BorderWidths BorderWidths{};
        vg::BorderColors BorderColors{};
        vg::CornerRadii Radii{};
        Array<BoxShadow> Shadows;

        [[nodiscard]] bool IsEmpty() const noexcept
        {
            return BackgroundColor.a <= 0.0f && Layers.IsEmpty() && BorderWidths.IsZero() && Shadows.IsEmpty();
        }

        /// What goes under the content: outer shadows, the background colour, the layers (the last
        /// first), inset shadows, then the border.
        void DrawUnder(DrawContext& ctx, const Rect& box) const
        {
            if (IsEmpty() || box.width <= 0.0f || box.height <= 0.0f)
                return;
            vg::VGContext& canvas = ctx.VG();
            const Rectangle rect{box.x, box.y, box.width, box.height};
            for (const BoxShadow& shadow : Shadows)
            {
                if (shadow.Inset)
                    continue;
                const Rectangle grown{rect.x + shadow.Offset.x - shadow.Spread, rect.y + shadow.Offset.y - shadow.Spread,
                                      rect.width + 2.0f * shadow.Spread, rect.height + 2.0f * shadow.Spread};
                canvas.FillBoxShadow(grown, Grown(Radii, shadow.Spread), shadow.Blur, shadow.ShadowColor, false);
            }
            if (BackgroundColor.a > 0.0f)
                canvas.DrawBox(rect, Radii, BackgroundColor);
            for (usize i = Layers.Size(); i-- > 0;)
                DrawLayer(canvas, Layers[i], rect);
            for (const BoxShadow& shadow : Shadows)
            {
                if (!shadow.Inset)
                    continue;
                const Rectangle shrunk{rect.x + shadow.Offset.x + shadow.Spread, rect.y + shadow.Offset.y + shadow.Spread,
                                       rect.width - 2.0f * shadow.Spread, rect.height - 2.0f * shadow.Spread};
                canvas.FillBoxShadow(shrunk, Grown(Radii, -shadow.Spread), shadow.Blur, shadow.ShadowColor, true);
            }
            if (!BorderWidths.IsZero() && !BorderColors.IsTransparent())
                canvas.DrawBox(rect, Radii, Color{0.0f, 0.0f, 0.0f, 0.0f}, BorderWidths, BorderColors);
        }

    private:
        [[nodiscard]] static vg::CornerRadii Grown(const vg::CornerRadii& r, f32 by) noexcept
        {
            const auto grow = [by](f32 radius) { return radius > 0.0f ? Max(0.0f, radius + by) : 0.0f; };
            return vg::CornerRadii(grow(r.topLeft), grow(r.topRight), grow(r.bottomRight), grow(r.bottomLeft));
        }

        void DrawLayer(vg::VGContext& canvas, const BoxLayer& layer, const Rectangle& rect) const
        {
            if (layer.IsImage)
            {
                if (layer.Image != nullptr)
                    canvas.DrawImage(layer.Image, rect);
                return;
            }
            const BoxGradient& g = layer.Gradient;
            if (g.Stops.Size() < 2)
                return;
            vg::PathBuilder shape;
            vg::ShapeBuilder::BuildRoundedRect(rect, Radii, shape);
            const core::Float2 center{rect.x + rect.width * 0.5f, rect.y + rect.height * 0.5f};
            // A repeating gradient repeats the span of its stops (CSS), and VG repeats its whole
            // line: the line is cut to the stops' span (linear: its ends; radial: its radius, from
            // the centre) and the stops spread over it.
            const f32 first = g.Repeating ? g.Stops[0].offset : 0.0f;
            const f32 last = g.Repeating ? g.Stops[g.Stops.Size() - 1].offset : 1.0f;
            const bool repeating = g.Repeating && last - first > 1e-4f;
            const auto addStops = [&](auto& fill, f32 from, f32 to)
            {
                for (const vg::GradientStop& stop : g.Stops)
                    fill.AddStop(repeating ? (stop.offset - from) / (to - from) : stop.offset, stop.color);
                fill.spread = repeating ? vg::VGGradientSpread::Repeat : vg::VGGradientSpread::Pad;
            };
            if (g.Kind == BoxGradientKind::Linear)
            {
                // CSS's gradient line: through the centre at the angle (0 up, clockwise), as long as
                // the box's extent along it, so the corners take the end colours.
                const core::Float2 dir{Sin(g.Angle), -Cos(g.Angle)};
                const f32 half = (Abs(rect.width * dir.x) + Abs(rect.height * dir.y)) * 0.5f;
                const core::Float2 start = center - dir * half;
                const core::Float2 line = dir * (2.0f * half);
                vg::VGLinearGradientFill fill{start + line * first, start + line * last};
                addStops(fill, first, last);
                canvas.FillPath(shape.ToPath(), fill);
            }
            else if (g.Kind == BoxGradientKind::Radial)
            {
                // A circle at the centre reaching the farthest corner (CSS's default extent).
                const f32 radius = Sqrt(rect.width * rect.width + rect.height * rect.height) * 0.5f;
                // The radius ends at the last stop; the stops keep their distance from the centre.
                vg::VGRadialGradientFill fill{center, radius * last};
                addStops(fill, 0.0f, last);
                canvas.FillPath(shape.ToPath(), fill);
            }
            else
            {
                // CSS's conic 0 points up; VG's measures from +x, both clockwise on screen.
                vg::VGConicGradientFill fill{center, g.Angle - 1.57079632679f};
                for (const vg::GradientStop& stop : g.Stops)
                    fill.AddStop(stop.offset, stop.color);
                canvas.FillPath(shape.ToPath(), fill);
            }
        }
    };
}
