// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// ListView tests, logic only (no font). The SimpleListAdapter test double lives in TestHelpers.h.
// The ListView borrows its adapter, so the adapter is declared before the ListView to outlive it.
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
static core::RefPtr<ListView> MakeList()
{
    return core::MakeRef<ListView>(core::DefaultAllocator());
}

TEST_CASE("list-view: NoAdapter_NoViews")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 200, 300);
    auto lv = MakeList();
    root->AddView(lv.Get());
    LayoutPass(ctx, root.Get());

    // VisualChildCount is the active views (0) plus the scrollbar (1).
    CHECK(lv->VisualChildCount() == 1u);
}

TEST_CASE("list-view: IsFocusable")
{
    auto lv = MakeList();
    CHECK(lv->IsFocusable);
    CHECK(lv->IsTabStop);
}

TEST_CASE("list-view: SetAdapter_CreatesVisibleViews")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 200, 300);
    SimpleListAdapter adapter(100);
    auto lv = MakeList();
    lv->ItemHeight.SetValue(30);
    lv->SetAdapter(&adapter);
    root->AddView(lv.Get());
    LayoutPass(ctx, root.Get());

    // About 10 visible items (300 / 30) plus the scrollbar.
    CHECK(lv->VisualChildCount() > 1u);
}

TEST_CASE("list-view: ScrollBy_ClampsBounds")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 200, 300);
    SimpleListAdapter adapter(100);
    auto lv = MakeList();
    lv->ItemHeight.SetValue(30);
    lv->SetAdapter(&adapter);
    root->AddView(lv.Get());
    LayoutPass(ctx, root.Get());

    lv->ScrollBy(-1000);
    CHECK(lv->ScrollY() == 0);

    lv->ScrollBy(999999);
    CHECK(lv->ScrollY() == lv->MaxScrollY());
}

TEST_CASE("list-view: GetItemAtY")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 200, 300);
    SimpleListAdapter adapter(100);
    auto lv = MakeList();
    lv->ItemHeight.SetValue(30);
    lv->SetAdapter(&adapter);
    root->AddView(lv.Get());
    LayoutPass(ctx, root.Get());

    CHECK(lv->GetItemAtY(0) == 0);
    CHECK(lv->GetItemAtY(31) == 1);
    CHECK(lv->GetItemAtY(60) == 2);
}

TEST_CASE("list-view: Selection_SingleMode")
{
    SimpleListAdapter adapter(10);
    auto lv = MakeList();
    lv->SetAdapter(&adapter);

    lv->Selection.Select(3);
    CHECK(lv->Selection.IsSelected(3));
    CHECK(lv->Selection.SelectedCount() == 1u);

    lv->Selection.Select(5);
    CHECK(!lv->Selection.IsSelected(3));
    CHECK(lv->Selection.IsSelected(5));
}

TEST_CASE("list-view: NotifyDataChanged_PrunesOutOfRangeSelection")
{
    SimpleListAdapter big(10);
    SimpleListAdapter small(4);
    auto lv = MakeList();
    lv->SetAdapter(&big);
    lv->Selection.Select(2);
    lv->Selection.Toggle(8); // Single mode replaces; use two steps to end selected on 8
    CHECK(lv->Selection.IsSelected(8));

    lv->SetAdapter(&small); // data set shrank under the selection
    lv->NotifyDataChanged();

    // Index 8 does not exist any more, so it is dropped rather than left to highlight whichever
    // row a later grow puts at position 8.
    CHECK_FALSE(lv->Selection.IsSelected(8));
    CHECK(lv->Selection.SelectedCount() == 0u);
}

TEST_CASE("list-view: AdapterObserver_OnDataSetChanged")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 200, 300);
    SimpleListAdapter adapter(10);
    auto lv = MakeList();
    lv->SetAdapter(&adapter);
    root->AddView(lv.Get());
    LayoutPass(ctx, root.Get());

    adapter.Count = 20;
    adapter.NotifyDataSetChanged();

    CHECK(lv->GetAdapter()->ItemCount() == 20);
}

