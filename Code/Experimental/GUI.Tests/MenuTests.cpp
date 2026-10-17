// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - Menu tests: opening at a position as a popup, activating an item (runs the
// action + closes), and dismissal (outside click / Escape) via the dispatcher popup support.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import experimental.gui;

using namespace experimental::gui;
namespace core = foundation::core;

namespace
{
    template <typename T>
    core::RefPtr<T> Make()
    {
        return core::MakeRef<T>(core::DefaultAllocator());
    }
}

TEST_CASE("menu: sizes to its items")
{
    auto menu = Make<Menu>();
    menu->SetWidth(140.0f);
    menu->SetItemHeight(25.0f);
    menu->AddItem(core::StringView(u8"Cut"), [] {});
    menu->AddItem(core::StringView(u8"Copy"), [] {});
    menu->AddItem(core::StringView(u8"Paste"), [] {});

    CHECK(menu->ItemCount() == 3);
    CHECK(menu->GetSize().x == doctest::Approx(140.0f));
    CHECK(menu->GetSize().y == doctest::Approx(75.0f)); // 3 * 25
}

TEST_CASE("menu: Open shows it as the dispatcher popup at the given position")
{
    auto root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
    root->SetSize(core::Float2{400.0f, 400.0f});
    auto anchor = Make<UIWidget>();
    anchor->SetSize(core::Float2{10.0f, 10.0f});
    root->AddChild(anchor.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    auto menu = Make<Menu>();
    menu->SetItemHeight(26.0f);
    menu->AddItem(core::StringView(u8"One"), [] {});
    menu->AddItem(core::StringView(u8"Two"), [] {});

    menu->Open(*anchor, core::Float2{100.0f, 80.0f});
    CHECK(menu->IsOpen());
    CHECK(d->TopPopup() == menu.Get());
    CHECK(menu->GetPosition().x == doctest::Approx(100.0f));
    CHECK(menu->GetPosition().y == doctest::Approx(80.0f));
    CHECK(menu->GetParent() == anchor.Get()); // a popup of what opened it
}

TEST_CASE("menu: activating an item runs its action and closes")
{
    auto root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
    root->SetSize(core::Float2{400.0f, 400.0f});
    auto anchor = Make<UIWidget>();
    root->AddChild(anchor.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    int cut = 0, paste = 0;
    auto menu = Make<Menu>();
    menu->SetItemHeight(26.0f);
    menu->AddItem(core::StringView(u8"Cut"), [&] { ++cut; });
    menu->AddItem(core::StringView(u8"Paste"), [&] { ++paste; });
    menu->Open(*anchor, core::Float2{50.0f, 50.0f});

    // Item 1 (Paste) spans y in [50 + 26, 50 + 52) = [76, 102); click it.
    d->InjectMouseDown(core::Float2{60.0f, 88.0f}, MouseButton::Left);
    d->InjectMouseUp(core::Float2{60.0f, 88.0f}, MouseButton::Left);
    CHECK(paste == 1);
    CHECK(cut == 0);
    CHECK_FALSE(menu->IsOpen());
    CHECK(d->PopupCount() == 0u);
    CHECK(menu->GetParent() == nullptr); // removed from the tree
}

TEST_CASE("menu: outside click and Escape dismiss it")
{
    auto root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
    root->SetSize(core::Float2{400.0f, 400.0f});
    auto anchor = Make<UIWidget>();
    root->AddChild(anchor.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    auto menu = Make<Menu>();
    menu->AddItem(core::StringView(u8"One"), [] {});
    menu->Open(*anchor, core::Float2{50.0f, 50.0f});
    REQUIRE(menu->IsOpen());

    d->InjectMouseDown(core::Float2{300.0f, 300.0f}, MouseButton::Left); // outside
    CHECK_FALSE(menu->IsOpen());

    // Reopen and dismiss with Escape.
    menu->Open(*anchor, core::Float2{50.0f, 50.0f});
    REQUIRE(menu->IsOpen());
    d->InjectKeyDown(static_cast<core::u32>(KeyCode::Escape));
    CHECK_FALSE(menu->IsOpen());
}

TEST_CASE("menu: an outside click dismisses even when the opener covers the click point")
{
    // Regression: the context menu was opened with the full-window panel as its popup owner,
    // so clicks anywhere on the panel counted as "on the owner" and never dismissed. A menu
    // has no persistent owner - any press outside the menu itself must close it.
    auto root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
    root->SetSize(core::Float2{400.0f, 400.0f});
    auto panel = Make<UIWidget>();
    panel->SetSize(core::Float2{400.0f, 400.0f}); // covers the whole area (like the sandbox panel)
    root->AddChild(panel.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    auto menu = Make<Menu>();
    menu->SetItemHeight(26.0f);
    menu->AddItem(core::StringView(u8"One"), [] {});
    menu->Open(*panel, core::Float2{50.0f, 50.0f});
    REQUIRE(menu->IsOpen());

    // Left-click on the panel, well away from the menu -> dismissed.
    d->InjectMouseDown(core::Float2{300.0f, 300.0f}, MouseButton::Left);
    CHECK_FALSE(menu->IsOpen());
    CHECK(d->PopupCount() == 0u);
}

TEST_CASE("menu: can be reopened after being dismissed")
{
    auto root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
    root->SetSize(core::Float2{400.0f, 400.0f});
    auto panel = Make<UIWidget>();
    panel->SetSize(core::Float2{400.0f, 400.0f});
    root->AddChild(panel.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    auto menu = Make<Menu>();
    menu->AddItem(core::StringView(u8"One"), [] {});

    menu->Open(*panel, core::Float2{40.0f, 40.0f});
    REQUIRE(menu->IsOpen());
    d->InjectMouseDown(core::Float2{300.0f, 300.0f}, MouseButton::Left); // dismiss
    REQUIRE_FALSE(menu->IsOpen());

    // Reopen at a new position - must work and re-register as the popup.
    menu->Open(*panel, core::Float2{120.0f, 90.0f});
    CHECK(menu->IsOpen());
    CHECK(d->TopPopup() == menu.Get());
    CHECK(menu->GetParent() == panel.Get()); // a popup of what opened it
    CHECK(menu->GetPosition().x == doctest::Approx(120.0f));
}

// ===================== Depth: separators / checkable / submenus =====================

TEST_CASE("menu: separators are non-selectable rows that add height")
{
    auto menu = Make<Menu>();
    menu->SetWidth(140.0f);
    menu->SetItemHeight(26.0f);
    menu->AddItem(core::StringView(u8"Open"), [] {});
    menu->AddSeparator();
    menu->AddItem(core::StringView(u8"Quit"), [] {});

    CHECK(menu->RowCount() == 3);
    CHECK(menu->ItemCount() == 2); // the separator is not counted as an item
    CHECK(menu->GetSize().y == doctest::Approx(26.0f + 9.0f + 26.0f)); // item + separator + item
}

TEST_CASE("menu: a checkable item toggles, fires its callback, and closes the menu")
{
    auto root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
    root->SetSize(core::Float2{400.0f, 400.0f});
    auto anchor = Make<UIWidget>();
    root->AddChild(anchor.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    bool state = false;
    int toggles = 0;
    auto menu = Make<Menu>();
    menu->SetItemHeight(26.0f);
    MenuItem* check = menu->AddCheckItem(core::StringView(u8"Bold"), false,
                                         [&](bool on)
                                         {
                                             state = on;
                                             ++toggles;
                                         });
    menu->Open(*anchor, core::Float2{50.0f, 50.0f});

    CHECK_FALSE(check->IsChecked());
    // Row 0 spans y in [50, 76); click it.
    d->InjectMouseDown(core::Float2{60.0f, 60.0f}, MouseButton::Left);
    d->InjectMouseUp(core::Float2{60.0f, 60.0f}, MouseButton::Left);
    CHECK(check->IsChecked());
    CHECK(state == true);
    CHECK(toggles == 1);
    CHECK_FALSE(menu->IsOpen()); // menus close on activation
}

TEST_CASE("menu: a submenu opens to the right of its item on hover")
{
    auto root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
    root->SetSize(core::Float2{600.0f, 600.0f});
    auto anchor = Make<UIWidget>();
    root->AddChild(anchor.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    auto menu = Make<Menu>();
    menu->SetWidth(160.0f);
    menu->SetItemHeight(26.0f);
    menu->AddItem(core::StringView(u8"Top"), [] {});
    Menu* sub = menu->AddSubMenu(core::StringView(u8"More"));
    sub->AddItem(core::StringView(u8"Deep"), [] {});
    menu->Open(*anchor, core::Float2{50.0f, 50.0f});

    CHECK(menu->CurrentSubMenu() == nullptr);
    // Hover the "More" row (row 1: y in [76, 102)).
    d->InjectMouseMove(core::Float2{60.0f, 88.0f});
    CHECK(menu->CurrentSubMenu() == sub);
    CHECK(sub->IsOpen());
    CHECK(sub->GetParent()->GetParent() == menu.Get());     // a popup of its item, in the menu
    CHECK(sub->GetPosition().x == doctest::Approx(210.0f)); // menu.x(50) + menu.width(160)
    CHECK(sub->GetPosition().y == doctest::Approx(76.0f));  // menu.y(50) + item.y(26)
}

TEST_CASE("menu: hovering a different row closes the open submenu")
{
    auto root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
    root->SetSize(core::Float2{600.0f, 600.0f});
    auto anchor = Make<UIWidget>();
    root->AddChild(anchor.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    auto menu = Make<Menu>();
    menu->SetWidth(160.0f);
    menu->SetItemHeight(26.0f);
    menu->AddItem(core::StringView(u8"Top"), [] {});          // row 0
    Menu* sub = menu->AddSubMenu(core::StringView(u8"More")); // row 1
    sub->AddItem(core::StringView(u8"Deep"), [] {});
    menu->Open(*anchor, core::Float2{50.0f, 50.0f});

    d->InjectMouseMove(core::Float2{60.0f, 88.0f}); // open submenu (hover "More")
    REQUIRE(menu->CurrentSubMenu() == sub);
    d->InjectMouseMove(core::Float2{60.0f, 60.0f}); // hover "Top" (row 0)
    CHECK(menu->CurrentSubMenu() == nullptr);
    CHECK(sub->GetParent() == nullptr); // submenu removed from the tree
}

TEST_CASE(
    "menu: clicking inside an open submenu does not dismiss; activating closes the whole chain")
{
    auto root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
    root->SetSize(core::Float2{600.0f, 600.0f});
    auto anchor = Make<UIWidget>();
    root->AddChild(anchor.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    int deep = 0;
    auto menu = Make<Menu>();
    menu->SetWidth(160.0f);
    menu->SetItemHeight(26.0f);
    menu->AddItem(core::StringView(u8"Top"), [] {});
    Menu* sub = menu->AddSubMenu(core::StringView(u8"More"));
    sub->AddItem(core::StringView(u8"Deep"), [&] { ++deep; });
    menu->Open(*anchor, core::Float2{50.0f, 50.0f});

    d->InjectMouseMove(core::Float2{60.0f, 88.0f}); // open submenu
    REQUIRE(sub->IsOpen());

    // The submenu sits at x in [210, 370], its "Deep" row at y in [76, 102). A press there is
    // inside the chain -> the root menu must stay open (the popup 'contains' predicate).
    d->InjectMouseDown(core::Float2{260.0f, 88.0f}, MouseButton::Left);
    CHECK(menu->IsOpen());
    d->InjectMouseUp(core::Float2{260.0f, 88.0f}, MouseButton::Left);
    CHECK(deep == 1);
    CHECK_FALSE(menu->IsOpen()); // the whole chain closed
    CHECK(menu->GetParent() == nullptr);
    CHECK(sub->GetParent() == nullptr);
    CHECK(d->PopupCount() == 0u);
}

TEST_CASE("menu: an outside click dismisses the whole open submenu chain")
{
    auto root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
    root->SetSize(core::Float2{600.0f, 600.0f});
    auto anchor = Make<UIWidget>();
    root->AddChild(anchor.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    auto menu = Make<Menu>();
    menu->SetWidth(160.0f);
    menu->SetItemHeight(26.0f);
    Menu* sub = menu->AddSubMenu(core::StringView(u8"More"));
    sub->AddItem(core::StringView(u8"Deep"), [] {});
    menu->Open(*anchor, core::Float2{50.0f, 50.0f});

    d->InjectMouseMove(core::Float2{60.0f, 60.0f}); // "More" is row 0 (y in [50,76)) -> open sub
    REQUIRE(sub->IsOpen());

    d->InjectMouseDown(core::Float2{500.0f, 500.0f}, MouseButton::Left); // outside everything
    CHECK_FALSE(menu->IsOpen());
    CHECK(menu->GetParent() == nullptr);
    CHECK(sub->GetParent() == nullptr);
    CHECK(d->PopupCount() == 0u);
}

TEST_CASE("menu: a submenu stays open while the pointer heads for it across another row, then gives way")
{
    auto root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
    root->SetSize(core::Float2{600.0f, 400.0f});
    auto anchor = Make<UIWidget>();
    root->AddChild(anchor.Get());
    EventDispatcher* d = root->GetEventDispatcher();
    auto menu = Make<Menu>();
    menu->SetItemHeight(26.0f);
    Menu* sub = menu->AddSubMenu(core::StringView(u8"More"));
    sub->AddItem(core::StringView(u8"A"), [] {});
    sub->AddItem(core::StringView(u8"B"), [] {});
    sub->AddItem(core::StringView(u8"C"), [] {});
    menu->AddItem(core::StringView(u8"Next"), [] {});
    menu->AddItem(core::StringView(u8"Last"), [] {});
    menu->Open(*anchor, core::Float2{50.0f, 50.0f}); // rows at y 50, 76, 102; 160 wide
    d->InjectMouseMove(core::Float2{100.0f, 60.0f}); // on "More": its submenu opens at x 210, y 50
    REQUIRE(sub->IsOpen());
    d->InjectMouseMove(core::Float2{150.0f, 70.0f});
    // Down and right toward the submenu, over "Next": the submenu stays.
    d->InjectMouseMove(core::Float2{190.0f, 80.0f});
    CHECK(sub->IsOpen());
    // Resting on "Next" a moment: it takes over.
    for (int i = 0; i < 5; ++i)
        root->GetContext().Update(core::Duration::FromSeconds(0.1));
    CHECK_FALSE(sub->IsOpen());

    // Straight down, away from it: the submenu closes at once.
    d->InjectMouseMove(core::Float2{100.0f, 60.0f});
    REQUIRE(sub->IsOpen());
    d->InjectMouseMove(core::Float2{100.0f, 110.0f}); // onto "Last", heading away
    CHECK_FALSE(sub->IsOpen());
}
