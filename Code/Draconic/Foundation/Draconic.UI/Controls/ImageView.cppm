// Draconic UI - :image_view partition
//
// Displays an image with configurable scaling. Ported from Sedulous.UI/src/Controls/ImageView.bf.
// (The `ScaleType` property shadows the enum type, so enum values are fully qualified. Image is a
// borrowed pointer - not owned.)

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui:image_view;

import draconic.foundation;
import draconic.vg;
import draconic.image; // ImageData
import :view;
import :property;
import :box_constraints;
import :draw_context;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;
namespace image = draconic::image;

export namespace draconic::ui
{
    /// How an ImageView scales its source to fit its bounds.
    enum class ScaleType
    {
        None,
        FitCenter,
        FillBounds,
        CenterCrop
    };

    class ImageView : public View
    {
        DRACONIC_OBJECT(ImageView, View)
    public:
        Property<::draconic::ui::ScaleType> ScaleType{::draconic::ui::ScaleType::FitCenter};
        Property<foundation::Color> Tint{foundation::Color::White};

        ImageView()
        {
            ScaleType.SetOwner(this, InvalidationKind::Visual);
            Tint.SetOwner(this, InvalidationKind::Visual);
        }
        explicit ImageView(const image::ImageData* img) : ImageView() { SetImage(img); }

        [[nodiscard]] const image::ImageData* GetImage() const noexcept { return m_image; }
        void SetImage(const image::ImageData* img)
        {
            if (m_image == img)
            {
                return;
            }
            m_image = img;
            Invalidate();
        }

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            if (m_image != nullptr)
                MeasuredSize =
                    Float2{constraints.ConstrainWidth(static_cast<f32>(m_image->Width())),
                           constraints.ConstrainHeight(static_cast<f32>(m_image->Height()))};
            else
                MeasuredSize =
                    Float2{constraints.ConstrainWidth(0.0f), constraints.ConstrainHeight(0.0f)};
        }

        void OnDraw(UIDrawContext& ctx) override
        {
            if (m_image == nullptr)
            {
                return;
            }
            const f32 iw = static_cast<f32>(m_image->Width());
            const f32 ih = static_cast<f32>(m_image->Height());
            const Rectangle srcRect{0, 0, iw, ih};
            const Rectangle dstRect{0, 0, Width(), Height()};

            switch (ScaleType.Value())
            {
            case ::draconic::ui::ScaleType::None:
                ctx.VG().DrawImage(m_image, Rectangle{0, 0, iw, ih}, srcRect, Tint.Value());
                break;
            case ::draconic::ui::ScaleType::FillBounds:
                ctx.VG().DrawImage(m_image, dstRect, srcRect, Tint.Value());
                break;
            case ::draconic::ui::ScaleType::FitCenter:
            {
                const f32 scale = Min(Width() / iw, Height() / ih);
                const f32 fitW = iw * scale, fitH = ih * scale;
                ctx.VG().DrawImage(
                    m_image,
                    Rectangle{(Width() - fitW) * 0.5f, (Height() - fitH) * 0.5f, fitW, fitH},
                    srcRect, Tint.Value());
                break;
            }
            case ::draconic::ui::ScaleType::CenterCrop:
            {
                const f32 scale = Max(Width() / iw, Height() / ih);
                const f32 cropW = Width() / scale, cropH = Height() / scale;
                ctx.VG().DrawImage(
                    m_image, dstRect,
                    Rectangle{(iw - cropW) * 0.5f, (ih - cropH) * 0.5f, cropW, cropH},
                    Tint.Value());
                break;
            }
            }
        }

    private:
        const image::ImageData* m_image = nullptr;
    };

    DRACONIC_DEFINE_OBJECT(ImageView, "draconic::ui")
}
