// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Style model v2, P1 (ui-layout-and-style-model.md section 3): the ordered cascade
// (specificity, source order, last wins per property), selector chains (descendant / child
// combinators, #id, structural pseudo-classes, the CSS state aliases), inheritance with the
// `inherit` / `initial` keywords, custom properties + var() with fallbacks, the palette as
// root variables, relative units (%, em, calc) in sheets and markup, and the computed-style
// cache's invalidation.

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
    /// A TestView whose control state the test sets directly.
    class StateView final : public TestView
    {
        RTTI_OBJECT(StateView, TestView)
    public:
        ControlState State = ControlState::Normal;
        StateView() = default;
        [[nodiscard]] ControlState GetControlState() const override { return State; }
    };
    RTTI_DEFINE_OBJECT(StateView, "rtti::ui::tests")

    void EnsureGlobals()
    {
        StyleSheetLoader::InitializeGlobals();
        UITypeRegistry::Register(u8"View", &View::StaticType());
        UITypeRegistry::Register(u8"RootView", &RootView::StaticType());
        UITypeRegistry::Register(u8"TestView", &TestView::StaticType());
        UITypeRegistry::Register(u8"TestGroup", &TestGroup::StaticType());
        UITypeRegistry::Register(u8"StateView", &StateView::StaticType());
    }

    core::RefPtr<StyleSheet> LoadSSS(StringView src)
    {
        EnsureGlobals();
        StyleSheetLoader loader(DefaultAllocator());
        return loader.Load(src);
    }

    // A context + root with a sheet; helpers build nested groups/views.
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

        [[nodiscard]] core::RefPtr<TestGroup> Group(ViewGroup* parent, StringView cls = {},
                                                    StringView name = {})
        {
            core::RefPtr<TestGroup> g = core::MakeRef<TestGroup>(core::DefaultAllocator());
            if (cls.Size() > 0)
            {
                g->AddClass(cls);
            }
            if (name.Size() > 0)
            {
                g->Name = String(name);
            }
            parent->AddView(g.Get());
            return g;
        }
        [[nodiscard]] core::RefPtr<TestView> Leaf(ViewGroup* parent, StringView cls = {},
                                                  StringView name = {})
        {
            core::RefPtr<TestView> v = core::MakeRef<TestView>(core::DefaultAllocator(), 50.0f, 30.0f);
            if (cls.Size() > 0)
            {
                v->AddClass(cls);
            }
            if (name.Size() > 0)
            {
                v->Name = String(name);
            }
            parent->AddView(v.Get());
            return v;
        }
    };

    // Channel readers: -1 when the property is UNSET (the default color is white, whose
    // channels would read as 1).
    const Color kUnset{-1.0f, -1.0f, -1.0f, 0.0f};
    [[nodiscard]] f32 Red(View& v) { return v.ResolveStyleColor(StyleProperty::TextColor, kUnset).r; }
    [[nodiscard]] f32 Green(View& v) { return v.ResolveStyleColor(StyleProperty::TextColor, kUnset).g; }
    [[nodiscard]] f32 Blue(View& v) { return v.ResolveStyleColor(StyleProperty::TextColor, kUnset).b; }
} // namespace

// === Cascade order ===

TEST_CASE("cascade: three competing rules resolve by specificity (type < class < id)")
{
    Fixture f(LoadSSS(u8"#pick { text-color: #0000ff; }\n"
                      u8"TestView { text-color: #ff0000; }\n"
                      u8".accent { text-color: #00ff00; }\n"));
    auto plain = f.Leaf(f.root.Get());
    auto classed = f.Leaf(f.root.Get(), u8"accent");
    auto named = f.Leaf(f.root.Get(), u8"accent", u8"pick");
    CHECK(Red(*plain) == doctest::Approx(1.0f));
    CHECK(Green(*classed) == doctest::Approx(1.0f));
    CHECK(Blue(*named) == doctest::Approx(1.0f)); // the id rule wins although declared first
}

TEST_CASE("cascade: equal specificity resolves by source order, last wins PER PROPERTY")
{
    Fixture f(LoadSSS(u8".a { text-color: #ff0000; font-size: 11; }\n"
                      u8".a { text-color: #00ff00; }\n"));
    auto v = f.Leaf(f.root.Get(), u8"a");
    CHECK(Green(*v) == doctest::Approx(1.0f));                              // later rule wins
    CHECK(v->ResolveStyleFloat(StyleProperty::FontSize) == doctest::Approx(11.0f)); // earlier keeps its own
}

