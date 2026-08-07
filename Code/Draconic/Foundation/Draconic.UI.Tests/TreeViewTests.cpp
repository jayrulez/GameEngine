// Ported from Sedulous.UI.Tests/src/TreeViewTests.bf (faithful). SimpleTreeAdapter test double from
// TestHelpers.h; the borrowed adapter is declared before the TreeView so it outlives it. Beef property
// passthroughs -> methods (tv->FlatAdapter()/Selection()); HierarchicalState.CaptureState/ApplyState take
// a TreeView&. Logic only, no font.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.ui;
#include "TestHelpers.h"

using namespace draconic::ui;
using namespace draconic::ui::tests;
using namespace draconic::foundation;
namespace foundation = draconic::foundation;

static foundation::RefPtr<RootView> MakeRoot()
{
    return foundation::MakeRef<RootView>(foundation::DefaultAllocator());
}
static foundation::RefPtr<TreeView> MakeTree()
{
    return foundation::MakeRef<TreeView>(foundation::DefaultAllocator());
}

TEST_CASE("tree-view: SetAdapter_ShowsRootItems")
{
    UIContext ctx;
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

// Regression (editor shutdown segfault): SetAdapter(nullptr) must DETACH - it used to build a
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
    UIContext ctx;
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
