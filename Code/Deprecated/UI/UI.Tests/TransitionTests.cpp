// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// `transition` parsing, the per-view transitions that overlay ResolveStyle (started on a
// computed-style change from a class toggle or a control-state change, and retargeting mid-flight
// from the current value), the state cross-fade through Drawable::Draw, layout or visual damage
// per property kind, motion as theme data, the sheet-swap and detach edges, and @keyframes
// animations.

#include <doctest/doctest.h>

#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.vg;
import foundation.ui;

#include "TestHelpers.h"

using namespace foundation::ui;
using namespace foundation::ui::tests;
using namespace foundation::core;
namespace core = foundation::core;
namespace vg = foundation::vg;

namespace
{
    /// A TestView whose control state is set by the test, drawing its styled background.
    class StatefulView final : public TestView
    {
        RTTI_OBJECT(StatefulView, TestView)
    public:
        ControlState State = ControlState::Normal;
        StatefulView() = default;
        StatefulView(f32 w, f32 h) : TestView(w, h) {}
        [[nodiscard]] ControlState GetControlState() const override { return State; }
        void OnDraw(UIDrawContext& ctx) override
        {
            if (Drawable* bg = ResolveStyleDrawable(StyleProperty::Background))
            {
                bg->Draw(ctx, Rectangle{0, 0, Width(), Height()}, GetControlState());
            }
        }
    };
    RTTI_DEFINE_OBJECT(StatefulView, "rtti::ui::tests")

    /// Paints a rect whose color says which state it was drawn with (red normal, green hover).
    class StateColorDrawable final : public Drawable
    {
        RTTI_OBJECT(StateColorDrawable, Drawable)
    public:
        void Draw(UIDrawContext& ctx, const Rectangle& bounds) override
        {
            ctx.VG().FillRect(bounds, Color{1, 0, 0, 1});
        }

    protected:
        void DrawState(UIDrawContext& ctx, const Rectangle& bounds, ControlState state) override
        {
            const bool hover = (state & ControlState::Hover) == ControlState::Hover;
            ctx.VG().FillRect(bounds, hover ? Color{0, 1, 0, 1} : Color{1, 0, 0, 1});
        }
    };
    RTTI_DEFINE_OBJECT(StateColorDrawable, "rtti::ui::tests")

    void EnsureGlobals()
    {
        StyleSheetLoader::InitializeGlobals();
        UITypeRegistry::Register(u8"View", &View::StaticType());
        UITypeRegistry::Register(u8"RootView", &RootView::StaticType());
        UITypeRegistry::Register(u8"TestView", &TestView::StaticType());
        UITypeRegistry::Register(u8"TestGroup", &TestGroup::StaticType());
        UITypeRegistry::Register(u8"StatefulView", &StatefulView::StaticType());
    }

    core::RefPtr<StyleSheet> LoadSSS(StringView src)
    {
        EnsureGlobals();
        StyleSheetLoader loader(DefaultAllocator());
        return loader.Load(src);
    }

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
        core::RefPtr<StatefulView> Stateful()
        {
            auto v = core::MakeRef<StatefulView>(core::DefaultAllocator(), 50.0f, 30.0f);
            root->AddView(v.Get());
            return v;
        }
        core::RefPtr<TestView> Leaf()
        {
            auto v = core::MakeRef<TestView>(core::DefaultAllocator(), 50.0f, 30.0f);
            root->AddView(v.Get());
            return v;
        }
    };

    f32 Red(View& v) { return v.ResolveStyleColor(StyleProperty::TextColor, Color{-1, -1, -1, -1}).r; }
}

// === parsing ===

