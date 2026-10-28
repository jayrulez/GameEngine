// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Ported from Sedulous.UI.Tests/src/StyleSheetTests.bf (faithful, full file). Pure-data cases (StyleValue
// accessors, StyleRule fluent + string lifecycle, StyleSelector specificity, ForAll empty-selector) plus
// the integration cases (Resolve_*/Specificity cascade/Inheritance_*/RefCounted_*/TypeMatch/ForAll rules)
// that drive View.ResolveStyle over the UIContext/RootView/TestGroup tree - now that that cluster + the
// TestHelpers doubles exist. Beef `SetupSheet(ctx)` (`ctx.StyleSheet = new; ReleaseRef`) -> a RefPtr held
// by ctx via SetStyleSheet (helper returns a borrowed ptr for adding rules); `view.IsEnabled = false`
// sets the field; `=== drawable` -> pointer ==.
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
}

// === StyleValue accessors ===

TEST_CASE("stylesheet: StyleValue_ColorAccessor")
{
    StyleValue val = StyleValue::ColorVal(core::Color::Red);
    CHECK(val.AsColor().HasValue());
    CHECK_FALSE(val.AsFloat().HasValue());
    CHECK(val.AsDrawable() == nullptr);
}

TEST_CASE("stylesheet: StyleValue_FloatAccessor")
{
    StyleValue val = StyleValue::FloatVal(42.0f);
    CHECK(val.AsFloat().HasValue());
    CHECK_FALSE(val.AsColor().HasValue());
}

TEST_CASE("stylesheet: StyleValue_None")
{
    StyleValue val = StyleValue::None();
    CHECK_FALSE(val.AsColor().HasValue());
    CHECK_FALSE(val.AsFloat().HasValue());
    CHECK_FALSE(val.AsThickness().HasValue());
    CHECK(val.AsDrawable() == nullptr);
    CHECK_FALSE(val.AsBool().HasValue());
}

// === StyleRule ===

TEST_CASE("stylesheet: StyleRule_FluentSet")
{
    StyleRule rule;
    rule.Set(StyleProperty::TextColor, core::Color::Red)
        .Set(StyleProperty::FontSize, 16.0f)
        .Set(StyleProperty::Padding, Thickness{4.0f});

    CHECK(rule.PropertyCount() == 3u);
    CHECK(rule.GetValue(StyleProperty::TextColor).HasValue());
    CHECK(rule.GetValue(StyleProperty::FontSize).HasValue());
    CHECK(rule.GetValue(StyleProperty::Padding).HasValue());
    CHECK_FALSE(rule.GetValue(StyleProperty::Background).HasValue());
}

TEST_CASE("stylesheet: StringValue_RoundTrip")
{
    StyleRule rule;
    rule.Set(StyleProperty::FontFamily, StringView{u8"Roboto"});

    Optional<StyleValue> v = rule.GetValue(StyleProperty::FontFamily);
    REQUIRE(v.HasValue());
    CHECK(v.Value().AsString().HasValue());
    CHECK(v.Value().AsString().Value() == StringView{u8"Roboto"});
}

TEST_CASE("stylesheet: StringValue_OverwriteFreesPrevious")
{
    // RAII: overwriting an entry drops the previous StyleValue (its owned String). No leak.
    StyleRule rule;
    rule.Set(StyleProperty::FontFamily, StringView{u8"Roboto"});
    rule.Set(StyleProperty::FontFamily, StringView{u8"Chewy"});
    rule.Set(StyleProperty::FontFamily, StringView{u8"LilitaOne"});

    CHECK(rule.GetValue(StyleProperty::FontFamily).Value().AsString().Value() ==
          StringView{u8"LilitaOne"});
    CHECK(rule.PropertyCount() == 1u);
}

TEST_CASE("stylesheet: StringValue_RemoveFreesString")
{
    StyleRule rule;
    rule.Set(StyleProperty::FontFamily, StringView{u8"Roboto"});
    CHECK(rule.Remove(StyleProperty::FontFamily));
    CHECK_FALSE(rule.GetValue(StyleProperty::FontFamily).HasValue());
}

