// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :text_field partition
//
// TextField: a single-line editable text control, using the editing model and keys from
// TextEditBase. The text sits on one line, centred vertically, and scrolls horizontally to keep the
// caret in view. Return submits it: the field sends `submit`, and its value commits as `change`.
// It can mask its text as a password: each character shows as a bullet, copy and cut take nothing
// out, and there is no IME composition.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.gui:text_field;

import foundation.core; // Float2, Max, Min
import :rect;
import :text;
import :commands; // CommandEvent
import :text_editing_keys; // kTextSubmitCommand
import :text_edit_base;

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::gui
{
    class TextField : public TextEditBase
    {
        RTTI_OBJECT(TextField, TextEditBase)
    public:
        TextField()
        {
            SetTag(core::StringView(u8"textfield"));
            ShownText().SetWhiteSpace(TextWhiteSpace::Pre); // keep spaces as typed, on one line
            ShownText().SetAlignment(TextHAlign::Start, TextVAlign::Middle);
            HintText().SetAlignment(TextHAlign::Start, TextVAlign::Middle);
            {
                StyleWriteScope defaults(*this);
                SetTextAlignment(TextHAlign::Start, TextVAlign::Middle);
            }
            (void)OnCommand(core::StringView(kTextSubmitCommand), [this](CommandEvent&) { Submit(); });
        }

        /// Makes it a password field, which draws a bullet for each character and lets nothing out
        /// by copy or cut.
        void SetPassword(bool password)
        {
            if (password == m_password)
                return;
            m_password = password;
            GetModel().SetCopyAllowed(!password);
            Synced();
        }
        [[nodiscard]] bool IsPassword() const noexcept { return m_password; }

        /// Returns how far the text is scrolled from its inline start to keep the caret in view:
        /// from the left in a left-to-right line, from the right in a right-to-left one.
        [[nodiscard]] f32 GetScrollX() const noexcept { return m_scrollX; }

        /// Measures one line of the font at a field's usual width, used when nothing else sizes it.
        [[nodiscard]] core::Float2 MeasureContent(core::Float2) override
        {
            return core::Float2{160.0f, (ShownText().GetFont() != nullptr) ? ShownText().GetLineHeight() : 20.0f};
        }
        /// Returns the line's baseline, where the value is drawn (and the placeholder, in the same
        /// font).
        [[nodiscard]] f32 BaselineAt(core::Float2 size) const override
        {
            const Thickness c = GetChrome();
            return ShownText().FirstBaseline(Rect{c.Left, c.Top, core::Max(0.0f, size.x - c.TotalHorizontal()),
                                                  core::Max(0.0f, size.y - c.TotalVertical())});
        }

    protected:
        [[nodiscard]] bool IsMasked() const override { return m_password; }
        [[nodiscard]] const TextLayout& EditLayout() const override { return ShownText().LayoutFor(Rect{}); } // one line, unbounded
        // Returns the layout's origin. A line shorter than the field sits where its alignment puts
        // it, resolved against the line's own direction (start is the right in a right-to-left
        // line). A longer line is scrolled from its inline start. The line is centred vertically.
        [[nodiscard]] core::Float2 TextOrigin(const Rect& content) const override
        {
            const core::Float2 origin = ShownText().Origin(Rect{content.x, content.y, 0.0f, content.height});
            const TextLayout& layout = EditLayout();
            const f32 view = View(content);
            if (layout.Width <= view)
                return core::Float2{origin.x + Slack(layout, view - layout.Width), origin.y};
            return core::Float2{origin.x - ScrollFromLeft(layout, view), origin.y};
        }
        // Scrolls the least amount that shows the caret, and no further than the text needs. When
        // the field is not focused, a line too long for it rests at its start, so a value shows its
        // beginning (-9.81, not 81) wherever the caret was left.
        void KeepCaretInView() override
        {
            if (!IsFocused())
            {
                if (m_scrollX != 0.0f)
                {
                    m_scrollX = 0.0f;
                    Invalidate();
                }
                return;
            }
            const TextLayout& layout = EditLayout();
            const f32 view = View(EditBounds());
            const f32 caret = layout.CaretX(DisplayCaret());
            const f32 before = m_scrollX;
            f32 left = ScrollFromLeft(layout, view);
            if (caret - left > view)
                left = caret - view;
            if (caret < left)
                left = caret;
            const f32 range = core::Max(0.0f, layout.Width - view);
            left = core::Clamp(left, 0.0f, range);
            m_scrollX = RunsRightToLeft(layout) ? range - left : left;
            if (m_scrollX != before)
                Invalidate();
        }

    private:
        [[nodiscard]] static f32 View(const Rect& content) { return core::Max(0.0f, content.width - kCaretWidth); }
        // Returns whether the line's paragraph runs right to left; while the text is empty, the
        // field's own direction decides.
        [[nodiscard]] bool RunsRightToLeft(const TextLayout& layout) const
        {
            return layout.Lines.IsEmpty() ? IsRightToLeft() : layout.Lines[0].RightToLeft;
        }
        // Converts the kept inline-start scroll into how far the layout moves left.
        [[nodiscard]] f32 ScrollFromLeft(const TextLayout& layout, f32 view) const
        {
            const f32 range = core::Max(0.0f, layout.Width - view);
            const f32 fromStart = core::Clamp(m_scrollX, 0.0f, range);
            return RunsRightToLeft(layout) ? range - fromStart : fromStart;
        }
        // Returns how far a short line sits from the left, out of the `slack` it leaves.
        [[nodiscard]] f32 Slack(const TextLayout& layout, f32 slack) const
        {
            TextHAlign align = ShownText().GetHAlign();
            if (align == TextHAlign::Start)
                align = RunsRightToLeft(layout) ? TextHAlign::Right : TextHAlign::Left;
            else if (align == TextHAlign::End)
                align = RunsRightToLeft(layout) ? TextHAlign::Left : TextHAlign::Right;
            return align == TextHAlign::Center ? slack * 0.5f : align == TextHAlign::Right ? slack : 0.0f;
        }

    private:
        f32 m_scrollX = 0.0f;
        bool m_password = false;
    };
}
