// Draconic GUI - :rectangle_drawable partition
//
// RectangleDrawable: solid-color fill, sharp or rounded. The widget background primitive.
// Derived from eepp's RectangleDrawable / UIBackgroundDrawable (which hand-tessellate a
// TRIANGLE_FAN for rounded corners); here it maps to VG's FillRect / FillRoundedRect.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.gui:rectangle_drawable;

import draconic.foundation; // Color
import draconic.vg;   // CornerRadii
import :rect;
import :draw_context;
import :drawable;

using namespace draconic::foundation;
namespace vg = draconic::vg;

export namespace draconic::gui
{
    class RectangleDrawable : public Drawable
    {
        DRACONIC_OBJECT(RectangleDrawable, Drawable)
    public:
        RectangleDrawable() = default;
        explicit RectangleDrawable(Color color) noexcept { m_color = color; }

        void SetCornerRadii(vg::CornerRadii radii) noexcept { m_radii = radii; }
        [[nodiscard]] vg::CornerRadii GetCornerRadii() const noexcept { return m_radii; }

        void Draw(DrawContext& ctx, const Rect& dest) override
        {
            if (IsSharp(m_radii))
                ctx.VG().FillRect(dest.ToRectangle(), m_color);
            else
                ctx.VG().FillRoundedRect(dest.ToRectangle(), m_radii, m_color);
        }

    private:
        [[nodiscard]] static bool IsSharp(const vg::CornerRadii& r) noexcept
        {
            return r.topLeft == 0.0f && r.topRight == 0.0f && r.bottomRight == 0.0f &&
                   r.bottomLeft == 0.0f;
        }

        vg::CornerRadii m_radii{};
    };

    DRACONIC_DEFINE_OBJECT(RectangleDrawable, "draconic::gui")
}
