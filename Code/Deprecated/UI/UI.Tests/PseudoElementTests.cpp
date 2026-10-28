// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Ported from Sedulous.UI.Tests/src/PseudoElementTests.bf (faithful, full file, 17 cases). Covers
// pseudo-element ("part") styling end-to-end: StyleSelector matching + specificity with a pseudo,
// StyleSheet part resolution over the UIContext/RootView/TestView tree (ForTypePseudo/ForTypePseudoState,
// class selectors, cascade, subtype matching, no-inheritance), and the .sss `Type::part` parser syntax.
// Beef idioms: `scope StyleSelector()` -> stack value; `sel.Matches(view, .Normal, "thumb")` ->
// Matches(view, ControlState::Normal, u8"thumb"); `SetupSheet(ctx)` -> SetupSheet helper (copied from
// StyleSheetTests.cpp); byte Color(r,g,b,a) -> Rgb() float helper; `===` -> pointer ==; `x as T` ->
// core::Cast<T>; `bg is T` -> core::Cast<T>(bg) != nullptr; `.[Friend]mRules[i]` -> sheet->GetRule(i);
// `rule.Selector.PseudoElement != null` -> .HasValue(); `State.Value.HasFlag(.Hover)` -> HasFlag(...).
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
    // Create a StyleSheet owned by ctx; returns a borrowed pointer for the test to add rules to.
    StyleSheet* SetupSheet(UIContext& ctx)
    {
        auto sheet = core::MakeRef<StyleSheet>(core::DefaultAllocator());
        StyleSheet* raw = sheet.Get();
        ctx.SetStyleSheet(Move(sheet));
        return raw;
    }

    // Byte Color(r,g,b,a) -> float Color (mirrors the Beef Color(r,g,b,255) ctor).
    [[nodiscard]] Color Rgb(f32 r, f32 g, f32 b, f32 a = 255.0f)
    {
        return Color{r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
    }

    // Register the element type names the .sss parser tests reference (StyleSheetLoader::InitializeGlobals
    // registers the drawable factories; the type registry maps "View" etc. to the RTTI).
    void EnsureGlobals()
    {
        StyleSheetLoader::InitializeGlobals();
        UITypeRegistry::Register(u8"View", &View::StaticType());
        UITypeRegistry::Register(u8"TestView", &TestView::StaticType());
        UITypeRegistry::Register(u8"TestGroup", &TestGroup::StaticType());
    }

    core::RefPtr<StyleSheet> LoadSSS(StringView src)
    {
        StyleSheetLoader loader(DefaultAllocator());
        return loader.Load(src);
    }
}

// === StyleSelector matching ===

TEST_CASE("pseudo-element: Selector_PseudoElement_Matches")
{
    StyleSelector sel;
    sel.ViewType = &TestView::StaticType();
    sel.SetPseudoElement(u8"thumb");

    TestView view;
    CHECK(sel.Matches(view, ControlState::Normal, u8"thumb"));
    CHECK_FALSE(sel.Matches(view, ControlState::Normal, u8"track"));
    CHECK_FALSE(sel.Matches(view, ControlState::Normal)); // no pseudo = no match
}

TEST_CASE("pseudo-element: Selector_NoPseudo_RejectsQuery")
{
    StyleSelector sel;
    sel.ViewType = &TestView::StaticType();

    TestView view;
    CHECK(sel.Matches(view, ControlState::Normal));                  // element-level match
    CHECK_FALSE(sel.Matches(view, ControlState::Normal, u8"thumb")); // pseudo query, no match
}

TEST_CASE("pseudo-element: Selector_PseudoWithState")
{
    StyleSelector sel;
    sel.ViewType = &TestView::StaticType();
    sel.SetPseudoElement(u8"thumb");
    sel.State = ControlState::Hover;

    TestView view;
    CHECK(sel.Matches(view, ControlState::Hover, u8"thumb"));
    CHECK_FALSE(sel.Matches(view, ControlState::Normal, u8"thumb")); // wrong state
    CHECK_FALSE(sel.Matches(view, ControlState::Hover, u8"track"));  // wrong pseudo
}

TEST_CASE("pseudo-element: Specificity_WithPseudoElement")
{
    StyleSelector sel;
    sel.ViewType = &TestView::StaticType();
    sel.SetPseudoElement(u8"thumb");
    CHECK(sel.Specificity() == 2); // type=1 + pseudo=1
}

TEST_CASE("pseudo-element: Specificity_Full")
{
    StyleSelector sel;
    sel.ViewType = &TestView::StaticType();
    sel.AddClass(u8"primary");
    sel.State = ControlState::Hover;
    sel.SetPseudoElement(u8"thumb");
    CHECK(sel.Specificity() == 22); // CSS: type=1 + class=10 + pseudo-class(:hover)=10 + pseudo-element=1
}

// === StyleSheet pseudo-element resolution ===