namespace
{
    // Counts binds per position and tracks view identity, so a test can check that a range-changed
    // notify rebinds only the named items in place (the same view object, no recycle).
    class BindCountingAdapter : public ListAdapterBase
    {
    public:
        core::i32 Count = 0;
        core::Array<core::i32> Binds;
        core::Array<View*> BoundViews;
        explicit BindCountingAdapter(core::i32 count) : Count(count)
        {
            Binds.Resize(static_cast<core::usize>(count), 0);
            BoundViews.Resize(static_cast<core::usize>(count), nullptr);
        }
        [[nodiscard]] core::i32 ItemCount() const override { return Count; }
        [[nodiscard]] core::RefPtr<View> CreateView(core::i32) override
        {
            return core::MakeRef<TestView>(core::DefaultAllocator(), 100.0f, 30.0f);
        }
        void BindView(View* view, core::i32 position) override
        {
            Binds[static_cast<core::usize>(position)] += 1;
            BoundViews[static_cast<core::usize>(position)] = view;
        }
    };
}

TEST_CASE("list-view: NotifyRangeChanged rebinds only the named visible item, in place")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 200, 300);
    BindCountingAdapter adapter(100);
    auto lv = MakeList();
    lv->ItemHeight.SetValue(30);
    lv->SetAdapter(&adapter);
    root->AddView(lv.Get());
    LayoutPass(ctx, root.Get());

    const usize before = lv->VisualChildCount();
    REQUIRE(adapter.Binds[2] == 1); // visible, bound once by the layout pass
    View* boundView = adapter.BoundViews[2];
    REQUIRE(boundView != nullptr);

    adapter.NotifyRangeChanged(2, 1);
    CHECK(adapter.Binds[2] == 2);            // rebound...
    CHECK(adapter.BoundViews[2] == boundView); // ...into the same view (no recycle)
    CHECK(adapter.Binds[3] == 1);            // neighbors untouched
    CHECK(lv->VisualChildCount() == before); // no teardown

    // An off-screen position does nothing (nothing active to rebind).
    adapter.NotifyRangeChanged(90, 1);
    CHECK(adapter.Binds[90] == 0);
}

TEST_CASE("damage: visual-only invalidation redraws WITHOUT layout damage")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 200, 300);
    auto view = core::MakeRef<foundation::ui::tests::TestView>(core::DefaultAllocator(), 50.0f,
                                                               20.0f);
    root->AddView(view.Get()); // a tree mutation causes layout damage (the safe default)
    CHECK(ctx.NeedsLayout());
    CHECK(ctx.NeedsRedraw());

    // The host consumed the frame's layout damage.
    LayoutPass(ctx, root.Get());
    ctx.ClearLayoutDamage();
    CHECK(!ctx.NeedsLayout());

    // A visual-only change (hover tint, press state, focus ring): redraw, no relayout.
    view->InvalidateVisual();
    CHECK(!ctx.NeedsLayout());
    CHECK(ctx.NeedsRedraw());

    // A plain Invalidate stays the safe default: layout and redraw.
    view->Invalidate();
    CHECK(ctx.NeedsLayout());
}

TEST_CASE("damage: setting a button's text or a scroll bar's viewport to what it is asks for no layout")
{
    // Setting a property to the value it already has causes no layout damage, so a page that
    // syncs its toolbar each frame, or a scroll view that sets its bars from its own Layout, does
    // not lay out the whole window every frame. A real change still asks for layout.
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 200, 300);
    auto button = core::MakeRef<Button>(core::DefaultAllocator(), StringView(u8"Pin"));
    auto bar = core::MakeRef<ScrollBar>(core::DefaultAllocator());
    root->AddView(button.Get());
    root->AddView(bar.Get());
    bar->SetViewportSize(120.0f);
    LayoutPass(ctx, root.Get());
    ctx.ClearLayoutDamage();

    button->SetText(u8"Pin");
    bar->SetViewportSize(120.0f);
    bar->SetIsHorizontal(bar->IsHorizontal());
    CHECK(!ctx.NeedsLayout());

    button->SetText(u8"Unpin");
    CHECK(ctx.NeedsLayout());
    LayoutPass(ctx, root.Get());
    ctx.ClearLayoutDamage();
    bar->SetViewportSize(90.0f);
    CHECK(ctx.NeedsLayout());
}