TEST_CASE("transition: the list parses properties, all, times in ms/s, easings, delays, none")
{
    core::RefPtr<StyleSheet> sheet = LoadSSS(
        u8"TestView { transition: text-color 200ms ease-in 50ms, opacity 1s, all 120 ease-out; }\n"
        u8".none { transition: none; }\n"
        u8".unknown { transition: not-a-property 100ms, font-size 0.5s; }\n");
    REQUIRE(sheet);
    REQUIRE(sheet->RuleCount() == 3);
    const TransitionList* list = sheet->GetRule(0).GetValue(StyleProperty::Transition).Value().AsTransitions();
    REQUIRE(list != nullptr);
    REQUIRE(list->Specs.Size() == 3);
    CHECK(list->Specs[0].Property == StyleProperty::TextColor);
    CHECK(list->Specs[0].Duration == doctest::Approx(0.2f));
    CHECK(list->Specs[0].Delay == doctest::Approx(0.05f));
    CHECK(list->Specs[0].Easing == TimingFunction::EaseIn());
    CHECK(list->Specs[1].Property == StyleProperty::Opacity);
    CHECK(list->Specs[1].Duration == doctest::Approx(1.0f));
    CHECK(list->Specs[1].Easing == TimingFunction::Ease());
    CHECK(list->Specs[2].Property == StyleProperty::COUNT); // all
    CHECK(list->Specs[2].Duration == doctest::Approx(0.12f)); // a bare number is milliseconds
    CHECK(list->Specs[2].Easing == TimingFunction::EaseOut());
    // Later entries govern: `all` came last, so it covers text-color too.
    CHECK(list->Find(StyleProperty::TextColor) == &list->Specs[2]);
    CHECK(list->Find(StyleProperty::BorderColor) == &list->Specs[2]);

    const TransitionList* none = sheet->GetRule(1).GetValue(StyleProperty::Transition).Value().AsTransitions();
    REQUIRE(none != nullptr);
    CHECK(none->Specs.IsEmpty());
    CHECK(none->Find(StyleProperty::TextColor) == nullptr);

    const TransitionList* partial = sheet->GetRule(2).GetValue(StyleProperty::Transition).Value().AsTransitions();
    REQUIRE(partial != nullptr);
    REQUIRE(partial->Specs.Size() == 1); // the unknown entry was dropped, the list survived
    CHECK(partial->Specs[0].Property == StyleProperty::FontSize);
    CHECK(partial->Specs[0].Duration == doctest::Approx(0.5f));
}

TEST_CASE("transition: shadow lists interpolate shadow by shadow, the shorter padded with clear ones")
{
    BoxShadow one;
    one.Blur = 4.0f;
    one.Color = Color{0, 0, 0, 1};
    BoxShadow two[2] = {one, one};
    two[1].OffsetY = 10.0f;
    two[1].Color = Color{1, 0, 0, 1};
    const StyleValue mid = LerpStyleValue(StyleValue::ShadowVal(one),
                                          StyleValue::ShadowsVal(Span<const BoxShadow>(two, 2)), 0.5f);
    const Span<const BoxShadow> shadows = mid.AsShadows();
    REQUIRE(shadows.Size() == 2u);
    CHECK(shadows[0].Blur == doctest::Approx(4.0f));
    // The added shadow fades in from a clear one of its own colour.
    CHECK(shadows[1].OffsetY == doctest::Approx(5.0f));
    CHECK(shadows[1].Color.r == doctest::Approx(1.0f));
    CHECK(shadows[1].Color.a == doctest::Approx(0.5f));
    CHECK_FALSE(StyleValueEquivalent(StyleValue::ShadowVal(one),
                                     StyleValue::ShadowsVal(Span<const BoxShadow>(two, 2))));
}

TEST_CASE("transition: LerpStyleValue interpolates colors, numbers, lengths, thicknesses, shadows")
{
    const StyleValue c = LerpStyleValue(StyleValue::ColorVal(Color{0, 0, 0, 1}),
                                        StyleValue::ColorVal(Color{1, 0.5f, 0, 0}), 0.5f);
    CHECK(c.AsColor().Value().r == doctest::Approx(0.5f));
    CHECK(c.AsColor().Value().g == doctest::Approx(0.25f));
    CHECK(c.AsColor().Value().a == doctest::Approx(0.5f));
    CHECK(LerpStyleValue(StyleValue::FloatVal(10), StyleValue::FloatVal(20), 0.25f).AsFloat().Value() ==
          doctest::Approx(12.5f));
    // A Float mixed with a Length mixes as a length (the float is dp).
    const StyleValue mixed = LerpStyleValue(StyleValue::FloatVal(10), StyleValue::LengthVal(Unit::Em(2)), 0.5f);
    REQUIRE(mixed.AsLength().HasValue());
    CHECK(mixed.AsLength().Value().dp == doctest::Approx(5));
    CHECK(mixed.AsLength().Value().em == doctest::Approx(1));
    const StyleValue th = LerpStyleValue(StyleValue::ThicknessVal(Thickness{0, 0, 0, 0}),
                                         StyleValue::ThicknessVal(Thickness{4, 8, 12, 16}), 0.5f);
    CHECK(th.AsThickness().Value().Right == doctest::Approx(6));
    BoxShadow a;
    a.Blur = 0;
    BoxShadow b;
    b.Blur = 8;
    b.OffsetY = 4;
    b.Inset = true;
    const StyleValue sh = LerpStyleValue(StyleValue::ShadowVal(a), StyleValue::ShadowVal(b), 0.25f);
    CHECK(sh.AsShadow().Value().Blur == doctest::Approx(2));
    CHECK(sh.AsShadow().Value().OffsetY == doctest::Approx(1));
    CHECK_FALSE(sh.AsShadow().Value().Inset); // discrete: flips at the midpoint
    // Endpoints and discrete kinds.
    CHECK(LerpStyleValue(StyleValue::FloatVal(1), StyleValue::FloatVal(2), 0.0f).AsFloat().Value() == 1);
    CHECK(LerpStyleValue(StyleValue::FloatVal(1), StyleValue::FloatVal(2), 1.0f).AsFloat().Value() == 2);
    CHECK(LerpStyleValue(StyleValue::StringRef(u8"a"), StyleValue::StringRef(u8"b"), 0.4f).AsString().Value() ==
          StringView(u8"a"));
    CHECK(LerpStyleValue(StyleValue::StringRef(u8"a"), StyleValue::StringRef(u8"b"), 0.6f).AsString().Value() ==
          StringView(u8"b"));
}