TEST_CASE("pseudo-element: ResolvePart_Basic")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    StyleSheet* sheet = SetupSheet(ctx);
    sheet->ForTypePseudo(&TestView::StaticType(), u8"thumb")
        .Set(StyleProperty::Width, 12.0f)
        .Set(StyleProperty::Height, 12.0f);
    sheet->ForTypePseudo(&TestView::StaticType(), u8"track").Set(StyleProperty::Height, 4.0f);

    auto view = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(view.Get());

    const f32 thumbW =
        view->ResolvePartFloat(u8"thumb", StyleProperty::Width, ControlState::Normal);
    CHECK(thumbW == doctest::Approx(12.0f));

    const f32 trackH =
        view->ResolvePartFloat(u8"track", StyleProperty::Height, ControlState::Normal);
    CHECK(trackH == doctest::Approx(4.0f));

    // Element-level Width should not be set
    const f32 viewW = view->ResolveStyleFloat(StyleProperty::Width);
    CHECK(viewW == 0);
}

TEST_CASE("pseudo-element: ResolvePart_WithState")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    StyleSheet* sheet = SetupSheet(ctx);
    RefPtr<ColorDrawable> normalBg =
        core::MakeRef<ColorDrawable>(core::DefaultAllocator(), Rgb(100, 100, 100));
    RefPtr<ColorDrawable> hoverBg =
        core::MakeRef<ColorDrawable>(core::DefaultAllocator(), Rgb(200, 200, 200));
    sheet->OwnDrawable(normalBg);
    sheet->OwnDrawable(hoverBg);

    sheet->ForTypePseudo(&TestView::StaticType(), u8"thumb")
        .Set(StyleProperty::Background, normalBg);
    sheet->ForTypePseudoState(&TestView::StaticType(), u8"thumb", ControlState::Hover)
        .Set(StyleProperty::Background, hoverBg);

    auto view = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(view.Get());

    // Normal state
    Drawable* bg1 =
        view->ResolvePartDrawable(u8"thumb", StyleProperty::Background, ControlState::Normal);
    CHECK(bg1 == normalBg.Get());

    // Hover state - higher specificity
    Drawable* bg2 =
        view->ResolvePartDrawable(u8"thumb", StyleProperty::Background, ControlState::Hover);
    CHECK(bg2 == hoverBg.Get());
}

TEST_CASE("pseudo-element: ResolvePart_DoesNotInherit")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    StyleSheet* sheet = SetupSheet(ctx);
    sheet->ForTypePseudo(&ViewGroup::StaticType(), u8"thumb")
        .Set(StyleProperty::Background, sheet->OwnColor(Rgb(100, 100, 100)));

    auto group = core::MakeRef<TestGroup>(core::DefaultAllocator());
    root->AddView(group.Get());
    auto child = core::MakeRef<TestView>(core::DefaultAllocator());
    group->AddView(child.Get());

    // Pseudo-element rules should NOT inherit through the parent chain
    Drawable* bg =
        child->ResolvePartDrawable(u8"thumb", StyleProperty::Background, ControlState::Normal);
    CHECK(bg == nullptr);
}

TEST_CASE("pseudo-element: ResolvePart_SubtypeMatching")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    StyleSheet* sheet = SetupSheet(ctx);
    // Rule on View::thumb matches any subtype
    sheet->ForTypePseudo(&View::StaticType(), u8"thumb").Set(StyleProperty::Width, 16.0f);

    auto view = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(view.Get()); // TestView : View

    const f32 w = view->ResolvePartFloat(u8"thumb", StyleProperty::Width, ControlState::Normal);
    CHECK(w == doctest::Approx(16.0f));
}

TEST_CASE("pseudo-element: ResolvePart_ClassSelector")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    StyleSheet* sheet = SetupSheet(ctx);
    RefPtr<StyleRule> rule = core::MakeRef<StyleRule>(core::DefaultAllocator());
    rule->Selector.AddClass(u8"custom");
    rule->Selector.SetPseudoElement(u8"track");
    rule->Set(StyleProperty::Height, 8.0f);
    sheet->AddRule(Move(rule));

    auto view = core::MakeRef<TestView>(core::DefaultAllocator());
    view->AddClass(u8"custom");
    root->AddView(view.Get());

    const f32 h = view->ResolvePartFloat(u8"track", StyleProperty::Height, ControlState::Normal);
    CHECK(h == doctest::Approx(8.0f));

    // Without the class, no match
    auto view2 = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(view2.Get());
    const f32 h2 = view2->ResolvePartFloat(u8"track", StyleProperty::Height, ControlState::Normal);
    CHECK(h2 == 0);
}

TEST_CASE("pseudo-element: ResolvePart_Cascade")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    StyleSheet* sheet = SetupSheet(ctx);
    // Type-only pseudo: specificity 2 (type=1 + pseudo=1)
    sheet->ForTypePseudo(&TestView::StaticType(), u8"thumb").Set(StyleProperty::Width, 12.0f);
    // Type+state pseudo: specificity 3 (type=1 + state=1 + pseudo=1)
    sheet->ForTypePseudoState(&TestView::StaticType(), u8"thumb", ControlState::Hover)
        .Set(StyleProperty::Width, 16.0f);

    auto view = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(view.Get());

    // Normal: type-only wins
    CHECK(view->ResolvePartFloat(u8"thumb", StyleProperty::Width, ControlState::Normal) == 12.0f);
    // Hover: type+state wins (higher specificity)
    CHECK(view->ResolvePartFloat(u8"thumb", StyleProperty::Width, ControlState::Hover) == 16.0f);
}

