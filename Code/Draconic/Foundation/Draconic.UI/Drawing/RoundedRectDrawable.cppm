// Draconic UI - :rounded_rect_drawable partition
//
// Filled rounded rectangle with optional border; per-corner radii via vg::CornerRadii.
// Ported from Sedulous.UI/src/Drawing/RoundedRectDrawable.bf.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui:rounded_rect_drawable;

import draconic.foundation; // Color, Rectangle
import draconic.vg;   // CornerRadii
import :drawable;
import :draw_context;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;
namespace vg = draconic::vg;

export namespace draconic::ui
{
    class RoundedRectDrawable : public Drawable
    {
        DRACONIC_OBJECT(RoundedRectDrawable, Drawable)
    public:
        foundation::Color FillColor{};
        foundation::Color BorderColor = foundation::Color::Transparent;
        f32 BorderWidth = 0.0f;
        vg::CornerRadii Radii{};

        RoundedRectDrawable() = default;
        /// Uniform corner radius.
        explicit RoundedRectDrawable(foundation::Color fill, f32 cornerRadius = 0.0f,
                                     foundation::Color borderColor = foundation::Color::Transparent,
                                     f32 borderWidth = 0.0f)
            : FillColor(fill), BorderColor(borderColor), BorderWidth(borderWidth),
              Radii(cornerRadius)
        {
        }
        /// Per-corner radii.
        RoundedRectDrawable(foundation::Color fill, vg::CornerRadii radii,
                            foundation::Color borderColor = foundation::Color::Transparent,
                            f32 borderWidth = 0.0f)
            : FillColor(fill), BorderColor(borderColor), BorderWidth(borderWidth), Radii(radii)
        {
        }

        void Draw(UIDrawContext& ctx, const Rectangle& bounds) override
        {
            if (!Radii.IsZero())
            {
                if (FillColor.a > 0.0f)
                {
                    ctx.VG().FillRoundedRect(bounds, Radii, FillColor);
                }
                if (BorderColor.a > 0.0f && BorderWidth > 0.0f)
                {
                    ctx.VG().StrokeRoundedRect(bounds, Radii, BorderColor, BorderWidth);
                }
            }
            else
            {
                if (FillColor.a > 0.0f)
                {
                    ctx.VG().FillRect(bounds, FillColor);
                }
                if (BorderColor.a > 0.0f && BorderWidth > 0.0f)
                {
                    ctx.VG().StrokeRect(bounds, BorderColor, BorderWidth);
                }
            }
        }
    };

    DRACONIC_DEFINE_OBJECT(RoundedRectDrawable, "draconic::ui")
}