TEST_CASE("stylesheet: StringValue_DestructorFrees")
{
    // Construct/destroy many string-holding rules; RAII frees each (no leak).
    for (int i = 0; i < 16; ++i)
    {
        StyleRule rule;
        rule.Set(StyleProperty::FontFamily, StringView{u8"Roboto"});
        rule.Set(StyleProperty::FontFamily, StringView{u8"Chewy"});
        CHECK(rule.GetValue(StyleProperty::FontFamily).Value().AsString().Value() ==
              StringView{u8"Chewy"});
    }
}

// === StyleSelector ===

TEST_CASE("stylesheet: Selector_NullMatchesAnything")
{
    StyleSelector sel;
    TestView view;
    CHECK(sel.Matches(view, ControlState::Normal));
    CHECK(sel.Matches(view, ControlState::Hover));
    CHECK(sel.Specificity() == 0);
}

TEST_CASE("stylesheet: Selector_Specificity_Computed")
{
    StyleSelector selType;
    selType.ViewType = &TestView::StaticType();
    CHECK(selType.Specificity() == 1);

    StyleSelector selClass;
    selClass.AddClass(StringView{u8"primary"});
    CHECK(selClass.Specificity() == 10);

    StyleSelector selState;
    selState.State = ControlState::Hover;
    CHECK(selState.Specificity() == 10); // a pseudo-class weighs like a class (CSS)

    StyleSelector selAll;
    selAll.ViewType = &TestView::StaticType();
    selAll.AddClass(StringView{u8"primary"});
    selAll.State = ControlState::Hover;
    CHECK(selAll.Specificity() == 21);

    StyleSelector selId;
    selId.SetId(u8"ok");
    CHECK(selId.Specificity() == 100);

    StyleSelector selChain; // `.panel > TestView.primary:hover:checked`
    selChain.ViewType = &TestView::StaticType();
    selChain.AddClass(StringView{u8"primary"});
    selChain.State = ControlState::Hover | ControlState::Checked;
    SelectorCompound panel;
    panel.StyleClasses.PushBack(String(u8"panel"));
    selChain.AddAncestor(panel, true);
    CHECK(selChain.Specificity() == 1 + 10 + 20 + 10);
}

// === Palette ===

TEST_CASE("stylesheet: Palette_Lighten")
{
    Color c =
        Palette::Lighten(Color{100.0f / 255.0f, 100.0f / 255.0f, 100.0f / 255.0f, 1.0f}, 0.5f);
    CHECK(c.r > 100.0f / 255.0f);
    CHECK(c.r < 1.0f);
    CHECK(c.a == 1.0f);
}

TEST_CASE("stylesheet: Palette_Darken")
{
    Color c = Palette::Darken(Color{200.0f / 255.0f, 200.0f / 255.0f, 200.0f / 255.0f, 1.0f}, 0.5f);
    CHECK(c.r < 200.0f / 255.0f);
    CHECK(c.r > 0.0f);
    CHECK(c.a == 1.0f);
}

TEST_CASE("stylesheet: Palette_ComputeHover_Lighter")
{
    Color baseColor{60.0f / 255.0f, 60.0f / 255.0f, 60.0f / 255.0f, 1.0f};
    CHECK(Palette::ComputeHover(baseColor).r > baseColor.r);
}

TEST_CASE("stylesheet: Palette_ComputePressed_Darker")
{
    Color baseColor{60.0f / 255.0f, 60.0f / 255.0f, 60.0f / 255.0f, 1.0f};
    CHECK(Palette::ComputePressed(baseColor).r < baseColor.r);
}

TEST_CASE("stylesheet: Palette_ComputeDisabled_Faded")
{
    Color baseColor{60.0f / 255.0f, 120.0f / 255.0f, 200.0f / 255.0f, 1.0f};
    CHECK(Palette::ComputeDisabled(baseColor).a < 1.0f);
}

