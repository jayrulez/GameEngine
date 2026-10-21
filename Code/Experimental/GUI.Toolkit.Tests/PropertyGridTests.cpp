// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Toolkit - the property grid (T1 of Specs/gui-lab-evolution-3.md): a source's properties as rows,
// categories first-seen and groups folded by path, a search keeping what holds a match, only the
// rows in view realised, one label split dragged at any row, the wheel scrolling and never editing,
// and an edit as the source's gesture, its row not rebound from its own previews.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import experimental.gui;
import experimental.gui.toolkit;

using namespace experimental::gui;
namespace core = foundation::core;
using core::f32;
using core::i32;
using core::i64;
using core::usize;

namespace
{
    core::StringView SV(const char8_t* s) { return core::StringView(s); }

    struct Bed
    {
        core::RefPtr<SceneRoot> root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
        core::RefPtr<PropertyGrid> grid = core::MakeRef<PropertyGrid>(core::DefaultAllocator());
        PropertyBag bag;

        explicit Bed(usize objects = 1) : bag(core::DefaultAllocator(), objects)
        {
            root->SetSize(core::Float2{400.0f, 300.0f});
            root->EditLayoutStyle([](LayoutStyle& s) { s.AlignItems = Align::Start; });
            grid->EditLayoutStyle([](LayoutStyle& s)
            {
                s.Width = LayoutLength::Px(300.0f);
                s.Height = LayoutLength::Px(240.0f);
            });
            root->AddChild(grid.Get());
        }
        ~Bed() { grid->SetSource(nullptr); }
        PropertyInfo& Add(const char8_t* path, PropertyKind kind, const Variant& value = {}, const char8_t* category = u8"")
        {
            PropertyInfo& info = bag.Add(SV(path), kind, value);
            info.Category = core::String(category);
            return info;
        }
        void Show()
        {
            grid->SetSource(&bag);
            Frame();
        }
        void Frame() { (void)root->UpdateLayout(); }
        core::Array<core::String> Paths() const
        {
            core::Array<core::String> out(core::DefaultAllocator());
            for (usize i = 0; i < grid->GetRowCount(); ++i)
                out.PushBack(grid->GetRow(static_cast<i32>(i))->Path);
            return out;
        }
        EventDispatcher& D() { return *root->GetEventDispatcher(); }
    };

    core::String Join(const core::Array<core::String>& paths)
    {
        core::String out;
        for (const core::String& path : paths)
        {
            if (out.Size() != 0)
                out.Append(SV(u8" "));
            out.Append(path.AsView());
        }
        return out;
    }
    core::Float2 Centre(const Node& node)
    {
        const Rect r = node.GetScreenBounds();
        return core::Float2{r.x + r.width * 0.5f, r.y + r.height * 0.5f};
    }
}

TEST_CASE("property grid: what has no category first, then categories as they first come; groups start folded")
{
    Bed bed;
    bed.Add(u8"name", PropertyKind::String, Variant(SV(u8"Lamp")));
    bed.Add(u8"intensity", PropertyKind::Float, Variant(1.0), u8"Light");
    bed.Add(u8"position", PropertyKind::Group, {}, u8"Transform");
    bed.Add(u8"position.x", PropertyKind::Float, Variant(0.0));
    bed.Add(u8"position.y", PropertyKind::Float, Variant(0.0));
    bed.Add(u8"color", PropertyKind::Float, Variant(0.5), u8"Light");
    bed.Add(u8"secret", PropertyKind::Bool, Variant(false)).Visible = false;
    bed.Show();
    CHECK(Join(bed.Paths()) == core::String(u8"name #Light intensity color #Transform position"));
    CHECK(bed.grid->GetRow(1)->Category);
    CHECK(bed.grid->GetRow(5)->HasChildren);
    CHECK_FALSE(bed.grid->GetRow(5)->Open);
    CHECK(bed.grid->FindEditor(SV(u8"intensity")) != nullptr);

    // A group opened shows its parts a level in; the folding is by path, so it stays when the
    // source's properties are read again.
    bed.grid->SetOpen(SV(u8"position"), true);
    CHECK(Join(bed.Paths()) == core::String(u8"name #Light intensity color #Transform position position.x position.y"));
    CHECK(bed.grid->GetRow(6)->Depth == 1);
    bed.bag.Add(SV(u8"scale"), PropertyKind::Float, Variant(1.0)).Category = core::String(u8"Transform");
    bed.bag.PropertiesChanged(); // its category, set after it was added
    CHECK(Join(bed.Paths()) == core::String(u8"name #Light intensity color #Transform position position.x position.y scale"));

    // A category closed hides what is in it.
    bed.grid->SetOpen(SV(u8"#Light"), false);
    CHECK(Join(bed.Paths()) == core::String(u8"name #Light #Transform position position.x position.y scale"));
}

