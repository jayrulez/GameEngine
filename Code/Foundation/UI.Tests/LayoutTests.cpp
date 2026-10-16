// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Ported from Sedulous.UI.Tests layout suites: FrameLayoutTests, AbsoluteLayoutTests, FlowLayoutTests,
// DockLayoutTests, GridLayoutTests, FlexLayoutTests (faithful; RefPtr views/params, Beef object-init
// `new X() { F = v }` -> construct + set fields, Math.Abs(..) < eps -> doctest::Approx).
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
static core::RefPtr<TestView> TV(f32 w, f32 h)
{
    return core::MakeRef<TestView>(core::DefaultAllocator(), w, h);
}
template <typename T>
static core::RefPtr<T> New()
{
    return core::MakeRef<T>(core::DefaultAllocator());
}

// === FrameLayout ===

TEST_CASE("frame: Default_ChildAtTopLeft")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto frame = New<FrameLayout>();
    auto child = TV(50, 30);
    frame->AddView(child.Get());
    root->AddView(frame.Get());
    LayoutPass(ctx, root.Get());
    CHECK(child->Bounds.x == doctest::Approx(0));
    CHECK(child->Bounds.y == doctest::Approx(0));
}

TEST_CASE("frame: Gravity_Center")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto frame = New<FrameLayout>();
    auto child = TV(100, 50);
    LayoutStyle lp;
    lp.Gravity = Gravity::Center;
    frame->AddView(child.Get(), lp);
    root->AddView(frame.Get());
    LayoutPass(ctx, root.Get());
    CHECK(child->Bounds.x == doctest::Approx(150).epsilon(0.01));
    CHECK(child->Bounds.y == doctest::Approx(125).epsilon(0.01));
}

TEST_CASE("frame: Gravity_BottomRight")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto frame = New<FrameLayout>();
    auto child = TV(80, 40);
    LayoutStyle lp;
    lp.Gravity = Gravity::Bottom | Gravity::Right;
    frame->AddView(child.Get(), lp);
    root->AddView(frame.Get());
    LayoutPass(ctx, root.Get());
    CHECK(child->Bounds.x == doctest::Approx(320).epsilon(0.01));
    CHECK(child->Bounds.y == doctest::Approx(260).epsilon(0.01));
}

TEST_CASE("frame: Gravity_Fill")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto frame = New<FrameLayout>();
    auto child = TV(50, 30);
    LayoutStyle lp;
    lp.Gravity = Gravity::Fill;
    frame->AddView(child.Get(), lp);
    root->AddView(frame.Get());
    LayoutPass(ctx, root.Get());
    CHECK(child->Width() == doctest::Approx(400).epsilon(0.01));
    CHECK(child->Height() == doctest::Approx(300).epsilon(0.01));
}

TEST_CASE("frame: Padding_OffsetsGravity")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto frame = New<FrameLayout>();
    frame->Padding = Thickness{10, 20, 10, 20};
    auto child = TV(50, 30);
    frame->AddView(child.Get());
    root->AddView(frame.Get());
    LayoutPass(ctx, root.Get());
    CHECK(child->Bounds.x == doctest::Approx(10));
    CHECK(child->Bounds.y == doctest::Approx(20));
}

TEST_CASE("frame: MultipleChildren_Stacked")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto frame = New<FrameLayout>();
    auto a = TV(100, 50);
    auto b = TV(80, 40);
    LayoutStyle lpA;
    lpA.Gravity = Gravity::None;
    LayoutStyle lpB;
    lpB.Gravity = Gravity::Center;
    frame->AddView(a.Get(), lpA);
    frame->AddView(b.Get(), lpB);
    root->AddView(frame.Get());
    LayoutPass(ctx, root.Get());
    CHECK(a->Bounds.x == doctest::Approx(0));
    CHECK(b->Bounds.x == doctest::Approx(160).epsilon(0.01));
}

// Reproduces the scene editor's viewport pane (task #118 camera preview): a vertical FlexLayout
// stacks a fixed-height toolbar over a grow FrameLayout whose Fill child is the 3D viewport and
// whose bottom-right child is the preview overlay. The regression check is the FILL child's
// SCREEN position - the input surface region is LocalToScreen(0,0) + Width/Height, so if the
// nested Fill child's screen origin is off, viewport picking/gizmo input lands in the wrong place.
TEST_CASE("frame: nested Fill child inside a grow FlexLayout keeps its screen origin (task #118)")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);

    auto pane = New<FlexLayout>();
    pane->Direction = Orientation::Vertical;

    auto toolbar = TV(0, 0);
    {
        LayoutStyle lp;
        lp.Width = SizeSpec::Match();
        lp.Height = SizeSpec::Fixed(Unit::Px(30));
        pane->AddView(toolbar.Get(), lp);
    }

    auto frame = New<FrameLayout>();
    auto viewport = TV(256, 256); // measures small like ViewportView; Fill overrides it
    {
        LayoutStyle vfp;
        vfp.Gravity = Gravity::Fill;
        frame->AddView(viewport.Get(), vfp);
    }
    auto preview = TV(320, 204);
    {
        LayoutStyle pfp;
        pfp.Gravity = Gravity::Bottom | Gravity::Right;
        pfp.Margin = Thickness{12, 12, 12, 12};
        frame->AddView(preview.Get(), pfp);
    }
    {
        LayoutStyle lp;
        lp.Width = SizeSpec::Match();
        lp.FlexGrow = 1.0f;
        pane->AddView(frame.Get(), lp);
    }
    root->AddView(pane.Get());
    LayoutPass(ctx, root.Get());

    // The frame occupies everything below the 30px toolbar; the Fill viewport fills the frame.
    CHECK(frame->Bounds.y == doctest::Approx(30));
    CHECK(viewport->Width() == doctest::Approx(400).epsilon(0.01));
    CHECK(viewport->Height() == doctest::Approx(270).epsilon(0.01));

    // The load-bearing assertion: the viewport's content origin in SCREEN space is (0, 30), NOT
    // (0, 0). This is exactly what SyncInputRegion feeds the input surface.
    const Float2 origin = viewport->LocalToScreen(Float2{0.0f, 0.0f});
    CHECK(origin.x == doctest::Approx(0));
    CHECK(origin.y == doctest::Approx(30));

    // The preview overlay sits bottom-right inside the frame, 12px inset (screen space).
    const Float2 pv = preview->LocalToScreen(Float2{0.0f, 0.0f});
    CHECK(pv.x == doctest::Approx(400 - 12 - 320).epsilon(0.01));
    CHECK(pv.y == doctest::Approx(30 + 270 - 12 - 204).epsilon(0.01));
}

