// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :rounded_rect_drawable partition
//
// Filled rounded rectangle with optional border; per-corner radii via vg::CornerRadii, and a
// width and colour per side when SideWidths or SideColors is set.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui:rounded_rect_drawable;

import foundation.core; // Color, Rectangle
import foundation.vg;   // CornerRadii
import :drawable;
import :thickness; // DrawablePadding
import :draw_context;

using namespace foundation::core;
namespace core = foundation::core;
namespace vg = foundation::vg;

export namespace foundation::ui
{
    class RoundedRectDrawable : public Drawable
    {
        RTTI_OBJECT(RoundedRectDrawable, Drawable)
    public:
        core::Color FillColor{};
        core::Color BorderColor = core::Color::Transparent;
        f32 BorderWidth = 0.0f;
        vg::CornerRadii Radii{};
        /// A width per side, in place of BorderWidth (CSS's border-top-width and the rest).
        Optional<vg::BorderWidths> SideWidths;
        /// A colour per side, in place of BorderColor; where two sides meet, the corner splits
        /// between them as CSS draws it.
        Optional<vg::BorderColors> SideColors;
        /// Custom properties (`--name`) the fill and the border colour come from where the drawn
        /// view has them; FillColor and BorderColor are their fallbacks. A side's own colour
        /// (SideColors) stays as written.
        String FillVariable;
        String BorderVariable;

        /// The border's width on each side: SideWidths, or BorderWidth on all four.
        [[nodiscard]] vg::BorderWidths Widths() const
        {
            return SideWidths.HasValue() ? SideWidths.Value() : vg::BorderWidths(BorderWidth);
        }
        /// The border's colour on each side: SideColors, or BorderColor on all four.
        [[nodiscard]] vg::BorderColors Colors() const
        {
            return SideColors.HasValue() ? SideColors.Value() : vg::BorderColors(BorderColor);
        }

        RoundedRectDrawable() = default;
        /// Uniform corner radius.
        explicit RoundedRectDrawable(core::Color fill, f32 cornerRadius = 0.0f,
                                     core::Color borderColor = core::Color::Transparent,
                                     f32 borderWidth = 0.0f)
            : FillColor(fill), BorderColor(borderColor), BorderWidth(borderWidth),
              Radii(cornerRadius)
        {
        }
        /// Per-corner radii.
        RoundedRectDrawable(core::Color fill, vg::CornerRadii radii,
                            core::Color borderColor = core::Color::Transparent,
                            f32 borderWidth = 0.0f)
            : FillColor(fill), BorderColor(borderColor), BorderWidth(borderWidth), Radii(radii)
        {
        }

        [[nodiscard]] Optional<vg::CornerRadii> OutlineRadii(ControlState state) const override
        {
            (void)state;
            return Radii;
        }

        void Draw(UIDrawContext& ctx, const Rectangle& bounds) override
        {
            const core::Color fill = ctx.ColorOf(FillColor, FillVariable.AsView());
            const core::Color border = ctx.ColorOf(BorderColor, BorderVariable.AsView());
            // Border strokes are inset (stroke center pulled in by half the width) so the whole
            // border lies inside the bounds (border-box painting). VG strokes are centered on
            // the path, so an un-inset stroke would hang half outside the view, where
            // ClipsContent would cut it off and adjacent siblings' borders would overlap.
            if (SideWidths.HasValue() || SideColors.HasValue())
            {
                // Sides that differ: VG's box draws them, analytically or tessellated.
                const vg::BorderColors colors = SideColors.HasValue() ? SideColors.Value() : vg::BorderColors(border);
                const bool bordered = !colors.IsTransparent() && !Widths().IsZero();
                if (fill.a > 0.0f || bordered)
                {
                    ctx.VG().DrawBox(bounds, Radii, fill, bordered ? Widths() : vg::BorderWidths{}, colors);
                }
                return;
            }
            if (ctx.VG().AnalyticBoxes())
            {
                // Fill and border as one box: one signed distance gives both, so they cannot
                // disagree at the corners, and the border lies inside bounds (border-box).
                const bool bordered = border.a > 0.0f && BorderWidth > 0.0f;
                if (fill.a > 0.0f || bordered)
                {
                    ctx.VG().DrawBox(bounds, Radii, fill, vg::BorderWidths(bordered ? BorderWidth : 0.0f),
                                     border);
                }
                return;
            }
            const f32 inset = BorderWidth * 0.5f;
            const Rectangle borderRect{bounds.x + inset, bounds.y + inset,
                                       Max(0.0f, bounds.width - BorderWidth),
                                       Max(0.0f, bounds.height - BorderWidth)};
            if (!Radii.IsZero())
            {
                if (fill.a > 0.0f)
                {
                    ctx.VG().FillRoundedRect(bounds, Radii, fill);
                }
                if (border.a > 0.0f && BorderWidth > 0.0f)
                {
                    // Shrink radii with the inset so the stroke stays concentric.
                    vg::CornerRadii r = Radii;
                    r.topLeft = Max(0.0f, r.topLeft - inset);
                    r.topRight = Max(0.0f, r.topRight - inset);
                    r.bottomRight = Max(0.0f, r.bottomRight - inset);
                    r.bottomLeft = Max(0.0f, r.bottomLeft - inset);
                    ctx.VG().StrokeRoundedRect(borderRect, r, border, BorderWidth);
                }
            }
            else
            {
                if (fill.a > 0.0f)
                {
                    ctx.VG().FillRect(bounds, fill);
                }
                if (border.a > 0.0f && BorderWidth > 0.0f)
                {
                    ctx.VG().StrokeRect(borderRect, border, BorderWidth);
                }
            }
        }

        /// Border-box: the border is chrome that content must clear. Merged
        /// into padding by View::ResolveBoxMetrics / Panel::EffectivePadding.
        [[nodiscard]] Thickness DrawablePadding() const override
        {
            const vg::BorderWidths w = Widths();
            return Thickness{w.left, w.top, w.right, w.bottom};
        }
    };

    RTTI_DEFINE_OBJECT(RoundedRectDrawable, "rtti::ui")
}
