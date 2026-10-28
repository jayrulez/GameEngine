// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Ported from Sedulous.UI.Tests/src/TreeViewTests.bf (faithful). SimpleTreeAdapter test double from
// TestHelpers.h; the borrowed adapter is declared before the TreeView so it outlives it. Beef property
// passthroughs -> methods (tv->FlatAdapter()/Selection()); HierarchicalState.CaptureState/ApplyState take
// a TreeView&. Logic only, no font.
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
static core::RefPtr<TreeView> MakeTree()
{
    return core::MakeRef<TreeView>(core::DefaultAllocator());
}

TEST_CASE("tree-view: SetAdapter_ShowsRootItems")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 200, 300);
    SimpleTreeAdapter adapter;
    auto tv = MakeTree();
    tv->SetAdapter(&adapter);
    root->AddView(tv.Get());
    LayoutPass(ctx, root.Get());

    CHECK(tv->FlatAdapter() != nullptr);
    CHECK(tv->FlatAdapter()->ItemCount() == 3); // 3 roots
}

TEST_CASE("tree-view: ToggleExpand_ChangesCount")
{
    SimpleTreeAdapter adapter;
    auto tv = MakeTree();
    tv->SetAdapter(&adapter);

    CHECK(tv->FlatAdapter()->ItemCount() == 3);
    tv->ToggleExpand(0);                        // expand root 0
    CHECK(tv->FlatAdapter()->ItemCount() == 5); // 3 + 2 children
    tv->ToggleExpand(0);                        // collapse
    CHECK(tv->FlatAdapter()->ItemCount() == 3);
}

TEST_CASE("tree-view: ExpandNode_AddsChildren")
{
    SimpleTreeAdapter adapter;
    auto tv = MakeTree();
    tv->SetAdapter(&adapter);

    tv->FlatAdapter()->Expand(1); // root 1 has 1 child
    CHECK(tv->FlatAdapter()->ItemCount() == 4);
    CHECK(tv->FlatAdapter()->GetNodeId(2) == 20); // child of root 1
}

TEST_CASE("tree-view: CollapseNode_RemovesChildren")
{
    SimpleTreeAdapter adapter;
    auto tv = MakeTree();
    tv->SetAdapter(&adapter);

    tv->FlatAdapter()->Expand(0);
    tv->FlatAdapter()->Expand(1);
    CHECK(tv->FlatAdapter()->ItemCount() == 6);

    tv->FlatAdapter()->Collapse(0);
    CHECK(tv->FlatAdapter()->ItemCount() == 4);
}

TEST_CASE("tree-view: Selection")
{
    SimpleTreeAdapter adapter;
    auto tv = MakeTree();
    tv->SetAdapter(&adapter);

    tv->Selection().Select(0);
    CHECK(tv->Selection().IsSelected(0));
    CHECK(tv->Selection().FirstSelected() == 0);
}

TEST_CASE("tree-view: collapse ABOVE the selection keeps the same NODE selected (remap)")
{
    SimpleTreeAdapter adapter;
    auto tv = MakeTree();
    tv->SetAdapter(&adapter);

    tv->ToggleExpand(0); // expand root 0: flat = [root0, c, c, root1, root2]
    REQUIRE(tv->FlatAdapter()->ItemCount() == 5);
    const i32 root1Node = tv->FlatAdapter()->GetNodeId(3);
    tv->Selection().Select(3); // select root 1 by its CURRENT flat position

    tv->ToggleExpand(0); // collapse root 0 - every flat index above shifts by 2

    // The same NODE stays selected at its new position; the highlight must not jump to
    // whichever row inherited flat index 3.
    REQUIRE(tv->FlatAdapter()->ItemCount() == 3);
    const i32 newPosition = tv->FlatAdapter()->PositionOfNode(root1Node);
    CHECK(newPosition == 1);
    CHECK(tv->Selection().IsSelected(newPosition));
    CHECK_FALSE(tv->Selection().IsSelected(3));
}

TEST_CASE("tree-view: collapsing the selected node's parent DROPS the hidden selection")
{
    SimpleTreeAdapter adapter;
    auto tv = MakeTree();
    tv->SetAdapter(&adapter);

    tv->ToggleExpand(0); // expand root 0
    REQUIRE(tv->FlatAdapter()->ItemCount() == 5);
    tv->Selection().Select(1); // a CHILD of root 0

    tv->ToggleExpand(0); // collapse hides the selected node

    CHECK(tv->Selection().SelectedCount() == 0u); // gone, not remapped onto a wrong row
}

TEST_CASE("tree-view: HierarchicalState_CaptureRestore")
{
    SimpleTreeAdapter adapter;
    auto tv = MakeTree();
    tv->SetAdapter(&adapter);

    tv->FlatAdapter()->Expand(0);
    tv->FlatAdapter()->Expand(1);
    tv->Selection().Select(2);

    HierarchicalState state;
    state.CaptureState(*tv);

    tv->FlatAdapter()->Collapse(0);
    tv->FlatAdapter()->Collapse(1);
    tv->Selection().ClearSelection();
    CHECK(tv->FlatAdapter()->ItemCount() == 3);

    state.ApplyState(*tv);
    CHECK(tv->FlatAdapter()->IsExpanded(0));
    CHECK(tv->FlatAdapter()->IsExpanded(1));
    CHECK(tv->FlatAdapter()->ItemCount() == 6);
    CHECK(tv->Selection().IsSelected(2));
}

