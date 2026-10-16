// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - measure and arrange (E2 of Specs/gui-lab-evolution.md): a node asks for a size within
// constraints and its container places it; the display kinds (flex by CSS's rules, grid, stack,
// dock) and the out-of-flow children; sizes in lengths, percentages and auto, min and max, the
// box sizing; visibility against display; what a change lays out again; and the same from a sheet.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import experimental.gui;

using namespace experimental::gui;
namespace core = foundation::core;
using core::f32;

namespace
{
    core::StringView SV(const char8_t* s) { return core::StringView(s); }
    template <typename... T>
    core::Array<GridTrack> Tracks(T... tracks)
    {
        core::Array<GridTrack> out;
        (out.PushBack(tracks), ...);
        return out;
    }

    struct Bed
    {
        core::RefPtr<SceneRoot> root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
        Bed(f32 w = 400.0f, f32 h = 300.0f) { root->SetSize(core::Float2{w, h}); }
        void Layout() { root->UpdateLayout(); }
    };
    template <typename F>
    core::RefPtr<Panel> Box(Node& parent, F&& style)
    {
        auto panel = core::MakeRef<Panel>(core::DefaultAllocator());
        panel->EditLayoutStyle(static_cast<F&&>(style));
        parent.AddChild(panel.Get());
        return panel;
    }
    core::RefPtr<Panel> Fixed(Node& parent, f32 w, f32 h)
    {
        return Box(parent, [w, h](LayoutStyle& s) {
            s.Width = LayoutLength::Px(w);
            s.Height = LayoutLength::Px(h);
        });
    }
    void CheckRect(const Node& node, f32 x, f32 y, f32 w, f32 h)
    {
        CHECK(node.GetPosition().x == doctest::Approx(x));
        CHECK(node.GetPosition().y == doctest::Approx(y));
        CHECK(node.GetSize().x == doctest::Approx(w));
        CHECK(node.GetSize().y == doctest::Approx(h));
    }
}

TEST_CASE("layout: a plain container is a flex column; its children stack and stretch across")
{
    Bed bed;
    auto page = Box(*bed.root.Get(), [](LayoutStyle&) {});
    page->SetPadding(Thickness{10.0f});
    auto a = Box(*page.Get(), [](LayoutStyle& s) { s.Height = LayoutLength::Px(30.0f); });
    auto b = Box(*page.Get(), [](LayoutStyle& s) { s.Height = LayoutLength::Px(50.0f); });
    bed.Layout();
    CheckRect(*page, 0, 0, 400, 100); // stretched across the root; as tall as what it holds
    CheckRect(*a, 10, 10, 380, 30);
    CheckRect(*b, 10, 40, 380, 50);
}

TEST_CASE("layout: an auto-sized container is as big as its children and its chrome")
{
    Bed bed;
    auto holder = Box(*bed.root.Get(), [](LayoutStyle& s) {
        s.Placement = Position::Absolute; // out of flow: sized by its content, not stretched
        s.Direction = FlexDirection::Row;
        s.ColumnGap = 5.0f;
    });
    holder->SetPadding(Thickness{4.0f});
    (void)Fixed(*holder.Get(), 40.0f, 20.0f);
    (void)Fixed(*holder.Get(), 60.0f, 30.0f);
    bed.Layout();
    CHECK(holder->GetSize().x == doctest::Approx(4 + 40 + 5 + 60 + 4));
    CHECK(holder->GetSize().y == doctest::Approx(4 + 30 + 4));
}

