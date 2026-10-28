// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Style model v2, P4 (ui-layout-and-style-model.md section 5): Flex line wrapping with
// align-content, gaps on both axes (Spacing/LineSpacing and the CSS row/column gaps), flex-basis
// (inline and from the sheet), the markup vocabulary, and `text-overflow: ellipsis` reaching
// Label from the cascade.

#include <doctest/doctest.h>

#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.ui;

#include "TestHelpers.h"

using namespace foundation::ui;
using namespace foundation::ui::tests;
using namespace foundation::core;
namespace core = foundation::core;

namespace
{
    void EnsureGlobals()
    {
        StyleSheetLoader::InitializeGlobals();
        MarkupLoader::Initialize();
        UITypeRegistry::Register(u8"View", &View::StaticType());
        UITypeRegistry::Register(u8"RootView", &RootView::StaticType());
        UITypeRegistry::Register(u8"TestView", &TestView::StaticType());
        UITypeRegistry::Register(u8"TestGroup", &TestGroup::StaticType());
    }

    core::RefPtr<StyleSheet> LoadSSS(StringView src)
    {
        EnsureGlobals();
        StyleSheetLoader loader(DefaultAllocator());
        return loader.Load(src);
    }

    core::RefPtr<RootView> MakeRoot() { return core::MakeRef<RootView>(core::DefaultAllocator()); }
    core::RefPtr<TestView> TV(f32 w, f32 h) { return core::MakeRef<TestView>(core::DefaultAllocator(), w, h); }
    template <typename T>
    core::RefPtr<T> New()
    {
        return core::MakeRef<T>(core::DefaultAllocator());
    }

    /// A Flex of a FIXED size hosted in a Frame (the root would stretch it to the viewport).
    struct FlexFixture
    {
        UIContext ctx{DefaultAllocator()};
        core::RefPtr<RootView> root = MakeRoot();
        core::RefPtr<FrameLayout> host = New<FrameLayout>();
        core::RefPtr<FlexLayout> flex = New<FlexLayout>();

        FlexFixture(f32 width, f32 height, bool wrap = true, Orientation direction = Orientation::Horizontal)
        {
            Init(ctx, root.Get(), 800, 600);
            LayoutStyle size;
            size.Width = SizeSpec::Fixed(Unit::Dp(width));
            size.Height = SizeSpec::Fixed(Unit::Dp(height));
            flex->SetLayout(size);
            flex->Wrap = wrap;
            flex->Direction = direction;
            host->AddView(flex.Get());
            root->AddView(host.Get());
        }
        void Pass() { LayoutPass(ctx, root.Get()); }
    };
}

// === wrapping ===

TEST_CASE("flex-wrap: items break into lines against the main size; line count and positions")
{
    FlexFixture f(250, 300);
    f.flex->AlignContent = AlignContent::Start;
    core::RefPtr<TestView> items[5];
    for (auto& item : items)
    {
        item = TV(100, 30);
        f.flex->AddView(item.Get());
    }
    f.Pass();
    // 250 wide: two 100s per line -> [a b] [c d] [e]
    CHECK(items[0]->Bounds.x == doctest::Approx(0));
    CHECK(items[1]->Bounds.x == doctest::Approx(100));
    CHECK(items[1]->Bounds.y == doctest::Approx(0));
    CHECK(items[2]->Bounds.x == doctest::Approx(0));
    CHECK(items[2]->Bounds.y == doctest::Approx(30));
    CHECK(items[3]->Bounds.y == doctest::Approx(30));
    CHECK(items[4]->Bounds.x == doctest::Approx(0));
    CHECK(items[4]->Bounds.y == doctest::Approx(60));

    // Without wrap the same items run past the edge on one line.
    f.flex->Wrap = false;
    f.flex->Invalidate();
    f.Pass();
    CHECK(items[4]->Bounds.x == doctest::Approx(400));
    CHECK(items[4]->Bounds.y == doctest::Approx(0));
}

