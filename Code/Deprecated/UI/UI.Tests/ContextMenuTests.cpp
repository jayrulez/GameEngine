// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Ported from Sedulous.UI.Tests/src/ContextMenuTests.bf (faithful). Beef `delegate void()` -> Function<
// void()>; Beef nullable String -> empty String check; MenuItem.CreateSeparator returns UniquePtr; menu
// item/submenu structure only (no popup, no font).
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.ui;

using namespace foundation::ui;
using namespace foundation::core;
namespace core = foundation::core;

static core::RefPtr<ContextMenu> MakeMenu()
{
    return core::MakeRef<ContextMenu>(core::DefaultAllocator());
}

TEST_CASE("context-menu: MenuItem_Properties")
{
    MenuItem item(u8"Test", []() {}, true);
    CHECK(!item.Label.IsEmpty());
    CHECK(item.Label == u8"Test");
    CHECK(item.Enabled == true);
    CHECK(!item.IsSeparator);
    CHECK(!item.Submenu);
}

TEST_CASE("context-menu: MenuItem_CreateSeparator")
{
    auto item = MenuItem::CreateSeparator(DefaultAllocator());
    CHECK(item->IsSeparator);
    CHECK(item->Label.IsEmpty());
    CHECK(!item->Action);
}

TEST_CASE("context-menu: AddItem_IncreasesCount")
{
    auto menu = MakeMenu();
    CHECK(menu->ItemCount() == 0);

    menu->AddItem(u8"Item 1", []() {});
    CHECK(menu->ItemCount() == 1);

    menu->AddItem(u8"Item 2", []() {});
    CHECK(menu->ItemCount() == 2);
}

TEST_CASE("context-menu: AddSeparator_IncreasesCount")
{
    auto menu = MakeMenu();
    menu->AddItem(u8"Item 1", []() {});
    menu->AddSeparator();
    menu->AddItem(u8"Item 2", []() {});
    CHECK(menu->ItemCount() == 3);
}

TEST_CASE("context-menu: AddSubmenu_CreatesItemWithSubmenu")
{
    auto menu = MakeMenu();
    MenuItem* sub = menu->AddSubmenu(u8"More");
    CHECK(sub != nullptr);
    CHECK(sub->Submenu);
    CHECK(!sub->Label.IsEmpty());
    CHECK(sub->Label == u8"More");
    CHECK(menu->ItemCount() == 1);
}

TEST_CASE("context-menu: IsFocusable")
{
    auto menu = MakeMenu();
    CHECK(menu->IsFocusable);
}

TEST_CASE("context-menu: Show resets the stale hover highlight")
{
    // Menus are retained views (a menu bar reuses its instances): hover from the previous
    // open must NOT survive into the next Show - the regression was "Close Project" still
    // highlighted when reopening the File menu after a project close/reopen.
    auto menu = MakeMenu();
    menu->AddItem(u8"Open", {});
    menu->AddItem(u8"Close Project", {});

    MouseEventArgs move;
    move.X = 10.0f;
    move.Y = 10.0f; // inside the first item's band
    menu->OnMouseMove(move);
    REQUIRE(menu->HoveredIndex() >= 0);

    UIContext ctx{DefaultAllocator()}; // no active root: Show early-outs, but AFTER clearing the hover
    menu->Show(&ctx, 0.0f, 0.0f);
    CHECK(menu->HoveredIndex() == -1);
}

TEST_CASE("context-menu: taller-than-constraint menus clamp and wheel-scroll")
{
    auto menu = MakeMenu();
    for (core::i32 i = 0; i < 30; ++i)
    {
        menu->AddItem(u8"Item", {});
    }
    // 30 items x 28 + 8 padding = 848 of content against a 300 ceiling.
    menu->Measure(BoxConstraints::Loose(400, 300));
    CHECK(menu->MeasuredSize.y == 300);
    menu->Layout(0, 0, menu->MeasuredSize.x, menu->MeasuredSize.y);

    MouseEventArgs top;
    top.X = 10.0f;
    top.Y = 10.0f;
    menu->OnMouseMove(top);
    CHECK(menu->HoveredIndex() == 0);

    // One wheel notch scrolls the items up under the cursor.
    MouseWheelEventArgs wheel;
    wheel.Y = 10.0f;
    wheel.DeltaY = -1.0f;
    menu->OnMouseWheel(wheel);
    CHECK(wheel.Handled);
    menu->OnMouseMove(top);
    CHECK(menu->HoveredIndex() > 0);

    // Scrolling far past the end clamps - the LAST item is reachable at the bottom band
    // (the regression: overflow items simply could not be clicked).
    for (core::i32 i = 0; i < 100; ++i)
    {
        MouseWheelEventArgs w;
        w.Y = 10.0f;
        w.DeltaY = -1.0f;
        menu->OnMouseWheel(w);
    }
    MouseEventArgs bottom;
    bottom.X = 10.0f;
    bottom.Y = 290.0f;
    menu->OnMouseMove(bottom);
    CHECK(menu->HoveredIndex() == 29);
}

