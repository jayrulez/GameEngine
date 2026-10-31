// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :label partition
//
// Label: a UIWidget that displays text. It draws a Text primitive inside the content bounds (the
// bounds inset by the padding). :text_layout shapes and lays out the text: it wraps according to
// white-space, applies line-height and letter-spacing, ends an overflowing line according to
// text-overflow, and clamps the number of lines. A rich label's text is markup, and a click on one
// of its links sends an event.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.gui:label;

import foundation.core;  // StringView, Color, Float2
import foundation.fonts; // CachedFont
import :reactive; // Observable, which bindings follow
import :rect;
import :draw_context;
import :text;
import :ui_widget;
import :layout_style; // kUnbounded
import :event;        // LinkEvent

using namespace foundation::core;
namespace core = foundation::core;
namespace fonts = foundation::fonts;

export namespace foundation::gui
{
    /// CSS's text-overflow keyword for `overflow` ("clip", "ellipsis", "ellipsis-start", "ellipsis-middle").
    [[nodiscard]] inline core::StringView TextOverflowName(TextOverflow overflow) noexcept
    {
        switch (overflow)
        {
        case TextOverflow::Ellipsis: return core::StringView(u8"ellipsis");
        case TextOverflow::EllipsisStart: return core::StringView(u8"ellipsis-start");
        case TextOverflow::EllipsisMiddle: return core::StringView(u8"ellipsis-middle");
        default: return core::StringView(u8"clip");
        }
    }
    /// Returns the overflow a keyword names, or clip for an unknown keyword.
    [[nodiscard]] inline TextOverflow TextOverflowNamed(core::StringView name) noexcept
    {
        if (name == core::StringView(u8"ellipsis"))
            return TextOverflow::Ellipsis;
        if (name == core::StringView(u8"ellipsis-start"))
            return TextOverflow::EllipsisStart;
        if (name == core::StringView(u8"ellipsis-middle"))
            return TextOverflow::EllipsisMiddle;
        return TextOverflow::Clip;
    }

    class Label : public UIWidget
    {
        RTTI_OBJECT(Label, UIWidget)
    public:
        Label()
        {
            SetTag(core::StringView(u8"label"));
            StyleWriteScope defaults(*this); // defaults: a style sheet overrides them and falls back to them
            SetTextAlignment(TextHAlign::Start, TextVAlign::Middle);
        }

        /// Sets the byte range of the text that is the access key, underlined while access keys
        /// are shown; start == end means none. A button sets this after removing the `&` from its
        /// text.
        void SetMnemonicRange(usize start, usize end)
        {
            m_mnemonicStart = start;
            m_mnemonicEnd = end;
            if (end <= start)
                m_text.SetUnderline(0, 0);
            Invalidate();
        }
        /// Sets the text: plain text, or markup when the label is rich (SetRich).
        void SetText(core::StringView text)
        {
            if (m_for.Size() != 0 && !m_rich)
            {
                // A label for a field (`for`): the `&` marks the access key and is removed from the
                // shown text.
                m_written = core::String(text);
                const Mnemonic mnemonic = ParseMnemonic(text);
                m_mnemonic = mnemonic.Key;
                SetMnemonicRange(mnemonic.Start, mnemonic.End);
                if (mnemonic.Shown.AsView() == m_text.GetString())
                    return;
                m_text.SetString(mnemonic.Shown.AsView());
                RequestLayout();
                Invalidate();
                m_textChanged.Notify();
                return;
            }
            m_written = core::String(text);
            if (text == m_text.GetString())
                return;
            if (m_rich)
                m_text.SetMarkup(text);
            else
                m_text.SetString(text);
            RequestLayout(); // the label's size follows its text
            Invalidate();
            m_textChanged.Notify();
        }
        /// Returns the shown text. A binding that reads it follows later changes.
        [[nodiscard]] core::StringView GetText() const
        {
            m_textChanged.Track();
            return m_text.GetString();
        }

        /// Makes the label rich: its text is then markup (:text_runs) with runs in their own
        /// colours, sizes, faces and effects, underlines, inline icons and links. A click on a link
        /// sends a `link` event with the link's id. Markup sets this with `rich="true"` or the
        /// RichText element.
        void SetRich(bool rich)
        {
            if (rich == m_rich)
                return;
            m_rich = rich;
            const core::String text(m_text.GetString());
            if (rich)
                m_text.SetMarkup(text.AsView());
            else
                m_text.SetString(text.AsView());
            RequestLayout();
            Invalidate();
        }
        [[nodiscard]] bool IsRich() const noexcept { return m_rich; }

