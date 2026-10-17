// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :combo_box partition
//
// ComboBox: a control that shows the current selection and, when clicked, drops down a list of
// choices. Modeled on eepp's UIDropDownList (role only), built by composition: the dropdown is
// a ListBox added as a top-level popup (over everything) via the EventDispatcher's popup
// support, so a click outside or Escape dismisses it. Picking an item sets the selection and
// closes the dropdown.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module experimental.gui:combo_box;

import foundation.core;  // RefPtr, MakeRef, Array, String, Function, Move, Min, Float2
import foundation.fonts; // CachedFont
import foundation.vg;    // PathBuilder
import :rect;
import :event;
import :draw_context;
import :text;
import :node;
import :rectangle_drawable;
import :ui_widget;
import :list_box;
import :event_dispatcher;

using namespace foundation::core;
namespace core = foundation::core;
namespace fonts = foundation::fonts;
namespace vg = foundation::vg;

export namespace experimental::gui
{
    class ComboBox : public UIWidget
    {
        RTTI_OBJECT(ComboBox, UIWidget)
    public:
        ComboBox()
        {
            SetTag(core::StringView(u8"combobox"));
            SetTabFocusable(true);
            // A visible box so it reads as a control even before it is opened.
            SetBackground(core::MakeRef<RectangleDrawable>(MemoryAllocator(), m_boxColor));
            m_text.SetAlignment(TextHAlign::Left, TextVAlign::Middle);
        }


        void AddItem(core::StringView text)
        {
            m_items.PushBack(core::String(text));
            if (m_dropdown)
                m_dropdown->AddItem(text);
            RequestLayout(); // as wide as its widest item
        }
        [[nodiscard]] usize ItemCount() const noexcept { return m_items.Size(); }
        /// The items as one text, '|' between them (markup's and a sheet's `items`).
        void SetItemsText(const core::String& text)
        {
            m_items.Clear();
            m_selected = -1;
            if (m_dropdown)
                m_dropdown->Clear();
            for (const core::StringView item : SplitItems(text.AsView()))
                AddItem(item);
        }
        [[nodiscard]] core::String GetItemsText() const { return JoinItems(m_items); }

        [[nodiscard]] i32 GetSelectedIndex() const noexcept { return m_selected; }
        [[nodiscard]] core::StringView GetSelectedText() const
        {
            return (m_selected >= 0 && static_cast<usize>(m_selected) < m_items.Size())
                       ? m_items[static_cast<usize>(m_selected)].AsView()
                       : core::StringView{};
        }
        void SetSelectedIndex(i32 index)
        {
            if (index < 0 || index >= static_cast<i32>(m_items.Size()))
                return;
            if (index == m_selected)
                return;
            m_selected = index;
            m_text.SetString(m_items[static_cast<usize>(index)].AsView());
            Invalidate();
            if (m_onChanged)
                m_onChanged(index);
        }
        void SetOnSelectionChanged(core::Function<void(i32)> callback)
        {
            m_onChanged = core::Move(callback);
        }

        [[nodiscard]] bool IsOpen() const noexcept { return m_open; }
        void SetItemHeight(f32 height) { m_itemHeight = height; }

        /// Its widest item and the arrow beside it, one line of its font tall (E2).
        [[nodiscard]] core::Float2 MeasureContent(core::Float2) override
        {
            f32 widest = 0.0f;
            if (GetFont() != nullptr)
                for (const core::String& item : m_items)
                    widest = core::Max(widest, GetFont()->font->MeasureString(item.AsView()));
            return core::Float2{widest + 24.0f, GetFont() != nullptr ? GetFont()->font->Metrics().lineHeight : 20.0f};
        }
        /// The selection's baseline, where it draws it.
        [[nodiscard]] f32 BaselineAt(core::Float2 size) const override
        {
            const Thickness c = GetChrome();
            return m_text.FirstBaseline(Rect{c.Left, c.Top, core::Max(0.0f, size.x - c.TotalHorizontal() - 16.0f),
                                             core::Max(0.0f, size.y - c.TotalVertical())});
        }

    protected:
        // Its selection draws in its text style; the arrow is the text's colour; the dropdown
        // takes its font.
        void OnTextStyleChange() override
        {
            m_text.SetFont(GetFont());
            m_text.SetColor(GetTextColor());
            m_arrowColor = GetTextColor();
            if (m_dropdown)
                ShareTextStyle(*m_dropdown, false);
        }

        void OnMouseClick(const MouseEvent& event) override
        {
            event.Handled = true;
            if (m_open)
                CloseDropdown();
            else
                OpenDropdown();
        }

        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            (void)localBounds;
            const Rect b = GetContentBounds();
            // Selection text (leaving room for the arrow on the right).
            m_text.Draw(ctx, Rect{b.x, b.y, core::Max(0.0f, b.width - 16.0f), b.height});
            // A small down-arrow at the right.
            const f32 aw = 8.0f, ah = 5.0f;
            const f32 ax = b.x + b.width - aw - 4.0f;
            const f32 ay = b.y + (b.height - ah) * 0.5f;
            vg::PathBuilder pb;
            pb.MoveTo(ax, ay);
            pb.LineTo(ax + aw, ay);
            pb.LineTo(ax + aw * 0.5f, ay + ah);
            pb.Close();
            ctx.VG().FillPath(pb.ToPath(), m_arrowColor);
        }

    private:
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
            m_dropdown->SetOnSelectionChanged([self](i32 i) { self->OnPicked(i); });
        }

        void OpenDropdown()
        {
            Node* root = GetRootNode();
            EventDispatcher* dispatcher = GetEventDispatcher();
            if (root == nullptr || dispatcher == nullptr || m_items.IsEmpty())
                return;
            BuildDropdown();

            const core::Float2 below = ConvertToRootSpace(core::Float2{0.0f, GetSize().y});
            const f32 h =
                core::Min(static_cast<f32>(m_items.Size()) * m_itemHeight, m_dropdownMaxHeight);
            m_dropdown->SetPosition(below);
            m_dropdown->SetSize(core::Float2{GetSize().x, h});
            root->AddChild(m_dropdown.Get());

            m_open = true;
            ComboBox* self = this;
            dispatcher->OpenPopup(m_dropdown.Get(), this, [self]() { self->CloseDropdown(); });
        }

        void CloseDropdown()
        {
            if (!m_open)
                return;
            m_open = false;
            if (m_dropdown)
                m_dropdown->RemoveFromParent();
        }

        void OnPicked(i32 index)
        {
            SetSelectedIndex(index);
            if (EventDispatcher* dispatcher = GetEventDispatcher())
                dispatcher->ClosePopup();
            else
                CloseDropdown();
        }

        Array<core::String> m_items;
        Text m_text;
        RefPtr<ListBox> m_dropdown;
        i32 m_selected = -1;
        bool m_open = false;
        f32 m_itemHeight = 24.0f;
        f32 m_dropdownMaxHeight = 160.0f;
        Color m_boxColor{0.18f, 0.20f, 0.25f, 1.0f};
        Color m_arrowColor{0.75f, 0.80f, 0.86f, 1.0f};
        core::Function<void(i32)> m_onChanged;
    };

}