TEST_CASE("cascade: a pseudo-class weighs like a class, so `.primary` and `:hover` tie on order")
{
    // `.primary` is declared AFTER `TestView:hover`: equal specificity (11 each) -> source order.
    Fixture f(LoadSSS(u8"TestView:hover { text-color: #ff0000; }\n"
                      u8"TestView.primary { text-color: #00ff00; }\n"));
    auto v = f.Leaf(f.root.Get(), u8"primary");
    CHECK(Green(*v) == doctest::Approx(1.0f));
}

// === Selector chains ===

TEST_CASE("cascade: descendant combinator matches at any depth, child only the direct parent")
{
    Fixture f(LoadSSS(u8".panel TestView { text-color: #ff0000; }\n"
                      u8".panel > TestView { font-size: 42; }\n"));
    auto panel = f.Group(f.root.Get(), u8"panel");
    auto inner = f.Group(panel.Get());
    auto direct = f.Leaf(panel.Get());
    auto deep = f.Leaf(inner.Get());
    auto outside = f.Leaf(f.root.Get());
    CHECK(Red(*direct) == doctest::Approx(1.0f));
    CHECK(Red(*deep) == doctest::Approx(1.0f));
    CHECK(Red(*outside) == doctest::Approx(-1.0f));
    CHECK(direct->ResolveStyleFloat(StyleProperty::FontSize) == doctest::Approx(42.0f));
    CHECK(deep->ResolveStyleFloat(StyleProperty::FontSize) == doctest::Approx(0.0f));
}

TEST_CASE("cascade: a chain of three with mixed combinators backtracks over ancestors")
{
    // `.a .b > TestView`: the .b must be the direct parent, the .a any ancestor above it.
    Fixture f(LoadSSS(u8".a .b > TestView { text-color: #ff0000; }\n"));
    auto a = f.Group(f.root.Get(), u8"a");
    auto mid = f.Group(a.Get());
    auto b = f.Group(mid.Get(), u8"b");
    auto hit = f.Leaf(b.Get());
    auto bDeeper = f.Group(b.Get());
    auto miss = f.Leaf(bDeeper.Get()); // .b is a grandparent, not the parent
    CHECK(Red(*hit) == doctest::Approx(1.0f));
    CHECK(Red(*miss) == doctest::Approx(-1.0f));
}

TEST_CASE("cascade: #id, :first-child, :last-child and :empty match structure")
{
    Fixture f(LoadSSS(u8"TestView:first-child { text-color: #ff0000; }\n"
                      u8"TestView:last-child { font-size: 7; }\n"
                      u8"TestGroup:empty { corner-radius: 3; }\n"
                      u8"#solo { border-width: 9; }\n"));
    auto group = f.Group(f.root.Get());
    auto first = f.Leaf(group.Get());
    auto middle = f.Leaf(group.Get(), {}, u8"solo");
    auto last = f.Leaf(group.Get());
    auto emptyGroup = f.Group(f.root.Get());
    CHECK(Red(*first) == doctest::Approx(1.0f));
    CHECK(Red(*middle) == doctest::Approx(-1.0f));
    CHECK(last->ResolveStyleFloat(StyleProperty::FontSize) == doctest::Approx(7.0f));
    CHECK(first->ResolveStyleFloat(StyleProperty::FontSize) == doctest::Approx(0.0f));
    CHECK(emptyGroup->ResolveStyleFloat(StyleProperty::CornerRadius) == doctest::Approx(3.0f));
    CHECK(group->ResolveStyleFloat(StyleProperty::CornerRadius) == doctest::Approx(0.0f));
    CHECK(middle->ResolveStyleFloat(StyleProperty::BorderWidth) == doctest::Approx(9.0f));
}

