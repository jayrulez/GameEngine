// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Ported from Sedulous.UI.Tests/src/SSSParserTests.bf.
//
// The resolution-pipeline tests are driven end-to-end through a real
// UIContext + RootView + TestView (registering "View"/element selectors explicitly, since
// UITypeRegistry::RegisterBuiltins is not implemented while controls are unported).
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.ui;
import foundation.image;
import foundation.vg; // CornerRadii
#include "TestHelpers.h"

using namespace foundation::ui;
using namespace foundation::ui::tests;
using namespace foundation::core;
namespace core = foundation::core;
namespace image = foundation::image;
namespace vg = foundation::vg;

// Register the drawable factories + the element types these parser tests reference.
static void EnsureGlobals()
{
    StyleSheetLoader::InitializeGlobals();
    UITypeRegistry::Register(u8"View", &View::StaticType());
    UITypeRegistry::Register(u8"TestView", &TestView::StaticType());
    UITypeRegistry::Register(u8"TestGroup", &TestGroup::StaticType());
    UITypeRegistry::Register(u8"ButtonBase", &ButtonBase::StaticType());
    UITypeRegistry::Register(u8"Button", &Button::StaticType());
    UITypeRegistry::Register(u8"CheckBox", &CheckBox::StaticType());
}

// A UIContext + RootView with a stylesheet applied; the test adds views under `root`.
struct Fixture
{
    UIContext ctx{DefaultAllocator()};
    core::RefPtr<RootView> root = core::MakeRef<RootView>(core::DefaultAllocator());

    explicit Fixture(core::RefPtr<StyleSheet> sheet)
    {
        EnsureGlobals();
        Init(ctx, root.Get());
        ctx.SetStyleSheet(Move(sheet));
    }

    core::RefPtr<TestView> AddView()
    {
        core::RefPtr<TestView> v = core::MakeRef<TestView>(core::DefaultAllocator(), 50.0f, 30.0f);
        root->AddView(v.Get());
        return v;
    }
};

static core::RefPtr<StyleSheet> LoadSSS(StringView src)
{
    StyleSheetLoader loader(DefaultAllocator());
    return loader.Load(src);
}