TEST_CASE("stylesheet: Palette_CreateStateColors_AllStatesSet")
{
    core::RefPtr<StateListDrawable> sl =
        Palette::CreateStateColors(DefaultAllocator(), Color{80.0f / 255.0f, 80.0f / 255.0f, 80.0f / 255.0f, 1.0f});
    CHECK(sl->Get(ControlState::Normal) != nullptr);
    CHECK(sl->Get(ControlState::Hover) != nullptr);
    CHECK(sl->Get(ControlState::Pressed) != nullptr);
    CHECK(sl->Get(ControlState::Disabled) != nullptr);
    CHECK(sl->Get(ControlState::Focused) != nullptr);
}

// === ForAll rule (empty selector) ===

TEST_CASE("stylesheet: ForAll_HasEmptySelector_SpecificityZero")
{
    core::RefPtr<StyleSheet> sheet = core::MakeRef<StyleSheet>(core::DefaultAllocator());
    StyleRule& rule = sheet->ForAll();
    CHECK(rule.Selector.IsEmpty());
    CHECK(rule.Selector.Specificity() == 0);
    CHECK(sheet->RuleCount() == 1u);
}

// ===========================================================================================
// Integration cases (View.ResolveStyle over the UIContext/RootView/TestGroup tree). Ported from
// Sedulous.UI.Tests/src/StyleSheetTests.bf now that the View cluster + TestHelpers doubles exist.
// ===========================================================================================

namespace
{
    [[nodiscard]] Color Rgb(f32 r, f32 g, f32 b, f32 a = 255.0f)
    {
        return Color{r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
    }
}

TEST_CASE("stylesheet: Resolve_NoSheet_ReturnsNone")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    auto view = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(view.Get());
    CHECK_FALSE(view->ResolveStyle(StyleProperty::Background).AsDrawable() != nullptr);
    CHECK(view->ResolveStyleDrawable(StyleProperty::Background) == nullptr);
    CHECK(view->ResolveStyleColor(StyleProperty::TextColor, Color::White) == Color::White);
    CHECK(view->ResolveStyleFloat(StyleProperty::FontSize, 14.0f) == 14.0f);
}

TEST_CASE("stylesheet: Resolve_TypeMatch")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    StyleSheet* sheet = SetupSheet(ctx);
    sheet->ForType(&TestView::StaticType()).Set(StyleProperty::TextColor, Rgb(255, 0, 0));
    auto view = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(view.Get());
    const Color color = view->ResolveStyleColor(StyleProperty::TextColor, Color::White);
    CHECK((color.r == 1.0f && color.g == 0.0f && color.b == 0.0f));
}

TEST_CASE("stylesheet: Resolve_TypeMismatch_ReturnsDefault")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    StyleSheet* sheet = SetupSheet(ctx);
    sheet->ForType(&TestGroup::StaticType()).Set(StyleProperty::TextColor, Rgb(255, 0, 0));
    auto view = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(view.Get());
    CHECK(view->ResolveStyleColor(StyleProperty::TextColor, Color::White) == Color::White);
}

TEST_CASE("stylesheet: Resolve_ClassMatch")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    StyleSheet* sheet = SetupSheet(ctx);
    sheet->ForClass(u8"primary").Set(StyleProperty::FontSize, 24.0f);
    auto view = core::MakeRef<TestView>(core::DefaultAllocator());
    view->AddClass(u8"primary");
    root->AddView(view.Get());
    CHECK(view->ResolveStyleFloat(StyleProperty::FontSize, 14.0f) == 24.0f);
}

TEST_CASE("stylesheet: Resolve_ClassMismatch_ReturnsDefault")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    StyleSheet* sheet = SetupSheet(ctx);
    sheet->ForClass(u8"primary").Set(StyleProperty::FontSize, 24.0f);
    auto view = core::MakeRef<TestView>(core::DefaultAllocator());
    view->AddClass(u8"secondary");
    root->AddView(view.Get());
    CHECK(view->ResolveStyleFloat(StyleProperty::FontSize, 14.0f) == 14.0f);
}

TEST_CASE("stylesheet: Resolve_ClassIsCaseSensitive")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    StyleSheet* sheet = SetupSheet(ctx);
    sheet->ForClass(u8"Primary").Set(StyleProperty::FontSize, 24.0f);
    auto view = core::MakeRef<TestView>(core::DefaultAllocator());
    view->AddClass(u8"primary");
    root->AddView(view.Get());
    CHECK(view->ResolveStyleFloat(StyleProperty::FontSize, 14.0f) == 14.0f);
}

