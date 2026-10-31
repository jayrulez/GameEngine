// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - MenuBar tests: a horizontal strip of buttons, each opening a Menu below it. Clicking a
// button toggles its menu; while one is open, hovering another button switches to it; an outside
// click dismisses and clears the bar's state. Clearing the bar's menus closes an open one, and a
// menu is told as it opens, as are its submenus. No font is set, so each button is 2*padding (24px)
// wide and the bar is `barHeight` tall, which gives a deterministic layout.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.gui;

using namespace foundation::gui;
namespace core = foundation::core;

namespace
{
    template <typename T>
    core::RefPtr<T> Make()
    {
        return core::MakeRef<T>(core::DefaultAllocator());
    }

    // A menu bar (2 menus) under a scene root, ready to receive input.
    struct BarFixture
    {
        core::RefPtr<SceneRoot> root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
        core::RefPtr<MenuBar> bar = Make<MenuBar>();
        Menu* fileMenu = nullptr;
        Menu* editMenu = nullptr;

        BarFixture()
        {
            root->SetSize(core::Float2{600.0f, 400.0f});
            bar->SetBarHeight(28.0f);
            bar->SetItemPadding(12.0f); // each button is 24px wide with no font
            root->AddChild(bar.Get());
            fileMenu = bar->AddMenu(core::StringView(u8"File"));
            fileMenu->AddItem(core::StringView(u8"Open"), [] {});
            editMenu = bar->AddMenu(core::StringView(u8"Edit"));
            editMenu->AddItem(core::StringView(u8"Copy"), [] {});
        }
        EventDispatcher* d() { return root->GetEventDispatcher(); }
    };
}

TEST_CASE("menubar: builds a button per menu and lays them out horizontally")
{
    BarFixture fx;
    CHECK(fx.bar->MenuCount() == 2);
    CHECK(fx.bar->ButtonAt(0)->GetPosition().x == doctest::Approx(0.0f));
    CHECK(fx.bar->ButtonAt(0)->GetSize().x == doctest::Approx(24.0f)); // 2 * padding
    CHECK(fx.bar->ButtonAt(1)->GetPosition().x == doctest::Approx(24.0f));
    CHECK(fx.bar->GetSize().y == doctest::Approx(28.0f));
}

TEST_CASE("menubar: clicking a button opens its menu below the button")
{
    BarFixture fx;
    EventDispatcher* d = fx.d();

    // Click "File" (button 0, centered at x=12, y=14).
    d->InjectMouseDown(core::Float2{12.0f, 14.0f}, MouseButton::Left);
    d->InjectMouseUp(core::Float2{12.0f, 14.0f}, MouseButton::Left);

    CHECK(fx.bar->CurrentMenu() == fx.fileMenu);
    CHECK(fx.fileMenu->IsOpen());
    CHECK(fx.fileMenu->GetParent() == fx.bar->ButtonAt(0)); // a popup of its button
    CHECK(fx.fileMenu->GetPosition().x == doctest::Approx(0.0f));
    CHECK(fx.fileMenu->GetPosition().y == doctest::Approx(28.0f)); // below the bar
}

TEST_CASE("menubar: clicking the open button again toggles the menu closed")
{
    BarFixture fx;
    EventDispatcher* d = fx.d();

    d->InjectMouseDown(core::Float2{12.0f, 14.0f}, MouseButton::Left);
    d->InjectMouseUp(core::Float2{12.0f, 14.0f}, MouseButton::Left);
    REQUIRE(fx.bar->CurrentMenu() == fx.fileMenu);

    // A second click on the same button closes it (the button is the popup owner, so the
    // press does not dismiss before the toggle runs).
    d->InjectMouseDown(core::Float2{12.0f, 14.0f}, MouseButton::Left);
    d->InjectMouseUp(core::Float2{12.0f, 14.0f}, MouseButton::Left);
    CHECK(fx.bar->CurrentMenu() == nullptr);
    CHECK_FALSE(fx.fileMenu->IsOpen());
}

TEST_CASE("menubar: with a menu open, hovering another button switches to it")
{
    BarFixture fx;
    EventDispatcher* d = fx.d();

    d->InjectMouseDown(core::Float2{12.0f, 14.0f}, MouseButton::Left); // open File
    d->InjectMouseUp(core::Float2{12.0f, 14.0f}, MouseButton::Left);
    REQUIRE(fx.bar->CurrentMenu() == fx.fileMenu);

    // Hover "Edit" (button 1, centered at x=36): it switches without a click.
    d->InjectMouseMove(core::Float2{36.0f, 14.0f});
    CHECK(fx.bar->CurrentMenu() == fx.editMenu);
    CHECK(fx.editMenu->IsOpen());
    CHECK_FALSE(fx.fileMenu->IsOpen());
    CHECK(fx.fileMenu->GetParent() == nullptr);
}

TEST_CASE("menubar: hovering a button with nothing open does not open a menu")
{
    BarFixture fx;
    fx.d()->InjectMouseMove(core::Float2{12.0f, 14.0f}); // bare hover over "File"
    CHECK(fx.bar->CurrentMenu() == nullptr);
    CHECK_FALSE(fx.fileMenu->IsOpen());
}