TEST_CASE("layout: flex rows justify and align their items")
{
    const auto row = [](Justify justify, Align align) {
        Bed bed;
        auto line = Box(*bed.root.Get(), [=](LayoutStyle& s) {
            s.Height = LayoutLength::Px(100.0f);
            s.Direction = FlexDirection::Row;
            s.JustifyContent = justify;
            s.AlignItems = align;
        });
        auto a = Fixed(*line.Get(), 100.0f, 20.0f);
        auto b = Fixed(*line.Get(), 100.0f, 40.0f);
        bed.Layout();
        core::Array<f32> out;
        out.PushBack(a->GetPosition().x);
        out.PushBack(b->GetPosition().x);
        out.PushBack(a->GetPosition().y);
        out.PushBack(b->GetPosition().y);
        return out;
    };
    auto start = row(Justify::Start, Align::Start);
    CHECK(start[0] == doctest::Approx(0.0f));
    CHECK(start[1] == doctest::Approx(100.0f));
    auto between = row(Justify::SpaceBetween, Align::Center);
    CHECK(between[1] == doctest::Approx(300.0f));
    CHECK(between[2] == doctest::Approx(40.0f)); // (100 - 20) / 2
    CHECK(between[3] == doctest::Approx(30.0f));
    auto end = row(Justify::End, Align::End);
    CHECK(end[0] == doctest::Approx(200.0f));
    CHECK(end[2] == doctest::Approx(80.0f));
    auto evenly = row(Justify::SpaceEvenly, Align::Start);
    CHECK(evenly[0] == doctest::Approx(200.0f / 3.0f));
    auto center = row(Justify::Center, Align::Start);
    CHECK(center[0] == doctest::Approx(100.0f));
}

TEST_CASE("layout: grow shares free space; shrink takes an overflow, stopping at a minimum")
{
    Bed bed;
    auto line = Box(*bed.root.Get(), [](LayoutStyle& s) {
        s.Height = LayoutLength::Px(40.0f);
        s.Direction = FlexDirection::Row;
    });
    auto a = Box(*line.Get(), [](LayoutStyle& s) { s.Width = LayoutLength::Px(100.0f); s.Grow = 1.0f; });
    auto b = Box(*line.Get(), [](LayoutStyle& s) { s.Width = LayoutLength::Px(100.0f); s.Grow = 3.0f; });
    bed.Layout();
    CHECK(a->GetSize().x == doctest::Approx(150.0f)); // 200 free: a quarter
    CHECK(b->GetSize().x == doctest::Approx(250.0f));

    a->EditLayoutStyle([](LayoutStyle& s) { s.Width = LayoutLength::Px(300.0f); s.Grow = 0.0f; s.MinWidth = LayoutLength::Px(280.0f); });
    b->EditLayoutStyle([](LayoutStyle& s) { s.Width = LayoutLength::Px(300.0f); s.Grow = 0.0f; });
    bed.Layout();
    CHECK(a->GetSize().x == doctest::Approx(280.0f)); // its share would take it to 200
    CHECK(b->GetSize().x == doctest::Approx(120.0f));
    b->EditLayoutStyle([](LayoutStyle& s) { s.Shrink = 0.0f; });
    bed.Layout();
    CHECK(b->GetSize().x == doctest::Approx(300.0f)); // does not shrink: the line overflows
}

TEST_CASE("layout: flex wraps into lines; reverse and order change the sequence")
{
    Bed bed;
    auto flow = Box(*bed.root.Get(), [](LayoutStyle& s) {
        s.Direction = FlexDirection::Row;
        s.Wrap = FlexWrap::Wrap;
        s.ColumnGap = 10.0f;
        s.RowGap = 10.0f;
        s.AlignContent = Justify::Start;
    });
    core::Array<core::RefPtr<Panel>> cells;
    for (int i = 0; i < 5; ++i)
        cells.PushBack(Fixed(*flow.Get(), 120.0f, 30.0f));
    bed.Layout();
    CheckRect(*cells[2], 260, 0, 120, 30); // three fit in 400 with the gaps
    CheckRect(*cells[3], 0, 40, 120, 30);

    Bed second;
    auto row = Box(*second.root.Get(), [](LayoutStyle& s) { s.Direction = FlexDirection::RowReverse; s.Height = LayoutLength::Px(30.0f); });
    auto first = Fixed(*row.Get(), 50.0f, 30.0f);
    auto last = Fixed(*row.Get(), 50.0f, 30.0f);
    second.Layout();
    CHECK(first->GetPosition().x == doctest::Approx(350.0f));
    CHECK(last->GetPosition().x == doctest::Approx(300.0f));
    first->EditLayoutStyle([](LayoutStyle& s) { s.Order = 1; }); // after the other, so first from the end... last
    second.Layout();
    CHECK(first->GetPosition().x == doctest::Approx(300.0f));
}

