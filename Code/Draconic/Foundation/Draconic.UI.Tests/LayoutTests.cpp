// Ported from Sedulous.UI.Tests layout suites: FrameLayoutTests, AbsoluteLayoutTests, FlowLayoutTests,
// DockLayoutTests, GridLayoutTests, FlexLayoutTests (faithful; RefPtr views/params, Beef object-init
// `new X() { F = v }` -> construct + set fields, Math.Abs(..) < eps -> doctest::Approx).
#include <doctest/doctest.h>
#include "Draconic.Core/Prelude.h"
import draconic.core;
import draconic.ui;
#include "TestHelpers.h"

using namespace draconic::ui;
using namespace draconic::ui::tests;
using namespace draconic::core;
namespace core = draconic::core;

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
    UIContext ctx;
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
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto frame = New<FrameLayout>();
    auto child = TV(100, 50);
    auto lp = New<FrameLayoutParams>();
    lp->Gravity = Gravity::Center;
    frame->AddView(child.Get(), lp);
    root->AddView(frame.Get());
    LayoutPass(ctx, root.Get());
    CHECK(child->Bounds.x == doctest::Approx(150).epsilon(0.01));
    CHECK(child->Bounds.y == doctest::Approx(125).epsilon(0.01));
}

TEST_CASE("frame: Gravity_BottomRight")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto frame = New<FrameLayout>();
    auto child = TV(80, 40);
    auto lp = New<FrameLayoutParams>();
    lp->Gravity = Gravity::Bottom | Gravity::Right;
    frame->AddView(child.Get(), lp);
    root->AddView(frame.Get());
    LayoutPass(ctx, root.Get());
    CHECK(child->Bounds.x == doctest::Approx(320).epsilon(0.01));
    CHECK(child->Bounds.y == doctest::Approx(260).epsilon(0.01));
}

TEST_CASE("frame: Gravity_Fill")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto frame = New<FrameLayout>();
    auto child = TV(50, 30);
    auto lp = New<FrameLayoutParams>();
    lp->Gravity = Gravity::Fill;
    frame->AddView(child.Get(), lp);
    root->AddView(frame.Get());
    LayoutPass(ctx, root.Get());
    CHECK(child->Width() == doctest::Approx(400).epsilon(0.01));
    CHECK(child->Height() == doctest::Approx(300).epsilon(0.01));
}

TEST_CASE("frame: Padding_OffsetsGravity")
{
    UIContext ctx;
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
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto frame = New<FrameLayout>();
    auto a = TV(100, 50);
    auto b = TV(80, 40);
    auto lpA = New<FrameLayoutParams>();
    lpA->Gravity = Gravity::None;
    auto lpB = New<FrameLayoutParams>();
    lpB->Gravity = Gravity::Center;
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
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);

    auto pane = New<FlexLayout>();
    pane->Direction = Orientation::Vertical;

    auto toolbar = TV(0, 0);
    {
        auto lp = New<FlexLayoutParams>();
        lp->Width = SizeSpec::Match();
        lp->Height = SizeSpec::Fixed(Unit::Px(30));
        pane->AddView(toolbar.Get(), lp);
    }

    auto frame = New<FrameLayout>();
    auto viewport = TV(256, 256); // measures small like ViewportView; Fill overrides it
    {
        auto vfp = New<FrameLayoutParams>();
        vfp->Gravity = Gravity::Fill;
        frame->AddView(viewport.Get(), vfp);
    }
    auto preview = TV(320, 204);
    {
        auto pfp = New<FrameLayoutParams>();
        pfp->Gravity = Gravity::Bottom | Gravity::Right;
        pfp->Margin = Thickness{12, 12, 12, 12};
        frame->AddView(preview.Get(), pfp);
    }
    {
        auto lp = New<FlexLayoutParams>();
        lp->Width = SizeSpec::Match();
        lp->Grow = 1.0f;
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
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto abs = New<AbsoluteLayout>();
    auto child = TV(50, 30);
    auto lp = New<AbsoluteLayoutParams>();
    lp->X = 100;
    lp->Y = 50;
    abs->AddView(child.Get(), lp);
    root->AddView(abs.Get());
    LayoutPass(ctx, root.Get());
    CHECK(child->Bounds.x == doctest::Approx(100));
    CHECK(child->Bounds.y == doctest::Approx(50));
}