TEST_CASE("flex-wrap: a wrapping container measures its stacked lines")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 800, 600);
    auto host = New<FrameLayout>();
    auto flex = New<FlexLayout>();
    flex->Wrap = true;
    flex->LineSpacing = 10;
    flex->Spacing = 5;
    LayoutStyle width;
    width.Width = SizeSpec::Fixed(Unit::Dp(250)); // height wraps
    flex->SetLayout(width);
    core::RefPtr<TestView> items[5];
    for (auto& item : items)
    {
        item = TV(100, 30);
        flex->AddView(item.Get());
    }
    host->AddView(flex.Get());
    root->AddView(host.Get());
    LayoutPass(ctx, root.Get());
    // 100 + 5 + 100 = 205 fits, a third (310) does not: three lines of 30 + two 10 gaps.
    CHECK(flex->Bounds.height == doctest::Approx(30 + 10 + 30 + 10 + 30));
    CHECK(items[1]->Bounds.x == doctest::Approx(105));
    CHECK(items[2]->Bounds.y == doctest::Approx(40));
    CHECK(items[4]->Bounds.y == doctest::Approx(80));
}

TEST_CASE("flex-wrap: vertical direction wraps into columns")
{
    FlexFixture f(300, 100, true, Orientation::Vertical);
    f.flex->AlignContent = AlignContent::Start;
    core::RefPtr<TestView> items[4];
    for (auto& item : items)
    {
        item = TV(50, 30);
        f.flex->AddView(item.Get());
    }
    f.Pass();
    // 100 tall: three 30s per column -> the fourth starts a second column at x = 50.
    CHECK(items[2]->Bounds.y == doctest::Approx(60));
    CHECK(items[3]->Bounds.x == doctest::Approx(50));
    CHECK(items[3]->Bounds.y == doctest::Approx(0));
}

TEST_CASE("flex-wrap: align-content packs the lines on the cross axis")
{
    // 200 tall, three lines of 30 -> 110 free.
    auto build = [](FlexFixture& f, core::RefPtr<TestView> (&items)[5]) {
        for (auto& item : items)
        {
            item = TV(100, 30);
            f.flex->AddView(item.Get());
        }
    };
    SUBCASE("start")
    {
        FlexFixture f(250, 200);
        f.flex->AlignContent = AlignContent::Start;
        core::RefPtr<TestView> items[5];
        build(f, items);
        f.Pass();
        CHECK(items[0]->Bounds.y == doctest::Approx(0));
        CHECK(items[4]->Bounds.y == doctest::Approx(60));
        CHECK(items[0]->Bounds.height == doctest::Approx(30)); // lines keep their size
    }
    SUBCASE("end")
    {
        FlexFixture f(250, 200);
        f.flex->AlignContent = AlignContent::End;
        core::RefPtr<TestView> items[5];
        build(f, items);
        f.Pass();
        CHECK(items[0]->Bounds.y == doctest::Approx(110));
        CHECK(items[4]->Bounds.y == doctest::Approx(170));
    }
    SUBCASE("center")
    {
        FlexFixture f(250, 200);
        f.flex->AlignContent = AlignContent::Center;
        core::RefPtr<TestView> items[5];
        build(f, items);
        f.Pass();
        CHECK(items[0]->Bounds.y == doctest::Approx(55));
    }
    SUBCASE("space-between")
    {
        FlexFixture f(250, 200);
        f.flex->AlignContent = AlignContent::SpaceBetween;
        core::RefPtr<TestView> items[5];
        build(f, items);
        f.Pass();
        CHECK(items[0]->Bounds.y == doctest::Approx(0));
        CHECK(items[2]->Bounds.y == doctest::Approx(85));
        CHECK(items[4]->Bounds.y == doctest::Approx(170));
    }
    SUBCASE("space-around")
    {
        FlexFixture f(250, 200);
        f.flex->AlignContent = AlignContent::SpaceAround;
        core::RefPtr<TestView> items[5];
        build(f, items);
        f.Pass();
        // 110 / 3 = 36.67 around each line: half before the first (bounds snap to the pixel grid).
        CHECK(items[0]->Bounds.y == doctest::Approx(110.0f / 6.0f).epsilon(0.03));
        CHECK(items[2]->Bounds.y == doctest::Approx(110.0f / 6.0f + 30 + 110.0f / 3.0f).epsilon(0.03));
    }
    SUBCASE("stretch (the default) shares the free space between lines and stretches items")
    {
        FlexFixture f(250, 200);
        CHECK(f.flex->AlignContent == AlignContent::Stretch);
        core::RefPtr<TestView> items[5];
        build(f, items);
        f.Pass();
        const f32 lineCross = 30 + 110.0f / 3.0f; // bounds snap to the pixel grid
        CHECK(items[0]->Bounds.height == doctest::Approx(lineCross).epsilon(0.03)); // AlignItems stretch, auto height
        CHECK(items[2]->Bounds.y == doctest::Approx(lineCross).epsilon(0.03));
        CHECK(items[4]->Bounds.y == doctest::Approx(2 * lineCross).epsilon(0.03));
    }
}