TEST_CASE("cascade: the CSS state aliases parse to the control states")
{
    auto sheet = LoadSSS(u8"TestView:active { font-size: 1; }\n"
                         u8"TestView:focus { font-size: 2; }\n"
                         u8"TestView:focus-visible:hover { font-size: 3; }\n");
    REQUIRE(sheet->RuleCount() == 3u);
    CHECK(sheet->GetRule(0).Selector.State.Value() == ControlState::Pressed);
    CHECK(sheet->GetRule(1).Selector.State.Value() == ControlState::Focused);
    CHECK(sheet->GetRule(2).Selector.State.Value() == (ControlState::Focused | ControlState::Hover));
    CHECK(sheet->GetRule(2).Selector.Specificity() == 21);
}

TEST_CASE("cascade: an unknown type name matches nothing and does not eat the declarations")
{
    Fixture f(LoadSSS(u8"NoSuchControl { text-color: #ff0000; }\n"
                      u8"TestView { font-size: 5; }\n"));
    auto v = f.Leaf(f.root.Get());
    CHECK(Red(*v) == doctest::Approx(-1.0f));
    CHECK(v->ResolveStyleFloat(StyleProperty::FontSize) == doctest::Approx(5.0f));
}

// === Inheritance ===

TEST_CASE("cascade: text properties inherit the parent's COMPUTED value through three levels")
{
    Fixture f(LoadSSS(u8".top { text-color: #ff0000; font-size: 13; word-wrap: true; }\n"));
    auto top = f.Group(f.root.Get(), u8"top");
    auto mid = f.Group(top.Get());
    auto leaf = f.Leaf(mid.Get());
    CHECK(Red(*leaf) == doctest::Approx(1.0f));
    CHECK(leaf->ResolveStyleFloat(StyleProperty::FontSize) == doctest::Approx(13.0f));
    CHECK(leaf->ResolveStyle(StyleProperty::WordWrap).AsBool().Value() == true);
    // A non-inheritable property does not walk up.
    CHECK(leaf->ResolveStyleFloat(StyleProperty::CornerRadius, -1.0f) == doctest::Approx(-1.0f));
}

// A label wraps by the `word-wrap` style as by its markup attribute: the style was parsed and
// inherited but the label read only its own property (the Shader Gallery's caption did not wrap).
TEST_CASE("cascade: a label wraps by the word-wrap style, its own setting first")
{
    Fixture f(LoadSSS(u8".wraps { word-wrap: true; }\n"));
    core::RefPtr<Label> plain = core::MakeRef<Label>(core::DefaultAllocator());
    f.root->AddView(plain.Get());
    CHECK_FALSE(plain->IsWordWrapped()); // nothing says so: off

    auto box = f.Group(f.root.Get(), u8"wraps");
    core::RefPtr<Label> styled = core::MakeRef<Label>(core::DefaultAllocator());
    box->AddView(styled.Get());
    CHECK(styled->IsWordWrapped()); // the container's style reaches it

    styled->WordWrap.SetValue(false); // its own setting wins over the style
    CHECK_FALSE(styled->IsWordWrapped());
    plain->WordWrap.SetValue(true);
    CHECK(plain->IsWordWrapped());
}

TEST_CASE("cascade: `inherit` takes the parent's value even for non-inheritable properties; `initial` unsets")
{
    Fixture f(LoadSSS(u8"TestGroup { corner-radius: 6; text-color: #00ff00; }\n"
                      u8"TestView { corner-radius: inherit; text-color: initial; }\n"));
    auto group = f.Group(f.root.Get());
    auto leaf = f.Leaf(group.Get());
    CHECK(leaf->ResolveStyleFloat(StyleProperty::CornerRadius) == doctest::Approx(6.0f));
    CHECK(leaf->ResolveStyle(StyleProperty::TextColor).IsNone()); // initial: not even inherited
}

// === Variables ===

TEST_CASE("cascade: var() reads a custom property from the cascade, with a fallback when unset")
{
    Fixture f(LoadSSS(u8"View { --accent: #ff0000; --ring-angle: 45; }\n"
                      u8"TestView { text-color: var(--accent); border-color: var(--missing, #0000ff); "
                      u8"font-size: var(--ring-angle); corner-radius: var(--nope); }\n"));
    auto v = f.Leaf(f.root.Get());
    CHECK(Red(*v) == doctest::Approx(1.0f));
    CHECK(v->ResolveStyleColor(StyleProperty::BorderColor).b == doctest::Approx(1.0f));
    CHECK(v->ResolveStyleFloat(StyleProperty::FontSize) == doctest::Approx(45.0f));
    CHECK(v->ResolveStyleFloat(StyleProperty::CornerRadius, -1.0f) == doctest::Approx(-1.0f)); // unset, no fallback
    CHECK(v->CustomProperty(u8"--ring-angle").AsFloat().Value() == doctest::Approx(45.0f));
    CHECK(v->CustomProperty(u8"--nope").IsNone());
}