TEST_CASE("absolute: DefaultPosition_AtOrigin")
{
    UIContext ctx;
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
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto abs = New<AbsoluteLayout>();
    abs->Padding = Thickness{10, 20, 10, 20};
    auto child = TV(50, 30);
    auto lp = New<AbsoluteLayoutParams>();
    lp->X = 5;
    lp->Y = 5;
    abs->AddView(child.Get(), lp);
    root->AddView(abs.Get());
    LayoutPass(ctx, root.Get());
    CHECK(child->Bounds.x == doctest::Approx(15));
    CHECK(child->Bounds.y == doctest::Approx(25));
}

TEST_CASE("absolute: ChildRetainsMeasuredSize")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto abs = New<AbsoluteLayout>();
    auto child = TV(80, 45);
    auto lp = New<AbsoluteLayoutParams>();
    lp->X = 50;
    lp->Y = 50;
    abs->AddView(child.Get(), lp);
    root->AddView(abs.Get());
    LayoutPass(ctx, root.Get());
    CHECK(child->Width() == doctest::Approx(80));
    CHECK(child->Height() == doctest::Approx(45));
}

// === FlowLayout ===

TEST_CASE("flow: Horizontal_NoWrap_SingleLine")
{
    UIContext ctx;
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
    UIContext ctx;
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
    UIContext ctx;
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
    UIContext ctx;
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
    UIContext ctx;
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
    UIContext ctx;
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
    UIContext ctx;
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

TEST_CASE("dock: Top_TakesFullWidthMeasuredHeight")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto dock = New<DockLayout>();
    auto top = TV(400, 50);
    dock->AddView(top.Get(), core::MakeRef<DockLayoutParams>(core::DefaultAllocator(), Dock::Top));
    root->AddView(dock.Get());
    LayoutPass(ctx, root.Get());
    CHECK(top->Bounds.x == doctest::Approx(0));
    CHECK(top->Bounds.y == doctest::Approx(0));
    CHECK(top->Width() == doctest::Approx(400).epsilon(0.01));
    CHECK(top->Height() == doctest::Approx(50).epsilon(0.01));
}

TEST_CASE("dock: Bottom_DocksToBottom")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto dock = New<DockLayout>();
    auto bottom = TV(400, 40);
    dock->AddView(bottom.Get(),
                  core::MakeRef<DockLayoutParams>(core::DefaultAllocator(), Dock::Bottom));
    root->AddView(dock.Get());
    LayoutPass(ctx, root.Get());
    CHECK(bottom->Bounds.y == doctest::Approx(260).epsilon(0.01));
    CHECK(bottom->Width() == doctest::Approx(400).epsilon(0.01));
}

TEST_CASE("dock: Left_TakesFullHeightMeasuredWidth")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto dock = New<DockLayout>();
    auto left = TV(80, 300);
    dock->AddView(left.Get(),
                  core::MakeRef<DockLayoutParams>(core::DefaultAllocator(), Dock::Left));
    root->AddView(dock.Get());
    LayoutPass(ctx, root.Get());
    CHECK(left->Bounds.x == doctest::Approx(0));
    CHECK(left->Width() == doctest::Approx(80).epsilon(0.01));
    CHECK(left->Height() == doctest::Approx(300).epsilon(0.01));
}

TEST_CASE("dock: Right_DocksToRight")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto dock = New<DockLayout>();
    auto right = TV(60, 300);
    dock->AddView(right.Get(),
                  core::MakeRef<DockLayoutParams>(core::DefaultAllocator(), Dock::Right));
    root->AddView(dock.Get());
    LayoutPass(ctx, root.Get());
    CHECK(right->Bounds.x == doctest::Approx(340).epsilon(0.01));
    CHECK(right->Width() == doctest::Approx(60).epsilon(0.01));
}

TEST_CASE("dock: Fill_TakesRemainingSpace")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto dock = New<DockLayout>();
    auto top = TV(400, 50);
    auto fill = TV(50, 30);
    dock->AddView(top.Get(), core::MakeRef<DockLayoutParams>(core::DefaultAllocator(), Dock::Top));
    dock->AddView(fill.Get(),
                  core::MakeRef<DockLayoutParams>(core::DefaultAllocator(), Dock::Fill));
    root->AddView(dock.Get());
    LayoutPass(ctx, root.Get());
    CHECK(fill->Bounds.y == doctest::Approx(50).epsilon(0.01));
    CHECK(fill->Width() == doctest::Approx(400).epsilon(0.01));
    CHECK(fill->Height() == doctest::Approx(250).epsilon(0.01));
}

