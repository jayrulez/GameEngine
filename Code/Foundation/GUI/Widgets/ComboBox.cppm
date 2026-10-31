// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :combo_box partition
//
// ComboBox: a control that shows the current selection and, when clicked, drops down a list of
// choices. The dropdown is a ListBox opened as an auto popup. While open it stays the combo box's
// child, sits in the top layer and is placed below the combo box (above it when there is more
// room there), shrunk to fit the room. A press outside or Escape closes it. Picking an item sets
// the selection and closes the dropdown.
//
// Keyboard and gamepad: Return, Space, the pad's South button or Alt+Down opens the dropdown with
// the list focused on the current item; there the arrows move and Return picks. While it is
// closed, Up and Down move the selection by one item. Past the first or the last item the key is
// left unhandled, so focus can move on. A letter or a digit selects the next item that starts
// with it.
//
// When nothing is selected (index -1) the combo box shows its placeholder in the placeholder
// colour. That happens before anything is picked, or when the values it stands for differ, as in a
// property grid's mixed state.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.gui:combo_box;

import foundation.core;  // RefPtr, MakeRef, Array, String, Function, Move, Min, Float2
import foundation.fonts; // CachedFont
import foundation.vg;    // PathBuilder
import :rect;
import :event;
import :draw_context;
import :text;
import :node;
import :ui_widget;
import :list_box;
import :event_dispatcher; // the popup stack
import :popup_placement;
import :reactive; // Observable: a binding follows the selection

using namespace foundation::core;
namespace core = foundation::core;
namespace fonts = foundation::fonts;
namespace vg = foundation::vg;

export namespace foundation::gui
{
    class ComboBox : public UIWidget
    {
        RTTI_OBJECT(ComboBox, UIWidget)
    public:
        ComboBox()
        {
            SetTag(core::StringView(u8"combobox"));
            SetTabFocusable(true);
            m_text.SetAlignment(TextHAlign::Start, TextVAlign::Middle);
        }


        void AddItem(core::StringView text)
        {
            m_items.PushBack(core::String(text));
            if (m_dropdown)
                m_dropdown->AddItem(text);
            RequestLayout(); // the combo box is as wide as its widest item
        }
        [[nodiscard]] usize ItemCount() const noexcept { return m_items.Size(); }
        /// Replaces the items with those in `text`, separated by '|'. Markup and style sheets set
        /// `items` through this.
        void SetItemsText(const core::String& text)
        {
            m_items.Clear();
            if (m_dropdown)
                m_dropdown->Clear();
            for (const core::StringView item : SplitItems(text.AsView()))
                AddItem(item);
            // The selected item was removed with the others, so nothing is selected now.
            if (m_selected != -1)
            {
                m_selected = -1;
                ShowSelection();
                m_selectedChanged.Notify();
                Invalidate();
            }
        }
        [[nodiscard]] core::String GetItemsText() const { return JoinItems(m_items); }

        /// Returns the selected item's index, or -1 when nothing is selected. This is the
        /// `selected` property of markup and style sheets, and a binding that reads it follows
        /// later changes.
        [[nodiscard]] i32 GetSelectedIndex() const
        {
            m_selectedChanged.Track();
            return m_selected;
        }
        [[nodiscard]] core::StringView GetSelectedText() const
        {
            return (m_selected >= 0 && static_cast<usize>(m_selected) < m_items.Size())
                       ? m_items[static_cast<usize>(m_selected)].AsView()
                       : core::StringView{};
        }
        /// Selects the item at `index`, or nothing when `index` is -1.
        void SetSelectedIndex(i32 index)
        {
            if (index < -1 || index >= static_cast<i32>(m_items.Size()))
                return;
            if (index == m_selected)
                return;
            m_selected = index;
            ShowSelection();
            Invalidate();
            m_selectedChanged.Notify();
            if (m_onChanged)
                m_onChanged(index);
        }
        /// Selects `index` on the user's behalf (from the keys, type-ahead or the dropdown). It
        /// does what SetSelectedIndex does and, when the selection changed, also sends a `change`
        /// event, as a slider or a text field does.
        void SelectByUser(i32 index)
        {
            const i32 was = m_selected;
            SetSelectedIndex(index);
            if (m_selected != was)
                SendEvent(Event(EventType::Changed, this));
        }
        void SetOnSelectionChanged(core::Function<void(i32)> callback)
        {
            m_onChanged = core::Move(callback);
        }

