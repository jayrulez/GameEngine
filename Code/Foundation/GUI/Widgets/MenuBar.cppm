// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :menu_bar partition
//
// MenuBar: the classic application menu bar, a horizontal strip of buttons that each open a Menu
// below them. Clicking a button opens its menu as a popup of the button. Pressing the button
// again, which is outside the menu, closes the menu without reopening it, because the popup stack
// consumes that press. While a menu is open, hovering another button switches to its menu.
// Opening the new menu closes the old one (the old menu does not contain the new button), and the
// old menu's close notice clears the bar's open-menu state.
//
// Keyboard and gamepad: F10 (the `menu.enter-bar` command) focuses the first button. Down,
// Return, Space or the pad's South button on a button opens its menu with the first item lit.
// Left and Right move between the buttons, or between the menus while one is open; a submenu item
// handles Right first, and a submenu handles Left first. Escape closes the menu and focus returns
// to its button.
//
// In markup the bar's children are its menus, and each `<Menu text="File">` gets a button.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.gui:menu_bar;

import foundation.core;  // RefPtr, MakeRef, Array, Function, Move, Max, Float2
import foundation.fonts; // CachedFont
import :event;
import :node;
import :ui_widget;
import :button;
import :menu;
import :event_dispatcher;

using namespace foundation::core;
namespace core = foundation::core;
namespace fonts = foundation::fonts;

export namespace foundation::gui
{
    class MenuBar : public UIWidget
    {
        RTTI_OBJECT(MenuBar, UIWidget)
    public:
        MenuBar()
        {
            SetTag(core::StringView(u8"menubar"));
            SetBox(FilledBox(m_barColor)); // the default face; a style sheet's box replaces it
            // The bar's keys are bindings in the menu key context. Left and Right, from a button or
            // from an open menu that passes them on, move along the bar; Down opens a button's menu.
            SetKeyContext(core::StringView(kMenuKeyContext));
            MenuBar* self = this;
            (void)OnCommand(core::StringView(kMenuLeftCommand), [self](CommandEvent&) { self->Along(false); });
            (void)OnCommand(core::StringView(kMenuRightCommand), [self](CommandEvent&) { self->Along(true); });
            (void)OnCommand(core::StringView(kMenuDownCommand), [self](CommandEvent& e)
                            {
                                if (!self->OpenFromButton())
                                    e.Propagate = true;
                            });
        }

        /// Handles F10 by focusing the first button.
        bool EnterFromKeyboard() override
        {
            if (m_entries.IsEmpty() || !IsTreeVisible() || !IsTreeEnabled())
                return false;
            m_entries[0].button->RequestFocus();
            return true;
        }

        /// The bar's height, and each button's padding on either side of its text. A style sheet
        /// sets them as `bar-height` and `item-padding`, for example for an editor's compact bar
        /// or a game's large one.
        [[nodiscard]] f32 GetBarHeight() const noexcept { return m_barHeight; }
        [[nodiscard]] f32 GetItemPadding() const noexcept { return m_hpad; }
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

