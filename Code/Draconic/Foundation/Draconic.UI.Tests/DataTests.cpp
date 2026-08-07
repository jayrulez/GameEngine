// Ported from Sedulous.UI.Tests/src/DataTests.bf - ViewRecycler + SelectionModel + FlattenedTreeAdapter.
// Beef `SimpleListAdapter : ListAdapterBase` with CreateView returning a raw owned View -> CreateView
// returns RefPtr<View>; recycle/acquire move refs (RAII, no `delete`); `===` ref-equality -> pointer ==.
// SimpleListAdapter / SimpleTreeAdapter test doubles live in TestHelpers.h.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.ui;
#include "TestHelpers.h"

using namespace draconic::ui;
using namespace draconic::ui::tests;
using namespace draconic::foundation;
namespace foundation = draconic::foundation;

// SimpleListAdapter test double lives in TestHelpers.h (shared with ListViewTests).

// === ViewRecycler ===

TEST_CASE("data: ViewRecycler_AcquireReturnsNull_WhenEmpty")
{
    ViewRecycler recycler;
    CHECK(!recycler.Acquire(0));
}

TEST_CASE("data: ViewRecycler_RecycleAndAcquire_ReusesView")
{
    ViewRecycler recycler;
    auto view = foundation::MakeRef<TestView>(foundation::DefaultAllocator());
    View* raw = view.Get();
    recycler.Recycle(Move(view), 0);
    auto reused = recycler.Acquire(0);
    CHECK(reused.Get() == raw);
    CHECK(recycler.ReusedCount() == 1);
    CHECK(recycler.RecycledCount() == 1);
}

TEST_CASE("data: ViewRecycler_GetOrCreate_CreatesWhenEmpty")
{
    ViewRecycler recycler;
    SimpleListAdapter adapter(5);
    auto view = recycler.GetOrCreate(adapter, 0);
    CHECK(view);
    CHECK(recycler.CreatedCount() == 1);
}

TEST_CASE("data: ViewRecycler_DiagnosticCounters")
{
    ViewRecycler recycler;
    SimpleListAdapter adapter(5);
    auto v1 = recycler.GetOrCreate(adapter, 0);
    CHECK(recycler.CreatedCount() == 1);
    View* raw1 = v1.Get();
    recycler.Recycle(Move(v1), 0);
    CHECK(recycler.RecycledCount() == 1);
    auto v2 = recycler.GetOrCreate(adapter, 1);
    CHECK(recycler.ReusedCount() == 1);
    CHECK(v2.Get() == raw1);
}

// === SelectionModel ===

TEST_CASE("data: SelectionModel_SingleMode_ReplacesSelection")
{
    SelectionModel sel;
    sel.Mode = SelectionMode::Single;
    sel.Select(0);
    sel.Select(1);
    CHECK(!sel.IsSelected(0));
    CHECK(sel.IsSelected(1));
    CHECK(sel.SelectedCount() == 1u);
}

// DELIBERATE DEVIATION from the Sedulous port (which accumulated here): Select() is a plain
// click and replaces the selection in EVERY mode - Toggle (Ctrl) and SelectRange (Shift) are
// the extend paths. Accumulating plain clicks grew multi-select lists forever (user-reported
// in the asset browser).
TEST_CASE("data: SelectionModel_MultipleMode_PlainSelectReplaces")
{
    SelectionModel sel;
    sel.Mode = SelectionMode::Multiple;
    sel.Select(0);
    sel.Select(1);
    sel.Select(2);
    CHECK(!sel.IsSelected(0));
    CHECK(!sel.IsSelected(1));
    CHECK(sel.IsSelected(2));
    CHECK(sel.SelectedCount() == 1u);

    // Extending still works through Toggle/SelectRange.
    sel.Toggle(0);
    CHECK(sel.SelectedCount() == 2u);
    sel.SelectRange(0, 3);
    CHECK(sel.SelectedCount() == 4u);
    sel.Select(1); // plain click collapses back to one
    CHECK(sel.SelectedCount() == 1u);
    CHECK(sel.IsSelected(1));
}

TEST_CASE("data: SelectionModel_Toggle")
{
    SelectionModel sel;
    sel.Mode = SelectionMode::Multiple;
    sel.Select(0);
    sel.Toggle(0);
    CHECK(!sel.IsSelected(0));
    sel.Toggle(0);
    CHECK(sel.IsSelected(0));
}

