// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :text_area partition
//
// TextArea: the multiline text field, using the same editing model and keys as TextField
// (TextEditBase). The text wraps at the area's width and keeps spaces and line breaks as typed
// (white-space: pre-wrap). Return breaks a line. Up and Down move by displayed lines, keeping the
// column they started from, and Page Up and Page Down move by as many lines as fit. Home and End
// go to the ends of a displayed line. The area is `rows` lines tall unless something else sizes
// it, and it scrolls vertically to keep the caret in view; the wheel scrolls it too. It declares
// the key context "textinput multiline", where the multiline keys are bound, and Ctrl+Return
// submits it.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.gui:text_area;

import foundation.core; // Float2, Max, Min
import :rect;
import :event;
import :text;
import :text_segmentation; // TextRange
import :text_edit_model;   // ITextLines
import :text_edit_base;
import :commands;          // CommandEvent
import :text_editing_keys; // kTextSubmitCommand

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::gui
{
    class TextArea : public TextEditBase
    {
        RTTI_OBJECT(TextArea, TextEditBase)
    public:
        TextArea()
        {
            SetTag(core::StringView(u8"textarea"));
            SetKeyContext(core::StringView(u8"textinput multiline"));
            ShownText().SetWhiteSpace(TextWhiteSpace::PreWrap);
            HintText().SetWhiteSpace(TextWhiteSpace::Normal);
            GetModel().SetMultiline(true);
            GetModel().SetLines(&m_lines);
            (void)OnCommand(core::StringView(kTextSubmitCommand), [this](CommandEvent&) { Submit(); });
        }
        TextArea(const TextArea&) = delete;
        TextArea& operator=(const TextArea&) = delete;

        /// Sets how many lines tall the area is when nothing else sizes it.
        void SetRows(i32 rows)
        {
            m_rows = core::Max(1, rows);
            RequestLayout();
        }
        [[nodiscard]] i32 GetRows() const noexcept { return m_rows; }
        /// Returns how far the text is scrolled down, which keeps the caret in view.
        [[nodiscard]] f32 GetScrollY() const noexcept { return m_scrollY; }

        /// Measures a text area's usual width and `rows` lines of height.
        [[nodiscard]] core::Float2 MeasureContent(core::Float2) override
        {
            const f32 line = ShownText().GetFont() != nullptr ? ShownText().GetLineHeight() : 20.0f;
            return core::Float2{240.0f, line * static_cast<f32>(m_rows)};
        }
        /// Returns the size of the text wrapped at `width` (the widest line by the total height),
        /// at least one line tall. A box that grows with the typed text (an editing EditableLabel)
        /// measures this.
        [[nodiscard]] core::Float2 MeasureText(f32 width) const
        {
            const f32 line = ShownText().GetFont() != nullptr ? ShownText().GetLineHeight() : 20.0f;
            if (GetText().Size() == 0 || ShownText().GetFont() == nullptr)
                return core::Float2{0.0f, line};
            const core::Float2 size = ShownText().MeasureWrapped(width);
            return core::Float2{size.x, core::Max(size.y, line)};
        }
        /// Returns the first line's baseline.
        [[nodiscard]] f32 BaselineAt(core::Float2 size) const override
        {
            const Thickness c = GetChrome();
            const TextLayout& layout = ShownText().LayoutFor(Rect{0.0f, 0.0f, core::Max(0.0f, size.x - c.TotalHorizontal()), 0.0f});
            const f32 first = layout.Lines.IsEmpty() ? 0.0f : layout.Lines[0].Baseline;
            return c.Top + first - m_scrollY;
        }

    protected:
        [[nodiscard]] const TextLayout& EditLayout() const override
        {
            return ShownText().LayoutFor(Rect{0.0f, 0.0f, GetContentBounds().width, 0.0f});
        }
        [[nodiscard]] core::Float2 TextOrigin(const Rect& content) const override
        {
            return core::Float2{content.x, content.y - m_scrollY};
        }
        // Scrolls the least amount that shows the caret's line, and no further than the text needs.
        void KeepCaretInView() override
        {
            const f32 view = GetContentBounds().height;
            const TextLayout& layout = EditLayout();
            const LaidOutLine line = LineAt(layout, DisplayCaret());
            const f32 before = m_scrollY;
            if (line.Top + line.Height - m_scrollY > view)
                m_scrollY = line.Top + line.Height - view;
            if (line.Top < m_scrollY)
                m_scrollY = line.Top;
            m_scrollY = core::Max(0.0f, core::Min(m_scrollY, layout.Height - view));
            if (m_scrollY != before)
                Invalidate();
        }
        // The wheel scrolls a few lines per notch. At an end the wheel event is left unhandled, so
        // it passes on to the area's ancestors.
        void OnMouseWheel(const WheelEvent& event) override
        {
            const f32 view = GetContentBounds().height;
            const f32 most = core::Max(0.0f, EditLayout().Height - view);
            const f32 before = m_scrollY;
            m_scrollY = core::Max(0.0f, core::Min(most, m_scrollY - event.Delta.y * ShownText().GetLineHeight() * 3.0f));
            if (m_scrollY != before)
            {
                event.Handled = true;
                Invalidate();
            }
        }

    private:
        // The displayed lines, which vertical caret movement and Home and End follow.
        class ShownLines : public ITextLines
        {
        public:
            explicit ShownLines(TextArea& area) : m_area(area) {}

            [[nodiscard]] TextRange LineAt(usize offset) const override
            {
                const TextLayout& layout = m_area.EditLayout();
                if (layout.Lines.IsEmpty())
                    return {};
                const usize index = layout.LineOf(m_area.DisplayOffset(offset));
                const LaidOutLine& line = layout.Lines[index];
                // A wrapped line ends where the next line starts, so End stops before the space
                // where it broke.
                const usize end = layout.OffsetOnLine(index, 1.0e9f);
                return TextRange{m_area.ModelOffset(line.Start), m_area.ModelOffset(core::Max(line.Start, end))};
            }
            [[nodiscard]] usize OffsetByLines(usize offset, i32 lines, f32& goalX) const override
            {
                const TextLayout& layout = m_area.EditLayout();
                if (layout.Lines.IsEmpty())
                    return 0;
                const usize display = m_area.DisplayOffset(offset);
                if (goalX < 0.0f)
                    goalX = layout.CaretX(display);
                const i32 target = static_cast<i32>(layout.LineOf(display)) + lines;
                if (target < 0)
                    return 0;
                if (target >= static_cast<i32>(layout.Lines.Size()))
                    return m_area.GetModel().GetText().Size();
                return m_area.ModelOffset(layout.OffsetOnLine(static_cast<usize>(target), goalX));
            }
            [[nodiscard]] i32 LinesPerPage() const override
            {
                const f32 line = m_area.ShownText().GetLineHeight();
                return line > 0.0f ? core::Max(1, static_cast<i32>(m_area.GetContentBounds().height / line)) : 1;
            }

        private:
            TextArea& m_area;
        };

        ShownLines m_lines{*this};
        i32 m_rows = 3;
        f32 m_scrollY = 0.0f;
    };
}