TEST_CASE("stylesheet: Specificity_ClassBeatsType")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    StyleSheet* sheet = SetupSheet(ctx);
    sheet->ForType(&TestView::StaticType()).Set(StyleProperty::FontSize, 10.0f);
    sheet->ForClass(u8"big").Set(StyleProperty::FontSize, 30.0f);
    auto view = core::MakeRef<TestView>(core::DefaultAllocator());
    view->AddClass(u8"big");
    root->AddView(view.Get());
    CHECK(view->ResolveStyleFloat(StyleProperty::FontSize) == 30.0f);
}

TEST_CASE("stylesheet: Specificity_TypePlusStateBeatsType")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    StyleSheet* sheet = SetupSheet(ctx);
    sheet->ForType(&TestView::StaticType()).Set(StyleProperty::TextColor, Rgb(100, 100, 100));
    sheet->ForTypeState(&TestView::StaticType(), ControlState::Disabled)
        .Set(StyleProperty::TextColor, Rgb(50, 50, 50));
    auto view = core::MakeRef<TestView>(core::DefaultAllocator());
    view->IsEnabled = false;
    root->AddView(view.Get());
    CHECK(view->ResolveStyleColor(StyleProperty::TextColor).r == doctest::Approx(50 / 255.0f));
}

TEST_CASE("stylesheet: Specificity_ClassPlusStateBeatsClass")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    StyleSheet* sheet = SetupSheet(ctx);
    sheet->ForClass(u8"btn").Set(StyleProperty::FontSize, 14.0f);
    sheet->ForTypeClassState(&TestView::StaticType(), u8"btn", ControlState::Disabled)
        .Set(StyleProperty::FontSize, 12.0f);
    auto view = core::MakeRef<TestView>(core::DefaultAllocator());
    view->AddClass(u8"btn");
    view->IsEnabled = false;
    root->AddView(view.Get());
    CHECK(view->ResolveStyleFloat(StyleProperty::FontSize) == 12.0f);
}

TEST_CASE("stylesheet: Specificity_StateOnlyMatchesCurrentState")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    StyleSheet* sheet = SetupSheet(ctx);
    sheet->ForTypeState(&TestView::StaticType(), ControlState::Hover)
        .Set(StyleProperty::TextColor, Rgb(0, 255, 0));
    sheet->ForType(&TestView::StaticType()).Set(StyleProperty::TextColor, Rgb(200, 200, 200));
    auto view = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(view.Get());
    CHECK(view->ResolveStyleColor(StyleProperty::TextColor).r == doctest::Approx(200 / 255.0f));
}

TEST_CASE("stylesheet: Resolve_Drawable")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    StyleSheet* sheet = SetupSheet(ctx);
    RefPtr<Drawable> drawable =
        core::MakeRef<ColorDrawable>(core::DefaultAllocator(), Rgb(60, 60, 60));
    sheet->OwnDrawable(drawable);
    sheet->ForType(&TestView::StaticType()).Set(StyleProperty::Background, drawable);
    auto view = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(view.Get());
    CHECK(view->ResolveStyleDrawable(StyleProperty::Background) == drawable.Get());
}

TEST_CASE("stylesheet: Resolve_StateListDrawable")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    StyleSheet* sheet = SetupSheet(ctx);
    RefPtr<Drawable> sl = Palette::CreateStateColors(DefaultAllocator(), Rgb(60, 60, 60));
    sheet->OwnDrawable(sl);
    sheet->ForType(&TestView::StaticType()).Set(StyleProperty::Background, sl);
    auto view = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(view.Get());
    CHECK(view->ResolveStyleDrawable(StyleProperty::Background) == sl.Get());
}

TEST_CASE("stylesheet: Resolve_Thickness")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    StyleSheet* sheet = SetupSheet(ctx);
    sheet->ForType(&TestView::StaticType()).Set(StyleProperty::Padding, Thickness(8, 4));
    auto view = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(view.Get());
    const Thickness pad = view->ResolveStyleThickness(StyleProperty::Padding);
    CHECK((pad.Left == 8 && pad.Top == 4 && pad.Right == 8 && pad.Bottom == 4));
}

