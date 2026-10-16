// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :list_box partition
//
// ListBox: a scrollable, single-selection list of text items. Modeled on eepp's UIListBox
// (role only). Built by composition on the existing pieces: it hosts a ScrollView whose
// content holds one ListBoxItem (a selectable Label) per entry, stacked by index. Clicking a
// row selects it; when focused, Up/Down move the selection and scroll it into view. The
// ScrollView brings the scrollbar / wheel / clipping for free. Virtualized item rendering (for
// very large lists) is a follow-up - here every item is a real row widget.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module experimental.gui:list_box;

import foundation.core;  // RefPtr, MakeRef, Array, Function, Move, String, Max, Min
import foundation.fonts; // CachedFont
import :rect;
import :event;
import :drawable;
import :rectangle_drawable;
import :label;
import :ui_widget;
import :scroll_view;

using namespace foundation::core;
namespace core = foundation::core;
namespace fonts = foundation::fonts;

export namespace experimental::gui
{
    /// A list's items written as one text, '|' between them (markup's `items="Apple|Banana"`),
    /// each trimmed; empty text, no items.
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

    // One selectable row. Internal to the ListBox, exposed as a Label for styling.
    class ListBoxItem : public Label
    {
        RTTI_OBJECT(ListBoxItem, Label)
    public:
        ListBoxItem() { SetTextAlignment(TextHAlign::Left, TextVAlign::Middle); }

        void SetIndex(i32 index) noexcept { m_index = index; }
        [[nodiscard]] i32 GetIndex() const noexcept { return m_index; }
        void SetOnPicked(core::Function<void(i32)> callback) { m_onPicked = core::Move(callback); }

        void SetSelectedRow(bool selected)
        {
            if (selected == m_selected)
                return;
            m_selected = selected;
            SetBackground(selected ? core::RefPtr<Drawable>(core::MakeRef<RectangleDrawable>(
                                         MemoryAllocator(), m_highlight))
                                   : core::RefPtr<Drawable>());
            Invalidate();
        }
        void SetHighlightColor(Color color)
        {
            m_highlight = color;
            if (m_selected)
                SetBackground(core::MakeRef<RectangleDrawable>(MemoryAllocator(),
                                                               color)); // refresh live
        }

    protected:
        void OnMouseClick(const MouseEvent&) override
        {
            if (m_onPicked)
                m_onPicked(m_index);
        }

    private:
        i32 m_index = -1;
        bool m_selected = false;
        Color m_highlight{0.24f, 0.40f, 0.62f, 1.0f};
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
            // Opaque panel background so the list (and any dropdown built on it) paints over
            // whatever is behind it. Override with SetBackground for a themed look.
            SetBackground(core::MakeRef<RectangleDrawable>(MemoryAllocator(), m_panelColor));
            m_scroll = core::MakeRef<ScrollView>(MemoryAllocator());
            AddChild(m_scroll.Get());
        }

        void SetFont(fonts::CachedFont* font)
        {
            m_font = font;
            for (ListBoxItem* it : m_items)
                it->SetFont(font);
        }
        void SetThemeFont(fonts::CachedFont* font) override { SetFont(font); } // CSS font-family and font-size
        [[nodiscard]] fonts::CachedFont* GetFont() const { return m_font; }
        void SetItemHeight(f32 height)
        {
            m_itemHeight = core::Max(1.0f, height);
            Relayout();
        }
        [[nodiscard]] f32 GetItemHeight() const noexcept { return m_itemHeight; }

        void AddItem(core::StringView text)
        {
            auto item = core::MakeRef<ListBoxItem>(MemoryAllocator());
            item->SetText(text);
            item->SetFont(m_font);
            item->SetTextColor(m_textColor);
            item->SetHighlightColor(m_highlightColor);
            item->SetPadding(Thickness{8.0f, 0.0f, 8.0f, 0.0f});
            item->SetIndex(static_cast<i32>(m_items.Size()));
            ListBox* self = this;
            item->SetOnPicked([self](i32 index) { self->SetSelectedIndex(index); });
            m_scroll->GetContent()->AddChild(item.Get());
            m_items.PushBack(item.Get());
            Relayout();
        }

        void Clear()
        {
            m_scroll->GetContent()->RemoveAllChildren();
            m_items.Clear();
            m_selected = -1;
            Relayout();
        }

        [[nodiscard]] usize ItemCount() const noexcept { return m_items.Size(); }
        /// The items as one text, '|' between them (markup's and a sheet's `items`).
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
                ScrollIntoView(m_selected);
            }
            if (m_onChanged)
                m_onChanged(m_selected);
        }
        void SetOnSelectionChanged(core::Function<void(i32)> callback)
        {
            m_onChanged = core::Move(callback);
        }

        void SetTextColor(Color color)
        {
            m_textColor = color;
            for (ListBoxItem* it : m_items)
                it->SetTextColor(color);
        }

        // Theming: text color for all rows; listbox::selection = the row-selection highlight.
        void SetThemeTextColor(Color color) override { SetTextColor(color); }
        void CollectStyleParts(core::Array<core::StringView>& out) const override
        {
            out.PushBack(core::StringView(u8"selection"));
        }
        void SetThemePartColor(core::StringView part, Color color) override
        {
            if (part != core::StringView(u8"selection"))
                return;
            m_highlightColor = color;
            for (ListBoxItem* it : m_items)
                it->SetHighlightColor(color);
        }

        [[nodiscard]] ScrollView* GetScrollView() const noexcept { return m_scroll.Get(); }

    protected:
        void OnSizeChange() override { Relayout(); }

        void OnKeyDown(const KeyEvent& event) override
        {
            switch (static_cast<KeyCode>(event.KeyCode))
            {
            // An arrow at the list's end is given back (focus moves on).
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
        void Relayout()
        {
            const Rect box = GetContentBounds();
            m_scroll->SetPosition(core::Float2{box.x, box.y});
            m_scroll->SetSize(core::Float2{box.width, box.height});

            const f32 rowW = m_scroll->Viewport().width;
            for (usize i = 0; i < m_items.Size(); ++i)
            {
                m_items[i]->SetPosition(core::Float2{0.0f, static_cast<f32>(i) * m_itemHeight});
                m_items[i]->SetSize(core::Float2{rowW, m_itemHeight});
            }
            m_scroll->SetContentSize(
                core::Float2{rowW, static_cast<f32>(m_items.Size()) * m_itemHeight});
        }

        void ScrollIntoView(i32 index)
        {
            const f32 top = static_cast<f32>(index) * m_itemHeight;
            const f32 bottom = top + m_itemHeight;
            const f32 viewH = m_scroll->Viewport().height;
            const core::Float2 off = m_scroll->GetScrollOffset();
            if (top < off.y)
                m_scroll->SetScrollOffset(core::Float2{off.x, top});
            else if (bottom > off.y + viewH)
                m_scroll->SetScrollOffset(core::Float2{off.x, bottom - viewH});
        }

        RefPtr<ScrollView> m_scroll;
        Array<ListBoxItem*> m_items; // non-owning (owned by the scroll content)
        fonts::CachedFont* m_font = nullptr;
        f32 m_itemHeight = 24.0f;
        i32 m_selected = -1;
        Color m_panelColor{0.13f, 0.14f, 0.17f, 1.0f};
        Color m_textColor{0.88f, 0.90f, 0.94f, 1.0f};
        Color m_highlightColor{0.24f, 0.40f, 0.62f, 1.0f};
        core::Function<void(i32)> m_onChanged;
    };

    RTTI_DEFINE_OBJECT(ListBoxItem, "rtti::gui")
}