// === AbsoluteLayout ===

TEST_CASE("absolute: ChildAtExplicitPosition")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto abs = New<AbsoluteLayout>();
    auto child = TV(50, 30);
    LayoutStyle lp;
    lp.Left = 100;
    lp.Top = 50;
    abs->AddView(child.Get(), lp);
    root->AddView(abs.Get());
    LayoutPass(ctx, root.Get());
    CHECK(child->Bounds.x == doctest::Approx(100));
    CHECK(child->Bounds.y == doctest::Approx(50));
}

TEST_CASE("absolute: DefaultPosition_AtOrigin")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto abs = New<AbsoluteLayout>();
    auto child = TV(50, 30);
    abs->AddView(child.Get());
    root->AddView(abs.Get());
    LayoutPass(ctx, root.Get());
    CHECK(child->Bounds.x == doctest::Approx(0));
    CHECK(child->Bounds.y == doctest::Approx(0));
}

TEST_CASE("absolute: Padding_OffsetsAll")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto abs = New<AbsoluteLayout>();
    abs->Padding = Thickness{10, 20, 10, 20};
    auto child = TV(50, 30);
    LayoutStyle lp;
    lp.Left = 5;
    lp.Top = 5;
    abs->AddView(child.Get(), lp);
    root->AddView(abs.Get());
    LayoutPass(ctx, root.Get());
    CHECK(child->Bounds.x == doctest::Approx(15));
    CHECK(child->Bounds.y == doctest::Approx(25));
}

TEST_CASE("absolute: ChildRetainsMeasuredSize")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto abs = New<AbsoluteLayout>();
    auto child = TV(80, 45);
    LayoutStyle lp;
    lp.Left = 50;
    lp.Top = 50;
    abs->AddView(child.Get(), lp);
    root->AddView(abs.Get());
    LayoutPass(ctx, root.Get());
    CHECK(child->Width() == doctest::Approx(80));
    CHECK(child->Height() == doctest::Approx(45));
}

// === FlowLayout ===

TEST_CASE("flow: Horizontal_NoWrap_SingleLine")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto flow = New<FlowLayout>();
    flow->Orientation = Orientation::Horizontal;
    auto a = TV(50, 30);
    auto b = TV(60, 30);
    flow->AddView(a.Get());
    flow->AddView(b.Get());
    root->AddView(flow.Get());
    LayoutPass(ctx, root.Get());
    CHECK(a->Bounds.y == doctest::Approx(b->Bounds.y));
    CHECK(b->Bounds.x == doctest::Approx(50));
}

TEST_CASE("flow: Horizontal_Wraps_WhenExceedsWidth")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto flow = New<FlowLayout>();
    flow->Orientation = Orientation::Horizontal;
    auto a = TV(150, 30);
    auto b = TV(150, 30);
    auto c = TV(150, 30);
    flow->AddView(a.Get());
    flow->AddView(b.Get());
    flow->AddView(c.Get());
    root->AddView(flow.Get());
    LayoutPass(ctx, root.Get());
    CHECK(a->Bounds.y == doctest::Approx(b->Bounds.y));
    CHECK(c->Bounds.y > a->Bounds.y);
}

TEST_CASE("flow: Horizontal_VSpacing_BetweenLines")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto flow = New<FlowLayout>();
    flow->Orientation = Orientation::Horizontal;
    flow->VSpacing = 8;
    auto a = TV(250, 30);
    auto b = TV(250, 40);
    flow->AddView(a.Get());
    flow->AddView(b.Get());
    root->AddView(flow.Get());
    LayoutPass(ctx, root.Get());
    CHECK(b->Bounds.y == doctest::Approx(38));
}

TEST_CASE("flow: Vertical_NoWrap_SingleColumn")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto flow = New<FlowLayout>();
    flow->Orientation = Orientation::Vertical;
    auto a = TV(50, 30);
    auto b = TV(50, 40);
    flow->AddView(a.Get());
    flow->AddView(b.Get());
    root->AddView(flow.Get());
    LayoutPass(ctx, root.Get());
    CHECK(a->Bounds.x == doctest::Approx(b->Bounds.x));
    CHECK(b->Bounds.y == doctest::Approx(30));
}