        /// Sets the id of the field this label is for (HTML's `for`). The `&` in the text then marks
        /// an access key, and Alt with that key (or the key alone inside an open menu) focuses the
        /// field. An empty id means the label is for no field and its text is shown as written.
        void SetFor(const core::String& id)
        {
            if (id == m_for)
                return;
            m_for = id;
            m_mnemonic = 0;
            SetMnemonicRange(0, 0);
            const core::String written = m_written;
            m_text.SetString(core::StringView{}); // so SetText below rebuilds the shown text from m_written
            SetText(written.AsView());
        }
        [[nodiscard]] core::String GetFor() const { return m_for; }
        [[nodiscard]] char32_t GetMnemonic() const override { return m_for.Size() != 0 ? m_mnemonic : 0; }

        // Markup: <Label text="Hello" rich="true">.
        bool SetMarkupAttribute(core::StringView name, core::StringView value) override
        {
            if (name == core::StringView(u8"text"))
            {
                SetText(value);
                return true;
            }
            if (name == core::StringView(u8"rich"))
            {
                SetRich(value == core::StringView(u8"true"));
                return true;
            }
            if (name == core::StringView(u8"for"))
            {
                SetFor(core::String(value));
                return true;
            }
            return UIWidget::SetMarkupAttribute(name, value);
        }

        /// Sets CSS text-overflow: what shows where a line is wider than the label (clip, or an
        /// ellipsis at the end, start or middle). Not inherited.
        void SetTextOverflow(TextOverflow overflow)
        {
            m_text.SetOverflow(overflow);
            Invalidate();
        }
        [[nodiscard]] TextOverflow GetTextOverflow() const noexcept { return m_text.GetOverflow(); }
        void SetTextOverflowName(const core::String& name) { SetTextOverflow(TextOverflowNamed(Trim(name.AsView()))); }
        [[nodiscard]] core::String GetTextOverflowName() const { return core::String(TextOverflowName(GetTextOverflow())); }
        /// Sets CSS line-clamp: the most lines shown, 0 for all. The last line ends in an ellipsis
        /// when text-overflow uses one. Not inherited.
        void SetLineClamp(i32 lines)
        {
            m_text.SetMaxLines(static_cast<u32>(core::Max(0, lines)));
            RequestLayout();
            Invalidate();
        }
        [[nodiscard]] i32 GetLineClamp() const noexcept { return static_cast<i32>(m_text.GetMaxLines()); }

        // The natural size of the text (for layout).
        [[nodiscard]] core::Float2 MeasureText() const { return m_text.Measure(); }
        /// Measures the shaped text. It is one line unless white-space wraps, in which case it wraps
        /// to the available width (to its longest word when no width is given). The line clamp
        /// applies.
        [[nodiscard]] core::Float2 MeasureContent(core::Float2 available) override
        {
            SyncServices();
            if (m_text.GetFont() == nullptr || m_text.IsEmpty())
                return core::Float2{0.0f, (m_text.GetFont() != nullptr) ? m_text.GetLineHeight() : 0.0f};
            return m_text.MeasureWrapped(available.x);
        }
        /// Returns the first baseline of the text as drawn in the content box.
        [[nodiscard]] f32 BaselineAt(core::Float2 size) const override
        {
            const Thickness c = GetChrome();
            return m_text.FirstBaseline(Rect{c.Left, c.Top, core::Max(0.0f, size.x - c.TotalHorizontal()),
                                             core::Max(0.0f, size.y - c.TotalVertical())});
        }
        // The size wrapped text occupies at `maxWidth` (widest line x total height).
        [[nodiscard]] core::Float2 MeasureWrapped(f32 maxWidth) const { return m_text.MeasureWrapped(maxWidth); }
        /// Returns the Text primitive the label lays out and draws.
        [[nodiscard]] const Text& GetTextLayout() const noexcept { return m_text; }

    protected:
        // The text is drawn in the label's text style, whether set directly, by a sheet, or inherited.
        void OnTextStyleChange() override
        {
            const TextStyle& style = GetTextStyle();
            m_text.SetFont(GetFont());
            m_text.SetFontSelection(GetFontService(), GetTextStyle().FontFamily.AsView(), GetTextStyle().Selection);
            m_text.SetColor(GetTextColor());
            m_text.SetAlignment(GetTextAlignH(), GetTextAlignV());
            m_text.SetDirection(TextDirectionFor());
            m_text.SetWhiteSpace(style.WhiteSpace);
            m_text.SetLineHeight(style.LineHeightPixels());
            m_text.SetLetterSpacing(style.LetterSpacing);
            m_text.SetBreakWords(style.BreakWords);
        }