TEST_CASE("layout: margins, percentages, min and max, and the box sizing")
{
    Bed bed;
    auto page = Box(*bed.root.Get(), [](LayoutStyle& s) { s.AlignItems = Align::Start; });
    auto half = Box(*page.Get(), [](LayoutStyle& s) {
        s.Width = LayoutLength::Percent(50.0f);
        s.Height = LayoutLength::Px(20.0f);
        s.Margin = Thickness{5.0f, 6.0f, 0.0f, 4.0f};
    });
    auto capped = Box(*page.Get(), [](LayoutStyle& s) {
        s.Width = LayoutLength::Percent(100.0f);
        s.MaxWidth = LayoutLength::Px(150.0f);
        s.Height = LayoutLength::Px(10.0f);
    });
    auto content = Box(*page.Get(), [](LayoutStyle& s) {
        s.Width = LayoutLength::Px(100.0f);
        s.Height = LayoutLength::Px(10.0f);
        s.Sizing = BoxSizing::ContentBox;
    });
    content->SetPadding(Thickness{10.0f});
    bed.Layout();
    CheckRect(*half, 5, 6, 200, 20);
    CHECK(capped->GetPosition().y == doctest::Approx(6 + 20 + 4));
    CHECK(capped->GetSize().x == doctest::Approx(150.0f));
    CHECK(content->GetSize().x == doctest::Approx(120.0f)); // its width is the content's: padding outside
    CHECK(content->GetSize().y == doctest::Approx(30.0f));
}

TEST_CASE("layout: grid tracks of lengths, fr and auto; items flow, span and sit where told")
{
    Bed bed;
    auto grid = Box(*bed.root.Get(), [](LayoutStyle& s) {
        s.Kind = Display::Grid;
        s.Columns = Tracks(GridTrack::Fixed(100.0f), GridTrack::Flex(1.0f), GridTrack::Flex(2.0f));
        s.ColumnGap = 10.0f;
        s.RowGap = 5.0f;
        s.AlignItems = Align::Start;
    });
    auto a = Box(*grid.Get(), [](LayoutStyle& s) { s.Height = LayoutLength::Px(20.0f); });
    auto b = Box(*grid.Get(), [](LayoutStyle& s) { s.Height = LayoutLength::Px(30.0f); });
    auto c = Box(*grid.Get(), [](LayoutStyle& s) { s.Height = LayoutLength::Px(20.0f); });
    auto wide = Box(*grid.Get(), [](LayoutStyle& s) { s.Height = LayoutLength::Px(10.0f); s.Column.Span = 2; });
    auto placed = Box(*grid.Get(), [](LayoutStyle& s) { s.Height = LayoutLength::Px(10.0f); s.Column.Start = 1; s.Row.Start = 3; });
    bed.Layout();
    // 400 - 100 - 20 gaps = 280 for 3fr: 93.33 and 186.67.
    CheckRect(*a, 0, 0, 100, 20);
    CHECK(b->GetPosition().x == doctest::Approx(110.0f));
    CHECK(b->GetSize().x == doctest::Approx(280.0f / 3.0f));
    CHECK(c->GetPosition().x == doctest::Approx(110.0f + 280.0f / 3.0f + 10.0f));
    CHECK(wide->GetPosition().y == doctest::Approx(30.0f + 5.0f)); // the first row as tall as its tallest
    CHECK(wide->GetSize().x == doctest::Approx(100.0f + 10.0f + 280.0f / 3.0f));
    CHECK(placed->GetPosition().x == doctest::Approx(0.0f));
    CHECK(placed->GetPosition().y == doctest::Approx(35.0f + 10.0f + 5.0f));
}

TEST_CASE("layout: a stack lays its children over each other, each aligned on its own")
{
    Bed bed;
    auto stack = Box(*bed.root.Get(), [](LayoutStyle& s) { s.Kind = Display::Stack; s.Height = LayoutLength::Px(200.0f); });
    auto fill = Box(*stack.Get(), [](LayoutStyle&) {});
    auto corner = Box(*stack.Get(), [](LayoutStyle& s) {
        s.Width = LayoutLength::Px(50.0f);
        s.Height = LayoutLength::Px(20.0f);
        s.JustifySelf = Align::End;
        s.AlignSelf = Align::End;
    });
    auto middle = Box(*stack.Get(), [](LayoutStyle& s) {
        s.Width = LayoutLength::Px(100.0f);
        s.Height = LayoutLength::Px(40.0f);
        s.JustifySelf = Align::Center;
        s.AlignSelf = Align::Center;
    });
    bed.Layout();
    CheckRect(*fill, 0, 0, 400, 200);
    CheckRect(*corner, 350, 180, 50, 20);
    CheckRect(*middle, 150, 80, 100, 40);
}