TEST_CASE("menubar: an outside click dismisses the open menu and clears the bar state")
{
    BarFixture fx;
    EventDispatcher* d = fx.d();

    d->InjectMouseDown(core::Float2{12.0f, 14.0f}, MouseButton::Left);
    d->InjectMouseUp(core::Float2{12.0f, 14.0f}, MouseButton::Left);
    REQUIRE(fx.fileMenu->IsOpen());

    d->InjectMouseDown(core::Float2{300.0f, 300.0f}, MouseButton::Left); // far outside
    CHECK(fx.bar->CurrentMenu() == nullptr);
    CHECK_FALSE(fx.fileMenu->IsOpen());
    CHECK(fx.fileMenu->GetParent() == nullptr);
}

TEST_CASE("menubar: a sheet sets its height and its buttons' padding")
{
    BarFixture fx;
    StyleManager styles;
    styles.SetStyleSheet(CSSParser::Parse(core::StringView(u8"menubar { bar-height: calc(20px + 2px); item-padding: 8px; }")));
    styles.ApplyTree(*fx.root.Get());
    CHECK(fx.bar->GetBarHeight() == doctest::Approx(22.0f));
    CHECK(fx.bar->GetSize().y == doctest::Approx(22.0f));
    CHECK(fx.bar->ButtonAt(0)->GetSize().y == doctest::Approx(22.0f));
    CHECK(fx.bar->ButtonAt(0)->GetSize().x == doctest::Approx(16.0f)); // 2 * padding, no font
}

TEST_CASE("menubar: the built-in theme rings no menu bar: a click opening a menu shows no ring")
{
    BarFixture fx;
    StyleManager styles;
    styles.SetUserAgentSheet(CSSParser::Parse(DefaultThemeCSS()));
    // A click on the first button: it takes the focus and opens its menu.
    fx.d()->InjectMouseDown(core::Float2{12.0f, 14.0f}, MouseButton::Left);
    fx.d()->InjectMouseUp(core::Float2{12.0f, 14.0f}, MouseButton::Left);
    REQUIRE(fx.fileMenu->IsOpen());
    REQUIRE(fx.bar->IsFocusWithin()); // the focus is within the bar, yet no ring is drawn
    styles.ApplyTree(*fx.root.Get());
    CHECK(fx.bar->GetBox().Shadows.IsEmpty());
}

TEST_CASE("menubar: a menu is told each time just before it opens, its items as they are then; a submenu too")
{
    BarFixture fx;
    EventDispatcher* d = fx.d();
    int opening = 0;
    MenuItem* open = nullptr;
    for (core::usize i = 0; i < fx.fileMenu->ChildCount(); ++i)
        if (auto* item = core::Cast<MenuItem>(fx.fileMenu->GetChildAt(i)))
            open = item;
    REQUIRE(open != nullptr);
    fx.fileMenu->SetOnOpening([&](Menu& menu)
    {
        CHECK(&menu == fx.fileMenu);
        CHECK_FALSE(menu.IsOpen()); // told before it opens, not after
        ++opening;
        open->SetEnabled(opening % 2 == 0); // the owner updates the item as it knows now
    });
    d->InjectMouseDown(core::Float2{12.0f, 14.0f}, MouseButton::Left);
    d->InjectMouseUp(core::Float2{12.0f, 14.0f}, MouseButton::Left);
    CHECK(opening == 1);
    CHECK(fx.fileMenu->IsOpen());
    CHECK_FALSE(open->IsEnabled());
    fx.fileMenu->Close();
    d->InjectMouseDown(core::Float2{12.0f, 14.0f}, MouseButton::Left);
    d->InjectMouseUp(core::Float2{12.0f, 14.0f}, MouseButton::Left);
    CHECK(opening == 2);
    CHECK(open->IsEnabled());

    Menu* recent = fx.fileMenu->AddSubMenu(core::StringView(u8"Recent"));
    int subOpening = 0;
    recent->SetOnOpening([&](Menu&) { ++subOpening; });
    recent->AddItem(core::StringView(u8"a.scene"), [] {});
    fx.fileMenu->Close();
    d->InjectMouseDown(core::Float2{12.0f, 14.0f}, MouseButton::Left);
    d->InjectMouseUp(core::Float2{12.0f, 14.0f}, MouseButton::Left);
    MenuSubItem* recentItem = nullptr;
    for (core::usize i = 0; i < fx.fileMenu->ChildCount(); ++i)
        if (auto* item = core::Cast<MenuSubItem>(fx.fileMenu->GetChildAt(i)))
            recentItem = item;
    REQUIRE(recentItem != nullptr);
    CHECK(recentItem->HandleActivate()); // as Return or a click on it does, this opens its submenu
    CHECK(recent->IsOpen());
    CHECK(subOpening == 1);
}

TEST_CASE("menubar: its menus cleared, an open one closed first; menus added after are its only ones")
{
    BarFixture fx;
    EventDispatcher* d = fx.d();
    d->InjectMouseDown(core::Float2{12.0f, 14.0f}, MouseButton::Left);
    d->InjectMouseUp(core::Float2{12.0f, 14.0f}, MouseButton::Left);
    REQUIRE(fx.fileMenu->IsOpen());
    core::RefPtr<Menu> held(fx.fileMenu); // the test reads it after the bar lets it go
    fx.bar->ClearMenus();
    CHECK_FALSE(held->IsOpen());
    CHECK(fx.bar->MenuCount() == 0u);
    CHECK(fx.bar->CurrentMenu() == nullptr);
    CHECK(fx.bar->ChildCount() == 0u);

    Menu* view = fx.bar->AddMenu(core::StringView(u8"View"));
    CHECK(fx.bar->MenuCount() == 1u);
    CHECK(fx.bar->MenuAt(0) == view);
}