TEST_CASE("flow: Vertical_Wraps_WhenExceedsHeight")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto flow = New<FlowLayout>();
    flow->Orientation = Orientation::Vertical;
    auto a = TV(50, 150);
    auto b = TV(50, 150);
    auto c = TV(50, 150);
    flow->AddView(a.Get());
    flow->AddView(b.Get());
    flow->AddView(c.Get());
    root->AddView(flow.Get());
    LayoutPass(ctx, root.Get());
    CHECK(a->Bounds.x == doctest::Approx(b->Bounds.x));
    CHECK(c->Bounds.x > a->Bounds.x);
}

TEST_CASE("flow: Gone_ChildSkipped")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto flow = New<FlowLayout>();
    flow->Orientation = Orientation::Horizontal;
    auto a = TV(50, 30);
    auto b = TV(60, 30);
    b->Visibility = Visibility::Gone;
    auto c = TV(70, 30);
    flow->AddView(a.Get());
    flow->AddView(b.Get());
    flow->AddView(c.Get());
    root->AddView(flow.Get());
    LayoutPass(ctx, root.Get());
    CHECK(c->Bounds.x == doctest::Approx(50));
}

TEST_CASE("flow: Padding_OffsetsContent")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto flow = New<FlowLayout>();
    flow->Orientation = Orientation::Horizontal;
    flow->Padding = Thickness{10, 20, 10, 20};
    auto a = TV(50, 30);
    flow->AddView(a.Get());
    root->AddView(flow.Get());
    LayoutPass(ctx, root.Get());
    CHECK(a->Bounds.x == doctest::Approx(10));
    CHECK(a->Bounds.y == doctest::Approx(20));
}

// === DockLayout ===

static LayoutStyle Docked(Dock dock)
{
    LayoutStyle lp;
    lp.Dock = dock;
    return lp;
}

TEST_CASE("dock: Top_TakesFullWidthMeasuredHeight")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto dock = New<DockLayout>();
    auto top = TV(400, 50);
    dock->AddView(top.Get(), Docked(Dock::Top));
    root->AddView(dock.Get());
    LayoutPass(ctx, root.Get());
    CHECK(top->Bounds.x == doctest::Approx(0));
    CHECK(top->Bounds.y == doctest::Approx(0));
    CHECK(top->Width() == doctest::Approx(400).epsilon(0.01));
    CHECK(top->Height() == doctest::Approx(50).epsilon(0.01));
}

TEST_CASE("dock: Bottom_DocksToBottom")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto dock = New<DockLayout>();
    auto bottom = TV(400, 40);
    dock->AddView(bottom.Get(),
                  Docked(Dock::Bottom));
    root->AddView(dock.Get());
    LayoutPass(ctx, root.Get());
    CHECK(bottom->Bounds.y == doctest::Approx(260).epsilon(0.01));
    CHECK(bottom->Width() == doctest::Approx(400).epsilon(0.01));
}

TEST_CASE("dock: Left_TakesFullHeightMeasuredWidth")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto dock = New<DockLayout>();
    auto left = TV(80, 300);
    dock->AddView(left.Get(),
                  Docked(Dock::Left));
    root->AddView(dock.Get());
    LayoutPass(ctx, root.Get());
    CHECK(left->Bounds.x == doctest::Approx(0));
    CHECK(left->Width() == doctest::Approx(80).epsilon(0.01));
    CHECK(left->Height() == doctest::Approx(300).epsilon(0.01));
}

TEST_CASE("dock: Right_DocksToRight")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto dock = New<DockLayout>();
    auto right = TV(60, 300);
    dock->AddView(right.Get(),
                  Docked(Dock::Right));
    root->AddView(dock.Get());
    LayoutPass(ctx, root.Get());
    CHECK(right->Bounds.x == doctest::Approx(340).epsilon(0.01));
    CHECK(right->Width() == doctest::Approx(60).epsilon(0.01));
}

TEST_CASE("dock: Fill_TakesRemainingSpace")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto dock = New<DockLayout>();
    auto top = TV(400, 50);
    auto fill = TV(50, 30);
    dock->AddView(top.Get(), Docked(Dock::Top));
    dock->AddView(fill.Get(),
                  Docked(Dock::Fill));
    root->AddView(dock.Get());
    LayoutPass(ctx, root.Get());
    CHECK(fill->Bounds.y == doctest::Approx(50).epsilon(0.01));
    CHECK(fill->Width() == doctest::Approx(400).epsilon(0.01));
    CHECK(fill->Height() == doctest::Approx(250).epsilon(0.01));
}

TEST_CASE("dock: LastChildFill_False_DoesNotFill")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto dock = New<DockLayout>();
    dock->LastChildFill = false;
    auto top = TV(400, 50);
    auto last = TV(100, 40);
    dock->AddView(top.Get(), Docked(Dock::Top));
    dock->AddView(last.Get(),
                  Docked(Dock::Left));
    root->AddView(dock.Get());
    LayoutPass(ctx, root.Get());
    CHECK(last->Width() == doctest::Approx(100).epsilon(0.01));
}

TEST_CASE("dock: LastChildFill_True_FillsRemaining")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto dock = New<DockLayout>();
    dock->LastChildFill = true;
    auto top = TV(400, 50);
    auto last = TV(100, 40);
    dock->AddView(top.Get(), Docked(Dock::Top));
    dock->AddView(last.Get(),
                  Docked(Dock::Left));
    root->AddView(dock.Get());
    LayoutPass(ctx, root.Get());
    CHECK(last->Width() == doctest::Approx(400).epsilon(0.01));
    CHECK(last->Height() == doctest::Approx(250).epsilon(0.01));
}

