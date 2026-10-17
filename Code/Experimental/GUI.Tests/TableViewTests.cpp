// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - TableModel + TableView tests: multi-column data, virtualized rows with a
// header, single selection (mouse + keyboard), and header-click reporting (for sorting).
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import experimental.gui;

using namespace experimental::gui;
namespace core = foundation::core;

namespace
{
    template <typename T>
    core::RefPtr<T> Make()
    {
        return core::MakeRef<T>(core::DefaultAllocator());
    }
    core::StringView SV(const char8_t* s) { return core::StringView(s); }

    // A 2-column model (Name, Age) with `count` rows.
    void FillPeople(TableModel& model, int count)
    {
        core::Array<core::String> cols;
        cols.PushBack(core::String(SV(u8"Name")));
        cols.PushBack(core::String(SV(u8"Age")));
        model.SetColumns(core::Move(cols));
        for (int i = 0; i < count; ++i)
        {
            core::Array<Variant> row;
            row.PushBack(Variant(SV(u8"Person")));
            row.PushBack(Variant(static_cast<core::i64>(20 + i)));
            model.AddRow(core::Move(row));
        }
    }

    core::RefPtr<TableView> MountTable(core::RefPtr<SceneRoot>& root, IModel* model)
    {
        root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
        root->SetSize(core::Float2{400.0f, 400.0f});
        auto table = Make<TableView>();
        table->SetSize(core::Float2{300.0f, 200.0f});
        table->SetRowHeight(24.0f);
        table->SetHeaderHeight(26.0f);
        root->AddChild(table.Get());
        table->SetModel(model);
        (void)root->UpdateLayout();
        return table;
    }
}

TEST_CASE("table-model: columns, rows, and typed cells")
{
    TableModel model;
    FillPeople(model, 3);

    CHECK(model.ColumnCount() == 2);
    CHECK(model.RowCount() == 3);
    CHECK(model.ColumnName(0) == SV(u8"Name"));
    CHECK(model.ColumnName(1) == SV(u8"Age"));
    CHECK(model.Data(MakeModelIndex(0, 0)).AsString() == SV(u8"Person"));
    CHECK(model.Data(MakeModelIndex(1, 1)).AsInt() == 21);
    CHECK(model.Data(MakeModelIndex(1, 1)).ToString() == SV(u8"21"));
    CHECK(model.Data(MakeModelIndex(9, 0)).IsEmpty()); // out of range
}

TEST_CASE("table-view: virtualizes rows over the body viewport")
{
    TableModel model;
    FillPeople(model, 1000);
    core::RefPtr<SceneRoot> root;
    auto table = MountTable(root, &model);

    // body = 200 - 26 header = 174: rows 0 to 7 in view and row 8 past the edge, not 1000.
    CHECK(table->VisibleRowCount() == 9);
    CHECK(table->GetModel() == &model);
}

TEST_CASE("table-view: clicking a row selects it")
{
    TableModel model;
    FillPeople(model, 5); // fits (no scrollbar), body width = 300, 2 cols of 150
    core::RefPtr<SceneRoot> root;
    auto table = MountTable(root, &model);
    EventDispatcher* d = root->GetEventDispatcher();

    int selected = -1;
    table->SetOnSelectionChanged([&](ModelIndex i) { selected = i.Row; });

    // Row 1 spans y in [26 + 24, 26 + 48) = [50, 74); cells are hit-transparent -> row gets it.
    d->InjectMouseDown(core::Float2{60.0f, 60.0f}, MouseButton::Left);
    d->InjectMouseUp(core::Float2{60.0f, 60.0f}, MouseButton::Left);
    CHECK(table->GetSelectedItem() == 1);
    CHECK(selected == 1);
}

TEST_CASE("table-view: keyboard navigation moves the selection")
{
    TableModel model;
    FillPeople(model, 8);
    core::RefPtr<SceneRoot> root;
    auto table = MountTable(root, &model);
    EventDispatcher* d = root->GetEventDispatcher();
    table->RequestFocus();

    const auto key = [](KeyCode k) { return static_cast<core::u32>(k); };
    d->InjectKeyDown(key(KeyCode::Down)); // -> row 0
    d->InjectKeyDown(key(KeyCode::Down)); // -> row 1
    CHECK(table->GetSelectedItem() == 1);
    d->InjectKeyDown(key(KeyCode::End));
    CHECK(table->GetSelectedItem() == 7);
}

