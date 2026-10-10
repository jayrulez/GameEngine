// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Ported from Sedulous.UI.Tests/src/ListViewTests.bf (faithful). Beef get/set props -> methods
// (lv->SetAdapter / lv->ScrollY()); SimpleListAdapter test double lives in TestHelpers.h. The adapter is
// borrowed (pattern-B) - declared before the ListView so it outlives it. Logic only, no font.
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

    // VisualChildCount = active views (0) + scrollbar (1).
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

    // ~10 visible items (300 / 30) + scrollbar.
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

    // Index 8 no longer exists - it must be dropped, never left to silently highlight
    // whichever row a future grow puts at position 8.
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
    // Counts binds per position and tracks view identity, so a range-changed notify can be
    // pinned as an IN-PLACE rebind (same view object, no recycle) of only the named items.
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
    CHECK(adapter.BoundViews[2] == boundView); // ...into the SAME view (no recycle)
    CHECK(adapter.Binds[3] == 1);            // neighbors untouched
    CHECK(lv->VisualChildCount() == before); // no teardown

    // An off-screen position no-ops (nothing active to rebind).
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
    root->AddView(view.Get()); // tree mutation -> layout damage (the safe default)
    CHECK(ctx.NeedsLayout());
    CHECK(ctx.NeedsRedraw());

    // The host consumed the frame's layout damage.
    LayoutPass(ctx, root.Get());
    ctx.ClearLayoutDamage();
    CHECK(!ctx.NeedsLayout());

    // Visual-only producer (hover tint / press state / focus ring): redraw, no relayout.
    view->InvalidateVisual();
    CHECK(!ctx.NeedsLayout());
    CHECK(ctx.NeedsRedraw());

    // A plain Invalidate stays the safe default: layout + redraw.
    view->Invalidate();
    CHECK(ctx.NeedsLayout());
}

// Shift+arrow grows and shrinks ONE range from the anchor; before, each press re-ranged from an
// arbitrary selected row, so the selection collapsed to two rows (Sedulous 5482ab11).
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