TEST_CASE("dock: MultipleEdges_ShrinkRemaining")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto dock = New<DockLayout>();
    auto top = TV(400, 40);
    auto left = TV(60, 260);
    auto fill = TV(50, 30);
    dock->AddView(top.Get(), Docked(Dock::Top));
    dock->AddView(left.Get(),
                  Docked(Dock::Left));
    dock->AddView(fill.Get(),
                  Docked(Dock::Fill));
    root->AddView(dock.Get());
    LayoutPass(ctx, root.Get());
    CHECK(fill->Bounds.x == doctest::Approx(60).epsilon(0.01));
    CHECK(fill->Bounds.y == doctest::Approx(40).epsilon(0.01));
    CHECK(fill->Width() == doctest::Approx(340).epsilon(0.01));
    CHECK(fill->Height() == doctest::Approx(260).epsilon(0.01));
}

// === GridLayout ===

static LayoutStyle Cell(i32 row, i32 col, i32 rowSpan = 1, i32 colSpan = 1)
{
    LayoutStyle lp;
    lp.GridRow = row;
    lp.GridColumn = col;
    lp.GridRowSpan = rowSpan;
    lp.GridColumnSpan = colSpan;
    return lp;
}

TEST_CASE("grid: FixedColumns_CorrectWidths")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto grid = New<GridLayout>();
    grid->Columns.PushBack(TrackSize::Fixed(100));
    grid->Columns.PushBack(TrackSize::Fixed(200));
    grid->Rows.PushBack(TrackSize::Fixed(50));
    auto a = TV(50, 30);
    auto b = TV(50, 30);
    grid->AddView(a.Get(), Cell(0, 0));
    grid->AddView(b.Get(), Cell(0, 1));
    root->AddView(grid.Get());
    LayoutPass(ctx, root.Get());
    CHECK(a->Width() == doctest::Approx(100).epsilon(0.01));
    CHECK(b->Width() == doctest::Approx(200).epsilon(0.01));
    CHECK(b->Bounds.x == doctest::Approx(100).epsilon(0.01));
}

TEST_CASE("grid: FlexColumns_ProportionalWidths")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto grid = New<GridLayout>();
    grid->Columns.PushBack(TrackSize::Flex(1));
    grid->Columns.PushBack(TrackSize::Flex(3));
    grid->Rows.PushBack(TrackSize::Flex(1));
    auto a = TV(50, 30);
    auto b = TV(50, 30);
    grid->AddView(a.Get(), Cell(0, 0));
    grid->AddView(b.Get(), Cell(0, 1));
    root->AddView(grid.Get());
    LayoutPass(ctx, root.Get());
    CHECK(a->Width() == doctest::Approx(100).epsilon(0.01));
    CHECK(b->Width() == doctest::Approx(300).epsilon(0.01));
}

TEST_CASE("grid: AutoColumns_SizeToContent")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto grid = New<GridLayout>();
    grid->Columns.PushBack(TrackSize::Auto());
    grid->Columns.PushBack(TrackSize::Auto());
    grid->Rows.PushBack(TrackSize::Auto());
    auto a = TV(80, 30);
    auto b = TV(120, 40);
    grid->AddView(a.Get(), Cell(0, 0));
    grid->AddView(b.Get(), Cell(0, 1));
    root->AddView(grid.Get());
    LayoutPass(ctx, root.Get());
    CHECK(a->Width() == doctest::Approx(80).epsilon(0.01));
    CHECK(b->Width() == doctest::Approx(120).epsilon(0.01));
}

TEST_CASE("grid: Spacing_BetweenCells")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto grid = New<GridLayout>();
    grid->Columns.PushBack(TrackSize::Fixed(100));
    grid->Columns.PushBack(TrackSize::Fixed(100));
    grid->Rows.PushBack(TrackSize::Fixed(50));
    grid->ColumnSpacing = 10;
    auto a = TV(50, 30);
    auto b = TV(50, 30);
    grid->AddView(a.Get(), Cell(0, 0));
    grid->AddView(b.Get(), Cell(0, 1));
    root->AddView(grid.Get());
    LayoutPass(ctx, root.Get());
    CHECK(b->Bounds.x == doctest::Approx(110).epsilon(0.01));
}

TEST_CASE("grid: AutoFlow_PlacesSequentially")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto grid = New<GridLayout>();
    grid->AutoFlow = true;
    grid->Columns.PushBack(TrackSize::Flex(1));
    grid->Columns.PushBack(TrackSize::Flex(1));
    grid->Rows.PushBack(TrackSize::Flex(1));
    grid->Rows.PushBack(TrackSize::Flex(1));
    auto a = TV(50, 30);
    auto b = TV(50, 30);
    auto c = TV(50, 30);
    auto d = TV(50, 30);
    grid->AddView(a.Get());
    grid->AddView(b.Get());
    grid->AddView(c.Get());
    grid->AddView(d.Get());
    root->AddView(grid.Get());
    LayoutPass(ctx, root.Get());
    CHECK(a->Bounds.x == doctest::Approx(0).epsilon(0.01));
    CHECK(b->Bounds.x == doctest::Approx(200).epsilon(0.01));
    CHECK(c->Bounds.y == doctest::Approx(150).epsilon(0.01));
    CHECK(d->Bounds.x == doctest::Approx(200).epsilon(0.01));
    CHECK(d->Bounds.y == doctest::Approx(150).epsilon(0.01));
}

