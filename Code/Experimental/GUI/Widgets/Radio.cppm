// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :radio partition
//
// RadioButton + RadioGroup: a mutually-exclusive selection. Modeled on eepp's UIRadioButton
// (role only). A RadioButton is like a CheckBox but circular and one-way: clicking selects it,
// and a RadioGroup ensures exactly one member is selected at a time (selecting one clears the
// rest). Hover/press reaction comes for free from UINode's control state. Pair each with a
// Label in a horizontal LinearLayout for the usual "(o) caption" look.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module experimental.gui:radio;

import foundation.core; // Color, Function, Move, Array
import :rect;
import :event;
import :draw_context;
import :ui_widget;

using namespace foundation::core;
namespace core = foundation::core;

export namespace experimental::gui
{
    class RadioGroup; // controller defined below; a button routes selection through it

    class RadioButton : public UIWidget
    {
        RTTI_OBJECT(RadioButton, UIWidget)
        friend class RadioGroup;

    public:
        RadioButton()
        {
            SetTag(core::StringView(u8"radio"));
            SetTabFocusable(true);
        }

        [[nodiscard]] bool IsSelected() const noexcept { return m_selected; }
        [[nodiscard]] u32 GetWidgetStates() const override { return m_selected ? WidgetStateChecked : WidgetStateNone; } // :checked

        // Select this button (routes through the group so the others clear). Selecting an
        // already-selected button is a no-op.
        void Select();
        void SetSelected(bool selected)
        {
            if (selected)
                Select();
            else
                SetSelectedState(false);
        }

        // Fired when this button becomes selected.
        void SetOnSelected(core::Function<void()> callback) { m_onSelected = core::Move(callback); }

        void SetRingColor(Color color)
        {
            m_ringColor = color;
            Invalidate();
        }
        void SetDotColor(Color color)
        {
            m_dotColor = color;
            Invalidate();
        }

        // Theming parts: radio::ring (outline) / ::dot (inner fill).
        void CollectStyleParts(core::Array<core::StringView>& out) const override
        {
            out.PushBack(core::StringView(u8"ring"));
            out.PushBack(core::StringView(u8"dot"));
        }

        /// Its dot's size when nothing sizes it (E2).
        [[nodiscard]] core::Float2 MeasureContent(core::Float2) override { return core::Float2{20.0f, 20.0f}; }

    protected:
        bool OnActivate() override { return ActivateAsClick(); } // Return, Space: a click
        void OnMouseClick(const MouseEvent& event) override
        {
            event.Handled = true;
            Select();
        }

        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            (void)localBounds;
            const Rect box = GetContentBounds();
            const f32 r = core::Min(box.width, box.height) * 0.5f;
            const core::Float2 c{box.x + box.width * 0.5f, box.y + box.height * 0.5f};
            // radio::ring its outline, ::dot the choice, each its own look with the sheet's over it.
            GetPartStyle(u8"ring").Draw(ctx, Rect{c.x - r, c.y - r, r * 2.0f, r * 2.0f}, OutlinedBox(m_ringColor, 2.0f, vg::CornerRadii(r)));
            if (m_selected)
            {
                const f32 d = r * 0.5f;
                GetPartStyle(u8"dot").Draw(ctx, Rect{c.x - d, c.y - d, d * 2.0f, d * 2.0f}, FilledBox(m_dotColor, vg::CornerRadii(d)));
            }
        }

    private:
        // Set the visual/selected flag directly (the group drives this); fires OnSelected on a
        // false->true transition.
        void SetSelectedState(bool selected)
        {
            if (selected == m_selected)
                return;
            m_selected = selected;
            MarkStyleDirty(true); // :checked
            Invalidate();
            if (m_selected && m_onSelected)
                m_onSelected();
        }

        RadioGroup* m_group = nullptr; // non-owning; set by RadioGroup::Add
        bool m_selected = false;
        Color m_ringColor{0.60f, 0.65f, 0.72f, 1.0f};
        Color m_dotColor{0.31f, 0.63f, 0.85f, 1.0f};
        core::Function<void(void)> m_onSelected;
    };

    // Controller (not a Node) that keeps exactly one of its member buttons selected.
    class RadioGroup
    {
    public:
        // Register a button (non-owning). It becomes part of the mutual-exclusion set.
        void Add(RadioButton* button)
        {
            if (button == nullptr)
                return;
            button->m_group = this;
            m_buttons.PushBack(button);
        }

        // Select `button` (must be a member): clears the others, selects it, and fires the
        // group callback with its index.
        void Select(RadioButton* button)
        {
            i32 index = -1;
            for (usize i = 0; i < m_buttons.Size(); ++i)
            {
                const bool isTarget = (m_buttons[i] == button);
                m_buttons[i]->SetSelectedState(isTarget);
                if (isTarget)
                    index = static_cast<i32>(i);
            }
            if (index != m_selectedIndex)
            {
                m_selectedIndex = index;
                if (m_onChanged)
                    m_onChanged(index);
            }
        }

        [[nodiscard]] i32 GetSelectedIndex() const noexcept { return m_selectedIndex; }
        [[nodiscard]] RadioButton* GetSelected() const
        {
            return (m_selectedIndex >= 0 && static_cast<usize>(m_selectedIndex) < m_buttons.Size())
                       ? m_buttons[static_cast<usize>(m_selectedIndex)]
                       : nullptr;
        }
        [[nodiscard]] usize Count() const noexcept { return m_buttons.Size(); }

        void SetOnSelectionChanged(core::Function<void(i32)> callback)
        {
            m_onChanged = core::Move(callback);
        }

    private:
        core::Array<RadioButton*> m_buttons; // non-owning (the tree owns the buttons)
        i32 m_selectedIndex = -1;
        core::Function<void(i32)> m_onChanged;
    };

    inline void RadioButton::Select()
    {
        if (m_group != nullptr)
            m_group->Select(this);
        else
            SetSelectedState(true);
    }

}
