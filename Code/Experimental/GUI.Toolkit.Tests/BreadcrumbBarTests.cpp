// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Toolkit - the breadcrumb bar (T7 of Specs/gui-lab-evolution-3.md): segments carrying their
// target, from items or a path; a click navigating on release, the current segment inert; the
// segments that do not fit folded from the root into a "…" menu; one tab stop, the arrows moving
// along it; right to left, run the other way with the chevrons turned.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.fonts;
import experimental.gui;
import experimental.gui.toolkit;

using namespace experimental::gui;
namespace core = foundation::core;
namespace fonts = foundation::fonts;
using core::f32;
using core::usize;

namespace
{
    core::StringView SV(const char8_t* s) { return core::StringView(s); }

    // A mock font: 6px a byte, 12px a line.
    class SixFont : public fonts::IFont
    {
    public:
        core::StringView FamilyName() const override { return SV(u8"six"); }
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
        core::f32 PixelHeight() const override { return 10.0f; }
        fonts::GlyphInfo GetGlyphInfo(core::i32 cp) const override
        {
            fonts::GlyphInfo g;
            g.codepoint = cp;
            g.advanceWidth = 6.0f;
            return g;
        }
        core::f32 GetKerning(core::i32, core::i32) const override { return 0.0f; }
        bool HasGlyph(core::i32) const override { return true; }
        core::f32 MeasureString(core::StringView text) const override { return static_cast<core::f32>(text.Size()) * 6.0f; }
        core::f32 MeasureString(core::StringView text, core::Array<fonts::GlyphPosition>&) const override { return MeasureString(text); }
    };

    // A bar 400 wide at the root's top-left, over a path of five segments.
    struct Bed
    {
        fonts::CachedFont font{core::DefaultAllocator(), core::DefaultAllocator().New<SixFont>(), nullptr, nullptr}; // outlives the widgets
        core::RefPtr<SceneRoot> root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
        core::RefPtr<BreadcrumbBar> bar = core::MakeRef<BreadcrumbBar>(core::DefaultAllocator());
        core::Array<core::String> went{core::DefaultAllocator()};

        explicit Bed(f32 width = 400.0f)
        {
            root->SetSize(core::Float2{800.0f, 600.0f});
            root->EditLayoutStyle([](LayoutStyle& s) { s.AlignItems = Align::Start; });
            bar->EditLayoutStyle([width](LayoutStyle& s)
            {
                s.Width = LayoutLength::Px(width);
                s.Height = LayoutLength::Px(24.0f);
            });
            root->AddChild(bar.Get());
            bar->SetPath(SV(u8"/home/robert/Projects/Game"));
            Fonts();
            bar->SetOnNavigate([this](const BreadcrumbItem& item) { went.PushBack(item.Id); });
            Frame();
        }
        // The mock font on what shows text (a font object is not inherited; a family name is,
        // through a font service the bed has none of).
        void Fonts()
        {
            bar->GetOverflowButton().SetFont(&font);
            for (usize i = 0; i < bar->ItemCount(); ++i)
                bar->SegmentAt(i).SetFont(&font);
        }
        // A frame: styles (the bar's font reaching its segments), then layout.
        void Frame()
        {
            root->GetContext().Update(core::Duration::FromSeconds(0.016f));
            (void)root->UpdateLayout();
        }
        ~Bed()
        {
            root->GetEventDispatcher()->CloseAllPopups();
        }
        EventDispatcher& D() { return *root->GetEventDispatcher(); }
        [[nodiscard]] core::Float2 Middle(const Node& node) const
        {
            const Rect r = node.GetScreenBounds();
            return core::Float2{r.x + r.width * 0.5f, r.y + r.height * 0.5f};
        }
        void Click(core::Float2 at)
        {
            D().InjectMouseMove(at);
            D().InjectMouseDown(at, MouseButton::Left);
            D().InjectMouseUp(at, MouseButton::Left);
        }
        void Key(KeyCode key) { D().InjectKeyDown(static_cast<core::u32>(key)); }
    };
}

