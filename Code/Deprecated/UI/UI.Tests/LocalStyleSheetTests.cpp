// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Ported from Sedulous.UI.Tests/src/LocalStyleSheetTests.bf (faithful, full file, 23 cases). Sub-phase E
// lifecycle of View.LocalStyleSheet + sub-phase F/G resolution wiring (ancestor walk + context fallback +
// pseudo-elements + inheritable-via-ForAll). Beef refcount lifecycle (`new/scope/defer ReleaseRef` + `delete
// view`) -> RAII RefPtr/MakeRef with .Reset() standing in for `delete`; `view.LocalStyleSheet = s; s.ReleaseRef()`
// -> a SetupLocalSheet(view) helper mirroring SetupSheet(ctx); `=== sheet` -> pointer ==; `case .None` ->
// StyleValue::Kind::None; `SetInlinePartStyle(part, prop, .FloatVal(v))` -> View::SetPartStyle(part, prop, v).
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
    [[nodiscard]] Color Rgb(f32 r, f32 g, f32 b, f32 a = 255.0f)
    {
        return Color{r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
    }

    // Install a fresh empty StyleSheet on ctx; returns a borrowed pointer for the test to add rules to.
    StyleSheet* SetupCtxSheet(UIContext& ctx)
    {
        auto s = core::MakeRef<StyleSheet>(core::DefaultAllocator());
        StyleSheet* raw = s.Get();
        ctx.SetStyleSheet(Move(s));
        return raw;
    }

    // Attach a fresh empty StyleSheet to view as its local sheet; returns a borrowed pointer to add rules to.
    StyleSheet* SetupLocalSheet(View& view)
    {
        auto s = core::MakeRef<StyleSheet>(core::DefaultAllocator());
        StyleSheet* raw = s.Get();
        view.SetLocalStyleSheet(Move(s));
        return raw;
    }
}

// === Lifecycle (sub-phase E) ===

TEST_CASE("local-stylesheet: Default_IsNull")
{
    TestView view;
    CHECK(view.GetLocalStyleSheet() == nullptr);
}

TEST_CASE("local-stylesheet: Set_AddRefsAndRetains")
{
    auto sheet = core::MakeRef<StyleSheet>(core::DefaultAllocator());
    auto view = core::MakeRef<TestView>(core::DefaultAllocator());
    view->SetLocalStyleSheet(sheet);
    CHECK(view->GetLocalStyleSheet() == sheet.Get());
    view.Reset(); // destroy view; sheet stays alive on the local ref, freed at scope exit
}

TEST_CASE("local-stylesheet: Set_Twice_NoOpForIdentical")
{
    auto sheet = core::MakeRef<StyleSheet>(core::DefaultAllocator());
    TestView view;
    view.SetLocalStyleSheet(sheet);
    view.SetLocalStyleSheet(sheet); // second assignment to the same sheet is a guarded no-op
    CHECK(view.GetLocalStyleSheet() == sheet.Get());
}

TEST_CASE("local-stylesheet: Reassign_ReleasesPrevious")
{
    auto s1 = core::MakeRef<StyleSheet>(core::DefaultAllocator());
    auto s2 = core::MakeRef<StyleSheet>(core::DefaultAllocator());
    auto view = core::MakeRef<TestView>(core::DefaultAllocator());
    view->SetLocalStyleSheet(s1);
    view->SetLocalStyleSheet(s2); // releases view's ref on s1
    CHECK(view->GetLocalStyleSheet() == s2.Get());
    s1.Reset(); // s1 now unreferenced -> freed
    view.Reset();
}

TEST_CASE("local-stylesheet: Clear_ReleasesAndFallsBackToNull")
{
    auto sheet = core::MakeRef<StyleSheet>(core::DefaultAllocator());
    TestView view;
    view.SetLocalStyleSheet(sheet);
    view.SetLocalStyleSheet(nullptr); // view releases its ref
    CHECK(view.GetLocalStyleSheet() == nullptr);
}

TEST_CASE("local-stylesheet: Destruction_ReleasesSheet")
{
    auto sheet = core::MakeRef<StyleSheet>(core::DefaultAllocator());
    auto view = core::MakeRef<TestView>(core::DefaultAllocator());
    view->SetLocalStyleSheet(sheet);
    view.Reset(); // view destructor releases its ref; sheet still held by the local RefPtr
    CHECK(sheet.Get() != nullptr);
}

