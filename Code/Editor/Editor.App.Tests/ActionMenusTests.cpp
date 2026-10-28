// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App tests - the menu bar generated from the action registry: menus in first-appearance
// order or the declared one, items by order band with separators and their chords, nested paths as
// submenus, leading list-driven items, enabled and checked as the registry answers when a menu
// opens, a click executing through the registry, the bar built again on a registration, and a bar
// that dies leaving nothing behind.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.gui;
import editor.core;
import editor.app;

using namespace foundation::core;
using namespace editor;
namespace gui = foundation::gui;

namespace
{
    EditorActionDeclaration Declare(StringView id, StringView label, StringView menuPath, i32 order)
    {
        EditorActionDeclaration d;
        d.id = String(id);
        d.label = String(label);
        d.menuPath = String(menuPath);
        d.menuOrder = order;
        d.execute = [](EditorPage*) {};
        return d;
    }
    // A menu's rows: an item's text (">" after a submenu's), "-" a separator.
    String Labels(const gui::Menu& menu)
    {
        String out;
        bool first = true;
        for (usize i = 0; i < menu.ChildCount(); ++i)
        {
            gui::Node* row = menu.GetChildAt(i);
            StringView text;
            bool sub = false;
            if (Cast<gui::MenuSeparator>(row) != nullptr)
                text = u8"-";
            else if (auto* subItem = Cast<gui::MenuSubItem>(row))
            {
                text = subItem->GetText();
                sub = true;
            }
            else if (auto* item = Cast<gui::MenuItem>(row))
                text = item->GetText();
            else
                continue; // its scroll arrows
            if (!first)
                out += u8"|";
            first = false;
            out += text;
            if (sub)
                out += u8">";
        }
        return out;
    }
    gui::MenuItem* ItemNamed(const gui::Menu& menu, StringView text)
    {
        for (usize i = 0; i < menu.ChildCount(); ++i)
            if (auto* item = Cast<gui::MenuItem>(menu.GetChildAt(i)); item != nullptr && item->GetText() == text)
                return item;
        return nullptr;
    }
    gui::Menu* SubMenuNamed(const gui::Menu& menu, StringView text)
    {
        for (usize i = 0; i < menu.ChildCount(); ++i)
            if (auto* item = Cast<gui::MenuSubItem>(menu.GetChildAt(i)); item != nullptr && item->GetText() == text)
                return item->GetSubMenu();
        return nullptr;
    }

    struct Bed
    {
        RefPtr<gui::SceneRoot> root = MakeRef<gui::SceneRoot>(DefaultAllocator(), MakeRef<gui::GUIContext>(DefaultAllocator()));
        RefPtr<gui::MenuBar> bar = MakeRef<gui::MenuBar>(DefaultAllocator());
        Bed()
        {
            root->SetSize(Float2{800.0f, 600.0f});
            root->AddChild(bar.Get());
        }
        [[nodiscard]] String Title(usize index) const { return String(bar->ButtonAt(index)->GetText()); }
    };
}

