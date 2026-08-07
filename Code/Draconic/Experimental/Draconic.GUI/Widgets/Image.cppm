// Draconic GUI - :image partition
//
// Image: a widget that draws a Drawable (an ImageDrawable/NineSlice/gradient/…) inside its
// content box with a scale mode. Modeled on eepp's UIImage (role only). The drawable is drawn
// in the foreground (over any background/skin); Stretch fills the box, while Fit/Fill/Center/
// None use the drawable's intrinsic size to place it (clip the widget to crop Fill/None).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.gui:image;

import draconic.foundation; // RefPtr, Move, Optional, Float2, Min, Max
import :rect;
import :draw_context;
import :drawable;
import :ui_widget;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;

export namespace draconic::gui
{
    enum class ImageScaleMode
    {
        Stretch, // fill the content box, ignoring aspect
        Fit,     // largest size that fits inside, aspect preserved, centered
        Fill,    // smallest size that covers the box, aspect preserved, centered (crops)
        Center,  // natural size, centered
        None,    // natural size, top-left
    };

    class Image : public UIWidget
    {
        DRACONIC_OBJECT(Image, UIWidget)
    public:
        Image() { SetTag(foundation::StringView(u8"image")); }

        void SetDrawable(RefPtr<Drawable> drawable)
        {
            m_drawable = foundation::Move(drawable);
            Invalidate();
        }
        [[nodiscard]] Drawable* GetDrawable() const noexcept { return m_drawable.Get(); }

        void SetScaleMode(ImageScaleMode mode)
        {
            m_mode = mode;
            Invalidate();
        }
        [[nodiscard]] ImageScaleMode GetScaleMode() const noexcept { return m_mode; }

        // Natural size of the drawable (for layout), if it has one.
        [[nodiscard]] Optional<foundation::Float2> IntrinsicSize() const
        {
            return m_drawable ? m_drawable->IntrinsicSize() : Optional<foundation::Float2>{};
        }

        // The rect the drawable is painted into, for the current scale mode, within the
        // content box (useful for hit-testing the visible image / positioning overlays).
        [[nodiscard]] Rect DrawnBounds() const
        {
            const Rect box = GetContentBounds();
            const Optional<foundation::Float2> intrinsic =
                m_drawable ? m_drawable->IntrinsicSize() : Optional<foundation::Float2>{};
            if (m_mode == ImageScaleMode::Stretch || !intrinsic.HasValue())
                return box;

            const foundation::Float2 nat = intrinsic.Value();
            foundation::Float2 size = nat;
            if (m_mode == ImageScaleMode::Fit || m_mode == ImageScaleMode::Fill)
            {
                if (nat.x > 0.0f && nat.y > 0.0f)
                {
                    const f32 sx = box.width / nat.x;
                    const f32 sy = box.height / nat.y;
                    const f32 scale =
                        (m_mode == ImageScaleMode::Fit) ? foundation::Min(sx, sy) : foundation::Max(sx, sy);
                    size = foundation::Float2{nat.x * scale, nat.y * scale};
                }
            }
            // None uses the natural size at the top-left; the rest center within the box.
            if (m_mode == ImageScaleMode::None)
                return Rect{box.x, box.y, size.x, size.y};
            return Rect{box.x + (box.width - size.x) * 0.5f, box.y + (box.height - size.y) * 0.5f,
                        size.x, size.y};
        }

    protected:
        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            (void)localBounds;
            if (!m_drawable)
                return;
            m_drawable->Draw(ctx, DrawnBounds());
        }

    private:
        RefPtr<Drawable> m_drawable;
        ImageScaleMode m_mode = ImageScaleMode::Stretch;
    };

    DRACONIC_DEFINE_OBJECT(Image, "draconic::gui")
}