TEST_CASE("cascade: custom properties inherit and a nearer declaration overrides")
{
    // Declared on the ROOT (a `View { --accent }` rule would re-declare it on every view,
    // shadowing any ancestor's value - the same as CSS `* { --x }`).
    Fixture f(LoadSSS(u8"RootView { --accent: #ff0000; }\n"
                      u8".dark { --accent: #0000ff; }\n"
                      u8"TestView { text-color: var(--accent); }\n"));
    auto light = f.Group(f.root.Get());
    auto dark = f.Group(f.root.Get(), u8"dark");
    auto inner = f.Group(dark.Get());
    auto a = f.Leaf(light.Get());
    auto b = f.Leaf(inner.Get());
    CHECK(Red(*a) == doctest::Approx(1.0f));
    CHECK(Blue(*b) == doctest::Approx(1.0f)); // the .dark ancestor's value wins for its subtree
}

TEST_CASE("cascade: the loader's palette is a root rule of variables")
{
    EnsureGlobals();
    StyleSheetLoader loader(DefaultAllocator());
    loader.SetPaletteVariable(u8"primary", Color{0.0f, 1.0f, 0.0f, 1.0f});
    Fixture f(loader.Load(u8"@palette p { accent: #0000ff; }\n"
                          u8"TestView { text-color: var(--primary); border-color: var(--accent); }\n"));
    auto v = f.Leaf(f.root.Get());
    CHECK(Green(*v) == doctest::Approx(1.0f));
    CHECK(v->ResolveStyleColor(StyleProperty::BorderColor).b == doctest::Approx(1.0f));
    CHECK(v->CustomProperty(u8"--primary").AsColor().Value().g == doctest::Approx(1.0f));
}

TEST_CASE("cascade: a var() chain resolves through nested references and nested fallbacks")
{
    Fixture f(LoadSSS(u8"View { --base: #ff0000; --alias: var(--base); }\n"
                      u8"TestView { text-color: var(--alias); border-color: var(--x, var(--y, #00ff00)); }\n"));
    auto v = f.Leaf(f.root.Get());
    CHECK(Red(*v) == doctest::Approx(1.0f));
    CHECK(v->ResolveStyleColor(StyleProperty::BorderColor).g == doctest::Approx(1.0f));
}

// === Units ===

TEST_CASE("cascade: percent and em resolve against the reference box and the font size")
{
    Fixture f(LoadSSS(u8"TestGroup { font-size: 20; }\n"
                      u8"TestView { corner-radius: 50%; border-width: 2em; font-size: 1.5em; "
                      u8"spacing: calc(100% - 20dp); padding: 4; }\n"));
    auto group = f.Group(f.root.Get());
    auto v = f.Leaf(group.Get());
    CHECK(v->ResolveStyleLength(StyleProperty::CornerRadius, 80.0f) == doctest::Approx(40.0f));
    // em of the view's OWN font size (30 = 1.5em of the parent's 20).
    CHECK(v->ResolveStyleLength(StyleProperty::FontSize, 0.0f) == doctest::Approx(30.0f));
    CHECK(v->ResolveStyleLength(StyleProperty::BorderWidth, 0.0f) == doctest::Approx(60.0f));
    CHECK(v->ResolveStyleLength(StyleProperty::Spacing, 300.0f) == doctest::Approx(280.0f));
    // The plain Float accessor resolves a Length too (P2 consumer migration): em against the
    // font size; percent has no reference box on that path and reads 0.
    CHECK(v->ResolveStyleFloat(StyleProperty::CornerRadius, -1.0f) == doctest::Approx(0.0f));
    CHECK(v->ResolveStyleFloat(StyleProperty::BorderWidth, -1.0f) == doctest::Approx(60.0f));
    CHECK(v->ResolveStyleThickness(StyleProperty::Padding).Left == doctest::Approx(4.0f));
}