TEST_CASE("action-menus: the bar is generated from the registry - menus in first-appearance order, items by band with "
          "separators and chords, submenus from nested paths, leading items first, enabled and checked as the registry "
          "answers on opening, a click through the funnel")
{
    EditorActionRegistry actions;
    u32 saves = 0;
    bool dirty = false;
    bool grid = false;
    EditorActionDeclaration save = Declare(u8"file.save", u8"Save", u8"File/Save", 100);
    save.enabled = [&dirty](EditorPage*) { return dirty; };
    save.execute = [&saves](EditorPage*) { ++saves; };
    save.shortcut = EditorShortcut::Of(u8"ctrl-s");
    REQUIRE(actions.Register(Move(save)));
    REQUIRE(actions.Register(Declare(u8"file.exit", u8"Exit", u8"File/Exit", 300)));
    REQUIRE(actions.Register(Declare(u8"edit.undo", u8"Undo", u8"Edit/Undo", 100)));
    REQUIRE(actions.Register(Declare(u8"file.saveAs", u8"Save As...", u8"File/Save As...", 101)));
    REQUIRE(actions.Register(Declare(u8"scene.sim.stop", u8"Stop", u8"Scene/Simulate/Stop", 101)));
    REQUIRE(actions.Register(Declare(u8"scene.sim.start", u8"Start", u8"Scene/Simulate/Start", 100)));
    EditorActionDeclaration toggle = Declare(u8"scene.grid", u8"Grid", u8"Scene/Grid", 200);
    toggle.checked = [&grid](EditorPage*) { return grid; };
    REQUIRE(actions.Register(Move(toggle)));
    REQUIRE(actions.Register(Declare(u8"hidden.one", u8"No menu", u8"", 0))); // not in the bar

    Bed bed;
    app::ActionMenuBar menus(DefaultAllocator(), *bed.bar, actions);
    REQUIRE(bed.bar->MenuCount() == 3u);
    CHECK(bed.Title(0) == String(u8"File"));
    CHECK(bed.Title(1) == String(u8"Edit"));
    CHECK(bed.Title(2) == String(u8"Scene"));
    // File: the 100 band (Save, Save As...), a separator, the 300 band (Exit).
    CHECK(Labels(*bed.bar->MenuAt(0)) == String(u8"Save|Save As...|-|Exit"));
    CHECK(Labels(*bed.bar->MenuAt(1)) == String(u8"Undo"));
    CHECK(ItemNamed(*bed.bar->MenuAt(0), u8"Save")->GetShortcut() == FormatShortcut(EditorShortcut::Of(u8"ctrl-s")).AsView());
    // Scene/Simulate/* nests: one submenu holding Start then Stop by order.
    CHECK(Labels(*bed.bar->MenuAt(2)) == String(u8"Simulate>|-|Grid"));
    gui::Menu* simulate = SubMenuNamed(*bed.bar->MenuAt(2), u8"Simulate");
    REQUIRE(simulate != nullptr);
    CHECK(Labels(*simulate) == String(u8"Start|Stop"));

    // Enabled and checked are the registry's answer as the menu opens, opened the way a click on
    // the bar opens it.
    gui::MenuItem* saveItem = ItemNamed(*bed.bar->MenuAt(0), u8"Save");
    CHECK_FALSE(saveItem->IsEnabled());
    dirty = true;
    CHECK_FALSE(saveItem->IsEnabled()); // not yet: built before the change
    REQUIRE(bed.bar->ButtonAt(0)->HandleActivate());
    CHECK(bed.bar->MenuAt(0)->IsOpen());
    CHECK(saveItem->IsEnabled());
    bed.bar->MenuAt(0)->Close();
    gui::MenuItem* gridItem = ItemNamed(*bed.bar->MenuAt(2), u8"Grid");
    REQUIRE(gridItem != nullptr);
    CHECK(gridItem->IsCheckable());
    CHECK_FALSE(gridItem->IsChecked());
    grid = true;
    REQUIRE(bed.bar->ButtonAt(2)->HandleActivate());
    CHECK(gridItem->IsChecked());
    bed.bar->MenuAt(2)->Close();
    // A click executes through the registry.
    CHECK(saveItem->HandleActivate());
    CHECK(saves == 1u);

    // Leading items come first, then a separator, then the actions; a registration builds the bar
    // again and a new menu takes its place at the end.
    u32 leading = 0;
    menus.AddLeadingItems(u8"File", [&leading](gui::Menu& file)
                          {
                              (void)file.AddItem(u8"New Scene", [&leading]() { ++leading; });
                              (void)file.AddItem(u8"New Material", []() {});
                          });
    CHECK(Labels(*bed.bar->MenuAt(0)) == String(u8"New Scene|New Material|-|Save|Save As...|-|Exit"));
    CHECK(ItemNamed(*bed.bar->MenuAt(0), u8"New Scene")->HandleActivate());
    CHECK(leading == 1u);
    REQUIRE(actions.Register(Declare(u8"help.about", u8"About", u8"Help/About", 100)));
    REQUIRE(bed.bar->MenuCount() == 4u);
    CHECK(bed.Title(3) == String(u8"Help"));
    CHECK(Labels(*bed.bar->MenuAt(3)) == String(u8"About"));
    // A menu with only leading items and no actions shows them without a trailing separator.
    menus.AddLeadingItems(u8"Recent", [](gui::Menu& recent) { (void)recent.AddItem(u8"yesterday.scene", []() {}); });
    REQUIRE(bed.bar->MenuCount() == 5u);
    CHECK(Labels(*bed.bar->MenuAt(4)) == String(u8"yesterday.scene"));

    // A declared order: the named menus in their places, the rest at "*", in the order they first
    // appear; a domain's menu registered after Help still lands before it.
    const StringView order[] = {u8"File", u8"Edit", u8"*", u8"Help", u8"Nowhere"};
    menus.SetMenuOrder(Span<const StringView>(order, 5));
    REQUIRE(bed.bar->MenuCount() == 5u); // "Nowhere" has nothing in it: no menu
    CHECK(bed.Title(0) == String(u8"File"));
    CHECK(bed.Title(1) == String(u8"Edit"));
    CHECK(bed.Title(2) == String(u8"Scene"));
    CHECK(bed.Title(3) == String(u8"Recent"));
    CHECK(bed.Title(4) == String(u8"Help"));
    REQUIRE(actions.Register(Declare(u8"tools.bake", u8"Bake", u8"Tools/Bake", 100))); // late
    REQUIRE(bed.bar->MenuCount() == 6u);
    CHECK(bed.Title(3) == String(u8"Tools")); // with the action menus, before the leading-only Recent
    CHECK(bed.Title(4) == String(u8"Recent"));
    CHECK(bed.Title(5) == String(u8"Help"));
    CHECK(Labels(*bed.bar->MenuAt(0)) == String(u8"New Scene|New Material|-|Save|Save As...|-|Exit")); // items unchanged
}

TEST_CASE("action-menus: a generated bar that dies leaves it empty, and the registry no longer reaches it")
{
    EditorActionRegistry actions;
    REQUIRE(actions.Register(Declare(u8"file.save", u8"Save", u8"File/Save", 100)));
    Bed bed;
    {
        app::ActionMenuBar menus(DefaultAllocator(), *bed.bar, actions);
        CHECK(bed.bar->MenuCount() == 1u);
    }
    CHECK(bed.bar->MenuCount() == 0u);
    // A registration after it is gone reaches nobody.
    REQUIRE(actions.Register(Declare(u8"file.exit", u8"Exit", u8"File/Exit", 300)));
    CHECK(bed.bar->MenuCount() == 0u);
}
