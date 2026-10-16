// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App tests - the surfaces generated from the action registry: the menu bar (menus in
// first-appearance order, items by order band with separators, nested paths as submenus,
// leading list-driven items, enabled as the registry answers when a menu opens, a click
// executing through the registry, a rebuild on a registration) and the global shortcuts (one
// binding per effective chord and alternate, executing through the registry, rebound on a
// rebind and on a registration).
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.ui;
import foundation.ui.toolkit;
import editor.core;
import editor.app;

using namespace foundation::core;
using namespace editor;
namespace ui = foundation::ui;

namespace
{
    EditorActionDeclaration Declare(StringView id, StringView label, StringView menuPath,
                                    i32 order)
    {
        EditorActionDeclaration d;
        d.id = String(id);
        d.label = String(label);
        d.menuPath = String(menuPath);
        d.menuOrder = order;
        d.execute = [](EditorPage*) {};
        return d;
    }
    String Labels(const ui::ContextMenu& menu)
    {
        String out;
        for (i32 i = 0; i < menu.ItemCount(); ++i)
        {
            const ui::MenuItem* item = menu.ItemAt(i);
            if (i > 0)
            {
                out += u8"|";
            }
            out += item->IsSeparator ? StringView(u8"-") : item->Label.AsView();
            if (item->Submenu)
            {
                out += u8">";
            }
        }
        return out;
    }
}

