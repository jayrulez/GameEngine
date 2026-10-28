// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Ported from Sedulous.UI.Tests/src/TabViewTests.bf (faithful). Beef SelectedIndex/TabCount get/set ->
// methods; `new TestView()` content -> a MakeRef<TestView> whose ref AddView adopts (kept as a local when
// the test inspects its Visibility). KeyEventArgs.Set + OnKeyDown drive the keyboard case. No font needed.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.ui;
#include "TestHelpers.h"

using namespace foundation::ui;
using namespace foundation::ui::tests;
using namespace foundation::core;
namespace core = foundation::core;

static core::RefPtr<RootView> MakeRoot()
{
    return core::MakeRef<RootView>(core::DefaultAllocator());
}
static core::RefPtr<TabView> MakeTabs() { return core::MakeRef<TabView>(core::DefaultAllocator()); }
static core::RefPtr<TestView> MakeView(f32 w = 50.0f, f32 h = 30.0f)
{
    return core::MakeRef<TestView>(core::DefaultAllocator(), w, h);
}

TEST_CASE("tab-view: AddTab_SelectsFirst")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto tabs = MakeTabs();
    tabs->AddTab(u8"Tab 1", MakeView(100, 100).Get());
    root->AddView(tabs.Get());
    LayoutPass(ctx, root.Get());

    CHECK(tabs->SelectedIndex() == 0);
    CHECK(tabs->TabCount() == 1u);
}

TEST_CASE("tab-view: SwitchTab")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto tabs = MakeTabs();
    auto content1 = MakeView(100, 100);
    auto content2 = MakeView(100, 100);
    tabs->AddTab(u8"Tab 1", content1.Get());
    tabs->AddTab(u8"Tab 2", content2.Get());
    root->AddView(tabs.Get());

    CHECK(tabs->SelectedIndex() == 0);
    CHECK(content1->Visibility == Visibility::Visible);
    CHECK(content2->Visibility == Visibility::Gone);

    tabs->SetSelectedIndex(1);
    CHECK(content1->Visibility == Visibility::Gone);
    CHECK(content2->Visibility == Visibility::Visible);
}

TEST_CASE("tab-view: TabChangedEvent")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto tabs = MakeTabs();
    tabs->AddTab(u8"A", MakeView().Get());
    tabs->AddTab(u8"B", MakeView().Get());
    root->AddView(tabs.Get());

    i32 lastIdx = -1;
    tabs->OnTabChanged.Add(
        Event<void(TabView*, i32)>::Handler{[&lastIdx](TabView*, i32 idx) { lastIdx = idx; }});

    tabs->SetSelectedIndex(1);
    CHECK(lastIdx == 1);
}

TEST_CASE("tab-view: RemoveTab")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto tabs = MakeTabs();
    tabs->AddTab(u8"A", MakeView().Get());
    tabs->AddTab(u8"B", MakeView().Get());
    tabs->AddTab(u8"C", MakeView().Get());
    root->AddView(tabs.Get());

    tabs->SetSelectedIndex(2);
    tabs->RemoveTab(2);
    CHECK(tabs->TabCount() == 2u);
    CHECK(tabs->SelectedIndex() == 1); // clamps to last
}

TEST_CASE("tab-view: RemoveTab_AdjustsSelection")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto tabs = MakeTabs();
    tabs->AddTab(u8"A", MakeView().Get());
    tabs->AddTab(u8"B", MakeView().Get());
    root->AddView(tabs.Get());

    tabs->SetSelectedIndex(1);
    tabs->RemoveTab(1);
    CHECK(tabs->TabCount() == 1u);
    CHECK(tabs->SelectedIndex() == 0);
}

TEST_CASE("tab-view: KeyboardNavigation")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto tabs = MakeTabs();
    tabs->AddTab(u8"A", MakeView().Get());
    tabs->AddTab(u8"B", MakeView().Get());
    tabs->AddTab(u8"C", MakeView().Get());
    root->AddView(tabs.Get());

    CHECK(tabs->SelectedIndex() == 0);

    KeyEventArgs right;
    right.Set(KeyCode::Right, KeyModifiers::None, false);
    tabs->OnKeyDown(right);
    CHECK(tabs->SelectedIndex() == 1);

    KeyEventArgs left;
    left.Set(KeyCode::Left, KeyModifiers::None, false);
    tabs->OnKeyDown(left);
    CHECK(tabs->SelectedIndex() == 0);

    tabs->OnKeyDown(left); // can't go below 0
    CHECK(tabs->SelectedIndex() == 0);
}