TEST_CASE("breadcrumb bar: segments from a path, each carrying the path up to it; the last the current one")
{
    Bed bed;
    REQUIRE(bed.bar->ItemCount() == 5);
    CHECK(bed.bar->ItemAt(0).Text == core::String(u8"/"));
    CHECK(bed.bar->ItemAt(0).Id == core::String(u8"/"));
    CHECK(bed.bar->ItemAt(2).Text == core::String(u8"robert"));
    CHECK(bed.bar->ItemAt(2).Id == core::String(u8"/home/robert"));
    CHECK(bed.bar->ItemAt(2).Tooltip == core::String(u8"/home/robert"));
    CHECK(bed.bar->ItemAt(4).Id == core::String(u8"/home/robert/Projects/Game"));
    CHECK(bed.bar->SegmentAt(4).HasClass(SV(u8"current")));
    CHECK_FALSE(bed.bar->SegmentAt(3).HasClass(SV(u8"current")));
    CHECK(bed.bar->SegmentAt(4).GetAccessDescription() == core::String(u8"current"));
    CHECK(bed.bar->SegmentAt(3).GetAccessDescription().IsEmpty());

    // A drive is a segment of its own; empty parts are skipped.
    bed.bar->SetPath(SV(u8"C:/Users//me/"));
    REQUIRE(bed.bar->ItemCount() == 3);
    CHECK(bed.bar->ItemAt(0).Text == core::String(u8"C:"));
    CHECK(bed.bar->ItemAt(1).Id == core::String(u8"C:/Users"));
    CHECK(bed.bar->ItemAt(2).Id == core::String(u8"C:/Users//me"));

    // Given items, as they are (an icon and a tooltip the application's).
    const BreadcrumbItem items[] = {
        BreadcrumbItem{core::String(u8"g1"), core::String(u8"Content"), core::String(u8"All content"), {}},
        BreadcrumbItem{core::String(u8"g7"), core::String(u8"models"), {}, {}},
    };
    bed.bar->SetItems(items);
    bed.Frame();
    REQUIRE(bed.bar->ItemCount() == 2);
    CHECK(bed.bar->SegmentAt(0).GetTooltipText() == SV(u8"All content"));
    CHECK(bed.bar->SegmentAt(1).HasClass(SV(u8"current")));
}

TEST_CASE("breadcrumb bar: a click navigates on release, with the segment's item; the current segment is inert")
{
    Bed bed;
    const core::Float2 home = bed.Middle(bed.bar->SegmentAt(1));
    bed.D().InjectMouseMove(home);
    bed.D().InjectMouseDown(home, MouseButton::Left);
    CHECK(bed.went.IsEmpty()); // not on the press
    bed.D().InjectMouseUp(home, MouseButton::Left);
    REQUIRE(bed.went.Size() == 1);
    CHECK(bed.went[0] == core::String(u8"/home"));

    bed.Click(bed.Middle(bed.bar->SegmentAt(4)));
    CHECK(bed.went.Size() == 1);
    CHECK_FALSE(bed.bar->Navigate(4));
    CHECK(bed.bar->Navigate(0));
    CHECK(bed.went.Back() == core::String(u8"/"));

    // Laid out in order, a chevron between each two.
    for (usize i = 1; i < 5; ++i)
    {
        CHECK(bed.bar->SeparatorBefore(i).GetScreenBounds().x >= bed.bar->SegmentAt(i - 1).GetScreenBounds().Right() - 0.5f);
        CHECK(bed.bar->SegmentAt(i).GetScreenBounds().x >= bed.bar->SeparatorBefore(i).GetScreenBounds().Right() - 0.5f);
    }
    CHECK(bed.bar->FoldedCount() == 0);
    CHECK_FALSE(bed.bar->GetOverflowButton().IsVisible());
}

