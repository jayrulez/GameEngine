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
import :layout_style; // kUnbounded

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
            StyleWriteScope defaults(*this); // its own values, which a sheet returns to
            SetTextAlignment(TextHAlign::Left, TextVAlign::Middle);
        }

        void SetText(core::StringView text)
        {
            if (text == m_text.GetString())
                return;
            m_text.SetString(text);
            RequestLayout(); // its size follows its text
            Invalidate();
            m_textChanged.Notify();
        }
        /// The text (a read a binding records).
        [[nodiscard]] core::StringView GetText() const
        {
            m_textChanged.Track();
            return m_text.GetString();
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


        // Word wrap: when on, the label draws multiple lines within its content width.
        void SetWordWrap(bool enabled)
        {
            m_text.SetWordWrap(enabled);
            RequestLayout();
            Invalidate();
        }
        [[nodiscard]] bool IsWordWrap() const { return m_text.IsWordWrap(); }

        // The natural size of the text (for layout).
        [[nodiscard]] core::Float2 MeasureText() const { return m_text.Measure(); }
        /// Its text's size (E2): one line, or wrapped to the width available (its longest word when
        /// none is), lines broken where the text breaks them.
        [[nodiscard]] core::Float2 MeasureContent(core::Float2 available) override
        {
            if (!(m_text.GetFont() != nullptr) || m_text.GetString().Size() == 0)
                return core::Float2{0.0f, (m_text.GetFont() != nullptr) ? m_text.GetLineHeight() : 0.0f};
            return m_text.MeasureWrapped(m_text.IsWordWrap() && !IsUnbounded(available.x) ? available.x : kUnbounded);
        }
        /// Its text's first baseline, where it draws it in the content box.
        [[nodiscard]] f32 BaselineAt(core::Float2 size) const override
        {
            const Thickness c = GetChrome();
            return m_text.FirstBaseline(Rect{c.Left, c.Top, core::Max(0.0f, size.x - c.TotalHorizontal()),
                                             core::Max(0.0f, size.y - c.TotalVertical())});
        }
        // The size wrapped text occupies at `maxWidth` (widest line x total height).
        [[nodiscard]] core::Float2 MeasureWrapped(f32 maxWidth) const
        {
            return m_text.MeasureWrapped(maxWidth);
        }

    protected:
        // Its text draws in its text style (its own, a sheet's, or what it inherits).
        void OnTextStyleChange() override
        {
            m_text.SetFont(GetFont());
            m_text.SetColor(GetTextColor());
            m_text.SetAlignment(GetTextAlignH(), GetTextAlignV());
        }

        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            (void)localBounds;
            m_text.Draw(ctx, GetContentBounds()); // aligned within the padding-inset bounds
        }

        Text m_text;
        Observable m_textChanged; // what bindings reading the text follow
    };
}