TEST_CASE("grid: ColumnSpan_MergesCells")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto grid = New<GridLayout>();
    grid->Columns.PushBack(TrackSize::Fixed(100));
    grid->Columns.PushBack(TrackSize::Fixed(100));
    grid->Columns.PushBack(TrackSize::Fixed(100));
    grid->Rows.PushBack(TrackSize::Fixed(50));
    grid->ColumnSpacing = 5;
    auto a = TV(50, 30);
    grid->AddView(a.Get(), Cell(0, 0, 1, 2));
    root->AddView(grid.Get());
    LayoutPass(ctx, root.Get());
    CHECK(a->Width() == doctest::Approx(205).epsilon(0.01));
}

TEST_CASE("grid: RowSpan_MergesCells")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto grid = New<GridLayout>();
    grid->Columns.PushBack(TrackSize::Fixed(100));
    grid->Rows.PushBack(TrackSize::Fixed(50));
    grid->Rows.PushBack(TrackSize::Fixed(60));
    grid->RowSpacing = 4;
    auto a = TV(50, 30);
    grid->AddView(a.Get(), Cell(0, 0, 2, 1));
    root->AddView(grid.Get());
    LayoutPass(ctx, root.Get());
    CHECK(a->Height() == doctest::Approx(114).epsilon(0.01));
}

TEST_CASE("grid: MixedTracks_FixedAutoFlex")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto grid = New<GridLayout>();
    grid->Columns.PushBack(TrackSize::Fixed(80));
    grid->Columns.PushBack(TrackSize::Auto());
    grid->Columns.PushBack(TrackSize::Flex(1));
    grid->Rows.PushBack(TrackSize::Flex(1));
    auto a = TV(80, 30);
    auto b = TV(60, 30);
    auto c = TV(50, 30);
    grid->AddView(a.Get(), Cell(0, 0));
    grid->AddView(b.Get(), Cell(0, 1));
    grid->AddView(c.Get(), Cell(0, 2));
    root->AddView(grid.Get());
    LayoutPass(ctx, root.Get());
    CHECK(a->Width() == doctest::Approx(80).epsilon(0.01));
    CHECK(b->Width() == doctest::Approx(60).epsilon(0.01));
    CHECK(c->Width() == doctest::Approx(260).epsilon(0.01));
}

// === FlexLayout ===

static LayoutStyle Growth(f32 grow)
{
    LayoutStyle lp;
    lp.FlexGrow = grow;
    return lp;
}

TEST_CASE("flex: Row_ChildrenArrangedHorizontally")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto flex = New<FlexLayout>();
    flex->Direction = Orientation::Horizontal;
    auto a = TV(50, 30);
    auto b = TV(60, 30);
    flex->AddView(a.Get());
    flex->AddView(b.Get());
    root->AddView(flex.Get());
    LayoutPass(ctx, root.Get());
    CHECK(a->Bounds.x < b->Bounds.x);
    CHECK(b->Bounds.x == doctest::Approx(50));
}

TEST_CASE("flex: Column_ChildrenArrangedVertically")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto flex = New<FlexLayout>();
    flex->Direction = Orientation::Vertical;
    auto a = TV(50, 30);
    auto b = TV(50, 40);
    flex->AddView(a.Get());
    flex->AddView(b.Get());
    root->AddView(flex.Get());
    LayoutPass(ctx, root.Get());
    CHECK(a->Bounds.y < b->Bounds.y);
    CHECK(b->Bounds.y == doctest::Approx(30));
}

TEST_CASE("flex: Row_Spacing")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto flex = New<FlexLayout>();
    flex->Direction = Orientation::Horizontal;
    flex->Spacing = 10;
    auto a = TV(50, 30);
    auto b = TV(60, 30);
    flex->AddView(a.Get());
    flex->AddView(b.Get());
    root->AddView(flex.Get());
    LayoutPass(ctx, root.Get());
    CHECK(b->Bounds.x == doctest::Approx(60));
}

TEST_CASE("flex: Column_Spacing")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto flex = New<FlexLayout>();
    flex->Direction = Orientation::Vertical;
    flex->Spacing = 8;
    auto a = TV(50, 30);
    auto b = TV(50, 40);
    flex->AddView(a.Get());
    flex->AddView(b.Get());
    root->AddView(flex.Get());
    LayoutPass(ctx, root.Get());
    CHECK(b->Bounds.y == doctest::Approx(38));
}

TEST_CASE("flex: Row_Grow_DistributesExtraSpace")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto flex = New<FlexLayout>();
    flex->Direction = Orientation::Horizontal;
    auto a = TV(50, 30);
    auto b = TV(50, 30);
    flex->AddView(a.Get(), Growth(1));
    flex->AddView(b.Get(), Growth(1));
    root->AddView(flex.Get());
    LayoutPass(ctx, root.Get());
    CHECK(a->Width() == doctest::Approx(200).epsilon(0.01));
    CHECK(b->Width() == doctest::Approx(200).epsilon(0.01));
}

TEST_CASE("flex: Row_Grow_WeightedDistribution")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto flex = New<FlexLayout>();
    flex->Direction = Orientation::Horizontal;
    auto a = TV(0, 30);
    auto b = TV(0, 30);
    flex->AddView(a.Get(), Growth(1));
    flex->AddView(b.Get(), Growth(3));
    root->AddView(flex.Get());
    LayoutPass(ctx, root.Get());
    CHECK(a->Width() == doctest::Approx(100).epsilon(0.01));
    CHECK(b->Width() == doctest::Approx(300).epsilon(0.01));
}

