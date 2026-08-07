// Draconic GUI - TableModel + TableView tests: multi-column data, virtualized rows with a
// header, single selection (mouse + keyboard), and header-click reporting (for sorting).
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.gui;

using namespace draconic::gui;
namespace foundation = draconic::foundation;

namespace
{
    template <typename T>
    foundation::RefPtr<T> Make()
    {
        return foundation::MakeRef<T>(foundation::DefaultAllocator());
    }
    foundation::StringView SV(const char8_t* s) { return foundation::StringView(s); }

    // A 2-column model (Name, Age) with `count` rows.
    void FillPeople(TableModel& model, int count)
    {
        foundation::Array<foundation::String> cols;
        cols.PushBack(foundation::String(SV(u8"Name")));
        cols.PushBack(foundation::String(SV(u8"Age")));
        model.SetColumns(foundation::Move(cols));
        for (int i = 0; i < count; ++i)
        {
            foundation::Array<Variant> row;
            row.PushBack(Variant(SV(u8"Person")));
            row.PushBack(Variant(static_cast<foundation::i64>(20 + i)));
            model.AddRow(foundation::Move(row));
        }
    }

    foundation::RefPtr<TableView> MountTable(foundation::RefPtr<SceneNode>& root, IModel* model)
    {
        root = Make<SceneNode>();
        root->SetSize(foundation::Float2{400.0f, 400.0f});
        auto table = Make<TableView>();
        table->SetSize(foundation::Float2{300.0f, 200.0f});
        table->SetRowHeight(24.0f);
        table->SetHeaderHeight(26.0f);
        root->AddChild(table.Get());
        table->SetModel(model);
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
    foundation::RefPtr<SceneNode> root;
    auto table = MountTable(root, &model);

    // body = 200 - 26 header = 174; 174/24 + 2 buffer = 9 realized, not 1000.
    CHECK(table->VisibleRowCount() == 9);
    CHECK(table->GetModel() == &model);
}

TEST_CASE("table-view: clicking a row selects it")
{
    TableModel model;
    FillPeople(model, 5); // fits (no scrollbar), body width = 300, 2 cols of 150
    foundation::RefPtr<SceneNode> root;
    auto table = MountTable(root, &model);
    EventDispatcher* d = root->GetEventDispatcher();

    int selected = -1;
    table->SetOnSelectionChanged([&](ModelIndex i) { selected = i.Row; });

    // Row 1 spans y in [26 + 24, 26 + 48) = [50, 74); cells are hit-transparent -> row gets it.
    d->InjectMouseDown(foundation::Float2{60.0f, 60.0f}, MouseButton::Left);
    d->InjectMouseUp(foundation::Float2{60.0f, 60.0f}, MouseButton::Left);
    CHECK(table->GetSelectedRow() == 1);
    CHECK(selected == 1);
}

TEST_CASE("table-view: keyboard navigation moves the selection")
{
    TableModel model;
    FillPeople(model, 8);
    foundation::RefPtr<SceneNode> root;
    auto table = MountTable(root, &model);
    EventDispatcher* d = root->GetEventDispatcher();
    table->RequestFocus();

    const auto key = [](KeyCode k) { return static_cast<foundation::u32>(k); };
    d->InjectKeyDown(key(KeyCode::Down)); // -> row 0
    d->InjectKeyDown(key(KeyCode::Down)); // -> row 1
    CHECK(table->GetSelectedRow() == 1);
    d->InjectKeyDown(key(KeyCode::End));
    CHECK(table->GetSelectedRow() == 7);
}

TEST_CASE("table-view: clicking a column header reports its index")
{
    TableModel model;
    FillPeople(model, 3);
    foundation::RefPtr<SceneNode> root;
    auto table = MountTable(root, &model);
    EventDispatcher* d = root->GetEventDispatcher();

    int clickedColumn = -1;
    table->SetOnColumnHeaderClicked([&](foundation::usize col)
                                    { clickedColumn = static_cast<int>(col); });

    // Header spans y in [0, 26). Two columns of 150 -> column 1 header at x in [150, 300).
    d->InjectMouseDown(foundation::Float2{200.0f, 13.0f}, MouseButton::Left);
    d->InjectMouseUp(foundation::Float2{200.0f, 13.0f}, MouseButton::Left);
    CHECK(clickedColumn == 1);
}

TEST_CASE("table-view: reacts to model updates and rebuilds the header")
{
    TableModel model;
    FillPeople(model, 4);
    foundation::RefPtr<SceneNode> root;
    auto table = MountTable(root, &model);

    table->SetSelectedRow(3);
    CHECK(table->GetSelectedRow() == 3);

    model.Clear(); // notifies -> selection cleared, no rows
    CHECK(table->GetSelectedRow() == -1);
    CHECK(table->VisibleRowCount() == 0);
}
