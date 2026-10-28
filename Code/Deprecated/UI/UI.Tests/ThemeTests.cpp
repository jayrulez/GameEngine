// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Ported from Sedulous.UI.Tests/src/ThemeTests.bf (faithful). Beef static props ThemePalette.Dark/Light
// -> ThemePalette::Dark()/Light(); `let sheet = DarkTheme.Create(); ctx.StyleSheet = sheet; sheet.ReleaseRef();`
// -> `ctx.SetStyleSheet(DarkTheme::Create(DefaultAllocator()))` (RefPtr ownership; Create returns refcount-1); Color byte
// fields R/G/B -> r/g/b; TestThemeExtension/CountingThemeExtension implement IThemeExtension via *out ptrs.
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
    class TestThemeExtension : public IThemeExtension
    {
    public:
        explicit TestThemeExtension(bool* applied) : m_applied(applied) {}
        void Apply(StyleSheet& /*sheet*/, ThemePalette /*palette*/) override { *m_applied = true; }

    private:
        bool* m_applied;
    };

    class CountingThemeExtension : public IThemeExtension
    {
    public:
        explicit CountingThemeExtension(i32* count) : m_count(count) {}
        void Apply(StyleSheet& /*sheet*/, ThemePalette /*palette*/) override { (*m_count)++; }

    private:
        i32* m_count;
    };
}

// === ThemePalette ===

TEST_CASE("theme: DarkPalette_HasDarkBackground")
{
    ThemePalette p = ThemePalette::Dark();
    CHECK(p.Background.r < 50 / 255.0f);
    CHECK(p.Background.g < 50 / 255.0f);
    CHECK(p.Background.b < 50 / 255.0f);
}

TEST_CASE("theme: LightPalette_HasLightBackground")
{
    ThemePalette p = ThemePalette::Light();
    CHECK(p.Background.r > 200 / 255.0f);
    CHECK(p.Background.g > 200 / 255.0f);
    CHECK(p.Background.b > 200 / 255.0f);
}

TEST_CASE("theme: DarkPalette_TextIsLight")
{
    ThemePalette p = ThemePalette::Dark();
    CHECK(p.Text.r > 200 / 255.0f);
}

TEST_CASE("theme: LightPalette_TextIsDark")
{
    ThemePalette p = ThemePalette::Light();
    CHECK(p.Text.r < 50 / 255.0f);
}

TEST_CASE("theme: GraphiteOrangePalette_IsWarmDarkWithOrangeAccent")
{
    ThemePalette p = ThemePalette::GraphiteOrange();
    // Warm dark background: dark, and red >= green >= blue (a warm, not cool, neutral).
    CHECK(p.Background.r < 50 / 255.0f);
    CHECK(p.Background.r >= p.Background.g);
    CHECK(p.Background.g >= p.Background.b);
    // Orange accent: warm and bright (red high, green mid, blue low).
    CHECK(p.PrimaryAccent.r > 200 / 255.0f);
    CHECK(p.PrimaryAccent.b < p.PrimaryAccent.g);
    CHECK(p.PrimaryAccent.g < p.PrimaryAccent.r);
    // Light warm text.
    CHECK(p.Text.r > 200 / 255.0f);
}

// === DarkTheme ===

TEST_CASE("theme: DarkTheme_Creates")
{
    auto sheet = DarkTheme::Create(DefaultAllocator());
    CHECK(sheet.Get() != nullptr);
    CHECK(sheet->RuleCount() > 0);
}

TEST_CASE("theme: RoundedDarkTheme_CreatesWithCustomPalette")
{
    auto sheet = RoundedDarkTheme::Create(DefaultAllocator(), ThemePalette::GraphiteOrange());
    CHECK(sheet.Get() != nullptr);
    CHECK(sheet->RuleCount() > 0);
}

TEST_CASE("theme: DarkTheme_ResolvesTextColor")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    ctx.SetStyleSheet(DarkTheme::Create(DefaultAllocator()));

    auto view = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(view.Get());

    const Color color = view->ResolveStyleColor(StyleProperty::TextColor);
    // Dark theme text should be light
    CHECK(color.r > 200 / 255.0f);
}

TEST_CASE("theme: DarkTheme_ResolvesFontSize")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    ctx.SetStyleSheet(DarkTheme::Create(DefaultAllocator()));

    auto view = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(view.Get());

    const f32 size = view->ResolveStyleFloat(StyleProperty::FontSize);
    CHECK(size == 16.0f);
}

TEST_CASE("theme: DarkTheme_ButtonStyleClass")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    ctx.SetStyleSheet(DarkTheme::Create(DefaultAllocator()));

    auto view = core::MakeRef<Button>(core::DefaultAllocator(), StringView(u8"Test"));
    root->AddView(view.Get());

    // Button should have a background drawable (matched by type)
    Drawable* bg = view->ResolveStyleDrawable(StyleProperty::Background);
    CHECK(bg != nullptr);

    // Button should have padding
    const Thickness pad = view->ResolveStyleThickness(StyleProperty::Padding);
    CHECK(pad.Left > 0);

    // Button corner radius (flat themes use 0)
    const f32 radius = view->ResolveStyleFloat(StyleProperty::CornerRadius);
    CHECK(radius == 0.0f);
}

