// Draconic UI - :nine_slice_drawable partition
//
// 9-slice image drawable with optional Expand (shadow/glow extending beyond the logical bounds).
// Ported from Sedulous.UI/src/Drawing/NineSliceDrawable.bf. Slices is image::NineSlice (lowercase
// fields); Expand is a ui::Thickness (PascalCase).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui:nine_slice_drawable;

import draconic.foundation;  // Color, Rectangle, Float2, Optional, Max
import draconic.image; // ImageData, NineSlice
import :thickness;
import :drawable;
import :draw_context;

using namespace draconic::foundation;
namespace image = draconic::image;

export namespace draconic::ui
{
    class NineSliceDrawable : public Drawable
    {
        DRACONIC_OBJECT(NineSliceDrawable, Drawable)
    public:
        const image::ImageData* Image = nullptr;
        image::NineSlice Slices{};
        Thickness Expand{};
        Color Tint = Color::White;

        NineSliceDrawable() = default;
        NineSliceDrawable(const image::ImageData* image, image::NineSlice slices,
                          Color tint = Color::White)
            : Image(image), Slices(slices), Tint(tint)
        {
        }

        void Draw(UIDrawContext& ctx, const Rectangle& bounds) override
        {
            if (Image == nullptr)
            {
                return;
            }
            const Rectangle drawBounds{bounds.x - Expand.Left, bounds.y - Expand.Top,
                                       bounds.width + Expand.TotalHorizontal(),
                                       bounds.height + Expand.TotalVertical()};
            const Rectangle srcRect{0.0f, 0.0f, static_cast<f32>(Image->Width()),
                                    static_cast<f32>(Image->Height())};
            ctx.VG().DrawNineSlice(Image, drawBounds, srcRect, Slices, Tint);
        }

        [[nodiscard]] Thickness DrawablePadding() const override
        {
            return Thickness{
                Max(0.0f, Slices.left - Expand.Left), Max(0.0f, Slices.top - Expand.Top),
                Max(0.0f, Slices.right - Expand.Right), Max(0.0f, Slices.bottom - Expand.Bottom)};
        }

        [[nodiscard]] Optional<Float2> IntrinsicSize() const override
        {
            if (Image != nullptr)
            {
                return Float2{static_cast<f32>(Image->Width()) - Expand.TotalHorizontal(),
                              static_cast<f32>(Image->Height()) - Expand.TotalVertical()};
            }
            return {};
        }
    };

    DRACONIC_DEFINE_OBJECT(NineSliceDrawable, "draconic::ui")
}
