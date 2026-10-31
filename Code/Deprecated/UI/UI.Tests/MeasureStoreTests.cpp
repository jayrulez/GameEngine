// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The measure store (View::Measure): within one layout pass a repeat request with the same
// constraints is answered from the view's last answers, not by measuring its subtree again.
// Nested flex containers measure a child more than once, and without the store the count would
// double at every level. A stored answer leaves the subtree as its last real measure left it, so
// when that measure was for other constraints, Layout measures it again first: a stack of
// requests lays out by the last one.
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
    // A leaf that fills the width it is given (50 when unbounded) and counts its measures.
    class CountingView final : public View
    {
    public:
        u32 measures = 0;

    protected:
        void OnMeasure(BoxConstraints c) override
        {
            ++measures;
            const f32 w = BoxConstraints::IsBounded(c.MaxWidth) ? c.MaxWidth : 50.0f;
            MeasuredSize = Float2{c.ConstrainWidth(w), c.ConstrainHeight(20.0f)};
        }
    };

    // A container that asks its one child three times (100 wide, then 60, then 100 again) and lays
    // it out by the last answer: the case a stored answer has to get right.
    class AskThrice final : public ViewGroup
    {
    protected:
        void OnMeasure(BoxConstraints) override
        {
            View* child = GetChildAt(0);
            child->Measure(BoxConstraints::Tight(100.0f, 20.0f));
            child->Measure(BoxConstraints::Tight(60.0f, 20.0f));
            child->Measure(BoxConstraints::Tight(100.0f, 20.0f));
            MeasuredSize = child->MeasuredSize;
        }
        void OnLayout(f32, f32, f32, f32) override
        {
            View* child = GetChildAt(0);
            child->Layout(0.0f, 0.0f, child->MeasuredSize.x, child->MeasuredSize.y);
        }
    };
}

TEST_CASE("measure store: deep nested flex columns measure a leaf a few times a pass, not hundreds")
{
    // Eight nested columns, each holding a leaf and the next column, every child stretched across
    // (a cross-axis Match, the re-measure that would double per level).
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get(), 400, 600);
    Array<core::RefPtr<FlexLayout>> columns;
    Array<core::RefPtr<CountingView>> leaves;
    ViewGroup* parent = root.Get();
    LayoutStyle stretch;
    stretch.Width = SizeSpec::Match();
    for (u32 level = 0; level < 8; ++level)
    {
        auto column = core::MakeRef<FlexLayout>(core::DefaultAllocator());
        column->Direction = Orientation::Vertical;
        auto leaf = core::MakeRef<CountingView>(core::DefaultAllocator());
        column->AddView(leaf.Get(), stretch);
        parent->AddView(column.Get(), stretch);
        parent = column.Get();
        columns.PushBack(column);
        leaves.PushBack(leaf);
    }
    LayoutPass(ctx, root.Get());
    for (const auto& leaf : leaves)
    {
        leaf->measures = 0;
    }
    LayoutPass(ctx, root.Get()); // one pass, counted
    // One more per level (each level's measured height is a new constraint below it: 2, 4, 5, 6,
    // ... 10 at eight levels) rather than doubling (2, 4, 8, ... 256).
    const u32 deepest = leaves[leaves.Size() - 1]->measures;
    MESSAGE("measures of the deepest leaf in one pass: " << deepest);
    CHECK(deepest <= 12u);
    // And the layout is the one a full re-measure gives: every leaf across the whole width.
    for (const auto& leaf : leaves)
    {
        CHECK(leaf->Bounds.width == doctest::Approx(400.0f));
    }
}

TEST_CASE("measure store: a view laid out by a stored answer lays its subtree out for that answer")
{
    // The child is asked for 100, then 60, then 100 again: the third is answered from the store,
    // while the child's own row still holds what the 60 gave it. Laid out at 100, the row's
    // stretching leaf is 100 wide too, not 60.
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get(), 400, 600);
    auto asker = core::MakeRef<AskThrice>(core::DefaultAllocator());
    auto row = core::MakeRef<FlexLayout>(core::DefaultAllocator());
    row->Direction = Orientation::Horizontal;
    auto leaf = core::MakeRef<CountingView>(core::DefaultAllocator());
    LayoutStyle grow;
    grow.FlexGrow = 1;
    row->AddView(leaf.Get(), grow);
    asker->AddView(row.Get());
    root->AddView(asker.Get());
    LayoutPass(ctx, root.Get());
    CHECK(row->Bounds.width == doctest::Approx(100.0f));
    CHECK(leaf->Bounds.width == doctest::Approx(100.0f));
}

TEST_CASE("measure store: a change between measures is measured afresh")
{
    // An invalidation starts a new generation: the same constraints measure again.
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get(), 400, 600);
    auto leaf = core::MakeRef<CountingView>(core::DefaultAllocator());
    root->AddView(leaf.Get());
    LayoutPass(ctx, root.Get());
    leaf->measures = 0;
    leaf->Measure(BoxConstraints::Loose(200.0f, 50.0f));
    leaf->Measure(BoxConstraints::Loose(200.0f, 50.0f));
    CHECK(leaf->measures == 1u); // the repeat came from the store
    leaf->Invalidate();
    leaf->Measure(BoxConstraints::Loose(200.0f, 50.0f));
    CHECK(leaf->measures == 2u);
}