TEST_CASE("table-view: clicking a column header reports its index")
{
    TableModel model;
    FillPeople(model, 3);
    core::RefPtr<SceneRoot> root;
    auto table = MountTable(root, &model);
    EventDispatcher* d = root->GetEventDispatcher();

    int clickedColumn = -1;
    table->SetOnColumnHeaderClicked([&](core::usize col)
                                    { clickedColumn = static_cast<int>(col); });

    // Header spans y in [0, 26). Two columns of 150 -> column 1 header at x in [150, 300).
    d->InjectMouseDown(core::Float2{200.0f, 13.0f}, MouseButton::Left);
    d->InjectMouseUp(core::Float2{200.0f, 13.0f}, MouseButton::Left);
    CHECK(clickedColumn == 1);
}

TEST_CASE("table-view: reacts to model updates and rebuilds the header")
{
    TableModel model;
    FillPeople(model, 4);
    core::RefPtr<SceneRoot> root;
    auto table = MountTable(root, &model);

    table->SetSelectedItem(3);
    CHECK(table->GetSelectedItem() == 3);

    model.Clear(); // notifies -> selection cleared, no rows
    (void)root->UpdateLayout();
    CHECK(table->GetSelectedItem() == -1);
    CHECK(table->VisibleRowCount() == 0);
}

TEST_CASE("table-view: sorting from a header click survives the header it rebuilds")
{
    // The sandbox's table (where it crashed): a header click sorts through a proxy, the proxy's
    // update rebuilds the header, and neither the click nor the next mouse move may reach the
    // freed header cell.
    TableModel model;
    FillPeople(model, 5);
    SortingProxyModel proxy(&model);
    core::RefPtr<SceneRoot> root;
    auto table = MountTable(root, &proxy);
    table->SetOnColumnHeaderClicked([&](core::usize col) { proxy.ToggleSort(col); });
    EventDispatcher* d = root->GetEventDispatcher();
    for (int i = 0; i < 3; ++i) // ascending, descending, ascending
    {
        d->InjectMouseMove(core::Float2{200.0f, 13.0f});
        d->InjectMouseDown(core::Float2{200.0f, 13.0f}, MouseButton::Left);
        d->InjectMouseUp(core::Float2{200.0f, 13.0f}, MouseButton::Left);
        root->GetContext().Update(core::Duration::FromSeconds(0.016));
        // The rebuild freed the hovered header cell: the dispatcher let go of it (what it still
        // holds is in the tree), so moving on leaves nothing freed.
        REQUIRE(d->GetOverNode() != nullptr);
        CHECK(d->GetOverNode()->GetRootNode() == root.Get());
    }
    d->InjectMouseMove(core::Float2{200.0f, 60.0f});
    CHECK(d->GetOverNode() != nullptr);
    CHECK(proxy.GetSortColumn() == 1);
    CHECK(proxy.GetSortOrder() == SortOrder::Ascending);
}

TEST_CASE("table-view: columns are pixels, percentages or auto shares, none under the minimum")
{
    TableModel model;
    core::Array<core::String> cols;
    const char8_t* const names[] = {u8"A", u8"B", u8"C", u8"D"};
    for (const char8_t* name : names)
        cols.PushBack(core::String(SV(name)));
    model.SetColumns(core::Move(cols));
    core::RefPtr<SceneRoot> root;
    auto table = MountTable(root, &model);
    table->SetColumnWidthsText(core::String(SV(u8"60px 20% auto")));
    (void)root->UpdateLayout();
    // 300 wide: A 60, B 60 (20%), C and D share the 180 left.
    CHECK(table->GetColumnWidth(0) == doctest::Approx(60.0f));
    CHECK(table->GetColumnWidth(1) == doctest::Approx(60.0f));
    CHECK(table->GetColumnWidth(2) == doctest::Approx(90.0f));
    CHECK(table->GetColumnWidth(3) == doctest::Approx(90.0f));
    CHECK(table->GetColumnWidthsText() == SV(u8"60px 20% auto"));
    CHECK(table->GetScrollRange().x == doctest::Approx(0.0f));

    // Too many pixels: the autos keep the minimum, and the table scrolls across.
    table->SetColumnWidth(0, LayoutLength::Px(250.0f));
    (void)root->UpdateLayout();
    CHECK(table->GetColumnWidth(2) == doctest::Approx(table->GetMinColumnWidth()));
    CHECK(table->GetColumnsWidth() == doctest::Approx(250.0f + 60.0f + 80.0f));
    CHECK(table->GetScrollRange().x == doctest::Approx(90.0f));
    // The header scrolls across with the rows (and stays at the top as they scroll down).
    table->ScrollTo(core::Float2{90.0f, 0.0f}, ScrollBehavior::Instant);
    (void)root->UpdateLayout();
    CHECK(table->GetHeader()->GetSize().x == doctest::Approx(390.0f));
}