TEST_CASE("data: SelectionModel_SelectRange")
{
    SelectionModel sel;
    sel.Mode = SelectionMode::Multiple;
    sel.SelectRange(2, 5);
    CHECK(sel.SelectedCount() == 4u);
    CHECK(sel.IsSelected(2));
    CHECK(sel.IsSelected(3));
    CHECK(sel.IsSelected(4));
    CHECK(sel.IsSelected(5));
}

TEST_CASE("data: SelectionModel_ClearSelection")
{
    SelectionModel sel;
    sel.Mode = SelectionMode::Multiple;
    sel.Select(0);
    sel.Toggle(1);
    sel.ClearSelection();
    CHECK(sel.SelectedCount() == 0u);
}

TEST_CASE("data: SelectionModel_ShiftIndices_Insert")
{
    SelectionModel sel;
    sel.Mode = SelectionMode::Multiple;
    sel.Select(2);
    sel.Toggle(4);
    sel.ShiftIndices(3, 1);   // insert at 3
    CHECK(sel.IsSelected(2)); // unchanged
    CHECK(sel.IsSelected(5)); // shifted from 4
}

TEST_CASE("data: SelectionModel_NoneMode_Ignores")
{
    SelectionModel sel;
    sel.Mode = SelectionMode::None;
    sel.Select(0);
    CHECK(sel.SelectedCount() == 0u);
}

// === FlattenedTreeAdapter ===

TEST_CASE("data: FlattenedTreeAdapter_InitialCount_IsRootCount")
{
    SimpleTreeAdapter tree;
    FlattenedTreeAdapter flat(&tree);
    CHECK(flat.ItemCount() == 3); // 3 roots, none expanded
}

TEST_CASE("data: FlattenedTreeAdapter_ExpandRoot_IncludesChildren")
{
    SimpleTreeAdapter tree;
    FlattenedTreeAdapter flat(&tree);
    flat.Expand(0);               // root 0 has 2 children
    CHECK(flat.ItemCount() == 5); // 3 roots + 2 children
}

TEST_CASE("data: FlattenedTreeAdapter_CollapseRoot_RemovesChildren")
{
    SimpleTreeAdapter tree;
    FlattenedTreeAdapter flat(&tree);
    flat.Expand(0);
    CHECK(flat.ItemCount() == 5);
    flat.Collapse(0);
    CHECK(flat.ItemCount() == 3);
}

TEST_CASE("data: FlattenedTreeAdapter_ToggleExpand")
{
    SimpleTreeAdapter tree;
    FlattenedTreeAdapter flat(&tree);
    flat.ToggleExpand(0);
    CHECK(flat.IsExpanded(0));
    CHECK(flat.ItemCount() == 5);
    flat.ToggleExpand(0);
    CHECK(!flat.IsExpanded(0));
    CHECK(flat.ItemCount() == 3);
}

TEST_CASE("data: FlattenedTreeAdapter_GetNodeId_GetDepth")
{
    SimpleTreeAdapter tree;
    FlattenedTreeAdapter flat(&tree);
    flat.Expand(0);
    // Visible: 0, 10, 11, 1, 2
    CHECK(flat.GetNodeId(0) == 0);
    CHECK(flat.GetNodeId(1) == 10);
    CHECK(flat.GetNodeId(2) == 11);
    CHECK(flat.GetNodeId(3) == 1);
    CHECK(flat.GetDepth(0) == 0);
    CHECK(flat.GetDepth(1) == 1);
    CHECK(flat.GetDepth(2) == 1);
    CHECK(flat.GetDepth(3) == 0);
}

TEST_CASE("data: FlattenedTreeAdapter_GetSetExpandedNodes")
{
    SimpleTreeAdapter tree;
    FlattenedTreeAdapter flat(&tree);
    flat.Expand(0);
    flat.Expand(1);

    HashSet<i32> saved;
    flat.GetExpandedNodes(saved);
    CHECK(saved.Contains(0));
    CHECK(saved.Contains(1));

    flat.Collapse(0);
    flat.Collapse(1);
    CHECK(flat.ItemCount() == 3);

    flat.SetExpandedNodes(saved);
    CHECK(flat.IsExpanded(0));
    CHECK(flat.IsExpanded(1));
    CHECK(flat.ItemCount() == 6); // 3 roots + 2 children of 0 + 1 child of 1
}
