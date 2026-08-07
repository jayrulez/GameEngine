// Draconic GUI - :image_drawable partition
//
// ImageDrawable: draws an image stretched to fill the destination rect, with a tint.
// Derived from eepp's Texture/TextureRegion drawing (batched textured quad); maps to VG's
// DrawImage. The image is owned elsewhere (theme/atlas) - held by non-owning pointer.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.gui:image_drawable;

import draconic.foundation;  // Color, Rectangle, Float2, Optional
import draconic.image; // ImageData
import :rect;
import :draw_context;
import :drawable;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;
namespace image = draconic::image;

export namespace draconic::gui
{
    class ImageDrawable : public Drawable
    {
        DRACONIC_OBJECT(ImageDrawable, Drawable)
    public:
        const image::ImageData* Image = nullptr;
        Color Tint = Color::White;

        ImageDrawable() = default;
        explicit ImageDrawable(const image::ImageData* image, Color tint = Color::White) noexcept
            : Image(image), Tint(tint)
        {
        }

        void Draw(DrawContext& ctx, const Rect& dest) override
        {
            if (Image == nullptr)
                return;
            ctx.VG().DrawImage(Image, dest.ToRectangle(),
                               foundation::Rectangle{0.0f, 0.0f, static_cast<f32>(Image->Width()),
                                               static_cast<f32>(Image->Height())},
                               Tint);
        }

        [[nodiscard]] Optional<Float2> IntrinsicSize() const override
        {
            if (Image == nullptr)
                return {};
            return foundation::Float2{static_cast<f32>(Image->Width()),
                                static_cast<f32>(Image->Height())};
        }
    };

    DRACONIC_DEFINE_OBJECT(ImageDrawable, "draconic::gui")
}
