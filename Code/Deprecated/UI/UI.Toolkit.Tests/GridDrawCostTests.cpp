// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Draw-cost probe for large property grids (the ImportTest blank-UI incident: a ~1300-field
// generic asset form exceeded the VG renderer's per-frame vertex ceiling, blanking the whole
// window - blank-but-interactive, the 9e35fb51 class). Pins that off-screen rows contribute no
// tessellated geometry: a ScrollView'd grid must draw its VIEWPORT, not its whole content.
// (Headless: no FontService, so text is excluded - the on-screen cost is strictly higher, which
// makes the bound this test pins conservative.)
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include <cstdio>

import foundation.core;
import foundation.vg;
import foundation.ui;
import foundation.ui.toolkit;

using namespace foundation::core;
using namespace foundation::ui;
namespace vg = foundation::vg;

namespace
{
    u32 DrawOnce(RootView& root)
    {
        vg::VGContext vgContext;
        UIDrawContext draw(vgContext, 1.0f, nullptr);
        root.OnDraw(draw);
        return static_cast<u32>(vgContext.GetBatch().VertexCount());
    }
}

TEST_CASE("propertygrid: off-screen rows tessellate no geometry (blank-UI ceiling regression)")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRef<RootView>(DefaultAllocator());
    root->ViewportSize = Float2{800, 600};
    ctx.AddRootView(root.Get());

    auto makeGrid = [&](i32 rows)
    {
        auto grid = MakeRef<toolkit::PropertyGrid>(DefaultAllocator());
        for (i32 i = 0; i < rows; ++i)
        {
            grid->AddProperty(RefPtr<toolkit::PropertyEditor>(
                MakeRef<toolkit::FloatEditor>(DefaultAllocator(),
                                              Format(u8"field{}", i).AsView(), 1.0f)
                    .Get()));
        }
        return grid;
    };

    // A viewport-sized grid: ~20 visible rows at the default 26px row height.
    auto small = makeGrid(20);
    root->AddView(small.Get());
    ctx.BeginFrame(0.016f);
    root->Measure(BoxConstraints::Tight(800, 600));
    root->Layout(0, 0, 800, 600);
    const u32 smallVerts = DrawOnce(*root);
    root->RemoveView(small.Get(), true);

    // The incident-sized grid: ~1300 rows, same viewport. Only ~20 rows are visible; the
    // draw cost must track the VIEWPORT, not the row count.
    auto big = makeGrid(1300);
    root->AddView(big.Get());
    ctx.BeginFrame(0.016f);
    root->Measure(BoxConstraints::Tight(800, 600));
    root->Layout(0, 0, 800, 600);
    const u32 bigVerts = DrawOnce(*root);

    std::printf("[grid-draw-cost] 20 rows = %u verts, 1300 rows = %u verts\n", smallVerts,
                bigVerts);
    CHECK(smallVerts > 0u);
    // Off-screen culling bound: the big grid may cost somewhat more (scrollbar, partial rows),
    // but never scale with total row count. 4x the visible-set cost is generous headroom;
    // without culling the 1300-row grid draws ~65x the small one and this fails.
    CHECK(bigVerts < smallVerts * 4u);
}

// Companion to the draw cull: a DEFAULT-COLLAPSED category's rows are Gone - they cost no
// MEASURE either, so a bulk section makes neither the draw list nor the per-frame layout scale
// with its row count (the generic asset form marks every array group collapsed).
TEST_CASE("propertygrid: a default-collapsed category costs no layout or draw")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRef<RootView>(DefaultAllocator());
    root->ViewportSize = Float2{800, 600};
    ctx.AddRootView(root.Get());

    auto grid = MakeRef<toolkit::PropertyGrid>(DefaultAllocator());
    grid->SetCategoryDefaultCollapsed(u8"bulk");
    grid->AddProperty(RefPtr<toolkit::PropertyEditor>(
        MakeRef<toolkit::FloatEditor>(DefaultAllocator(), StringView(u8"visible"), 1.0f).Get()));
    for (i32 i = 0; i < 1000; ++i)
    {
        grid->AddProperty(RefPtr<toolkit::PropertyEditor>(
            MakeRef<toolkit::FloatEditor>(DefaultAllocator(), Format(u8"row{}", i).AsView(), 1.0f,
                                          0.0, 100.0, 1.0, 2,
                                          Function<void(f64)>{}, u8"bulk")
                .Get()));
    }
    root->AddView(grid.Get());
    ctx.BeginFrame(0.016f);
    root->Measure(BoxConstraints::Tight(800, 600));
    root->Layout(0, 0, 800, 600);

    const u32 collapsedVerts = DrawOnce(*root);

    // A tiny grid with just the visible row + an empty expander header is the same ballpark:
    // the 1000 collapsed rows must contribute neither geometry nor (by being Gone) layout.
    auto tiny = MakeRef<toolkit::PropertyGrid>(DefaultAllocator());
    tiny->SetCategoryDefaultCollapsed(u8"bulk");
    tiny->AddProperty(RefPtr<toolkit::PropertyEditor>(
        MakeRef<toolkit::FloatEditor>(DefaultAllocator(), StringView(u8"visible"), 1.0f).Get()));
    tiny->AddProperty(RefPtr<toolkit::PropertyEditor>(
        MakeRef<toolkit::FloatEditor>(DefaultAllocator(), StringView(u8"row"), 1.0f, 0.0, 100.0,
                                      1.0, 2, Function<void(f64)>{}, u8"bulk")
            .Get()));
    root->RemoveView(grid.Get(), true);
    root->AddView(tiny.Get());
    ctx.BeginFrame(0.016f);
    root->Measure(BoxConstraints::Tight(800, 600));
    root->Layout(0, 0, 800, 600);
    const u32 tinyVerts = DrawOnce(*root);

    std::printf("[grid-collapsed] 1000 collapsed rows = %u verts, 1 collapsed row = %u verts\n",
                collapsedVerts, tinyVerts);
    CHECK(collapsedVerts <= tinyVerts + 64u); // same ballpark: bulk contributes ~nothing
}
