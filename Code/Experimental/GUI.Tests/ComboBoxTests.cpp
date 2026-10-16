// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - ComboBox tests: item management, opening/closing the dropdown popup, picking
// an item through the dropdown, and dismissal (outside click / Escape) via the dispatcher's
// popup support.
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

    core::RefPtr<ComboBox> MakeCombo(SceneRoot* root)
    {
        auto cb = core::MakeRef<ComboBox>(core::DefaultAllocator());
        cb->SetSize(core::Float2{120.0f, 26.0f});
        cb->SetItemHeight(24.0f);
        cb->AddItem(core::StringView(u8"Red"));
        cb->AddItem(core::StringView(u8"Green"));
        cb->AddItem(core::StringView(u8"Blue"));
        root->AddChild(cb.Get());
        return cb;
    }
}

TEST_CASE("combobox: item management and selection")
{
    auto cb = Make<ComboBox>();
    cb->AddItem(core::StringView(u8"One"));
    cb->AddItem(core::StringView(u8"Two"));
    CHECK(cb->ItemCount() == 2);
    CHECK(cb->GetSelectedIndex() == -1);

    int changes = 0;
    cb->SetOnSelectionChanged([&](int) { ++changes; });
    cb->SetSelectedIndex(1);
    CHECK(cb->GetSelectedIndex() == 1);
    CHECK(cb->GetSelectedText() == core::StringView(u8"Two"));
    CHECK(changes == 1);
    cb->SetSelectedIndex(1); // no change
    CHECK(changes == 1);
}

TEST_CASE("combobox: click opens the dropdown; it registers as the dispatcher popup")
{
    auto root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
    root->SetSize(core::Float2{400.0f, 400.0f});
    auto cb = MakeCombo(root.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    CHECK_FALSE(cb->IsOpen());
    CHECK(d->GetPopup() == nullptr);

    // Click the combo (a press+release on it).
    d->InjectMouseDown(core::Float2{20.0f, 13.0f}, MouseButton::Left);
    d->InjectMouseUp(core::Float2{20.0f, 13.0f}, MouseButton::Left);
    CHECK(cb->IsOpen());
    CHECK(d->GetPopup() != nullptr); // the dropdown is the active popup
}

TEST_CASE("combobox: clicking a dropdown row selects it and closes")
{
    auto root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
    root->SetSize(core::Float2{400.0f, 400.0f});
    auto cb = MakeCombo(root.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    d->InjectMouseDown(core::Float2{20.0f, 13.0f}, MouseButton::Left);
    d->InjectMouseUp(core::Float2{20.0f, 13.0f}, MouseButton::Left); // open
    REQUIRE(cb->IsOpen());

    // The dropdown sits just below the combo (combo is 26 tall at y=0). Row 1 (Green) spans
    // y in [26 + 24, 26 + 48) = [50, 74).
    d->InjectMouseDown(core::Float2{20.0f, 60.0f}, MouseButton::Left);
    d->InjectMouseUp(core::Float2{20.0f, 60.0f}, MouseButton::Left);
    CHECK(cb->GetSelectedIndex() == 1);
    CHECK(cb->GetSelectedText() == core::StringView(u8"Green"));
    CHECK_FALSE(cb->IsOpen());
    CHECK(root->GetEventDispatcher()->GetPopup() == nullptr);
}

TEST_CASE("combobox: clicking outside dismisses the dropdown")
{
    auto root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
    root->SetSize(core::Float2{400.0f, 400.0f});
    auto cb = MakeCombo(root.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    d->InjectMouseDown(core::Float2{20.0f, 13.0f}, MouseButton::Left);
    d->InjectMouseUp(core::Float2{20.0f, 13.0f}, MouseButton::Left); // open
    REQUIRE(cb->IsOpen());

    // Press far away from the combo and its dropdown -> dismissed.
    d->InjectMouseDown(core::Float2{350.0f, 350.0f}, MouseButton::Left);
    CHECK_FALSE(cb->IsOpen());
    CHECK(d->GetPopup() == nullptr);
}

TEST_CASE("combobox: Escape dismisses the dropdown")
{
    auto root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
    root->SetSize(core::Float2{400.0f, 400.0f});
    auto cb = MakeCombo(root.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    d->InjectMouseDown(core::Float2{20.0f, 13.0f}, MouseButton::Left);
    d->InjectMouseUp(core::Float2{20.0f, 13.0f}, MouseButton::Left); // open
    REQUIRE(cb->IsOpen());

    d->InjectKeyDown(static_cast<core::u32>(KeyCode::Escape));
    CHECK_FALSE(cb->IsOpen());
    CHECK(d->GetPopup() == nullptr);
}