TEST_CASE("local-stylesheet: SharedBetweenViews")
{
    auto sheet = core::MakeRef<StyleSheet>(core::DefaultAllocator());
    auto v1 = core::MakeRef<TestView>(core::DefaultAllocator());
    auto v2 = core::MakeRef<TestView>(core::DefaultAllocator());
    v1->SetLocalStyleSheet(sheet);
    v2->SetLocalStyleSheet(sheet);
    CHECK(v1->GetLocalStyleSheet() == sheet.Get());
    CHECK(v2->GetLocalStyleSheet() == sheet.Get());
    v1.Reset();
    v2.Reset();
    CHECK(sheet.Get() != nullptr); // creation ref still holds it
}

// === Resolution-order (sub-phase F) ===

TEST_CASE("local-stylesheet: Resolution_LocalOnThisView_WinsOverContext")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    StyleSheet* ctxSheet = SetupCtxSheet(ctx);
    ctxSheet->ForType(&TestView::StaticType()).Set(StyleProperty::TextColor, Rgb(255, 0, 0));

    auto view = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(view.Get());
    StyleSheet* local = SetupLocalSheet(*view);
    local->ForType(&TestView::StaticType()).Set(StyleProperty::TextColor, Rgb(0, 255, 0));

    const Color c = view->ResolveStyleColor(StyleProperty::TextColor);
    CHECK((c.g == 1.0f && c.r == 0.0f));
}

TEST_CASE("local-stylesheet: Resolution_LocalOnAncestor_WinsOverContext")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    StyleSheet* ctxSheet = SetupCtxSheet(ctx);
    ctxSheet->ForType(&TestView::StaticType()).Set(StyleProperty::TextColor, Color::Red);

    auto group = core::MakeRef<TestGroup>(core::DefaultAllocator());
    auto child = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(group.Get());
    group->AddView(child.Get());

    StyleSheet* parentLocal = SetupLocalSheet(*group);
    parentLocal->ForType(&TestView::StaticType()).Set(StyleProperty::TextColor, Rgb(50, 200, 50));

    CHECK(child->ResolveStyleColor(StyleProperty::TextColor).g == doctest::Approx(200 / 255.0f));
}

TEST_CASE("local-stylesheet: Resolution_CloserAncestor_WinsOverFarther")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    SetupCtxSheet(ctx);

    auto outer = core::MakeRef<TestGroup>(core::DefaultAllocator());
    auto inner = core::MakeRef<TestGroup>(core::DefaultAllocator());
    auto child = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(outer.Get());
    outer->AddView(inner.Get());
    inner->AddView(child.Get());

    StyleSheet* outerLocal = SetupLocalSheet(*outer);
    outerLocal->ForType(&TestView::StaticType()).Set(StyleProperty::TextColor, Color::Red);
    StyleSheet* innerLocal = SetupLocalSheet(*inner);
    innerLocal->ForType(&TestView::StaticType()).Set(StyleProperty::TextColor, Rgb(0, 0, 255));

    CHECK(child->ResolveStyleColor(StyleProperty::TextColor).b == 1.0f);
}

TEST_CASE("local-stylesheet: Resolution_NotFound_FallsThroughToNextAncestor")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    SetupCtxSheet(ctx);

    auto outer = core::MakeRef<TestGroup>(core::DefaultAllocator());
    auto inner = core::MakeRef<TestGroup>(core::DefaultAllocator());
    auto child = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(outer.Get());
    outer->AddView(inner.Get());
    inner->AddView(child.Get());

    StyleSheet* outerLocal = SetupLocalSheet(*outer);
    outerLocal->ForType(&TestView::StaticType()).Set(StyleProperty::TextColor, Rgb(0, 200, 0));
    StyleSheet* innerLocal = SetupLocalSheet(*inner);
    innerLocal->ForType(&TestView::StaticType()).Set(StyleProperty::FontSize, 24.0f);

    CHECK(child->ResolveStyleColor(StyleProperty::TextColor).g == doctest::Approx(200 / 255.0f));
    CHECK(child->ResolveStyleFloat(StyleProperty::FontSize) == 24.0f);
}

TEST_CASE("local-stylesheet: Resolution_NotFound_FallsThroughToContext")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    StyleSheet* ctxSheet = SetupCtxSheet(ctx);
    ctxSheet->ForType(&TestView::StaticType()).Set(StyleProperty::TextColor, Rgb(100, 100, 100));

    auto group = core::MakeRef<TestGroup>(core::DefaultAllocator());
    auto child = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(group.Get());
    group->AddView(child.Get());

    StyleSheet* groupLocal = SetupLocalSheet(*group);
    groupLocal->ForType(&TestView::StaticType()).Set(StyleProperty::FontSize, 18.0f);

    CHECK(child->ResolveStyleColor(StyleProperty::TextColor).r == doctest::Approx(100 / 255.0f));
}