// === class toggles ===

TEST_CASE("transition: a class toggle animates only the listed properties")
{
    Fixture f(LoadSSS(u8"TestView { transition: text-color 100ms linear; text-color: #000000; font-size: 10; }\n"
                      u8".big { text-color: #ffffff; font-size: 20; }\n"));
    auto v = f.Leaf();
    CHECK(Red(*v) == doctest::Approx(0.0f)); // builds the cache
    v->AddClass(u8"big");
    // The first read after the change starts the transition from the old value ...
    CHECK(Red(*v) == doctest::Approx(0.0f));
    CHECK(v->ResolveStyleFloat(StyleProperty::FontSize) == doctest::Approx(20)); // unlisted: instant
    CHECK(v->ActiveTransitionCount() == 1);
    CHECK(f.ctx.TransitioningViewCount() == 1);
    // ... and the frame clock moves it.
    f.ctx.BeginFrame(0.05f);
    CHECK(Red(*v) == doctest::Approx(0.5f));
    f.ctx.BeginFrame(0.03f);
    CHECK(Red(*v) == doctest::Approx(0.8f));
    f.ctx.BeginFrame(0.05f); // past the end: the entry leaves, the cascade value stands
    CHECK(Red(*v) == doctest::Approx(1.0f));
    CHECK(v->ActiveTransitionCount() == 0);
    CHECK_FALSE(v->IsTransitioning());
    CHECK(f.ctx.TransitioningViewCount() == 0);
}

TEST_CASE("transition: duration 0 is instant and a generation bump without a change starts nothing")
{
    Fixture f(LoadSSS(u8"TestView { transition: text-color 0ms; text-color: #000000; }\n"
                      u8".big { text-color: #ffffff; }\n"));
    auto v = f.Leaf();
    CHECK(Red(*v) == doctest::Approx(0.0f));
    v->AddClass(u8"big");
    CHECK(Red(*v) == doctest::Approx(1.0f));
    CHECK(v->ActiveTransitionCount() == 0);
    // An unrelated tree mutation bumps the style generation: same values, no transition.
    auto other = f.Leaf();
    (void)other;
    CHECK(Red(*v) == doctest::Approx(1.0f));
    CHECK(v->ActiveTransitionCount() == 0);
}

// === state changes ===

TEST_CASE("transition: hover in then out mid-flight retargets from the current value (no snap)")
{
    Fixture f(LoadSSS(u8"StatefulView { transition: text-color 100ms linear; text-color: #000000; }\n"
                      u8"StatefulView:hover { text-color: #ffffff; }\n"));
    auto v = f.Stateful();
    CHECK(Red(*v) == doctest::Approx(0.0f));
    v->State = ControlState::Hover;
    CHECK(Red(*v) == doctest::Approx(0.0f)); // starts
    f.ctx.BeginFrame(0.05f);
    CHECK(Red(*v) == doctest::Approx(0.5f));
    // Hover out at the halfway point: the new transition runs from 0.5 to 0 over a full 100ms.
    v->State = ControlState::Normal;
    CHECK(Red(*v) == doctest::Approx(0.5f));
    CHECK(v->ActiveTransitionCount() == 1); // retargeted, not stacked
    f.ctx.BeginFrame(0.025f);
    CHECK(Red(*v) == doctest::Approx(0.375f));
    f.ctx.BeginFrame(0.075f);
    CHECK(Red(*v) == doctest::Approx(0.0f));
    CHECK(v->ActiveTransitionCount() == 0);
}

TEST_CASE("transition: easing and delay shape the curve")
{
    Fixture f(LoadSSS(u8"StatefulView { transition: text-color 100ms ease-in 50ms; text-color: #000000; }\n"
                      u8"StatefulView:hover { text-color: #ffffff; }\n"));
    auto v = f.Stateful();
    CHECK(Red(*v) == doctest::Approx(0.0f));
    v->State = ControlState::Hover;
    CHECK(Red(*v) == doctest::Approx(0.0f));
    f.ctx.BeginFrame(0.05f); // inside the delay
    CHECK(Red(*v) == doctest::Approx(0.0f));
    f.ctx.BeginFrame(0.05f); // at t = 0.5, CSS's ease-in, cubic-bezier(0.42, 0, 1, 1), is 0.3154
    CHECK(Red(*v) == doctest::Approx(0.3154f).epsilon(0.002));
}

