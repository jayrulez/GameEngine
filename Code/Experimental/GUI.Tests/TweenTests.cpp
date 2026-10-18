// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - tweens (E14 of Specs/gui-lab-evolution-2.md): a property a sheet names moves over the
// cascade on the GUI's clock, from the value underneath or a given start, through the same typed
// values a transition moves (a number, a length, a colour, box shadows, a transform), and holds
// its end until stopped, a sheet's later change leaving it be; a tween of the same property
// replaces the running one from where it shows; delays, repeats, ping-pong and the callback at
// the end; reflected properties move through their accessors; a paused GUI's tweens stand still,
// and a destroyed node takes its tweens with it.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import experimental.gui;

using namespace experimental::gui;
namespace core = foundation::core;

namespace
{
    core::StringView SV(const char8_t* s) { return core::StringView(s); }

    // A widget in a GUI styled by a sheet; a frame runs the clock, then the style pass, as a host does.
    struct Bed
    {
        core::RefPtr<SceneRoot> root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
        core::RefPtr<UIWidget> widget = core::MakeRef<UIWidget>(core::DefaultAllocator());
        StyleManager styles;

        explicit Bed(const char8_t* css)
        {
            widget->SetTag(SV(u8"box"));
            {
                Node::StyleWriteScope defaults(*widget);
                widget->SetSize(core::Float2{100.0f, 50.0f});
            }
            root->AddChild(widget.Get());
            styles.SetStyleSheet(CSSParser::Parse(SV(css)));
            styles.ApplyTree(*root);
        }
        void Frame(double seconds)
        {
            root->GetContext().Update(core::Duration::FromSeconds(seconds));
            styles.ApplyTree(*root);
            (void)root->UpdateLayout();
        }
        [[nodiscard]] float Red() const { return widget->GetBox().BackgroundColor.r; }
    };
}

TEST_CASE("tween: a sheet's property moves over the cascade from the value underneath, and holds its end")
{
    Bed bed(u8"box { background-color: #000000; } box.lit { background-color: #00ff00; }");
    core::RefPtr<Tween> tween = bed.widget->Animate(SV(u8"background-color"), SV(u8"#ff0000"), 1.0f, SV(u8"linear"));
    REQUIRE(tween);
    bed.Frame(0.5);
    CHECK(tween->MovesStyle());
    CHECK(bed.Red() == doctest::Approx(0.5f));
    bed.Frame(0.5);
    CHECK(bed.Red() == doctest::Approx(1.0f));
    CHECK(tween->IsDone());
    bed.Frame(0.5);
    CHECK(bed.Red() == doctest::Approx(1.0f)); // held

    // The sheet changes underneath: the tween's end still shows.
    bed.widget->AddClass(SV(u8"lit"));
    bed.Frame(0.1);
    CHECK(bed.Red() == doctest::Approx(1.0f));
    CHECK(bed.widget->GetBox().BackgroundColor.g == doctest::Approx(0.0f));
    // Stopped: the property is the sheet's again.
    CHECK(bed.widget->StopAnimation(SV(u8"background-color")));
    bed.Frame(0.0);
    CHECK(bed.Red() == doctest::Approx(0.0f));
    CHECK(bed.widget->GetBox().BackgroundColor.g == doctest::Approx(1.0f));
    CHECK_FALSE(bed.widget->StopAnimation(SV(u8"background-color"))); // nothing moves it now
}

