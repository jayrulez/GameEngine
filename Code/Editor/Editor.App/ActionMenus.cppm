// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App - :action_menus partition.
//
// ActionMenuBar: the menu bar generated from the action registry. Every action with a menuPath
// ("File/Save", "Scene/Simulate/Start") lands in the menu its first segment names, the rest as
// nested submenus. The application declares its menus' order (SetMenuOrder: names, and "*" where
// every other menu goes), so a domain's menu (Scene), or an extension's, registered later still
// lands before Help. Without a declaration, and for the menus "*" holds, menus keep the order their
// names first appear among the registrations; a menu holding only leading items comes after them.
// Items order by menuOrder with a separator between order bands (hundreds), each showing its
// effective chord. The bar is built again when the registry changes (a registration, a rebind). As
// a menu opens, its items' enabled and checked states are asked of the registry, and every item
// executes through the registry. A menu may carry leading items that are not actions (a
// list-driven set such as File > New <creator>) through AddLeadingItems; they come first, then a
// separator.
module;
#include "Core/Prelude.h"

export module editor.app:action_menus;

import foundation.core;
import foundation.gui;
import editor.core;

using namespace foundation::core;

export namespace editor::app
{
    namespace gui = foundation::gui;

    class ActionMenuBar
    {
    public:
        using LeadingItems = Function<void(gui::Menu&)>;

        ActionMenuBar(IAllocator& allocator, gui::MenuBar& bar, EditorActionRegistry& actions)
            : m_allocator(allocator), m_bar(&bar), m_actions(&actions), m_leading(allocator), m_order(allocator), m_items(allocator), m_submenus(allocator)
        {
            ActionMenuBar* self = this;
            m_changed = MakeUnique<gui::Effect>(m_allocator, [self]()
                                                {
                                                    self->m_actions->TrackChanges();
                                                    self->Rebuild();
                                                });
        }
        ActionMenuBar(const ActionMenuBar&) = delete;
        ActionMenuBar& operator=(const ActionMenuBar&) = delete;
        /// The bar's items execute through this object, so destroying it empties the bar and
        /// stops the registry calling back.
        ~ActionMenuBar()
        {
            m_changed.Reset();
            m_bar->ClearMenus();
        }

        /// Adds items that are not actions (a list-driven set) at the top of `menu`, creating the
        /// menu when absent. They are made again whenever the bar is rebuilt.
        void AddLeadingItems(StringView menu, LeadingItems items)
        {
            Leading entry;
            entry.menu = String(menu);
            entry.items = Move(items);
            m_leading.PushBack(Move(entry));
            Rebuild();
        }

        /// The top-level menus' order: these names in this order, "*" where every menu not named
        /// goes (in the order it first appears; at the end when there is no "*"). A name with no
        /// actions and no leading items makes no menu.
        void SetMenuOrder(Span<const StringView> order)
        {
            m_order.Clear();
            for (const StringView name : order)
                m_order.PushBack(String(name));
            Rebuild();
        }

        /// Builds the whole bar, its menus and their items, from the registry.
        void Rebuild()
        {
            m_bar->ClearMenus();
            m_items.Clear();
            m_submenus.Clear();
            // Collect the menus in the order their names first appear among the actions; a menu
            // that holds only leading items comes after them.
            Array<String> present;
            const auto note = [&present](StringView name)
            {
                for (const String& seen : present)
                    if (seen.AsView() == name)
                        return;
                present.PushBack(String(name));
            };
            for (const EditorActionDeclaration& action : m_actions->Actions())
                if (!action.menuPath.IsEmpty())
                    note(TopLevel(action.menuPath.AsView()));
            for (const Leading& leading : m_leading)
                note(leading.menu.AsView());
            // Then arrange them in the declared order.
            const auto declared = [this](StringView name)
            {
                for (const String& entry : m_order)
                    if (entry.AsView() == name)
                        return true;
                return false;
            };
            Array<String> titles;
            const auto placeRest = [&]()
            {
                for (const String& name : present)
                    if (!declared(name.AsView()))
                        titles.PushBack(name);
            };
            bool restPlaced = false;
            for (const String& entry : m_order)
            {
                if (entry.AsView() == StringView(u8"*"))
                {
                    placeRest();
                    restPlaced = true;
                    continue;
                }
                for (const String& name : present)
                    if (name.AsView() == entry.AsView())
                    {
                        titles.PushBack(name);
                        break;
                    }
            }
            if (!restPlaced)
                placeRest();
            for (const String& title : titles)
            {
                gui::Menu* menu = m_bar->AddMenu(title.AsView());
                Fill(title.AsView(), *menu);
            }
        }