TEST_CASE("local-stylesheet: Resolution_InheritableProperty_CascadesThroughLocalOnAncestor")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    SetupCtxSheet(ctx);

    auto dialog = core::MakeRef<TestGroup>(core::DefaultAllocator());
    auto inner = core::MakeRef<TestGroup>(core::DefaultAllocator());
    auto child = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(dialog.Get());
    dialog->AddView(inner.Get());
    inner->AddView(child.Get());

    StyleSheet* dialogLocal = SetupLocalSheet(*dialog);
    dialogLocal->ForType(&TestGroup::StaticType()).Set(StyleProperty::TextColor, Rgb(50, 150, 250));

    CHECK(child->ResolveStyleColor(StyleProperty::TextColor).b == doctest::Approx(250 / 255.0f));
}

TEST_CASE("local-stylesheet: Resolution_NonInheritable_DoesNotCascade")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    SetupCtxSheet(ctx);

    auto group = core::MakeRef<TestGroup>(core::DefaultAllocator());
    auto child = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(group.Get());
    group->AddView(child.Get());

    StyleSheet* groupLocal = SetupLocalSheet(*group);
    groupLocal->ForType(&TestGroup::StaticType()).Set(StyleProperty::Padding, Thickness(8));

    CHECK(child->ResolveStyleThickness(StyleProperty::Padding).IsZero());
}

// === Pseudo-element resolution (sub-phase G) ===

TEST_CASE("local-stylesheet: Pseudo_LocalOnThisView_WinsOverContext")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    StyleSheet* ctxSheet = SetupCtxSheet(ctx);
    ctxSheet->ForTypePseudo(&TestView::StaticType(), u8"thumb")
        .Set(StyleProperty::CornerRadius, 4.0f);

    auto view = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(view.Get());
    StyleSheet* local = SetupLocalSheet(*view);
    local->ForTypePseudo(&TestView::StaticType(), u8"thumb")
        .Set(StyleProperty::CornerRadius, 12.0f);

    CHECK(view->ResolvePartFloat(u8"thumb", StyleProperty::CornerRadius, ControlState::Normal) ==
          12.0f);
}

TEST_CASE("local-stylesheet: Pseudo_LocalOnAncestor_WinsOverContext")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    StyleSheet* ctxSheet = SetupCtxSheet(ctx);
    ctxSheet->ForTypePseudo(&TestView::StaticType(), u8"thumb")
        .Set(StyleProperty::CornerRadius, 2.0f);

    auto group = core::MakeRef<TestGroup>(core::DefaultAllocator());
    auto child = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(group.Get());
    group->AddView(child.Get());

    StyleSheet* parentLocal = SetupLocalSheet(*group);
    parentLocal->ForTypePseudo(&TestView::StaticType(), u8"thumb")
        .Set(StyleProperty::CornerRadius, 16.0f);

    CHECK(child->ResolvePartFloat(u8"thumb", StyleProperty::CornerRadius, ControlState::Normal) ==
          16.0f);
}

TEST_CASE("local-stylesheet: Pseudo_CloserAncestor_WinsOverFarther")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    SetupCtxSheet(ctx);

    auto outer = core::MakeRef<TestGroup>(core::DefaultAllocator());
    auto inner = core::MakeRef<TestGroup>(core::DefaultAllocator());
    auto child = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(outer.Get());
    outer->AddView(inner.Get());
    inner->AddView(child.Get());

    StyleSheet* outerLocal = SetupLocalSheet(*outer);
    outerLocal->ForTypePseudo(&TestView::StaticType(), u8"thumb")
        .Set(StyleProperty::CornerRadius, 4.0f);
    StyleSheet* innerLocal = SetupLocalSheet(*inner);
    innerLocal->ForTypePseudo(&TestView::StaticType(), u8"thumb")
        .Set(StyleProperty::CornerRadius, 10.0f);

    CHECK(child->ResolvePartFloat(u8"thumb", StyleProperty::CornerRadius, ControlState::Normal) ==
          10.0f);
}