TEST_CASE("transition: a filter interpolates its blur and colour factors")
{
    StyleFilter to;
    to.Blur = 4.0f;
    to.Saturation = 0.0f;
    const StyleFilter mid =
        LerpStyleValue(StyleValue::FilterVal(StyleFilter{}), StyleValue::FilterVal(to), 0.5f).AsFilter().Value();
    CHECK(mid.Blur == doctest::Approx(2.0f));
    CHECK(mid.Saturation == doctest::Approx(0.5f));
    CHECK(mid.Brightness == doctest::Approx(1.0f));
}

TEST_CASE("transition: a transform interpolates function by function, or through its matrix")
{
    const auto parse = [](StringView text) -> StyleTransform
    {
        core::RefPtr<StyleSheet> sheet = LoadSSS(Format(u8"TestView {{ transform: {}; }}", text).AsView());
        return sheet->GetRule(0).GetValue(StyleProperty::Transform).Value().AsTransform().Value();
    };
    const auto lerp = [](const StyleTransform& a, const StyleTransform& b, f32 t)
    { return LerpStyleValue(StyleValue::TransformVal(a), StyleValue::TransformVal(b), t).AsTransform().Value(); };

    // The same functions: each one's values.
    StyleTransform mid = lerp(parse(u8"translateY(0) rotate(0deg) scale(1)"),
                              parse(u8"translateY(-8) rotate(90deg) scale(1.5)"), 0.5f);
    REQUIRE(mid.Count == 3u);
    CHECK(mid.Functions[0].Y.dp == doctest::Approx(-4.0f));
    CHECK(mid.Functions[1].A == doctest::Approx(45.0f));
    CHECK(mid.Functions[2].A == doctest::Approx(1.25f));
    // `none` against a list interpolates from the list's identity functions, so it turns from 0.
    mid = lerp(parse(u8"none"), parse(u8"rotate(90deg)"), 0.5f);
    REQUIRE(mid.Count == 1u);
    CHECK(mid.Functions[0].A == doctest::Approx(45.0f));
    // Different functions: through the decomposed matrices, turning the short way.
    mid = lerp(parse(u8"rotate(170deg)"), parse(u8"scale(2) rotate(-170deg)"), 0.5f);
    REQUIRE(mid.Count == 1u);
    CHECK(mid.Functions[0].Kind == TransformFunctionKind::Matrix);
    const Transform2D m = mid.Compose(Float2{0.0f, 0.0f}, 1.0f);
    const f32 angle = Atan2(m.b, m.a) * kRadToDeg;
    CHECK(Abs(Abs(angle) - 180.0f) < 0.5f);                       // through 180, not through 0
    CHECK(Sqrt(m.a * m.a + m.b * m.b) == doctest::Approx(1.5f)); // half way to twice the size
    CHECK(IsAnimatableStyleProperty(StyleProperty::Transform));
    CHECK(IsVisualOnlyStyleProperty(StyleProperty::Transform)); // a lift never relays the page out
    CHECK(IsAnimatableStyleProperty(StyleProperty::TransformOrigin));
}

TEST_CASE("transition: timing functions are CSS's curves, cubic-bezier and steps")
{
    // The named easings at their midpoints, as a browser computes them.
    CHECK(TimingFunction::Linear().Evaluate(0.5f) == doctest::Approx(0.5f));
    CHECK(TimingFunction::Ease().Evaluate(0.5f) == doctest::Approx(0.8024f).epsilon(0.002));
    CHECK(TimingFunction::EaseIn().Evaluate(0.5f) == doctest::Approx(0.3154f).epsilon(0.002));
    CHECK(TimingFunction::EaseOut().Evaluate(0.5f) == doctest::Approx(1.0f - 0.3154f).epsilon(0.002));
    CHECK(TimingFunction::EaseInOut().Evaluate(0.5f) == doctest::Approx(0.5f));
    CHECK(TimingFunction::Ease().Evaluate(0.0f) == doctest::Approx(0.0f));
    CHECK(TimingFunction::Ease().Evaluate(1.0f) == doctest::Approx(1.0f));
    // A curve may overshoot (a back-out bounce): y past 1 between the ends.
    const TimingFunction back = TimingFunction::CubicBezier(0.34f, 1.56f, 0.64f, 1.0f);
    CHECK(back.Evaluate(0.6f) > 1.0f);
    // Steps hold each level, jumping at a step's end, or its start.
    CHECK(TimingFunction::StepsOf(4).Evaluate(0.3f) == doctest::Approx(0.25f));
    CHECK(TimingFunction::StepsOf(4, true).Evaluate(0.3f) == doctest::Approx(0.5f));
    CHECK(TimingFunction::StepsOf(4).Evaluate(1.0f) == doctest::Approx(1.0f));

    core::RefPtr<StyleSheet> sheet = LoadSSS(
        u8"TestView { transition: opacity 200ms cubic-bezier(0.34, 1.56, 0.64, 1), text-color 1s steps(5, start), "
        u8"background 1s step-end, border-color 1s cubic-bezier(1.5, 0, -1, 1); }");
    REQUIRE(sheet);
    const TransitionList* list = sheet->GetRule(0).GetValue(StyleProperty::Transition).Value().AsTransitions();
    REQUIRE(list != nullptr);
    REQUIRE(list->Specs.Size() == 4u);
    CHECK(list->Specs[0].Easing == back);
    CHECK(list->Specs[0].Duration == doctest::Approx(0.2f));
    CHECK(list->Specs[1].Easing == TimingFunction::StepsOf(5, true));
    CHECK(list->Specs[2].Easing == TimingFunction::StepsOf(1, false));
    // CSS keeps a curve's x within 0 to 1.
    CHECK(list->Specs[3].Easing.X1 == doctest::Approx(1.0f));
    CHECK(list->Specs[3].Easing.X2 == doctest::Approx(0.0f));
}