TEST_CASE("flex: Row_Grow_FixedPlusFlexible")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto flex = New<FlexLayout>();
    flex->Direction = Orientation::Horizontal;
    auto fixedChild = TV(100, 30);
    auto flexChild = TV(0, 30);
    flex->AddView(fixedChild.Get());
    flex->AddView(flexChild.Get(), Growth(1));
    root->AddView(flex.Get());
    LayoutPass(ctx, root.Get());
    CHECK(fixedChild->Width() == doctest::Approx(100).epsilon(0.01));
    CHECK(flexChild->Width() == doctest::Approx(300).epsilon(0.01));
}

TEST_CASE("flex: Column_Grow")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto flex = New<FlexLayout>();
    flex->Direction = Orientation::Vertical;
    auto a = TV(50, 0);
    auto b = TV(50, 0);
    flex->AddView(a.Get(), Growth(1));
    flex->AddView(b.Get(), Growth(1));
    root->AddView(flex.Get());
    LayoutPass(ctx, root.Get());
    CHECK(a->Height() == doctest::Approx(150).epsilon(0.01));
    CHECK(b->Height() == doctest::Approx(150).epsilon(0.01));
}

TEST_CASE("flex: Justify_End")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto flex = New<FlexLayout>();
    flex->Direction = Orientation::Horizontal;
    flex->JustifyContent = Justify::End;
    auto a = TV(50, 30);
    flex->AddView(a.Get());
    root->AddView(flex.Get());
    LayoutPass(ctx, root.Get());
    CHECK(a->Bounds.x == doctest::Approx(350).epsilon(0.01));
}

TEST_CASE("flex: Justify_Center")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto flex = New<FlexLayout>();
    flex->Direction = Orientation::Horizontal;
    flex->JustifyContent = Justify::Center;
    auto a = TV(100, 30);
    flex->AddView(a.Get());
    root->AddView(flex.Get());
    LayoutPass(ctx, root.Get());
    CHECK(a->Bounds.x == doctest::Approx(150).epsilon(0.01));
}

TEST_CASE("flex: Justify_SpaceBetween")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto flex = New<FlexLayout>();
    flex->Direction = Orientation::Horizontal;
    flex->JustifyContent = Justify::SpaceBetween;
    auto a = TV(50, 30);
    auto b = TV(50, 30);
    flex->AddView(a.Get());
    flex->AddView(b.Get());
    root->AddView(flex.Get());
    LayoutPass(ctx, root.Get());
    CHECK(a->Bounds.x == doctest::Approx(0));
    CHECK(b->Bounds.x == doctest::Approx(350).epsilon(0.01));
}

TEST_CASE("flex: Justify_SpaceEvenly")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto flex = New<FlexLayout>();
    flex->Direction = Orientation::Horizontal;
    flex->JustifyContent = Justify::SpaceEvenly;
    auto a = TV(50, 30);
    auto b = TV(50, 30);
    flex->AddView(a.Get());
    flex->AddView(b.Get());
    root->AddView(flex.Get());
    LayoutPass(ctx, root.Get());
    CHECK(a->Bounds.x == doctest::Approx(100).epsilon(0.01));
    CHECK(b->Bounds.x == doctest::Approx(250).epsilon(0.01));
}

TEST_CASE("flex: AlignItems_Stretch")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto flex = New<FlexLayout>();
    flex->Direction = Orientation::Horizontal;
    flex->AlignItems = Align::Stretch;
    auto a = TV(50, 30);
    flex->AddView(a.Get());
    root->AddView(flex.Get());
    LayoutPass(ctx, root.Get());
    CHECK(a->Height() == doctest::Approx(300).epsilon(0.01));
}

TEST_CASE("flex: AlignItems_Center")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto flex = New<FlexLayout>();
    flex->Direction = Orientation::Horizontal;
    flex->AlignItems = Align::Center;
    auto a = TV(50, 30);
    flex->AddView(a.Get());
    root->AddView(flex.Get());
    LayoutPass(ctx, root.Get());
    CHECK(a->Bounds.y == doctest::Approx(135).epsilon(0.01));
}

TEST_CASE("flex: AlignItems_End")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto flex = New<FlexLayout>();
    flex->Direction = Orientation::Horizontal;
    flex->AlignItems = Align::End;
    auto a = TV(50, 30);
    flex->AddView(a.Get());
    root->AddView(flex.Get());
    LayoutPass(ctx, root.Get());
    CHECK(a->Bounds.y == doctest::Approx(270).epsilon(0.01));
}

TEST_CASE("flex: Gone_ChildSkipped")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto flex = New<FlexLayout>();
    flex->Direction = Orientation::Horizontal;
    auto a = TV(50, 30);
    auto b = TV(60, 30);
    b->Visibility = Visibility::Gone;
    auto c = TV(70, 30);
    flex->AddView(a.Get());
    flex->AddView(b.Get());
    flex->AddView(c.Get());
    root->AddView(flex.Get());
    LayoutPass(ctx, root.Get());
    CHECK(c->Bounds.x == doctest::Approx(50));
}

