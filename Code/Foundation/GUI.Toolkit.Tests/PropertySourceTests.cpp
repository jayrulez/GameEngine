// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Toolkit - the property source. Properties are found by path; each object's value is read,
// and the mixed state over them; flags are mixed per bit. An edit is a gesture: it previews on every
// object, then commits once from the originals or cancels back to them.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.gui;
import foundation.gui.toolkit;

using namespace foundation::gui;
namespace core = foundation::core;
using core::i64;
using core::u64;

namespace
{
    core::StringView SV(const char8_t* s) { return core::StringView(s); }

    struct Client : IPropertySourceClient
    {
        int properties = 0, values = 0;
        void OnPropertiesChanged() override { ++properties; }
        void OnValuesChanged() override { ++values; }
    };
}

TEST_CASE("property source: paths join and part, and a bag lists the properties under a path")
{
    CHECK(JoinPropertyPath(SV(u8""), SV(u8"position")) == core::String(u8"position"));
    CHECK(JoinPropertyPath(SV(u8"position"), SV(u8"x")) == core::String(u8"position.x"));
    CHECK(ParentPropertyPath(SV(u8"position.x")) == SV(u8"position"));
    CHECK(ParentPropertyPath(SV(u8"position")).Size() == 0u);

    PropertyBag bag(core::DefaultAllocator());
    bag.Add(SV(u8"name"), PropertyKind::String, Variant(SV(u8"Lamp")));
    bag.Add(SV(u8"position"), PropertyKind::Group);
    bag.Add(SV(u8"position.x"), PropertyKind::Float, Variant(1.0));
    bag.Add(SV(u8"position.y"), PropertyKind::Float, Variant(2.0)).DisplayName = core::String(u8"Y");

    core::Array<PropertyInfo> top(core::DefaultAllocator());
    bag.GetProperties(SV(u8""), top);
    REQUIRE(top.Size() == 2u);
    CHECK(top[0].Name == core::String(u8"name"));
    CHECK(top[1].Kind == PropertyKind::Group);
    core::Array<PropertyInfo> parts(core::DefaultAllocator());
    bag.GetProperties(SV(u8"position"), parts);
    REQUIRE(parts.Size() == 2u);
    CHECK(parts[0].Name == core::String(u8"x"));
    CHECK(parts[0].ShownName() == SV(u8"x"));
    CHECK(parts[1].ShownName() == SV(u8"Y"));
    CHECK(bag.GetValue(SV(u8"position.y"), 0).AsFloat() == doctest::Approx(2.0));
}

TEST_CASE("property source: a value over several objects is mixed when any differs; flags per bit")
{
    PropertyBag bag(core::DefaultAllocator(), 3);
    bag.Add(SV(u8"speed"), PropertyKind::Float, Variant(1.0));
    bag.Add(SV(u8"layers"), PropertyKind::Flags, Variant(i64{0b0101}));
    CHECK_FALSE(ReadPropertyValue(bag, SV(u8"speed"), PropertyKind::Float).Mixed);

    bag.SetValue(SV(u8"speed"), 2, Variant(3.0));
    const PropertyValue speed = ReadPropertyValue(bag, SV(u8"speed"), PropertyKind::Float);
    CHECK(speed.Mixed);
    CHECK(speed.Value.AsFloat() == doctest::Approx(1.0)); // the first object's

    bag.SetValue(SV(u8"layers"), 1, Variant(i64{0b0111}));
    const PropertyValue layers = ReadPropertyValue(bag, SV(u8"layers"), PropertyKind::Flags);
    CHECK(layers.Mixed);
    CHECK(layers.MixedBits == u64{0b0010});

    // Setting a flag on all of them leaves the bit they differ in as each object had it.
    bag.BeginEdit(SV(u8"layers"));
    bag.CommitEdit(SV(u8"layers"), PropertyWrite{Variant(i64{0b1000}), u64{0b1000}});
    CHECK(bag.GetValue(SV(u8"layers"), 0).AsInt() == 0b1101);
    CHECK(bag.GetValue(SV(u8"layers"), 1).AsInt() == 0b1111);
    CHECK(ReadPropertyValue(bag, SV(u8"layers"), PropertyKind::Flags).MixedBits == u64{0b0010});
}

