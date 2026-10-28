// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// BoxMetrics / border-box coverage: the three padding
// channels max-merge in View::ResolveBoxMetrics, borders are layout-participating chrome
// (RoundedRectDrawable reports its border via DrawablePadding), and Panel's content box honors
// all of it - including stylesheet `padding:` on a CONTAINER, which must not silently no-op.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.ui;
#include "TestHelpers.h"

using namespace foundation::ui;
using namespace foundation::ui::tests;
using namespace foundation::core;
namespace core = foundation::core;

namespace
{
    core::RefPtr<RootView> MakeRoot() { return core::MakeRef<RootView>(core::DefaultAllocator()); }
}

TEST_CASE("box-metrics: RoundedRectDrawable reports its border as DrawablePadding")
{
    RoundedRectDrawable d{Color{1, 1, 1, 1}, 4.0f, Color{0, 0, 0, 1}, 2.0f};
    const Thickness p = d.DrawablePadding();
    CHECK(p.Left == 2.0f);
    CHECK(p.Top == 2.0f);
    CHECK(p.Right == 2.0f);
    CHECK(p.Bottom == 2.0f);
}

TEST_CASE("box-metrics: the three padding channels max-merge, border resolves separately")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto group = core::MakeRef<ViewGroup>(core::DefaultAllocator());
    root->AddView(group.Get());

    group->Padding = Thickness{10, 1, 1, 1};                       // field channel: wide LEFT
    group->SetStyle(StyleProperty::Padding, Thickness{2, 8, 2, 2}); // style channel: wide TOP
    auto bg = core::MakeRef<RoundedRectDrawable>(core::DefaultAllocator(), Color{1, 1, 1, 1},
                                                 0.0f, Color{0, 0, 0, 1}, 3.0f); // drawable channel: 3 everywhere
    group->SetStyle(StyleProperty::Background, RefPtr<Drawable>(bg.Get()));
    group->SetStyle(StyleProperty::BorderWidth, 5.0f);

    const BoxMetrics m = group->ResolveBoxMetrics();
    CHECK(m.Padding.Left == 10.0f);  // field wins left
    CHECK(m.Padding.Top == 8.0f);    // style wins top
    CHECK(m.Padding.Right == 3.0f);  // drawable wins right
    CHECK(m.Padding.Bottom == 3.0f); // drawable wins bottom
    CHECK(m.Border.Left == 5.0f);
    const Thickness chrome = m.Chrome();
    CHECK(chrome.Left == 15.0f); // padding + border
    CHECK(chrome.Top == 13.0f);
}

TEST_CASE("box-metrics: stylesheet padding on a CONTAINER takes effect (Panel content box)")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto panel = core::MakeRef<Panel>(core::DefaultAllocator());
    root->AddView(panel.Get());
    panel->SetStyle(StyleProperty::Padding, Thickness{7, 7, 7, 7});

    auto child = core::MakeRef<TestView>(core::DefaultAllocator(), 50.0f, 20.0f);
    panel->AddView(child.Get());
    // Measure with LOOSE constraints (the root would stretch it tight and mask the chrome).
    panel->Measure(BoxConstraints::Loose(400, 300));

    // Panel measures content + chrome; the style-declared padding must be part of it now.
    CHECK(panel->MeasuredSize.x == 50.0f + 14.0f);
    CHECK(panel->MeasuredSize.y == 20.0f + 14.0f);
}

