// Draconic UI - :atlas_nine_slice_drawable partition
//
// 9-slice drawable over a sub-region of a shared atlas image. Ported from
// Sedulous.UI/src/Drawing/AtlasNineSliceDrawable.bf.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui:atlas_nine_slice_drawable;

import draconic.foundation;  // Color, Rectangle, Float2, Optional, Max
import draconic.image; // ImageData, NineSlice
import :thickness;
import :drawable;
import :draw_context;

using namespace draconic::foundation;
namespace image = draconic::image;

export namespace draconic::ui
{
    class AtlasNineSliceDrawable : public Drawable
    {
        DRACONIC_OBJECT(AtlasNineSliceDrawable, Drawable)
    public:
        const image::ImageData* AtlasImage = nullptr;
        Rectangle SourceRect{};
        image::NineSlice Slices{};
        Thickness Expand{};
        Color Tint = Color::White;

        AtlasNineSliceDrawable() = default;
        AtlasNineSliceDrawable(const image::ImageData* atlas, Rectangle sourceRect,
                               image::NineSlice slices, Color tint = Color::White,
                               Thickness expand = {})
            : AtlasImage(atlas), SourceRect(sourceRect), Slices(slices), Expand(expand), Tint(tint)
        {
        }

        void Draw(UIDrawContext& ctx, const Rectangle& bounds) override
        {
            if (AtlasImage == nullptr)
            {
                return;
            }
            const Rectangle drawBounds{bounds.x - Expand.Left, bounds.y - Expand.Top,
                                       bounds.width + Expand.TotalHorizontal(),
                                       bounds.height + Expand.TotalVertical()};
            ctx.VG().DrawNineSlice(AtlasImage, drawBounds, SourceRect, Slices, Tint);
        }

        [[nodiscard]] Thickness DrawablePadding() const override
        {
            return Thickness{
                Max(0.0f, Slices.left - Expand.Left), Max(0.0f, Slices.top - Expand.Top),
                Max(0.0f, Slices.right - Expand.Right), Max(0.0f, Slices.bottom - Expand.Bottom)};
        }

        [[nodiscard]] Optional<Float2> IntrinsicSize() const override
        {
            return Float2{SourceRect.width - Expand.TotalHorizontal(),
                          SourceRect.height - Expand.TotalVertical()};
        }
    };

    DRACONIC_DEFINE_OBJECT(AtlasNineSliceDrawable, "draconic::ui")
}
