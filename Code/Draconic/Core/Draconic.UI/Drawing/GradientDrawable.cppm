// Draconic UI - :gradient_drawable partition
//
// Linear gradient fill with two colors and a direction. Ported from
// Sedulous.UI/src/Drawing/GradientDrawable.bf (VGLinearGradientFill + a rect Path).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui:gradient_drawable;

import draconic.foundation; // Color, Float2, Rectangle
import draconic.vg;   // VGLinearGradientFill, Path
import :drawable;
import :draw_context;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;
namespace vg = draconic::vg;

export namespace draconic::ui
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
        DRACONIC_OBJECT(GradientDrawable, Drawable)
    public:
        foundation::Color StartColor{};
        foundation::Color EndColor{};
        GradientDirection Direction = GradientDirection::TopToBottom;

        GradientDrawable() = default;
        GradientDrawable(foundation::Color start, foundation::Color end,
                         GradientDirection dir = GradientDirection::TopToBottom)
            : StartColor(start), EndColor(end), Direction(dir)
        {
        }

        void Draw(UIDrawContext& ctx, const Rectangle& bounds) override
        {
            const f32 x = bounds.x, y = bounds.y, w = bounds.width, h = bounds.height;
            Float2 from{}, to{};
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

            vg::VGLinearGradientFill fill{from, to};
            fill.AddStop(0.0f, StartColor);
            fill.AddStop(1.0f, EndColor);

            vg::PathBuilder pb;
            pb.MoveTo(x, y);
            pb.LineTo(x + w, y);
            pb.LineTo(x + w, y + h);
            pb.LineTo(x, y + h);
            pb.Close();
            ctx.VG().FillPath(pb.ToPath(), fill);
        }
    };

    DRACONIC_DEFINE_OBJECT(GradientDrawable, "draconic::ui")
}
