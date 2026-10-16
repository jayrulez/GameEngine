// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - motion (G3 of Specs/vg-and-gui-lab.md): the `transition` and `animation` lists parse; a
// changed property with a transition moves from what showed toward its new value on the GUI's
// clock, eased, for any property the sheet sets (opacity, lengths, colours, box shadows,
// transforms, gradients); without one, or unchanged, nothing moves; a transition retargets from
// where it is.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import experimental.gui;

using namespace experimental::gui;
namespace core = foundation::core;

namespace
{
    core::StringView SV(const char8_t* s) { return core::StringView(s); }
    core::Duration Sec(double s) { return core::Duration::FromSeconds(s); }

    // A widget in a GUI, styled by a sheet; `cls` toggles a class to change its style.
    struct Bed
    {
        core::RefPtr<SceneRoot> root = core::MakeRef<SceneRoot>(core::DefaultAllocator());
        core::RefPtr<UIWidget> widget = core::MakeRef<UIWidget>(core::DefaultAllocator());
        StyleManager styles;

        explicit Bed(const char8_t* css)
        {
            widget->SetTag(SV(u8"box"));
            widget->SetSize(core::Float2{100.0f, 50.0f});
            root->AddChild(widget.Get());
            styles.SetStyleSheet(CSSParser::Parse(SV(css)));
            styles.ApplyTree(*root);
        }
        void Toggle(const char8_t* cls)
        {
            widget->ToggleClass(SV(cls));
            styles.ApplyTree(*root);
        }
        void Advance(double seconds) { root->Update(Sec(seconds)); }
    };
}

TEST_CASE("transition: the list parses properties, durations, easings and delays")
{
    const core::Array<TransitionSpec> one = ParseTransitionList(SV(u8"opacity 0.3s"));
    REQUIRE(one.Size() == 1u);
    CHECK(one[0].Property == SV(u8"opacity"));
    CHECK(one[0].Duration == doctest::Approx(0.3f));
    CHECK(one[0].Timing.Kind == EasingKind::CubicBezier); // ease, CSS's default

    const core::Array<TransitionSpec> many =
        ParseTransitionList(SV(u8"opacity 300ms linear 0.1s, transform 1s steps(4, jump-start), all 2s"));
    REQUIRE(many.Size() == 3u);
    CHECK(many[0].Duration == doctest::Approx(0.3f));
    CHECK(many[0].Delay == doctest::Approx(0.1f));
    CHECK(many[0].Timing.Kind == EasingKind::Linear);
    CHECK(many[1].Timing.Kind == EasingKind::Steps);
    CHECK(many[1].Timing.Steps == 4);
    CHECK(many[2].Property == SV(u8"all"));
    CHECK(ParseTransitionList(SV(u8"none")).IsEmpty());
}

TEST_CASE("transition: opacity moves on the GUI's clock, linearly when asked")
{
    Bed bed(u8"box { opacity: 1; transition: opacity 0.5s linear; } box.gone { opacity: 0; }");
    CHECK(bed.widget->GetAlpha() == doctest::Approx(1.0f));
    bed.Toggle(u8"gone");
    CHECK(bed.root->GetActionManager()->Count() == 1); // the widget's StyleAnimation
    CHECK(bed.widget->GetAlpha() == doctest::Approx(1.0f)); // starts where it was
    bed.Advance(0.25);
    CHECK(bed.widget->GetAlpha() == doctest::Approx(0.5f)); // halfway
    bed.Advance(0.25);
    CHECK(bed.widget->GetAlpha() == doctest::Approx(0.0f)); // arrived
    CHECK(bed.root->GetActionManager()->IsEmpty());          // finished
}

TEST_CASE("transition: the easing shapes the move (ease-in starts slow)")
{
    Bed bed(u8"box { opacity: 0; transition: opacity 1s ease-in; } box.on { opacity: 1; }");
    bed.Toggle(u8"on");
    bed.Advance(0.5);
    const float expected = Easing::Bezier(0.42f, 0.0f, 1.0f, 1.0f).Evaluate(0.5f);
    CHECK(bed.widget->GetAlpha() == doctest::Approx(expected));
    CHECK(bed.widget->GetAlpha() < 0.5f);
}

