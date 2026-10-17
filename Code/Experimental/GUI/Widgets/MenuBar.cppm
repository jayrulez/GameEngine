// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :menu_bar partition
//
// MenuBar: a horizontal strip of buttons, each opening a Menu below it - the classic
// application menu bar. Modeled on eepp's UIMenuBar (role only). Clicking a button toggles its
// menu; while any menu is open, hovering a different button switches to it (the standard menu-
// bar behavior). Switching is clean because the dispatcher tracks a single popup: opening the
// next menu closes the current one, whose close-notification clears the bar's state.
//
// Keyboard left/right navigation between menus is a follow-up (eepp's showNext/showPrevMenu);
// click + hover-switch is the core behavior.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module experimental.gui:menu_bar;

import foundation.core;  // RefPtr, MakeRef, Array, Function, Move, Max, Float2
import foundation.fonts; // CachedFont
import :event;
import :node;
import :rectangle_drawable;
import :ui_widget;
import :button;
import :menu;

using namespace foundation::core;
namespace core = foundation::core;
namespace fonts = foundation::fonts;

export namespace experimental::gui
{
    class MenuBar : public UIWidget
    {
        RTTI_OBJECT(MenuBar, UIWidget)
    public:
        MenuBar()
        {
            SetTag(core::StringView(u8"menubar"));
            SetBackground(core::MakeRef<RectangleDrawable>(MemoryAllocator(), m_barColor));
        }

        void SetBarHeight(f32 height)
        {
            m_barHeight = core::Max(1.0f, height);
            Relayout();
        }
        void SetItemPadding(f32 padding)
        {
            m_hpad = core::Max(0.0f, padding);
            Relayout();
        }

        // Add a top-level menu with its button label; returns the (empty) Menu to populate.
        Menu* AddMenu(core::StringView text)
        {
            auto button = core::MakeRef<Button>(MemoryAllocator());
            button->SetText(text);
            ShareTextStyle(*button, false);
            button->AddClass(
                core::StringView(u8"menubutton")); // flat bar styling (class beats the button tag)

            auto menu = core::MakeRef<Menu>(MemoryAllocator());
            ShareTextStyle(*menu, false);

            Button* rawButton = button.Get();
            Menu* rawMenu = menu.Get();
            MenuBar* self = this;
            button->SetOnClick([self, rawButton, rawMenu]()
                               { self->ToggleMenu(rawButton, rawMenu); });
            button->AddEventListener(EventType::MouseEnter, [self, rawButton, rawMenu](const Event&)
                                     { self->HoverMenu(rawButton, rawMenu); });
            menu->SetOnClosed([self, rawMenu]() { self->OnMenuClosed(rawMenu); });

            AddChild(button.Get());
            m_entries.PushBack(BarEntry{rawButton, core::Move(menu)});
            Relayout();
            return rawMenu;
        }

        [[nodiscard]] usize MenuCount() const noexcept { return m_entries.Size(); }
        [[nodiscard]] Menu* CurrentMenu() const noexcept { return m_current; }
        [[nodiscard]] Button* ButtonAt(usize index) const
        {
            return index < m_entries.Size() ? m_entries[index].button : nullptr;
        }
        [[nodiscard]] Menu* MenuAt(usize index) const
        {
            return index < m_entries.Size() ? m_entries[index].menu.Get() : nullptr;
        }

    protected:
        // Its buttons and their menus take its font.
        void OnTextStyleChange() override
        {
            for (BarEntry& e : m_entries)
            {
                ShareTextStyle(*e.button, false);
                if (e.menu)
                    ShareTextStyle(*e.menu, false);
            }
            Relayout();
        }

        void OnSizeChange() override { Relayout(); }

    private:
        void ToggleMenu(Button* button, Menu* menu)
        {
            if (m_current == menu)
                menu->Close(); // Close -> OnMenuClosed clears the bar state
            else
                OpenMenu(button, menu);
        }
        void HoverMenu(Button* button, Menu* menu)
        {
            // Only switch when a menu is already open (a bare hover must not drop a menu).
            if (m_current != nullptr && m_current != menu)
                OpenMenu(button, menu);
        }
        void OpenMenu(Button* button, Menu* menu)
        {
                        // The button is the popup owner: clicking it again toggles the menu closed instead
            // of the outside-click path dismissing it before the toggle runs.
            menu->Open(*this, button->ConvertToRootSpace(core::Float2{0.0f, button->GetSize().y}), button);
            // Opening closed any previous menu (its OnMenuClosed cleared m_current + deselected
            // its button); now record the new one.
            m_current = menu;
            m_currentButton = button;
            button->AddClass(core::StringView(u8"selected"));
        }
        void OnMenuClosed(Menu* menu)
        {
            if (m_current != menu)
                return;
            if (m_currentButton != nullptr)
                m_currentButton->RemoveClass(core::StringView(u8"selected"));
            m_current = nullptr;
            m_currentButton = nullptr;
        }

        void Relayout()
        {
            f32 x = 0.0f;
            for (BarEntry& e : m_entries)
            {
                const f32 w = e.button->MeasureText().x + m_hpad * 2.0f;
                e.button->SetPosition(core::Float2{x, 0.0f});
                e.button->SetSize(core::Float2{w, m_barHeight});
                x += w;
            }
            SetSize(core::Float2{x, m_barHeight});
        }

        // Top-left of `node` in root-local space (sum of positions up to, but excluding, the
        // root). Assumes identity scale/rotation on the path, matching the menu popup model.
        struct BarEntry
        {
            Button* button;
            core::RefPtr<Menu> menu;
        };
        Array<BarEntry> m_entries;
        Menu* m_current = nullptr;         // the open menu (non-owning)
        Button* m_currentButton = nullptr; // its button (non-owning)
        f32 m_barHeight = 28.0f;
        f32 m_hpad = 12.0f;
        Color m_barColor{0.13f, 0.15f, 0.18f, 1.0f};
    };

    RTTI_DEFINE_OBJECT(MenuBar, "rtti::gui")
}