TEST_CASE("local-stylesheet: Pseudo_InlineBeatsLocalOnThisView")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    SetupCtxSheet(ctx);

    auto view = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(view.Get());
    StyleSheet* local = SetupLocalSheet(*view);
    local->ForTypePseudo(&TestView::StaticType(), u8"thumb").Set(StyleProperty::CornerRadius, 4.0f);

    view->SetPartStyle(u8"thumb", StyleProperty::CornerRadius, 20.0f); // inline part override wins

    CHECK(view->ResolvePartFloat(u8"thumb", StyleProperty::CornerRadius, ControlState::Normal) ==
          20.0f);
}

TEST_CASE("local-stylesheet: Pseudo_InlineBeatsLocalOnAncestor")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    SetupCtxSheet(ctx);

    auto group = core::MakeRef<TestGroup>(core::DefaultAllocator());
    auto child = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(group.Get());
    group->AddView(child.Get());

    StyleSheet* parentLocal = SetupLocalSheet(*group);
    parentLocal->ForTypePseudo(&TestView::StaticType(), u8"thumb")
        .Set(StyleProperty::CornerRadius, 4.0f);

    child->SetPartStyle(u8"thumb", StyleProperty::CornerRadius, 22.0f);

    CHECK(child->ResolvePartFloat(u8"thumb", StyleProperty::CornerRadius, ControlState::Normal) ==
          22.0f);
}

TEST_CASE("local-stylesheet: Pseudo_NotFound_FallsThroughToContext")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    StyleSheet* ctxSheet = SetupCtxSheet(ctx);
    ctxSheet->ForTypePseudo(&TestView::StaticType(), u8"thumb")
        .Set(StyleProperty::CornerRadius, 7.0f);

    auto group = core::MakeRef<TestGroup>(core::DefaultAllocator());
    auto child = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(group.Get());
    group->AddView(child.Get());

    StyleSheet* parentLocal = SetupLocalSheet(*group);
    parentLocal->ForTypePseudo(&TestView::StaticType(), u8"track")
        .Set(StyleProperty::CornerRadius, 99.0f);

    CHECK(child->ResolvePartFloat(u8"thumb", StyleProperty::CornerRadius, ControlState::Normal) ==
          7.0f);
}

// === FontFamily inheritance via ForAll() ===

TEST_CASE("local-stylesheet: Pseudo_FontFamily_ForAllOnAncestorLocal_ReachesAllDescendantTypes")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    SetupCtxSheet(ctx);

    auto pauseRoot = core::MakeRef<TestGroup>(core::DefaultAllocator());
    auto inner = core::MakeRef<TestGroup>(core::DefaultAllocator());
    auto view = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(pauseRoot.Get());
    pauseRoot->AddView(inner.Get());
    inner->AddView(view.Get());

    StyleSheet* pauseLocal = SetupLocalSheet(*pauseRoot);
    pauseLocal->ForAll().Set(StyleProperty::FontFamily, StringView(u8"Chewy"));

    CHECK(pauseRoot->ResolveStyle(StyleProperty::FontFamily).AsString().Value() ==
          StringView(u8"Chewy"));
    CHECK(inner->ResolveStyle(StyleProperty::FontFamily).AsString().Value() ==
          StringView(u8"Chewy"));
    CHECK(view->ResolveStyle(StyleProperty::FontFamily).AsString().Value() ==
          StringView(u8"Chewy"));
}

TEST_CASE("local-stylesheet: FontFamily_TypeScopedRule_NoMatchInChain_ReturnsNone")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    SetupCtxSheet(ctx);

    auto outer = core::MakeRef<TestGroup>(core::DefaultAllocator());
    auto inner = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(outer.Get());
    outer->AddView(inner.Get());

    StyleSheet* local = SetupLocalSheet(*outer);
    local->ForType(&Label::StaticType())
        .Set(StyleProperty::FontFamily, StringView(u8"Chewy"));

    CHECK(inner->ResolveStyle(StyleProperty::FontFamily).GetKind() == StyleValue::Kind::None);
}

TEST_CASE("local-stylesheet: FontFamily_TypeScopedRule_AncestorMatchesType_InheritsDown")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    SetupCtxSheet(ctx);

    auto outer = core::MakeRef<TestGroup>(core::DefaultAllocator());
    auto inner = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(outer.Get());
    outer->AddView(inner.Get());

    StyleSheet* local = SetupLocalSheet(*outer);
    local->ForType(&TestGroup::StaticType())
        .Set(StyleProperty::FontFamily, StringView(u8"Chewy"));

    CHECK(inner->ResolveStyle(StyleProperty::FontFamily).AsString().Value() ==
          StringView(u8"Chewy"));
}