TEST_CASE("transition: a control-state change cross-fades a state-aware background")
{
    Fixture f(LoadSSS(u8"StatefulView { transition: all 100ms linear; }\n"));
    auto v = f.Stateful();
    core::RefPtr<StateColorDrawable> drawable = core::MakeRef<StateColorDrawable>(core::DefaultAllocator());
    v->GetOrCreateInlineSheet().GetOrCreateInlineElementRule().Set(StyleProperty::Background,
                                                                  core::RefPtr<Drawable>(drawable.Get()));
    LayoutPass(f.ctx, f.root.Get());
    {
        vg::VGContext vgContext;
        f.ctx.DrawRootView(f.root.Get(), vgContext); // plain: one red quad
        REQUIRE(vgContext.GetBatch().vertices.Size() == 4);
        CHECK(vgContext.GetBatch().vertices[0].color.r == doctest::Approx(1.0f));
    }
    v->State = ControlState::Hover;
    {
        vg::VGContext vgContext;
        f.ctx.DrawRootView(f.root.Get(), vgContext); // the change is seen here: blend starts at t = 0
        REQUIRE(vgContext.GetBatch().vertices.Size() == 8);
    }
    CHECK(v->IsTransitioning());
    f.ctx.BeginFrame(0.05f);
    {
        vg::VGContext vgContext;
        f.ctx.DrawRootView(f.root.Get(), vgContext);
        const vg::VGBatch& batch = vgContext.GetBatch();
        REQUIRE(batch.vertices.Size() == 8);
        // Old state (red) at full strength under the new state (green) at t = 0.5.
        CHECK(batch.vertices[0].color.r == doctest::Approx(1.0f));
        CHECK(batch.vertices[0].color.a == doctest::Approx(1.0f));
        CHECK(batch.vertices[4].color.g == doctest::Approx(1.0f));
        CHECK(batch.vertices[4].color.a == doctest::Approx(0.5f));
    }
    f.ctx.BeginFrame(0.06f);
    CHECK_FALSE(v->IsTransitioning());
    {
        vg::VGContext vgContext;
        f.ctx.DrawRootView(f.root.Get(), vgContext); // settled: one green quad
        REQUIRE(vgContext.GetBatch().vertices.Size() == 4);
        CHECK(vgContext.GetBatch().vertices[0].color.g == doctest::Approx(1.0f));
    }
}

// === damage ===

TEST_CASE("transition: a layout-kind property marks layout damage per frame, a visual one redraw only")
{
    Fixture f(LoadSSS(u8"TestView { transition: font-size 100ms, text-color 100ms; font-size: 10; text-color: #000; }\n"
                      u8".big { font-size: 20; }\n"
                      u8".light { text-color: #fff; }\n"));
    auto v = f.Leaf();
    LayoutPass(f.ctx, f.root.Get());
    v->AddClass(u8"light");
    (void)Red(*v); // starts the visual transition
    LayoutPass(f.ctx, f.root.Get());
    f.ctx.ClearLayoutDamage(); // the host clears after it lays out
    CHECK_FALSE(f.ctx.NeedsLayout());
    f.ctx.BeginFrame(0.01f);
    CHECK_FALSE(f.ctx.NeedsLayout());
    CHECK(f.ctx.NeedsRedraw());

    v->AddClass(u8"big");
    (void)v->ResolveStyleFloat(StyleProperty::FontSize); // starts the layout-kind one
    LayoutPass(f.ctx, f.root.Get());
    f.ctx.ClearLayoutDamage();
    f.ctx.BeginFrame(0.01f);
    CHECK(f.ctx.NeedsLayout());
}