TEST_CASE("action-menus: the bar is generated from the registry - menus in first-appearance "
          "order, items by band with separators, submenus from nested paths, leading items "
          "first, enabled as the registry answers on opening, a click through the funnel")
{
    EditorActionRegistry actions;
    u32 saves = 0;
    bool dirty = false;
    EditorActionDeclaration save = Declare(u8"file.save", u8"Save", u8"File/Save", 100);
    save.enabled = [&dirty](EditorPage*) { return dirty; };
    save.execute = [&saves](EditorPage*) { ++saves; };
    REQUIRE(actions.Register(Move(save)));
    REQUIRE(actions.Register(Declare(u8"file.exit", u8"Exit", u8"File/Exit", 300)));
    REQUIRE(actions.Register(Declare(u8"edit.undo", u8"Undo", u8"Edit/Undo", 100)));
    REQUIRE(actions.Register(Declare(u8"file.saveAs", u8"Save As...", u8"File/Save As...", 101)));
    REQUIRE(actions.Register(Declare(u8"scene.sim.stop", u8"Stop", u8"Scene/Simulate/Stop", 101)));
    REQUIRE(actions.Register(Declare(u8"scene.sim.start", u8"Start", u8"Scene/Simulate/Start", 100)));
    REQUIRE(actions.Register(Declare(u8"hidden.one", u8"No menu", u8"", 0))); // not in the bar

    ui::UIContext ctx{DefaultAllocator()};
    auto root = MakeRef<ui::RootView>(DefaultAllocator());
    root->ViewportSize = Float2{800.0f, 600.0f};
    ctx.AddRootView(root.Get());
    auto bar = MakeRef<ui::toolkit::MenuBar>(DefaultAllocator());
    root->AddView(bar.Get());
    app::ActionMenuBar menus(*bar, actions);
    REQUIRE(bar->MenuCount() == 3u);
    CHECK(bar->MenuTitle(0) == u8"File");
    CHECK(bar->MenuTitle(1) == u8"Edit");
    CHECK(bar->MenuTitle(2) == u8"Scene");
    // File: the 100 band (Save, Save As...), a separator, the 300 band (Exit).
    CHECK(Labels(*bar->MenuAt(0)) == u8"Save|Save As...|-|Exit");
    CHECK(Labels(*bar->MenuAt(1)) == u8"Undo");
    // Scene/Simulate/* nests: one submenu holding Start then Stop by order.
    CHECK(Labels(*bar->MenuAt(2)) == u8"Simulate>");
    const ui::MenuItem* simulate = bar->MenuAt(2)->ItemAt(0);
    REQUIRE(simulate->Submenu);
    CHECK(Labels(*Cast<ui::ContextMenu>(simulate->Submenu.Get())) == u8"Start|Stop");

    // Enabled is the registry's answer at the moment the menu opens - opened the way a click
    // on the bar opens it, not by calling the hook by hand (the bar once skipped the hook).
    CHECK_FALSE(bar->MenuAt(0)->ItemAt(0)->Enabled);
    dirty = true;
    CHECK_FALSE(bar->MenuAt(0)->ItemAt(0)->Enabled); // not yet: built before the change
    bar->OpenMenuAt(0);
    CHECK(bar->MenuAt(0)->ItemAt(0)->Enabled);
    CHECK(root->GetPopupLayer()->PopupCount() == 1u);
    bar->ClearMenus();
    menus.Rebuild();
    CHECK(bar->MenuAt(0)->ItemAt(0)->Enabled);
    // A click executes through the registry.
    bar->MenuAt(0)->ItemAt(0)->Action();
    CHECK(saves == 1u);

    // Leading items come first, then a separator, then the actions; a registration rebuilds
    // the bar and a new menu takes its place at the end.
    u32 leading = 0;
    menus.AddLeadingItems(u8"File", [&leading](ui::ContextMenu& file)
                          {
                              file.AddItem(u8"New Scene", [&leading]() { ++leading; });
                              file.AddItem(u8"New Material", []() {});
                          });
    CHECK(Labels(*bar->MenuAt(0)) == u8"New Scene|New Material|-|Save|Save As...|-|Exit");
    bar->MenuAt(0)->ItemAt(0)->Action();
    CHECK(leading == 1u);
    REQUIRE(actions.Register(Declare(u8"help.about", u8"About", u8"Help/About", 100)));
    REQUIRE(bar->MenuCount() == 4u);
    CHECK(bar->MenuTitle(3) == u8"Help");
    CHECK(Labels(*bar->MenuAt(3)) == u8"About");
    // A menu with only leading items and no actions shows them without a trailing separator.
    menus.AddLeadingItems(u8"Recent", [](ui::ContextMenu& recent)
                          { recent.AddItem(u8"yesterday.scene", []() {}); });
    REQUIRE(bar->MenuCount() == 5u);
    CHECK(Labels(*bar->MenuAt(4)) == u8"yesterday.scene");

    // A declared order: the named menus in their places, the rest at "*", in the order they first
    // appear; a domain's menu registered after Help still lands before it.
    const StringView order[] = {u8"File", u8"Edit", u8"*", u8"Help", u8"Nowhere"};
    menus.SetMenuOrder(Span<const StringView>(order, 5));
    REQUIRE(bar->MenuCount() == 5u); // "Nowhere" has nothing in it: no menu
    CHECK(bar->MenuTitle(0) == u8"File");
    CHECK(bar->MenuTitle(1) == u8"Edit");
    CHECK(bar->MenuTitle(2) == u8"Scene");
    CHECK(bar->MenuTitle(3) == u8"Recent");
    CHECK(bar->MenuTitle(4) == u8"Help");
    REQUIRE(actions.Register(Declare(u8"tools.bake", u8"Bake", u8"Tools/Bake", 100))); // late
    REQUIRE(bar->MenuCount() == 6u);
    CHECK(bar->MenuTitle(3) == u8"Tools"); // with the action menus, before the leading-only Recent
    CHECK(bar->MenuTitle(4) == u8"Recent");
    CHECK(bar->MenuTitle(5) == u8"Help");
    CHECK(Labels(*bar->MenuAt(0)) == u8"New Scene|New Material|-|Save|Save As...|-|Exit"); // items unchanged
}

TEST_CASE("action-surfaces: a generated bar or shortcut set that dies leaves nothing behind - the "
          "bar empty, the globals gone, the registry no longer calling back")
{
    ui::UIContext ctx{DefaultAllocator()};
    auto root = MakeRef<ui::RootView>(DefaultAllocator());
    root->ViewportSize = Float2{800.0f, 600.0f};
    ctx.AddRootView(root.Get());
    EditorActionRegistry actions;
    EditorActionDeclaration save = Declare(u8"file.save", u8"Save", u8"File/Save", 100);
    save.shortcut = EditorShortcut{ui::KeyCode::S, ui::KeyModifiers::Ctrl};
    REQUIRE(actions.Register(Move(save)));
    auto bar = MakeRef<ui::toolkit::MenuBar>(DefaultAllocator());
    const usize before = ctx.GetShortcuts()->Count();
    {
        app::ActionMenuBar menus(*bar, actions);
        app::ActionShortcuts shortcuts(*ctx.GetShortcuts(), actions);
        CHECK(bar->MenuCount() == 1u);
        CHECK(shortcuts.BoundCount() == 1u);
        CHECK(actions.OnActionsChanged.Count() == 2u);
    }
    CHECK(bar->MenuCount() == 0u);
    CHECK(ctx.GetShortcuts()->Count() == before);
    CHECK(actions.OnActionsChanged.Count() == 0u);
    // A registration after they are gone reaches nobody (it once called into freed memory).
    REQUIRE(actions.Register(Declare(u8"file.exit", u8"Exit", u8"File/Exit", 300)));
    CHECK(bar->MenuCount() == 0u);
}