TEST_CASE("cascade: markup width=\"50%\" and height=\"2em\" measure against the containing box")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get(), 400, 300);
    EnsureGlobals();
    ctx.SetStyleSheet(LoadSSS(u8"View { font-size: 20; }\n"));

    auto frame = core::MakeRef<FrameLayout>(core::DefaultAllocator());
    LayoutStyle fill;
    fill.Width = SizeSpec::Match();
    fill.Height = SizeSpec::Match();
    root->AddView(frame.Get(), fill);

    auto half = core::MakeRef<TestView>(core::DefaultAllocator(), 10.0f, 10.0f);
    LayoutStyle ls;
    ls.Width = MarkupRegistry::ParseSizeSpec(u8"50%");
    ls.Height = MarkupRegistry::ParseSizeSpec(u8"2em");
    frame->AddView(half.Get(), ls);

    auto calc = core::MakeRef<TestView>(core::DefaultAllocator(), 10.0f, 10.0f);
    LayoutStyle lc;
    lc.Width = MarkupRegistry::ParseSizeSpec(u8"calc(100% - 40)");
    frame->AddView(calc.Get(), lc);

    LayoutPass(ctx, root.Get());
    CHECK(half->Width() == doctest::Approx(200.0f));
    CHECK(half->Height() == doctest::Approx(40.0f));
    CHECK(calc->Width() == doctest::Approx(360.0f));
}

TEST_CASE("unit: calc sums components; percent and em need the full resolve")
{
    const Unit sum = Unit::Percent(100.0f) - Unit::Dp(20.0f) + Unit::Em(1.0f);
    CHECK(sum.IsRelative());
    CHECK(sum.Resolve(1.0f) == doctest::Approx(-20.0f));           // absolute part only
    CHECK(sum.Resolve(1.0f, 300.0f, 16.0f) == doctest::Approx(296.0f));
    CHECK(!Unit::Dp(5.0f).IsRelative());
    CHECK(Unit::Px(50.0f).Resolve(2.0f, 100.0f, 16.0f) == doctest::Approx(25.0f));

    CHECK(StyleValueParser::ParseLengthText(u8"50%").Value() == Unit::Percent(50.0f));
    CHECK(StyleValueParser::ParseLengthText(u8"2em").Value() == Unit::Em(2.0f));
    CHECK(StyleValueParser::ParseLengthText(u8"12px").Value() == Unit::Px(12.0f));
    CHECK(StyleValueParser::ParseLengthText(u8"calc(100% - 20dp)").Value() ==
          Unit::Percent(100.0f) - Unit::Dp(20.0f));
    CHECK(!StyleValueParser::ParseLengthText(u8"wide").HasValue());
    CHECK(!StyleValueParser::ParseLengthText(u8"12furlongs").HasValue());
    CHECK(SizeSpec::Fixed(Unit::Percent(25.0f)).ResolveFixed(1.0f, 200.0f, 16.0f) == doctest::Approx(50.0f));
}

// === Cache ===

TEST_CASE("cascade: the computed-style cache follows class edits, sheet edits and reorders")
{
    Fixture f(LoadSSS(u8"TestView { text-color: #ff0000; }\n"
                      u8".on { text-color: #00ff00; }\n"
                      u8"TestView:first-child { font-size: 9; }\n"));
    auto group = f.Group(f.root.Get());
    auto a = f.Leaf(group.Get());
    auto b = f.Leaf(group.Get());
    CHECK(Red(*a) == doctest::Approx(1.0f));
    a->AddClass(u8"on");
    CHECK(Green(*a) == doctest::Approx(1.0f));
    a->RemoveClass(u8"on");
    CHECK(Red(*a) == doctest::Approx(1.0f));

    // A rule added to the context sheet AFTER the first resolve is seen.
    StyleSheet* sheet = f.ctx.GetStyleSheet();
    sheet->ForType(&TestView::StaticType()).Set(StyleProperty::TextColor, Color{0.0f, 0.0f, 1.0f, 1.0f});
    CHECK(Blue(*a) == doctest::Approx(1.0f));

    // Reordering the children moves :first-child.
    CHECK(a->ResolveStyleFloat(StyleProperty::FontSize) == doctest::Approx(9.0f));
    CHECK(b->ResolveStyleFloat(StyleProperty::FontSize) == doctest::Approx(0.0f));
    group->MoveView(b.Get(), 0);
    CHECK(b->ResolveStyleFloat(StyleProperty::FontSize) == doctest::Approx(9.0f));
    CHECK(a->ResolveStyleFloat(StyleProperty::FontSize) == doctest::Approx(0.0f));

    // A local sheet set later wins over the context sheet.
    auto local = core::MakeRef<StyleSheet>(core::DefaultAllocator());
    local->ForType(&TestView::StaticType()).Set(StyleProperty::TextColor, Color{0.5f, 0.5f, 0.5f, 1.0f});
    group->SetLocalStyleSheet(local);
    CHECK(Red(*a) == doctest::Approx(0.5f));

    // Inline style beats both.
    SSSParser::ApplyInlineStyle(a.Get(), u8"text-color: #00ff00;");
    CHECK(Green(*a) == doctest::Approx(1.0f));
}

