// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :label partition
//
// Label: a UIWidget that displays a line of text. The thinnest real control - it composes
// the Phase-4 Text primitive (drawn in the padding-inset content bounds) onto the widget
// base. Modeled on eepp's UITextView (role, not a line-for-line port). Wrapping / rich text
// follow the Text primitive's own growth.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module experimental.gui:label;

import foundation.core;  // StringView, Color, Float2
import foundation.fonts; // CachedFont
import :reactive; // Observable (what bindings follow)
import :rect;
import :draw_context;
import :text;
import :ui_widget;

using namespace foundation::core;
namespace core = foundation::core;
namespace fonts = foundation::fonts;

export namespace experimental::gui
{
    class Label : public UIWidget
    {
        RTTI_OBJECT(Label, UIWidget)
    public:
        Label()
        {
            SetTag(core::StringView(u8"label"));
            m_text.SetAlignment(TextHAlign::Left, TextVAlign::Middle);
        }

        void SetText(core::StringView text)
        {
            if (text == m_text.GetString())
                return;
            m_text.SetString(text);
            Invalidate();
            m_textChanged.Notify();
        }
        /// The text (a read a binding records).
        [[nodiscard]] core::StringView GetText() const
        {
            m_textChanged.Track();
            return m_text.GetString();
        }

        void SetFont(fonts::CachedFont* font)
        {
            m_text.SetFont(font);
            Invalidate();
        }
        [[nodiscard]] fonts::CachedFont* GetFont() const { return m_text.GetFont(); }

        void SetTextColor(Color color)
        {
            m_text.SetColor(color);
            Invalidate();
        }
        [[nodiscard]] Color GetTextColor() const { return m_text.GetColor(); }

        // Theming hooks (CSS color / font-family / text-align reach the text).
        void SetThemeTextColor(Color color) override { SetTextColor(color); }
        void SetThemeFont(fonts::CachedFont* font) override { SetFont(font); }
        void SetThemeTextAlign(TextHAlign horizontal) override
        {
            SetTextAlignment(horizontal, m_text.GetVAlign());
        }
        void SetThemeTextAlignV(TextVAlign vertical) override
        {
            SetTextAlignment(m_text.GetHAlign(), vertical);
        }

        // Markup: <Label text="Hello" wrap="true">.
        bool SetMarkupAttribute(core::StringView name, core::StringView value) override
        {
            if (name == core::StringView(u8"text"))
            {
                SetText(value);
                return true;
            }
            if (name == core::StringView(u8"wrap"))
            {
                SetWordWrap(value == core::StringView(u8"true"));
                return true;
            }
            return UIWidget::SetMarkupAttribute(name, value);
        }

        void SetTextAlignment(TextHAlign horizontal, TextVAlign vertical)
        {
            m_text.SetAlignment(horizontal, vertical);
            Invalidate();
        }
        [[nodiscard]] TextHAlign GetTextAlignH() const noexcept { return m_text.GetHAlign(); }
        [[nodiscard]] TextVAlign GetTextAlignV() const noexcept { return m_text.GetVAlign(); }

        // Word wrap: when on, the label draws multiple lines within its content width.
        void SetWordWrap(bool enabled)
        {
            m_text.SetWordWrap(enabled);
            Invalidate();
        }
        [[nodiscard]] bool IsWordWrap() const { return m_text.IsWordWrap(); }

        // The natural size of the text (for layout).
        [[nodiscard]] core::Float2 MeasureText() const { return m_text.Measure(); }
        // The size wrapped text occupies at `maxWidth` (widest line x total height).
        [[nodiscard]] core::Float2 MeasureWrapped(f32 maxWidth) const
        {
            return m_text.MeasureWrapped(maxWidth);
        }

    protected:
        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            (void)localBounds;
            m_text.Draw(ctx, GetContentBounds()); // aligned within the padding-inset bounds
        }

        Text m_text;
        Observable m_textChanged; // what bindings reading the text follow
    };
}
