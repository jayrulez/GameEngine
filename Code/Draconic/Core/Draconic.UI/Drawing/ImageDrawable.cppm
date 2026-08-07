// Draconic UI - :image_drawable partition
//
// Draws an image stretched to fill bounds. Ported from Sedulous.UI/src/Drawing/ImageDrawable.bf.
// Sedulous IImageData -> draconic::image::ImageData (non-owning pointer; the image is owned by the
// theme/atlas, not the drawable).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui:image_drawable;

import draconic.foundation;  // Color, Rectangle, Float2, Optional
import draconic.image; // ImageData
import :drawable;
import :draw_context;

using namespace draconic::foundation;
namespace image = draconic::image;

export namespace draconic::ui
{
    class ImageDrawable : public Drawable
    {
        DRACONIC_OBJECT(ImageDrawable, Drawable)
    public:
        const image::ImageData* Image = nullptr;
        Color Tint = Color::White;

        ImageDrawable() = default;
        explicit ImageDrawable(const image::ImageData* image, Color tint = Color::White)
            : Image(image), Tint(tint)
        {
        }

        void Draw(UIDrawContext& ctx, const Rectangle& bounds) override
        {
            if (Image != nullptr)
            {
                ctx.VG().DrawImage(Image, bounds,
                                   Rectangle{0.0f, 0.0f, static_cast<f32>(Image->Width()),
                                             static_cast<f32>(Image->Height())},
                                   Tint);
            }
        }

        [[nodiscard]] Optional<Float2> IntrinsicSize() const override
        {
            if (Image != nullptr)
            {
                return Float2{static_cast<f32>(Image->Width()), static_cast<f32>(Image->Height())};
            }
            return {};
        }
    };

    DRACONIC_DEFINE_OBJECT(ImageDrawable, "draconic::ui")
}