// Shift with an arrow grows and shrinks one range from the anchor, so repeated presses keep
// extending the same range.
TEST_CASE("list-view: Shift+Down and Shift+Up extend one range from the anchor")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 200, 300);
    SimpleListAdapter adapter(20);
    auto lv = MakeList();
    lv->ItemHeight.SetValue(30);
    lv->Selection.Mode = SelectionMode::Multiple;
    lv->SetAdapter(&adapter);
    root->AddView(lv.Get());
    LayoutPass(ctx, root.Get());

    lv->Selection.Select(3);
    const auto press = [&](KeyCode key, bool shift)
    {
        KeyEventArgs e{};
        e.Key = key;
        e.Modifiers = shift ? KeyModifiers::Shift : KeyModifiers::None;
        lv->OnKeyDown(e);
    };
    press(KeyCode::Down, true);
    press(KeyCode::Down, true);
    press(KeyCode::Down, true);
    CHECK(lv->Selection.SelectedCount() == 4u); // 3..6
    CHECK(lv->Selection.IsSelected(3));
    CHECK(lv->Selection.IsSelected(6));
    press(KeyCode::Up, true);
    CHECK(lv->Selection.SelectedCount() == 3u); // shrinks to 3..5
    CHECK_FALSE(lv->Selection.IsSelected(6));
    press(KeyCode::Down, false); // a plain arrow moves from the caret
    CHECK(lv->Selection.SelectedCount() == 1u);
    CHECK(lv->Selection.IsSelected(6));
}

namespace
{
    /// Rows 500 wide: wider than the list, as a deep hierarchy's rows are.
    class WideListAdapter final : public ListAdapterBase
    {
    public:
        i32 Count = 0;
        f32 RowWidth = 500.0f;
        explicit WideListAdapter(i32 count) : Count(count) {}
        [[nodiscard]] i32 ItemCount() const override { return Count; }
        [[nodiscard]] core::RefPtr<View> CreateView(i32) override
        {
            return core::MakeRef<TestView>(core::DefaultAllocator(), RowWidth, 30.0f);
        }
        // Rows are recycled: the width is the bound data, as a real row's text is.
        void BindView(View* view, i32) override { static_cast<TestView*>(view)->DesiredWidth = RowWidth; }
    };
}

TEST_CASE("list-view: wide rows scroll sideways only when asked, Shift+wheel moves them")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 200, 300);
    WideListAdapter adapter(100);
    auto lv = MakeList();
    lv->ItemHeight.SetValue(30);
    lv->SetAdapter(&adapter);
    root->AddView(lv.Get());
    LayoutPass(ctx, root.Get());

    // Off by default: rows are cut at the list's width, and there is no sideways bar.
    const usize visibleRows = lv->VisualChildCount() - 1;
    CHECK(lv->MaxScrollX() == 0.0f);
    CHECK(lv->GetActiveView(0)->Width() < 200.0f);
    const f32 maxScrollYWithout = lv->MaxScrollY();

    // On: the rows lay out at their natural width, and the bar lies over the bottom.
    lv->ScrollsHorizontally.SetValue(true);
    LayoutPass(ctx, root.Get());
    CHECK(lv->VisualChildCount() == visibleRows + 2);
    CHECK(lv->ContentWidth() == doctest::Approx(500.0f));
    CHECK(lv->GetActiveView(0)->Width() == doctest::Approx(500.0f));
    CHECK(lv->MaxScrollX() > 300.0f);
    CHECK(lv->MaxScrollY() > maxScrollYWithout); // the last row scrolls clear of the bar

    // Shift+wheel scrolls sideways (not down); the rows follow.
    ctx.GetInputManager()->ProcessMouseWheel(50, 50, 0, -1, KeyModifiers::Shift);
    LayoutPass(ctx, root.Get());
    CHECK(lv->ScrollX() > 0.0f);
    CHECK(lv->ScrollY() == 0.0f);
    CHECK(lv->GetActiveView(0)->Bounds.x == doctest::Approx(-lv->ScrollX()));
    lv->ScrollByX(100000.0f);
    CHECK(lv->ScrollX() == lv->MaxScrollX());

    // Rows that fit: nothing to scroll, and the offset drops back.
    adapter.RowWidth = 50.0f;
    lv->NotifyDataChanged();
    LayoutPass(ctx, root.Get());
    CHECK(lv->MaxScrollX() == 0.0f);
    CHECK(lv->ScrollX() == 0.0f);
}