TEST_CASE("stylesheet: Resolve_Bool")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    StyleSheet* sheet = SetupSheet(ctx);
    sheet->ForType(&TestView::StaticType()).Set(StyleProperty::WordWrap, true);
    auto view = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(view.Get());
    Optional<bool> b = view->ResolveStyle(StyleProperty::WordWrap).AsBool();
    REQUIRE(b.HasValue());
    CHECK(b.Value() == true);
}

TEST_CASE("stylesheet: Inheritance_TextColorInheritsFromParent")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    StyleSheet* sheet = SetupSheet(ctx);
    sheet->ForType(&TestGroup::StaticType()).Set(StyleProperty::TextColor, Rgb(255, 100, 0));
    auto group = core::MakeRef<TestGroup>(core::DefaultAllocator());
    auto child = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(group.Get());
    group->AddView(child.Get());
    const Color color = child->ResolveStyleColor(StyleProperty::TextColor, Color::White);
    CHECK(color.r == 1.0f);
    CHECK(color.g == doctest::Approx(100 / 255.0f));
    CHECK(color.b == 0.0f);
}

TEST_CASE("stylesheet: Inheritance_FontSizeInheritsFromParent")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    StyleSheet* sheet = SetupSheet(ctx);
    sheet->ForType(&TestGroup::StaticType()).Set(StyleProperty::FontSize, 20.0f);
    auto group = core::MakeRef<TestGroup>(core::DefaultAllocator());
    auto child = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(group.Get());
    group->AddView(child.Get());
    CHECK(child->ResolveStyleFloat(StyleProperty::FontSize) == 20.0f);
}

TEST_CASE("stylesheet: Inheritance_ChildOverridesParent")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    StyleSheet* sheet = SetupSheet(ctx);
    sheet->ForType(&TestGroup::StaticType()).Set(StyleProperty::TextColor, Rgb(255, 0, 0));
    sheet->ForType(&TestView::StaticType()).Set(StyleProperty::TextColor, Rgb(0, 0, 255));
    auto group = core::MakeRef<TestGroup>(core::DefaultAllocator());
    auto child = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(group.Get());
    group->AddView(child.Get());
    const Color color = child->ResolveStyleColor(StyleProperty::TextColor);
    CHECK((color.b == 1.0f && color.r == 0.0f));
}

TEST_CASE("stylesheet: Inheritance_BackgroundDoesNotInherit")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    StyleSheet* sheet = SetupSheet(ctx);
    RefPtr<Drawable> drawable = core::MakeRef<ColorDrawable>(core::DefaultAllocator(), Color::Red);
    sheet->OwnDrawable(drawable);
    sheet->ForType(&TestGroup::StaticType()).Set(StyleProperty::Background, drawable);
    auto group = core::MakeRef<TestGroup>(core::DefaultAllocator());
    auto child = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(group.Get());
    group->AddView(child.Get());
    CHECK(child->ResolveStyleDrawable(StyleProperty::Background) == nullptr);
}

TEST_CASE("stylesheet: Inheritance_PaddingDoesNotInherit")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    StyleSheet* sheet = SetupSheet(ctx);
    sheet->ForType(&TestGroup::StaticType()).Set(StyleProperty::Padding, Thickness(20));
    auto group = core::MakeRef<TestGroup>(core::DefaultAllocator());
    auto child = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(group.Get());
    group->AddView(child.Get());
    CHECK(child->ResolveStyleThickness(StyleProperty::Padding).IsZero());
}

TEST_CASE("stylesheet: TypeMatch_IncludesSubtypes")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    StyleSheet* sheet = SetupSheet(ctx);
    sheet->ForType(&View::StaticType()).Set(StyleProperty::TextColor, Rgb(128, 128, 128));
    auto view = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(view.Get());
    CHECK(view->ResolveStyleColor(StyleProperty::TextColor).r == doctest::Approx(128 / 255.0f));
}

