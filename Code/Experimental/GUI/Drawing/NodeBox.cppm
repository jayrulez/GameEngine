// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :node_box partition
//
// NodeBox: a node's box as values, resolved to pixels (G2 of Specs/vg-and-gui-lab.md): background
// colour and layers (gradients, images), a border width and colour per side, corner radii, and box
// shadows, and the effects drawn with them (G4): a blur of the node with its subtree, a blur of
// what lies behind the box, and how the node blends into what is under it. A sheet's box properties write it and native code may set it directly; the node draws it
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
        f32 FilterBlur = 0.0f;   ///< filter: blur(): the node and its subtree blur as one (a sigma, logical units)
        f32 BackdropBlur = 0.0f; ///< backdrop-filter: blur(): what this canvas drew behind the box blurs
        vg::VGBlendMode BlendMode = vg::VGBlendMode::Normal; ///< mix-blend-mode: the node's composite
        /// text-shadow, text-outline, text-glow: drawn under this node's text and its children's
        /// (inherited, as CSS's text-shadow is, through the VG's state); each the node sets
        /// (TextEffectsSet's bits, kTextShadow and the rest) replaces its parent's.
        vg::TextEffects TextEffects{};
        u8 TextEffectsSet = 0;
        static constexpr u8 kTextShadow = 1, kTextOutline = 2, kTextGlow = 4;

        /// The text effects this node draws under: `inherited` with what it sets replaced.
        [[nodiscard]] vg::TextEffects MergeTextEffects(vg::TextEffects inherited) const noexcept
        {
            if ((TextEffectsSet & kTextShadow) != 0)
            {
                inherited.shadowOffset = TextEffects.shadowOffset;
                inherited.shadowBlur = TextEffects.shadowBlur;
                inherited.shadowColor = TextEffects.shadowColor;
            }
            if ((TextEffectsSet & kTextOutline) != 0)
            {
                inherited.outlineWidth = TextEffects.outlineWidth;
                inherited.outlineColor = TextEffects.outlineColor;
            }
            if ((TextEffectsSet & kTextGlow) != 0)
            {
                inherited.glowRadius = TextEffects.glowRadius;
                inherited.glowColor = TextEffects.glowColor;
            }
            return inherited;
        }

        [[nodiscard]] bool IsEmpty() const noexcept
        {
            return BackgroundColor.a <= 0.0f && Layers.IsEmpty() && BorderWidths.IsZero() && Shadows.IsEmpty() &&
                   BackdropBlur <= 0.0f;
        }
        /// Whether the node must draw through a layer of its own: a filter or a blend applies to
        /// the node and its subtree as one picture.
        [[nodiscard]] bool NeedsLayer() const noexcept
        {
            return FilterBlur > 0.0f || BlendMode != vg::VGBlendMode::Normal;
        }

        /// What goes under the content: outer shadows, the backdrop blur, the background colour, the
        /// layers (the last first), inset shadows, then the border.
        void DrawUnder(DrawContext& ctx, const Rect& box, bool withBackdrop = true) const
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
            // The backdrop blurs inside the box's shape, under its (translucent) background.
            if (withBackdrop)
                DrawBackdrop(ctx, box);
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

        /// The backdrop blur alone (a node drawing through a layer draws it first, behind the layer).
        void DrawBackdrop(DrawContext& ctx, const Rect& box) const
        {
            if (BackdropBlur > 0.0f && box.width > 0.0f && box.height > 0.0f)
                ctx.VG().DrawBackdropBlur(Rectangle{box.x, box.y, box.width, box.height}, Radii, BackdropBlur * ctx.DeviceScale());
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

    /// How a sheet styles one of a widget's parts (`slider::thumb`, `checkbox::box`; E3): a box
    /// and an opacity, each aspect only where the sheet gives it. A widget draws a part from its
    /// own look (a NodeBox: its colour, radii, outline, glow) with the sheet's aspects over it.
    struct PartStyle
    {
        NodeBox Box;
        f32 Opacity = 1.0f;
        bool HasBackground = false;  ///< background-color or background-image
        bool HasBorderWidth = false; ///< a border's width or style
        bool HasBorderColor = false;
        bool HasRadii = false;
        bool HasShadows = false;

        /// `own` with the sheet's aspects over it.
        [[nodiscard]] NodeBox Over(NodeBox own) const
        {
            if (HasBackground)
            {
                own.BackgroundColor = Box.BackgroundColor;
                own.Layers = Box.Layers;
            }
            if (HasBorderWidth)
            {
                own.BorderWidths = Box.BorderWidths;
                own.BorderColors = Box.BorderColors;
            }
            else if (HasBorderColor)
                own.BorderColors = Box.BorderColors;
            if (HasRadii)
                own.Radii = Box.Radii;
            if (HasShadows)
                own.Shadows = Box.Shadows;
            return own;
        }
        /// Draws the part over `rect`: its own look with the sheet's over it, at the sheet's opacity.
        void Draw(DrawContext& ctx, const Rect& rect, const NodeBox& own) const
        {
            if (Opacity <= 0.0f)
                return;
            if (Opacity < 1.0f)
                ctx.PushOpacity(Opacity);
            Over(own).DrawUnder(ctx, rect, false);
            if (Opacity < 1.0f)
                ctx.PopOpacity();
        }
    };

    /// A part's own look: a fill, with radii.
    [[nodiscard]] inline NodeBox FilledBox(Color color, vg::CornerRadii radii = {})
    {
        NodeBox box;
        box.BackgroundColor = color;
        box.Radii = radii;
        return box;
    }
    /// A part's own look: an outline of `width`, with radii.
    [[nodiscard]] inline NodeBox OutlinedBox(Color color, f32 width, vg::CornerRadii radii = {})
    {
        NodeBox box;
        box.BorderWidths = vg::BorderWidths(width);
        box.BorderColors = vg::BorderColors(color);
        box.Radii = radii;
        return box;
    }
}