namespace
{
    // Byte Color(r,g,b,a) -> float Color helper (mirrors the Beef Color(r,g,b,255) ctor).
    Color Rgb(core::u8 r, core::u8 g, core::u8 b, core::u8 a = 255)
    {
        return Color{r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
    }

    // Port of Sedulous.UI.Tests MockResourceProvider: an in-memory IResourceProvider for @import /
    // @icon / @image. Owned image data is kept alive as a member so the LoadImage() pointer stays valid.
    class MockResourceProvider : public IResourceProvider
    {
    public:
        void AddText(StringView path, StringView content)
        {
            m_texts.InsertOrAssign(String(path), String(content));
        }

        void AddImage(StringView path) { AddImagePath(path); } // 2x2 RGBA white pixel image

        bool LoadText(StringView path, String& outText) override
        {
            if (const String* v = m_texts.Find(String(path)))
            {
                outText = *v;
                return true;
            }
            return false;
        }

        const image::ImageData* LoadImage(StringView path) override
        {
            for (core::usize i = 0; i < m_imagePaths.Size(); ++i)
            {
                if (m_imagePaths[i] == path)
                {
                    return m_images[i].Get();
                }
            }
            return nullptr;
        }

    private:
        void AddImagePath(StringView path)
        {
            const core::u8 pixels[16] = {255, 255, 255, 255, 255, 255, 255, 255,
                                         255, 255, 255, 255, 255, 255, 255, 255};
            m_imagePaths.PushBack(String(path));
            m_images.PushBack(core::MakeUnique<image::OwnedImageData>(
                core::DefaultAllocator(), 2u, 2u, image::PixelFormat::RGBA8,
                core::Span<const core::u8>(pixels, 16)));
        }

        core::HashMap<String, String> m_texts;
        core::Array<String> m_imagePaths;                             // parallel to m_images
        core::Array<core::UniquePtr<image::OwnedImageData>> m_images; // owns each image
    };
} // namespace

// === Hex color parsing (StyleValueParser) ===

TEST_CASE("sss: HexColor_6Digit")
{
    core::Optional<Color> c = StyleValueParser::ParseHexColor(u8"#4a8eff");
    REQUIRE(c.HasValue());
    CHECK(c.Value().r == doctest::Approx(0x4a / 255.0f));
    CHECK(c.Value().g == doctest::Approx(0x8e / 255.0f));
    CHECK(c.Value().b == 1.0f);
    CHECK(c.Value().a == 1.0f);
}

TEST_CASE("sss: HexColor_8Digit")
{
    core::Optional<Color> c = StyleValueParser::ParseHexColor(u8"#4a8effcc");
    REQUIRE(c.HasValue());
    CHECK(c.Value().r == doctest::Approx(0x4a / 255.0f));
    CHECK(c.Value().a == doctest::Approx(0xcc / 255.0f));
}

TEST_CASE("sss: Palette_Derivation")
{
    const Color dark = Palette::Darken(Color{200 / 255.0f, 200 / 255.0f, 200 / 255.0f, 1.0f}, 0.5f);
    CHECK(dark.r == doctest::Approx(100 / 255.0f));
    const Color light = Palette::Lighten(Color{0.0f, 0.0f, 0.0f, 1.0f}, 0.5f);
    CHECK(light.r > 100 / 255.0f);
}

// === Basic rule parsing (parse + inspect) ===

TEST_CASE("sss: CompoundStateRule")
{
    EnsureGlobals();
    core::RefPtr<StyleSheet> sheet = LoadSSS(u8R"(
        View { text-color: #ffffff; }
        View:checked:hover { text-color: #ff0000; }
    )");
    CHECK(sheet->RuleCount() == 2);
    const StyleRule& rule = sheet->GetRule(1);
    REQUIRE(rule.Selector.State.HasValue());
    const ControlState state = rule.Selector.State.Value();
    CHECK(HasFlag(state, ControlState::Checked));
    CHECK(HasFlag(state, ControlState::Hover));
}

TEST_CASE("sss: AllDrawableProperties_Parse")
{
    EnsureGlobals();
    core::RefPtr<StyleSheet> sheet = LoadSSS(u8R"(
        View { background: color(#111); checked-background: color(#222); menu-item-hover-drawable: color(#333); }
    )");
    CHECK(sheet->RuleCount() == 1);
    CHECK(sheet->GetRule(0).PropertyCount() == 3);
}

TEST_CASE("sss: AllColorProperties_Parse")
{
    EnsureGlobals();
    core::RefPtr<StyleSheet> sheet = LoadSSS(u8R"(
        View { text-color: #111; text-dim-color: #222; placeholder-color: #333; border-color: #444;
               cursor-color: #555; selection-color: #666; accent-color: #777; }
    )");
    CHECK(sheet->GetRule(0).PropertyCount() == 7);
}

TEST_CASE("sss: AllFloatProperties_Parse")
{
    EnsureGlobals();
    core::RefPtr<StyleSheet> sheet = LoadSSS(u8R"(
        View { font-size: 16; corner-radius: 4; border-width: 1; spacing: 8; opacity: 0.5; width: 100; height: 50; }
    )");
    CHECK(sheet->GetRule(0).PropertyCount() == 7);
}

TEST_CASE("sss: Comments_Ignored")
{
    EnsureGlobals();
    core::RefPtr<StyleSheet> sheet = LoadSSS(u8R"(
        /* comment */
        View { font-size: 14; /* inline */ }
    )");
    CHECK(sheet->RuleCount() == 1);
}

// === Resolution through the View cluster ===

TEST_CASE("sss: SimpleTypeRule")
{
    Fixture f(LoadSSS(u8"View { text-color: #ff0000; font-size: 16; }"));
    core::RefPtr<TestView> view = f.AddView();

    const Color color = view->ResolveStyleColor(StyleProperty::TextColor);
    CHECK(color.r == 1.0f);
    CHECK(color.g == 0);
    CHECK(color.b == 0);
    CHECK(view->ResolveStyleFloat(StyleProperty::FontSize) == doctest::Approx(16));
}

TEST_CASE("sss: ClassRule")
{
    Fixture f(LoadSSS(u8".primary { font-size: 24; }"));
    core::RefPtr<TestView> view = f.AddView();
    view->AddClass(u8"primary");
    CHECK(view->ResolveStyleFloat(StyleProperty::FontSize) == 24.0f);
}

TEST_CASE("sss: StateRule")
{
    Fixture f(LoadSSS(u8"View { font-size: 12; } View:disabled { font-size: 10; }"));
    core::RefPtr<TestView> view = f.AddView();
    view->IsEnabled = false;
    CHECK(view->ResolveStyleFloat(StyleProperty::FontSize) == doctest::Approx(10));
}

TEST_CASE("sss: PaletteVariable")
{
    Fixture f(LoadSSS(u8R"(
        @palette dark { text: #e0e0ee; }
        View { text-color: $text; }
    )"));
    core::RefPtr<TestView> view = f.AddView();
    CHECK(view->ResolveStyleColor(StyleProperty::TextColor).r == doctest::Approx(0xe0 / 255.0f));
}

TEST_CASE("sss: SetPalette_ThemePalette")
{
    StyleSheetLoader loader(DefaultAllocator());
    loader.SetPalette(ThemePalette::Dark());
    Fixture f(loader.Load(u8"View { text-color: $text; }"));
    core::RefPtr<TestView> view = f.AddView();
    CHECK(view->ResolveStyleColor(StyleProperty::TextColor).r == doctest::Approx(220 / 255.0f));
}

TEST_CASE("sss: ColorFunction_Lighten")
{
    Fixture f(LoadSSS(u8"View { text-color: lighten(#000000, 50%); }"));
    core::RefPtr<TestView> view = f.AddView();
    CHECK(view->ResolveStyleColor(StyleProperty::TextColor).r > 100 / 255.0f);
}

TEST_CASE("sss: ColorFunction_Alpha")
{
    Fixture f(LoadSSS(u8"View { text-color: alpha(#ff0000, 0.5); }"));
    core::RefPtr<TestView> view = f.AddView();
    const Color c = view->ResolveStyleColor(StyleProperty::TextColor);
    CHECK(c.r == 1.0f);
    CHECK(c.a == doctest::Approx(0.5f));
}

TEST_CASE("sss: ColorFunction_Mix")
{
    Fixture f(LoadSSS(u8"View { text-color: mix(#000000, #ffffff, 0.5); }"));
    core::RefPtr<TestView> view = f.AddView();
    const f32 r = view->ResolveStyleColor(StyleProperty::TextColor).r;
    CHECK((r > 100 / 255.0f && r < 160 / 255.0f));
}

TEST_CASE("sss: NamedColor")
{
    Fixture f(LoadSSS(u8"View { text-color: white; }"));
    core::RefPtr<TestView> view = f.AddView();
    const Color c = view->ResolveStyleColor(StyleProperty::TextColor);
    CHECK((c.r == 1.0f && c.g == 1.0f && c.b == 1.0f));
}

TEST_CASE("sss: RgbFunction")
{
    Fixture f(LoadSSS(u8"View { text-color: rgb(100, 150, 200); }"));
    core::RefPtr<TestView> view = f.AddView();
    const Color c = view->ResolveStyleColor(StyleProperty::TextColor);
    CHECK(c.r == doctest::Approx(100 / 255.0f));
    CHECK(c.g == doctest::Approx(150 / 255.0f));
    CHECK(c.b == doctest::Approx(200 / 255.0f));
}

TEST_CASE("sss: RgbaFunction")
{
    Fixture f(LoadSSS(u8"View { text-color: rgba(100, 150, 200, 0.5); }"));
    core::RefPtr<TestView> view = f.AddView();
    const Color c = view->ResolveStyleColor(StyleProperty::TextColor);
    CHECK(c.r == doctest::Approx(100 / 255.0f));
    CHECK(c.a == doctest::Approx(0.5f));
}

// === Drawable factories (View-compatible) ===

TEST_CASE("sss: DrawableFactory_Color")
{
    Fixture f(LoadSSS(u8"View { background: color(#336699); }"));
    core::RefPtr<TestView> view = f.AddView();
    Drawable* bg = view->ResolveStyleDrawable(StyleProperty::Background);
    REQUIRE(bg != nullptr);
    CHECK(core::Cast<ColorDrawable>(bg) != nullptr);
}

TEST_CASE("sss: DrawableFactory_RoundedRect")
{
    Fixture f(LoadSSS(
        u8"View { background: rounded-rect(#336699, radius=6, border=#555555, border-width=1); }"));
    core::RefPtr<TestView> view = f.AddView();
    Drawable* bg = view->ResolveStyleDrawable(StyleProperty::Background);
    REQUIRE(bg != nullptr);
    RoundedRectDrawable* rrd = core::Cast<RoundedRectDrawable>(bg);
    REQUIRE(rrd != nullptr);
    CHECK(rrd->FillColor.r == doctest::Approx(0x33 / 255.0f));
    CHECK(rrd->BorderWidth == 1.0f);
}

TEST_CASE("sss: DrawableFactory_Gradient")
{
    Fixture f(LoadSSS(u8"View { background: gradient(left-to-right, #000000, #ffffff); }"));
    core::RefPtr<TestView> view = f.AddView();
    Drawable* bg = view->ResolveStyleDrawable(StyleProperty::Background);
    REQUIRE(bg != nullptr);
    GradientDrawable* gd = core::Cast<GradientDrawable>(bg);
    REQUIRE(gd != nullptr);
    CHECK(gd->Direction == GradientDirection::LeftToRight);
}

TEST_CASE("sss: DrawableFactory_StateList")
{
    Fixture f(LoadSSS(u8R"(
        View { background: state-list(normal=color(#111111), hover=color(#222222), pressed=color(#333333)); }
    )"));
    core::RefPtr<TestView> view = f.AddView();
    Drawable* bg = view->ResolveStyleDrawable(StyleProperty::Background);
    REQUIRE(bg != nullptr);
    CHECK(core::Cast<StateListDrawable>(bg) != nullptr);
}

TEST_CASE("sss: DrawableFactory_Layer")
{
    Fixture f(LoadSSS(u8"View { background: layer(color(#111111), color(#222222)); }"));
    core::RefPtr<TestView> view = f.AddView();
    Drawable* bg = view->ResolveStyleDrawable(StyleProperty::Background);
    REQUIRE(bg != nullptr);
    CHECK(core::Cast<LayerDrawable>(bg) != nullptr);
}

TEST_CASE("sss: DrawableFactory_Inset")
{
    Fixture f(LoadSSS(u8"View { background: inset(color(#336699), 4, 4, 4, 4); }"));
    core::RefPtr<TestView> view = f.AddView();
    Drawable* bg = view->ResolveStyleDrawable(StyleProperty::Background);
    REQUIRE(bg != nullptr);
    InsetDrawable* id = core::Cast<InsetDrawable>(bg);
    REQUIRE(id != nullptr);
    CHECK(id->Inset.Top == 4);
}

TEST_CASE("sss: ColorLiteral_AsDrawable")
{
    Fixture f(LoadSSS(u8"View { background: #336699; }"));
    core::RefPtr<TestView> view = f.AddView();
    Drawable* bg = view->ResolveStyleDrawable(StyleProperty::Background);
    REQUIRE(bg != nullptr);
    ColorDrawable* cd = core::Cast<ColorDrawable>(bg);
    REQUIRE(cd != nullptr);
    CHECK(cd->Color.r == doctest::Approx(0x33 / 255.0f));
}

TEST_CASE("sss: Variable_InDrawable")
{
    Fixture f(LoadSSS(u8R"(
        @palette dark { surface: #24242c; border: #3a3a45; }
        View { background: rounded-rect($surface, radius=6, border=$border, border-width=1); }
    )"));
    core::RefPtr<TestView> view = f.AddView();
    Drawable* bg = view->ResolveStyleDrawable(StyleProperty::Background);
    RoundedRectDrawable* rrd = core::Cast<RoundedRectDrawable>(bg);
    REQUIRE(rrd != nullptr);
    CHECK(rrd->FillColor.r == doctest::Approx(0x24 / 255.0f));
    CHECK(rrd->BorderColor.r == doctest::Approx(0x3a / 255.0f));
}

// === Property types ===

TEST_CASE("sss: ThicknessProperty_SingleValue")
{
    Fixture f(LoadSSS(u8"View { padding: 8; }"));
    core::RefPtr<TestView> view = f.AddView();
    const Thickness pad = view->ResolveStyleThickness(StyleProperty::Padding);
    CHECK((pad.Left == 8 && pad.Top == 8 && pad.Right == 8 && pad.Bottom == 8));
}

TEST_CASE("sss: ThicknessProperty_TwoValues")
{
    Fixture f(LoadSSS(u8"View { padding: 8 12; }"));
    core::RefPtr<TestView> view = f.AddView();
    const Thickness pad = view->ResolveStyleThickness(StyleProperty::Padding);
    CHECK(pad.Top == 8);
    CHECK(pad.Left == 12);
}

TEST_CASE("sss: ThicknessProperty_FourValues")
{
    Fixture f(LoadSSS(u8"View { padding: 1 2 3 4; }"));
    core::RefPtr<TestView> view = f.AddView();
    const Thickness pad = view->ResolveStyleThickness(StyleProperty::Padding);
    CHECK((pad.Top == 1 && pad.Right == 2 && pad.Bottom == 3 && pad.Left == 4));
}

TEST_CASE("sss: BoolProperty")
{
    Fixture f(LoadSSS(u8"View { word-wrap: true; }"));
    core::RefPtr<TestView> view = f.AddView();
    core::Optional<bool> b = view->ResolveStyle(StyleProperty::WordWrap).AsBool();
    REQUIRE(b.HasValue());
    CHECK(b.Value() == true);
}

TEST_CASE("sss: FloatProperty")
{
    Fixture f(LoadSSS(u8"View { corner-radius: 6; }"));
    core::RefPtr<TestView> view = f.AddView();
    CHECK(view->ResolveStyleFloat(StyleProperty::CornerRadius) == 6.0f);
}

TEST_CASE("sss: FontFamily_QuotedString")
{
    Fixture f(LoadSSS(u8"View { font-family: \"Lilita One\"; }"));
    core::RefPtr<TestView> view = f.AddView();
    const StyleValue v =
        view->ResolveStyle(StyleProperty::FontFamily); // hold alive: AsString borrows its String
    core::Optional<StringView> s = v.AsString();
    REQUIRE(s.HasValue());
    CHECK(s.Value() == StringView(u8"Lilita One"));
}

// === Cascade + inheritance ===

TEST_CASE("sss: Cascade_ClassBeatsType")
{
    Fixture f(LoadSSS(u8"View { font-size: 12; } .big { font-size: 24; }"));
    core::RefPtr<TestView> view = f.AddView();
    view->AddClass(u8"big");
    CHECK(view->ResolveStyleFloat(StyleProperty::FontSize) == 24.0f);
}

TEST_CASE("sss: Cascade_TypeStateBeatsType")
{
    Fixture f(LoadSSS(u8"View { text-color: #cccccc; } View:disabled { text-color: #333333; }"));
    core::RefPtr<TestView> view = f.AddView();
    view->IsEnabled = false;
    CHECK(view->ResolveStyleColor(StyleProperty::TextColor).r == doctest::Approx(0x33 / 255.0f));
}

TEST_CASE("sss: Inheritance_TextColor")
{
    Fixture f(LoadSSS(u8"View { text-color: #aabbcc; }"));
    core::RefPtr<TestGroup> group = core::MakeRef<TestGroup>(core::DefaultAllocator());
    f.root->AddView(group.Get());
    core::RefPtr<TestView> child = core::MakeRef<TestView>(core::DefaultAllocator(), 50.0f, 30.0f);
    group->AddView(child.Get());
    // Child inherits text-color from the View rule + inheritance walk.
    CHECK(child->ResolveStyleColor(StyleProperty::TextColor).r == doctest::Approx(0xaa / 255.0f));
}

TEST_CASE("sss: MultipleRules_SameType")
{
    Fixture f(LoadSSS(u8"View { font-size: 12; } View { text-color: #ff0000; }"));
    core::RefPtr<TestView> view = f.AddView();
    CHECK(view->ResolveStyleFloat(StyleProperty::FontSize) == 12.0f);
    CHECK(view->ResolveStyleColor(StyleProperty::TextColor).r == 1.0f);
}

// === Subtype matching + control-typed drawable factories ===

TEST_CASE("sss: SubtypeMatching_ButtonMatchesButtonBase")
{
    Fixture f(LoadSSS(u8"ButtonBase { padding: 10 20; }"));
    auto btn = core::MakeRef<Button>(core::DefaultAllocator(), StringView(u8"Test"));
    f.root->AddView(btn.Get());
    const Thickness pad = btn->ResolveStyleThickness(StyleProperty::Padding);
    CHECK(pad.Top == 10);
    CHECK(pad.Left == 20);
}

TEST_CASE("sss: SubtypeMatching_ViewMatchesAll")
{
    Fixture f(LoadSSS(u8"View { font-size: 13; }"));
    auto btn = core::MakeRef<Button>(core::DefaultAllocator(), StringView(u8"B"));
    auto cb = core::MakeRef<CheckBox>(core::DefaultAllocator(), StringView(u8"C"));
    f.root->AddView(btn.Get());
    f.root->AddView(cb.Get());
    CHECK(btn->ResolveStyleFloat(StyleProperty::FontSize) == 13.0f);
    CHECK(cb->ResolveStyleFloat(StyleProperty::FontSize) == 13.0f);
}

TEST_CASE("sss: DrawableFactory_StateColors")
{
    Fixture f(LoadSSS(u8"ButtonBase { background: state-colors(#334455); }"));
    auto btn = core::MakeRef<Button>(core::DefaultAllocator(), StringView(u8"Test"));
    f.root->AddView(btn.Get());
    Drawable* bg = btn->ResolveStyleDrawable(StyleProperty::Background);
    REQUIRE(bg != nullptr);
    CHECK(core::Cast<StateListDrawable>(bg) != nullptr);
}

TEST_CASE("sss: DrawableFactory_StateRounded")
{
    Fixture f(LoadSSS(u8"ButtonBase { background: state-rounded(#334455, radius=4); }"));
    auto btn = core::MakeRef<Button>(core::DefaultAllocator(), StringView(u8"Test"));
    f.root->AddView(btn.Get());
    Drawable* bg = btn->ResolveStyleDrawable(StyleProperty::Background);
    REQUIRE(bg != nullptr);
    CHECK(core::Cast<StateListDrawable>(bg) != nullptr);
}

// Inline style="..." with a drawable function (regression: ApplyInlineStyle must register the drawable
// factory builtins - rounded-rect/gradient/state-* - itself, or the value falls back to a plain white
// color; StyleSheetLoader did this for .sss files but the inline path did not).
TEST_CASE("sss: InlineStyle_RoundedRectFunction")
{
    EnsureGlobals();
    auto view = core::MakeRef<TestView>(core::DefaultAllocator());
    SSSParser::ApplyInlineStyle(view.Get(),
                                StringView(u8"background: rounded-rect(rgb(35, 38, 48), radius=12, "
                                           u8"border-width=2, border=rgb(80, 90, 110));"));

    Drawable* bg = view->ResolveStyleDrawable(StyleProperty::Background);
    REQUIRE(bg != nullptr);
    auto* rr = core::Cast<RoundedRectDrawable>(bg);
    REQUIRE(rr != nullptr); // not a fallback ColorDrawable
    CHECK(rr->FillColor.r == doctest::Approx(35 / 255.0f));
    CHECK(rr->FillColor.g == doctest::Approx(38 / 255.0f));
    CHECK(rr->FillColor.b == doctest::Approx(48 / 255.0f));
    CHECK(rr->Radii.topLeft == 12.0f);
    CHECK(rr->BorderWidth == 2.0f);
    CHECK(rr->BorderColor.r == doctest::Approx(80 / 255.0f));
}

// UITypeRegistry::RegisterBuiltins registers the built-in control type names, so .sss element selectors
// resolve to a concrete type. Regression: without it, unresolved type names (e.g. ComboBox) matched
// nothing correctly and a pseudo-element rule like `ComboBox::arrow` leaked its drawable onto other
// controls' backgrounds (dropdown arrows appeared on buttons + text fields in the Breeze theme).
TEST_CASE("sss: RegisterBuiltins_ResolvesTypes")
{
    UITypeRegistry::RegisterBuiltins();
    CHECK(UITypeRegistry::Resolve(u8"ComboBox") != nullptr);
    CHECK(UITypeRegistry::Resolve(u8"EditText") != nullptr);
    CHECK(UITypeRegistry::Resolve(u8"NumericField") != nullptr);
    CHECK(UITypeRegistry::Resolve(u8"Flex") == &FlexLayout::StaticType()); // alias
}

TEST_CASE("sss: RegisterBuiltins is safe to call from many threads at once (the theme cook does)")
{
    // The UI theme cook reaches this through StyleSheetLoader on job workers; a plain flag let
    // two builds rehash the type map under each other. Hammer it, then the map is intact and
    // holds exactly what one registration holds.
    UITypeRegistry::RegisterBuiltins();
    const usize expected = UITypeRegistry::Count();
    core::JobSystem jobs(core::DefaultAllocator(), 4);
    jobs.ParallelFor(64, [](core::u32) { UITypeRegistry::RegisterBuiltins(); }, 1);
    CHECK(UITypeRegistry::Count() == expected);
    CHECK(UITypeRegistry::Resolve(u8"ComboBox") != nullptr);
    CHECK(UITypeRegistry::Resolve(u8"Flex") == &FlexLayout::StaticType());
}

TEST_CASE("sss: TypeSelectors_DoNotLeakAcrossControls")
{
    EnsureGlobals();
    UITypeRegistry::RegisterBuiltins();
    // A ButtonBase background + a ComboBox arrow pseudo-element (the shape breeze.sss uses).
    Fixture f(
        LoadSSS(u8"ButtonBase { background: rounded-rect(rgb(10,20,30), radius=2); }"
                u8" ComboBox::arrow { background: rounded-rect(rgb(200,100,50), radius=2); }"));
    auto btn = core::MakeRef<Button>(core::DefaultAllocator(), StringView(u8"x"));
    f.root->AddView(btn.Get());

    Drawable* bg = btn->ResolveStyleDrawable(StyleProperty::Background);
    REQUIRE(bg != nullptr);
    auto* rr = core::Cast<RoundedRectDrawable>(bg);
    REQUIRE(rr != nullptr);
    // The button resolves the ButtonBase background, NOT the ComboBox::arrow drawable.
    CHECK(rr->FillColor.r == doctest::Approx(10 / 255.0f));
    CHECK(rr->FillColor.g == doctest::Approx(20 / 255.0f));
}

// === Ported from Sedulous.UI.Tests ===

TEST_CASE("sss: ColorFunction_Darken")
{
    Fixture f(LoadSSS(u8"View { text-color: darken(#ffffff, 50%); }"));
    core::RefPtr<TestView> view = f.AddView();
    // darken white by 50% -> ~(128, 128, 128)
    const f32 r = view->ResolveStyleColor(StyleProperty::TextColor).r;
    CHECK((r < 200 / 255.0f && r > 100 / 255.0f));
}

TEST_CASE("sss: DrawableFactory_GradientWithDirection")
{
    Fixture f(LoadSSS(u8"View { background: gradient(left-to-right, #000000, #ffffff); }"));
    core::RefPtr<TestView> view = f.AddView();
    Drawable* bg = view->ResolveStyleDrawable(StyleProperty::Background);
    REQUIRE(bg != nullptr);
    GradientDrawable* gd = core::Cast<GradientDrawable>(bg);
    REQUIRE(gd != nullptr);
    CHECK(gd->Direction == GradientDirection::LeftToRight);
}

TEST_CASE("sss: DrawableFactory_Svg")
{
    EnsureGlobals();
    StyleSheetLoader loader(DefaultAllocator());
    loader.RegisterSvg(u8"checkmark", u8R"(<svg viewBox="0 0 16 16">
  <path d="M3 8 L6.5 11.5 L13 5" fill="none" stroke="white" stroke-width="2"/>
</svg>)");
    Fixture f(loader.Load(u8"CheckBox::checkmark { background: svg(checkmark); }"));
    auto cb = core::MakeRef<CheckBox>(core::DefaultAllocator(), StringView(u8"Test"));
    f.root->AddView(cb.Get());
    Drawable* icon =
        cb->ResolvePartDrawable(u8"checkmark", StyleProperty::Background, ControlState::Normal);
    REQUIRE(icon != nullptr);
    CHECK(core::Cast<SVGDrawable>(icon) != nullptr);
}

TEST_CASE("sss: DrawableFactory_SvgWithTint")
{
    EnsureGlobals();
    StyleSheetLoader loader(DefaultAllocator());
    loader.RegisterSvg(u8"checkmark", u8R"(<svg viewBox="0 0 16 16">
  <path d="M3 8 L6.5 11.5 L13 5" fill="none" stroke="white" stroke-width="2"/>
</svg>)");
    Fixture f(loader.Load(u8"CheckBox::checkmark { background: svg(checkmark, tint=#ff0000); }"));
    auto cb = core::MakeRef<CheckBox>(core::DefaultAllocator(), StringView(u8"Test"));
    f.root->AddView(cb.Get());
    Drawable* icon =
        cb->ResolvePartDrawable(u8"checkmark", StyleProperty::Background, ControlState::Normal);
    REQUIRE(icon != nullptr);
    SVGDrawable* svgd = core::Cast<SVGDrawable>(icon);
    REQUIRE(svgd != nullptr);
    REQUIRE(svgd->TintColor.HasValue());
    CHECK(svgd->TintColor.Value().r == 1.0f);
}

TEST_CASE("sss: FontFamily_BareIdentifier")
{
    Fixture f(LoadSSS(u8"View { font-family: Chewy; }"));
    core::RefPtr<TestView> view = f.AddView();
    const StyleValue v = view->ResolveStyle(StyleProperty::FontFamily);
    core::Optional<StringView> s = v.AsString();
    REQUIRE(s.HasValue());
    CHECK(s.Value() == StringView(u8"Chewy"));
}

TEST_CASE("sss: TypePlusClassRule")
{
    Fixture f(LoadSSS(u8"View { font-size: 12; } ButtonBase.primary { font-size: 24; }"));
    auto btn = core::MakeRef<Button>(core::DefaultAllocator(), StringView(u8"Test"));
    btn->AddClass(u8"primary");
    f.root->AddView(btn.Get());
    // Type+class (specificity 11) beats type-only (specificity 1)
    CHECK(btn->ResolveStyleFloat(StyleProperty::FontSize) == 24.0f);
}

TEST_CASE("sss: PaletteExtends")
{
    EnsureGlobals();
    StyleSheetLoader loader(DefaultAllocator());
    loader.SetPaletteVariable(u8"base-color", Rgb(100, 100, 100));
    core::RefPtr<StyleSheet> sheet = loader.Load(u8R"(
        @palette custom extends base { accent: #ff0000; }
        View { text-color: $base-color; accent-color: $accent; }
    )");
    Fixture f(Move(sheet));
    core::RefPtr<TestView> view = f.AddView();
    // base-color came from loader pre-set
    CHECK(view->ResolveStyleColor(StyleProperty::TextColor).r == doctest::Approx(100 / 255.0f));
    // accent came from the @palette block
    const Color accent = view->ResolveStyleColor(StyleProperty::AccentColor);
    CHECK(accent.r == 1.0f);
    CHECK(accent.g == 0);
}

TEST_CASE("sss: PaletteExtends_InheritsLoaderValues")
{
    EnsureGlobals();
    StyleSheetLoader loader(DefaultAllocator());
    loader.SetPaletteVariable(u8"base-bg", Rgb(40, 40, 50));
    loader.SetPaletteVariable(u8"base-text", Rgb(220, 220, 230));
    core::RefPtr<StyleSheet> sheet = loader.Load(u8R"(
        @palette custom extends base { accent: #ff8800; }
        View { text-color: $base-text; accent-color: $accent; background: color($base-bg); }
    )");
    Fixture f(Move(sheet));
    core::RefPtr<TestView> view = f.AddView();
    // base-text came from loader
    CHECK(view->ResolveStyleColor(StyleProperty::TextColor).r == doctest::Approx(220 / 255.0f));
    // accent came from the @palette block
    const Color accent = view->ResolveStyleColor(StyleProperty::AccentColor);
    CHECK(accent.r == 1.0f);
    CHECK(accent.g == doctest::Approx(0x88 / 255.0f));
    // base-bg came from loader, used in a drawable
    Drawable* bg = view->ResolveStyleDrawable(StyleProperty::Background);
    REQUIRE(bg != nullptr);
    ColorDrawable* cd = core::Cast<ColorDrawable>(bg);
    REQUIRE(cd != nullptr);
    CHECK(cd->Color.r == doctest::Approx(40 / 255.0f));
}

// === IResourceProvider tests ===

TEST_CASE("sss: Import_LoadsFromProvider")
{
    EnsureGlobals();
    MockResourceProvider provider;
    provider.AddText(u8"buttons.sss", u8"ButtonBase { padding: 6 12; }");
    StyleSheetLoader loader(DefaultAllocator());
    loader.ResourceProvider = &provider;
    core::RefPtr<StyleSheet> sheet = loader.Load(u8R"(
        View { font-size: 14; }
        @import "buttons.sss";
    )");
    // Should have 2 rules: View from main, ButtonBase from import
    CHECK(sheet->RuleCount() == 2);
    Fixture f(Move(sheet));
    auto btn = core::MakeRef<Button>(core::DefaultAllocator(), StringView(u8"Test"));
    f.root->AddView(btn.Get());
    const Thickness pad = btn->ResolveStyleThickness(StyleProperty::Padding);
    CHECK(pad.Top == 6);
    CHECK(pad.Left == 12);
}

TEST_CASE("sss: Import_NoProvider_Graceful")
{
    EnsureGlobals();
    // No resource provider -> @import should be silently skipped
    core::RefPtr<StyleSheet> sheet = LoadSSS(u8R"(
        @import "nonexistent.sss";
        View { font-size: 14; }
    )");
    CHECK(sheet->RuleCount() == 1);
}

TEST_CASE("sss: Icon_LoadsFromProvider")
{
    EnsureGlobals();
    MockResourceProvider provider;
    provider.AddText(u8"icons/check.svg", u8R"(<svg viewBox="0 0 16 16">
  <path d="M3 8 L6.5 11.5 L13 5" fill="none" stroke="white" stroke-width="2"/>
</svg>)");
    StyleSheetLoader loader(DefaultAllocator());
    loader.ResourceProvider = &provider;
    core::RefPtr<StyleSheet> sheet = loader.Load(u8R"(
        @icon checkmark "icons/check.svg";
        CheckBox::checkmark { background: svg(checkmark); }
    )");
    Fixture f(Move(sheet));
    auto cb = core::MakeRef<CheckBox>(core::DefaultAllocator(), StringView(u8"Test"));
    f.root->AddView(cb.Get());
    Drawable* icon =
        cb->ResolvePartDrawable(u8"checkmark", StyleProperty::Background, ControlState::Normal);
    REQUIRE(icon != nullptr);
    CHECK(core::Cast<SVGDrawable>(icon) != nullptr);
}

TEST_CASE("sss: Icon_PreRegisteredBeatsFile")
{
    EnsureGlobals();
    StyleSheetLoader loader(DefaultAllocator());
    // Pre-register inline SVG; no resource provider needed.
    loader.RegisterSvg(u8"checkmark", u8R"(<svg viewBox="0 0 16 16">
  <path d="M3 8 L6.5 11.5 L13 5" fill="none" stroke="white" stroke-width="2"/>
</svg>)");
    Fixture f(loader.Load(u8"CheckBox::checkmark { background: svg(checkmark); }"));
    auto cb = core::MakeRef<CheckBox>(core::DefaultAllocator(), StringView(u8"Test"));
    f.root->AddView(cb.Get());
    Drawable* icon =
        cb->ResolvePartDrawable(u8"checkmark", StyleProperty::Background, ControlState::Normal);
    REQUIRE(icon != nullptr);
    CHECK(core::Cast<SVGDrawable>(icon) != nullptr);
}

TEST_CASE("sss: Icon_NoProvider_SvgReturnsNull")
{
    EnsureGlobals();
    // No resource provider, no pre-registered SVG -> svg() returns null gracefully (no crash).
    Fixture f(LoadSSS(u8"CheckBox::checkmark { background: svg(missing); }"));
    auto cb = core::MakeRef<CheckBox>(core::DefaultAllocator(), StringView(u8"Test"));
    f.root->AddView(cb.Get());
    Drawable* icon =
        cb->ResolvePartDrawable(u8"checkmark", StyleProperty::Background, ControlState::Normal);
    (void)icon; // may be null or a fallback -- either way, no crash
    CHECK(true);
}

// === @image directive + image/nine-slice factories ===

TEST_CASE("sss: Image_LoadsFromProvider")
{
    EnsureGlobals();
    MockResourceProvider provider;
    provider.AddImage(u8"textures/bg.png");
    StyleSheetLoader loader(DefaultAllocator());
    loader.ResourceProvider = &provider;
    core::RefPtr<StyleSheet> sheet = loader.Load(u8R"(
        @image bg "textures/bg.png";
        View { background: image(bg); }
    )");
    Fixture f(Move(sheet));
    core::RefPtr<TestView> view = f.AddView();
    Drawable* bg = view->ResolveStyleDrawable(StyleProperty::Background);
    REQUIRE(bg != nullptr);
    CHECK(core::Cast<ImageDrawable>(bg) != nullptr);
}

TEST_CASE("sss: an image() takes radius= to round its corners, with a tint beside it")
{
    EnsureGlobals();
    MockResourceProvider provider;
    provider.AddImage(u8"textures/bg.png");
    StyleSheetLoader loader(DefaultAllocator());
    loader.ResourceProvider = &provider;
    core::RefPtr<StyleSheet> sheet = loader.Load(u8R"(
        @image bg "textures/bg.png";
        View { background: image(bg, radius=8 8 0 0, tint=#FF000080); }
    )");
    Fixture f(Move(sheet));
    core::RefPtr<TestView> view = f.AddView();
    ImageDrawable* bg = core::Cast<ImageDrawable>(view->ResolveStyleDrawable(StyleProperty::Background));
    REQUIRE(bg != nullptr);
    CHECK(bg->Radii == vg::CornerRadii(8.0f, 8.0f, 0.0f, 0.0f));
    CHECK(bg->Tint.r == doctest::Approx(1.0f));
    CHECK(bg->Tint.a == doctest::Approx(128.0f / 255.0f).epsilon(0.01));
}

TEST_CASE("sss: NineSlice_LoadsFromProvider")
{
    EnsureGlobals();
    MockResourceProvider provider;
    provider.AddImage(u8"textures/panel.png");
    StyleSheetLoader loader(DefaultAllocator());
    loader.ResourceProvider = &provider;
    core::RefPtr<StyleSheet> sheet = loader.Load(u8R"(
        @image panel "textures/panel.png";
        View { background: nine-slice(panel, 4 4 4 4); }
    )");
    Fixture f(Move(sheet));
    core::RefPtr<TestView> view = f.AddView();
    Drawable* bg = view->ResolveStyleDrawable(StyleProperty::Background);
    REQUIRE(bg != nullptr);
    CHECK(core::Cast<NineSliceDrawable>(bg) != nullptr);
}

TEST_CASE("sss: Image_PreRegistered")
{
    EnsureGlobals();
    const core::u8 pixels[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 0, 255};
    image::OwnedImageData imageData(2, 2, image::PixelFormat::RGBA8,
                                    core::Span<const core::u8>(pixels, 16));
    StyleSheetLoader loader(DefaultAllocator());
    loader.RegisterImage(u8"test-img", &imageData);
    core::RefPtr<StyleSheet> sheet = loader.Load(u8"View { background: image(test-img); }");
    Fixture f(Move(sheet));
    core::RefPtr<TestView> view = f.AddView();
    Drawable* bg = view->ResolveStyleDrawable(StyleProperty::Background);
    REQUIRE(bg != nullptr);
    CHECK(core::Cast<ImageDrawable>(bg) != nullptr);
}

TEST_CASE("sss: NineSlice_SingleSliceValue")
{
    EnsureGlobals();
    MockResourceProvider provider;
    provider.AddImage(u8"textures/btn.png");
    StyleSheetLoader loader(DefaultAllocator());
    loader.ResourceProvider = &provider;
    core::RefPtr<StyleSheet> sheet = loader.Load(u8R"(
        @image btn "textures/btn.png";
        View { background: nine-slice(btn, 8); }
    )");
    Fixture f(Move(sheet));
    core::RefPtr<TestView> view = f.AddView();
    Drawable* bg = view->ResolveStyleDrawable(StyleProperty::Background);
    REQUIRE(bg != nullptr);
    CHECK(core::Cast<NineSliceDrawable>(bg) != nullptr);
}

TEST_CASE("sss: Image_WithTint")
{
    EnsureGlobals();
    MockResourceProvider provider;
    provider.AddImage(u8"textures/icon.png");
    StyleSheetLoader loader(DefaultAllocator());
    loader.ResourceProvider = &provider;
    core::RefPtr<StyleSheet> sheet = loader.Load(u8R"(
        @image icon "textures/icon.png";
        View { background: image(icon, tint=#ff0000); }
    )");
    Fixture f(Move(sheet));
    core::RefPtr<TestView> view = f.AddView();
    Drawable* bg = view->ResolveStyleDrawable(StyleProperty::Background);
    REQUIRE(bg != nullptr);
    ImageDrawable* id = core::Cast<ImageDrawable>(bg);
    REQUIRE(id != nullptr);
    CHECK(id->Tint.r == 1.0f);
    CHECK(id->Tint.g == 0);
}

// === Theme capabilities ===

TEST_CASE("sss: svg builtin glyph resolves via ThemeIconSet WITHOUT any registration")
{
    // Cooked/runtime themes must not get a silent null here: without this, only
    // hosts that pre-registered names could resolve svg(close).
    Fixture f(LoadSSS(u8"View { background: svg(close); }"));
    core::RefPtr<TestView> view = f.AddView();
    Drawable* bg = view->ResolveStyleDrawable(StyleProperty::Background);
    REQUIRE(bg != nullptr);
    CHECK(core::Cast<SVGDrawable>(bg) != nullptr);
}

TEST_CASE("sss: svg builtin glyph shares the BAKED instance when ThemeIconSet is live")
{
    ThemeIconSet::Get().Initialize();
    Fixture f(LoadSSS(u8"View { background: svg(close); }"));
    core::RefPtr<TestView> view = f.AddView();
    Drawable* bg = view->ResolveStyleDrawable(StyleProperty::Background);
    REQUIRE(bg != nullptr);
    // The SHARED instance - not a fresh parse (identity proves the crispness path).
    RefPtr<Drawable> shared = ThemeIconSet::Acquire(ThemeIcon::Close);
    CHECK(bg == shared.Get());
    ThemeIconSet::Get().Shutdown(); // test hygiene
}

TEST_CASE("sss: rounded-rect accepts per-corner radius=a b c d")
{
    Fixture f(LoadSSS(u8"View { background: rounded-rect(#336699, radius=1 2 3 4); }"));
    core::RefPtr<TestView> view = f.AddView();
    RoundedRectDrawable* rrd =
        core::Cast<RoundedRectDrawable>(view->ResolveStyleDrawable(StyleProperty::Background));
    REQUIRE(rrd != nullptr);
    CHECK(rrd->Radii.topLeft == 1.0f);
    CHECK(rrd->Radii.topRight == 2.0f);
    CHECK(rrd->Radii.bottomRight == 3.0f);
    CHECK(rrd->Radii.bottomLeft == 4.0f);
}

TEST_CASE("sss: background-color stores Background as a raw COLOR, not a drawable")
{
    // Controls that resolve Background via ResolveStyleColor (ToastCard) need the Color kind;
    // plain `background:` always builds a drawable and would be ignored by that path.
    Fixture f(LoadSSS(u8"View { background-color: #6496c8; }"));
    core::RefPtr<TestView> view = f.AddView();
    CHECK(view->ResolveStyleDrawable(StyleProperty::Background) == nullptr);
    const Color got = view->ResolveStyleColor(StyleProperty::Background, Color::White);
    CHECK(got.r == doctest::Approx(0x64 / 255.0f));
    CHECK(got.g == doctest::Approx(0x96 / 255.0f));
    CHECK(got.b == doctest::Approx(0xc8 / 255.0f));
}

TEST_CASE("sss: state color functions match Palette derivations")
{
    // disabled()'s luminance desaturation is the one lighten/darken cannot express - the
    // sheet-declared color must equal the C++ themes' Palette math exactly (migration parity).
    Fixture f(LoadSSS(u8"View { text-color: disabled(#6496c8); }"));
    core::RefPtr<TestView> view = f.AddView();
    const Color expected =
        Palette::ComputeDisabled(Color{0x64 / 255.0f, 0x96 / 255.0f, 0xc8 / 255.0f, 1.0f});
    const Color got = view->ResolveStyleColor(StyleProperty::TextColor, Color::White);
    CHECK(got.r == doctest::Approx(expected.r));
    CHECK(got.g == doctest::Approx(expected.g));
    CHECK(got.b == doctest::Approx(expected.b));

    Fixture f2(LoadSSS(u8"View { text-color: hover(#404040); }"));
    core::RefPtr<TestView> view2 = f2.AddView();
    const Color expectedHover =
        Palette::ComputeHover(Color{0x40 / 255.0f, 0x40 / 255.0f, 0x40 / 255.0f, 1.0f});
    const Color gotHover = view2->ResolveStyleColor(StyleProperty::TextColor, Color::White);
    CHECK(gotHover.r == doctest::Approx(expectedHover.r));
}