TEST_CASE("property source: an edit previews on every object, commits once from the originals, or cancels back")
{
    PropertyBag bag(core::DefaultAllocator(), 2);
    Client client;
    bag.SetClient(&client);
    bag.Add(SV(u8"size"), PropertyKind::Float, Variant(1.0));
    CHECK(client.properties == 1);
    bag.SetValue(SV(u8"size"), 1, Variant(4.0));
    core::Array<PropertyBag::Commit> commits(core::DefaultAllocator());
    bag.SetOnCommit([&commits](const PropertyBag::Commit& commit) { commits.PushBack(commit); });

    bag.BeginEdit(SV(u8"size"));
    CHECK(bag.OpenEdits() == 1u);
    bag.PreviewEdit(SV(u8"size"), PropertyWrite{Variant(2.0)});
    bag.PreviewEdit(SV(u8"size"), PropertyWrite{Variant(3.0)});
    CHECK(bag.GetValue(SV(u8"size"), 0).AsFloat() == doctest::Approx(3.0));
    CHECK(bag.GetValue(SV(u8"size"), 1).AsFloat() == doctest::Approx(3.0));
    CHECK(commits.IsEmpty()); // previews are not edits of their own
    bag.CommitEdit(SV(u8"size"), PropertyWrite{Variant(5.0)});
    CHECK(bag.OpenEdits() == 0u);
    REQUIRE(commits.Size() == 1u);
    CHECK(commits[0].Before[0].AsFloat() == doctest::Approx(1.0)); // from before the previews
    CHECK(commits[0].Before[1].AsFloat() == doctest::Approx(4.0));
    CHECK(commits[0].After[1].AsFloat() == doctest::Approx(5.0));

    bag.BeginEdit(SV(u8"size"));
    bag.PreviewEdit(SV(u8"size"), PropertyWrite{Variant(9.0)});
    const int before = client.values;
    bag.CancelEdit(SV(u8"size"));
    CHECK(client.values == before + 1);
    CHECK(bag.GetValue(SV(u8"size"), 0).AsFloat() == doctest::Approx(5.0));
    CHECK(commits.Size() == 1u);

    // Read only, so nothing is written.
    bag.FindInfo(SV(u8"size"))->ReadOnly = true;
    bag.BeginEdit(SV(u8"size"));
    bag.PreviewEdit(SV(u8"size"), PropertyWrite{Variant(7.0)});
    bag.CommitEdit(SV(u8"size"), PropertyWrite{Variant(7.0)});
    CHECK(bag.GetValue(SV(u8"size"), 1).AsFloat() == doctest::Approx(5.0));
    CHECK(bag.OpenEdits() == 0u);
}

TEST_CASE("property source: a list's elements are its children by index, saying what the list gives them, changed in each object by one call")
{
    PropertyBag bag(core::DefaultAllocator(), 2);
    PropertyInfo& info = bag.AddList(SV(u8"tags"), PropertyKind::String, Variant(SV(u8"new")), 2);
    info.MaxCount = 3;
    bag.SetValue(SV(u8"tags.0"), 0, Variant(SV(u8"a")));
    bag.SetValue(SV(u8"tags.1"), 0, Variant(SV(u8"b")));
    CHECK(bag.GetValue(SV(u8"tags"), 0).AsInt() == 2); // a list's value is its count
    core::Array<PropertyInfo> elements(core::DefaultAllocator());
    bag.GetProperties(SV(u8"tags"), elements);
    REQUIRE(elements.Size() == 2u);
    CHECK(elements[1].Name == core::String(u8"1"));
    CHECK(elements[1].Kind == PropertyKind::String);
    // The list provides each element's info beyond its name and kind.
    REQUIRE(bag.FindElementInfo(SV(u8"tags")) != nullptr);
    bag.FindElementInfo(SV(u8"tags"))->Description = core::String(u8"A tag");
    bag.FindElementInfo(SV(u8"tags"))->Name = core::String(u8"ignored"); // an element is named by its index
    elements.Clear();
    bag.GetProperties(SV(u8"tags"), elements);
    CHECK(elements[0].Description == core::String(u8"A tag"));
    CHECK(elements[0].Name == core::String(u8"0"));
    CHECK(bag.FindElementInfo(SV(u8"tags.0")) == nullptr); // no list

    // An element is edited as any property is.
    bag.BeginEdit(SV(u8"tags.1"));
    bag.CommitEdit(SV(u8"tags.1"), PropertyWrite{Variant(SV(u8"c"))});
    CHECK(bag.GetValue(SV(u8"tags.1"), 1).AsString() == SV(u8"c"));

    ListEdit move;
    move.Kind = ListEdit::Op::Move;
    move.Index = 1;
    move.To = 0;
    CHECK(bag.EditList(SV(u8"tags"), move));
    CHECK(bag.GetValue(SV(u8"tags.0"), 0).AsString() == SV(u8"c"));
    ListEdit insert;
    insert.Kind = ListEdit::Op::Insert;
    insert.Index = 9; // past the end, so it goes at the end
    CHECK(bag.EditList(SV(u8"tags"), insert));
    CHECK(bag.GetValue(SV(u8"tags.2"), 1).AsString() == SV(u8"new"));
    CHECK_FALSE(bag.EditList(SV(u8"tags"), insert)); // the list is full
    ListEdit remove;
    remove.Kind = ListEdit::Op::Remove;
    remove.Index = 0;
    CHECK(bag.EditList(SV(u8"tags"), remove));
    CHECK(bag.ElementCount(SV(u8"tags"), 0) == 2u);

    info.ListOps = PropertyListAdd; // may only add
    CHECK_FALSE(bag.EditList(SV(u8"tags"), remove));
}