TEST_CASE("tween: every value kind a transition moves: a number, a length, box shadows, a transform")
{
    Bed bed(u8"box { opacity: 1; width: 100; box-shadow: 0 0 0 transparent; transform: rotate(0deg); }");
    bed.widget->Animate(SV(u8"opacity"), SV(u8"0"), 1.0f, SV(u8"linear"));
    bed.widget->Animate(SV(u8"width"), SV(u8"200"), 1.0f, SV(u8"linear"));
    bed.widget->Animate(SV(u8"box-shadow"), SV(u8"0 4px 8px #000000"), 1.0f, SV(u8"linear"));
    bed.widget->Animate(SV(u8"transform"), SV(u8"rotate(90deg)"), 1.0f, SV(u8"linear"));
    bed.Frame(0.5);
    CHECK(bed.widget->GetAlpha() == doctest::Approx(0.5f));
    CHECK(bed.widget->GetSize().x == doctest::Approx(150.0f));
    REQUIRE(bed.widget->GetBox().Shadows.Size() == 1u);
    CHECK(bed.widget->GetBox().Shadows[0].Offset.y == doctest::Approx(2.0f));
    REQUIRE(bed.widget->GetStyleTransforms().Size() == 1u);
    CHECK(bed.widget->GetStyleTransforms()[0].Angle == doctest::Approx(0.7853982f));
    bed.Frame(0.5);
    CHECK(bed.widget->GetAlpha() == doctest::Approx(0.0f));
    CHECK(bed.widget->GetSize().x == doctest::Approx(200.0f));
}

TEST_CASE("tween: a given start, the easing, and nothing to move from")
{
    Bed bed(u8"box { }");
    bed.widget->Animate(SV(u8"opacity"), SV(u8"1"), 1.0f, SV(u8"ease-in-quad"))->From(SV(u8"0"));
    bed.Frame(0.5);
    CHECK(bed.widget->GetAlpha() == doctest::Approx(0.25f)); // eased in: a quarter at half time
    // A property no sheet sets and with no start moves from its initial value (transparent).
    bed.widget->Animate(SV(u8"background-color"), SV(u8"#ff0000"), 1.0f, SV(u8"linear"));
    bed.Frame(0.5);
    CHECK(bed.Red() == doctest::Approx(0.5f));
    CHECK(bed.widget->GetBox().BackgroundColor.a == doctest::Approx(0.5f));
}

TEST_CASE("tween: a tween of the same property replaces the running one from where it shows")
{
    Bed bed(u8"box { opacity: 1; }");
    core::RefPtr<Tween> first = bed.widget->Animate(SV(u8"opacity"), SV(u8"0"), 1.0f, SV(u8"linear"));
    bool firstFinished = false;
    first->OnFinished([&firstFinished]() { firstFinished = true; });
    bed.Frame(0.5);
    REQUIRE(bed.widget->GetAlpha() == doctest::Approx(0.5f));
    core::RefPtr<Tween> second = bed.widget->Animate(SV(u8"opacity"), SV(u8"1"), 1.0f, SV(u8"linear"));
    CHECK(first->IsDone());
    bed.Frame(0.0);
    CHECK(bed.widget->GetAlpha() == doctest::Approx(0.5f)); // no jump
    bed.Frame(0.5);
    CHECK(bed.widget->GetAlpha() == doctest::Approx(0.75f));
    bed.Frame(0.5);
    CHECK(bed.widget->GetAlpha() == doctest::Approx(1.0f));
    CHECK_FALSE(firstFinished); // replaced, not finished
}

TEST_CASE("tween: a delay waits, repeats run again, ping-pong comes back, and the end calls back once")
{
    Bed bed(u8"box { opacity: 1; }");
    int finished = 0;
    bed.widget->Animate(SV(u8"opacity"), SV(u8"0"), 1.0f, SV(u8"linear"))
        ->Delay(0.5f)
        .Repeat(2)
        .PingPong()
        .OnFinished([&finished]() { ++finished; });
    bed.Frame(0.25);
    CHECK(bed.widget->GetAlpha() == doctest::Approx(1.0f)); // waiting
    bed.Frame(0.75); // 0.5 into the first run
    CHECK(bed.widget->GetAlpha() == doctest::Approx(0.5f));
    bed.Frame(0.75); // 0.25 into the second, coming back
    CHECK(bed.widget->GetAlpha() == doctest::Approx(0.25f));
    CHECK(finished == 0);
    bed.Frame(1.0);
    CHECK(bed.widget->GetAlpha() == doctest::Approx(1.0f)); // back at the start
    CHECK(finished == 1);
    bed.Frame(1.0);
    CHECK(finished == 1);

    // Forever: still running long after, and Cancel gives the property back.
    core::RefPtr<Tween> forever = bed.widget->Animate(SV(u8"opacity"), SV(u8"0"), 0.1f, SV(u8"linear"));
    forever->Repeat(-1);
    for (int i = 0; i < 40; ++i)
        bed.Frame(0.25);
    CHECK_FALSE(forever->IsDone());
    forever->Cancel();
    bed.Frame(0.0);
    CHECK(bed.widget->GetAlpha() == doctest::Approx(1.0f)); // the sheet's
}

