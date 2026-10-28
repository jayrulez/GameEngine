// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// A slider's focus shows (Sedulous a7745c65). Its parts were drawn without the control's state, so
// a state-list thumb (a knob that lights up when focused) only ever drew its normal entry and a pad
// player could not tell which slider they were moving; and the built-in themes gave the knob no
// focused look to draw.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.vg;
import foundation.ui;
#include "TestHelpers.h"

using namespace foundation::ui;
using namespace foundation::ui::tests;
using namespace foundation::core;
namespace core = foundation::core;

namespace
{
    // Records the state each draw was given.
    class StateProbe final : public Drawable
    {
    public:
        i32 draws = 0;
        ControlState lastState = ControlState::Normal;

        void Draw(UIDrawContext&, const core::Rectangle&) override
        {
            ++draws;
            lastState = ControlState::Normal;
        }

    protected:
        void DrawState(UIDrawContext&, const core::Rectangle&, ControlState state) override
        {
            ++draws;
            lastState = state;
        }
    };
}

TEST_CASE("slider-focus: the thumb is drawn with the slider's focus")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    auto sheet = core::MakeRef<StyleSheet>(core::DefaultAllocator());
    auto probe = core::MakeRef<StateProbe>(core::DefaultAllocator());
    StateProbe* raw = probe.Get();
    sheet->ForTypePseudo(&Slider::StaticType(), u8"thumb").Set(StyleProperty::Background, RefPtr<Drawable>(probe));
    ctx.SetStyleSheet(sheet);

    auto slider = core::MakeRef<Slider>(core::DefaultAllocator(), 0.0f, 1.0f, 0.5f);
    root->AddView(slider.Get());
    slider->Measure(BoxConstraints::Tight(200.0f, 24.0f));
    slider->Layout(0.0f, 0.0f, 200.0f, 24.0f);
    foundation::vg::VGContext vg;
    UIDrawContext draw(vg, 1.0f, nullptr);

    // Focus as keys or a pad give it: the ring's state reaches the thumb.
    ctx.GetFocusManager()->SetFocus(slider.Get(), FocusSource::Keyboard);
    static_cast<View*>(slider.Get())->OnDraw(draw);
    CHECK(raw->draws > 0); // the thumb was drawn
    CHECK(HasFlag(raw->lastState, ControlState::Focused));

    // Without focus it is not.
    ctx.GetFocusManager()->ClearFocus();
    static_cast<View*>(slider.Get())->OnDraw(draw);
    CHECK_FALSE(HasFlag(raw->lastState, ControlState::Focused));
}

TEST_CASE("slider-focus: every built-in theme gives the thumb a focused look of its own")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());
    auto slider = core::MakeRef<Slider>(core::DefaultAllocator(), 0.0f, 1.0f, 0.5f);
    root->AddView(slider.Get());

    const auto check = [&](RefPtr<StyleSheet> sheet, const char* theme)
    {
        CAPTURE(theme);
        ctx.SetStyleSheet(sheet);
        auto* thumb = Cast<StateListDrawable>(
            slider->ResolvePartDrawable(u8"thumb", StyleProperty::Background, ControlState::Normal));
        REQUIRE(thumb != nullptr); // the thumb is a state list
        CHECK(thumb->Get(ControlState::Focused) != nullptr);
        CHECK(thumb->Get(ControlState::Focused) != thumb->Get(ControlState::Normal));
    };
    check(DarkTheme::Create(DefaultAllocator()), "dark");
    check(LightTheme::Create(DefaultAllocator()), "light");
    check(RoundedDarkTheme::Create(DefaultAllocator()), "rounded-dark");
}
