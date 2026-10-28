// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :tooltip_view partition
//
// Tooltip container with a themed background. Content is any View (defaults to a simple text label set
// by TooltipManager). Ported from Sedulous.UI/src/Overlay/TooltipView.bf. Ownership: Beef raw mContent
// owned by the ViewGroup tree -> the content is held only by the AddView RefPtr; SetContent/ClearContent
// go through RemoveView (our RemoveView drops the tree's ref). m_content is a borrowed raw pointer valid
// while attached.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui:tooltip_view;

import foundation.core;
import foundation.vg;
import :view;
import :box_constraints;
import :thickness;
import :draw_context;
import :drawable;
import :style_property;

using namespace foundation::core;

export namespace foundation::ui
{
    class TooltipView : public ViewGroup
    {
        RTTI_OBJECT(TooltipView, ViewGroup)
    public:
        TooltipView() { Padding = Thickness{8, 4}; }

        /// Set a custom view as tooltip content.
        void SetContent(View* content)
        {
            if (m_content != nullptr)
            {
                RemoveView(m_content, true);
            }
            m_content = content;
            if (content != nullptr)
            {
                AddView(content);
            }
        }

        /// Clear content (called before reuse).
        void ClearContent()
        {
            if (m_content != nullptr)
            {
                RemoveView(m_content, true);
                m_content = nullptr;
            }
        }

        void OnDraw(UIDrawContext& ctx) override
        {
            const Rectangle bounds{0, 0, Width(), Height()};

            // Background from theme.
            Drawable* bg = ResolveStyleDrawable(StyleProperty::Background);
            if (bg != nullptr)
            {
                bg->Draw(ctx, bounds);
            }
            else
            {
                ctx.VG().FillRoundedRect(
                    bounds, 4.0f,
                    Color{40.0f / 255.0f, 42.0f / 255.0f, 50.0f / 255.0f, 230.0f / 255.0f});
                ctx.VG().StrokeRoundedRect(
                    bounds, 4.0f, Color{70.0f / 255.0f, 75.0f / 255.0f, 85.0f / 255.0f, 1.0f},
                    1.0f);
            }

            // Draw content.
            ViewGroup::OnDraw(ctx);
        }

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            const BoxConstraints inner = constraints.Deflate(Padding);
            f32 contentW = 0, contentH = 0;
            if (m_content != nullptr)
            {
                m_content->Measure(inner);
                contentW = m_content->MeasuredSize.x;
                contentH = m_content->MeasuredSize.y;
            }
            MeasuredSize = Float2{constraints.ConstrainWidth(contentW + Padding.TotalHorizontal()),
                                  constraints.ConstrainHeight(contentH + Padding.TotalVertical())};
        }

        void OnLayout(f32 left, f32 top, f32 width, f32 height) override
        {
            (void)left;
            (void)top;
            if (m_content != nullptr)
            {
                m_content->Layout(Padding.Left, Padding.Top, width - Padding.TotalHorizontal(),
                                  height - Padding.TotalVertical());
            }
        }

    private:
        View* m_content = nullptr;
    };

    RTTI_DEFINE_OBJECT(TooltipView, "rtti::ui")
}
