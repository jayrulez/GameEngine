// Draconic GUI - :border_drawable partition
//
// BorderDrawable: a stroked (optionally rounded) rectangle border. Derived from eepp's
// UIBorderDrawable, which supports per-side colors/widths + per-corner radii via a
// TRIANGLE_STRIP. This v1 draws a uniform color/width border through VG's
// StrokeRoundedRect; per-side colors/widths are a refinement for the skin/CSS phase.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.gui:border_drawable;

import draconic.foundation; // Color
import draconic.vg;   // CornerRadii
import :rect;
import :draw_context;
import :drawable;

using namespace draconic::foundation;
namespace vg = draconic::vg;

export namespace draconic::gui
{
    class BorderDrawable : public Drawable
    {
        DRACONIC_OBJECT(BorderDrawable, Drawable)
    public:
        BorderDrawable() noexcept { m_color = Color{0.0f, 0.0f, 0.0f, 1.0f}; }
        explicit BorderDrawable(Color color, f32 width = 1.0f) noexcept : m_width(width)
        {
            m_color = color;
        }

        void SetWidth(f32 width) noexcept { m_width = width; }
        [[nodiscard]] f32 GetWidth() const noexcept { return m_width; }

        void SetCornerRadii(vg::CornerRadii radii) noexcept { m_radii = radii; }
        [[nodiscard]] vg::CornerRadii GetCornerRadii() const noexcept { return m_radii; }

        void Draw(DrawContext& ctx, const Rect& dest) override
        {
            if (m_width <= 0.0f)
                return;
            ctx.VG().StrokeRoundedRect(dest.ToRectangle(), m_radii, m_color, m_width);
        }

    private:
        f32 m_width = 1.0f;
        vg::CornerRadii m_radii{};
    };

    DRACONIC_DEFINE_OBJECT(BorderDrawable, "draconic::gui")
}
