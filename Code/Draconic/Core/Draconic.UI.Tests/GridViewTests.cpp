// Ported from Sedulous.UI.Tests/src/GridViewTests.bf (faithful). Beef get/set props -> methods
// (gv->SetAdapter / gv->ScrollY()); shared SimpleListAdapter test double from TestHelpers.h; the borrowed
// adapter is declared before the GridView so it outlives it. Logic only, no font.
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
static foundation::RefPtr<GridView> MakeGrid()
{
    return foundation::MakeRef<GridView>(foundation::DefaultAllocator());
}

TEST_CASE("grid-view: NoAdapter_NoViews")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 300, 300);
    auto gv = MakeGrid();
    root->AddView(gv.Get());
    LayoutPass(ctx, root.Get());

    // VisualChildCount = active views (0) + scrollbar (1).
    CHECK(gv->VisualChildCount() == 1u);
}

TEST_CASE("grid-view: IsFocusable")
{
    auto gv = MakeGrid();
    CHECK(gv->IsFocusable);
    CHECK(gv->IsTabStop);
}

TEST_CASE("grid-view: ColumnCalculation")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 300, 300);
    SimpleListAdapter adapter(50);
    auto gv = MakeGrid();
    gv->CellWidth.SetValue(60);
    gv->CellSpacing.SetValue(4);
    gv->SetAdapter(&adapter);
    root->AddView(gv.Get());
    LayoutPass(ctx, root.Get());

    // 300 / (60 + 4) = 4.68 -> 4 columns; items at different x map to positions.
    CHECK(gv->GetItemAtPoint(0, 0) == 0);
    CHECK(gv->GetItemAtPoint(64, 0) == 1);
}

TEST_CASE("grid-view: ScrollBy_ClampsBounds")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 300, 300);
    SimpleListAdapter adapter(200);
    auto gv = MakeGrid();
    gv->CellWidth.SetValue(60);
    gv->CellHeight.SetValue(60);
    gv->SetAdapter(&adapter);
    root->AddView(gv.Get());
    LayoutPass(ctx, root.Get());

    gv->ScrollBy(-1000);
    CHECK(gv->ScrollY() == 0);

    gv->ScrollBy(999999);
    CHECK(gv->ScrollY() == gv->MaxScrollY());
}

TEST_CASE("grid-view: Selection")
{
    SimpleListAdapter adapter(20);
    auto gv = MakeGrid();
    gv->SetAdapter(&adapter);

    gv->Selection.Select(5);
    CHECK(gv->Selection.IsSelected(5));
    CHECK(gv->Selection.SelectedCount() == 1u);
}

TEST_CASE("grid-view: DefaultValues")
{
    auto gv = MakeGrid();
    CHECK(gv->CellWidth.Value() == 60);
    CHECK(gv->CellHeight.Value() == 60);
    CHECK(gv->CellSpacing.Value() == 4);
    CHECK(gv->ScrollY() == 0);
}

TEST_CASE("grid-view: GetActiveView + OnItemKeyDown (item keys before navigation)")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 300, 300);
    SimpleListAdapter adapter(10);
    auto gv = MakeGrid();
    gv->SetAdapter(&adapter);
    root->AddView(gv.Get());
    LayoutPass(ctx, root.Get());

    // Visible positions have live views; far-offscreen ones don't.
    CHECK(gv->GetActiveView(0) != nullptr);
    CHECK(gv->GetActiveView(999) == nullptr);

    gv->Selection.Select(1);

    // An item-key handler sees the key FIRST and can consume it (F2-rename contract).
    i32 seenPosition = -1;
    gv->OnItemKeyDown.Add(
        [&](i32 position, KeyEventArgs& e)
        {
            seenPosition = position;
            if (e.Key == KeyCode::F2)
            {
                e.Handled = true;
            }
        });
    KeyEventArgs f2{};
    f2.Key = KeyCode::F2;
    gv->OnKeyDown(f2);
    CHECK(seenPosition == 1);
    CHECK(f2.Handled);
    CHECK(gv->Selection.FirstSelected() == 1); // navigation didn't run

    // Unconsumed keys fall through to grid navigation.
    KeyEventArgs right{};
    right.Key = KeyCode::Right;
    gv->OnKeyDown(right);
    CHECK(seenPosition == 1);                  // handler saw it (pre-navigation position)
    CHECK(gv->Selection.FirstSelected() == 2); // then navigation moved the selection
}
