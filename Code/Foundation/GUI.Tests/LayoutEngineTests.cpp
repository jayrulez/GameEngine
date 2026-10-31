// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - measure and arrange. A node asks for a size within constraints, and its container places
// it. Covers the display kinds (flex by CSS's rules, grid, stack, dock) and out-of-flow children;
// sizes in lengths, percentages and auto, min and max, and the box sizing; visibility against
// display; what a change lays out again; and the same driven from a sheet.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.fonts;
import foundation.vg;
import foundation.gui;

using namespace foundation::gui;
namespace core = foundation::core;
namespace fonts = foundation::fonts;
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
    CheckRect(*page, 0, 0, 400, 100); // stretched across the root, and as tall as its content
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
    CHECK(evenly[0] == doctest::Approx(67.0f)); // a third of 200, on the pixel grid
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
    CHECK(a->GetSize().x == doctest::Approx(150.0f)); // a quarter of the 200 free
    CHECK(b->GetSize().x == doctest::Approx(250.0f));

    a->EditLayoutStyle([](LayoutStyle& s) { s.Width = LayoutLength::Px(300.0f); s.Grow = 0.0f; s.MinWidth = LayoutLength::Px(280.0f); });
    b->EditLayoutStyle([](LayoutStyle& s) { s.Width = LayoutLength::Px(300.0f); s.Grow = 0.0f; });
    bed.Layout();
    CHECK(a->GetSize().x == doctest::Approx(280.0f)); // its share of the shrink would take it to 200
    CHECK(b->GetSize().x == doctest::Approx(120.0f));
    b->EditLayoutStyle([](LayoutStyle& s) { s.Shrink = 0.0f; });
    bed.Layout();
    CHECK(b->GetSize().x == doctest::Approx(300.0f)); // it does not shrink, so the line overflows
}