TEST_CASE("context-menu: menus that fit ignore the wheel")
{
    auto menu = MakeMenu();
    menu->AddItem(u8"A", {});
    menu->AddItem(u8"B", {});
    menu->Measure(BoxConstraints::Loose(400, 300));
    menu->Layout(0, 0, menu->MeasuredSize.x, menu->MeasuredSize.y);

    MouseWheelEventArgs wheel;
    wheel.Y = 10.0f;
    wheel.DeltaY = -1.0f;
    menu->OnMouseWheel(wheel);
    CHECK(!wheel.Handled); // bubbles on: nothing to scroll here

    MouseEventArgs top;
    top.X = 10.0f;
    top.Y = 10.0f;
    menu->OnMouseMove(top);
    CHECK(menu->HoveredIndex() == 0); // items did not move
}

TEST_CASE("context-menu: keyboard navigation scrolls the hovered item into view")
{
    auto menu = MakeMenu();
    for (core::i32 i = 0; i < 30; ++i)
    {
        menu->AddItem(u8"Item", {});
    }
    menu->Measure(BoxConstraints::Loose(400, 300));
    menu->Layout(0, 0, menu->MeasuredSize.x, menu->MeasuredSize.y);

    // Walk down past the visible band; each step must keep the hovered row inside the
    // frame, so by the last item the menu has scrolled to its end.
    for (core::i32 i = 0; i < 30; ++i)
    {
        KeyEventArgs down;
        down.Key = KeyCode::Down;
        menu->OnKeyDown(down);
    }
    CHECK(menu->HoveredIndex() == 29);
    MouseEventArgs bottom;
    bottom.X = 10.0f;
    bottom.Y = 290.0f;
    menu->OnMouseMove(bottom);
    CHECK(menu->HoveredIndex() == 29); // the row under the cursor IS the scrolled-to row
}

TEST_CASE("context-menu: ItemAt, ClearItems, and OnOpening rebuilding the items at Show")
{
    auto menu = MakeMenu();
    menu->AddItem(u8"One", []() {});
    menu->AddSeparator();
    menu->AddItem(u8"Two", []() {}, false);
    REQUIRE(menu->ItemCount() == 3);
    CHECK(menu->ItemAt(0)->Label == u8"One");
    CHECK(menu->ItemAt(1)->IsSeparator);
    CHECK_FALSE(menu->ItemAt(2)->Enabled);
    CHECK(menu->ItemAt(3) == nullptr);
    CHECK(menu->ItemAt(-1) == nullptr);
    menu->ClearItems();
    CHECK(menu->ItemCount() == 0);
    // The opening hook is the menu's to call at Show; a menu built from live state fills
    // itself there. Called directly here (no popup layer in a unit test).
    int opened = 0;
    menu->OnOpening = [&opened](ContextMenu& opening)
    {
        ++opened;
        opening.ClearItems();
        opening.AddItem(u8"Fresh", []() {});
    };
    menu->OnOpening(*menu);
    CHECK(opened == 1);
    REQUIRE(menu->ItemCount() == 1);
    CHECK(menu->ItemAt(0)->Label == u8"Fresh");
}


TEST_CASE("context-menu: ClearItems closes an open submenu with the item that owned it")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    root->ViewportSize = Float2{800.0f, 600.0f};
    ctx.AddRootView(root.Get());
    auto menu = MakeMenu();
    menu->AddItem(u8"One", []() {});
    MenuItem* more = menu->AddSubmenu(u8"More");
    Cast<ContextMenu>(more->Submenu.Get())->AddItem(u8"Deep", []() {});
    menu->Show(&ctx, 10.0f, 10.0f);
    REQUIRE(root->GetPopupLayer()->PopupCount() == 1u);

    // Down twice lands on the submenu item; Right opens it as a second popup.
    KeyEventArgs down;
    down.Key = KeyCode::Down;
    menu->OnKeyDown(down);
    menu->OnKeyDown(down);
    REQUIRE(menu->HoveredIndex() == 1);
    KeyEventArgs right;
    right.Key = KeyCode::Right;
    menu->OnKeyDown(right);
    REQUIRE(root->GetPopupLayer()->PopupCount() == 2u);

    // Rebuilding the items (what a live menu does on every opening) takes the submenu's popup
    // down with the item; the menu itself stays open.
    menu->ClearItems();
    CHECK(root->GetPopupLayer()->PopupCount() == 1u);
    CHECK(menu->ItemCount() == 0);
    root->GetPopupLayer()->ClosePopup(menu.Get()); // now, not queued: the root dies with the test
    CHECK(root->GetPopupLayer()->PopupCount() == 0u);
}
