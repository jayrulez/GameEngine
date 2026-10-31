// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :list_box partition
//
// ListBox: a scrollable, single-selection list of text items. It hosts a ScrollView whose content
// holds one ListBoxItem (a selectable Label) per entry, stacked by index. Clicking a row selects
// it; when the list is focused, Up and Down move the selection and scroll it into view. The
// ScrollView provides the scroll bar, wheel scrolling and clipping. Every item is a real row
// widget, so for very large lists use ListView, which is virtualised.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.gui:list_box;

import foundation.core;  // RefPtr, MakeRef, Array, Function, Move, String, Max, Min
import foundation.fonts; // CachedFont
import :rect;
import :event;
import :drawable;
import :label;
import :ui_widget;
import :layout_style;

using namespace foundation::core;
namespace core = foundation::core;
namespace fonts = foundation::fonts;

export namespace foundation::gui
{
    /// Splits a list's items written as one string with '|' between them (markup's
    /// `items="Apple|Banana"`), trimming each. Empty text gives no items.
    [[nodiscard]] inline Array<core::StringView> SplitItems(core::StringView text)
    {
        Array<core::StringView> items;
        usize start = 0;
        for (usize i = 0; i <= text.Size(); ++i)
            if (i == text.Size() || text[i] == u8'|')
            {
                const core::StringView item = core::Trim(text.SubStr(start, i - start));
                if (item.Size() != 0 || i < text.Size())
                    items.PushBack(item);
                start = i + 1;
            }
        return items;
    }
    [[nodiscard]] inline core::String JoinItems(const Array<core::String>& items)
    {
        core::String text;
        for (usize i = 0; i < items.Size(); ++i)
        {
            if (i > 0)
                text.Append(core::StringView(u8"|"));
            text.Append(items[i].AsView());
        }
        return text;
    }

    // One selectable row, internal to the ListBox. It is a Label tagged "listboxitem" so a sheet can
    // style the rows (`listboxitem:hover`) separately from other labels around the list.
    class ListBoxItem : public Label
    {
        RTTI_OBJECT(ListBoxItem, Label)
    public:
        ListBoxItem()
        {
            SetTag(core::StringView(u8"listboxitem"));
            StyleWriteScope defaults(*this); // defaults: a style sheet overrides them and falls back to them
            SetTextAlignment(TextHAlign::Start, TextVAlign::Middle);
        }

        void SetIndex(i32 index) noexcept { m_index = index; }
        [[nodiscard]] i32 GetIndex() const noexcept { return m_index; }
        void SetOnPicked(core::Function<void(i32)> callback) { m_onPicked = core::Move(callback); }

        void SetSelectedRow(bool selected)
        {
            if (selected == m_selected)
                return;
            m_selected = selected;
            Invalidate();
        }
        void SetHighlightColor(Color color)
        {
            m_highlight = color;
            Invalidate();
        }
        /// Sets the list's listbox::selection style, which a selected row draws under its text.
        void SetSelectionStyle(const PartStyle& style)
        {
            m_selectionStyle = style;
            Invalidate();
        }

    protected:
        void OnMouseClick(const MouseEvent& event) override
        {
            event.Handled = true;
            if (m_onPicked)
                m_onPicked(m_index);
        }

    protected:
        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            if (m_selected)
                m_selectionStyle.Draw(ctx, GetLocalBounds(), FilledBox(m_highlight));
            Label::OnDraw(ctx, localBounds);
        }