TEST_CASE("layout: dock puts children to the sides in order, and the fill takes the rest")
{
    Bed bed;
    auto dock = Box(*bed.root.Get(), [](LayoutStyle& s) { s.Kind = Display::Dock; s.Height = LayoutLength::Px(300.0f); });
    auto top = Box(*dock.Get(), [](LayoutStyle& s) { s.DockSide = Dock::Top; s.Height = LayoutLength::Px(30.0f); });
    auto left = Box(*dock.Get(), [](LayoutStyle& s) { s.DockSide = Dock::Left; s.Width = LayoutLength::Px(80.0f); });
    auto bottom = Box(*dock.Get(), [](LayoutStyle& s) { s.DockSide = Dock::Bottom; s.Height = LayoutLength::Px(20.0f); });
    auto right = Box(*dock.Get(), [](LayoutStyle& s) { s.DockSide = Dock::Right; s.Width = LayoutLength::Px(40.0f); });
    auto fill = Box(*dock.Get(), [](LayoutStyle&) {});
    bed.Layout();
    CheckRect(*top, 0, 0, 400, 30);
    CheckRect(*left, 0, 30, 80, 270);
    CheckRect(*bottom, 80, 280, 320, 20);
    CheckRect(*right, 360, 30, 40, 250);
    CheckRect(*fill, 80, 30, 280, 250);
}

TEST_CASE("layout: out of flow by its insets; placed from code; relative moves it alone")
{
    Bed bed;
    auto page = Box(*bed.root.Get(), [](LayoutStyle& s) { s.Height = LayoutLength::Px(300.0f); });
    auto first = Box(*page.Get(), [](LayoutStyle& s) { s.Height = LayoutLength::Px(30.0f); });
    auto banner = Box(*page.Get(), [](LayoutStyle& s) {
        s.Placement = Position::Absolute;
        s.Left = LayoutLength::Px(20.0f);
        s.Right = LayoutLength::Px(20.0f);
        s.Bottom = LayoutLength::Px(10.0f);
        s.Height = LayoutLength::Px(40.0f);
    });
    auto second = Box(*page.Get(), [](LayoutStyle& s) {
        s.Height = LayoutLength::Px(30.0f);
        s.Placement = Position::Relative;
        s.Left = LayoutLength::Px(15.0f);
    });
    auto third = Box(*page.Get(), [](LayoutStyle& s) { s.Height = LayoutLength::Px(30.0f); });
    bed.Layout();
    CheckRect(*banner, 20, 250, 360, 40); // left and right stretch it; bottom places it
    CHECK(second->GetPosition().y == doctest::Approx(30.0f)); // the banner takes no space
    CHECK(second->GetPosition().x == doctest::Approx(15.0f));
    CHECK(third->GetPosition().x == doctest::Approx(0.0f)); // its sibling where it was
    CHECK(third->GetPosition().y == doctest::Approx(60.0f));

    first->SetPosition(core::Float2{200.0f, 100.0f}); // from code: out of the flow, at that place
    bed.Layout();
    CHECK(first->GetPosition().x == doctest::Approx(200.0f));
    CHECK(second->GetPosition().y == doctest::Approx(0.0f)); // the others closed the gap
}