TEST_CASE("box-metrics: a bordered background reserves content space in a Panel (border-box)")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto plain = core::MakeRef<Panel>(core::DefaultAllocator());
    auto bordered = core::MakeRef<Panel>(core::DefaultAllocator());
    root->AddView(plain.Get());
    root->AddView(bordered.Get());
    auto bg = core::MakeRef<RoundedRectDrawable>(core::DefaultAllocator(), Color{1, 1, 1, 1}, 0.0f,
                                                    Color{0, 0, 0, 1}, 2.0f);
    bordered->SetStyle(StyleProperty::Background, RefPtr<Drawable>(bg.Get()));

    auto childA = core::MakeRef<TestView>(core::DefaultAllocator(), 50.0f, 20.0f);
    plain->AddView(childA.Get());
    auto childB = core::MakeRef<TestView>(core::DefaultAllocator(), 50.0f, 20.0f);
    bordered->AddView(childB.Get());
    // Loose constraints - tight root constraints would stretch both and mask the chrome.
    plain->Measure(BoxConstraints::Loose(400, 300));
    bordered->Measure(BoxConstraints::Loose(400, 300));

    // The bordered panel is larger by its border chrome on each axis - content no longer sits
    // on the border line.
    CHECK(bordered->MeasuredSize.x == plain->MeasuredSize.x + 4.0f);
    CHECK(bordered->MeasuredSize.y == plain->MeasuredSize.y + 4.0f);
}

// === Acceptance: Fixed + margin work in EVERY container ===

TEST_CASE("box-model: Fixed(100) child is 100 in Dock, Flow, Grid, and Panel (was Frame-only)")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);

    auto makeFixedChild = []
    {
        auto v = core::MakeRef<TestView>(core::DefaultAllocator(), 10.0f, 10.0f);
        LayoutStyle lp;
        lp.Width = SizeSpec::Fixed(Unit::Dp(100.0f));
        lp.Height = SizeSpec::Fixed(Unit::Dp(40.0f));
        v->SetLayout(lp);
        return v;
    };

    auto dock = core::MakeRef<DockLayout>(core::DefaultAllocator());
    auto flow = core::MakeRef<FlowLayout>(core::DefaultAllocator());
    auto grid = core::MakeRef<GridLayout>(core::DefaultAllocator());
    auto panel = core::MakeRef<Panel>(core::DefaultAllocator());
    root->AddView(dock.Get());
    root->AddView(flow.Get());
    root->AddView(grid.Get());
    root->AddView(panel.Get());

    auto a = makeFixedChild();
    auto b = makeFixedChild();
    auto c = makeFixedChild();
    auto d = makeFixedChild();
    dock->AddView(a.Get());
    flow->AddView(b.Get());
    grid->AddView(c.Get());
    panel->AddView(d.Get());
    LayoutPass(ctx, root.Get());

    CHECK(a->MeasuredSize.x == 100.0f);
    CHECK(b->MeasuredSize.x == 100.0f);
    CHECK(c->MeasuredSize.x == 100.0f);
    CHECK(d->MeasuredSize.x == 100.0f);
    CHECK(a->MeasuredSize.y == 40.0f);
}

TEST_CASE("box-model: margins are honored in FlowLayout (rows advance by the margin box)")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto flow = core::MakeRef<FlowLayout>(core::DefaultAllocator());
    root->AddView(flow.Get());

    auto a = core::MakeRef<TestView>(core::DefaultAllocator(), 50.0f, 20.0f);
    LayoutStyle lpA;
    lpA.Margin = Thickness{5, 5, 5, 5};
    a->SetLayout(lpA);
    auto b = core::MakeRef<TestView>(core::DefaultAllocator(), 50.0f, 20.0f);
    flow->AddView(a.Get());
    flow->AddView(b.Get());
    LayoutPass(ctx, root.Get());

    // a's border box is inset by its margin; b starts after a's MARGIN box (no overlap).
    CHECK(a->Bounds.x == 5.0f);
    CHECK(a->Bounds.y == 5.0f);
    CHECK(b->Bounds.x >= 60.0f); // 5 + 50 + 5 (+ spacing)
}

TEST_CASE("box-model: a fill-style leaf in a GridLayout cell does not explode to kFloatMax")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto grid = core::MakeRef<GridLayout>(core::DefaultAllocator());
    root->AddView(grid.Get());
    auto sep = core::MakeRef<Separator>(core::DefaultAllocator());
    grid->AddView(sep.Get());
    LayoutPass(ctx, root.Get());

    // Grid children get BOUNDED loose constraints now (was Expand()).
    CHECK(sep->MeasuredSize.x <= 400.0f);
    CHECK(grid->MeasuredSize.x <= 400.0f);
}