TEST_CASE("breadcrumb bar: what does not fit folds from the root into the overflow menu, the current segment kept")
{
    Bed bed(150.0f);
    const usize folded = bed.bar->FoldedCount();
    REQUIRE(folded > 0);
    CHECK(folded < 5);
    CHECK(bed.bar->GetOverflowButton().IsVisible());
    for (usize i = 0; i < folded; ++i)
        CHECK_FALSE(bed.bar->SegmentAt(i).IsVisible());
    for (usize i = folded; i < 5; ++i)
        CHECK(bed.bar->SegmentAt(i).IsVisible());
    // Everything shown inside the bar, the "…" first.
    const Rect barAt = bed.bar->GetContentBounds();
    CHECK(bed.bar->SegmentAt(4).GetScreenBounds().Right() <= barAt.Right() + 0.5f);
    CHECK(bed.bar->GetOverflowButton().GetScreenBounds().x < bed.bar->SegmentAt(folded).GetScreenBounds().x);

    // Its menu lists the folded ones; choosing one navigates there.
    bed.Click(bed.Middle(bed.bar->GetOverflowButton()));
    Menu* menu = bed.bar->GetOverflowMenu();
    REQUIRE(menu != nullptr);
    usize items = 0;
    MenuItem* firstItem = nullptr;
    for (usize i = 0; i < menu->ChildCount(); ++i)
        if (MenuItem* item = core::Cast<MenuItem>(menu->GetChildAt(i)))
        {
            if (firstItem == nullptr)
                firstItem = item;
            ++items;
        }
    CHECK(items == folded);
    REQUIRE(firstItem != nullptr);
    CHECK(firstItem->GetText() == SV(u8"/"));
    (void)firstItem->HandleActivate();
    REQUIRE(bed.went.Size() == 1);
    CHECK(bed.went[0] == core::String(u8"/"));

    // Too narrow for even the current one: it takes what is left.
    bed.bar->EditLayoutStyle([](LayoutStyle& s) { s.Width = LayoutLength::Px(60.0f); });
    bed.Frame();
    CHECK(bed.bar->FoldedCount() == 4);
    CHECK(bed.bar->SegmentAt(4).GetScreenBounds().Right() <= bed.bar->GetScreenBounds().Right() + 0.5f);
}

TEST_CASE("breadcrumb bar: one tab stop, the current segment; Left, Right, Home and End move along it, Return navigates")
{
    Bed bed;
    for (usize i = 0; i < 4; ++i)
        CHECK_FALSE(bed.bar->SegmentAt(i).IsTabFocusable());
    CHECK(bed.bar->SegmentAt(4).IsTabFocusable());
    bed.bar->SegmentAt(4).RequestFocus();
    bed.Key(KeyCode::Left);
    CHECK(bed.bar->SegmentAt(3).IsFocused());
    CHECK(bed.bar->SegmentAt(3).IsTabFocusable()); // the tab stop goes with it
    CHECK_FALSE(bed.bar->SegmentAt(4).IsTabFocusable());
    bed.Key(KeyCode::Return);
    REQUIRE(bed.went.Size() == 1);
    CHECK(bed.went[0] == core::String(u8"/home/robert/Projects"));
    bed.Key(KeyCode::Home);
    CHECK(bed.bar->SegmentAt(0).IsFocused());
    bed.Key(KeyCode::Left); // at the start, it stays
    CHECK(bed.bar->SegmentAt(0).IsFocused());
    bed.Key(KeyCode::End);
    CHECK(bed.bar->SegmentAt(4).IsFocused());
    bed.Key(KeyCode::Right);
    CHECK(bed.bar->SegmentAt(4).IsFocused());
}

TEST_CASE("breadcrumb bar: right to left, its segments run from the right, its chevrons turned, Left toward the current one")
{
    Bed bed;
    bed.root->SetLayoutDirection(experimental::gui::Node::LayoutDirection::RightToLeft);
    bed.Frame();
    CHECK(bed.bar->SegmentAt(0).GetScreenBounds().x > bed.bar->SegmentAt(4).GetScreenBounds().x);
    CHECK(bed.bar->SegmentAt(0).GetScreenBounds().Right() <= bed.bar->GetScreenBounds().Right() + 0.5f);
    CHECK(bed.bar->SeparatorBefore(1).IsMirrored());
    bed.bar->SegmentAt(2).RequestFocus();
    bed.Key(KeyCode::Left); // toward the inline end
    CHECK(bed.bar->SegmentAt(3).IsFocused());
}

TEST_CASE("breadcrumb bar: an element markup places")
{
    RegisterGuiElements();
    RegisterToolkitElements();
    static const ElementSet elements = ElementSet::All();
    MarkupLoader loader(elements, core::DefaultAllocator());
    core::RefPtr<experimental::gui::Node> made = loader.LoadFromString(SV(u8R"(<Panel><BreadcrumbBar/></Panel>)"));
    REQUIRE(made);
    REQUIRE(made->ChildCount() == 1);
    CHECK(core::Cast<BreadcrumbBar>(made->GetChildAt(0)) != nullptr);
}
