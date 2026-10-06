// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :content_button partition
//
// Button with arbitrary View content - icons, icon+text combos, or any custom content layout. Ported
// from Sedulous.UI/src/Controls/ContentButton.bf. Content is RefPtr-owned (Beef raw owned + manual
// delete); it is drawn manually in OnDraw (not a logical/visual child), and attached to the context in
// OnMeasure so it can resolve fonts - same content pattern as ToggleButton.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui:content_button;

import foundation.core;
import foundation.vg;
import :button_base;
import :view;
import :control_state;
import :style_property;
import :thickness;
import :box_constraints;
import :draw_context;

using namespace foundation::core;

export namespace foundation::ui
{
    class ContentButton : public ButtonBase
    {
        RTTI_OBJECT(ContentButton, ButtonBase)
    public:
        ContentButton() = default;
        explicit ContentButton(RefPtr<View> content) : m_content(Move(content)) {}

        [[nodiscard]] View* Content() const noexcept { return m_content.Get(); }
        void SetContent(RefPtr<View> content)
        {
            m_content = Move(content);
            Invalidate();
        }

        [[nodiscard]] View* ContentChild() const noexcept override { return m_content.Get(); }
        bool SetContentChild(RefPtr<View> content) override
        {
            SetContent(Move(content));
            return true;
        }

    protected:
        [[nodiscard]] Thickness DefaultStylePadding() const override { return Thickness{12, 8}; }

        // Content-only measure - chrome is base-handled.
        [[nodiscard]] Float2 OnMeasureContent(BoxConstraints contentConstraints) override
        {
            f32 contentW = 0, contentH = 0;
            if (m_content)
            {
                // Pass the context down so content can resolve fonts during measure.
                if (m_content->Context == nullptr && Context != nullptr)
                {
                    Context->AttachView(m_content.Get());
                }
                m_content->Measure(contentConstraints.Loosen());
                contentW = m_content->MeasuredSize.x;
                contentH = m_content->MeasuredSize.y;
            }
            return Float2{contentW, contentH};
        }

        void OnLayout(f32 left, f32 top, f32 width, f32 height) override
        {
            (void)left;
            (void)top;
            if (!m_content)
            {
                return;
            }
            const Thickness pad = ResolveBoxMetrics().Chrome(); // same chrome measure used
            const f32 contentW = width - pad.TotalHorizontal();
            const f32 contentH = height - pad.TotalVertical();
            const f32 cw = m_content->MeasuredSize.x;
            const f32 ch = m_content->MeasuredSize.y;
            m_content->Layout(pad.Left + (contentW - cw) * 0.5f, pad.Top + (contentH - ch) * 0.5f,
                              cw, ch);
        }

        void OnDraw(UIDrawContext& ctx) override
        {
            const Rectangle bounds{0, 0, Width(), Height()};
            DrawButtonBackground(ctx, bounds, GetControlState());
            if (m_content)
            {
                ctx.VG().PushState();
                ctx.VG().Translate(m_content->Bounds.x, m_content->Bounds.y);
                m_content->OnDraw(ctx);
                ctx.VG().PopState();
            }
        }

    private:
        RefPtr<View> m_content;
    };

    RTTI_DEFINE_OBJECT(ContentButton, "rtti::ui")
}
