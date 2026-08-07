// Draconic GUI - :radial_gradient_drawable partition
//
// RadialGradientDrawable: a multi-stop radial gradient centered within the destination
// rect. Derived from eepp's RadialGradientDrawable (ColorStop list + center + radius);
// maps to VG's VGRadialGradientFill over a rect path. Center is a fraction of the bounds
// (0.5,0.5 = middle); RadiusScale multiplies the farthest-corner distance (CSS default).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.gui:radial_gradient_drawable;

import draconic.foundation; // Color, Float2, Array, Distance, Max
import draconic.vg;   // VGRadialGradientFill, GradientStop, PathBuilder
import :rect;
import :draw_context;
import :drawable;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;
namespace vg = draconic::vg;

export namespace draconic::gui
{
    class RadialGradientDrawable : public Drawable
    {
        DRACONIC_OBJECT(RadialGradientDrawable, Drawable)
    public:
        foundation::Float2 Center{0.5f, 0.5f}; ///< Center as a fraction of the bounds.
        f32 RadiusScale = 1.0f;          ///< Radius = RadiusScale * farthest-corner distance.

        RadialGradientDrawable() = default;

        void AddStop(f32 offset, Color color) { m_stops.PushBack(vg::GradientStop{offset, color}); }
        void ClearStops() { m_stops.Clear(); }
        [[nodiscard]] usize StopCount() const noexcept { return m_stops.Size(); }

        void Draw(DrawContext& ctx, const Rect& dest) override
        {
            if (m_stops.Size() == 0)
                return;

            const foundation::Float2 centerPx{dest.x + Center.x * dest.width,
                                        dest.y + Center.y * dest.height};
            const f32 radius = RadiusScale * FarthestCornerDistance(dest, centerPx);

            vg::VGRadialGradientFill fill{centerPx, radius};
            for (const vg::GradientStop& stop : m_stops)
                fill.AddStop(stop.offset, stop.color);

            ctx.VG().FillPath(RectPath(dest), fill);
        }

    private:
        [[nodiscard]] static f32 FarthestCornerDistance(const Rect& r, foundation::Float2 c)
        {
            const f32 d0 = foundation::Distance(c, foundation::Float2{r.Left(), r.Top()});
            const f32 d1 = foundation::Distance(c, foundation::Float2{r.Right(), r.Top()});
            const f32 d2 = foundation::Distance(c, foundation::Float2{r.Left(), r.Bottom()});
            const f32 d3 = foundation::Distance(c, foundation::Float2{r.Right(), r.Bottom()});
            return foundation::Max(foundation::Max(d0, d1), foundation::Max(d2, d3));
        }

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

    DRACONIC_DEFINE_OBJECT(RadialGradientDrawable, "draconic::gui")
}
