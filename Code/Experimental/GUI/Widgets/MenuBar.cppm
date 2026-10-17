// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :menu_bar partition
//
// MenuBar: a horizontal strip of buttons, each opening a Menu below it - the classic
// application menu bar. Modeled on eepp's UIMenuBar (role only). Clicking a button opens its menu,
// a popup of the button (E5); a press on the button again, outside the menu, closes it without
// reopening, as the popup stack consumes it. While a menu is open, hovering a different button
// switches to it: opening that menu closes the other (it does not hold the new one's button), and
// the closed menu's notice clears the bar's state.
//
// The keyboard and the pad (E5): F10 (the `menu.enter-bar` command) focuses the bar's first
// button; Down, Return, Space or a pad's South on a button opens its menu with its first item lit;
// Left and Right move between the buttons, and between the menus while one is open (a submenu's
// item takes Right first, and a submenu Left); Escape closes the menu, focus going back to its
// button.

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
import :event_dispatcher;

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
            // Left and Right that nothing took, from a button or an open menu (which bubbles here).
            MenuBar* self = this;
            (void)AddEventListener(EventType::KeyDown, [self](const Event& e) { self->OnBarKey(static_cast<const KeyEvent&>(e)); });
        }

        /// F10: its first button takes focus.
        bool EnterFromKeyboard() override
        {
            if (m_entries.IsEmpty() || !IsTreeVisible() || !IsTreeEnabled())
                return false;
            m_entries[0].button->RequestFocus();
            return true;
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
            menu->SetOnClosed([self, rawMenu](PopupCloseReason) { self->OnMenuClosed(rawMenu); });

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
            const EventDispatcher* d = GetEventDispatcher();
            const bool fromKeys = d != nullptr && d->IsUsingKeyboard();
            menu->OpenBelow(*button, *button);
            if (fromKeys)
                menu->FocusFirstItem();
            // Opening closed any previous menu (its OnMenuClosed cleared m_current + deselected
            // its button); now record the new one.
            m_current = menu;
            m_currentButton = button;
            button->AddClass(core::StringView(u8"selected"));
        }
        void OnBarKey(const KeyEvent& event)
        {
            if (event.Modifiers != 0 || m_entries.IsEmpty())
                return;
            const KeyCode key = static_cast<KeyCode>(event.KeyCode);
            const i32 at = EntryHolding(event.Target);
            if (at < 0)
                return;
            const i32 count = static_cast<i32>(m_entries.Size());
            if (key == KeyCode::Left || key == KeyCode::Right)
            {
                const i32 next = (at + (key == KeyCode::Right ? 1 : -1) + count) % count;
                BarEntry& e = m_entries[static_cast<usize>(next)];
                event.Handled = true;
                if (m_current != nullptr)
                {
                    OpenMenu(e.button, e.menu.Get()); // the open one closes: it does not hold this button
                    e.menu->FocusFirstItem();
                }
                else
                    e.button->RequestFocus();
            }
            else if (key == KeyCode::Down && m_current == nullptr && event.Target == m_entries[static_cast<usize>(at)].button)
            {
                event.Handled = true;
                BarEntry& e = m_entries[static_cast<usize>(at)];
                OpenMenu(e.button, e.menu.Get());
                e.menu->FocusFirstItem();
            }
        }
        // The entry whose button is `node` or holds it (its open menu too), or -1.
        [[nodiscard]] i32 EntryHolding(const Node* node) const
        {
            for (usize i = 0; i < m_entries.Size(); ++i)
                for (const Node* n = node; n != nullptr && n != this; n = n->GetParent())
                    if (n == m_entries[i].button)
                        return static_cast<i32>(i);
            return -1;
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