// === motion is theme data ===

TEST_CASE("transition: motion comes from the theme's View rule; the engine adds no defaults")
{
    // A sheet with no transition rule: nothing transitions, controls included.
    Fixture f(LoadSSS(u8"TestView { text-color: #000; }\n"));
    auto button = core::MakeRef<Button>(core::DefaultAllocator(), StringView(u8"Go"));
    f.root->AddView(button.Get());
    auto leaf = f.Leaf();
    CHECK(button->ResolveStyle(StyleProperty::Transition).AsTransitions() == nullptr);
    CHECK(leaf->ResolveStyle(StyleProperty::Transition).AsTransitions() == nullptr);

    // The shipped themes declare it on View, so every view (a Button, a plain leaf) gets it;
    // a later rule on a type opts that type out.
    f.ctx.SetStyleSheet(LoadSSS(u8"View { transition: all 120ms ease-out; }\n"
                                u8"ButtonBase { transition: none; }\n"));
    const StyleValue onLeaf = leaf->ResolveStyle(StyleProperty::Transition);
    REQUIRE(onLeaf.AsTransitions() != nullptr);
    const TransitionSpec* all = onLeaf.AsTransitions()->Find(StyleProperty::TextColor);
    REQUIRE(all != nullptr);
    CHECK(all->Duration == doctest::Approx(0.12f));
    CHECK(all->Easing == TimingFunction::EaseOut());
    const StyleValue onButton = button->ResolveStyle(StyleProperty::Transition);
    REQUIRE(onButton.AsTransitions() != nullptr);
    CHECK(onButton.AsTransitions()->Find(StyleProperty::TextColor) == nullptr);
}

TEST_CASE("transition: the shipped themes carry a motion rule on View")
{
    EnsureGlobals();
    core::RefPtr<StyleSheet> dark = DarkTheme::Create(DefaultAllocator());
    REQUIRE(dark);
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    ctx.SetStyleSheet(dark);
    auto button = core::MakeRef<Button>(core::DefaultAllocator(), StringView(u8"Go"));
    root->AddView(button.Get());
    const StyleValue spec = button->ResolveStyle(StyleProperty::Transition);
    REQUIRE(spec.AsTransitions() != nullptr);
    CHECK(spec.AsTransitions()->Find(StyleProperty::Background) != nullptr);
    CHECK(spec.AsTransitions()->Find(StyleProperty::TextColor) != nullptr);
    CHECK(spec.AsTransitions()->Find(StyleProperty::FontSize) == nullptr); // geometry never animates
}

TEST_CASE("transition: a child with no rule of its own inherits the parent's mid-flight value")
{
    Fixture f(LoadSSS(u8"TestGroup { transition: text-color 100ms linear; text-color: #000000; }\n"
                      u8"TestGroup.light { text-color: #ffffff; }\n"));
    auto group = core::MakeRef<TestGroup>(core::DefaultAllocator());
    f.root->AddView(group.Get());
    auto child = core::MakeRef<TestView>(core::DefaultAllocator(), 50.0f, 30.0f);
    group->AddView(child.Get());
    CHECK(Red(*child) == doctest::Approx(0.0f));
    group->AddClass(u8"light");
    CHECK(Red(*group) == doctest::Approx(0.0f)); // starts on the parent
    f.ctx.BeginFrame(0.05f);
    CHECK(Red(*group) == doctest::Approx(0.5f));
    CHECK(Red(*child) == doctest::Approx(0.5f)); // inherited through the overlay, no entry of its own
    CHECK(child->ActiveTransitionCount() == 0);
}

TEST_CASE("transition: only a changed winner starts an entry (a theme-wide rule stays cheap)")
{
    Fixture f(LoadSSS(u8"View { transition: all 100ms linear; }\n"
                      u8"TestView { text-color: #000000; font-size: 10; }\n"
                      u8".tag { border-color: #ff0000; }\n"));
    auto v = f.Leaf();
    CHECK(Red(*v) == doctest::Approx(0.0f));
    v->AddClass(u8"tag"); // border-color gains a winner; text-color and font-size keep theirs
    (void)Red(*v);
    CHECK(v->ActiveTransitionCount() == 0); // border-color had no old value to start from
    v->RemoveClass(u8"tag");
    (void)Red(*v);
    CHECK(v->ActiveTransitionCount() == 0); // and losing it goes to None: nothing to run to
}

// === edges ===

