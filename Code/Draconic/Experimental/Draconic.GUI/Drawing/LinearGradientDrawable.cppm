// Draconic GUI - :linear_gradient_drawable partition
//
// LinearGradientDrawable: a multi-stop linear gradient across the destination rect at a
// given angle. Derived from eepp's LinearGradientDrawable (ColorStop list + angle, which
// it hand-tessellates into per-vertex-colored quads); here it maps to VG's
// VGLinearGradientFill over a rect path. Angle is RADIANS (0 = left-to-right).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.gui:linear_gradient_drawable;

import draconic.foundation; // Color, Float2, Array, Cos/Sin, Max
import draconic.vg;   // VGLinearGradientFill, GradientStop, PathBuilder
import :rect;
import :draw_context;
import :drawable;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;
namespace vg = draconic::vg;

export namespace draconic::gui
{
    class LinearGradientDrawable : public Drawable
    {
        DRACONIC_OBJECT(LinearGradientDrawable, Drawable)
    public:
        f32 Angle = 0.0f; ///< Gradient direction in radians (0 = left-to-right).

        LinearGradientDrawable() = default;

        void AddStop(f32 offset, Color color) { m_stops.PushBack(vg::GradientStop{offset, color}); }
        void ClearStops() { m_stops.Clear(); }
        [[nodiscard]] usize StopCount() const noexcept { return m_stops.Size(); }

        void Draw(DrawContext& ctx, const Rect& dest) override
        {
            if (m_stops.Size() == 0)
                return;

            // Endpoints span the box along the gradient axis (through the center).
            const foundation::Float2 dir{foundation::Cos(Angle), foundation::Sin(Angle)};
            const foundation::Float2 center = dest.Center();
            const f32 half = dest.width * 0.5f * (dir.x < 0.0f ? -dir.x : dir.x) +
                             dest.height * 0.5f * (dir.y < 0.0f ? -dir.y : dir.y);
            const foundation::Float2 from = center - dir * half;
            const foundation::Float2 to = center + dir * half;

            vg::VGLinearGradientFill fill{from, to};
            for (const vg::GradientStop& stop : m_stops)
                fill.AddStop(stop.offset, stop.color);

            ctx.VG().FillPath(RectPath(dest), fill);
        }

    private:
        [[nodiscard]] static vg::Path RectPath(const Rect& r)
        {
            vg::PathBuilder pb;
            pb.MoveTo(r.Left(), r.Top());
            pb.LineTo(r.Right(), r.Top());
            pb.LineTo(r.Right(), r.Bottom());
            pb.LineTo(r.Left(), r.Bottom());
            pb.Close();
            return pb.ToPath();
        }

        Array<vg::GradientStop> m_stops;
    };

    DRACONIC_DEFINE_OBJECT(LinearGradientDrawable, "draconic::gui")
}
