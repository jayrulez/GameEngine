// Draconic UI - :atlas_image_drawable partition
//
// Draws a sub-region of a shared atlas image (single-texture batching for themed UI).
// Ported from Sedulous.UI/src/Drawing/AtlasImageDrawable.bf.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui:atlas_image_drawable;

import draconic.foundation;  // Color, Rectangle, Float2, Optional
import draconic.image; // ImageData
import :drawable;
import :draw_context;

using namespace draconic::foundation;
namespace image = draconic::image;

export namespace draconic::ui
{
    class AtlasImageDrawable : public Drawable
    {
        DRACONIC_OBJECT(AtlasImageDrawable, Drawable)
    public:
        const image::ImageData* AtlasImage = nullptr;
        Rectangle SourceRect{};
        Color Tint = Color::White;

        AtlasImageDrawable() = default;
        AtlasImageDrawable(const image::ImageData* atlas, Rectangle sourceRect,
                           Color tint = Color::White)
            : AtlasImage(atlas), SourceRect(sourceRect), Tint(tint)
        {
        }

        void Draw(UIDrawContext& ctx, const Rectangle& bounds) override
        {
            if (AtlasImage != nullptr)
            {
                ctx.VG().DrawImage(AtlasImage, bounds, SourceRect, Tint);
            }
        }

        [[nodiscard]] Optional<Float2> IntrinsicSize() const override
        {
            return Float2{SourceRect.width, SourceRect.height};
        }
    };

    DRACONIC_DEFINE_OBJECT(AtlasImageDrawable, "draconic::ui")
}
