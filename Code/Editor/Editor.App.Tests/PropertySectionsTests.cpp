// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// A property source made of other sources, as an inspector has one source per part of an entity.
// Each section's top-level properties appear under its title, with their paths under its key. Reads,
// edits and notifications all go to and from the section's own source, and a grid shows a header per
// section with its rows under it.

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.gui;
import foundation.gui.toolkit;
import editor.app;

using namespace foundation::core;
namespace gui = foundation::gui;
using editor::app::PropertySections;

namespace
{
    struct Watcher final : gui::IPropertySourceClient
    {
        int properties = 0;
        int values = 0;
        void OnPropertiesChanged() override { ++properties; }
        void OnValuesChanged() override { ++values; }
    };
}

TEST_CASE("property sections: each source's properties under its title and key; reads and edits reach the section's own source")
{
    gui::PropertyBag transform(DefaultAllocator(), 1);
    (void)transform.Add(u8"position", gui::PropertyKind::Float, gui::Variant(1.0));
    gui::PropertyBag light(DefaultAllocator(), 1);
    (void)light.Add(u8"intensity", gui::PropertyKind::Float, gui::Variant(3.0));
    (void)light.Add(u8"position", gui::PropertyKind::Float, gui::Variant(9.0)); // the same name, another section

    PropertySections sections(DefaultAllocator());
    Watcher watcher;
    sections.SetClient(&watcher);
    sections.Add(u8"s0", u8"Transform", transform);
    sections.Add(u8"s1", u8"Light", light);
    CHECK(sections.SectionCount() == 2u);

    Array<gui::PropertyInfo> top;
    sections.GetProperties(StringView{}, top);
    REQUIRE(top.Size() == 3u);
    CHECK(top[0].Name == String(u8"s0/position"));
    CHECK(top[0].Category == String(u8"Transform"));
    CHECK(top[2].Name == String(u8"s1/position"));
    CHECK(top[2].Category == String(u8"Light"));
    CHECK(top[2].ShownName() == StringView(u8"position"));

    CHECK(sections.GetValue(u8"s1/position", 0).AsFloat() == doctest::Approx(9.0));
    CHECK(sections.GetValue(u8"s0/position", 0).AsFloat() == doctest::Approx(1.0));
    CHECK(sections.GetValue(u8"s2/position", 0).IsEmpty()); // no such section

    gui::PropertyWrite write;
    write.Value = gui::Variant(5.0);
    sections.BeginEdit(u8"s1/intensity");
    sections.CommitEdit(u8"s1/intensity", write);
    CHECK(light.GetValue(u8"intensity", 0).AsFloat() == doctest::Approx(5.0));
    CHECK(transform.GetValue(u8"position", 0).AsFloat() == doctest::Approx(1.0)); // the other section untouched

    // A notification from a section's source reaches the client of the sections.
    const int before = watcher.values;
    light.SetValue(u8"intensity", 0, gui::Variant(6.0));
    CHECK(watcher.values > before);

    CHECK(sections.SectionOf(u8"s1/intensity") == StringView(u8"s1"));
    CHECK(sections.SectionOf(u8"#Transform") == StringView(u8"s0"));
    CHECK(sections.SectionOf(u8"#Nowhere").IsEmpty());

    // Read only: every row says so, and an edit through it is refused.
    sections.SetReadOnly(true);
    Array<gui::PropertyInfo> locked;
    sections.GetProperties(StringView{}, locked);
    CHECK(locked[1].ReadOnly);
    write.Value = gui::Variant(7.0);
    sections.BeginEdit(u8"s1/intensity");
    sections.CommitEdit(u8"s1/intensity", write);
    CHECK(light.GetValue(u8"intensity", 0).AsFloat() == doctest::Approx(6.0));
    sections.SetReadOnly(false);

    sections.Clear(); // the sources are released
    CHECK(light.GetClient() == nullptr);
}

TEST_CASE("property sections: a grid shows a header per section, its rows under it")
{
    gui::PropertyBag transform(DefaultAllocator(), 1);
    (void)transform.Add(u8"position", gui::PropertyKind::Float, gui::Variant(1.0));
    gui::PropertyBag light(DefaultAllocator(), 1);
    (void)light.Add(u8"intensity", gui::PropertyKind::Float, gui::Variant(3.0));
    PropertySections sections(DefaultAllocator());
    sections.Add(u8"s0", u8"Transform", transform);
    sections.Add(u8"s1", u8"Light", light);

    RefPtr<gui::SceneRoot> root = MakeRef<gui::SceneRoot>(DefaultAllocator(), MakeRef<gui::GUIContext>(DefaultAllocator()));
    root->SetSize(Float2{400.0f, 300.0f});
    RefPtr<gui::PropertyGrid> grid = MakeRef<gui::PropertyGrid>(DefaultAllocator());
    grid->EditLayoutStyle([](gui::LayoutStyle& s)
    {
        s.Width = gui::LayoutLength::Px(300.0f);
        s.Height = gui::LayoutLength::Px(240.0f);
    });
    root->AddChild(grid.Get());
    grid->SetSource(&sections);
    (void)root->UpdateLayout();
    String rows;
    for (usize i = 0; i < grid->GetRowCount(); ++i)
    {
        if (!rows.IsEmpty())
            rows += u8" ";
        rows += grid->GetRow(static_cast<i32>(i))->Path;
    }
    CHECK(rows == String(u8"#Transform s0/position #Light s1/intensity"));
    grid->SetSource(nullptr);
}