TEST_CASE("property source: a bag's describe hook names a list's elements and sums up a group, as it lists them")
{
    // An owner names an element from its values (a terrain's layers by channel, each by its albedo).
    PropertyBag bag(core::DefaultAllocator());
    PropertyInfo& list = bag.AddList(SV(u8"layers"), PropertyKind::Group, Variant{}, 2);
    (void)list;
    REQUIRE(bag.AddElementField(SV(u8"layers"), SV(u8"albedo"), PropertyKind::String, Variant(SV(u8"grass"))) != nullptr);
    bag.SetValue(SV(u8"layers.1.albedo"), 0, Variant(SV(u8"rock")));
    core::Array<core::String> seen(core::DefaultAllocator());
    bag.SetOnDescribe([&bag, &seen](core::StringView path, PropertyInfo& info)
                      {
                          seen.PushBack(core::String(path));
                          if (ParentPropertyPath(path) == SV(u8"layers"))
                          {
                              info.DisplayName = core::String(u8"Layer");
                              info.Summary = core::String(bag.GetValue(JoinPropertyPath(path, SV(u8"albedo")).AsView(), 0).AsString());
                          }
                      });
    core::Array<PropertyInfo> elements(core::DefaultAllocator());
    bag.GetProperties(SV(u8"layers"), elements);
    REQUIRE(elements.Size() == 2u);
    CHECK(elements[0].ShownName() == SV(u8"Layer"));
    CHECK(elements[1].Summary == core::String(u8"rock"));
    core::Array<PropertyInfo> fields(core::DefaultAllocator());
    bag.GetProperties(SV(u8"layers.0"), fields);
    REQUIRE(fields.Size() == 1u);
    CHECK(seen.Back() == core::String(u8"layers.0.albedo")); // the hook sees every row the bag lists, by its full path
}

TEST_CASE("property source: each object's own default, and a revert of a path and what is under it")
{
    PropertyBag bag(core::DefaultAllocator(), 2);
    bag.Add(SV(u8"offset"), PropertyKind::Group);
    bag.Add(SV(u8"offset.x"), PropertyKind::Float, Variant(5.0));
    bag.Add(SV(u8"offset.y"), PropertyKind::Float, Variant(6.0));
    bag.SetDefault(SV(u8"offset.x"), Variant(0.0));
    bag.SetDefault(SV(u8"offset.y"), Variant(1.0), 0); // the objects' defaults differ (two prefabs)
    bag.SetDefault(SV(u8"offset.y"), Variant(2.0), 1);
    CHECK(bag.GetDefault(SV(u8"offset.y"), 1).AsFloat() == doctest::Approx(2.0));
    CHECK(bag.GetDefault(SV(u8"offset"), 0).IsEmpty());
    int reverts = 0;
    bag.SetOnReverted([&reverts](core::StringView) { ++reverts; });

    CHECK(bag.Revert(SV(u8"offset")));
    CHECK(bag.GetValue(SV(u8"offset.x"), 1).AsFloat() == doctest::Approx(0.0));
    CHECK(bag.GetValue(SV(u8"offset.y"), 0).AsFloat() == doctest::Approx(1.0));
    CHECK(bag.GetValue(SV(u8"offset.y"), 1).AsFloat() == doctest::Approx(2.0));
    CHECK(reverts == 1);
    CHECK_FALSE(bag.Revert(SV(u8"offset"))); // nothing left to put back
    CHECK(reverts == 1);
    CHECK(bag.GetRevertText(SV(u8"offset")) == core::String(u8"Revert to default"));
}
