// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - Slider tests: value clamping and the change callback, drag-to-set, pointer capture (a drag
// keeps tracking after the cursor leaves the slider), Escape putting a drag back, and the switch
// that stops the slider stepping on the wheel.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.vg;
import foundation.gui;

using namespace foundation::gui;
namespace core = foundation::core;

namespace
{
    template <typename T>
    core::RefPtr<T> Make()
    {
        return core::MakeRef<T>(core::DefaultAllocator());
    }
}

TEST_CASE("slider: value clamps to its range and snaps to its step; code's value reports nothing")
{
    auto s = Make<Slider>();
    int changes = 0;
    s->SetOnValueChanged([&](core::f64) { ++changes; });
    s->SetOnValueChanging([&](core::f64) { ++changes; });

    s->SetValue(0.5);
    CHECK(s->GetValue() == doctest::Approx(0.5));
    s->SetValue(2.0); // clamps to 1
    CHECK(s->GetValue() == doctest::Approx(1.0));
    s->SetValue(-1.0); // clamps to 0
    CHECK(s->GetValue() == doctest::Approx(0.0));
    CHECK(changes == 0);

    s->SetMin(10.0);
    s->SetMax(20.0);
    s->SetStep(2.5);
    CHECK(s->GetValue() == doctest::Approx(10.0)); // into the new range
    s->SetValue(13.9);
    CHECK(s->GetValue() == doctest::Approx(15.0)); // the nearest step
}