TEST_CASE("property grid: the chevron, a header's click and Left and Right fold and unfold")
{
    Bed bed;
    bed.Add(u8"offset", PropertyKind::Group, {}, u8"Physics");
    bed.Add(u8"offset.x", PropertyKind::Float, Variant(0.0));
    bed.Show();
    REQUIRE(bed.grid->GetRowCount() == 2u);

    ItemRow* header = bed.grid->RowForItem(0);
    REQUIRE(header != nullptr);
    bed.D().InjectMouseDown(Centre(*header), MouseButton::Left);
    bed.D().InjectMouseUp(Centre(*header), MouseButton::Left);
    CHECK_FALSE(bed.grid->IsOpen(SV(u8"#Physics")));
    CHECK(bed.grid->GetRowCount() == 1u);
    bed.grid->SetOpen(SV(u8"#Physics"), true);
    bed.Frame();

    // The group's row selected, Right opens it and Left closes it.
    bed.grid->SetSelectedItem(1);
    bed.grid->RequestFocus();
    bed.D().InjectKeyDown(static_cast<core::u32>(KeyCode::Right));
    CHECK(bed.grid->IsOpen(SV(u8"offset")));
    CHECK(bed.grid->GetRowCount() == 3u);
    bed.D().InjectKeyDown(static_cast<core::u32>(KeyCode::Left));
    CHECK_FALSE(bed.grid->IsOpen(SV(u8"offset")));
}

TEST_CASE("property grid: a search keeps matches by name or path, with what holds them opened; the folding stays")
{
    Bed bed;
    bed.Add(u8"name", PropertyKind::String, Variant(SV(u8"Lamp")));
    bed.Add(u8"position", PropertyKind::Group, {}, u8"Transform");
    bed.Add(u8"position.x", PropertyKind::Float, Variant(0.0)).DisplayName = core::String(u8"Along");
    bed.Add(u8"position.y", PropertyKind::Float, Variant(0.0));
    bed.Add(u8"intensity", PropertyKind::Float, Variant(1.0), u8"Light");
    bed.Show();
    bed.grid->SetOpen(SV(u8"#Light"), false);

    bed.grid->SetFilter(SV(u8"ALONG")); // by shown name, either case
    CHECK(Join(bed.Paths()) == core::String(u8"#Transform position position.x"));
    bed.grid->SetFilter(SV(u8"position.y")); // by path
    CHECK(Join(bed.Paths()) == core::String(u8"#Transform position position.y"));
    bed.grid->SetFilter(SV(u8"inten")); // in a closed category: opened while searching
    CHECK(Join(bed.Paths()) == core::String(u8"#Light intensity"));
    bed.grid->SetFilter(SV(u8"position")); // a group that matches keeps its own folding
    CHECK(Join(bed.Paths()) == core::String(u8"#Transform position"));
    bed.grid->SetFilter(SV(u8"nothing"));
    CHECK(bed.grid->GetRowCount() == 0u);

    bed.grid->SetFilter(SV(u8""));
    CHECK(Join(bed.Paths()) == core::String(u8"name #Transform position #Light"));
}