// Regression: calling SetAdapter again (rebuild - e.g. an editor hierarchy refreshing over a
// changed scene) must not UAF the previous FlattenedTreeAdapter: the internal list detaches
// from the old flat adapter before the reassignment destroys it.
TEST_CASE("tree-view: SetAdapter_Twice_RebuildsSafely")
{
    SimpleTreeAdapter adapter;
    auto tv = MakeRef<TreeView>(DefaultAllocator());
    tv->SetAdapter(&adapter);
    const i32 before = tv->FlatAdapter()->ItemCount();

    tv->SetAdapter(&adapter); // rebuild with the same source adapter
    REQUIRE(tv->FlatAdapter() != nullptr);
    CHECK(tv->FlatAdapter()->ItemCount() == before);

    SimpleTreeAdapter other;
    tv->SetAdapter(&other); // and with a different one
    REQUIRE(tv->FlatAdapter() != nullptr);
}

// Regression (editor shutdown segfault): SetAdapter(nullptr) must DETACH - it must not build a
// FlattenedTreeAdapter over the null source and dereference it. Owners call it from their
// destructors so a view can't outlive an adapter it doesn't own.
TEST_CASE("tree-view: SetAdapter_Null_Detaches")
{
    SimpleTreeAdapter adapter;
    auto tv = MakeRef<TreeView>(DefaultAllocator());
    tv->SetAdapter(&adapter);
    REQUIRE(tv->FlatAdapter() != nullptr);

    tv->SetAdapter(nullptr);
    CHECK(tv->FlatAdapter() == nullptr);

    // Reattaching afterwards works.
    tv->SetAdapter(&adapter);
    REQUIRE(tv->FlatAdapter() != nullptr);
    CHECK(tv->FlatAdapter()->ItemCount() > 0);
}

// The editor F2/Delete path end-to-end: click focuses the INTERNAL ListView (click-to-focus),
// an unhandled key bubbles ListView -> TreeView, whose OnKeyDown maps the flat selection to a
// nodeId and fires OnItemKeyDown.
TEST_CASE("tree-view: FocusedInternalList_KeyBubbles_To_OnItemKeyDown")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    ctx.AddRootView(root.Get());

    SimpleTreeAdapter adapter;
    auto tv = MakeTree();
    root->AddView(tv.Get());
    tv->SetAdapter(&adapter);
    tv->Selection().Select(0);

    ctx.GetFocusManager()->SetFocus(tv->InternalListView());

    i32 firedNode = -1;
    tv->OnItemKeyDown.Add(
        [&firedNode](i32 nodeId, KeyEventArgs& e)
        {
            firedNode = nodeId;
            e.Handled = true;
        });

    CHECK(ctx.GetInputManager()->ProcessKeyDown(KeyCode::F2, KeyModifiers::None, false));
    CHECK(firedNode == 0);
}

TEST_CASE("tree-view: a chevron's click zone scrolls sideways with its row")
{
    // Rows wider than the tree, so it scrolls sideways.
    class WideTreeAdapter final : public SimpleTreeAdapter
    {
    public:
        [[nodiscard]] core::RefPtr<View> CreateView(i32) override
        {
            return core::MakeRef<TestView>(core::DefaultAllocator(), 600.0f, 30.0f);
        }
    };
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 200, 300);
    WideTreeAdapter adapter;
    auto tv = MakeTree();
    tv->SetAdapter(&adapter);
    tv->InternalListView()->ScrollsHorizontally.SetValue(true);
    root->AddView(tv.Get());
    LayoutPass(ctx, root.Get());
    REQUIRE(tv->InternalListView()->MaxScrollX() > 0.0f);

    // Scrolled 8: root 0's chevron zone is [-8, 12) in the list, so 16 misses it and 2 hits it.
    tv->InternalListView()->ScrollByX(8.0f);
    LayoutPass(ctx, root.Get());
    tv->InternalListView()->OnItemClicked.Invoke(0, 1, 16.0f, 5.0f);
    CHECK(tv->FlatAdapter()->ItemCount() == 3);
    tv->InternalListView()->OnItemClicked.Invoke(0, 1, 2.0f, 5.0f);
    CHECK(tv->FlatAdapter()->ItemCount() == 5); // root 0 expanded
}

TEST_CASE("editable-label: its natural width is its text past the offset, not the box it fills")
{
    auto label = core::MakeRef<EditableLabel>(core::DefaultAllocator());
    label->TextOffsetX.SetValue(40.0f);
    // No font service here: the offset and the trailing room alone.
    CHECK(label->NaturalWidth(20.0f) == doctest::Approx(48.0f));
}