TEST_CASE("flex: Padding_OffsetsChildren")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto flex = New<FlexLayout>();
    flex->Direction = Orientation::Horizontal;
    flex->Padding = Thickness{10, 20, 10, 20};
    auto a = TV(50, 30);
    flex->AddView(a.Get());
    root->AddView(flex.Get());
    LayoutPass(ctx, root.Get());
    CHECK(a->Bounds.x == doctest::Approx(10));
    CHECK(a->Bounds.y >= 20);
}

TEST_CASE("absolute: MultipleChildren_IndependentPositions")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto abs = New<AbsoluteLayout>();
    auto a = TV(50, 30);
    auto b = TV(60, 40);
    LayoutStyle lpa;
    lpa.Left = 10;
    lpa.Top = 10;
    LayoutStyle lpb;
    lpb.Left = 200;
    lpb.Top = 150;
    abs->AddView(a.Get(), lpa);
    abs->AddView(b.Get(), lpb);
    root->AddView(abs.Get());
    LayoutPass(ctx, root.Get());
    CHECK(a->Bounds.x == doctest::Approx(10));
    CHECK(b->Bounds.x == doctest::Approx(200));
    CHECK(b->Bounds.y == doctest::Approx(150));
}

TEST_CASE("flow: Horizontal_Spacing")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto flow = New<FlowLayout>();
    flow->Orientation = Orientation::Horizontal;
    flow->HSpacing = 10;
    flow->VSpacing = 5;
    auto a = TV(50, 30);
    auto b = TV(60, 30);
    flow->AddView(a.Get());
    flow->AddView(b.Get());
    root->AddView(flow.Get());
    LayoutPass(ctx, root.Get());
    CHECK(b->Bounds.x == doctest::Approx(60)); // a width 50 + HSpacing 10
}

// === Custom container (spec P0: a ViewGroup subclass reads child.Layout() directly) ===

namespace
{
    /// Stacks children top-to-bottom; each child's LayoutStyle::Left offsets it horizontally and
    /// LayoutStyle::Gravity Right pins it to the right edge. No parameter subclass, no registry.
    class StaircaseLayout : public ViewGroup
    {
        RTTI_OBJECT(StaircaseLayout, ViewGroup)
    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            f32 h = 0, w = 0;
            for (usize i = 0; i < ChildCount(); ++i)
            {
                View* child = GetChildAt(i);
                child->Measure(constraints.Loosen());
                const Float2 mb = child->MarginBoxSize();
                h += mb.y;
                w = Max(w, child->Layout().Left + mb.x);
            }
            MeasuredSize = Float2{constraints.ConstrainWidth(w), constraints.ConstrainHeight(h)};
        }
        void OnLayout(f32, f32, f32 width, f32) override
        {
            f32 y = 0;
            for (usize i = 0; i < ChildCount(); ++i)
            {
                View* child = GetChildAt(i);
                const Float2 mb = child->MarginBoxSize();
                const LayoutStyle& ls = child->Layout();
                const f32 x = (ls.Gravity & Gravity::Right) == Gravity::Right ? width - mb.x : ls.Left.Value();
                child->Layout(x, y, mb.x, mb.y);
                y += mb.y;
            }
        }
    };
    RTTI_DEFINE_OBJECT(StaircaseLayout, "rtti::ui::tests")
}

TEST_CASE("custom: Container_PositionsChildrenFromLayoutStyle")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto stairs = New<StaircaseLayout>();
    auto a = TV(50, 20);
    auto b = TV(50, 20);
    auto c = TV(50, 20);
    LayoutStyle la;
    la.Left = 10;
    LayoutStyle lb;
    lb.Left = 30;
    lb.Margin = Thickness{0, 5, 0, 5};
    LayoutStyle lc;
    lc.Gravity = Gravity::Right;
    stairs->AddView(a.Get(), la);
    stairs->AddView(b.Get(), lb);
    stairs->AddView(c.Get(), lc);
    LayoutStyle fill;
    fill.Width = SizeSpec::Match();
    root->AddView(stairs.Get(), fill);
    LayoutPass(ctx, root.Get());

    CHECK(a->Bounds.x == doctest::Approx(10));
    CHECK(a->Bounds.y == doctest::Approx(0));
    CHECK(b->Bounds.x == doctest::Approx(30));
    CHECK(b->Bounds.y == doctest::Approx(25)); // 20 + top margin 5
    CHECK(c->Bounds.x == doctest::Approx(350)); // pinned right: 400 - 50
    CHECK(c->Bounds.y == doctest::Approx(50));  // 20 + (5 + 20 + 5)
}

TEST_CASE("grid: AutoFlow_KeepsChildIntent")
{
    // Auto-flow assigns cells per pass and never writes the placement back into the
    // child's LayoutStyle (GridRow/GridColumn stay -1), so reordering re-flows.
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto grid = New<GridLayout>();
    grid->Columns.PushBack(TrackSize::Fixed(100));
    grid->Columns.PushBack(TrackSize::Fixed(100));
    grid->Rows.PushBack(TrackSize::Fixed(50));
    grid->Rows.PushBack(TrackSize::Fixed(50));
    grid->AutoFlow = true;
    auto a = TV(10, 10);
    auto b = TV(10, 10);
    auto c = TV(10, 10);
    grid->AddView(a.Get());
    grid->AddView(b.Get());
    grid->AddView(c.Get());
    root->AddView(grid.Get());
    LayoutPass(ctx, root.Get());
    CHECK(c->Bounds.x == doctest::Approx(0));
    CHECK(c->Bounds.y == doctest::Approx(50));
    CHECK(c->Layout().GridRow == -1);
    CHECK(c->Layout().GridColumn == -1);

    grid->RemoveView(a.Get());
    LayoutPass(ctx, root.Get());
    CHECK(c->Bounds.x == doctest::Approx(100)); // re-flowed into the second cell
    CHECK(c->Bounds.y == doctest::Approx(0));
}

