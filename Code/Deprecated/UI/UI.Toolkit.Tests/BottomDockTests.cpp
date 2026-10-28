// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// BottomDock tests: the Godot-style collapsible bottom strip. Headless - exercises the tab/expand
// state machine + OnExpandedChanged event + per-tab content visibility (the host wires the event to
// SplitView::SetPaneCollapsed; that half is covered by SplitViewTests).
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.ui;
import foundation.ui.toolkit;

using namespace foundation::ui;
using namespace foundation::ui::toolkit;
using namespace foundation::core;
namespace core = foundation::core;

TEST_CASE("toolkit-bottomdock: starts collapsed; clicking a tab expands, clicking it again collapses")
{
    auto dock = core::MakeRef<BottomDock>(core::DefaultAllocator());
    auto anim = core::MakeRef<Panel>(core::DefaultAllocator());
    dock->AddTab(u8"animation", u8"Animation", anim.Get());
    CHECK(dock->TabCount() == 1u);

    // Default state: collapsed, content hidden.
    CHECK_FALSE(dock->IsExpanded());
    CHECK(anim->Visibility == Visibility::Gone);

    int events = 0;
    bool lastExpanded = false;
    dock->OnExpandedChanged.Add(
        [&](bool e)
        {
            ++events;
            lastExpanded = e;
        });

    // Click the tab -> expands, content shown, event(true), active id set.
    dock->ClickTab(u8"animation");
    CHECK(dock->IsExpanded());
    CHECK(anim->Visibility == Visibility::Visible);
    CHECK(dock->ActiveTabId() == StringView(u8"animation"));
    CHECK(events == 1);
    CHECK(lastExpanded == true);

    // Click the SAME (active) tab -> collapses back to just the bar, content hidden, event(false).
    dock->ClickTab(u8"animation");
    CHECK_FALSE(dock->IsExpanded());
    CHECK(anim->Visibility == Visibility::Gone);
    CHECK(events == 2);
    CHECK(lastExpanded == false);
}

TEST_CASE("toolkit-bottomdock: switching tabs keeps expanded and shows only the active content")
{
    auto dock = core::MakeRef<BottomDock>(core::DefaultAllocator());
    auto a = core::MakeRef<Panel>(core::DefaultAllocator());
    auto b = core::MakeRef<Panel>(core::DefaultAllocator());
    dock->AddTab(u8"a", u8"A", a.Get());
    dock->AddTab(u8"b", u8"B", b.Get());

    dock->ClickTab(u8"a");
    CHECK(dock->IsExpanded());
    CHECK(a->Visibility == Visibility::Visible);
    CHECK(b->Visibility == Visibility::Gone);

    // Clicking the OTHER tab switches (still expanded), only B visible now.
    dock->ClickTab(u8"b");
    CHECK(dock->IsExpanded());
    CHECK(dock->ActiveTabId() == StringView(u8"b"));
    CHECK(a->Visibility == Visibility::Gone);
    CHECK(b->Visibility == Visibility::Visible);

    // Collapsing hides all tab content.
    dock->SetExpanded(false);
    CHECK_FALSE(dock->IsExpanded());
    CHECK(a->Visibility == Visibility::Gone);
    CHECK(b->Visibility == Visibility::Gone);
}

TEST_CASE("toolkit-bottomdock: ActivateTab expands the named tab")
{
    auto dock = core::MakeRef<BottomDock>(core::DefaultAllocator());
    auto a = core::MakeRef<Panel>(core::DefaultAllocator());
    dock->AddTab(u8"a", u8"A", a.Get());

    dock->ActivateTab(u8"nope"); // unknown id -> no change
    CHECK_FALSE(dock->IsExpanded());

    dock->ActivateTab(u8"a");
    CHECK(dock->IsExpanded());
    CHECK(dock->ActiveTabId() == StringView(u8"a"));
    CHECK(a->Visibility == Visibility::Visible);
}

// Sedulous 5e7d4e94: opened from elsewhere, the dock is GONE while nothing is open, bar and all,
// so the pane holding it takes no height; a tab added for a docked tool panel comes and goes
// with it, and the remaining tabs' buttons still find their tabs by id.
TEST_CASE("toolkit-bottomdock: hidden when collapsed, and tabs come and go")
{
    auto dock = core::MakeRef<BottomDock>(core::DefaultAllocator());
    dock->SetHideWhenCollapsed(true);
    auto animation = core::MakeRef<Panel>(core::DefaultAllocator());
    dock->AddTab(u8"animation", u8"Animation", animation.Get());

    // Collapsed: nothing to measure.
    dock->Measure(BoxConstraints{0, 400, 0, 300});
    CHECK(dock->MeasuredSize.y == 0.0f); // no bar while nothing is open

    // A docked tool's tab opens it, and removing the open tab closes it again.
    auto terrain = core::MakeRef<Panel>(core::DefaultAllocator());
    dock->AddTab(u8"tool:Terrain", u8"Terrain", terrain.Get());
    dock->ActivateTab(u8"tool:Terrain");
    CHECK(dock->IsExpanded());
    CHECK(terrain->Visibility == Visibility::Visible);
    dock->Measure(BoxConstraints{0, 400, 0, 300});
    CHECK(dock->MeasuredSize.y > 0.0f);
    dock->RemoveTab(u8"tool:Terrain");
    CHECK_FALSE(dock->IsExpanded());
    CHECK_FALSE(dock->HasTab(u8"tool:Terrain"));
    CHECK(dock->TabCount() == 1u);
    dock->Measure(BoxConstraints{0, 400, 0, 300});
    CHECK(dock->MeasuredSize.y == 0.0f);

    // The remaining tab's button still toggles it, by id after the removal.
    dock->AddTab(u8"tool:Terrain", u8"Terrain", terrain.Get());
    dock->RemoveTab(u8"animation");
    dock->ClickTab(u8"tool:Terrain");
    CHECK(dock->IsExpanded());
    CHECK(dock->ActiveTabId() == u8"tool:Terrain");
    dock->ClickTab(u8"tool:Terrain");
    CHECK_FALSE(dock->IsExpanded());
}
