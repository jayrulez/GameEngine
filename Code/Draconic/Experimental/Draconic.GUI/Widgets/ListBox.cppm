// Draconic GUI - :list_box partition
//
// ListBox: a scrollable, single-selection list of text items. Modeled on eepp's UIListBox
// (role only). Built by composition on the existing pieces: it hosts a ScrollView whose
// content holds one ListBoxItem (a selectable Label) per entry, stacked by index. Clicking a
// row selects it; when focused, Up/Down move the selection and scroll it into view. The
// ScrollView brings the scrollbar / wheel / clipping for free. Virtualized item rendering (for
// very large lists) is a follow-up - here every item is a real row widget.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.gui:list_box;

import draconic.foundation;  // RefPtr, MakeRef, Array, Function, Move, String, Max, Min
import draconic.fonts; // CachedFont
import :rect;
import :event;
import :drawable;
import :rectangle_drawable;
import :label;
import :ui_widget;
import :scroll_view;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;
namespace fonts = draconic::fonts;

export namespace draconic::gui
{
    // One selectable row. Internal to the ListBox, exposed as a Label for styling.
    class ListBoxItem : public Label
    {
        DRACONIC_OBJECT(ListBoxItem, Label)
    public:
        ListBoxItem() { SetTextAlignment(TextHAlign::Left, TextVAlign::Middle); }

        void SetIndex(i32 index) noexcept { m_index = index; }
        [[nodiscard]] i32 GetIndex() const noexcept { return m_index; }
        void SetOnPicked(foundation::Function<void(i32)> callback) { m_onPicked = foundation::Move(callback); }

        void SetSelectedRow(bool selected)
        {
            if (selected == m_selected)
                return;
            m_selected = selected;
            SetBackground(selected ? foundation::RefPtr<Drawable>(foundation::MakeRef<RectangleDrawable>(
                                         foundation::DefaultAllocator(), m_highlight))
                                   : foundation::RefPtr<Drawable>());
            Invalidate();
        }
        void SetHighlightColor(Color color)
        {
            m_highlight = color;
            if (m_selected)
                SetBackground(foundation::MakeRef<RectangleDrawable>(foundation::DefaultAllocator(),
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
        foundation::Function<void(i32)> m_onPicked;
    };

    class ListBox : public UIWidget
    {
        DRACONIC_OBJECT(ListBox, UIWidget)
    public:
        ListBox()
        {
            SetTag(foundation::StringView(u8"listbox"));
            SetTabFocusable(true);
            // Opaque panel background so the list (and any dropdown built on it) paints over
            // whatever is behind it. Override with SetBackground for a themed look.
            SetBackground(foundation::MakeRef<RectangleDrawable>(foundation::DefaultAllocator(), m_panelColor));
            m_scroll = foundation::MakeRef<ScrollView>(foundation::DefaultAllocator());
            AddChild(m_scroll.Get());
        }

        void SetFont(fonts::CachedFont* font)
        {
            m_font = font;
            for (ListBoxItem* it : m_items)
                it->SetFont(font);
        }
        void SetItemHeight(f32 height)
        {
            m_itemHeight = foundation::Max(1.0f, height);
            Relayout();
        }
        [[nodiscard]] f32 GetItemHeight() const noexcept { return m_itemHeight; }

        void AddItem(foundation::StringView text)
        {
            auto item = foundation::MakeRef<ListBoxItem>(foundation::DefaultAllocator());
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
        [[nodiscard]] foundation::StringView GetItem(usize index) const
        {
            return index < m_items.Size() ? m_items[index]->GetText() : foundation::StringView{};
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
        void SetOnSelectionChanged(foundation::Function<void(i32)> callback)
        {
            m_onChanged = foundation::Move(callback);
        }

        void SetTextColor(Color color)
        {
            m_textColor = color;
            for (ListBoxItem* it : m_items)
                it->SetTextColor(color);
        }

        // Theming: text color for all rows; listbox::selection = the row-selection highlight.
        void SetThemeTextColor(Color color) override { SetTextColor(color); }
        void CollectStyleParts(foundation::Array<foundation::StringView>& out) const override
        {
            out.PushBack(foundation::StringView(u8"selection"));
        }
        void SetThemePartColor(foundation::StringView part, Color color) override
        {
            if (part != foundation::StringView(u8"selection"))
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
            case KeyCode::Down:
                if (!m_items.IsEmpty())
                    SetSelectedIndex(
                        foundation::Min(m_selected + 1, static_cast<i32>(m_items.Size()) - 1));
                break;
            case KeyCode::Up:
                if (!m_items.IsEmpty())
                    SetSelectedIndex(foundation::Max(m_selected <= 0 ? 0 : m_selected - 1, 0));
                break;
            case KeyCode::Home:
                if (!m_items.IsEmpty())
                    SetSelectedIndex(0);
                break;
            case KeyCode::End:
                if (!m_items.IsEmpty())
                    SetSelectedIndex(static_cast<i32>(m_items.Size()) - 1);
                break;
            default:
                break;
            }
        }

    private:
        void Relayout()
        {
            const Rect box = GetContentBounds();
            m_scroll->SetPosition(foundation::Float2{box.x, box.y});
            m_scroll->SetSize(foundation::Float2{box.width, box.height});

            const f32 rowW = m_scroll->Viewport().width;
            for (usize i = 0; i < m_items.Size(); ++i)
            {
                m_items[i]->SetPosition(foundation::Float2{0.0f, static_cast<f32>(i) * m_itemHeight});
                m_items[i]->SetSize(foundation::Float2{rowW, m_itemHeight});
            }
            m_scroll->SetContentSize(
                foundation::Float2{rowW, static_cast<f32>(m_items.Size()) * m_itemHeight});
        }

        void ScrollIntoView(i32 index)
        {
            const f32 top = static_cast<f32>(index) * m_itemHeight;
            const f32 bottom = top + m_itemHeight;
            const f32 viewH = m_scroll->Viewport().height;
            const foundation::Float2 off = m_scroll->GetScrollOffset();
            if (top < off.y)
                m_scroll->SetScrollOffset(foundation::Float2{off.x, top});
            else if (bottom > off.y + viewH)
                m_scroll->SetScrollOffset(foundation::Float2{off.x, bottom - viewH});
        }

        RefPtr<ScrollView> m_scroll;
        Array<ListBoxItem*> m_items; // non-owning (owned by the scroll content)
        fonts::CachedFont* m_font = nullptr;
        f32 m_itemHeight = 24.0f;
        i32 m_selected = -1;
        Color m_panelColor{0.13f, 0.14f, 0.17f, 1.0f};
        Color m_textColor{0.88f, 0.90f, 0.94f, 1.0f};
        Color m_highlightColor{0.24f, 0.40f, 0.62f, 1.0f};
        foundation::Function<void(i32)> m_onChanged;
    };

    DRACONIC_DEFINE_OBJECT(ListBoxItem, "draconic::gui")
    DRACONIC_DEFINE_OBJECT(ListBox, "draconic::gui")
}