// === LightTheme ===

TEST_CASE("theme: LightTheme_Creates")
{
    auto sheet = LightTheme::Create(DefaultAllocator());
    CHECK(sheet.Get() != nullptr);
    CHECK(sheet->RuleCount() > 0);
}

TEST_CASE("theme: LightTheme_ResolvesTextColor")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    ctx.SetStyleSheet(LightTheme::Create(DefaultAllocator()));

    auto view = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(view.Get());

    const Color color = view->ResolveStyleColor(StyleProperty::TextColor);
    // Light theme text should be dark
    CHECK(color.r < 50 / 255.0f);
}

// === Theme switching ===

TEST_CASE("theme: ThemeSwitching_ChangesResolvedValues")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());

    auto view = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(view.Get());

    // Dark theme
    ctx.SetStyleSheet(DarkTheme::Create(DefaultAllocator()));
    const Color darkText = view->ResolveStyleColor(StyleProperty::TextColor);

    // Light theme
    ctx.SetStyleSheet(LightTheme::Create(DefaultAllocator()));
    const Color lightText = view->ResolveStyleColor(StyleProperty::TextColor);

    // Colors should be different
    CHECK(darkText.r != lightText.r);
    // Dark text is light, light text is dark
    CHECK(darkText.r > 200 / 255.0f);
    CHECK(lightText.r < 50 / 255.0f);
}

// === Custom palette ===

TEST_CASE("theme: DarkTheme_WithCustomPalette")
{
    ThemePalette palette = ThemePalette::Dark();
    palette.Text = Color{1.0f, 0.0f, 0.0f, 1.0f}; // red text

    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    ctx.SetStyleSheet(DarkTheme::Create(DefaultAllocator(), palette));

    auto view = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(view.Get());

    const Color color = view->ResolveStyleColor(StyleProperty::TextColor);
    CHECK((color.r == 1.0f && color.g == 0.0f && color.b == 0.0f));
}

// === ThemeRegistry ===

TEST_CASE("theme: ThemeRegistry_ExtensionApplied")
{
    bool applied = false;
    TestThemeExtension ext(&applied);
    ThemeRegistry::RegisterExtension(&ext);

    auto sheet = DarkTheme::Create(DefaultAllocator());
    CHECK(applied);

    ThemeRegistry::UnregisterExtension(&ext);
}

TEST_CASE("theme: ThemeRegistry_ExtensionAppliedToBothThemes")
{
    i32 applyCount = 0;
    CountingThemeExtension ext(&applyCount);
    ThemeRegistry::RegisterExtension(&ext);

    auto dark = DarkTheme::Create(DefaultAllocator());
    auto light = LightTheme::Create(DefaultAllocator());
    CHECK(applyCount == 2);

    ThemeRegistry::UnregisterExtension(&ext);
}

// === ThemeIconSet ===

TEST_CASE("theme-icon-set: initialized set shares ONE instance per glyph; tints dedupe")
{
    ThemeIconSet& set = ThemeIconSet::Get();
    set.Initialize();

    RefPtr<Drawable> a = ThemeIconSet::Acquire(ThemeIcon::Close);
    RefPtr<Drawable> b = ThemeIconSet::Acquire(ThemeIcon::Close);
    CHECK(a.Get() == b.Get()); // shared - the host bakes ONE instance, every theme gets it

    const Color tint{0.2f, 0.3f, 0.4f, 1.0f};
    RefPtr<Drawable> t1 = ThemeIconSet::Acquire(ThemeIcon::Close, tint);
    RefPtr<Drawable> t2 = ThemeIconSet::Acquire(ThemeIcon::Close, tint);
    CHECK(t1.Get() == t2.Get());  // same (glyph, tint) dedupes
    CHECK(t1.Get() != a.Get());   // tinted variant is its own instance

    Array<BakedSVGDrawable*> bakeable;
    set.CollectBakeable(bakeable);
    CHECK(bakeable.Size() == ThemeIconSet::kGlyphCount + 1); // 10 base + 1 tinted

    set.Shutdown(); // test hygiene - later tests see the uninitialized (fallback) behavior
}

TEST_CASE("theme-icon-set: uninitialized set hands out fresh unbaked fallbacks")
{
    ThemeIconSet& set = ThemeIconSet::Get();
    set.Shutdown(); // ensure uninitialized regardless of test order

    RefPtr<Drawable> a = ThemeIconSet::Acquire(ThemeIcon::Close);
    RefPtr<Drawable> b = ThemeIconSet::Acquire(ThemeIcon::Close);
    REQUIRE(a.Get() != nullptr);
    CHECK(a.Get() != b.Get()); // distinct instances - the old (headless/tests) behavior
}