        /// Updates each action item's enabled state (with the reason when disabled) and checked
        /// state from the registry's current answer.
        void Refresh(gui::Menu& menu) const
        {
            for (const Item& item : m_items)
                if (item.menu == &menu)
                {
                    item.item->SetEnabled(m_actions->IsEnabled(item.action.AsView()));
                    item.item->SetTooltip(m_actions->DisabledReason(item.action.AsView()).AsView()); // the reason it is disabled, when the action gives one
                    if (item.item->IsCheckable())
                        item.item->SetCheckedSilently(m_actions->IsChecked(item.action.AsView()));
                }
        }

    private:
        struct Leading
        {
            String menu;
            LeadingItems items;
        };
        // An action's item, and the menu (or submenu) it is in.
        struct Item
        {
            gui::Menu* menu = nullptr;
            gui::MenuItem* item = nullptr;
            String action;
        };

        // The items of one top-level menu.
        void Fill(StringView title, gui::Menu& menu)
        {
            bool any = false;
            for (const Leading& leading : m_leading)
                if (leading.menu.AsView() == title)
                {
                    leading.items(menu);
                    any = menu.ItemCount() > 0;
                }
            // The actions of this menu, in menuOrder, nested by the rest of their path.
            Array<const EditorActionDeclaration*> ordered;
            for (const EditorActionDeclaration& action : m_actions->Actions())
                if (!action.menuPath.IsEmpty() && TopLevel(action.menuPath.AsView()) == title)
                    ordered.PushBack(&action);
            ordered.Sort([](const EditorActionDeclaration* a, const EditorActionDeclaration* b) { return a->menuOrder < b->menuOrder; });
            if (any && !ordered.IsEmpty())
                menu.AddSeparator();
            i32 band = 0;
            bool first = true;
            for (const EditorActionDeclaration* action : ordered)
            {
                const i32 thisBand = action->menuOrder / 100;
                if (!first && thisBand != band)
                    menu.AddSeparator();
                band = thisBand;
                first = false;
                AddItem(menu, Rest(action->menuPath.AsView()), *action);
            }
            Watch(menu);
        }

        // Refreshes the menu's item states from the registry each time it opens.
        void Watch(gui::Menu& menu)
        {
            const ActionMenuBar* self = this;
            menu.SetOnOpening([self](gui::Menu& opening) { self->Refresh(opening); });
        }

        [[nodiscard]] static StringView TopLevel(StringView path) noexcept
        {
            for (usize i = 0; i < path.Size(); ++i)
                if (path[i] == u8'/')
                    return path.SubStr(0, i);
            return path;
        }
        [[nodiscard]] static StringView Rest(StringView path) noexcept
        {
            for (usize i = 0; i < path.Size(); ++i)
                if (path[i] == u8'/')
                    return path.SubStr(i + 1, path.Size() - i - 1);
            return StringView();
        }

        /// `path` is the part after the top-level menu: "Save", or "Simulate/Start" (the item
        /// "Start" in the submenu "Simulate", which is created when it does not exist yet).
        void AddItem(gui::Menu& menu, StringView path, const EditorActionDeclaration& action)
        {
            const StringView head = TopLevel(path);
            const StringView tail = Rest(path);
            if (tail.IsEmpty())
            {
                const EditorActionRegistry* actions = m_actions;
                const String id = action.id;
                gui::MenuItem* item = menu.AddItem(head, [actions, id]() { (void)actions->Execute(id.AsView()); });
                item->SetShortcut(FormatShortcut(m_actions->Shortcut(id.AsView())).AsView());
                if (action.checked)
                {
                    item->SetCheckable(true);
                    item->SetCheckedSilently(m_actions->IsChecked(id.AsView()));
                }
                item->SetEnabled(m_actions->IsEnabled(id.AsView()));
                m_items.PushBack(Item{&menu, item, id});
                return;
            }
            gui::Menu* submenu = nullptr;
            for (const Submenu& known : m_submenus)
                if (known.parent == &menu && known.title.AsView() == head)
                    submenu = known.menu;
            if (submenu == nullptr)
            {
                submenu = menu.AddSubMenu(head);
                m_submenus.PushBack(Submenu{&menu, String(head), submenu});
                Watch(*submenu);
            }
            AddItem(*submenu, tail, action);
        }

        struct Submenu
        {
            gui::Menu* parent = nullptr;
            String title;
            gui::Menu* menu = nullptr;
        };

        IAllocator& m_allocator;
        gui::MenuBar* m_bar;
        EditorActionRegistry* m_actions;
        Array<Leading> m_leading;
        Array<String> m_order; // SetMenuOrder: names, "*" for every other menu
        Array<Item> m_items;   // the action items of the bar as built
        Array<Submenu> m_submenus;
        UniquePtr<gui::Effect> m_changed; // rebuilds the bar when the registry changes
    };
}