TEST_CASE("transition: a sheet swap ends running transitions without animating")
{
    Fixture f(LoadSSS(u8"TestView { transition: text-color 100ms linear; text-color: #000000; }\n"
                      u8".big { text-color: #ffffff; }\n"));
    auto v = f.Leaf();
    CHECK(Red(*v) == doctest::Approx(0.0f));
    v->AddClass(u8"big");
    (void)Red(*v);
    f.ctx.BeginFrame(0.05f);
    CHECK(Red(*v) == doctest::Approx(0.5f));
    f.ctx.SetStyleSheet(LoadSSS(u8"TestView { transition: text-color 100ms linear; text-color: #0000ff; }\n"));
    CHECK(Red(*v) == doctest::Approx(0.0f)); // the new sheet's value, at once
    CHECK(v->ActiveTransitionCount() == 0);
    f.ctx.BeginFrame(0.01f);
    CHECK(f.ctx.TransitioningViewCount() == 0);
}

TEST_CASE("transition: detaching a view mid-flight de-lists it and the tick stays safe")
{
    Fixture f(LoadSSS(u8"TestView { transition: text-color 100ms linear; text-color: #000000; }\n"
                      u8".big { text-color: #ffffff; }\n"));
    auto v = f.Leaf();
    CHECK(Red(*v) == doctest::Approx(0.0f));
    v->AddClass(u8"big");
    (void)Red(*v);
    CHECK(f.ctx.TransitioningViewCount() == 1);
    f.root->RemoveView(v.Get());
    CHECK(f.ctx.TransitioningViewCount() == 0);
    CHECK_FALSE(v->IsTransitioning());
    v.Reset(); // gone for good
    f.ctx.BeginFrame(0.05f); // nothing dangling to tick
    CHECK(f.ctx.TransitioningViewCount() == 0);
}

// === animations (@keyframes) ===

namespace
{
    f32 OpacityOf(View& v) { return v.ResolveStyleFloat(StyleProperty::Opacity, -1.0f); }
}

TEST_CASE("animation: @keyframes and the animation list parse")
{
    core::RefPtr<StyleSheet> sheet = LoadSSS(
        u8"@keyframes pulse { to { opacity: 1; } from { opacity: 0; } 25%, 75% { opacity: 0.8; } }\n"
        u8"TestView { animation: pulse 2s cubic-bezier(0.2, 0, 0, 1) 500ms infinite alternate both, "
        u8"spin 300ms 3 reverse forwards; }\n"
        u8".none { animation: none; }\n");
    REQUIRE(sheet);
    const KeyframesRule* pulse = sheet->FindKeyframes(u8"pulse");
    REQUIRE(pulse != nullptr);
    // Sorted by offset, a shared block under each of its selectors.
    REQUIRE(pulse->Frames.Size() == 4u);
    CHECK(pulse->Frames[0].Offset == doctest::Approx(0.0f));
    CHECK(pulse->Frames[1].Offset == doctest::Approx(0.25f));
    CHECK(pulse->Frames[2].Offset == doctest::Approx(0.75f));
    CHECK(pulse->Frames[3].Offset == doctest::Approx(1.0f));
    CHECK(pulse->Animates(StyleProperty::Opacity));
    CHECK_FALSE(pulse->Animates(StyleProperty::TextColor));
    CHECK(sheet->FindKeyframes(u8"spin") == nullptr);

    const AnimationList* list = sheet->GetRule(0).GetValue(StyleProperty::Animation).Value().AsAnimations();
    REQUIRE(list != nullptr);
    REQUIRE(list->Specs.Size() == 2u);
    const AnimationSpec& a = list->Specs[0];
    CHECK(a.Name == StringView(u8"pulse"));
    CHECK(a.Duration == doctest::Approx(2.0f));
    CHECK(a.Delay == doctest::Approx(0.5f));
    CHECK(a.Iterations < 0.0f);
    CHECK(a.Direction == AnimationDirection::Alternate);
    CHECK(a.Fill == AnimationFill::Both);
    CHECK(a.Easing == TimingFunction::CubicBezier(0.2f, 0.0f, 0.0f, 1.0f));
    const AnimationSpec& b = list->Specs[1];
    CHECK(b.Duration == doctest::Approx(0.3f));
    CHECK(b.Iterations == doctest::Approx(3.0f));
    CHECK(b.Direction == AnimationDirection::Reverse);
    CHECK(b.Fill == AnimationFill::Forwards);
    const AnimationList* none = sheet->GetRule(1).GetValue(StyleProperty::Animation).Value().AsAnimations();
    REQUIRE(none != nullptr);
    CHECK(none->Specs.IsEmpty());
}