TEST_CASE("action-shortcuts: one global per effective chord and alternate, executing through "
          "the registry; rebound on a rebind and on a registration")
{
    ui::UIContext ctx{DefaultAllocator()};
    auto root = MakeRef<ui::RootView>(DefaultAllocator());
    root->ViewportSize = Float2{800.0f, 600.0f};
    ctx.AddRootView(root.Get());

    EditorActionRegistry actions;
    u32 saves = 0;
    u32 redos = 0;
    EditorActionDeclaration save = Declare(u8"file.save", u8"Save", u8"File/Save", 100);
    save.shortcut = EditorShortcut{ui::KeyCode::S, ui::KeyModifiers::Ctrl};
    save.execute = [&saves](EditorPage*) { ++saves; };
    REQUIRE(actions.Register(Move(save)));
    EditorActionDeclaration redo = Declare(u8"edit.redo", u8"Redo", u8"Edit/Redo", 101);
    redo.shortcut = EditorShortcut{ui::KeyCode::Z, ui::KeyModifiers::Ctrl | ui::KeyModifiers::Shift};
    redo.alternateShortcut = EditorShortcut{ui::KeyCode::Y, ui::KeyModifiers::Ctrl};
    redo.execute = [&redos](EditorPage*) { ++redos; };
    REQUIRE(actions.Register(Move(redo)));
    REQUIRE(actions.Register(Declare(u8"view.reset", u8"Reset Layout", u8"View/Reset Layout", 100)));

    app::ActionShortcuts shortcuts(*ctx.GetShortcuts(), actions);
    CHECK(shortcuts.BoundCount() == 3u); // save, redo, redo's alternate; reset has none
    CHECK(ctx.GetShortcuts()->TryDispatch(ui::KeyCode::S, ui::KeyModifiers::LeftCtrl));
    CHECK(saves == 1u);
    CHECK(ctx.GetShortcuts()->TryDispatch(ui::KeyCode::Y, ui::KeyModifiers::LeftCtrl));
    CHECK(redos == 1u);
    CHECK(ctx.GetShortcuts()->TryDispatch(ui::KeyCode::Z,
                                          ui::KeyModifiers::LeftCtrl | ui::KeyModifiers::LeftShift));
    CHECK(redos == 2u);
    CHECK_FALSE(ctx.GetShortcuts()->TryDispatch(ui::KeyCode::S, ui::KeyModifiers::None));

    // A rebind moves the binding; the old chord is free, the new one fires.
    REQUIRE(actions.Rebind(u8"file.save", EditorShortcut{ui::KeyCode::F2, ui::KeyModifiers::None}).IsOk());
    CHECK(shortcuts.BoundCount() == 3u);
    CHECK_FALSE(ctx.GetShortcuts()->TryDispatch(ui::KeyCode::S, ui::KeyModifiers::LeftCtrl));
    CHECK(ctx.GetShortcuts()->TryDispatch(ui::KeyCode::F2, ui::KeyModifiers::None));
    CHECK(saves == 2u);
    // Clearing an override unbinds; a registration with a chord binds.
    REQUIRE(actions.Rebind(u8"file.save", EditorShortcut{}).IsOk());
    CHECK(shortcuts.BoundCount() == 2u);
    CHECK_FALSE(ctx.GetShortcuts()->TryDispatch(ui::KeyCode::F2, ui::KeyModifiers::None));
    EditorActionDeclaration run = Declare(u8"sim.run", u8"Simulate", u8"", 0);
    run.shortcut = EditorShortcut{ui::KeyCode::F5, ui::KeyModifiers::None};
    u32 runs = 0;
    run.execute = [&runs](EditorPage*) { ++runs; };
    REQUIRE(actions.Register(Move(run)));
    CHECK(shortcuts.BoundCount() == 3u);
    CHECK(ctx.GetShortcuts()->TryDispatch(ui::KeyCode::F5, ui::KeyModifiers::None));
    CHECK(runs == 1u);
}
