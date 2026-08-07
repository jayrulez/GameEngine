// Ported from Sedulous.UI.Tests/src/InlineStyleTests.bf (faithful). Covers element-level + pseudo-element
// inline styles, resolution priority (inline beats type/class+state/pseudo/local rules + inheritance), and
// Drawable ownership via SetStyle. The ResolveStyleFontFamily / FontService resolution subset (5 cases) is
// already ported in FontFamilyTests.cpp and is NOT duplicated here.
//
// API mapping (Beef -> Draconic):
//   view.SetInlineStyle(.Prop, .XxxVal(v))        -> view->SetStyle(StyleProperty::Prop, v)  (typed overloads)
//   view.SetInlinePartStyle(part, .Prop, .XxxVal) -> view->SetPartStyle(part, StyleProperty::Prop, v)
//   view.GetInlineStyle(.Prop)                    -> view->GetInlineStyle(StyleProperty::Prop)
//   view.HasInlineStyle / HasAnyInlineStyles      -> view->HasInlineStyle / HasAnyInlineStyles()
//   view.ClearInlineStyle(.Prop)                  -> view->ClearInlineStyle(StyleProperty::Prop)
// The element getters exist directly; the PART getters/has/clear have no dedicated View method, so they are
// expressed here through the public inline sheet (view->InlineSheet()->FindInlinePartRule(part) + StyleRule).
// Clear-all uses View::ClearInlineStyles() (added to match Sedulous).
//   `.None` StyleValue -> GetKind()==StyleValue::Kind::None; `x === y` -> pointer ==; byte Color -> Rgb().
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"
import draconic.foundation;
import draconic.ui;
#include "TestHelpers.h"

using namespace draconic::ui;
using namespace draconic::ui::tests;
using namespace draconic::foundation;
namespace foundation = draconic::foundation;