TEST_CASE("flex-wrap: a single line without wrap ignores align-content and fills the cross axis")
{
    FlexFixture f(400, 200, false);
    f.flex->AlignContent = AlignContent::End;
    auto a = TV(100, 30);
    f.flex->AddView(a.Get());
    f.Pass();
    CHECK(a->Bounds.y == doctest::Approx(0));
    CHECK(a->Bounds.height == doctest::Approx(200)); // stretched, as before P4
}

// === gaps ===

TEST_CASE("flex-wrap: gaps on both axes, by axis name and by direction")
{
    SUBCASE("row: column-gap is the main gap, row-gap the line gap")
    {
        FlexFixture f(250, 300);
        f.flex->AlignContent = AlignContent::Start;
        f.flex->ColumnGap = 20;
        f.flex->RowGap = 7;
        auto a = TV(100, 30);
        auto b = TV(100, 30);
        auto c = TV(100, 30);
        f.flex->AddView(a.Get());
        f.flex->AddView(b.Get());
        f.flex->AddView(c.Get());
        f.Pass();
        CHECK(f.flex->MainGap() == doctest::Approx(20));
        CHECK(f.flex->CrossGap() == doctest::Approx(7));
        CHECK(b->Bounds.x == doctest::Approx(120));
        CHECK(c->Bounds.y == doctest::Approx(37));
    }
    SUBCASE("column: row-gap is the main gap")
    {
        FlexFixture f(300, 100, true, Orientation::Vertical);
        f.flex->AlignContent = AlignContent::Start;
        f.flex->RowGap = 20;
        f.flex->ColumnGap = 7;
        auto a = TV(50, 30);
        auto b = TV(50, 30);
        auto c = TV(50, 30);
        f.flex->AddView(a.Get());
        f.flex->AddView(b.Get());
        f.flex->AddView(c.Get()); // 30 + 20 + 30 + 20 + 30 = 130 > 100: wraps
        f.Pass();
        CHECK(b->Bounds.y == doctest::Approx(50));
        CHECK(c->Bounds.x == doctest::Approx(57));
        CHECK(c->Bounds.y == doctest::Approx(0));
    }
    SUBCASE("Spacing/LineSpacing stay the code-side names when no axis gap is set")
    {
        FlexFixture f(250, 300);
        f.flex->Spacing = 10;
        f.flex->LineSpacing = 4;
        CHECK(f.flex->MainGap() == doctest::Approx(10));
        CHECK(f.flex->CrossGap() == doctest::Approx(4));
    }
}

// === flex-basis ===