    private:
        i32 m_index = -1;
        bool m_selected = false;
        Color m_highlight{0.24f, 0.40f, 0.62f, 1.0f};
        PartStyle m_selectionStyle;
        core::Function<void(i32)> m_onPicked;
    };

    class ListBox : public UIWidget
    {
        RTTI_OBJECT(ListBox, UIWidget)
    public:
        ListBox()
        {
            SetTag(core::StringView(u8"listbox"));
            SetTabFocusable(true);
            // The rows stack in a column as wide as the list and scroll when they overflow it.
            StyleWriteScope defaults(*this); // defaults: a style sheet overrides them and falls back to them
            EditLayoutStyle([](LayoutStyle& s) { s.OverflowY = Overflow::Auto; });
            SetTextColor(Color{0.88f, 0.90f, 0.94f, 1.0f});
        }

        void SetItemHeight(f32 height)
        {
            m_itemHeight = core::Max(1.0f, height);
            for (ListBoxItem* item : m_items)
                item->EditLayoutStyle([this](LayoutStyle& s) { s.Height = LayoutLength::Px(m_itemHeight); });
        }
        [[nodiscard]] f32 GetItemHeight() const noexcept { return m_itemHeight; }

        void AddItem(core::StringView text)
        {
            auto item = core::MakeRef<ListBoxItem>(MemoryAllocator());
            item->SetText(text);
            ShareTextStyle(*item);
            item->SetHighlightColor(m_highlightColor);
            item->SetSelectionStyle(GetPartStyle(u8"selection"));
            StyleWriteScope defaults(*item); // defaults: a style sheet overrides them and falls back to them
            item->SetPadding(Thickness{8.0f, 0.0f, 8.0f, 0.0f});
            item->SetIndex(static_cast<i32>(m_items.Size()));
            ListBox* self = this;
            item->SetOnPicked([self](i32 index)
                              {
                                  self->SetSelectedIndex(index);
                                  if (self->m_onActivated)
                                      self->m_onActivated(index);
                              });
            item->EditLayoutStyle([this](LayoutStyle& s) { s.Height = LayoutLength::Px(m_itemHeight); });
            AddChild(item.Get());
            m_items.PushBack(item.Get());
        }

        void Clear()
        {
            RemoveAllChildren();
            m_items.Clear();
            m_selected = -1;
        }

        [[nodiscard]] usize ItemCount() const noexcept { return m_items.Size(); }
        /// Replaces the items with those in `text`, separated by '|'. Markup and style sheets set
        /// `items` through this.
        void SetItemsText(const core::String& text)
        {
            Clear();
            for (const core::StringView item : SplitItems(text.AsView()))
                AddItem(item);
        }
        [[nodiscard]] core::String GetItemsText() const
        {
            core::String text;
            for (usize i = 0; i < m_items.Size(); ++i)
            {
                if (i > 0)
                    text.Append(core::StringView(u8"|"));
                text.Append(m_items[i]->GetText());
            }
            return text;
        }
        [[nodiscard]] core::StringView GetItem(usize index) const
        {
            return index < m_items.Size() ? m_items[index]->GetText() : core::StringView{};
        }

        [[nodiscard]] i32 GetSelectedIndex() const noexcept { return m_selected; }
        void SetSelectedIndex(i32 index)
        {
            if (index < -1 || index >= static_cast<i32>(m_items.Size()))
                return;
            if (index == m_selected)
                return;
            if (m_selected >= 0)
                m_items[static_cast<usize>(m_selected)]->SetSelectedRow(false);
            m_selected = index;
            if (m_selected >= 0)
            {
                m_items[static_cast<usize>(m_selected)]->SetSelectedRow(true);
                ScrollToItem(m_selected);
            }
            if (m_onChanged)
                m_onChanged(m_selected);
        }
        void SetOnSelectionChanged(core::Function<void(i32)> callback)
        {
            m_onChanged = core::Move(callback);
        }
        /// Sets the callback for an item that is chosen, not just selected: clicked, or picked with
        /// Return, Space or the pad's South button on the list.
        void SetOnItemActivated(core::Function<void(i32)> callback) { m_onActivated = core::Move(callback); }

        // The listbox::selection part is the highlight behind the selected row; its text style also
        // applies to the rows.
        void CollectStyleParts(core::Array<core::StringView>& out) const override
        {
            out.PushBack(core::StringView(u8"selection"));
        }


    protected:
        // Paint an opaque background so the list, and any dropdown built on it, covers whatever is
        // behind it, unless a style sheet gives it a fill.
        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            if (!BoxFills())
                ctx.VG().FillRect(Rectangle{localBounds.x, localBounds.y, localBounds.width, localBounds.height}, m_panelColor);
            UIWidget::OnDraw(ctx, localBounds);
        }
        // The items use the list's font and colour (sheet inheritance gives them the same), and
        // the rows draw the selection in the listbox::selection style.
        void OnPartStyleChange(core::StringView part) override
        {
            if (part == core::StringView(u8"selection"))
                for (ListBoxItem* item : m_items)
                    item->SetSelectionStyle(GetPartStyle(part));
        }
        void OnTextStyleChange() override
        {
            for (ListBoxItem* item : m_items)
                ShareTextStyle(*item);
        }


        bool OnActivate() override
        {
            if (m_selected < 0 || !m_onActivated)
                return false;
            m_onActivated(m_selected);
            return true;
        }

        void OnKeyDown(const KeyEvent& event) override
        {
            if (event.Target != this)
                return;
            switch (static_cast<KeyCode>(event.KeyCode))
            {
            // An arrow key at either end of the list is left unhandled, so focus can move on.
            case KeyCode::Down:
                if (!m_items.IsEmpty() && m_selected < static_cast<i32>(m_items.Size()) - 1)
                {
                    SetSelectedIndex(m_selected + 1);
                    event.Handled = true;
                }
                break;
            case KeyCode::Up:
                if (!m_items.IsEmpty() && m_selected > 0)
                {
                    SetSelectedIndex(m_selected - 1);
                    event.Handled = true;
                }
                break;
            case KeyCode::Home:
                if (!m_items.IsEmpty())
                    SetSelectedIndex(0);
                event.Handled = true;
                break;
            case KeyCode::End:
                if (!m_items.IsEmpty())
                    SetSelectedIndex(static_cast<i32>(m_items.Size()) - 1);
                event.Handled = true;
                break;
            default:
                break;
            }
        }

    private:
        // Scrolls the least amount that brings row `index` into view. The row's position comes from
        // its index, since the rows may not be laid out yet.
        void ScrollToItem(i32 index)
        {
            const f32 top = static_cast<f32>(index) * m_itemHeight;
            const f32 bottom = top + m_itemHeight;
            const f32 viewH = GetContentBounds().height;
            const core::Float2 off = GetScrollOffset();
            if (top < off.y)
                SetScrollOffset(core::Float2{off.x, top});
            else if (bottom > off.y + viewH)
                SetScrollOffset(core::Float2{off.x, bottom - viewH});
        }

        Array<ListBoxItem*> m_items; // non-owning; the rows are owned as children
        f32 m_itemHeight = 24.0f;
        i32 m_selected = -1;
        Color m_panelColor{0.13f, 0.14f, 0.17f, 1.0f};
        Color m_highlightColor{0.24f, 0.40f, 0.62f, 1.0f};
        core::Function<void(i32)> m_onChanged;
        core::Function<void(i32)> m_onActivated;
    };

    RTTI_DEFINE_OBJECT(ListBoxItem, "rtti::gui")
}