// === .sss parser pseudo-element syntax ===

TEST_CASE("pseudo-element: SSS_PseudoElement_Parses")
{
    EnsureGlobals();
    core::RefPtr<StyleSheet> sheet = LoadSSS(u8R"(
        View::thumb {
            width: 12;
            height: 12;
        }
        View::track {
            height: 4;
        }
    )");

    CHECK(sheet->RuleCount() == 2);

    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    ctx.SetStyleSheet(sheet);

    auto view = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(view.Get());

    const f32 thumbW =
        view->ResolvePartFloat(u8"thumb", StyleProperty::Width, ControlState::Normal);
    CHECK(thumbW == doctest::Approx(12.0f));

    const f32 trackH =
        view->ResolvePartFloat(u8"track", StyleProperty::Height, ControlState::Normal);
    CHECK(trackH == doctest::Approx(4.0f));
}

TEST_CASE("pseudo-element: SSS_PseudoElement_WithState")
{
    EnsureGlobals();
    core::RefPtr<StyleSheet> sheet = LoadSSS(u8R"(
        View::thumb {
            background: color(#666666);
        }
        View::thumb:hover {
            background: color(#999999);
        }
    )");

    CHECK(sheet->RuleCount() == 2);

    // Verify the second rule has both pseudo-element and state
    const StyleRule& rule = sheet->GetRule(1);
    CHECK(rule.Selector.PseudoElement.HasValue());
    CHECK(rule.Selector.PseudoElement.Value().AsView() == StringView{u8"thumb"});
    REQUIRE(rule.Selector.State.HasValue());
    CHECK(HasFlag(rule.Selector.State.Value(), ControlState::Hover));
}

TEST_CASE("pseudo-element: SSS_PseudoElement_StateBeforePseudo")
{
    EnsureGlobals();
    core::RefPtr<StyleSheet> sheet = LoadSSS(u8R"(
        View:disabled::thumb {
            background: color(#333333);
        }
    )");

    CHECK(sheet->RuleCount() == 1);

    const StyleRule& rule = sheet->GetRule(0);
    CHECK(rule.Selector.PseudoElement.HasValue());
    CHECK(rule.Selector.PseudoElement.Value().AsView() == StringView{u8"thumb"});
    REQUIRE(rule.Selector.State.HasValue());
    CHECK(HasFlag(rule.Selector.State.Value(), ControlState::Disabled));
}

TEST_CASE("pseudo-element: SSS_PseudoElement_WithDrawable")
{
    EnsureGlobals();
    core::RefPtr<StyleSheet> sheet = LoadSSS(u8R"(
        View::thumb {
            background: rounded-rect(#aabbcc, radius=6);
        }
    )");

    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    ctx.SetStyleSheet(sheet);

    auto view = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(view.Get());

    Drawable* bg =
        view->ResolvePartDrawable(u8"thumb", StyleProperty::Background, ControlState::Normal);
    REQUIRE(bg != nullptr);
    CHECK(core::Cast<RoundedRectDrawable>(bg) != nullptr);
}

TEST_CASE("pseudo-element: SSS_PseudoElement_WithPaletteVariable")
{
    EnsureGlobals();
    StyleSheetLoader loader(DefaultAllocator());
    loader.SetPaletteVariable(u8"accent", Rgb(61, 174, 233, 255));

    core::RefPtr<StyleSheet> sheet = loader.Load(u8R"(
        View::fill {
            background: color($accent);
        }
    )");

    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    ctx.SetStyleSheet(sheet);

    auto view = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(view.Get());

    Drawable* bg =
        view->ResolvePartDrawable(u8"fill", StyleProperty::Background, ControlState::Normal);
    REQUIRE(bg != nullptr);
    ColorDrawable* cd = core::Cast<ColorDrawable>(bg);
    REQUIRE(cd != nullptr);
    CHECK(cd->Color.r == doctest::Approx(61 / 255.0f));
}

// === Element-level rules don't match pseudo-element queries ===

TEST_CASE("pseudo-element: ElementRule_DoesNotMatchPseudoQuery")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    StyleSheet* sheet = SetupSheet(ctx);
    sheet->ForType(&TestView::StaticType())
        .Set(StyleProperty::Background, sheet->OwnColor(Rgb(255, 0, 0)));

    auto view = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(view.Get());

    // Element-level Background is set
    CHECK(view->ResolveStyleDrawable(StyleProperty::Background) != nullptr);

    // But pseudo-element query should NOT match element-level rule
    Drawable* partBg =
        view->ResolvePartDrawable(u8"thumb", StyleProperty::Background, ControlState::Normal);
    CHECK(partBg == nullptr);
}
