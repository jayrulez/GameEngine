// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :image_view partition
//
// Displays an image with configurable scaling. Ported from Sedulous.UI/src/Controls/ImageView.bf.
// (The `ScaleType` property shadows the enum type, so enum values are fully qualified. Image is a
// borrowed pointer - not owned.)

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui:image_view;

import foundation.core;
import foundation.vg;
import foundation.image; // ImageData
import :view;
import :property;
import :box_constraints;
import :draw_context;
import :iresource_provider;

using namespace foundation::core;
namespace core = foundation::core;
namespace image = foundation::image;

export namespace foundation::ui
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
        RTTI_OBJECT(ImageView, View)
    public:
        Property<::foundation::ui::ScaleType> ScaleType{::foundation::ui::ScaleType::FitCenter};
        Property<core::Color> Tint{core::Color::White};
        /// What to show, named by a string the context's resource provider resolves (the engine
        /// takes an asset id: a texture, or a render texture a camera draws into). Resolved when
        /// the view is measured or drawn in a context that has a provider; until the provider
        /// answers (an asset still loading), the view shows nothing and asks again. Empty: the
        /// image SetImage gave.
        Property<core::String> Source;
        /// Rounds the picture's corners (the drawn picture's own rect: the fitted rect under
        /// FitCenter, the view under CenterCrop and FillBounds). Zero: square corners.
        Property<vg::CornerRadii> CornerRadius;

        ImageView()
        {
            ScaleType.SetOwner(this, InvalidationKind::Visual);
            CornerRadius.SetOwner(this, InvalidationKind::Visual);
            Tint.SetOwner(this, InvalidationKind::Visual);
            Source.SetOwner(this, InvalidationKind::Layout);
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
            ResolveSource();
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
            ResolveSource();
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
            case ::foundation::ui::ScaleType::None:
                Blit(ctx, Rectangle{0, 0, iw, ih}, srcRect);
                break;
            case ::foundation::ui::ScaleType::FillBounds:
                Blit(ctx, dstRect, srcRect);
                break;
            case ::foundation::ui::ScaleType::FitCenter:
            {
                const f32 scale = Min(Width() / iw, Height() / ih);
                const f32 fitW = iw * scale, fitH = ih * scale;
                Blit(ctx, Rectangle{(Width() - fitW) * 0.5f, (Height() - fitH) * 0.5f, fitW, fitH},
                     srcRect);
                break;
            }
            case ::foundation::ui::ScaleType::CenterCrop:
            {
                const f32 scale = Max(Width() / iw, Height() / ih);
                const f32 cropW = Width() / scale, cropH = Height() / scale;
                Blit(ctx, dstRect,
                     Rectangle{(iw - cropW) * 0.5f, (ih - cropH) * 0.5f, cropW, cropH});
                break;
            }
            }
        }

    private:
        void Blit(UIDrawContext& ctx, Rectangle dst, Rectangle src)
        {
            ctx.VG().DrawImageRounded(m_image, dst, src, CornerRadius.Value(), Tint.Value());
        }

        void ResolveSource()
        {
            const StringView source = Source.Value().AsView();
            if (source.IsEmpty())
            {
                if (!m_resolvedSource.IsEmpty()) // the source was cleared: so is its image
                {
                    m_resolvedSource = String{};
                    m_image = nullptr;
                    Invalidate();
                }
                return;
            }
            if (m_resolvedSource.AsView() == source)
            {
                return;
            }
            IResourceProvider* provider =
                (Context != nullptr) ? Context->ResourceProvider() : nullptr;
            const image::ImageData* image =
                (provider != nullptr) ? provider->LoadImage(source) : nullptr;
            if (image == nullptr)
            {
                m_image = nullptr;
                InvalidateVisual(); // not there yet (a loading asset): ask again next frame
                return;
            }
            m_image = image;
            m_resolvedSource = String(source);
            Invalidate(); // the image's size is the view's natural size: lay out again
        }

        const image::ImageData* m_image = nullptr;
        core::String m_resolvedSource; // the Source m_image answers
    };

    RTTI_DEFINE_OBJECT(ImageView, "rtti::ui")
}