// Our addition (not in Sedulous.UI.Tests): the TabView clears its hovered-tab highlight when the
// cursor leaves the view. Sedulous' TabView has no OnMouseLeave, so a hovered tab stays stuck in the
// Hover state after the mouse moves away; this verifies our OnMouseLeave fix.
TEST_CASE("tab-view: HoverClearsOnMouseLeave")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto tabs = MakeTabs();
    tabs->AddTab(u8"Tab 1", MakeView(100, 100).Get());
    tabs->AddTab(u8"Tab 2", MakeView(100, 100).Get());
    root->AddView(tabs.Get());
    LayoutPass(ctx, root.Get());

    // Hover the first tab in the top strip.
    ctx.GetInputManager()->ProcessMouseMove(10, 10);
    CHECK(tabs->HoveredTabIndex() == 0);

    // Move into the content area (a child view) so the TabView is no longer the hovered view.
    ctx.GetInputManager()->ProcessMouseMove(200, 200);
    CHECK(tabs->HoveredTabIndex() == -1);
}

// Our addition (not in Sedulous.UI.Tests): overflow scrolling of the tab strip, ported from the
// dock tab strip. With no font service each tab uses the 80px fallback width, so six tabs (480px) overflow
// a 400px view. The scroll shifts m_tabRects (reused for hit-testing), which we observe via HoveredTabIndex:
// once the strip scrolls right, the leftmost tab moves off-screen and a later tab sits under x=10.

TEST_CASE("tab-view: SelectingHiddenTabScrollsItIntoView")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto tabs = MakeTabs();
    for (i32 i = 0; i < 6; ++i)
    {
        tabs->AddTab(u8"Tab", MakeView(100, 100).Get());
    }
    root->AddView(tabs.Get());
    LayoutPass(ctx, root.Get());

    // At rest the first tab occupies x[0,80): hovering x=10 selects it, the last tab is off the right edge.
    ctx.GetInputManager()->ProcessMouseMove(10, 10);
    CHECK(tabs->HoveredTabIndex() == 0);

    // Select the last (hidden) tab -> the strip scrolls it into view; tab 0 slides off the left edge, so
    // x=10 now lands on tab 1 and the last tab (index 5) becomes hittable near the right edge.
    tabs->SetSelectedIndex(5);
    LayoutPass(ctx, root.Get());
    ctx.GetInputManager()->ProcessMouseMove(10, 10);
    CHECK(tabs->HoveredTabIndex() == 1);
    ctx.GetInputManager()->ProcessMouseMove(390, 10);
    CHECK(tabs->HoveredTabIndex() == 5);
}

TEST_CASE("tab-view: WheelScrollsOverflowingStrip")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto tabs = MakeTabs();
    for (i32 i = 0; i < 6; ++i)
    {
        tabs->AddTab(u8"Tab", MakeView(100, 100).Get());
    }
    root->AddView(tabs.Get());
    LayoutPass(ctx, root.Get());

    ctx.GetInputManager()->ProcessMouseMove(10, 10);
    CHECK(tabs->HoveredTabIndex() == 0);

    // Wheel down over the strip band scrolls the tabs right (delta * 40, clamped to the 80px overflow).
    MouseWheelEventArgs wheel;
    wheel.X = 10;
    wheel.Y = 10;
    wheel.DeltaY = -3;
    tabs->OnMouseWheel(wheel);
    CHECK(wheel.Handled);
    LayoutPass(ctx, root.Get());

    ctx.GetInputManager()->ProcessMouseMove(10, 10);
    CHECK(tabs->HoveredTabIndex() == 1); // tab 0 scrolled off the left edge
}

TEST_CASE("tab-view: WheelIgnoredWithoutOverflow")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto tabs = MakeTabs();
    for (i32 i = 0; i < 3; ++i)
    {
        tabs->AddTab(u8"Tab", MakeView(100, 100).Get());
    }
    root->AddView(tabs.Get());
    LayoutPass(ctx, root.Get());

    // Three 80px tabs (240px) fit inside 400px: the third sits at x[160,240).
    ctx.GetInputManager()->ProcessMouseMove(170, 10);
    CHECK(tabs->HoveredTabIndex() == 2);

    // Wheel is a no-op when the strip doesn't overflow: not handled, and the hit-rects don't move.
    MouseWheelEventArgs wheel;
    wheel.X = 170;
    wheel.Y = 10;
    wheel.DeltaY = -3;
    tabs->OnMouseWheel(wheel);
    CHECK_FALSE(wheel.Handled);
    LayoutPass(ctx, root.Get());

    ctx.GetInputManager()->ProcessMouseMove(170, 10);
    CHECK(tabs->HoveredTabIndex() == 2);
}
