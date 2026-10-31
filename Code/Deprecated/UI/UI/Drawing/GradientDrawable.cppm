// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :gradient_drawable partition
//
// A gradient fill of the view's box, rounded by Radii: linear (two colours and a direction, or
// colour stops along a CSS angle) or radial (stops from the centre out to the farthest corner).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui:gradient_drawable;

import foundation.core; // Color, Float2, Rectangle
import foundation.vg;   // VGLinearGradientFill, Path
import :drawable;
import :draw_context;

using namespace foundation::core;
namespace core = foundation::core;
namespace vg = foundation::vg;

export namespace foundation::ui
{
    enum class GradientDirection
    {
        TopToBottom,
        LeftToRight,
        TopLeftToBottomRight,
        TopRightToBottomLeft
    };

    class GradientDrawable : public Drawable
    {
        RTTI_OBJECT(GradientDrawable, Drawable)
    public:
        core::Color StartColor{};
        core::Color EndColor{};
        GradientDirection Direction = GradientDirection::TopToBottom;
        /// Colour stops at offsets along the gradient (0 to 1), in place of StartColor and EndColor.
        Array<vg::GradientStop> Stops;
        /// A CSS angle in degrees, in place of Direction: 0 runs up, 90 to the right, 180 down;
        /// the gradient line passes through the centre and is as long as the box's extent along it.
        Optional<f32> AngleDegrees;
        /// Radial: from the centre out to the farthest corner (a circle).
        bool Radial = false;
        /// The box's corners: the fill is rounded to them, and a shadow under it follows them.
        vg::CornerRadii Radii{};
        /// Custom properties (`--name`) the colours come from where the drawn view has them, the
        /// written colours their fallbacks: StartColor's and EndColor's, and one per stop (empty
        /// for a written one) when StopVariables is as long as Stops.
        String StartVariable;
        String EndVariable;
        Array<String> StopVariables;

        GradientDrawable() = default;
        GradientDrawable(core::Color start, core::Color end,
                         GradientDirection dir = GradientDirection::TopToBottom)
            : StartColor(start), EndColor(end), Direction(dir)
        {
        }

        [[nodiscard]] Optional<vg::CornerRadii> OutlineRadii(ControlState state) const override
        {
            (void)state;
            return Radii;
        }

        void Draw(UIDrawContext& ctx, const Rectangle& bounds) override
        {
            const f32 x = bounds.x, y = bounds.y, w = bounds.width, h = bounds.height;
            vg::PathBuilder pb;
            if (Radii.IsZero())
            {
                pb.MoveTo(x, y);
                pb.LineTo(x + w, y);
                pb.LineTo(x + w, y + h);
                pb.LineTo(x, y + h);
                pb.Close();
            }
            else
            {
                vg::ShapeBuilder::BuildRoundedRect(bounds, Radii, pb);
            }
            const Float2 centre{x + w * 0.5f, y + h * 0.5f};
            if (Radial)
            {
                vg::VGRadialGradientFill fill{centre, Sqrt(w * w + h * h) * 0.5f};
                AddStopsTo(ctx, fill);
                ctx.VG().FillPath(pb.ToPath(), fill);
                return;
            }
            Float2 from{}, to{};
            if (AngleDegrees.HasValue())
            {
                const f32 a = AngleDegrees.Value() * kDegToRad;
                const Float2 dir{Sin(a), -Cos(a)};
                const f32 half = (Abs(w * dir.x) + Abs(h * dir.y)) * 0.5f;
                from = Float2{centre.x - dir.x * half, centre.y - dir.y * half};
                to = Float2{centre.x + dir.x * half, centre.y + dir.y * half};
            }
            else
            {
                switch (Direction)
                {
                case GradientDirection::TopToBottom:
                    from = Float2{x, y};
                    to = Float2{x, y + h};
                    break;
                case GradientDirection::LeftToRight:
                    from = Float2{x, y};
                    to = Float2{x + w, y};
                    break;
                case GradientDirection::TopLeftToBottomRight:
                    from = Float2{x, y};
                    to = Float2{x + w, y + h};
                    break;
                case GradientDirection::TopRightToBottomLeft:
                    from = Float2{x + w, y};
                    to = Float2{x, y + h};
                    break;
                }
            }
            vg::VGLinearGradientFill fill{from, to};
            AddStopsTo(ctx, fill);
            ctx.VG().FillPath(pb.ToPath(), fill);
        }

    private:
        template <typename Fill>
        void AddStopsTo(const UIDrawContext& ctx, Fill& fill) const
        {
            if (Stops.IsEmpty())
            {
                fill.AddStop(0.0f, ctx.ColorOf(StartColor, StartVariable.AsView()));
                fill.AddStop(1.0f, ctx.ColorOf(EndColor, EndVariable.AsView()));
                return;
            }
            const bool named = StopVariables.Size() == Stops.Size();
            for (usize i = 0; i < Stops.Size(); ++i)
            {
                const vg::GradientStop& stop = Stops[i];
                fill.AddStop(stop.offset, named ? ctx.ColorOf(stop.color, StopVariables[i].AsView()) : stop.color);
            }
        }
    };

    RTTI_DEFINE_OBJECT(GradientDrawable, "rtti::ui")
}
