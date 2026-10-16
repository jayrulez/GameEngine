// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Ported from Sedulous.UI.Tests/src/ScrollViewTests.bf (faithful; includes its MomentumHelper +
// ScrollBar cases). Beef get/set properties -> methods (scroll->SetScrollY / ScrollY()); the (dx,dy)
// tuple -> Float2. Logic only - no font service needed.
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
static core::RefPtr<ScrollView> MakeScroll()
{
    return core::MakeRef<ScrollView>(core::DefaultAllocator());
}
static core::RefPtr<TestView> MakeContent(f32 w, f32 h)
{
    return core::MakeRef<TestView>(core::DefaultAllocator(), w, h);
}

// === ScrollView ===

TEST_CASE("scroll-view: ContentLargerThanViewport")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 200, 100);
    auto scroll = MakeScroll();
    auto content = MakeContent(200, 500); // taller than viewport
    scroll->AddView(content.Get());
    root->AddView(scroll.Get());
    LayoutPass(ctx, root.Get());

    CHECK(scroll->MaxScrollY() > 0);
    CHECK(scroll->MaxScrollX() == 0);
}

TEST_CASE("scroll-view: ScrollClamps")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 200, 100);
    auto scroll = MakeScroll();
    auto content = MakeContent(200, 500);
    scroll->AddView(content.Get());
    root->AddView(scroll.Get());
    LayoutPass(ctx, root.Get());

    scroll->SetScrollY(-100);
    CHECK(scroll->ScrollY() == 0);

    scroll->SetScrollY(9999);
    CHECK(scroll->ScrollY() == scroll->MaxScrollY());
}

TEST_CASE("scroll-view: ScrollTo")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 200, 100);
    auto scroll = MakeScroll();
    auto content = MakeContent(200, 500);
    scroll->AddView(content.Get());
    root->AddView(scroll.Get());
    LayoutPass(ctx, root.Get());

    scroll->ScrollTo(0, 100);
    CHECK(scroll->ScrollY() == 100);
}

TEST_CASE("scroll-view: ScrollToTop")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 200, 100);
    auto scroll = MakeScroll();
    auto content = MakeContent(200, 500);
    scroll->AddView(content.Get());
    root->AddView(scroll.Get());
    LayoutPass(ctx, root.Get());

    scroll->SetScrollY(200);
    scroll->ScrollToTop();
    CHECK(scroll->ScrollY() == 0);
}

TEST_CASE("scroll-view: ScrollToBottom")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 200, 100);
    auto scroll = MakeScroll();
    auto content = MakeContent(200, 500);
    scroll->AddView(content.Get());
    root->AddView(scroll.Get());
    LayoutPass(ctx, root.Get());

    scroll->ScrollToBottom();
    CHECK(scroll->ScrollY() == scroll->MaxScrollY());
}

TEST_CASE("scroll-view: NeverPolicy_NoBar")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 200, 100);
    auto scroll = MakeScroll();
    scroll->VScrollBarPolicy.SetValue(ScrollBarPolicy::Never);
    auto content = MakeContent(200, 500);
    scroll->AddView(content.Get());
    root->AddView(scroll.Get());
    LayoutPass(ctx, root.Get());

    // ViewportHeight should be full height (no bar reserved).
    CHECK(Abs(scroll->ViewportHeight() - 100) < 1.0f);
}

// === MomentumHelper ===

TEST_CASE("scroll-view: Momentum_Decelerates")
{
    MomentumHelper m;
    m.VelocityY = 500;
    CHECK(m.IsActive());

    f32 totalDy = 0.0f;
    for (int i = 0; i < 100; i++)
    {
        totalDy += m.Update(0.016f).y;
    }

    CHECK(totalDy > 0);
    CHECK((!m.IsActive() || Abs(m.VelocityY) < 1.0f));
}

TEST_CASE("scroll-view: Momentum_Stop")
{
    MomentumHelper m;
    m.VelocityY = 500;
    m.Stop();
    CHECK(!m.IsActive());
}

// === ScrollBar ===

TEST_CASE("scroll-view: ScrollBar_ValueClamps")
{
    auto bar = core::MakeRef<ScrollBar>(core::DefaultAllocator());
    bar->SetMaxValue(100);
    bar->SetValue(-10);
    CHECK(bar->Value() == 0);
    bar->SetValue(200);
    CHECK(bar->Value() == 100);
}

TEST_CASE("scroll-view: ScrollBar_ValueChanged")
{
    auto bar = core::MakeRef<ScrollBar>(core::DefaultAllocator());
    bar->SetMaxValue(100);
    f32 lastVal = -1;
    bar->OnValueChanged.Add(
        Event<void(ScrollBar*, f32)>::Handler{[&lastVal](ScrollBar*, f32 v) { lastVal = v; }});
    bar->SetValue(42);
    CHECK(lastVal == 42);
}

TEST_CASE("scroll-view: a child focused by keys or code scrolls into view; one clicked does not move it")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 200, 100);
    auto scroll = MakeScroll();
    auto column = core::MakeRef<FlexLayout>(core::DefaultAllocator());
    column->Direction = Orientation::Vertical;
    core::RefPtr<TestView> rows[10];
    for (auto& row : rows)
    {
        row = MakeContent(200, 50);
        row->IsFocusable = true;
        column->AddView(row.Get());
    }
    scroll->AddView(column.Get());
    root->AddView(scroll.Get());
    LayoutPass(ctx, root.Get());
    REQUIRE(scroll->ScrollY() == doctest::Approx(0.0f));

    ctx.GetFocusManager()->SetFocus(rows[1].Get(), FocusSource::Pointer); // the pointer is on it
    CHECK(scroll->ScrollY() == doctest::Approx(0.0f));
    ctx.GetFocusManager()->SetFocus(rows[6].Get(), FocusSource::Keyboard); // y 300..350
    CHECK(scroll->ScrollY() == doctest::Approx(250.0f)); // the least that shows it
    LayoutPass(ctx, root.Get());
    ctx.GetFocusManager()->SetFocus(rows[0].Get(), FocusSource::Programmatic);
    CHECK(scroll->ScrollY() == doctest::Approx(0.0f));
}