namespace
{
    [[nodiscard]] Color Rgb(f32 r, f32 g, f32 b, f32 a = 255.0f)
    {
        return Color{r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
    }

    // Create a StyleSheet owned by ctx; returns a borrowed pointer for the test to add rules to (Sedulous SetupSheet).
    StyleSheet* SetupSheet(UIContext& ctx)
    {
        auto sheet = foundation::MakeRef<StyleSheet>(foundation::DefaultAllocator());
        StyleSheet* raw = sheet.Get();
        ctx.SetStyleSheet(Move(sheet));
        return raw;
    }

    // --- Pseudo-element inline accessors (Beef's Get/Has/ClearInlinePartStyle have no dedicated View
    //     method; express the same intent through the public inline sheet's part rule). ---
    [[nodiscard]] StyleValue GetInlinePartStyle(const View& view, StringView part,
                                                StyleProperty prop)
    {
        StyleSheet* sheet = view.InlineSheet();
        if (sheet == nullptr)
        {
            return StyleValue::None();
        }
        StyleRule* rule = sheet->FindInlinePartRule(part);
        if (rule == nullptr)
        {
            return StyleValue::None();
        }
        if (Optional<StyleValue> v = rule->GetValue(prop); v.HasValue())
        {
            return v.Value();
        }
        return StyleValue::None();
    }
    [[nodiscard]] bool HasInlinePartStyle(const View& view, StringView part, StyleProperty prop)
    {
        StyleSheet* sheet = view.InlineSheet();
        if (sheet == nullptr)
        {
            return false;
        }
        StyleRule* rule = sheet->FindInlinePartRule(part);
        return rule != nullptr && rule->GetValue(prop).HasValue();
    }
    bool ClearInlinePartStyle(const View& view, StringView part, StyleProperty prop)
    {
        StyleSheet* sheet = view.InlineSheet();
        if (sheet == nullptr)
        {
            return false;
        }
        StyleRule* rule = sheet->FindInlinePartRule(part);
        return rule != nullptr && rule->Remove(prop);
    }

    /// A Drawable that tracks its live count via a static counter, so tests can assert ownership
    /// transfer / cleanup. Draw is a no-op. (Sedulous TrackingDrawable.) No DRACONIC_OBJECT needed:
    /// it never queries its own type - Drawable's GetType() override already makes it concrete.
    class TrackingDrawable final : public Drawable
    {
    public:
        static int LiveCount;
        TrackingDrawable() { ++LiveCount; }
        ~TrackingDrawable() override { --LiveCount; }
        void Draw(UIDrawContext&, const Rectangle&) override {}
    };
    int TrackingDrawable::LiveCount = 0;
}

// === Element-level inline styles ===

TEST_CASE("inline-style: Inline_GetWithoutSet_ReturnsNone")
{
    auto view = foundation::MakeRef<TestView>(foundation::DefaultAllocator());
    CHECK(view->GetInlineStyle(StyleProperty::TextColor).GetKind() == StyleValue::Kind::None);
    CHECK_FALSE(view->HasInlineStyle(StyleProperty::TextColor));
    CHECK_FALSE(view->HasAnyInlineStyles());
}

TEST_CASE("inline-style: Inline_SetThenGet_RoundTrip")
{
    auto view = foundation::MakeRef<TestView>(foundation::DefaultAllocator());
    view->SetStyle(StyleProperty::TextColor, Rgb(255, 0, 0));

    CHECK(view->HasInlineStyle(StyleProperty::TextColor));
    CHECK(view->HasAnyInlineStyles());

    Optional<Color> c = view->GetInlineStyle(StyleProperty::TextColor).AsColor();
    REQUIRE(c.HasValue());
    CHECK(c.Value().r == 1.0f);
}

TEST_CASE("inline-style: Inline_SetOverwritesPrevious")
{
    auto view = foundation::MakeRef<TestView>(foundation::DefaultAllocator());
    view->SetStyle(StyleProperty::FontSize, 12.0f);
    view->SetStyle(StyleProperty::FontSize, 24.0f);

    CHECK(view->GetInlineStyle(StyleProperty::FontSize).AsFloat().Value() == 24.0f);
}

TEST_CASE("inline-style: Inline_ClearOne_LeavesOthers")
{
    auto view = foundation::MakeRef<TestView>(foundation::DefaultAllocator());
    view->SetStyle(StyleProperty::TextColor, Color::Red);
    view->SetStyle(StyleProperty::FontSize, 18.0f);

    view->ClearInlineStyle(StyleProperty::TextColor);

    CHECK_FALSE(view->HasInlineStyle(StyleProperty::TextColor));
    CHECK(view->HasInlineStyle(StyleProperty::FontSize));
    CHECK(view->HasAnyInlineStyles());
}

TEST_CASE("inline-style: Inline_ClearAll_RemovesEverything")
{
    auto view = foundation::MakeRef<TestView>(foundation::DefaultAllocator());
    view->SetStyle(StyleProperty::TextColor, Color::Red);
    view->SetStyle(StyleProperty::FontSize, 18.0f);
    view->SetPartStyle(StringView(u8"thumb"), StyleProperty::Background, Color::Blue);

    view->ClearInlineStyles();

    CHECK_FALSE(view->HasInlineStyle(StyleProperty::TextColor));
    CHECK_FALSE(view->HasInlineStyle(StyleProperty::FontSize));
    // The whole inline sheet was dropped, so the part override is gone too.
    CHECK(view->InlineSheet() == nullptr);
    CHECK_FALSE(view->HasAnyInlineStyles());
}

TEST_CASE("inline-style: Inline_ClearOne_NoOpWhenUnset")
{
    auto view = foundation::MakeRef<TestView>(foundation::DefaultAllocator());
    view->ClearInlineStyle(StyleProperty::TextColor);
    CHECK_FALSE(view->HasAnyInlineStyles());
}

TEST_CASE("inline-style: Inline_StorageIsLazy_NoSetMeansNoAlloc")
{
    // Without any Set, the public surface stays empty; reads and clears force no allocation.
    auto view = foundation::MakeRef<TestView>(foundation::DefaultAllocator());
    (void)view->GetInlineStyle(StyleProperty::TextColor);
    (void)view->HasInlineStyle(StyleProperty::TextColor);
    view->ClearInlineStyle(StyleProperty::TextColor);
    view->ClearInlineStyles();
    CHECK_FALSE(view->HasAnyInlineStyles());
    CHECK(view->InlineSheet() == nullptr);
}

TEST_CASE("inline-style: Inline_AcceptsEveryValueKind")
{
    auto view = foundation::MakeRef<TestView>(foundation::DefaultAllocator());
    // Keep our own ref on the drawable so we can compare identity after the view stores it.
    RefPtr<Drawable> drawable = foundation::MakeRef<ColorDrawable>(foundation::DefaultAllocator(), Color::Red);

    view->SetStyle(StyleProperty::TextColor, Rgb(10, 20, 30));
    view->SetStyle(StyleProperty::FontSize, 16.0f);
    view->SetStyle(StyleProperty::Padding, Thickness(2, 4));
    view->SetStyle(StyleProperty::WordWrap, true);
    view->SetStyle(StyleProperty::Background, drawable);

    CHECK(view->GetInlineStyle(StyleProperty::TextColor).AsColor().Value().r ==
          doctest::Approx(10 / 255.0f));
    CHECK(view->GetInlineStyle(StyleProperty::FontSize).AsFloat().Value() == 16.0f);
    CHECK(view->GetInlineStyle(StyleProperty::Padding).AsThickness().Value().Left == 2.0f);
    CHECK(view->GetInlineStyle(StyleProperty::WordWrap).AsBool().Value() == true);
    CHECK(view->GetInlineStyle(StyleProperty::Background).AsDrawable() == drawable.Get());
}

// === Pseudo-element inline styles ===

TEST_CASE("inline-style: InlinePart_GetWithoutSet_ReturnsNone")
{
    auto view = foundation::MakeRef<TestView>(foundation::DefaultAllocator());
    CHECK(GetInlinePartStyle(*view, u8"thumb", StyleProperty::Background).GetKind() ==
          StyleValue::Kind::None);
    CHECK_FALSE(HasInlinePartStyle(*view, u8"thumb", StyleProperty::Background));
    CHECK_FALSE(view->HasAnyInlineStyles());
}

TEST_CASE("inline-style: InlinePart_SetThenGet_RoundTrip")
{
    auto view = foundation::MakeRef<TestView>(foundation::DefaultAllocator());
    view->SetPartStyle(u8"thumb", StyleProperty::Background, Color::Red);

    CHECK(HasInlinePartStyle(*view, u8"thumb", StyleProperty::Background));
    CHECK(view->HasAnyInlineStyles());

    Optional<Color> c = GetInlinePartStyle(*view, u8"thumb", StyleProperty::Background).AsColor();
    REQUIRE(c.HasValue());
    CHECK(c.Value().r == 1.0f);
}

TEST_CASE("inline-style: InlinePart_SetOverwritesPrevious_NoDuplicates")
{
    auto view = foundation::MakeRef<TestView>(foundation::DefaultAllocator());
    view->SetPartStyle(u8"thumb", StyleProperty::Background, Color::Red);
    view->SetPartStyle(u8"thumb", StyleProperty::Background, Color::Blue);

    CHECK(GetInlinePartStyle(*view, u8"thumb", StyleProperty::Background).AsColor().Value().b ==
          1.0f);
    // A second set on the same key must not leave a stale entry - clearing removes it entirely.
    ClearInlinePartStyle(*view, u8"thumb", StyleProperty::Background);
    CHECK_FALSE(HasInlinePartStyle(*view, u8"thumb", StyleProperty::Background));
}

TEST_CASE("inline-style: InlinePart_DistinguishesByPartName")
{
    auto view = foundation::MakeRef<TestView>(foundation::DefaultAllocator());
    view->SetPartStyle(u8"thumb", StyleProperty::Background, Color::Red);
    view->SetPartStyle(u8"track", StyleProperty::Background, Color::Blue);

    CHECK(GetInlinePartStyle(*view, u8"thumb", StyleProperty::Background).AsColor().Value().r ==
          1.0f);
    CHECK(GetInlinePartStyle(*view, u8"track", StyleProperty::Background).AsColor().Value().b ==
          1.0f);
}

TEST_CASE("inline-style: InlinePart_DistinguishesByProperty")
{
    auto view = foundation::MakeRef<TestView>(foundation::DefaultAllocator());
    view->SetPartStyle(u8"thumb", StyleProperty::Background, Color::Red);
    view->SetPartStyle(u8"thumb", StyleProperty::CornerRadius, 8.0f);

    CHECK(GetInlinePartStyle(*view, u8"thumb", StyleProperty::Background).AsColor().HasValue());
    CHECK(GetInlinePartStyle(*view, u8"thumb", StyleProperty::CornerRadius).AsFloat().Value() ==
          8.0f);
}

TEST_CASE("inline-style: InlinePart_ClearOne_LeavesOthers")
{
    auto view = foundation::MakeRef<TestView>(foundation::DefaultAllocator());
    view->SetPartStyle(u8"thumb", StyleProperty::Background, Color::Red);
    view->SetPartStyle(u8"track", StyleProperty::Background, Color::Blue);

    ClearInlinePartStyle(*view, u8"thumb", StyleProperty::Background);

    CHECK_FALSE(HasInlinePartStyle(*view, u8"thumb", StyleProperty::Background));
    CHECK(HasInlinePartStyle(*view, u8"track", StyleProperty::Background));
}

TEST_CASE("inline-style: InlinePart_ClearOne_NoOpWhenUnset")
{
    auto view = foundation::MakeRef<TestView>(foundation::DefaultAllocator());
    ClearInlinePartStyle(*view, u8"thumb", StyleProperty::Background);
    CHECK_FALSE(view->HasAnyInlineStyles());
}

TEST_CASE("inline-style: Inline_Destruction_DoesNotLeakPartStrings")
{
    // Instantiating + dropping a view with inline part styles must free the part-name strings (RAII).
    // Wires that path through the runner so leaks would surface under ASAN.
    auto view = foundation::MakeRef<TestView>(foundation::DefaultAllocator());
    view->SetPartStyle(u8"thumb", StyleProperty::Background, Color::Red);
    view->SetPartStyle(u8"track", StyleProperty::Background, Color::Blue);
    view->SetPartStyle(u8"thumb", StyleProperty::CornerRadius, 4.0f);
    view.Reset();
    CHECK(view.Get() == nullptr);
}

// === Resolution priority ===

TEST_CASE("inline-style: Resolution_InlineBeatsTypeRule")
{
    UIContext ctx;
    auto root = foundation::MakeRef<RootView>(foundation::DefaultAllocator());
    Init(ctx, root.Get());
    StyleSheet* sheet = SetupSheet(ctx);
    sheet->ForType(&TestView::StaticType()).Set(StyleProperty::TextColor, Rgb(255, 0, 0));

    auto view = foundation::MakeRef<TestView>(foundation::DefaultAllocator());
    root->AddView(view.Get());
    view->SetStyle(StyleProperty::TextColor, Rgb(0, 255, 0));

    // Inline (green) wins over the rule (red).
    const Color c = view->ResolveStyleColor(StyleProperty::TextColor);
    CHECK((c.r == 0.0f && c.g == 1.0f));
}

TEST_CASE("inline-style: Resolution_InlineBeatsClassPlusStateRule")
{
    UIContext ctx;
    auto root = foundation::MakeRef<RootView>(foundation::DefaultAllocator());
    Init(ctx, root.Get());
    StyleSheet* sheet = SetupSheet(ctx);
    // Class + state rule (specificity 11) on a disabled view - would win without an inline override.
    sheet->ForTypeClassState(&TestView::StaticType(), u8"btn", ControlState::Disabled)
        .Set(StyleProperty::FontSize, 12.0f);

    auto view = foundation::MakeRef<TestView>(foundation::DefaultAllocator());
    view->AddClass(u8"btn");
    view->IsEnabled = false;
    root->AddView(view.Get());

    view->SetStyle(StyleProperty::FontSize, 99.0f);

    CHECK(view->ResolveStyleFloat(StyleProperty::FontSize) == 99.0f);
}

TEST_CASE("inline-style: Resolution_InlineIgnoresControlState")
{
    // Inline values apply across every ControlState - changing state doesn't make a state-scoped rule reappear.
    UIContext ctx;
    auto root = foundation::MakeRef<RootView>(foundation::DefaultAllocator());
    Init(ctx, root.Get());
    StyleSheet* sheet = SetupSheet(ctx);
    sheet->ForTypeState(&TestView::StaticType(), ControlState::Hover)
        .Set(StyleProperty::TextColor, Color::Red);
    sheet->ForType(&TestView::StaticType()).Set(StyleProperty::TextColor, Color::Blue);

    auto view = foundation::MakeRef<TestView>(foundation::DefaultAllocator());
    root->AddView(view.Get());
    view->SetStyle(StyleProperty::TextColor, Rgb(10, 20, 30));

    CHECK(view->ResolveStyleColor(StyleProperty::TextColor).r == doctest::Approx(10 / 255.0f));
}

TEST_CASE("inline-style: Resolution_InlineBeatsPseudoElementRule")
{
    UIContext ctx;
    auto root = foundation::MakeRef<RootView>(foundation::DefaultAllocator());
    Init(ctx, root.Get());
    StyleSheet* sheet = SetupSheet(ctx);
    sheet->ForTypePseudo(&TestView::StaticType(), u8"thumb").Set(StyleProperty::CornerRadius, 4.0f);

    auto view = foundation::MakeRef<TestView>(foundation::DefaultAllocator());
    root->AddView(view.Get());
    view->SetPartStyle(u8"thumb", StyleProperty::CornerRadius, 16.0f);

    CHECK(view->ResolvePartFloat(u8"thumb", StyleProperty::CornerRadius, ControlState::Normal) ==
          16.0f);
}

TEST_CASE("inline-style: Resolution_InlinePartScopedToItsPart")
{
    // Inline override on "thumb" doesn't affect "track" resolution.
    UIContext ctx;
    auto root = foundation::MakeRef<RootView>(foundation::DefaultAllocator());
    Init(ctx, root.Get());
    StyleSheet* sheet = SetupSheet(ctx);
    sheet->ForTypePseudo(&TestView::StaticType(), u8"track").Set(StyleProperty::CornerRadius, 4.0f);

    auto view = foundation::MakeRef<TestView>(foundation::DefaultAllocator());
    root->AddView(view.Get());
    view->SetPartStyle(u8"thumb", StyleProperty::CornerRadius, 16.0f);

    // "track" still resolves to its rule (4); the thumb override doesn't bleed.
    CHECK(view->ResolvePartFloat(u8"track", StyleProperty::CornerRadius, ControlState::Normal) ==
          4.0f);
}

TEST_CASE("inline-style: Resolution_InlineOnParent_InheritsToChild")
{
    // Inheritable property set inline on a parent reaches the child via the normal inheritance walk.
    UIContext ctx;
    auto root = foundation::MakeRef<RootView>(foundation::DefaultAllocator());
    Init(ctx, root.Get());
    SetupSheet(ctx); // empty sheet - only the parent's inline value can satisfy this.

    auto group = foundation::MakeRef<TestGroup>(foundation::DefaultAllocator());
    auto child = foundation::MakeRef<TestView>(foundation::DefaultAllocator());
    root->AddView(group.Get());
    group->AddView(child.Get());

    group->SetStyle(StyleProperty::TextColor, Rgb(40, 50, 60));

    const Color c = child->ResolveStyleColor(StyleProperty::TextColor);
    CHECK(c.r == doctest::Approx(40 / 255.0f));
    CHECK(c.g == doctest::Approx(50 / 255.0f));
    CHECK(c.b == doctest::Approx(60 / 255.0f));
}

TEST_CASE("inline-style: Resolution_InlineOnChild_BeatsRuleOnParent")
{
    // Child's inline value wins over a rule on the parent type.
    UIContext ctx;
    auto root = foundation::MakeRef<RootView>(foundation::DefaultAllocator());
    Init(ctx, root.Get());
    StyleSheet* sheet = SetupSheet(ctx);
    sheet->ForType(&TestGroup::StaticType()).Set(StyleProperty::TextColor, Color::Red);

    auto group = foundation::MakeRef<TestGroup>(foundation::DefaultAllocator());
    auto child = foundation::MakeRef<TestView>(foundation::DefaultAllocator());
    root->AddView(group.Get());
    group->AddView(child.Get());

    child->SetStyle(StyleProperty::TextColor, Rgb(0, 255, 0));

    CHECK(child->ResolveStyleColor(StyleProperty::TextColor).g == 1.0f);
}

TEST_CASE("inline-style: Resolution_InlineBeatsLocalOnThisView")
{
    // View has both a LocalStyleSheet AND an inline override on the same property. Inline wins.
    UIContext ctx;
    auto root = foundation::MakeRef<RootView>(foundation::DefaultAllocator());
    Init(ctx, root.Get());
    SetupSheet(ctx);

    auto view = foundation::MakeRef<TestView>(foundation::DefaultAllocator());
    root->AddView(view.Get());

    // Local sheet on this view defines TextColor.
    auto local = foundation::MakeRef<StyleSheet>(foundation::DefaultAllocator());
    local->ForType(&TestView::StaticType()).Set(StyleProperty::TextColor, Color::Red);
    view->SetLocalStyleSheet(Move(local));

    // Inline override - should win.
    view->SetStyle(StyleProperty::TextColor, Rgb(0, 255, 0));

    CHECK(view->ResolveStyleColor(StyleProperty::TextColor).g == 1.0f);
}

TEST_CASE("inline-style: Resolution_InlineBeatsLocalOnAncestor")
{
    // LocalStyleSheet sits on a parent; child has the inline override. Inline (on the styled view) wins.
    UIContext ctx;
    auto root = foundation::MakeRef<RootView>(foundation::DefaultAllocator());
    Init(ctx, root.Get());
    SetupSheet(ctx);

    auto group = foundation::MakeRef<TestGroup>(foundation::DefaultAllocator());
    auto child = foundation::MakeRef<TestView>(foundation::DefaultAllocator());
    root->AddView(group.Get());
    group->AddView(child.Get());

    auto parentLocal = foundation::MakeRef<StyleSheet>(foundation::DefaultAllocator());
    parentLocal->ForType(&TestView::StaticType()).Set(StyleProperty::TextColor, Color::Red);
    group->SetLocalStyleSheet(Move(parentLocal));

    child->SetStyle(StyleProperty::TextColor, Rgb(0, 0, 255));

    CHECK(child->ResolveStyleColor(StyleProperty::TextColor).b == 1.0f);
}

// === Drawable ownership via SetStyle ===
//
// Draconic's SetStyle takes a RefPtr<Drawable>: passing a temporary (Move) transfers the caller's ref
// (Beef's consumeRef: true default); passing a retained RefPtr copy keeps the caller's ref alive
// (Beef's consumeRef: false). The inline sheet releases its ref when the view is destroyed or the value
// is overwritten. LiveCount (ctor++/dtor--) proves the release. `delete view` -> view.Reset() (sole owner).

TEST_CASE("inline-style: SetStyle_Drawable_ConsumesByDefault")
{
    const int before = TrackingDrawable::LiveCount;

    auto view = foundation::MakeRef<TestView>(foundation::DefaultAllocator());
    view->SetStyle(StyleProperty::Background,
                   foundation::MakeRef<TrackingDrawable>(foundation::DefaultAllocator()));

    CHECK(TrackingDrawable::LiveCount == before + 1);

    view.Reset();

    CHECK(TrackingDrawable::LiveCount == before);
}

TEST_CASE("inline-style: SetStyle_Drawable_OptOutPreservesCallerRef")
{
    const int before = TrackingDrawable::LiveCount;

    RefPtr<Drawable> d = foundation::MakeRef<TrackingDrawable>(foundation::DefaultAllocator());

    auto view = foundation::MakeRef<TestView>(foundation::DefaultAllocator());
    view->SetStyle(StyleProperty::Background, d); // copy keeps our ref (Beef consumeRef: false)

    CHECK(TrackingDrawable::LiveCount == before + 1);

    view.Reset();

    // Still alive: the caller's ref (d) is intact; it drops at scope exit.
    CHECK(TrackingDrawable::LiveCount == before + 1);
}

TEST_CASE("inline-style: SetStyle_Drawable_MultipleConsumed_AllReleased")
{
    const int before = TrackingDrawable::LiveCount;

    auto view = foundation::MakeRef<TestView>(foundation::DefaultAllocator());
    view->SetStyle(StyleProperty::Background,
                   foundation::MakeRef<TrackingDrawable>(foundation::DefaultAllocator()));
    view->SetPartStyle(u8"thumb", StyleProperty::Background,
                       foundation::MakeRef<TrackingDrawable>(foundation::DefaultAllocator()));

    CHECK(TrackingDrawable::LiveCount == before + 2);

    view.Reset();

    CHECK(TrackingDrawable::LiveCount == before);
}

TEST_CASE("inline-style: SetStyle_Drawable_OverwriteReleasesPrevious")
{
    const int before = TrackingDrawable::LiveCount;

    auto view = foundation::MakeRef<TestView>(foundation::DefaultAllocator());
    view->SetStyle(StyleProperty::Background,
                   foundation::MakeRef<TrackingDrawable>(foundation::DefaultAllocator()));

    CHECK(TrackingDrawable::LiveCount == before + 1);

    // Overwrite with a second drawable - the first should be freed.
    view->SetStyle(StyleProperty::Background,
                   foundation::MakeRef<TrackingDrawable>(foundation::DefaultAllocator()));

    CHECK(TrackingDrawable::LiveCount == before + 1);

    view.Reset();

    CHECK(TrackingDrawable::LiveCount == before);
}

TEST_CASE("inline-style: SetStyle_Drawable_NullClearsAndReleases")
{
    const int before = TrackingDrawable::LiveCount;

    auto view = foundation::MakeRef<TestView>(foundation::DefaultAllocator());
    view->SetStyle(StyleProperty::Background,
                   foundation::MakeRef<TrackingDrawable>(foundation::DefaultAllocator()));

    CHECK(TrackingDrawable::LiveCount == before + 1);

    view->SetStyle(StyleProperty::Background,
                   RefPtr<Drawable>{}); // (Drawable)null -> releases previous

    CHECK(TrackingDrawable::LiveCount == before);

    view.Reset();
}

// === String (FontFamily) inline-style round-trip ===
// (The ResolveStyleFontFamily / FontService resolution subset is already ported in FontFamilyTests.cpp.)

TEST_CASE("inline-style: SetStyle_String_RoundTrip")
{
    auto view = foundation::MakeRef<TestView>(foundation::DefaultAllocator());
    view->SetStyle(StyleProperty::FontFamily, StringView{u8"Roboto"});

    CHECK(view->HasInlineStyle(StyleProperty::FontFamily));
    // Hold the StyleValue in a named local: AsString() returns a StringView INTO it, which would dangle
    // if the StyleValue were a destroyed temporary.
    const StyleValue sv = view->GetInlineStyle(StyleProperty::FontFamily);
    Optional<StringView> v = sv.AsString();
    REQUIRE(v.HasValue());
    CHECK(v.Value() == StringView{u8"Roboto"});
}

TEST_CASE("inline-style: SetStyle_String_OverwriteFreesPrevious")
{
    // Multiple overwrites; each drops the previous owned String (RAII, no leak).
    auto view = foundation::MakeRef<TestView>(foundation::DefaultAllocator());
    view->SetStyle(StyleProperty::FontFamily, StringView{u8"Roboto"});
    view->SetStyle(StyleProperty::FontFamily, StringView{u8"JungleAdventurer"});
    view->SetStyle(StyleProperty::FontFamily, StringView{u8"AttackOfMonster"});

    CHECK(view->GetInlineStyle(StyleProperty::FontFamily).AsString().Value() ==
          StringView{u8"AttackOfMonster"});
}