TEST_CASE("layout: a width of its own holds with no room; an item shrinks no narrower than a fixed child")
{
    // Measured with no room (its min-content), a node with a width keeps that width. The room
    // limits an auto size, not a size its style gives, unless the container fixes the size exactly.
    auto lone = core::MakeRef<Panel>(core::DefaultAllocator());
    lone->EditLayoutStyle([](LayoutStyle& s) { s.Width = LayoutLength::Px(120.0f); });
    CHECK(lone->Measure(LayoutConstraints{core::Float2{0.0f, 0.0f}, core::Float2{0.0f, kUnbounded}}).x == doctest::Approx(120.0f));
    CHECK(lone->Measure(LayoutConstraints::Tight(core::Float2{50.0f, 20.0f}, core::Float2{-1.0f, -1.0f})).x == doctest::Approx(50.0f));

    Bed bed;
    auto line = Box(*bed.root.Get(), [](LayoutStyle& s) {
        s.Height = LayoutLength::Px(40.0f);
        s.Direction = FlexDirection::Row;
    });
    auto a = Box(*line.Get(), [](LayoutStyle&) {}); // auto width, so its content's width
    (void)Fixed(*a.Get(), 150.0f, 20.0f);
    auto b = Box(*line.Get(), [](LayoutStyle& s) { s.Width = LayoutLength::Px(400.0f); });
    bed.Layout();
    CHECK(a->GetSize().x == doctest::Approx(150.0f)); // its automatic minimum is the fixed child
    CHECK(b->GetSize().x == doctest::Approx(250.0f)); // b takes all of the overflow
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
    first->EditLayoutStyle([](LayoutStyle& s) { s.Order = 1; }); // order 1 puts it after the other, so second from the right in a reversed row
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
    CHECK(content->GetSize().x == doctest::Approx(120.0f)); // its width is the content's, with the padding outside
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
    // 400 - 100 - 20 gaps = 280 for 3fr: 93.33 and 186.67, edges on whole pixels.
    CheckRect(*a, 0, 0, 100, 20);
    CHECK(b->GetPosition().x == doctest::Approx(110.0f));
    CHECK(b->GetSize().x == doctest::Approx(93.0f)); // 110 to 203.33
    CHECK(c->GetPosition().x == doctest::Approx(213.0f));
    CHECK(wide->GetPosition().y == doctest::Approx(30.0f + 5.0f)); // the first row is as tall as its tallest item
    CHECK(wide->GetSize().x == doctest::Approx(203.0f));
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
    CheckRect(*banner, 20, 250, 360, 40); // left and right stretch it, and bottom places it
    CHECK(second->GetPosition().y == doctest::Approx(30.0f)); // the banner takes no space
    CHECK(second->GetPosition().x == doctest::Approx(15.0f));
    CHECK(third->GetPosition().x == doctest::Approx(0.0f)); // its sibling stays where it was
    CHECK(third->GetPosition().y == doctest::Approx(60.0f));

    first->SetPosition(core::Float2{200.0f, 100.0f}); // set from code: out of the flow, at that place
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
    CHECK(outer->GetSize().y == doctest::Approx(100.0f)); // inner's size is fixed, so outer is untouched
    CHECK(bed.root->LastLayoutCount() <= 2u);             // inner, and the leaf at its new size
    bed.Layout();
    CHECK(bed.root->LastLayoutCount() == 0u); // nothing is waiting, so nothing is laid out
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
    CHECK(cards[1]->GetPosition().x == doctest::Approx(10.0f + 180.0f + 20.0f)); // each column is (380 - 20) / 2
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

namespace
{
    // 6px per byte, 12px per line.
    class SixFont : public fonts::IFont
    {
    public:
        core::StringView FamilyName() const override { return core::StringView(u8"six"); }
        core::Span<const core::u8> SourceData() const override { return {}; }
        fonts::FontMetrics Metrics() const override
        {
            fonts::FontMetrics m;
            m.ascent = 10.0f;
            m.descent = -2.0f;
            m.lineHeight = 12.0f;
            m.pixelHeight = 10.0f;
            m.scale = 1.0f;
            return m;
        }
        f32 PixelHeight() const override { return 10.0f; }
        fonts::GlyphInfo GetGlyphInfo(core::i32 cp) const override
        {
            fonts::GlyphInfo g;
            g.codepoint = cp;
            g.advanceWidth = 6.0f;
            return g;
        }
        f32 GetKerning(core::i32, core::i32) const override { return 0.0f; }
        bool HasGlyph(core::i32) const override { return true; }
        f32 MeasureString(core::StringView text) const override { return static_cast<f32>(text.Size()) * 6.0f; }
        f32 MeasureString(core::StringView text, core::Array<fonts::GlyphPosition>&) const override
        {
            return static_cast<f32>(text.Size()) * 6.0f;
        }
    };
}

TEST_CASE("layout: a label and a button size to their text, and follow it")
{
    fonts::CachedFont font(core::DefaultAllocator(), core::DefaultAllocator().New<SixFont>(), nullptr, nullptr);
    Bed bed;
    auto row = Box(*bed.root.Get(), [](LayoutStyle& s) { s.Direction = FlexDirection::Row; s.AlignItems = Align::Start; s.ColumnGap = 10.0f; });
    auto label = core::MakeRef<Label>(core::DefaultAllocator());
    label->SetFont(&font);
    label->SetText(SV(u8"Score"));
    label->SetPadding(Thickness{4.0f});
    row->AddChild(label.Get());
    auto button = core::MakeRef<Button>(core::DefaultAllocator());
    button->SetFont(&font);
    button->SetText(SV(u8"Play"));
    button->SetPadding(Thickness{10.0f, 6.0f});
    row->AddChild(button.Get());
    auto box = core::MakeRef<CheckBox>(core::DefaultAllocator());
    row->AddChild(box.Get());
    bed.Layout();
    CheckRect(*label, 0, 0, 5 * 6 + 8, 12 + 8);
    CheckRect(*button, 38 + 10, 0, 4 * 6 + 20, 12 + 12);
    CHECK(box->GetSize().x == doctest::Approx(20.0f));
    label->SetText(SV(u8"Score: 120"));
    bed.Layout();
    CHECK(label->GetSize().x == doctest::Approx(10 * 6 + 8));
    CHECK(button->GetPosition().x == doctest::Approx(68.0f + 10.0f)); // moved along with it
}

TEST_CASE("layout: a wrapping label wraps to the width it is given, and shrinks no narrower than its longest word")
{
    fonts::CachedFont font(core::DefaultAllocator(), core::DefaultAllocator().New<SixFont>(), nullptr, nullptr);
    Bed bed(400.0f, 300.0f);
    auto column = Box(*bed.root.Get(), [](LayoutStyle& s) { s.Width = LayoutLength::Px(100.0f); });
    auto text = core::MakeRef<Label>(core::DefaultAllocator());
    text->SetFont(&font);
    text->SetWhiteSpace(TextWhiteSpace::Normal);
    text->SetText(SV(u8"aaaa bbbb cccc dddd")); // words of 24 and spaces of 6, so four words take 114
    column->AddChild(text.Get());
    bed.Layout();
    CHECK(text->GetSize().x == doctest::Approx(100.0f)); // stretched across
    CHECK(text->GetSize().y == doctest::Approx(24.0f));  // two lines at 100 wide

    auto row = Box(*bed.root.Get(), [](LayoutStyle& s) { s.Direction = FlexDirection::Row; s.Width = LayoutLength::Px(60.0f); s.AlignItems = Align::Start; });
    auto squeezed = core::MakeRef<Label>(core::DefaultAllocator());
    squeezed->SetFont(&font);
    squeezed->SetWhiteSpace(TextWhiteSpace::Normal);
    squeezed->SetText(SV(u8"aaaaaaaa bb"));
    row->AddChild(squeezed.Get());
    (void)Fixed(*row.Get(), 50.0f, 10.0f);
    bed.Layout();
    CHECK(squeezed->GetSize().x == doctest::Approx(48.0f)); // its longest word, so the sibling overflows
    CHECK(squeezed->GetSize().y == doctest::Approx(24.0f));
}


TEST_CASE("layout: a row aligned by baseline lines up its items' text, and grows to hold them")
{
    // SixFont's ascent is 10 on a 12 line. A label's baseline is its padding plus the ascent,
    // since its line fills its content box, and a button's is the same. A plain box's baseline is
    // the bottom of its content box.
    fonts::CachedFont font(core::DefaultAllocator(), core::DefaultAllocator().New<SixFont>(), nullptr, nullptr);
    Bed bed;
    auto row = Box(*bed.root.Get(), [](LayoutStyle& s) { s.Direction = FlexDirection::Row; s.AlignItems = Align::Baseline; });
    auto label = core::MakeRef<Label>(core::DefaultAllocator());
    label->SetFont(&font);
    label->SetText(SV(u8"Name"));
    label->SetPadding(Thickness{4.0f}); // 20 tall, baseline 14
    row->AddChild(label.Get());
    auto button = core::MakeRef<Button>(core::DefaultAllocator());
    button->SetFont(&font);
    button->SetText(SV(u8"Go"));
    button->SetPadding(Thickness{10.0f, 6.0f}); // 24 tall, baseline 16
    row->AddChild(button.Get());
    auto box = Fixed(*row.Get(), 40.0f, 30.0f); // baseline 30
    auto pushed = core::MakeRef<Label>(core::DefaultAllocator());
    pushed->SetFont(&font);
    pushed->SetText(SV(u8"x"));
    pushed->SetPadding(Thickness{4.0f});
    pushed->SetMargin(Thickness{0.0f, 6.0f, 0.0f, 0.0f}); // its margin box's baseline is at 20
    row->AddChild(pushed.Get());
    bed.Layout();

    // The baselines meet at 30, the deepest baseline; each item sits so its baseline is there.
    CHECK(label->GetPosition().y == doctest::Approx(16.0f));
    CHECK(button->GetPosition().y == doctest::Approx(14.0f));
    CHECK(box->GetPosition().y == doctest::Approx(0.0f));
    CHECK(pushed->GetPosition().y == doctest::Approx(16.0f)); // 10 down, then its margin
    CHECK(label->GetPosition().y + label->GetBaseline() == doctest::Approx(30.0f));
    CHECK(button->GetPosition().y + button->GetBaseline() == doctest::Approx(30.0f));
    // The row also holds what reaches furthest below the baseline: the button's 8.
    CHECK(row->GetSize().y == doctest::Approx(38.0f));

    // In a column, baseline alignment places items as start does.
    row->EditLayoutStyle([](LayoutStyle& s) { s.Direction = FlexDirection::Column; });
    bed.Layout();
    CHECK(label->GetPosition().x == doctest::Approx(0.0f));
    CHECK(box->GetPosition().x == doctest::Approx(0.0f));
}

TEST_CASE("layout: a label's baseline follows its vertical alignment and its wrapping")
{
    fonts::CachedFont font(core::DefaultAllocator(), core::DefaultAllocator().New<SixFont>(), nullptr, nullptr);
    auto label = core::MakeRef<Label>(core::DefaultAllocator());
    label->SetFont(&font);
    label->SetText(SV(u8"aaaa bbbb"));
    label->SetPadding(Thickness{2.0f});
    const core::Float2 size{100.0f, 40.0f}; // a content box 36 tall, starting at 2
    label->SetTextAlignment(TextHAlign::Left, TextVAlign::Top);
    CHECK(label->BaselineAt(size) == doctest::Approx(2.0f + 10.0f));
    label->SetTextAlignment(TextHAlign::Left, TextVAlign::Middle);
    CHECK(label->BaselineAt(size) == doctest::Approx(2.0f + 12.0f + 10.0f));
    label->SetTextAlignment(TextHAlign::Left, TextVAlign::Bottom);
    CHECK(label->BaselineAt(size) == doctest::Approx(38.0f - 2.0f)); // the descent above the bottom
    // Wrapped to two lines in 30 wide and centred, the block of 24 starts 6 down, with its first
    // line's baseline 10 below that.
    label->SetWhiteSpace(TextWhiteSpace::Normal);
    label->SetTextAlignment(TextHAlign::Left, TextVAlign::Middle);
    CHECK(label->BaselineAt(core::Float2{34.0f, 40.0f}) == doctest::Approx(2.0f + 6.0f + 10.0f));
    // For any other node, the baseline is the bottom of its content box.
    auto panel = core::MakeRef<Panel>(core::DefaultAllocator());
    panel->SetPadding(Thickness{0.0f, 0.0f, 0.0f, 5.0f});
    CHECK(panel->BaselineAt(core::Float2{50.0f, 30.0f}) == doctest::Approx(25.0f));
}

TEST_CASE("layout: arranged edges round to the device grid at scales 1, 1.25 and 2")
{
    // Three equal items across 100 units have edges at 33.33 and 66.67. Each edge rounds on its
    // own, so neighbours still meet and the widths add up, and in device pixels every edge is whole.
    const f32 scales[] = {1.0f, 1.25f, 2.0f};
    for (const f32 scale : scales)
    {
        CAPTURE(scale);
        Bed bed(400.0f, 300.0f);
        bed.root->SetDisplayScale(scale);
        auto row = Box(*bed.root.Get(), [](LayoutStyle& s) {
            s.Direction = FlexDirection::Row;
            s.Width = LayoutLength::Px(100.0f);
            s.Height = LayoutLength::Px(10.3f);
        });
        row->SetPadding(Thickness{0.3f, 0.0f, 0.0f, 0.0f});
        core::RefPtr<Panel> cells[3];
        for (auto& cell : cells)
            cell = Box(*row.Get(), [](LayoutStyle& s) { s.Grow = 1.0f; });
        bed.Layout();
        const auto whole = [scale](f32 v) { return doctest::Approx(core::Round(v * scale)) == v * scale; };
        f32 x = cells[0]->GetPosition().x;
        for (const auto& cell : cells)
        {
            CHECK(whole(cell->GetPosition().x));
            CHECK(whole(cell->GetSize().x));
            CHECK(whole(cell->GetSize().y));
            CHECK(cell->GetPosition().x == doctest::Approx(x)); // meets its neighbour
            x += cell->GetSize().x;
        }
        CHECK(whole(row->GetSize().y));
        CHECK(row->GetPixelScale() == doctest::Approx(scale));
    }
}

TEST_CASE("layout: a new display scale lays the whole tree out again on its grid")
{
    Bed bed(400.0f, 300.0f);
    auto row = Box(*bed.root.Get(), [](LayoutStyle& s) { s.Direction = FlexDirection::Row; s.Width = LayoutLength::Px(100.0f); });
    auto inner = Box(*row.Get(), [](LayoutStyle& s) { s.Width = LayoutLength::Px(50.0f); s.Height = LayoutLength::Px(10.0f); });
    auto leaf = Box(*inner.Get(), [](LayoutStyle& s) { s.Width = LayoutLength::Px(10.3f); s.Height = LayoutLength::Px(4.0f); });
    bed.Layout();
    CHECK(leaf->GetSize().x == doctest::Approx(10.0f));
    bed.root->SetDisplayScale(2.0f);
    bed.Layout();
    CHECK(leaf->GetPixelScale() == doctest::Approx(2.0f));
    CHECK(leaf->GetSize().x == doctest::Approx(10.5f)); // 20.6 device pixels round to 21
}

namespace
{
    // The box covered by the vertices of one colour (matched by red, green and blue), and how
    // many vertices there are.
    struct Extent
    {
        core::usize count = 0;
        core::Float2 min{1.0e9f, 1.0e9f};
        core::Float2 max{-1.0e9f, -1.0e9f};
    };
    Extent ExtentOf(foundation::vg::VGContext& vg, core::Color color)
    {
        Extent e;
        for (const foundation::vg::VGVertex& v : vg.GetBatch().vertices)
            if (core::Abs(v.color.r - color.r) < 0.01f && core::Abs(v.color.g - color.g) < 0.01f &&
                core::Abs(v.color.b - color.b) < 0.01f)
            {
                ++e.count;
                e.min = core::Float2{core::Min(e.min.x, v.position.x), core::Min(e.min.y, v.position.y)};
                e.max = core::Float2{core::Max(e.max.x, v.position.x), core::Max(e.max.y, v.position.y)};
            }
        return e;
    }
}

TEST_CASE("layout: the overlay draws margins, padding, bounds and text baselines where layout put them")
{
    fonts::CachedFont font(core::DefaultAllocator(), core::DefaultAllocator().New<SixFont>(), nullptr, nullptr);
    Bed bed(200.0f, 200.0f);
    auto column = Box(*bed.root.Get(), [](LayoutStyle& s) { s.AlignItems = Align::Start; });
    auto box = Fixed(*column.Get(), 100.0f, 40.0f);
    box->SetMargin(Thickness{10.0f});
    box->SetPadding(Thickness{5.0f});
    auto label = core::MakeRef<Label>(core::DefaultAllocator());
    label->SetFont(&font);
    label->SetText(SV(u8"Hi"));
    column->AddChild(label.Get()); // at y 60, 12 tall, baseline 10 down
    bed.Layout();

    const core::Color margin{1.0f, 0.6f, 0.2f, 1.0f};
    const core::Color padding{0.3f, 0.85f, 0.4f, 1.0f};
    const core::Color baseline{1.0f, 0.2f, 0.9f, 1.0f};
    const auto draw = [&](const DebugOverlay& overlay)
    {
        bed.root->SetDebugOverlay(overlay);
        auto vg = core::MakeUnique<foundation::vg::VGContext>(core::DefaultAllocator());
        DrawContext dc{*vg};
        bed.root->Draw(dc);
        return vg;
    };
    {
        auto vg = draw(DebugOverlay{});
        CHECK(ExtentOf(*vg, margin).count == 0u);
        CHECK(ExtentOf(*vg, baseline).count == 0u);
    }
    {
        auto vg = draw(DebugOverlay{false, false, true, false});
        const Extent m = ExtentOf(*vg, margin); // the box's margin box: 0,0 to 120,60
        REQUIRE(m.count > 0u);
        CHECK(m.min.x == doctest::Approx(0.0f));
        CHECK(m.min.y == doctest::Approx(0.0f));
        CHECK(m.max.x == doctest::Approx(120.0f));
        CHECK(m.max.y == doctest::Approx(60.0f));
        CHECK(ExtentOf(*vg, padding).count == 0u);
    }
    {
        auto vg = draw(DebugOverlay{false, true, false, false});
        const Extent p = ExtentOf(*vg, padding); // within its border box: 10,10 to 110,50
        REQUIRE(p.count > 0u);
        CHECK(p.min.x == doctest::Approx(10.0f));
        CHECK(p.max.y == doctest::Approx(50.0f));
    }
    {
        auto vg = draw(DebugOverlay{false, false, false, true});
        const Extent b = ExtentOf(*vg, baseline); // only the label's, at about y 70
        REQUIRE(b.count > 0u);
        CHECK(b.min.y > 68.5f); // a hairline, with its edge fringe
        CHECK(b.max.y < 71.5f);
        CHECK(b.min.x == doctest::Approx(0.0f).epsilon(0.02));
    }
}

TEST_CASE("layout: a row's item that does not stretch is as tall as its content, overflowing a short row")
{
    Bed bed;
    auto row = Box(*bed.root.Get(), [](LayoutStyle& s) {
        s.Direction = FlexDirection::Row;
        s.Height = LayoutLength::Px(50.0f);
        s.AlignItems = Align::Start;
    });
    auto column = Box(*row.Get(), [](LayoutStyle&) {});
    auto a = Fixed(*column.Get(), 40.0f, 30.0f);
    auto b = Fixed(*column.Get(), 40.0f, 30.0f);
    auto c = Fixed(*column.Get(), 40.0f, 30.0f);
    bed.Layout();
    CHECK(column->GetSize().y == doctest::Approx(90.0f)); // not squeezed into the row's 50
    CHECK(c->GetPosition().y == doctest::Approx(60.0f));
    CHECK(c->GetSize().y == doctest::Approx(30.0f));
    (void)a;
    (void)b;

    // Stretched, it takes the row's height.
    row->EditLayoutStyle([](LayoutStyle& s) { s.AlignItems = Align::Stretch; });
    bed.Layout();
    CHECK(column->GetSize().y == doctest::Approx(50.0f));
}

namespace
{
    // A leaf that counts how often layout asks what it holds.
    class CountingLeaf : public Node
    {
    public:
        core::Float2 MeasureContent(core::Float2) override
        {
            ++Measures;
            return core::Float2{50.0f, 20.0f};
        }
        int Measures = 0;
    };
}

TEST_CASE("layout: deep nesting measures each node a bounded number of times, not twice per level")
{
    // Each flex level measures its item for the base size and again at the resolved size. With a
    // one-entry measure cache that would double per level, about a billion measures for 30
    // levels.
    Bed bed;
    Node* at = bed.root.Get();
    for (int i = 0; i < 30; ++i)
    {
        auto level = core::MakeRef<Panel>(core::DefaultAllocator());
        level->SetPadding(Thickness{1.0f});
        at->AddChild(level.Get());
        at = level.Get();
    }
    auto leaf = core::MakeRef<CountingLeaf>(core::DefaultAllocator());
    at->AddChild(leaf.Get());
    bed.Layout();
    CHECK(leaf->Measures <= 8);
    CHECK(leaf->GetSize().x == doctest::Approx(400.0f - 2.0f * 30.0f)); // stretched across each level
    CHECK(leaf->GetSize().y == doctest::Approx(20.0f));
}

TEST_CASE("layout: a wrapping row in a column is as tall as its lines at the width it gets")
{
    // Six items of 80 in a row 300 wide wrap to two lines of three. The row is two lines tall, and
    // what follows it in the column comes after both.
    Bed bed(300.0f, 400.0f);
    auto column = Box(*bed.root.Get(), [](LayoutStyle& s)
    {
        s.Direction = FlexDirection::Column;
        s.Width = LayoutLength::Px(300.0f);
    });
    auto row = Box(*column.Get(), [](LayoutStyle& s)
    {
        s.Direction = FlexDirection::Row;
        s.Wrap = FlexWrap::Wrap;
    });
    for (int i = 0; i < 6; ++i)
        (void)Fixed(*row.Get(), 80.0f, 20.0f);
    auto after = Fixed(*column.Get(), 50.0f, 10.0f);
    bed.Layout();
    CHECK(row->GetSize().y == doctest::Approx(40.0f));
    CHECK(after->GetPosition().y == doctest::Approx(40.0f));
}

TEST_CASE("layout: a wrapping row whose items grow after a layout wraps and grows with them")
{
    // The inspector's toggles get their font after they are first laid out, so they are narrow
    // at first (one line) and then 447 wide in all within 440 (two lines). The row must grow to
    // hold both lines.
    Bed bed(600.0f, 600.0f);
    auto scroller = Box(*bed.root.Get(), [](LayoutStyle& s)
    {
        s.Width = LayoutLength::Px(448.0f);
        s.Height = LayoutLength::Px(300.0f);
        s.OverflowX = Overflow::Hidden;
        s.OverflowY = Overflow::Auto;
    });
    auto column = Box(*scroller.Get(), [](LayoutStyle& s) { s.Direction = FlexDirection::Column; });
    column->SetPadding(Thickness{4.0f});
    auto row = Box(*column.Get(), [](LayoutStyle& s)
    {
        s.Direction = FlexDirection::Row;
        s.Wrap = FlexWrap::Wrap;
        s.ColumnGap = 8.0f;
        s.RowGap = 8.0f;
    });
    core::Array<core::RefPtr<Panel>> items;
    for (int i = 0; i < 6; ++i)
        items.PushBack(Fixed(*row.Get(), 30.0f, 48.0f));
    auto after = Fixed(*column.Get(), 50.0f, 10.0f);
    bed.Layout();
    CHECK(row->GetSize().y == doctest::Approx(48.0f));
    const f32 widths[6] = {57.0f, 58.0f, 57.0f, 93.0f, 71.0f, 71.0f};
    for (core::usize i = 0; i < 6; ++i)
        items[i]->SetSize(core::Float2{widths[i], 48.0f});
    bed.Layout();
    CHECK(row->GetSize().y == doctest::Approx(48.0f + 8.0f + 48.0f));
    CHECK(after->GetPosition().y == doctest::Approx(4.0f + 104.0f));
}

TEST_CASE("layout: each line of a wrapping row is as tall as its own items, not as the whole row")
{
    // Two lines of items 20 tall with a gap of 8: the row is 48 tall; the second line starts at 28
    // and stretched items are 20 tall (CSS gives a line the container's whole cross size only
    // when the container is single-line).
    Bed bed(600.0f, 600.0f);
    auto row = Box(*bed.root.Get(), [](LayoutStyle& s)
    {
        s.Direction = FlexDirection::Row;
        s.Wrap = FlexWrap::Wrap;
        s.ColumnGap = 8.0f;
        s.RowGap = 8.0f;
        s.Width = LayoutLength::Px(100.0f);
    });
    core::Array<core::RefPtr<Panel>> items;
    for (int i = 0; i < 4; ++i)
        items.PushBack(Box(*row.Get(), [](LayoutStyle& s) { s.Width = LayoutLength::Px(40.0f); s.MinHeight = LayoutLength::Px(20.0f); }));
    bed.Layout();
    CHECK(row->GetSize().y == doctest::Approx(48.0f));
    CHECK(items[0]->GetSize().y == doctest::Approx(20.0f));
    CHECK(items[2]->GetPosition().y == doctest::Approx(28.0f));
    CHECK(items[2]->GetSize().y == doctest::Approx(20.0f));
}

TEST_CASE("layout: a single-line row measured under a height limit is as tall as its content, not the limit")
{
    // A checkbox is such a row (its box and its label). Asked for its height with room for 48, it
    // must not answer 48, or a wrapping row of checkboxes makes each line 48 tall and the second
    // line falls out.
    Bed bed(600.0f, 600.0f);
    auto row = Box(*bed.root.Get(), [](LayoutStyle& s) { s.Direction = FlexDirection::Row; });
    (void)Fixed(*row.Get(), 20.0f, 20.0f);
    (void)Fixed(*row.Get(), 30.0f, 13.0f);
    LayoutConstraints limit;
    limit.Max = core::Float2{100.0f, 48.0f};
    CHECK(row->Measure(limit).y == doctest::Approx(20.0f));
    // Given its height exactly, it takes that height.
    CHECK(row->Measure(LayoutConstraints::Tight(core::Float2{100.0f, 48.0f}, core::Float2{-1.0f, -1.0f})).y == doctest::Approx(48.0f));
}

TEST_CASE("layout: a dock's fill takes what every side left, wherever it stands among them")
{
    Bed bed;
    auto dock = Box(*bed.root.Get(), [](LayoutStyle& s) { s.Kind = Display::Dock; s.Height = LayoutLength::Px(300.0f); });
    auto fill = Box(*dock.Get(), [](LayoutStyle&) {}); // added first, though it fills
    auto left = Box(*dock.Get(), [](LayoutStyle& s) { s.DockSide = Dock::Left; s.Width = LayoutLength::Px(80.0f); });
    auto top = Box(*dock.Get(), [](LayoutStyle& s) { s.DockSide = Dock::Top; s.Height = LayoutLength::Px(30.0f); });
    bed.Layout();
    CheckRect(*left, 0, 0, 80, 300);
    CheckRect(*top, 80, 0, 320, 30);
    CheckRect(*fill, 80, 30, 320, 270); // not under the sides
}

TEST_CASE("layout: a sheet's insets are from inside the border; a position code gives is in the box")
{
    Bed bed;
    auto holder = Box(*bed.root.Get(), [](LayoutStyle& s) { s.Width = LayoutLength::Px(200.0f); s.Height = LayoutLength::Px(100.0f); });
    NodeBox box;
    box.BorderWidths = foundation::vg::BorderWidths(5.0f);
    holder->SetBox(box);
    auto styled = Box(*holder.Get(), [](LayoutStyle& s) {
        s.Placement = Position::Absolute;
        s.Left = LayoutLength::Px(10.0f);
        s.Top = LayoutLength::Px(10.0f);
        s.Width = LayoutLength::Px(20.0f);
        s.Height = LayoutLength::Px(20.0f);
    });
    auto placed = Fixed(*holder.Get(), 20.0f, 20.0f);
    placed->SetPosition(core::Float2{10.0f, 10.0f});
    bed.Layout();
    CheckRect(*styled, 15, 15, 20, 20); // 10 inside the 5-wide border
    CheckRect(*placed, 10, 10, 20, 20); // where code put it
}

TEST_CASE("layout: percentage margins, padding and gaps are of the container's content size, as CSS's")
{
    Bed bed; // 400 wide
    auto row = Box(*bed.root.Get(), [](LayoutStyle& s) { s.Direction = FlexDirection::Row; s.AlignItems = Align::Start; });
    auto a = Fixed(*row.Get(), 50.0f, 20.0f);
    auto b = Fixed(*row.Get(), 50.0f, 20.0f);
    StyleManager styles(CSSParser::Parse(SV(u8".row { column-gap: 10%; padding-left: 5%; } .a { margin-left: 25%; }")));
    row->AddClass(SV(u8"row"));
    a->AddClass(SV(u8"a"));
    styles.ApplyTree(*bed.root.Get());
    bed.Layout();
    CHECK(row->GetPadding().Left == doctest::Approx(20.0f)); // 5% of the root's 400
    // a starts after the padding (20) and its margin (25% of the row's content width of 380, so
    // 95).
    CHECK(a->GetPosition().x == doctest::Approx(20.0f + 95.0f));
    CHECK(b->GetPosition().x == doctest::Approx(20.0f + 95.0f + 50.0f + 38.0f)); // the gap: 10% of 380
}

TEST_CASE("layout: calc() mixes pixels and percentages where percentages resolve")
{
    Bed bed; // 400 wide
    auto row = Box(*bed.root.Get(), [](LayoutStyle& s) { s.Direction = FlexDirection::Row; s.AlignItems = Align::Start; });
    // Their heights come from code and their widths from the sheet.
    auto a = Box(*row.Get(), [](LayoutStyle& s) { s.Height = LayoutLength::Px(20.0f); });
    auto b = Box(*row.Get(), [](LayoutStyle& s) { s.Height = LayoutLength::Px(20.0f); });
    row->AddClass(SV(u8"row"));
    a->AddClass(SV(u8"a"));
    b->AddClass(SV(u8"b"));
    StyleManager styles(CSSParser::Parse(SV(u8".row { padding-left: calc(10% - 20px); column-gap: calc(10px + 5%); }"
                                             u8".a { width: calc(50% - 2 * 10px); margin-left: calc((10% + 4px) / 2); }"
                                             u8".b { width: calc(1em + 4px); }")));
    styles.ApplyTree(*bed.root.Get());
    bed.Layout();
    CHECK(a->GetLayoutStyle().Width.Kind == LayoutLengthKind::Calc);
    CHECK(row->GetPadding().Left == doctest::Approx(20.0f)); // 10% of 400, less 20
    // The row's content is 380 wide: a is half of it less 20, after its margin (5% of 380, plus
    // 2).
    CHECK(a->GetSize().x == doctest::Approx(170.0f));
    CHECK(a->GetPosition().x == doctest::Approx(20.0f + 21.0f));
    CHECK(b->GetLayoutStyle().Width.Kind == LayoutLengthKind::Px); // no percent part, so plain pixels
    CHECK(b->GetSize().x == doctest::Approx(20.0f));                // 1em of 16, plus 4
    CHECK(b->GetPosition().x == doctest::Approx(41.0f + 170.0f + 29.0f)); // the gap is 10 plus 5% of 380
}

TEST_CASE("layout: fit-content is as wide as what it holds, not stretched across")
{
    Bed bed;
    auto column = Box(*bed.root.Get(), [](LayoutStyle&) {}); // a column stretches its children across
    auto fit = Box(*column.Get(), [](LayoutStyle& s) { s.Width = LayoutLength{LayoutLengthKind::FitContent, 0.0f}; });
    Fixed(*fit.Get(), 60.0f, 20.0f);
    bed.Layout();
    CHECK(fit->GetSize().x == doctest::Approx(60.0f));
}

TEST_CASE("layout: z-index orders siblings for drawing and the pointer, else they stand as they are")
{
    Bed bed;
    auto stack = Box(*bed.root.Get(), [](LayoutStyle& s) { s.Kind = Display::Stack; s.Width = LayoutLength::Px(100.0f); s.Height = LayoutLength::Px(100.0f); });
    auto under = Fixed(*stack.Get(), 100.0f, 100.0f);
    auto over = Fixed(*stack.Get(), 100.0f, 100.0f);
    bed.Layout();
    CHECK(bed.root->OverFind(core::Float2{50.0f, 50.0f}) == over.Get()); // the later sibling is on top
    under->EditLayoutStyle([](LayoutStyle& s) { s.ZIndex = 1; });
    bed.Layout();
    CHECK(bed.root->OverFind(core::Float2{50.0f, 50.0f}) == under.Get()); // raised above its sibling
    core::Array<Node*> order;
    stack->ChildrenInPaintOrder(order);
    REQUIRE(order.Size() == 2u);
    CHECK(order[1] == under.Get()); // drawn last
}

namespace
{
    // A leaf whose content is off the device grid, as shaped text is.
    class FractionalLeaf : public Node
    {
    public:
        core::Float2 MeasureContent(core::Float2) override { return core::Float2{29.06f, 15.2f}; }
    };
}

TEST_CASE("layout: a content size rounds up to the device grid, so a box keeps all of its content")
{
    // Each edge rounds to the grid on its own, so a label 29.06 wide placed off the grid could
    // come out 29 wide, and its text would end in an ellipsis. Measured up to the grid, it is 30
    // wide wherever it lands.
    const f32 scales[] = {1.0f, 2.0f};
    for (const f32 scale : scales)
    {
        Bed bed;
        bed.root->SetDisplayScale(scale);
        auto row = Box(*bed.root.Get(), [](LayoutStyle& s)
        {
            s.Direction = FlexDirection::Row;
            s.AlignItems = Align::Start;
        });
        row->SetPadding(Thickness{7.3f, 0.0f, 7.3f, 0.0f}); // the leaf lands off the grid
        auto leaf = core::MakeRef<FractionalLeaf>(core::DefaultAllocator());
        leaf->EditLayoutStyle([](LayoutStyle& s)
        {
            s.Shrink = 1.0f;
            s.MinWidth = LayoutLength::Px(0.0f);
        });
        row->AddChild(leaf.Get());
        bed.Layout();
        const f32 wide = core::Ceil(29.06f * scale) / scale;
        const f32 tall = core::Ceil(15.2f * scale) / scale;
        CHECK(leaf->GetSize().x == doctest::Approx(wide));
        CHECK(leaf->GetSize().y == doctest::Approx(tall));
        CHECK(leaf->GetSize().x >= 29.06f);
    }
}

TEST_CASE("layout: a row whose items grow is as wide as its items when measured, not the space offered")
{
    // A button's text grows to fill the button. Centred in a column 300 wide, the button must not
    // measure 300 wide (its text grown into all of it) and fill the column. As in CSS, a row's
    // content size is its items before they grow, and they then grow within the size the row is
    // given.
    Bed bed;
    auto column = Box(*bed.root.Get(), [](LayoutStyle& s)
    {
        s.Direction = FlexDirection::Column;
        s.Width = LayoutLength::Px(300.0f);
        s.AlignItems = Align::Center;
    });
    auto button = Box(*column.Get(), [](LayoutStyle& s) { s.Direction = FlexDirection::Row; });
    auto icon = Fixed(*button.Get(), 16.0f, 16.0f);
    auto text = Box(*button.Get(), [](LayoutStyle& s)
    {
        s.Grow = 1.0f;
        s.Width = LayoutLength::Px(40.0f);
        s.Height = LayoutLength::Px(16.0f);
    });
    bed.Layout();
    CHECK(button->GetSize().x == doctest::Approx(56.0f));
    CHECK(button->GetPosition().x == doctest::Approx((300.0f - 56.0f) * 0.5f));
    CHECK(text->GetSize().x == doctest::Approx(40.0f));

    // Stretched across the column, it is the column's width and its text grows into it.
    column->EditLayoutStyle([](LayoutStyle& s) { s.AlignItems = Align::Stretch; });
    bed.Layout();
    CHECK(button->GetSize().x == doctest::Approx(300.0f));
    CHECK(text->GetSize().x == doctest::Approx(284.0f));
}

TEST_CASE("layout: a scroller in a column has no automatic minimum on the axis it scrolls: its content scrolls inside it")
{
    Bed bed;
    auto column = Box(*bed.root, [](LayoutStyle& s)
                      {
                          s.Direction = FlexDirection::Column;
                          s.Width = LayoutLength::Px(200.0f);
                          s.Height = LayoutLength::Px(200.0f);
                      });
    // A virtualised list whose rows are all one height realises the rows its height shows.
    core::Array<core::String> items;
    for (int i = 0; i < 200; ++i)
        items.PushBack(core::String(SV(u8"row")));
    StringListModel model(core::Move(items));
    auto list = core::MakeRef<ListView>(core::DefaultAllocator());
    list->EditLayoutStyle([](LayoutStyle& s) { s.Grow = 1.0f; });
    list->SetRowHeight(20.0f);
    column->AddChild(list.Get());
    list->SetModel(&model);
    for (int pass = 0; pass < 5; ++pass)
    {
        bed.Layout();
        list->RequestLayout(); // asked again, as a new line in a frame does
    }
    bed.Layout();
    CHECK(list->GetSize().y == doctest::Approx(200.0f)); // not the rows it realised, nor all of them
    CHECK(list->GetScrollRange().y == doctest::Approx(200.0f * 20.0f - 200.0f));
    list->SetModel(nullptr); // the model was made after the bed, so it dies before the bed's tree
}
