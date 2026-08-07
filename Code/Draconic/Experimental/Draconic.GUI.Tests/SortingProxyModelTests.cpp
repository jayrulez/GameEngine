// Draconic GUI - SortingProxyModel tests: reorder a source model's rows by a column (numeric by
// value, strings lexically), toggle asc/desc, map proxy rows to source, and re-sort + notify on
// source changes. Also the TableView header-click -> ToggleSort wiring.
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

    struct CountingClient : public IModelClient
    {
        int updates = 0;
        void OnModelUpdated() override { ++updates; }
    };

    // Rows: (name, value) = (b,2) (a,10) (c,1).
    void FillBAC(TableModel& model)
    {
        foundation::Array<foundation::String> cols;
        cols.PushBack(foundation::String(SV(u8"Name")));
        cols.PushBack(foundation::String(SV(u8"Value")));
        model.SetColumns(foundation::Move(cols));
        const char8_t* names[3] = {u8"b", u8"a", u8"c"};
        const foundation::i64 values[3] = {2, 10, 1};
        for (int i = 0; i < 3; ++i)
        {
            foundation::Array<Variant> row;
            row.PushBack(Variant(foundation::StringView(names[i])));
            row.PushBack(Variant(values[i]));
            model.AddRow(foundation::Move(row));
        }
    }
}

TEST_CASE("sorting-proxy: numeric column sorts by value, ascending and descending")
{
    TableModel source;
    FillBAC(source);
    SortingProxyModel proxy(&source);

    proxy.SortBy(1, SortOrder::Ascending); // values 2,10,1 -> 1,2,10
    CHECK(proxy.RowCount() == 3);
    CHECK(proxy.Data(MakeModelIndex(0, 1)).AsInt() == 1);
    CHECK(proxy.Data(MakeModelIndex(1, 1)).AsInt() == 2);
    CHECK(proxy.Data(MakeModelIndex(2, 1)).AsInt() == 10);
    CHECK(proxy.Data(MakeModelIndex(0, 0)).AsString() == SV(u8"c")); // row with value 1
    CHECK(proxy.SourceRow(0) == 2);

    proxy.SortBy(1, SortOrder::Descending); // 10,2,1
    CHECK(proxy.Data(MakeModelIndex(0, 1)).AsInt() == 10);
    CHECK(proxy.Data(MakeModelIndex(2, 1)).AsInt() == 1);
}

TEST_CASE("sorting-proxy: string column sorts lexically")
{
    TableModel source;
    FillBAC(source);
    SortingProxyModel proxy(&source);

    proxy.SortBy(0, SortOrder::Ascending); // b,a,c -> a,b,c
    CHECK(proxy.Data(MakeModelIndex(0, 0)).AsString() == SV(u8"a"));
    CHECK(proxy.Data(MakeModelIndex(1, 0)).AsString() == SV(u8"b"));
    CHECK(proxy.Data(MakeModelIndex(2, 0)).AsString() == SV(u8"c"));
}

TEST_CASE("sorting-proxy: ToggleSort cycles order and switches columns")
{
    TableModel source;
    FillBAC(source);
    SortingProxyModel proxy(&source);

    proxy.ToggleSort(1);
    CHECK(proxy.GetSortColumn() == 1);
    CHECK(proxy.GetSortOrder() == SortOrder::Ascending);
    proxy.ToggleSort(1); // same column -> descending
    CHECK(proxy.GetSortOrder() == SortOrder::Descending);
    proxy.ToggleSort(0); // new column -> ascending
    CHECK(proxy.GetSortColumn() == 0);
    CHECK(proxy.GetSortOrder() == SortOrder::Ascending);
}

TEST_CASE("sorting-proxy: a source change re-sorts and notifies the proxy's clients")
{
    TableModel source;
    FillBAC(source);
    SortingProxyModel proxy(&source);
    proxy.SortBy(1, SortOrder::Ascending);

    CountingClient client;
    proxy.AddClient(&client);

    foundation::Array<Variant> row; // value 0 -> should sort to the front
    row.PushBack(Variant(SV(u8"z")));
    row.PushBack(Variant(static_cast<foundation::i64>(0)));
    source.AddRow(foundation::Move(row));

    CHECK(client.updates == 1); // source change propagated
    CHECK(proxy.RowCount() == 4);
    CHECK(proxy.Data(MakeModelIndex(0, 1)).AsInt() == 0); // re-sorted with the new row first
}

TEST_CASE("sorting-proxy: TableView header click wired to ToggleSort re-sorts the view")
{
    TableModel source;
    FillBAC(source);
    SortingProxyModel proxy(&source);

    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{400.0f, 400.0f});
    auto table = Make<TableView>();
    table->SetSize(foundation::Float2{300.0f, 200.0f});
    table->SetHeaderHeight(26.0f);
    root->AddChild(table.Get());
    table->SetModel(&proxy);
    table->SetOnColumnHeaderClicked([&](foundation::usize col) { proxy.ToggleSort(col); });
    EventDispatcher* d = root->GetEventDispatcher();

    // Click the "Value" header (column 1, x in [150, 300)).
    d->InjectMouseDown(foundation::Float2{200.0f, 13.0f}, MouseButton::Left);
    d->InjectMouseUp(foundation::Float2{200.0f, 13.0f}, MouseButton::Left);
    CHECK(proxy.GetSortColumn() == 1);
    CHECK(proxy.GetSortOrder() == SortOrder::Ascending);
    CHECK(proxy.Data(MakeModelIndex(0, 1)).AsInt() == 1); // ascending: smallest first
}