TEST_CASE("dock: LastChildFill_False_DoesNotFill")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto dock = New<DockLayout>();
    dock->LastChildFill = false;
    auto top = TV(400, 50);
    auto last = TV(100, 40);
    dock->AddView(top.Get(), core::MakeRef<DockLayoutParams>(core::DefaultAllocator(), Dock::Top));
    dock->AddView(last.Get(),
                  core::MakeRef<DockLayoutParams>(core::DefaultAllocator(), Dock::Left));
    root->AddView(dock.Get());
    LayoutPass(ctx, root.Get());
    CHECK(last->Width() == doctest::Approx(100).epsilon(0.01));
}

TEST_CASE("dock: LastChildFill_True_FillsRemaining")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto dock = New<DockLayout>();
    dock->LastChildFill = true;
    auto top = TV(400, 50);
    auto last = TV(100, 40);
    dock->AddView(top.Get(), core::MakeRef<DockLayoutParams>(core::DefaultAllocator(), Dock::Top));
    dock->AddView(last.Get(),
                  core::MakeRef<DockLayoutParams>(core::DefaultAllocator(), Dock::Left));
    root->AddView(dock.Get());
    LayoutPass(ctx, root.Get());
    CHECK(last->Width() == doctest::Approx(400).epsilon(0.01));
    CHECK(last->Height() == doctest::Approx(250).epsilon(0.01));
}

TEST_CASE("dock: MultipleEdges_ShrinkRemaining")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto dock = New<DockLayout>();
    auto top = TV(400, 40);
    auto left = TV(60, 260);
    auto fill = TV(50, 30);
    dock->AddView(top.Get(), core::MakeRef<DockLayoutParams>(core::DefaultAllocator(), Dock::Top));
    dock->AddView(left.Get(),
                  core::MakeRef<DockLayoutParams>(core::DefaultAllocator(), Dock::Left));
    dock->AddView(fill.Get(),
                  core::MakeRef<DockLayoutParams>(core::DefaultAllocator(), Dock::Fill));
    root->AddView(dock.Get());
    LayoutPass(ctx, root.Get());
    CHECK(fill->Bounds.x == doctest::Approx(60).epsilon(0.01));
    CHECK(fill->Bounds.y == doctest::Approx(40).epsilon(0.01));
    CHECK(fill->Width() == doctest::Approx(340).epsilon(0.01));
    CHECK(fill->Height() == doctest::Approx(260).epsilon(0.01));
}

// === GridLayout ===

static core::RefPtr<GridLayoutParams> Cell(i32 row, i32 col, i32 rowSpan = 1, i32 colSpan = 1)
{
    auto lp = New<GridLayoutParams>();
    lp->Row = row;
    lp->Column = col;
    lp->RowSpan = rowSpan;
    lp->ColumnSpan = colSpan;
    return lp;
}

TEST_CASE("grid: FixedColumns_CorrectWidths")
{
    UIContext ctx;
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
    UIContext ctx;
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
    UIContext ctx;
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
    UIContext ctx;
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
    UIContext ctx;
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
    UIContext ctx;
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
    UIContext ctx;
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
    UIContext ctx;
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

static core::RefPtr<FlexLayoutParams> Growth(f32 grow)
{
    auto lp = New<FlexLayoutParams>();
    lp->Grow = grow;
    return lp;
}

TEST_CASE("flex: Row_ChildrenArrangedHorizontally")
{
    UIContext ctx;
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
    UIContext ctx;
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
    UIContext ctx;
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
    UIContext ctx;
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
    UIContext ctx;
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
    UIContext ctx;
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
    UIContext ctx;
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
    UIContext ctx;
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
    UIContext ctx;
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
    UIContext ctx;
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
    UIContext ctx;
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
    UIContext ctx;
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
    UIContext ctx;
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
    UIContext ctx;
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
    UIContext ctx;
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
    UIContext ctx;
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
    UIContext ctx;
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
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto abs = New<AbsoluteLayout>();
    auto a = TV(50, 30);
    auto b = TV(60, 40);
    auto lpa = New<AbsoluteLayoutParams>();
    lpa->X = 10;
    lpa->Y = 10;
    auto lpb = New<AbsoluteLayoutParams>();
    lpb->X = 200;
    lpb->Y = 150;
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
    UIContext ctx;
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