TEST_CASE("property grid: only the rows in view are realised")
{
    Bed bed;
    for (int i = 0; i < 500; ++i)
        bed.bag.Add(core::Format(u8"p{}", i).AsView(), PropertyKind::Float, Variant(static_cast<double>(i)));
    bed.Show();
    CHECK(bed.grid->GetRowCount() == 500u);
    CHECK(bed.grid->VisibleRowCount() > 5u);
    CHECK(bed.grid->VisibleRowCount() < 20u);
    CHECK(bed.grid->FindEditor(SV(u8"p0")) != nullptr);
    CHECK(bed.grid->FindEditor(SV(u8"p400")) == nullptr);
    bed.grid->ScrollToItem(400);
    bed.Frame();
    REQUIRE(bed.grid->FindEditor(SV(u8"p400")) != nullptr);
    NumberPropertyEditor* editor = core::Cast<NumberPropertyEditor>(bed.grid->FindEditor(SV(u8"p400")));
    REQUIRE(editor != nullptr);
    CHECK(editor->GetField()->GetValue() == doctest::Approx(400.0)); // a pooled editor shows its row's value
}

TEST_CASE("property grid: an edit is one gesture of the source; its row keeps its editor while it previews")
{
    Bed bed(2);
    PropertyInfo& info = bed.Add(u8"speed", PropertyKind::Float, Variant(2.0));
    info.Step = 1.0;
    bed.bag.SetValue(SV(u8"speed"), 1, Variant(5.0));
    core::Array<PropertyBag::Commit> commits(core::DefaultAllocator());
    bed.bag.SetOnCommit([&commits](const PropertyBag::Commit& commit) { commits.PushBack(commit); });
    bed.Show();

    NumberPropertyEditor* editor = core::Cast<NumberPropertyEditor>(bed.grid->FindEditor(SV(u8"speed")));
    REQUIRE(editor != nullptr);
    NumberField& field = *editor->GetField();
    CHECK(field.IsMixed());

    const core::Float2 at = Centre(field) - core::Float2{30.0f, 0.0f};
    bed.D().InjectMouseDown(at, MouseButton::Left);
    bed.D().InjectMouseMove(at + core::Float2{8.0f, 0.0f});
    bed.Frame();
    CHECK(bed.grid->GetEditPath() == SV(u8"speed"));
    CHECK(bed.bag.GetValue(SV(u8"speed"), 1).AsFloat() == doctest::Approx(4.0)); // previewed on both
    bed.D().InjectMouseMove(at + core::Float2{16.0f, 0.0f});
    bed.Frame();
    CHECK(field.GetValue() == doctest::Approx(6.0));
    bed.D().InjectMouseUp(at + core::Float2{16.0f, 0.0f}, MouseButton::Left);
    bed.Frame();
    CHECK(bed.grid->GetEditPath().Size() == 0u);
    REQUIRE(commits.Size() == 1u);
    CHECK(commits[0].Before[0].AsFloat() == doctest::Approx(2.0));
    CHECK(commits[0].Before[1].AsFloat() == doctest::Approx(5.0));
    CHECK(commits[0].After[1].AsFloat() == doctest::Approx(6.0));
    CHECK_FALSE(field.IsMixed());

    // A value changed elsewhere (an undo) shows.
    bed.bag.SetValue(SV(u8"speed"), 0, Variant(9.0));
    bed.Frame();
    CHECK(field.IsMixed());
}

