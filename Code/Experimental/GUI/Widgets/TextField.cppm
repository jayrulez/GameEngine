// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :text_field partition
//
// TextField: a single-line, editable text control (E4 of Specs/gui-lab-evolution.md), over the
// editing model and keys TextEditBase gives it. Modeled on eepp's UITextInput (role only). Its text
// sits on one line, centred down its height, and scrolls across to keep the caret in view; Return
// submits it (the `submit` event, its value committing as `change`). It may mask its text as a
// password: a bullet for each character, copying and cutting taking nothing out, no IME composition.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module experimental.gui:text_field;

import foundation.core; // Float2, Max, Min
import :rect;
import :text;
import :commands; // CommandEvent
import :text_editing_keys; // kTextSubmitCommand
import :text_edit_base;

using namespace foundation::core;
namespace core = foundation::core;

export namespace experimental::gui
{
    class TextField : public TextEditBase
    {
        RTTI_OBJECT(TextField, TextEditBase)
    public:
        TextField()
        {
            SetTag(core::StringView(u8"textfield"));
            ShownText().SetWhiteSpace(TextWhiteSpace::Pre); // its spaces as typed, on one line
            ShownText().SetAlignment(TextHAlign::Start, TextVAlign::Middle);
            HintText().SetAlignment(TextHAlign::Start, TextVAlign::Middle);
            (void)OnCommand(core::StringView(kTextSubmitCommand), [this](CommandEvent&) { Submit(); });
        }

        /// A password field draws a bullet for each character and lets nothing out by copy or cut.
        void SetPassword(bool password)
        {
            if (password == m_password)
                return;
            m_password = password;
            GetModel().SetCopyAllowed(!password);
            Synced();
        }
        [[nodiscard]] bool IsPassword() const noexcept { return m_password; }

        /// How far its text is scrolled across (to keep the caret in view).
        [[nodiscard]] f32 GetScrollX() const noexcept { return m_scrollX; }

        /// One line of its font, a field's usual width, when nothing sizes it (E2).
        [[nodiscard]] core::Float2 MeasureContent(core::Float2) override
        {
            return core::Float2{160.0f, (ShownText().GetFont() != nullptr) ? ShownText().GetLineHeight() : 20.0f};
        }
        /// Its line's baseline, where it draws its value (and its placeholder, in the same font).
        [[nodiscard]] f32 BaselineAt(core::Float2 size) const override
        {
            const Thickness c = GetChrome();
            return ShownText().FirstBaseline(Rect{c.Left, c.Top, core::Max(0.0f, size.x - c.TotalHorizontal()),
                                                  core::Max(0.0f, size.y - c.TotalVertical())});
        }

    protected:
        [[nodiscard]] bool IsMasked() const override { return m_password; }
        [[nodiscard]] const TextLayout& EditLayout() const override { return ShownText().LayoutFor(Rect{}); } // one line, unbounded
        // The layout's origin: its content's left less the scroll, its line centred down it.
        [[nodiscard]] core::Float2 TextOrigin(const Rect& content) const override
        {
            const core::Float2 origin = ShownText().Origin(Rect{content.x, content.y, 0.0f, content.height});
            return core::Float2{origin.x - m_scrollX, origin.y};
        }
        // Scrolls by the least that shows the caret, and no further than the text needs.
        void KeepCaretInView() override
        {
            const f32 view = core::Max(0.0f, EditBounds().width - kCaretWidth);
            const TextLayout& layout = EditLayout();
            const f32 caret = layout.CaretX(DisplayCaret());
            const f32 before = m_scrollX;
            if (caret - m_scrollX > view)
                m_scrollX = caret - view;
            if (caret < m_scrollX)
                m_scrollX = caret;
            m_scrollX = core::Max(0.0f, core::Min(m_scrollX, layout.Width - view));
            if (m_scrollX != before)
                Invalidate();
        }

    private:
        f32 m_scrollX = 0.0f;
        bool m_password = false;
    };
}