TEST_CASE("transition: without a transition, or with no change, nothing moves")
{
    Bed snap(u8"box { opacity: 1; } box.gone { opacity: 0; }");
    snap.Toggle(u8"gone");
    CHECK(snap.widget->GetAlpha() == doctest::Approx(0.0f));
    CHECK(snap.root->GetActionManager()->IsEmpty());

    Bed same(u8"box { opacity: 1; transition: opacity 0.5s; } box.same { opacity: 1; }");
    same.Toggle(u8"same");
    CHECK(same.root->GetActionManager()->IsEmpty());
}

TEST_CASE("transition: any property moves: a length, a colour, box shadows, a transform")
{
    Bed bed(u8"box { width: 100; background-color: #000000; box-shadow: 0 0 0 transparent; transform: rotate(0deg); "
            u8"transition: all 1s linear; } "
            u8"box.on { width: 200; background-color: #ff0000; box-shadow: 0 4px 8px #000000; transform: rotate(90deg); }");
    bed.Toggle(u8"on");
    bed.Advance(0.5);
    CHECK(bed.widget->GetSize().x == doctest::Approx(150.0f));
    CHECK(bed.widget->GetBox().BackgroundColor.r == doctest::Approx(0.5f));
    REQUIRE(bed.widget->GetBox().Shadows.Size() == 1u);
    CHECK(bed.widget->GetBox().Shadows[0].Offset.y == doctest::Approx(2.0f));
    CHECK(bed.widget->GetBox().Shadows[0].Blur == doctest::Approx(4.0f));
    REQUIRE(bed.widget->GetStyleTransforms().Size() == 1u);
    CHECK(bed.widget->GetStyleTransforms()[0].Angle == doctest::Approx(0.7853982f)); // 45deg
    bed.Advance(0.5);
    CHECK(bed.widget->GetSize().x == doctest::Approx(200.0f));
    CHECK(bed.widget->GetStyleTransforms()[0].Angle == doctest::Approx(1.5707963f));
}

TEST_CASE("transition: a delay holds the old value; a change mid-way retargets from where it is")
{
    Bed bed(u8"box { width: 100; transition: width 1s linear 0.5s; } box.wide { width: 300; }");
    bed.Toggle(u8"wide");
    bed.Advance(0.25);
    CHECK(bed.widget->GetSize().x == doctest::Approx(100.0f)); // still in the delay
    bed.Advance(0.75);                                          // 0.5 s into the move
    CHECK(bed.widget->GetSize().x == doctest::Approx(200.0f));
    bed.Toggle(u8"wide"); // back, from 200, after its own delay
    bed.Advance(0.5);
    CHECK(bed.widget->GetSize().x == doctest::Approx(200.0f));
    bed.Advance(0.5);
    CHECK(bed.widget->GetSize().x == doctest::Approx(150.0f)); // halfway from 200 to 100
}

TEST_CASE("transition: a gradient's stops and angle move; different shapes switch halfway")
{
    Bed bed(u8"box { background-image: linear-gradient(0deg, #000000, #000000); transition: background-image 1s linear; } "
            u8"box.on { background-image: linear-gradient(90deg, #ff0000, #0000ff); }");
    bed.Toggle(u8"on");
    bed.Advance(0.5);
    REQUIRE(bed.widget->GetBox().Layers.Size() == 1u);
    CHECK(bed.widget->GetBox().Layers[0].Gradient.Angle == doctest::Approx(0.7853982f));
    CHECK(bed.widget->GetBox().Layers[0].Gradient.Stops[0].color.r == doctest::Approx(0.5f));

    // A keyword cannot interpolate: it switches at the halfway point.
    StyleValue left, right;
    left.Kind = right.Kind = StyleValueKind::Keyword;
    left.Keyword = core::String(u8"left");
    right.Keyword = core::String(u8"right");
    CHECK(InterpolateStyleValue(left, right, 0.4f).Keyword == SV(u8"left"));
    CHECK(InterpolateStyleValue(left, right, 0.6f).Keyword == SV(u8"right"));
}

TEST_CASE("transition: a widget destroyed mid-move ends its animation, which never touches it again")
{
    Bed bed(u8"box { opacity: 1; transition: opacity 1s linear; } box.gone { opacity: 0; }");
    bed.Toggle(u8"gone");
    REQUIRE(bed.root->GetActionManager()->Count() == 1);
    bed.styles.Forget(bed.widget.Get());
    bed.root->RemoveChild(bed.widget.Get());
    bed.widget = core::RefPtr<UIWidget>{}; // destroyed: the animation held it weakly
    bed.Advance(0.25);
    CHECK(bed.root->GetActionManager()->IsEmpty());
}