        // Adds a top-level menu under a button labelled `text`, and returns the empty Menu to fill.
        Menu* AddMenu(core::StringView text) { return AdoptMenu(core::MakeRef<Menu>(MemoryAllocator()), text); }
        /// Adds `menu`, made elsewhere (by markup, say), under a new button titled `text`.
        Menu* AdoptMenu(core::RefPtr<Menu> menu, core::StringView text)
        {
            auto button = core::MakeRef<Button>(MemoryAllocator());
            button->SetText(text);
            ShareTextStyle(*button, false);
            button->AddClass(
                core::StringView(u8"menubutton")); // flat bar styling; the class rule outranks the button tag's
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

        /// Returns the host for markup's children. They are the bar's menus, and each is adopted as
        /// it arrives.
        [[nodiscard]] Node* GetChildHost() override
        {
            if (!m_markupHost)
            {
                MenuBar* self = this;
                m_markupHost = core::MakeRef<MarkupChildHost>(MemoryAllocator(), [self](Node& host)
                {
                    while (host.ChildCount() != 0)
                    {
                        core::RefPtr<Node> child(host.GetChildAt(0));
                        host.RemoveChild(child.Get());
                        if (Menu* menu = core::Cast<Menu>(child.Get()))
                            (void)self->AdoptMenu(core::RefPtr<Menu>(menu), menu->GetTitle());
                    }
                });
            }
            return m_markupHost.Get();
        }

        /// Removes all menus, closing an open one first. An owner that builds the bar from a
        /// registry calls this before building it again.
        void ClearMenus()
        {
            if (m_current != nullptr)
                m_current->Close();
            for (BarEntry& e : m_entries)
                e.button->RemoveFromParent();
            m_entries.Clear();
            m_current = nullptr;
            m_currentButton = nullptr;
            Relayout();
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
        // The buttons sit where Relayout puts them, left to right, mirrored in right-to-left layouts.
        void OnLayout() override
        {
            LayoutChildren();
            if (!IsRightToLeft())
                return;
            for (BarEntry& e : m_entries)
                e.button->SetPosition(core::Float2{GetSize().x - e.button->GetPosition().x - e.button->GetSize().x, e.button->GetPosition().y});
        }

        // The buttons and their menus use the bar's font.
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
                menu->Close(); // OnMenuClosed clears the bar's state
            else
                OpenMenu(button, menu);
        }
        void HoverMenu(Button* button, Menu* menu)
        {
            // Switch only when a menu is already open; a hover alone must not open a menu.
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
            // Opening closed any previous menu (its OnMenuClosed cleared m_current and deselected
            // its button); now record the new one.
            m_current = menu;
            m_currentButton = button;
            button->AddClass(core::StringView(u8"selected"));
        }
        // Moves along the bar in its reading direction (in right to left, Left goes to the next
        // menu). With a menu open, the next menu opens; otherwise the next button takes focus.
        void Along(bool right)
        {
            const i32 at = FocusedEntry();
            if (at < 0 || m_entries.IsEmpty())
                return;
            const i32 count = static_cast<i32>(m_entries.Size());
            const bool onward = right != IsRightToLeft();
            const i32 next = (at + (onward ? 1 : -1) + count) % count;
            BarEntry& e = m_entries[static_cast<usize>(next)];
            if (m_current != nullptr)
            {
                OpenMenu(e.button, e.menu.Get()); // closes the open menu, which does not contain this button
                e.menu->FocusFirstItem();
            }
            else
                e.button->RequestFocus();
        }
        // Down on a focused button with no menu open opens its menu and focuses the first item.
        bool OpenFromButton()
        {
            const i32 at = FocusedEntry();
            if (at < 0 || m_current != nullptr || !m_entries[static_cast<usize>(at)].button->IsFocused())
                return false;
            BarEntry& e = m_entries[static_cast<usize>(at)];
            OpenMenu(e.button, e.menu.Get());
            e.menu->FocusFirstItem();
            return true;
        }
        // Returns the entry whose button has focus within it (including its open menu), or -1.
        [[nodiscard]] i32 FocusedEntry() const
        {
            for (usize i = 0; i < m_entries.Size(); ++i)
                if (m_entries[i].button->IsFocusWithin())
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
                const f32 w = (e.button->GetTextPart() != nullptr ? e.button->GetTextPart()->MeasureText().x : 0.0f) + m_hpad * 2.0f;
                e.button->SetPosition(core::Float2{x, 0.0f});
                e.button->SetSize(core::Float2{w, m_barHeight});
                x += w;
            }
            // Treat a size within a device pixel as unchanged. The layout snaps the box to the
            // device pixel grid, so setting it back to the fractional size would request another
            // layout every frame.
            const f32 pixel = 1.0f / core::Max(GetPixelScale(), 0.01f);
            if (core::Abs(GetSize().x - x) >= pixel || core::Abs(GetSize().y - m_barHeight) >= pixel)
                SetSize(core::Float2{x, m_barHeight});
        }

        // A button on the bar and the menu it opens.
        struct BarEntry
        {
            Button* button;
            core::RefPtr<Menu> menu;
        };
        Array<BarEntry> m_entries;
        core::RefPtr<Node> m_markupHost; // receives markup's menus before they are adopted
        Menu* m_current = nullptr;         // the open menu (non-owning)
        Button* m_currentButton = nullptr; // the open menu's button (non-owning)
        f32 m_barHeight = 28.0f;
        f32 m_hpad = 12.0f;
        Color m_barColor{0.13f, 0.15f, 0.18f, 1.0f};
    };

}
