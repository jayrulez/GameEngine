// Draconic GUI - MVC data-core tests: Variant (typed value + ToString + Compare), ModelIndex,
// and StringListModel (row/column/data + client notification on mutation).
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.gui;

using namespace draconic::gui;
namespace foundation = draconic::foundation;

namespace
{
    foundation::StringView SV(const char8_t* s) { return foundation::StringView(s); }

    // Counts model-update notifications.
    struct CountingClient : public IModelClient
    {
        int updates = 0;
        void OnModelUpdated() override { ++updates; }
    };
}

TEST_CASE("variant: types, accessors, ToString")
{
    CHECK(Variant{}.IsEmpty());
    CHECK(Variant(true).GetType() == Variant::Type::Bool);
    CHECK(Variant(static_cast<foundation::i64>(42)).AsInt() == 42);
    CHECK(Variant(SV(u8"hi")).AsString() == SV(u8"hi"));

    CHECK(Variant(static_cast<foundation::i64>(-7)).ToString() == SV(u8"-7"));
    CHECK(Variant(static_cast<foundation::i64>(0)).ToString() == SV(u8"0"));
    CHECK(Variant(true).ToString() == SV(u8"true"));
    CHECK(Variant(SV(u8"text")).ToString() == SV(u8"text"));
    CHECK(Variant(1.5).ToString() == SV(u8"1.5"));
    CHECK(Variant(3.0).ToString() == SV(u8"3")); // trailing zeros trimmed
}

TEST_CASE("variant: Compare orders numbers and strings")
{
    CHECK(Variant(static_cast<foundation::i64>(1)).Compare(Variant(static_cast<foundation::i64>(2))) < 0);
    CHECK(Variant(static_cast<foundation::i64>(5)).Compare(Variant(static_cast<foundation::i64>(5))) == 0);
    CHECK(Variant(SV(u8"apple")).Compare(Variant(SV(u8"banana"))) < 0);
    CHECK(Variant(SV(u8"pear")).Compare(Variant(SV(u8"peach"))) > 0);
}

TEST_CASE("model-index: validity + equality")
{
    CHECK_FALSE(ModelIndex{}.IsValid()); // default row -1
    CHECK(MakeModelIndex(0, 0).IsValid());
    CHECK(MakeModelIndex(3, 1) == MakeModelIndex(3, 1));
    CHECK_FALSE(MakeModelIndex(3, 1) == MakeModelIndex(3, 2));
}

TEST_CASE("string-list-model: rows/columns/data")
{
    foundation::Array<foundation::String> items;
    items.PushBack(foundation::String(SV(u8"Alpha")));
    items.PushBack(foundation::String(SV(u8"Beta")));
    StringListModel model(foundation::Move(items));

    CHECK(model.RowCount() == 2);
    CHECK(model.ColumnCount() == 1);
    CHECK(model.Data(MakeModelIndex(0)).AsString() == SV(u8"Alpha"));
    CHECK(model.Data(MakeModelIndex(1), ModelRole::Sort).AsString() == SV(u8"Beta"));

    // Out-of-range -> empty variant.
    CHECK(model.Data(MakeModelIndex(5)).IsEmpty());
    CHECK_FALSE(model.IsValidIndex(MakeModelIndex(2)));
}

TEST_CASE("model: mutating notifies registered clients")
{
    StringListModel model;
    CountingClient client;
    model.AddClient(&client);

    model.AddItem(SV(u8"one"));
    CHECK(client.updates == 1);
    CHECK(model.RowCount() == 1);

    model.SetItems({});
    CHECK(client.updates == 2);
    CHECK(model.RowCount() == 0);

    model.RemoveClient(&client);
    model.AddItem(SV(u8"ignored")); // no longer notified
    CHECK(client.updates == 2);
}