TEST_CASE("cascade: two contexts with different sheets stay independent (editor + embedded game)")
{
    EnsureGlobals();
    UIContext editor{DefaultAllocator()};
    UIContext game{DefaultAllocator()};
    auto editorRoot = core::MakeRef<RootView>(core::DefaultAllocator());
    auto gameRoot = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(editor, editorRoot.Get());
    Init(game, gameRoot.Get());
    editor.SetStyleSheet(LoadSSS(u8"RootView { --accent: #ff0000; } TestView { text-color: var(--accent); }"));
    game.SetStyleSheet(LoadSSS(u8"RootView { --accent: #0000ff; } TestView { text-color: var(--accent); }"));

    auto e = core::MakeRef<TestView>(core::DefaultAllocator(), 10.0f, 10.0f);
    auto g = core::MakeRef<TestView>(core::DefaultAllocator(), 10.0f, 10.0f);
    auto g2 = core::MakeRef<TestView>(core::DefaultAllocator(), 10.0f, 10.0f);
    editorRoot->AddView(e.Get());
    gameRoot->AddView(g.Get());
    gameRoot->AddView(g2.Get());
    CHECK(Red(*e) == doctest::Approx(1.0f));
    CHECK(Blue(*g) == doctest::Approx(1.0f));

    // Editing the game's sheet, or one game view's inline style, changes nothing in the editor
    // and nothing on the sibling. The version keys are per sheet, so the editor's cache is
    // not even rebuilt (its chain holds no game sheet).
    const u32 editorSheetVersion = editor.GetStyleSheet()->Version();
    game.GetStyleSheet()->ForType(&TestView::StaticType()).Set(StyleProperty::TextColor,
                                                              Color{0.0f, 1.0f, 0.0f, 1.0f});
    SSSParser::ApplyInlineStyle(g.Get(), u8"text-color: #ffffff;");
    CHECK(Green(*g2) == doctest::Approx(1.0f));
    CHECK(Red(*g) == doctest::Approx(1.0f));
    CHECK(Green(*g) == doctest::Approx(1.0f)); // inline white
    CHECK(Red(*e) == doctest::Approx(1.0f));
    CHECK(Green(*e) == doctest::Approx(0.0f));
    CHECK(editor.GetStyleSheet()->Version() == editorSheetVersion);
}

TEST_CASE("cascade: a compound state selector needs EVERY flag, not any one of them")
{
    // `:hover:checked` used to match a view that was merely hovered (the match tested ANY bit
    // of the compound); the selector's own doc said all flags must be present. Found by the
    // Beef port reading the code against the comment.
    Fixture f(LoadSSS(u8"StateView { text-color: #000000; }\n"
                      u8"StateView:hover:checked { text-color: #ff0000; }\n"));
    auto v = core::MakeRef<StateView>(core::DefaultAllocator());
    f.root->AddView(v.Get());
    v->State = ControlState::Hover;
    CHECK(Red(*v) == doctest::Approx(0.0f));
    v->State = ControlState::Checked;
    CHECK(Red(*v) == doctest::Approx(0.0f));
    v->State = ControlState::Hover | ControlState::Checked;
    CHECK(Red(*v) == doctest::Approx(1.0f));
    v->State = ControlState::Hover | ControlState::Checked | ControlState::Focused; // extra bits are fine
    CHECK(Red(*v) == doctest::Approx(1.0f));
}