TEST_CASE("layout: hidden keeps its place; not displayed takes none, and comes back as it was")
{
    Bed bed;
    auto row = Box(*bed.root.Get(), [](LayoutStyle& s) { s.Kind = Display::Grid; s.Columns = Tracks(GridTrack::Fixed(50.0f), GridTrack::Fixed(50.0f)); });
    auto a = Fixed(*row.Get(), 50.0f, 20.0f);
    auto b = Fixed(*row.Get(), 50.0f, 20.0f);
    auto c = Fixed(*row.Get(), 50.0f, 20.0f);
    a->SetVisible(false);
    bed.Layout();
    CHECK(b->GetPosition().x == doctest::Approx(50.0f)); // a still holds the first cell
    a->SetDisplayed(false);
    bed.Layout();
    CHECK(b->GetPosition().x == doctest::Approx(0.0f));
    CHECK(c->GetPosition().x == doctest::Approx(50.0f));
    row->SetDisplayed(false);
    row->SetDisplayed(true);
    CHECK(row->GetLayoutStyle().Kind == Display::Grid);
}

TEST_CASE("layout: a change lays out what it affects; a fixed-size box stops it")
{
    Bed bed;
    auto outer = Box(*bed.root.Get(), [](LayoutStyle& s) { s.Placement = Position::Absolute; });
    auto inner = Box(*outer.Get(), [](LayoutStyle&) {});
    auto leaf = Fixed(*inner.Get(), 30.0f, 30.0f);
    bed.Layout();
    CHECK(outer->GetSize().y == doctest::Approx(30.0f));
    leaf->SetSize(core::Float2{30.0f, 70.0f});
    bed.Layout();
    CHECK(outer->GetSize().y == doctest::Approx(70.0f)); // up through the auto-sized containers
    inner->EditLayoutStyle([](LayoutStyle& s) { s.Width = LayoutLength::Px(100.0f); s.Height = LayoutLength::Px(100.0f); });
    bed.Layout();
    const core::u32 before = bed.root->LastLayoutCount();
    (void)before;
    leaf->SetSize(core::Float2{30.0f, 40.0f});
    bed.Layout();
    CHECK(outer->GetSize().y == doctest::Approx(100.0f)); // inner's size held: outer untouched
    CHECK(bed.root->LastLayoutCount() <= 2u);             // inner, and the leaf at its new size
    bed.Layout();
    CHECK(bed.root->LastLayoutCount() == 0u); // nothing waiting: nothing laid out
}

TEST_CASE("layout: a sheet's display, flex, grid and size properties reach the layout")
{
    Bed bed;
    auto grid = core::MakeRef<Panel>(core::DefaultAllocator());
    grid->AddClass(SV(u8"cards"));
    bed.root->AddChild(grid.Get());
    core::Array<core::RefPtr<Panel>> cards;
    for (int i = 0; i < 4; ++i)
    {
        cards.PushBack(core::MakeRef<Panel>(core::DefaultAllocator()));
        cards[cards.Size() - 1]->AddClass(SV(u8"card"));
        grid->AddChild(cards[cards.Size() - 1].Get());
    }
    StyleManager styles(CSSParser::Parse(SV(
        u8".cards { display: grid; grid-template-columns: repeat(2, 1fr); gap: 10px 20px; padding: 10px; }"
        u8".card { height: 40px; }")));
    styles.ApplyTree(*bed.root.Get());
    bed.Layout();
    CHECK(grid->GetLayoutStyle().Kind == Display::Grid);
    CHECK(grid->GetLayoutStyle().Columns.Size() == 2u);
    CHECK(cards[1]->GetPosition().x == doctest::Approx(10.0f + 180.0f + 20.0f)); // (380 - 20) / 2 a column
    CHECK(cards[1]->GetPosition().y == doctest::Approx(10.0f));
    CHECK(cards[2]->GetPosition().x == doctest::Approx(10.0f));
    CHECK(cards[2]->GetPosition().y == doctest::Approx(10.0f + 40.0f + 10.0f));

    StyleManager flex(CSSParser::Parse(SV(u8".cards { display: flex; flex-flow: row wrap; } .card { flex: 1; width: 25%; min-width: 50px; }")));
    flex.ApplyTree(*bed.root.Get());
    bed.Layout();
    CHECK(grid->GetLayoutStyle().Direction == FlexDirection::Row);
    CHECK(grid->GetLayoutStyle().Wrap == FlexWrap::Wrap);
    CHECK(cards[0]->GetLayoutStyle().Grow == doctest::Approx(1.0f));
    CHECK(cards[0]->GetLayoutStyle().Width.Kind == LayoutLengthKind::Percent);
}