TEST_CASE("tween: a reflected property moves through its accessors, as code's")
{
    Bed bed(u8"box { }");
    auto slider = core::MakeRef<Slider>(core::DefaultAllocator());
    slider->SetMax(100.0f);
    auto label = core::MakeRef<Label>(core::DefaultAllocator());
    bed.root->AddChild(slider.Get());
    bed.root->AddChild(label.Get());
    core::RefPtr<Tween> value = slider->Animate(SV(u8"value"), SV(u8"80"), 1.0f, SV(u8"linear"));
    slider->SetValue(20.0f); // the start is the value it has at its first frame
    label->Animate(SV(u8"textColor"), SV(u8"#ffffff"), 1.0f, SV(u8"linear"))->From(SV(u8"#000000"));
    label->Animate(SV(u8"x"), SV(u8"40"), 1.0f, SV(u8"linear"));
    bed.Frame(0.5);
    CHECK_FALSE(value->MovesStyle());
    CHECK(slider->GetValue() == doctest::Approx(50.0f));
    CHECK(label->GetTextColor().r == doctest::Approx(0.5f));
    CHECK(label->GetX() == doctest::Approx(20.0f));
    bed.Frame(0.5);
    CHECK(slider->GetValue() == doctest::Approx(80.0f));
    // A name nothing has: the tween ends at once, touching nothing.
    core::RefPtr<Tween> nothing = label->Animate(SV(u8"no-such-thing"), SV(u8"1"), 1.0f);
    bed.Frame(0.1);
    CHECK(nothing->IsDone());
}

TEST_CASE("tween: a paused GUI's tweens stand still; a destroyed node takes its tweens with it")
{
    Bed bed(u8"box { opacity: 1; }");
    bed.widget->Animate(SV(u8"opacity"), SV(u8"0"), 1.0f, SV(u8"linear"));
    bed.Frame(0.25);
    REQUIRE(bed.widget->GetAlpha() == doctest::Approx(0.75f));
    bed.root->SetPaused(true);
    bed.Frame(0.5);
    CHECK(bed.widget->GetAlpha() == doctest::Approx(0.75f));
    bed.root->SetPaused(false);
    bed.root->SetTimeScale(2.0f); // a GUI's own time, apart from the game's
    bed.Frame(0.25);
    CHECK(bed.widget->GetAlpha() == doctest::Approx(0.25f));

    bed.styles.Forget(bed.widget.Get());
    bed.root->RemoveChild(bed.widget.Get());
    bed.widget = core::RefPtr<UIWidget>{};
    bed.Frame(0.1);
    CHECK(bed.root->GetActionManager()->IsEmpty());
}

TEST_CASE("tween: over a running transition, and a sheet transition after it ends")
{
    Bed bed(u8"box { opacity: 1; transition: opacity 1s linear; } box.dim { opacity: 0; }");
    bed.widget->ToggleClass(SV(u8"dim"));
    bed.styles.ApplyTree(*bed.root);
    bed.Frame(0.25);
    REQUIRE(bed.widget->GetAlpha() == doctest::Approx(0.75f));
    bed.widget->Animate(SV(u8"opacity"), SV(u8"1"), 0.5f, SV(u8"linear"))->From(SV(u8"0.5"));
    bed.Frame(0.25);
    CHECK(bed.widget->GetAlpha() == doctest::Approx(0.75f)); // the tween's, over the transition's 0.5
    bed.Frame(0.25);
    CHECK(bed.widget->GetAlpha() == doctest::Approx(1.0f));
    bed.Frame(1.0); // the transition ended; the tween still holds
    CHECK(bed.widget->GetAlpha() == doctest::Approx(1.0f));
}
