// Draconic GUI - :menu_bar partition
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
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.gui:menu_bar;

import draconic.foundation;  // RefPtr, MakeRef, Array, Function, Move, Max, Float2
import draconic.fonts; // CachedFont
import :event;
import :node;
import :rectangle_drawable;
import :ui_widget;
import :button;
import :menu;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;
namespace fonts = draconic::fonts;

export namespace draconic::gui
{
    class MenuBar : public UIWidget
    {
        DRACONIC_OBJECT(MenuBar, UIWidget)
    public:
        MenuBar()
        {
            SetTag(foundation::StringView(u8"menubar"));
            SetBackground(foundation::MakeRef<RectangleDrawable>(foundation::DefaultAllocator(), m_barColor));
        }

        void SetFont(fonts::CachedFont* font)
        {
            m_font = font;
            for (BarEntry& e : m_entries)
                e.button->SetFont(font);
            Relayout();
        }
        void SetBarHeight(f32 height)
        {
            m_barHeight = foundation::Max(1.0f, height);
            Relayout();
        }
        void SetItemPadding(f32 padding)
        {
            m_hpad = foundation::Max(0.0f, padding);
            Relayout();
        }

        // Add a top-level menu with its button label; returns the (empty) Menu to populate.
        Menu* AddMenu(foundation::StringView text)
        {
            auto button = foundation::MakeRef<Button>(foundation::DefaultAllocator());
            button->SetText(text);
            button->SetFont(m_font);
            button->AddClass(
                foundation::StringView(u8"menubutton")); // flat bar styling (class beats the button tag)

            auto menu = foundation::MakeRef<Menu>(foundation::DefaultAllocator());
            menu->SetFont(m_font);

            Button* rawButton = button.Get();
            Menu* rawMenu = menu.Get();
            MenuBar* self = this;
            button->SetOnClick([self, rawButton, rawMenu]()
                               { self->ToggleMenu(rawButton, rawMenu); });
            button->AddEventListener(EventType::MouseEnter, [self, rawButton, rawMenu](const Event&)
                                     { self->HoverMenu(rawButton, rawMenu); });
            menu->SetOnClosed([self, rawMenu]() { self->OnMenuClosed(rawMenu); });

            AddChild(button.Get());
            m_entries.PushBack(BarEntry{rawButton, foundation::Move(menu)});
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
            const foundation::Float2 origin = RootLocalOrigin(button);
            // The button is the popup owner: clicking it again toggles the menu closed instead
            // of the outside-click path dismissing it before the toggle runs.
            menu->Open(*this, foundation::Float2{origin.x, origin.y + button->GetSize().y}, button);
            // Opening closed any previous menu (its OnMenuClosed cleared m_current + deselected
            // its button); now record the new one.
            m_current = menu;
            m_currentButton = button;
            button->AddClass(foundation::StringView(u8"selected"));
        }
        void OnMenuClosed(Menu* menu)
        {
            if (m_current != menu)
                return;
            if (m_currentButton != nullptr)
                m_currentButton->RemoveClass(foundation::StringView(u8"selected"));
            m_current = nullptr;
            m_currentButton = nullptr;
        }

        void Relayout()
        {
            f32 x = 0.0f;
            for (BarEntry& e : m_entries)
            {
                const f32 w = e.button->MeasureText().x + m_hpad * 2.0f;
                e.button->SetPosition(foundation::Float2{x, 0.0f});
                e.button->SetSize(foundation::Float2{w, m_barHeight});
                x += w;
            }
            SetSize(foundation::Float2{x, m_barHeight});
        }

        // Top-left of `node` in root-local space (sum of positions up to, but excluding, the
        // root). Assumes identity scale/rotation on the path, matching the menu popup model.
        [[nodiscard]] static foundation::Float2 RootLocalOrigin(Node* node)
        {
            foundation::Float2 p{0.0f, 0.0f};
            for (Node* c = node; c != nullptr && c->GetParent() != nullptr; c = c->GetParent())
                p += c->GetPosition();
            return p;
        }

        struct BarEntry
        {
            Button* button;
            foundation::RefPtr<Menu> menu;
        };
        Array<BarEntry> m_entries;
        Menu* m_current = nullptr;         // the open menu (non-owning)
        Button* m_currentButton = nullptr; // its button (non-owning)
        fonts::CachedFont* m_font = nullptr;
        f32 m_barHeight = 28.0f;
        f32 m_hpad = 12.0f;
        Color m_barColor{0.13f, 0.15f, 0.18f, 1.0f};
    };

    DRACONIC_DEFINE_OBJECT(MenuBar, "draconic::gui")
}