TEST_CASE("dock: a styled border is part of the measured size (chrome, not just padding)")
{
    // The measure deflated by the CHROME but added back only the padding, so a bordered dock
    // measured short by the border (found by the Beef port).
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto host = New<FrameLayout>(); // loose constraints: the dock wraps its content
    auto dock = New<DockLayout>();
    dock->SetStyle(StyleProperty::BorderWidth, 5.0f);
    dock->Padding = Thickness{2, 2, 2, 2};
    auto top = TV(100, 50);
    dock->AddView(top.Get(), Docked(Dock::Top));
    host->AddView(dock.Get());
    root->AddView(host.Get());
    LayoutPass(ctx, root.Get());
    // A Top-docked child spans the dock rather than sizing it, so only the height is its own.
    CHECK(dock->MeasuredSize.y == doctest::Approx(50 + 2 * (5 + 2)));
    CHECK(top->Bounds.y == doctest::Approx(7)); // border + padding
    CHECK(top->Bounds.x == doctest::Approx(7));
}

TEST_CASE("layout: Flex, Flow, Grid and Absolute inset their children by the sheet's padding and border")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto flex = New<FlexLayout>();
    auto flow = New<FlowLayout>();
    auto grid = New<GridLayout>();
    auto absolute = New<AbsoluteLayout>();
    ViewGroup* groups[] = {flex.Get(), flow.Get(), grid.Get(), absolute.Get()};
    core::RefPtr<TestView> children[4];
    auto column = New<FlexLayout>();
    column->Direction = Orientation::Vertical;
    for (usize i = 0; i < 4; ++i)
    {
        children[i] = TV(20, 10);
        groups[i]->AddView(children[i].Get());
        groups[i]->SetStyle(StyleProperty::Padding, Thickness{7, 5, 7, 5});
        groups[i]->SetStyle(StyleProperty::BorderWidth, 2.0f);
        column->AddView(groups[i]);
    }
    root->AddView(column.Get());
    LayoutPass(ctx, root.Get());
    for (usize i = 0; i < 4; ++i)
    {
        CAPTURE(i);
        CHECK(children[i]->Bounds.x == doctest::Approx(9.0f)); // 7 padding + 2 border
        CHECK(children[i]->Bounds.y == doctest::Approx(7.0f)); // 5 + 2
    }
}

// === Flex shrink, sheet padding in every layout, transformed coordinates ===

namespace
{
    LayoutStyle Shrink(f32 shrink)
    {
        LayoutStyle lp;
        lp.FlexShrink = shrink;
        return lp;
    }
    // A 200 x 50 row holding two 150-wide children: 100 too many.
    struct OverfullRow
    {
        UIContext ctx{DefaultAllocator()};
        core::RefPtr<RootView> root = MakeRoot();
        core::RefPtr<FlexLayout> flex = New<FlexLayout>();
        core::RefPtr<TestView> a = TV(150, 30);
        core::RefPtr<TestView> b = TV(150, 30);
        OverfullRow(const LayoutStyle& la, const LayoutStyle& lb)
        {
            Init(ctx, root.Get(), 400, 300);
            flex->Direction = Orientation::Horizontal;
            flex->AddView(a.Get(), la);
            flex->AddView(b.Get(), lb);
            LayoutStyle box;
            box.Width = SizeSpec::Fixed(Unit::Px(200));
            box.Height = SizeSpec::Fixed(Unit::Px(50));
            root->AddView(flex.Get(), box);
            LayoutPass(ctx, root.Get());
        }
    };
}

TEST_CASE("flex: Shrink_OverflowSharedByShrinkTimesBase")
{
    OverfullRow even(Shrink(1), Shrink(1));
    CHECK(even.a->Width() == doctest::Approx(100.0f));
    CHECK(even.b->Width() == doctest::Approx(100.0f));
    CHECK(even.b->Bounds.x == doctest::Approx(100.0f));

    OverfullRow weighted(Shrink(3), Shrink(1)); // a gives three parts of four
    CHECK(weighted.a->Width() == doctest::Approx(75.0f));
    CHECK(weighted.b->Width() == doctest::Approx(125.0f));

    OverfullRow one(Shrink(1), LayoutStyle{}); // undeclared: does not shrink
    CHECK(one.a->Width() == doctest::Approx(50.0f));
    CHECK(one.b->Width() == doctest::Approx(150.0f));

    OverfullRow none(LayoutStyle{}, LayoutStyle{}); // nothing shrinks: the row overflows as before
    CHECK(none.a->Width() == doctest::Approx(150.0f));
    CHECK(none.b->Width() == doctest::Approx(150.0f));
}

TEST_CASE("flex: Shrink_StopsAtMinimumAndTheRestGiveMore")
{
    LayoutStyle floor = Shrink(1);
    floor.MinWidth = Unit::Px(120);
    OverfullRow row(floor, Shrink(1));
    CHECK(row.a->Width() == doctest::Approx(120.0f)); // its share would take it to 100
    CHECK(row.b->Width() == doctest::Approx(80.0f));
}
