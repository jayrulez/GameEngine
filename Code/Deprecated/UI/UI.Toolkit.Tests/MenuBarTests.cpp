// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Smoke test for MenuBar: constructs, adds menus, tracks count, returns usable ContextMenus.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.ui;
import foundation.ui.toolkit;

using namespace foundation::ui;
using namespace foundation::ui::toolkit;
using namespace foundation::core;
namespace core = foundation::core;

TEST_CASE("toolkit-menubar: AddsMenus")
{
    auto bar = core::MakeRef<MenuBar>(core::DefaultAllocator());
    CHECK(bar->MenuCount() == 0u);

    ContextMenu* fileMenu = bar->AddMenu(u8"File");
    REQUIRE(fileMenu != nullptr);
    ContextMenu* editMenu = bar->AddMenu(u8"Edit");
    REQUIRE(editMenu != nullptr);

    CHECK(bar->MenuCount() == 2u);
    CHECK(fileMenu != editMenu);

    // The returned menus are live and accept items (no popup shown here).
    bool ran = false;
    fileMenu->AddItem(u8"Open", [&]() { ran = true; });
    fileMenu->AddSeparator();
    editMenu->AddItem(u8"Undo", []() {});
    CHECK(ran == false); // action not invoked merely by adding

    // MenuBar is its own popup owner.
    CHECK(bar->OwnerView() == bar.Get());
}

TEST_CASE("toolkit-menubar: MenuAt / MenuTitle answer the menus in order; ClearMenus empties the bar")
{
    auto bar = core::MakeRef<MenuBar>(core::DefaultAllocator());
    ContextMenu* fileMenu = bar->AddMenu(u8"File");
    ContextMenu* editMenu = bar->AddMenu(u8"Edit");
    CHECK(bar->MenuAt(0) == fileMenu);
    CHECK(bar->MenuAt(1) == editMenu);
    CHECK(bar->MenuAt(2) == nullptr);
    CHECK(bar->MenuTitle(0) == u8"File");
    CHECK(bar->MenuTitle(1) == u8"Edit");
    CHECK(bar->MenuTitle(2).IsEmpty());
    bar->ClearMenus();
    CHECK(bar->MenuCount() == 0u);
    CHECK(bar->MenuAt(0) == nullptr);
    // A bar rebuilt after clearing is a fresh bar.
    ContextMenu* again = bar->AddMenu(u8"File");
    REQUIRE(again != nullptr);
    CHECK(bar->MenuCount() == 1u);
    CHECK(bar->MenuTitle(0) == u8"File");
}