TEST_CASE("flex-wrap: flex-basis is the starting main size, inline or from the sheet")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    EnsureGlobals();
    Init(ctx, root.Get(), 800, 600);
    ctx.SetStyleSheet(LoadSSS(u8".half { flex-basis: 50%; }"));
    auto host = New<FrameLayout>();
    auto flex = New<FlexLayout>();
    LayoutStyle size;
    size.Width = SizeSpec::Fixed(Unit::Dp(300));
    size.Height = SizeSpec::Fixed(Unit::Dp(50));
    flex->SetLayout(size);

    auto grown = TV(50, 30); // basis 100 + share
    LayoutStyle g;
    g.FlexGrow = 1;
    g.FlexBasis = Unit::Dp(100);
    flex->AddView(grown.Get(), g);
    auto bare = TV(50, 30); // basis 0 + share (the `flex: 1` shorthand)
    LayoutStyle b;
    b.FlexGrow = 1;
    flex->AddView(bare.Get(), b);
    host->AddView(flex.Get());
    root->AddView(host.Get());
    LayoutPass(ctx, root.Get());
    // remaining = 300 - 100 = 200, split 1:1.
    CHECK(grown->Bounds.width == doctest::Approx(200));
    CHECK(bare->Bounds.width == doctest::Approx(100));
    CHECK(bare->Bounds.x == doctest::Approx(200));

    // A basis without grow IS the size (the content size is ignored); from the sheet, percent
    // resolves against the available main size.
    flex->RemoveView(grown.Get());
    flex->RemoveView(bare.Get());
    auto fixedBasis = TV(50, 30);
    LayoutStyle fb;
    fb.FlexBasis = Unit::Dp(120);
    flex->AddView(fixedBasis.Get(), fb);
    auto half = TV(50, 30);
    half->AddClass(u8"half");
    flex->AddView(half.Get());
    LayoutPass(ctx, root.Get());
    CHECK(fixedBasis->Bounds.width == doctest::Approx(120));
    CHECK(half->Bounds.width == doctest::Approx(150));
    CHECK(half->Layout().FlexBasis->percent == doctest::Approx(50));
}

// === markup ===

TEST_CASE("flex-wrap: markup sets wrap, align-content, the gaps and flex-basis")
{
    EnsureGlobals();
    auto view = MarkupLoader::LoadFromString(
        DefaultAllocator(),
        u8"<Flex wrap=\"wrap\" align-content=\"center\" gap=\"8 4\"><Panel flex-basis=\"40\"/></Flex>");
    REQUIRE(view);
    FlexLayout* flex = Cast<FlexLayout>(view.Get());
    REQUIRE(flex != nullptr);
    CHECK(flex->Wrap);
    CHECK(flex->AlignContent == AlignContent::Center);
    REQUIRE(flex->RowGap.HasValue());
    REQUIRE(flex->ColumnGap.HasValue());
    CHECK(flex->RowGap.Value() == doctest::Approx(8));
    CHECK(flex->ColumnGap.Value() == doctest::Approx(4));
    CHECK(flex->MainGap() == doctest::Approx(4)); // a row: column-gap on the main axis
    CHECK(flex->CrossGap() == doctest::Approx(8));
    REQUIRE(flex->ChildCount() == 1);
    CHECK(flex->GetChildAt(0)->Layout().FlexBasis->dp == doctest::Approx(40));

    auto single = MarkupLoader::LoadFromString(DefaultAllocator(), u8"<Flex gap=\"6\" row-gap=\"2\"/>");
    FlexLayout* f2 = Cast<FlexLayout>(single.Get());
    REQUIRE(f2 != nullptr);
    CHECK(f2->RowGap.Value() == doctest::Approx(2));
    CHECK(f2->ColumnGap.Value() == doctest::Approx(6));

    LayoutStyle ls;
    CHECK(MarkupRegistry::ApplyLayoutAttribute(ls, u8"flex-basis", u8"2em"));
    CHECK(ls.FlexBasis->em == doctest::Approx(2));
}

// === ellipsis ===

TEST_CASE("flex-wrap: text-overflow from the sheet turns Label ellipsis on; the property wins")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    EnsureGlobals();
    Init(ctx, root.Get(), 800, 600);
    ctx.SetStyleSheet(LoadSSS(u8"Label { text-overflow: ellipsis; }\n"
                              u8"Label.clip { text-overflow: clip; }\n"));
    auto styled = core::MakeRef<Label>(core::DefaultAllocator(), StringView(u8"A long line of text"));
    auto clipped = core::MakeRef<Label>(core::DefaultAllocator(), StringView(u8"A long line of text"));
    clipped->AddClass(u8"clip");
    root->AddView(styled.Get());
    root->AddView(clipped.Get());
    CHECK(styled->EffectiveEllipsis());
    CHECK_FALSE(clipped->EffectiveEllipsis());
    clipped->Ellipsis.SetValue(true);
    CHECK(clipped->EffectiveEllipsis());
}
