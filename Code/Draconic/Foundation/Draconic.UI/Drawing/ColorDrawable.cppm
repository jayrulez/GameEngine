// Draconic UI - :color_drawable partition
//
// Fills bounds with a solid color. Ported from Sedulous.UI/src/Drawing/ColorDrawable.bf.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui:color_drawable;

import draconic.foundation; // Color, Rectangle
import :drawable;
import :draw_context;

using namespace draconic::foundation;
namespace foundation = draconic::foundation; // to name foundation::Color where the field shadows the type

export namespace draconic::ui
{
    class ColorDrawable : public Drawable
    {
        DRACONIC_OBJECT(ColorDrawable, Drawable)
    public:
        foundation::Color Color{};

        ColorDrawable() = default;
        explicit ColorDrawable(foundation::Color color) : Color(color) {}

        void Draw(UIDrawContext& ctx, const Rectangle& bounds) override
        {
            if (Color.a > 0.0f)
            {
                ctx.VG().FillRect(bounds, Color);
            }
        }
    };

    DRACONIC_DEFINE_OBJECT(ColorDrawable, "draconic::ui")
}
