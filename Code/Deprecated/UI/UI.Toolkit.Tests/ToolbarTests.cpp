// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Smoke test for Toolbar: constructs, adds buttons/separators/toggles, toggle round-trips + fires event.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.ui;
import foundation.ui.toolkit;

using namespace foundation::ui;
using namespace foundation::ui::toolkit;
using namespace foundation::core;
namespace core = foundation::core;

TEST_CASE("toolkit-toolbar: AddsItemsAndToggles")
{
    auto bar = core::MakeRef<Toolbar>(core::DefaultAllocator());
    CHECK(bar->Direction == Orientation::Horizontal);

    ToolbarButton* btn = bar->AddButton(u8"File");
    REQUIRE(btn != nullptr);
    ToolbarSeparator* sep = bar->AddSeparator();
    REQUIRE(sep != nullptr);
    ToolbarToggle* toggle = bar->AddToggle(u8"Bold");
    REQUIRE(toggle != nullptr);

    // button + separator + toggle = 3 children
    CHECK(bar->ChildCount() == 3u);

    // Toggle round-trip fires OnCheckedChanged.
    bool fired = false;
    bool lastValue = false;
    toggle->OnCheckedChanged.Add(
        [&](ToolbarToggle*, bool v)
        {
            fired = true;
            lastValue = v;
        });
    CHECK(toggle->IsChecked() == false);
    toggle->SetIsChecked(true);
    CHECK(toggle->IsChecked() == true);
    CHECK(fired);
    CHECK(lastValue == true);

    // Setting to the same value does not re-fire.
    fired = false;
    toggle->SetIsChecked(true);
    CHECK(fired == false);

    // Button OnClick event wiring.
    bool clicked = false;
    btn->OnClick.Add([&](ToolbarButton*) { clicked = true; });
    btn->OnClick.Invoke(btn);
    CHECK(clicked);
}

TEST_CASE("toolkit-toolbar: a menu button is a button with a checked state and a caret strip")
{
    auto bar = core::MakeRef<Toolbar>(core::DefaultAllocator());
    ToolbarMenuButton* menu = bar->AddMenuButton(u8"Terrain");
    REQUIRE(menu != nullptr);
    CHECK(bar->ChildCount() == 1u);
    CHECK(Cast<ToolbarButton>(menu) != nullptr); // it IS a button: OnClick opens the popup

    bool clicked = false;
    menu->OnClick.Add([&](ToolbarButton*) { clicked = true; });
    menu->OnClick.Invoke(menu);
    CHECK(clicked);

    // Checked state round-trips like a toggle's (the dropdown's mode is active).
    CHECK_FALSE(menu->IsChecked());
    menu->SetIsChecked(true);
    CHECK(menu->IsChecked());
    menu->SetIsChecked(false);
    CHECK_FALSE(menu->IsChecked());

    // The caret strip widens the button past a plain button with the same text.
    ToolbarButton* plain = bar->AddButton(u8"Terrain");
    const BoxConstraints loose = BoxConstraints::Loose(1000.0f, 100.0f);
    menu->Measure(loose);
    plain->Measure(loose);
    CHECK(menu->MeasuredSize.x == doctest::Approx(plain->MeasuredSize.x + ToolbarMenuButton::kCaretWidth));
}


TEST_CASE("toolkit-toolbar: setting a button's text to what it is asks for no layout")
{
    // A page syncs its toolbar every frame; an unconditional invalidate re-laid-out the editor
    // each frame. A change still asks.
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRef<RootView>(DefaultAllocator());
    root->ViewportSize = Float2{400, 100};
    ctx.AddRootView(root.Get());
    auto bar = MakeRef<Toolbar>(DefaultAllocator());
    root->AddView(bar.Get());
    ToolbarButton* button = bar->AddButton(u8"Pin");
    ctx.UpdateRootView(root.Get());
    ctx.ClearLayoutDamage();

    button->SetText(u8"Pin");
    CHECK(!ctx.NeedsLayout());
    button->SetText(u8"Unpin");
    CHECK(ctx.NeedsLayout());
    CHECK(button->Text() == StringView(u8"Unpin"));
}