TEST_CASE("slider: press sets value from the cursor x")
{
    auto root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
    root->SetSize(core::Float2{200.0f, 200.0f});
    auto s = Make<Slider>();
    s->SetSize(core::Float2{100.0f, 20.0f}); // handleR = 10 -> usable x in [10, 90]
    root->AddChild(s.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    d->InjectMouseDown(core::Float2{50.0f, 10.0f}, MouseButton::Left); // (50-10)/(90-10) = 0.5
    CHECK(s->GetValue() == doctest::Approx(0.5f));
}

TEST_CASE("slider: drag keeps tracking after the cursor leaves it (pointer capture)")
{
    auto root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
    root->SetSize(core::Float2{300.0f, 200.0f});
    auto s = Make<Slider>();
    s->SetSize(core::Float2{100.0f, 20.0f});
    root->AddChild(s.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    d->InjectMouseDown(core::Float2{10.0f, 10.0f}, MouseButton::Left); // value 0
    CHECK(s->GetValue() == doctest::Approx(0.0f));

    // Move the cursor well past the right edge of the slider: still captured, the value saturates at 1.
    d->InjectMouseMove(core::Float2{250.0f, 10.0f});
    CHECK(s->GetValue() == doctest::Approx(1.0f));

    // Drag back to the middle.
    d->InjectMouseMove(core::Float2{50.0f, 10.0f});
    CHECK(s->GetValue() == doctest::Approx(0.5f));

    // Release ends the drag; a later move (button up) no longer changes the value.
    d->InjectMouseUp(core::Float2{50.0f, 10.0f}, MouseButton::Left);
    d->InjectMouseMove(core::Float2{90.0f, 10.0f});
    CHECK(s->GetValue() == doctest::Approx(0.5f));
}

TEST_CASE("slider: hover then press then drag off still tracks (leave clears m_pressed)")
{
    // Hovering the slider first makes a MouseLeave fire when the cursor drags off. The drag keeps
    // tracking because the slider's own m_dragging flag survives the leave.
    auto root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
    root->SetSize(core::Float2{300.0f, 200.0f});
    auto s = Make<Slider>();
    s->SetSize(core::Float2{100.0f, 20.0f}); // usable x in [10, 90]
    root->AddChild(s.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    // Hover the slider first, which the capture-only test does not do.
    d->InjectMouseMove(core::Float2{50.0f, 10.0f});
    CHECK(s->IsHovered());

    // Press, then drag off the right edge. The MouseLeave must not cancel the drag.
    d->InjectMouseDown(core::Float2{50.0f, 10.0f}, MouseButton::Left);
    CHECK(s->GetValue() == doctest::Approx(0.5f));
    d->InjectMouseMove(core::Float2{250.0f, 10.0f}); // off to the right: the leave fires, then the move arrives
    CHECK_FALSE(s->IsHovered());                     // the leave did happen
    CHECK(s->GetValue() == doctest::Approx(1.0f));   // ...but the drag kept tracking

    // Release ends the drag.
    d->InjectMouseUp(core::Float2{250.0f, 10.0f}, MouseButton::Left);
    d->InjectMouseMove(core::Float2{30.0f, 10.0f});
    CHECK(s->GetValue() == doctest::Approx(1.0f)); // no longer dragging
}

TEST_CASE("slider: draws track + fill + handle")
{
    auto s = Make<Slider>();
    s->SetSize(core::Float2{120.0f, 20.0f});
    s->SetValue(0.5f);

    foundation::vg::VGContext ctx;
    DrawContext dc{ctx};
    s->Draw(dc);
    CHECK(ctx.GetBatch().vertices.Size() > 0);
}

TEST_CASE("slider: visible focus marks the thumb, in the colour of its focus part")
{
    auto s = Make<Slider>();
    s->SetSize(core::Float2{120.0f, 20.0f});
    s->SetValue(0.5f);
    const core::Color ring{1.0f, 0.0f, 1.0f, 1.0f};
    s->SetFocusColor(ring);
    const auto draw = [&]()
    {
        foundation::vg::VGContext ctx;
        DrawContext dc{ctx};
        s->Draw(dc);
        core::usize ringVertices = 0;
        for (const foundation::vg::VGVertex& v : ctx.GetBatch().vertices)
            if (v.color.r == ring.r && v.color.g == ring.g && v.color.b == ring.b)
                ++ringVertices;
        return ringVertices;
    };
    CHECK(draw() == 0u);
    s->HandleFocusGained();
    CHECK(draw() == 0u); // focused by a click, so no focus mark
    s->SetFocusVisible(true);
    CHECK(draw() > 0u); // the ring and halo round the thumb
}

TEST_CASE("slider: right button does not drag the value")
{
    auto root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
    root->SetSize(core::Float2{200.0f, 200.0f});
    auto s = Make<Slider>();
    s->SetSize(core::Float2{100.0f, 20.0f});
    root->AddChild(s.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    d->InjectMouseDown(core::Float2{50.0f, 10.0f}, MouseButton::Right); // right press
    d->InjectMouseMove(core::Float2{90.0f, 10.0f});
    CHECK(s->GetValue() == doctest::Approx(0.0f)); // unchanged, since only the left button drags
}

TEST_CASE("slider: a drag reports input as it goes and one change on release; keys and the wheel step")
{
    auto root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
    root->SetSize(core::Float2{300.0f, 300.0f});
    auto s = Make<Slider>();
    s->SetSize(core::Float2{100.0f, 20.0f}); // usable x in [10, 90]
    s->SetMax(100.0);
    s->SetStep(1.0);
    root->AddChild(s.Get());
    EventDispatcher* d = root->GetEventDispatcher();
    int inputs = 0, changes = 0, changeEvents = 0;
    s->SetOnValueChanging([&](core::f64) { ++inputs; });
    s->SetOnValueChanged([&](core::f64) { ++changes; });
    (void)s->AddEventListener(EventType::Changed, [&](const Event&) { ++changeEvents; });
    d->InjectMouseDown(core::Float2{30.0f, 10.0f}, MouseButton::Left); // 25
    d->InjectMouseMove(core::Float2{50.0f, 10.0f});                    // 50
    d->InjectMouseMove(core::Float2{70.0f, 10.0f});                    // 75
    CHECK(s->GetValue() == doctest::Approx(75.0));
    CHECK(inputs == 3);
    CHECK(changes == 0);
    d->InjectMouseUp(core::Float2{70.0f, 10.0f}, MouseButton::Left);
    CHECK(changes == 1);
    CHECK(changeEvents == 1);

    s->RequestFocus();
    d->InjectKeyDown(static_cast<core::u32>(KeyCode::Right));
    CHECK(s->GetValue() == doctest::Approx(76.0)); // its step
    d->InjectKeyDown(static_cast<core::u32>(KeyCode::PageDown));
    CHECK(s->GetValue() == doctest::Approx(66.0)); // a tenth of the range
    s->SetPage(25.0);
    d->InjectKeyDown(static_cast<core::u32>(KeyCode::PageUp));
    CHECK(s->GetValue() == doctest::Approx(91.0));
    d->InjectKeyDown(static_cast<core::u32>(KeyCode::End));
    CHECK(s->GetValue() == doctest::Approx(100.0));
    CHECK(changes == 5);

    // The wheel over the slider steps it; at its end the wheel is left to what holds the slider.
    int wheelsAbove = 0;
    (void)root->AddEventListener(EventType::MouseWheel, [&](const Event&) { ++wheelsAbove; });
    (void)d->InjectMouseWheel(core::Float2{50.0f, 10.0f}, core::Float2{0.0f, 1.0f});
    CHECK(s->GetValue() == doctest::Approx(100.0));
    CHECK(wheelsAbove == 1); // it could not move, so the wheel went on up
    (void)d->InjectMouseWheel(core::Float2{50.0f, 10.0f}, core::Float2{0.0f, -2.0f});
    CHECK(s->GetValue() == doctest::Approx(98.0));
    CHECK(wheelsAbove == 1);
}

TEST_CASE("slider: Escape during a drag puts the value back and reports it cancelled; the wheel may be left to what holds it")
{
    auto root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
    root->SetSize(core::Float2{300.0f, 300.0f});
    auto s = Make<Slider>();
    s->SetSize(core::Float2{100.0f, 20.0f}); // usable x in [10, 90]
    s->SetMax(100.0);
    s->SetStep(1.0);
    s->SetValue(40.0);
    root->AddChild(s.Get());
    EventDispatcher* d = root->GetEventDispatcher();
    int changes = 0, cancels = 0;
    s->SetOnValueChanged([&](core::f64) { ++changes; });
    s->SetOnValueCancelled([&]() { ++cancels; });
    s->RequestFocus();
    d->InjectMouseDown(core::Float2{70.0f, 10.0f}, MouseButton::Left); // 75
    CHECK(s->IsDragging());
    CHECK(s->GetValue() == doctest::Approx(75.0));
    d->InjectKeyDown(static_cast<core::u32>(KeyCode::Escape));
    CHECK_FALSE(s->IsDragging());
    CHECK(s->GetValue() == doctest::Approx(40.0)); // back to the value before the drag
    CHECK(cancels == 1);
    d->InjectMouseMove(core::Float2{30.0f, 10.0f});
    d->InjectMouseUp(core::Float2{30.0f, 10.0f}, MouseButton::Left);
    CHECK(s->GetValue() == doctest::Approx(40.0)); // the release after it changes nothing
    CHECK(changes == 0);

    // When told not to step on the wheel (as in a scrolling list), the slider passes the wheel on
    // to what holds it.
    s->SetStepsOnWheel(false);
    int wheelsAbove = 0;
    (void)root->AddEventListener(EventType::MouseWheel, [&](const Event&) { ++wheelsAbove; });
    (void)d->InjectMouseWheel(core::Float2{50.0f, 10.0f}, core::Float2{0.0f, 1.0f});
    CHECK(s->GetValue() == doctest::Approx(40.0));
    CHECK(wheelsAbove == 1);
}

TEST_CASE("slider: vertical grows upward; exponential spreads a wide range")
{
    auto root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
    root->SetSize(core::Float2{300.0f, 300.0f});
    auto s = Make<Slider>();
    s->SetOrientationName(core::String(core::StringView(u8"vertical")));
    s->SetSize(core::Float2{20.0f, 100.0f}); // usable y in [10, 90]
    root->AddChild(s.Get());
    EventDispatcher* d = root->GetEventDispatcher();
    d->InjectMouseDown(core::Float2{10.0f, 30.0f}, MouseButton::Left); // a quarter down: 0.75
    CHECK(s->GetValue() == doctest::Approx(0.75));
    d->InjectMouseUp(core::Float2{10.0f, 30.0f}, MouseButton::Left);
    s->RequestFocus();
    d->InjectKeyDown(static_cast<core::u32>(KeyCode::Up));
    CHECK(s->GetValue() == doctest::Approx(0.8));

    RangeValue hz;
    hz.Min = 20.0;
    hz.Max = 20000.0;
    hz.Exponential = true;
    CHECK(hz.FromRatio(0.5) == doctest::Approx(632.45).epsilon(0.01)); // the geometric middle
    hz.Value = 2000.0;
    CHECK(hz.Ratio() == doctest::Approx(2.0 / 3.0).epsilon(0.01));
    RangeValue steps;
    steps.Step = 0.25;
    CHECK(steps.Decimals() == 2);
}

TEST_CASE("range value: a step read from a float shows its own decimals and leaves no error in the value")
{
    RangeValue range;
    range.Min = 0.0;
    range.Max = 1000.0;
    range.Step = static_cast<double>(0.1f); // 0.100000001, as a reflected range's step is
    CHECK(range.Decimals() == 1);
    CHECK(range.Constrain(12.0) == 12.0); // not 12.0000001788
    CHECK(range.Constrain(0.6) == doctest::Approx(0.6).epsilon(1e-12));
    range.Min = -8.0;
    range.Max = 8.0;
    CHECK(range.Constrain(0.0) == 0.0); // not 1.2e-07
    range.Step = static_cast<double>(0.05f);
    CHECK(range.Decimals() == 2);
    range.Step = 0.25;
    CHECK(range.Decimals() == 2);
    range.Step = 1.0;
    CHECK(range.Decimals() == 0);
}
