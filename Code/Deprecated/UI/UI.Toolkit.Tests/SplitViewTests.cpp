// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Smoke test for SplitView: panes, ratio clamp + event, and a basic horizontal layout split.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.ui;
import foundation.ui.toolkit;

using namespace foundation::ui;
using namespace foundation::ui::toolkit;
using namespace foundation::core;
namespace core = foundation::core;

TEST_CASE("toolkit-splitview: PanesRatioAndLayout")
{
    auto split = core::MakeRef<SplitView>(core::DefaultAllocator());
    CHECK(split->Orientation == Orientation::Horizontal);
    CHECK(split->SplitRatio() == doctest::Approx(0.5f));

    auto first = core::MakeRef<Panel>(core::DefaultAllocator());
    auto second = core::MakeRef<Panel>(core::DefaultAllocator());
    split->SetPanes(first.Get(), second.Get());
    CHECK(split->FirstPane() == first.Get());
    CHECK(split->SecondPane() == second.Get());
    CHECK(split->ChildCount() == 2u);

    // Ratio clamps to [0,1] and fires OnSplitChanged.
    bool fired = false;
    f32 lastRatio = -1.0f;
    split->OnSplitChanged.Add(
        [&](SplitView*, f32 r)
        {
            fired = true;
            lastRatio = r;
        });
    split->SetSplitRatio(2.0f);
    CHECK(split->SplitRatio() == doctest::Approx(1.0f));
    CHECK(fired);
    CHECK(lastRatio == doctest::Approx(1.0f));

    // Basic horizontal layout: available = 200 - DividerSize(6) = 194, ratio 0.5 -> 97 each.
    split->SetSplitRatio(0.5f);
    split->Measure(BoxConstraints::Tight(200.0f, 100.0f));
    split->Layout(0.0f, 0.0f, 200.0f, 100.0f);
    CHECK(first->Width() == doctest::Approx(97.0f));
    CHECK(second->Width() == doctest::Approx(97.0f));
    CHECK(first->Height() == doctest::Approx(100.0f));
}

TEST_CASE("toolkit-splitview: pane collapse shrinks to content, hides divider, preserves ratio")
{
    auto split = core::MakeRef<SplitView>(core::DefaultAllocator());
    split->Orientation = Orientation::Vertical;

    auto first = core::MakeRef<Panel>(core::DefaultAllocator());
    // Second pane = a "bar" whose content minimum height is 26px.
    auto second = core::MakeRef<FlexLayout>(core::DefaultAllocator());
    second->Direction = Orientation::Vertical;
    auto bar = core::MakeRef<Panel>(core::DefaultAllocator());
    {
        LayoutStyle lp;
        lp.Width = SizeSpec::Match();
        lp.Height = SizeSpec::Fixed(Unit::Px(26.0f));
        second->AddView(bar.Get(), lp);
    }
    split->SetPanes(first.Get(), second.Get());
    split->SetSplitRatio(0.5f);

    // Expanded (vertical): available = 200 - 6 = 194; first = 194*0.5 = 97.
    split->Measure(BoxConstraints::Tight(300.0f, 200.0f));
    split->Layout(0.0f, 0.0f, 300.0f, 200.0f);
    CHECK_FALSE(split->AnyPaneCollapsed());
    CHECK(first->Height() == doctest::Approx(97.0f));

    // Collapse the second pane: it shrinks to its 26px content, first fills the rest (no divider space),
    // and the stored ratio is untouched.
    split->SetPaneCollapsed(SplitPane::Second, true);
    CHECK(split->IsPaneCollapsed(SplitPane::Second));
    CHECK(split->AnyPaneCollapsed());
    split->Measure(BoxConstraints::Tight(300.0f, 200.0f));
    split->Layout(0.0f, 0.0f, 300.0f, 200.0f);
    CHECK(second->Height() == doctest::Approx(26.0f));
    CHECK(first->Height() == doctest::Approx(200.0f - 26.0f));
    CHECK(split->SplitRatio() == doctest::Approx(0.5f));

    // Un-collapse restores the split at the preserved ratio.
    split->SetPaneCollapsed(SplitPane::Second, false);
    CHECK_FALSE(split->AnyPaneCollapsed());
    split->Measure(BoxConstraints::Tight(300.0f, 200.0f));
    split->Layout(0.0f, 0.0f, 300.0f, 200.0f);
    CHECK(first->Height() == doctest::Approx(97.0f));
}