        /// Returns the box, extended by the laid-out text where a line passes the box, unless the
        /// label clips its text.
        [[nodiscard]] Rect GetInkBounds() const override
        {
            const Rect ink = UIWidget::GetInkBounds();
            if (ClipsChildren() || !m_text.HasFont() || m_text.IsEmpty())
                return ink;
            const Rect bounds = GetContentBounds();
            const TextLayout& layout = m_text.LayoutFor(bounds);
            const core::Float2 origin = m_text.Origin(bounds);
            return Rect::Union(ink, Rect{origin.x, origin.y, core::Max(layout.Width, bounds.width), layout.Height});
        }

        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            (void)localBounds;
            SyncServices();
            // Underline the access key while access keys are shown.
            if (m_mnemonicEnd > m_mnemonicStart)
            {
                const bool shown = AreMnemonicsShown();
                m_text.SetUnderline(shown ? m_mnemonicStart : 0, shown ? m_mnemonicEnd : 0);
            }
            // Any overflow other than visible also clips the label's own text, as CSS clips an
            // element's content.
            const bool clip = ClipsChildren();
            if (clip)
                ctx.PushClip(GetLocalBounds());
            m_text.Draw(ctx, GetContentBounds()); // aligned within the padding-inset bounds
            if (clip)
                ctx.PopClip();
        }

        // Over a link in rich text the pointer shows the hand, as in a browser.
        [[nodiscard]] Cursor GetCursorNow() const override { return m_overLink ? Cursor::Pointer : UIWidget::GetCursorNow(); }
        void OnMouseMove(const MouseEvent& event) override
        {
            m_overLink = m_rich && m_text.LinkAt(GetContentBounds(), ConvertToNodeSpace(event.Position)).Size() != 0;
        }
        void OnMouseLeave(const MouseEvent&) override { m_overLink = false; }

        // A click on a link in rich text sends a LinkEvent.
        void OnMouseClick(const MouseEvent& event) override
        {
            if (!m_rich)
                return;
            const core::StringView link = m_text.LinkAt(GetContentBounds(), ConvertToNodeSpace(event.Position));
            if (link.Size() == 0)
                return;
            event.Handled = true;
            const core::String id(link); // copied, since a relayout could invalidate the view
            SendEvent(LinkEvent(this, id.AsView()));
        }

        // Gives rich text the GUI's font service and resource provider, which its faces and icons need.
        void SyncServices()
        {
            if (!m_rich)
                return;
            m_text.SetFontService(GetFontService());
            m_text.SetResourceProvider(GetResourceProvider());
        }

        // The access key focuses the labelled field; the label itself takes no focus.
        bool OnActivate() override
        {
            if (m_for.Size() == 0)
                return false;
            Node* top = this;
            while (top->GetParent() != nullptr)
                top = top->GetParent();
            const auto find = [this](auto& self, Node& node) -> Node*
            {
                if (const UIWidget* w = core::Cast<UIWidget>(&node); w != nullptr && w != this && w->GetId() == m_for.AsView())
                    return &node;
                for (usize i = 0; i < node.ChildCount(); ++i)
                    if (Node* found = self(self, *node.GetChildAt(i)))
                        return found;
                return nullptr;
            };
            Node* field = find(find, *top);
            if (field == nullptr || !field->IsTreeEnabled() || !field->IsTabFocusable())
                return false;
            field->RequestFocus();
            return true;
        }

        Text m_text;
        core::String m_written; // the text as written; a label for a field shows it without the `&`
        core::String m_for;     // id of the field this label is for
        char32_t m_mnemonic = 0;
        bool m_rich = false;
        bool m_overLink = false; // the pointer is over one of the label's links
        usize m_mnemonicStart = 0; // byte range of the access key, underlined while access keys are shown
        usize m_mnemonicEnd = 0;
        Observable m_textChanged; // notifies bindings that read the text
    };

    /// A Label with rich text switched on, for markup's <RichText text="...">.
    class RichText : public Label
    {
        RTTI_OBJECT(RichText, Label)
    public:
        RichText()
        {
            SetTag(core::StringView(u8"richtext"));
            SetRich(true);
        }
    };
}