TEST_CASE("animation: keyframes play over the cascade, a missing frame is the cascade's, fill holds the end")
{
    Fixture f(LoadSSS(u8"@keyframes fade { to { opacity: 1; } }\n"
                      u8"TestView { opacity: 0.2; animation: fade 1s linear forwards; }\n"));
    auto v = f.Leaf();
    CHECK(OpacityOf(*v) == doctest::Approx(0.2f)); // starts at the cascade's value: no from frame
    f.ctx.BeginFrame(0.5f);
    CHECK(OpacityOf(*v) == doctest::Approx(0.6f));
    f.ctx.BeginFrame(0.75f); // past the end
    CHECK(OpacityOf(*v) == doctest::Approx(1.0f)); // forwards holds the last frame
    CHECK(v->RunningAnimationCount() == 0u);
    f.ctx.BeginFrame(1.0f); // and it does not start again while the cascade names it
    CHECK(OpacityOf(*v) == doctest::Approx(1.0f));
}

TEST_CASE("animation: without a fill it gives the cascade back; a delay waits, backwards shows the first frame")
{
    Fixture f(LoadSSS(u8"@keyframes dim { from { opacity: 0.5; } to { opacity: 0; } }\n"
                      u8"TestView { opacity: 1; animation: dim 1s linear 1s; }\n"
                      u8".early { animation: dim 1s linear 1s backwards; }\n"));
    auto v = f.Leaf();
    CHECK(OpacityOf(*v) == doctest::Approx(1.0f)); // inside the delay: the cascade
    f.ctx.BeginFrame(1.5f);
    CHECK(OpacityOf(*v) == doctest::Approx(0.25f));
    f.ctx.BeginFrame(1.0f);
    CHECK(OpacityOf(*v) == doctest::Approx(1.0f)); // ended, no fill: the cascade again

    auto early = f.Leaf();
    early->AddClass(u8"early");
    CHECK(OpacityOf(*early) == doctest::Approx(0.5f)); // its delay shows the first frame
}

TEST_CASE("animation: alternate runs every other iteration backwards, each segment eased")
{
    Fixture f(LoadSSS(u8"@keyframes swing { from { opacity: 0; } 50% { opacity: 1; } to { opacity: 0.5; } }\n"
                      u8"TestView { animation: swing 1s linear infinite alternate; }\n"));
    auto v = f.Leaf();
    (void)OpacityOf(*v);
    f.ctx.BeginFrame(0.25f);
    CHECK(OpacityOf(*v) == doctest::Approx(0.5f)); // half way to the 50% frame
    f.ctx.BeginFrame(0.5f);
    CHECK(OpacityOf(*v) == doctest::Approx(0.75f)); // 75%: half way from 1 to 0.5
    f.ctx.BeginFrame(0.5f); // 1.25 s: the second iteration runs backwards, at 75% again
    CHECK(OpacityOf(*v) == doctest::Approx(0.75f));
    CHECK(v->RunningAnimationCount() == 1u);
}

TEST_CASE("animation: a transform keyframe animates the drawn transform and only redraws")
{
    Fixture f(LoadSSS(u8"@keyframes bob { from { transform: translateY(0); } to { transform: translateY(-10); } }\n"
                      u8"TestView { animation: bob 1s linear; }\n"));
    auto v = f.Leaf();
    LayoutPass(f.ctx, f.root.Get());
    f.ctx.ClearLayoutDamage();
    f.ctx.BeginFrame(0.5f);
    CHECK_FALSE(f.ctx.NeedsLayout()); // a transform never moves the layout
    vg::VGContext canvas;
    f.ctx.DrawRootView(f.root.Get(), canvas);
    CHECK(v->TransformMatrix().ty == doctest::Approx(-5.0f));
}

TEST_CASE("animation: the view says when an animation iterates and ends, after the frame's tick")
{
    Fixture f(LoadSSS(u8"@keyframes blink { to { opacity: 0; } }\n"
                      u8"TestView { animation: blink 1s linear 3; }\n"));
    auto v = f.Leaf();
    Array<String> events;
    v->OnAnimationIteration.Add([&events](View*, StringView name) { events.PushBack(Format(u8"iteration {}", name)); });
    v->OnAnimationEnd.Add([&events](View*, StringView name) { events.PushBack(Format(u8"end {}", name)); });
    (void)OpacityOf(*v);
    f.ctx.BeginFrame(1.25f); // into the second iteration
    // Queued during the tick: delivered at the next drain, not inside it.
    f.ctx.BeginFrame(0.0f);
    REQUIRE(events.Size() == 1u);
    CHECK(events[0] == StringView(u8"iteration blink"));
    f.ctx.BeginFrame(1.0f);  // the third
    f.ctx.BeginFrame(1.0f);  // past the last
    f.ctx.BeginFrame(0.0f);
    REQUIRE(events.Size() == 3u);
    CHECK(events[1] == StringView(u8"iteration blink"));
    CHECK(events[2] == StringView(u8"end blink"));
    f.ctx.BeginFrame(1.0f); // ended: nothing more
    f.ctx.BeginFrame(0.0f);
    CHECK(events.Size() == 3u);
}
