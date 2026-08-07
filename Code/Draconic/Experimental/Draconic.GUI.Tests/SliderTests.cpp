// Draconic GUI - Slider tests: value clamping + change callback, drag-to-set, and pointer
// capture (a drag keeps tracking after the cursor leaves the slider).
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.vg;
import draconic.gui;

using namespace draconic::gui;
namespace foundation = draconic::foundation;

namespace
{
    template <typename T>
    foundation::RefPtr<T> Make()
    {
        return foundation::MakeRef<T>(foundation::DefaultAllocator());
    }
}

TEST_CASE("slider: value clamps and notifies on change")
{
    auto s = Make<Slider>();
    int changes = 0;
    float last = -1.0f;
    s->SetOnValueChanged(
        [&](float v)
        {
            ++changes;
            last = v;
        });

    s->SetValue(0.5f);
    CHECK(s->GetValue() == doctest::Approx(0.5f));
    CHECK(changes == 1);
    CHECK(last == doctest::Approx(0.5f));

    s->SetValue(2.0f); // clamps to 1
    CHECK(s->GetValue() == doctest::Approx(1.0f));
    s->SetValue(-1.0f); // clamps to 0
    CHECK(s->GetValue() == doctest::Approx(0.0f));

    const int before = changes;
    s->SetValue(0.0f); // no change
    CHECK(changes == before);
}

TEST_CASE("slider: press sets value from the cursor x")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{200.0f, 200.0f});
    auto s = Make<Slider>();
    s->SetSize(foundation::Float2{100.0f, 20.0f}); // handleR = 10 -> usable x in [10, 90]
    root->AddChild(s.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    d->InjectMouseDown(foundation::Float2{50.0f, 10.0f}, MouseButton::Left); // (50-10)/(90-10) = 0.5
    CHECK(s->GetValue() == doctest::Approx(0.5f));
}

TEST_CASE("slider: drag keeps tracking after the cursor leaves it (pointer capture)")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{300.0f, 200.0f});
    auto s = Make<Slider>();
    s->SetSize(foundation::Float2{100.0f, 20.0f});
    root->AddChild(s.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    d->InjectMouseDown(foundation::Float2{10.0f, 10.0f}, MouseButton::Left); // value 0
    CHECK(s->GetValue() == doctest::Approx(0.0f));

    // Move the cursor well past the right edge of the slider - still captured, value saturates at 1.
    d->InjectMouseMove(foundation::Float2{250.0f, 10.0f});
    CHECK(s->GetValue() == doctest::Approx(1.0f));

    // Drag back to the middle.
    d->InjectMouseMove(foundation::Float2{50.0f, 10.0f});
    CHECK(s->GetValue() == doctest::Approx(0.5f));

    // Release ends the drag; a later move (button up) no longer changes the value.
    d->InjectMouseUp(foundation::Float2{50.0f, 10.0f}, MouseButton::Left);
    d->InjectMouseMove(foundation::Float2{90.0f, 10.0f});
    CHECK(s->GetValue() == doctest::Approx(0.5f));
}

TEST_CASE("slider: hover then press then drag off still tracks (leave clears m_pressed)")
{
    // Regression: hovering the slider first (so a MouseLeave fires when the cursor drags off)
    // used to cancel the drag - UINode::OnMouseLeave clears m_pressed and OnMouseMove gated on
    // IsPressed(). The slider's own m_dragging flag survives the leave.
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{300.0f, 200.0f});
    auto s = Make<Slider>();
    s->SetSize(foundation::Float2{100.0f, 20.0f}); // usable x in [10, 90]
    root->AddChild(s.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    // Hover the slider first (this is what the capture-only test skipped).
    d->InjectMouseMove(foundation::Float2{50.0f, 10.0f});
    CHECK(s->IsHovered());

    // Press, then drag off the right edge - the MouseLeave must not cancel the drag.
    d->InjectMouseDown(foundation::Float2{50.0f, 10.0f}, MouseButton::Left);
    CHECK(s->GetValue() == doctest::Approx(0.5f));
    d->InjectMouseMove(foundation::Float2{250.0f, 10.0f}); // off to the right -> leave fires, then move
    CHECK_FALSE(s->IsHovered());                     // the leave did happen
    CHECK(s->GetValue() == doctest::Approx(1.0f));   // ...but the drag kept tracking

    // Release ends the drag.
    d->InjectMouseUp(foundation::Float2{250.0f, 10.0f}, MouseButton::Left);
    d->InjectMouseMove(foundation::Float2{30.0f, 10.0f});
    CHECK(s->GetValue() == doctest::Approx(1.0f)); // no longer dragging
}

TEST_CASE("slider: draws track + fill + handle")
{
    auto s = Make<Slider>();
    s->SetSize(foundation::Float2{120.0f, 20.0f});
    s->SetValue(0.5f);

    draconic::vg::VGContext ctx;
    DrawContext dc{ctx};
    s->Draw(dc);
    CHECK(ctx.GetBatch().vertices.Size() > 0);
}

TEST_CASE("slider: right button does not drag the value")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{200.0f, 200.0f});
    auto s = Make<Slider>();
    s->SetSize(foundation::Float2{100.0f, 20.0f});
    root->AddChild(s.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    d->InjectMouseDown(foundation::Float2{50.0f, 10.0f}, MouseButton::Right); // right press
    d->InjectMouseMove(foundation::Float2{90.0f, 10.0f});
    CHECK(s->GetValue() == doctest::Approx(0.0f)); // unchanged - only the left button drags
}