TEST_CASE("table-view: dragging a header cell's right edge resizes its column, without sorting")
{
    TableModel model;
    FillPeople(model, 3);
    SortingProxyModel proxy(&model);
    core::RefPtr<SceneRoot> root;
    auto table = MountTable(root, &proxy);
    EventDispatcher* d = root->GetEventDispatcher();
    CHECK(table->GetColumnWidth(0) == doctest::Approx(150.0f));

    // Column 0's edge is at x 150: press just inside it and drag 40 right.
    d->InjectMouseMove(core::Float2{148.0f, 13.0f});
    CHECK(d->GetCursor() == Cursor::EwResize);
    d->InjectMouseDown(core::Float2{148.0f, 13.0f}, MouseButton::Left);
    d->InjectMouseMove(core::Float2{188.0f, 13.0f});
    (void)root->UpdateLayout();
    d->InjectMouseUp(core::Float2{188.0f, 13.0f}, MouseButton::Left);
    (void)root->UpdateLayout();
    CHECK(table->GetColumnWidth(0) == doctest::Approx(190.0f));
    CHECK(table->GetColumnWidth(1) == doctest::Approx(110.0f)); // the auto one takes what is left
    CHECK(proxy.GetSortColumn() == -1);                          // a resize is no click
    // Dragged past the minimum, it stops there.
    d->InjectMouseDown(core::Float2{188.0f, 13.0f}, MouseButton::Left);
    d->InjectMouseMove(core::Float2{0.0f, 13.0f});
    d->InjectMouseUp(core::Float2{0.0f, 13.0f}, MouseButton::Left);
    (void)root->UpdateLayout();
    CHECK(table->GetColumnWidth(0) == doctest::Approx(table->GetMinColumnWidth()));
}

TEST_CASE("table-view: a header click sorts where the model can, and the cell shows which way")
{
    TableModel model;
    FillPeople(model, 3);
    SortingProxyModel proxy(&model);
    core::RefPtr<SceneRoot> root;
    auto table = MountTable(root, &proxy);
    EventDispatcher* d = root->GetEventDispatcher();
    CHECK(table->GetSortColumn() == -1);
    d->InjectMouseDown(core::Float2{200.0f, 13.0f}, MouseButton::Left);
    d->InjectMouseUp(core::Float2{200.0f, 13.0f}, MouseButton::Left);
    CHECK(table->GetSortColumn() == 1);
    CHECK(table->GetSortOrder() == SortOrder::Ascending);
    d->InjectMouseDown(core::Float2{200.0f, 13.0f}, MouseButton::Left);
    d->InjectMouseUp(core::Float2{200.0f, 13.0f}, MouseButton::Left);
    CHECK(table->GetSortOrder() == SortOrder::Descending);
    CHECK(proxy.GetSortOrder() == SortOrder::Descending);
    CHECK(proxy.Data(MakeModelIndex(0, 1)).AsInt() == 22);

    // A table alone cannot sort: no indicator.
    TableModel plain;
    FillPeople(plain, 2);
    core::RefPtr<SceneRoot> root2;
    auto table2 = MountTable(root2, &plain);
    root2->GetEventDispatcher()->InjectMouseDown(core::Float2{200.0f, 13.0f}, MouseButton::Left);
    root2->GetEventDispatcher()->InjectMouseUp(core::Float2{200.0f, 13.0f}, MouseButton::Left);
    CHECK(table2->GetSortColumn() == -1);
}