        [[nodiscard]] bool IsOpen() const
        {
            const EventDispatcher* dispatcher = m_dropdown ? m_dropdown->GetEventDispatcher() : nullptr;
            return dispatcher != nullptr && dispatcher->IsPopupOpen(*m_dropdown);
        }
        /// Returns the dropdown list, or null until the dropdown first opens.
        [[nodiscard]] ListBox* GetDropdown() const noexcept { return m_dropdown.Get(); }
        void SetItemHeight(f32 height) { m_itemHeight = height; }

        /// Sets the text shown while nothing is selected (the `placeholder` property).
        void SetPlaceholder(const core::String& text)
        {
            m_placeholder = text;
            ShowSelection();
        }
        [[nodiscard]] core::String GetPlaceholder() const { return m_placeholder; }
        void SetPlaceholderColor(Color color)
        {
            m_placeholderColor = color;
            ShowSelection();
        }

        /// Measures the widest item plus the arrow beside it, one line of the font tall.
        [[nodiscard]] core::Float2 MeasureContent(core::Float2) override
        {
            f32 widest = 0.0f;
            if (GetFont() != nullptr)
                for (const core::String& item : m_items)
                    widest = core::Max(widest, GetFont()->font->MeasureString(item.AsView()));
            return core::Float2{widest + 24.0f, GetFont() != nullptr ? GetFont()->font->Metrics().lineHeight : 20.0f};
        }
        /// Returns the baseline of the selection text, where it is drawn.
        [[nodiscard]] f32 BaselineAt(core::Float2 size) const override
        {
            const Thickness c = GetChrome();
            return m_text.FirstBaseline(Rect{c.Left, c.Top, core::Max(0.0f, size.x - c.TotalHorizontal() - 16.0f),
                                             core::Max(0.0f, size.y - c.TotalVertical())});
        }

    protected:
        // The selection text uses this text style, the arrow takes the text colour, and the
        // dropdown shares the font.
        void OnTextStyleChange() override
        {
            m_text.SetFont(GetFont());
            m_text.SetFontSelection(GetFontService(), GetTextStyle().FontFamily.AsView(), GetTextStyle().Selection);
            m_text.SetDirection(TextDirectionFor());
            ShowSelection();
            m_arrowColor = GetTextColor();
            if (m_dropdown)
                ShareTextStyle(*m_dropdown, false);
        }

        void OnMouseClick(const MouseEvent& event) override
        {
            event.Handled = true;
            if (IsOpen())
                CloseDropdown();
            else
                OpenDropdown();
        }
        bool OnActivate() override
        {
            OpenDropdown();
            return IsOpen();
        }
        void OnKeyDown(const KeyEvent& event) override
        {
            if (event.Target != this || m_items.IsEmpty())
                return;
            const KeyCode key = static_cast<KeyCode>(event.KeyCode);
            if (key == KeyCode::Down && event.Modifiers == static_cast<u32>(KeyModAlt))
            {
                event.Handled = true;
                OpenDropdown();
                return;
            }
            if (event.Modifiers != 0)
                return;
            const i32 last = static_cast<i32>(m_items.Size()) - 1;
            if (key == KeyCode::Down && m_selected < last)
            {
                SelectByUser(m_selected + 1);
                event.Handled = true;
            }
            else if (key == KeyCode::Up && m_selected > 0)
            {
                SelectByUser(m_selected - 1);
                event.Handled = true;
            }
            else if (KeyTypesText(key))
                event.Handled = true; // the key's text arrives next, for type-ahead
        }
        // Typed text arrives through the keyboard layout and any IME. It selects the next item
        // after the current one that starts with the text's first character, wrapping past the
        // end of the list.
        [[nodiscard]] bool WantsTextInput() const override { return IsTreeEnabled() && !m_items.IsEmpty(); }
        void OnTextInput(const TextInputEvent& event) override
        {
            if (event.Target != this || m_items.IsEmpty() || event.Text.Size() == 0)
                return;
            usize end = 0;
            (void)core::DecodeUtf8(event.Text, end);
            const core::StringView typed = event.Text.SubStr(0, end);
            event.Handled = true;
            const i32 count = static_cast<i32>(m_items.Size());
            for (i32 n = 1; n <= count; ++n)
            {
                const i32 i = (m_selected + n + count) % count;
                if (StartsWithIgnoringCase(m_items[static_cast<usize>(i)].AsView(), typed))
                {
                    SelectByUser(i);
                    break;
                }
            }
        }

        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            // Draw a visible box so it reads as a control even unstyled; a style sheet's fill
            // replaces it.
            if (!BoxFills())
                ctx.VG().FillRect(Rectangle{localBounds.x, localBounds.y, localBounds.width, localBounds.height}, m_boxColor);
            const Rect b = GetContentBounds();
            const bool rtl = IsRightToLeft(); // the arrow sits at the inline end
            // Selection text (leaving room for the arrow).
            m_text.Draw(ctx, MirroredIn(Rect{b.x, b.y, core::Max(0.0f, b.width - 16.0f), b.height}, b, rtl));
            // A small down-arrow.
            const f32 aw = 8.0f, ah = 5.0f;
            const f32 ax = MirroredIn(Rect{b.x + b.width - aw - 4.0f, 0.0f, aw, 0.0f}, b, rtl).x;
            const f32 ay = b.y + (b.height - ah) * 0.5f;
            vg::PathBuilder pb;
            pb.MoveTo(ax, ay);
            pb.LineTo(ax + aw, ay);
            pb.LineTo(ax + aw * 0.5f, ay + ah);
            pb.Close();
            ctx.VG().FillPath(pb.ToPath(), m_arrowColor);
        }

