// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :image partition
//
// Image: a widget that draws a Drawable (an ImageDrawable/NineSlice/gradient/…) inside its
// content box with a scale mode. Modeled on eepp's UIImage (role only). The drawable is drawn
// in the foreground (over any background/skin); Stretch fills the box, while Fit/Fill/Center/
// None use the drawable's intrinsic size to place it (clip the widget to crop Fill/None).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module experimental.gui:image;

import foundation.core; // RefPtr, Move, Optional, Float2, Min, Max
import :rect;
import :draw_context;
import :drawable;
import :ui_widget;
import :layout_style; // kUnbounded

using namespace foundation::core;
namespace core = foundation::core;

export namespace experimental::gui
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
        RTTI_OBJECT(Image, UIWidget)
    public:
        Image() { SetTag(core::StringView(u8"image")); }

        void SetDrawable(RefPtr<Drawable> drawable)
        {
            m_drawable = core::Move(drawable);
            RequestLayout(); // its size follows its picture's
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
        /// Its picture's size (E2), when the picture has one.
        [[nodiscard]] core::Float2 MeasureContent(core::Float2) override
        {
            return IntrinsicSize().ValueOr(core::Float2{0.0f, 0.0f});
        }
        [[nodiscard]] Optional<core::Float2> IntrinsicSize() const
        {
            return m_drawable ? m_drawable->IntrinsicSize() : Optional<core::Float2>{};
        }

        // The rect the drawable is painted into, for the current scale mode, within the
        // content box (useful for hit-testing the visible image / positioning overlays).
        [[nodiscard]] Rect DrawnBounds() const
        {
            const Rect box = GetContentBounds();
            const Optional<core::Float2> intrinsic =
                m_drawable ? m_drawable->IntrinsicSize() : Optional<core::Float2>{};
            if (m_mode == ImageScaleMode::Stretch || !intrinsic.HasValue())
                return box;

            const core::Float2 nat = intrinsic.Value();
            core::Float2 size = nat;
            if (m_mode == ImageScaleMode::Fit || m_mode == ImageScaleMode::Fill)
            {
                if (nat.x > 0.0f && nat.y > 0.0f)
                {
                    const f32 sx = box.width / nat.x;
                    const f32 sy = box.height / nat.y;
                    const f32 scale =
                        (m_mode == ImageScaleMode::Fit) ? core::Min(sx, sy) : core::Max(sx, sy);
                    size = core::Float2{nat.x * scale, nat.y * scale};
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
}