TEST_CASE("stylesheet: Rule_MultipleProperties")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    StyleSheet* sheet = SetupSheet(ctx);
    sheet->ForType(&TestView::StaticType())
        .Set(StyleProperty::TextColor, Rgb(200, 200, 200))
        .Set(StyleProperty::FontSize, 16.0f)
        .Set(StyleProperty::Padding, Thickness(8));
    auto view = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(view.Get());
    CHECK(view->ResolveStyleColor(StyleProperty::TextColor).r == doctest::Approx(200 / 255.0f));
    CHECK(view->ResolveStyleFloat(StyleProperty::FontSize) == 16.0f);
    CHECK(view->ResolveStyleThickness(StyleProperty::Padding).Left == 8);
}

TEST_CASE("stylesheet: RefCounted_SharedBetweenContexts")
{
    RefPtr<StyleSheet> sheet = core::MakeRef<StyleSheet>(core::DefaultAllocator());
    sheet->ForType(&TestView::StaticType()).Set(StyleProperty::FontSize, 18.0f);

    UIContext ctx1{DefaultAllocator()};
    auto root1 = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx1, root1.Get());
    ctx1.SetStyleSheet(sheet);
    UIContext ctx2{DefaultAllocator()};
    auto root2 = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx2, root2.Get());
    ctx2.SetStyleSheet(sheet);

    auto view1 = core::MakeRef<TestView>(core::DefaultAllocator());
    root1->AddView(view1.Get());
    auto view2 = core::MakeRef<TestView>(core::DefaultAllocator());
    root2->AddView(view2.Get());
    CHECK(view1->ResolveStyleFloat(StyleProperty::FontSize) == 18.0f);
    CHECK(view2->ResolveStyleFloat(StyleProperty::FontSize) == 18.0f);

    sheet.Reset(); // drop the creation ref; both contexts still hold their own
    CHECK(view1->ResolveStyleFloat(StyleProperty::FontSize) == 18.0f);
}

TEST_CASE("stylesheet: RefCounted_ReplacingSheetReleasesOld")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    RefPtr<StyleSheet> sheet1 = core::MakeRef<StyleSheet>(core::DefaultAllocator());
    sheet1->ForType(&TestView::StaticType()).Set(StyleProperty::FontSize, 10.0f);
    RefPtr<StyleSheet> sheet2 = core::MakeRef<StyleSheet>(core::DefaultAllocator());
    sheet2->ForType(&TestView::StaticType()).Set(StyleProperty::FontSize, 20.0f);

    ctx.SetStyleSheet(sheet1);
    ctx.SetStyleSheet(sheet2); // replaces sheet1 (its ctx ref released)

    auto view = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(view.Get());
    CHECK(view->ResolveStyleFloat(StyleProperty::FontSize) == 20.0f);
}

TEST_CASE("stylesheet: ForAll_RuleMatchesEveryView")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    StyleSheet* sheet = SetupSheet(ctx);
    sheet->ForAll().Set(StyleProperty::FontFamily, StringView(u8"Chewy"));
    auto testView = core::MakeRef<TestView>(core::DefaultAllocator());
    auto testGroup = core::MakeRef<TestGroup>(core::DefaultAllocator());
    root->AddView(testGroup.Get());
    testGroup->AddView(testView.Get());
    CHECK(testView->ResolveStyle(StyleProperty::FontFamily).AsString().Value() ==
          StringView(u8"Chewy"));
    CHECK(testGroup->ResolveStyle(StyleProperty::FontFamily).AsString().Value() ==
          StringView(u8"Chewy"));
}

TEST_CASE("stylesheet: ForAll_LosesSpecificityToTypedRule")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    StyleSheet* sheet = SetupSheet(ctx);
    sheet->ForAll().Set(StyleProperty::FontFamily, StringView(u8"ForAllFamily"));
    sheet->ForType(&TestView::StaticType())
        .Set(StyleProperty::FontFamily, StringView(u8"TestViewFamily"));
    auto view = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(view.Get());
    CHECK(view->ResolveStyle(StyleProperty::FontFamily).AsString().Value() ==
          StringView(u8"TestViewFamily"));
}