    private:
        // Shows the selected item in the text colour, or the placeholder in the placeholder colour.
        void ShowSelection()
        {
            const bool none = m_selected < 0 || static_cast<usize>(m_selected) >= m_items.Size();
            m_text.SetString(none ? m_placeholder.AsView() : m_items[static_cast<usize>(m_selected)].AsView());
            m_text.SetColor(none ? m_placeholderColor : GetTextColor());
            Invalidate();
        }

        void BuildDropdown()
        {
            if (m_dropdown)
                return;
            m_dropdown = core::MakeRef<ListBox>(MemoryAllocator());
            ShareTextStyle(*m_dropdown, false);
            m_dropdown->SetItemHeight(m_itemHeight);
            for (const core::String& item : m_items)
                m_dropdown->AddItem(item.AsView());
            ComboBox* self = this;
            m_dropdown->SetOnItemActivated([self](i32 i) { self->OnPicked(i); });
        }

        void OpenDropdown()
        {
            EventDispatcher* dispatcher = GetEventDispatcher();
            if (dispatcher == nullptr || m_items.IsEmpty())
                return;
            BuildDropdown();
            // The dropdown is as wide as the combo box. Its height is left to the layout, which
            // measures its rows plus its own padding and border (the style sheet's popup surface),
            // capped at m_dropdownMaxHeight.
            const f32 width = GetSize().x, most = m_dropdownMaxHeight;
            m_dropdown->EditLayoutStyle([width, most](LayoutStyle& s)
            {
                s.Width = LayoutLength::Px(width);
                s.Height = LayoutLength{};
                s.MaxHeight = LayoutLength::Px(most);
            });
            if (m_selected >= 0)
                m_dropdown->SetSelectedIndex(m_selected); // highlights the current item and scrolls to it
            PopupOptions options;
            options.Anchor = this;
            options.Placement = PopupPlacement{PopupSide::Below, PopupAlign::Start, 0.0f, true};
            options.TakesFocus = true; // the list takes focus so the arrows and Return work; focus returns on close
            options.FitContent = true; // re-measured each layout pass from its rows, padding and border
            dispatcher->OpenPopup(*this, *m_dropdown, core::Move(options));
        }

        void CloseDropdown()
        {
            if (EventDispatcher* dispatcher = GetEventDispatcher(); dispatcher != nullptr && m_dropdown)
                (void)dispatcher->ClosePopup(*m_dropdown);
        }

        void OnPicked(i32 index)
        {
            SelectByUser(index);
            CloseDropdown();
        }



        Array<core::String> m_items;
        Text m_text;
        RefPtr<ListBox> m_dropdown; // a child of the combo box while open; held here while closed
        i32 m_selected = -1;
        f32 m_itemHeight = 24.0f;
        f32 m_dropdownMaxHeight = 160.0f;
        Color m_boxColor{0.18f, 0.20f, 0.25f, 1.0f};
        Color m_arrowColor{0.75f, 0.80f, 0.86f, 1.0f};
        Color m_placeholderColor{0.55f, 0.58f, 0.62f, 1.0f}; // the same as a text field's placeholder
        core::String m_placeholder;
        core::Function<void(i32)> m_onChanged;
        mutable Observable m_selectedChanged;
    };

}