TEST_CASE("property grid: the label split is dragged at any row, for every row, mirrored right to left")
{
    Bed bed;
    bed.root->SetPadding(Thickness{50.0f, 30.0f, 0.0f, 0.0f}); // the grid away from the origin
    bed.Add(u8"a", PropertyKind::Float, Variant(1.0));
    bed.Add(u8"b", PropertyKind::Float, Variant(2.0));
    bed.Show();
    PropertyEditor* a = bed.grid->FindEditor(SV(u8"a"));
    PropertyEditor* b = bed.grid->FindEditor(SV(u8"b"));
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    const f32 before = a->GetScreenBounds().x;

    const PropertyRowContent* row = core::Cast<PropertyRowContent>(b->GetParent());
    REQUIRE(row != nullptr);
    const core::Float2 grip = Centre(*row->GetSplitHandle());
    bed.D().InjectMouseMove(grip);
    CHECK(bed.D().GetCursor() == Cursor::EwResize);
    bed.D().InjectMouseDown(grip, MouseButton::Left);
    bed.D().InjectMouseMove(grip + core::Float2{60.0f, 0.0f});
    bed.D().InjectMouseUp(grip + core::Float2{60.0f, 0.0f}, MouseButton::Left);
    bed.Frame();
    // The split follows the pointer: 60 pixels further along the rows' width.
    const f32 rows = bed.grid->GetContentBounds().width;
    CHECK(bed.grid->GetLabelRatio() == doctest::Approx(0.4f + 60.0f / rows).epsilon(0.02));
    CHECK(a->GetScreenBounds().x > before + 50.0f); // the other row's editor moved too

    bed.grid->SetLabelRatio(0.05f);
    CHECK(bed.grid->GetLabelRatio() == doctest::Approx(0.1f));

    // Right to left the labels are at the right, the editors at the left.
    bed.grid->SetLabelRatio(0.4f);
    bed.root->SetLayoutDirection(Node::LayoutDirection::RightToLeft);
    bed.Frame();
    const Rect box = bed.grid->GetScreenBounds();
    CHECK(a->GetScreenBounds().x < box.x + 10.0f);
}

TEST_CASE("property grid: the wheel over an editor scrolls the grid and never changes a value")
{
    Bed bed;
    for (int i = 0; i < 60; ++i)
        bed.bag.Add(core::Format(u8"p{}", i).AsView(), PropertyKind::Float, Variant(1.0));
    bed.Show();
    NumberPropertyEditor* editor = core::Cast<NumberPropertyEditor>(bed.grid->FindEditor(SV(u8"p0")));
    REQUIRE(editor != nullptr);
    editor->GetField()->RequestFocus();
    bed.D().InjectMouseWheel(Centre(*editor->GetField()), core::Float2{0.0f, -3.0f});
    bed.Frame();
    CHECK(bed.bag.GetValue(SV(u8"p0"), 0).AsFloat() == doctest::Approx(1.0));
    CHECK(bed.grid->GetScrollOffset().y > 0.0f);
}

TEST_CASE("property grid: an application's editor for a hint, a hint none takes, and markup makes a grid")
{
    Bed bed;
    bed.Add(u8"look", PropertyKind::Float, Variant(1.0)).Hint = core::String(u8"toggle");
    bed.Show();
    CHECK(core::Cast<NumberPropertyEditor>(bed.grid->FindEditor(SV(u8"look"))) != nullptr);
    bed.grid->GetEditors().RegisterHint(SV(u8"toggle"), [](core::IAllocator& a) -> core::RefPtr<PropertyEditor> { return core::MakeRef<BoolPropertyEditor>(a); });
    bed.grid->Refresh();
    bed.Frame();
    CHECK(core::Cast<BoolPropertyEditor>(bed.grid->FindEditor(SV(u8"look"))) != nullptr);

    // A group hinted for an editor no one registered says so; one with no hint has no editor.
    bed.Add(u8"falloff", PropertyKind::Group).Hint = core::String(u8"curve");
    bed.Add(u8"plain", PropertyKind::Group);
    bed.Frame();
    UnknownPropertyEditor* unknown = core::Cast<UnknownPropertyEditor>(bed.grid->FindEditor(SV(u8"falloff")));
    REQUIRE(unknown != nullptr);
    CHECK(unknown->GetMessage() == SV(u8"No editor for curve"));
    CHECK(bed.grid->FindEditor(SV(u8"plain")) == nullptr);

    RegisterGuiElements();
    RegisterToolkitElements();
    static const ElementSet elements = ElementSet::All();
    MarkupLoader loader(elements, core::DefaultAllocator());
    core::RefPtr<Node> made = loader.LoadFromString(SV(u8R"(<PropertyGrid label-ratio="0.3" indent-width="20"/>)"));
    PropertyGrid* grid = core::Cast<PropertyGrid>(made.Get());
    REQUIRE(grid != nullptr);
    CHECK(grid->GetLabelRatio() == doctest::Approx(0.3f));
    CHECK(grid->GetIndentWidth() == doctest::Approx(20.0f));
}
