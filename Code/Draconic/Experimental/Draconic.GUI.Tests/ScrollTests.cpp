// Draconic GUI - ScrollBar + ScrollView tests: scroll offset clamping, wheel bubbling from
// hovered content, drag (with the leave-during-capture regression), and ScrollBar<->ScrollView
// composition.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.vg;
import draconic.gui;

using namespace draconic::gui;
namespace foundation = draconic::foundation;
namespace vg = draconic::vg;

namespace
{
    template <typename T>
    foundation::RefPtr<T> Make()
    {
        return foundation::MakeRef<T>(foundation::DefaultAllocator());
    }

    foundation::RefPtr<UIWidget> Cell(float w, float h)
    {
        auto n = foundation::MakeRef<UIWidget>(foundation::DefaultAllocator());
        n->SetSize(foundation::Float2{w, h});
        return n;
    }
}

TEST_CASE("scrollview: range and offset clamp to the content extent")
{
    auto sv = Make<ScrollView>();
    sv->SetSize(foundation::Float2{100.0f, 100.0f});        // viewport 100x100
    sv->SetContentSize(foundation::Float2{100.0f, 300.0f}); // vertical scroll only, range 200

    CHECK(sv->ScrollRange().x == doctest::Approx(0.0f));
    CHECK(sv->ScrollRange().y == doctest::Approx(200.0f));

    sv->SetScrollOffset(foundation::Float2{50.0f, 50.0f}); // x clamps to 0, y ok
    CHECK(sv->GetScrollOffset().x == doctest::Approx(0.0f));
    CHECK(sv->GetScrollOffset().y == doctest::Approx(50.0f));

    sv->SetScrollOffset(foundation::Float2{0.0f, 999.0f}); // clamps to range.y
    CHECK(sv->GetScrollOffset().y == doctest::Approx(200.0f));
}

TEST_CASE("scrollview: scrolling offsets the content container")
{
    auto sv = Make<ScrollView>();
    sv->SetSize(foundation::Float2{100.0f, 100.0f});
    sv->SetContentSize(foundation::Float2{100.0f, 300.0f});

    // Content starts at the viewport origin (no padding here).
    CHECK(sv->GetContent()->GetPosition().y == doctest::Approx(0.0f));
    sv->SetScrollOffset(foundation::Float2{0.0f, 40.0f});
    CHECK(sv->GetContent()->GetPosition().y == doctest::Approx(-40.0f)); // scrolled up
}

TEST_CASE("scrollview: content is clipped to the viewport")
{
    auto sv = Make<ScrollView>();
    sv->SetSize(foundation::Float2{100.0f, 100.0f});
    CHECK(sv->ClipsChildren());
}

TEST_CASE("scrollview: wheel bubbles from hovered content and scrolls")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{200.0f, 200.0f});
    auto sv = Make<ScrollView>();
    sv->SetSize(foundation::Float2{100.0f, 100.0f});
    sv->SetContentSize(foundation::Float2{100.0f, 300.0f});
    root->AddChild(sv.Get());

    // A child inside the content (so the wheel lands on it, not the ScrollView).
    auto item = Cell(100.0f, 40.0f);
    sv->GetContent()->AddChild(item.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    // Wheel down (negative y) over the item -> ScrollView scrolls down by wheelSpeed.
    sv->SetWheelSpeed(30.0f);
    d->InjectMouseWheel(foundation::Float2{20.0f, 20.0f}, foundation::Float2{0.0f, -1.0f});
    CHECK(sv->GetScrollOffset().y == doctest::Approx(30.0f));

    // Wheel up brings it back.
    d->InjectMouseWheel(foundation::Float2{20.0f, 20.0f}, foundation::Float2{0.0f, 1.0f});
    CHECK(sv->GetScrollOffset().y == doctest::Approx(0.0f));
}

TEST_CASE("scrollview: scroll fraction round-trips with SetVerticalFraction")
{
    auto sv = Make<ScrollView>();
    sv->SetSize(foundation::Float2{100.0f, 100.0f});
    sv->SetContentSize(foundation::Float2{100.0f, 300.0f}); // range 200

    sv->SetVerticalFraction(0.5f);
    CHECK(sv->GetScrollOffset().y == doctest::Approx(100.0f));
    CHECK(sv->GetScrollFraction().y == doctest::Approx(0.5f));
    sv->SetVerticalFraction(1.0f);
    CHECK(sv->GetScrollOffset().y == doctest::Approx(200.0f));
}

TEST_CASE("scrollbar: value clamps and notifies; thumb proportion clamps")
{
    auto sb = Make<ScrollBar>();
    int changes = 0;
    float last = -1.0f;
    sb->SetOnValueChanged(
        [&](float v)
        {
            ++changes;
            last = v;
        });

    sb->SetValue(0.4f);
    CHECK(sb->GetValue() == doctest::Approx(0.4f));
    CHECK(changes == 1);
    CHECK(last == doctest::Approx(0.4f));
    sb->SetValue(5.0f);
    CHECK(sb->GetValue() == doctest::Approx(1.0f));

    sb->SetThumbProportion(2.0f);
    CHECK(sb->GetThumbProportion() == doctest::Approx(1.0f));
}

TEST_CASE("scrollbar: drag keeps tracking after the cursor leaves it (capture + hover leave)")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{200.0f, 400.0f});
    auto sb = Make<ScrollBar>();
    sb->SetOrientation(Orientation::Vertical);
    sb->SetSize(foundation::Float2{16.0f, 200.0f}); // vertical track length 200
    sb->SetThumbProportion(0.5f);             // thumb 100 -> travel 100
    root->AddChild(sb.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    // Hover first (so a leave will fire when we drag off), then press near the top.
    d->InjectMouseMove(foundation::Float2{8.0f, 10.0f});
    CHECK(sb->IsHovered());
    d->InjectMouseDown(foundation::Float2{8.0f, 10.0f}, MouseButton::Left);

    // Drag well past the bottom edge -> value saturates at 1 (still tracking after leave).
    d->InjectMouseMove(foundation::Float2{8.0f, 500.0f});
    CHECK_FALSE(sb->IsHovered());
    CHECK(sb->GetValue() == doctest::Approx(1.0f));

    // Release ends the drag.
    d->InjectMouseUp(foundation::Float2{8.0f, 500.0f}, MouseButton::Left);
    d->InjectMouseMove(foundation::Float2{8.0f, 10.0f});
    CHECK(sb->GetValue() == doctest::Approx(1.0f));
}

TEST_CASE("scrollbar + scrollview compose: bar value drives the view")
{
    auto sv = Make<ScrollView>();
    sv->SetSize(foundation::Float2{100.0f, 100.0f});
    sv->SetContentSize(foundation::Float2{100.0f, 300.0f}); // range 200

    auto sb = Make<ScrollBar>();
    ScrollView* view = sv.Get();
    sb->SetOnValueChanged([view](float v) { view->SetVerticalFraction(v); });

    sb->SetValue(0.25f);
    CHECK(sv->GetScrollOffset().y == doctest::Approx(50.0f)); // 0.25 * 200
    sb->SetValue(1.0f);
    CHECK(sv->GetScrollOffset().y == doctest::Approx(200.0f));
}

TEST_CASE("scrollbar: draws track + thumb")
{
    auto sb = Make<ScrollBar>();
    sb->SetSize(foundation::Float2{16.0f, 120.0f});
    sb->SetValue(0.5f);

    vg::VGContext ctx;
    DrawContext dc{ctx};
    sb->Draw(dc);
    CHECK(ctx.GetBatch().vertices.Size() > 0);
}

TEST_CASE("scrollview: internal scrollbar shows/hides by policy")
{
    auto sv = Make<ScrollView>();
    sv->SetSize(foundation::Float2{100.0f, 100.0f});

    // Auto: hidden when content fits, shown when it overflows.
    sv->SetContentSize(foundation::Float2{100.0f, 50.0f});
    CHECK_FALSE(sv->GetVerticalScrollBar()->IsVisible());
    sv->SetContentSize(foundation::Float2{100.0f, 300.0f});
    CHECK(sv->GetVerticalScrollBar()->IsVisible());

    // AlwaysOff hides even when overflowing; AlwaysOn shows even when it fits.
    sv->SetVerticalScrollBarPolicy(ScrollBarPolicy::AlwaysOff);
    CHECK_FALSE(sv->GetVerticalScrollBar()->IsVisible());
    sv->SetVerticalScrollBarPolicy(ScrollBarPolicy::AlwaysOn);
    sv->SetContentSize(foundation::Float2{100.0f, 50.0f});
    CHECK(sv->GetVerticalScrollBar()->IsVisible());
}

TEST_CASE("scrollview: internal bar and offset stay in two-way sync")
{
    auto sv = Make<ScrollView>();
    sv->SetSize(foundation::Float2{100.0f, 100.0f});
    sv->SetContentSize(foundation::Float2{100.0f, 300.0f}); // range 200

    // Scrolling the view updates the bar's value.
    sv->SetScrollOffset(foundation::Float2{0.0f, 50.0f});
    CHECK(sv->GetVerticalScrollBar()->GetValue() == doctest::Approx(0.25f)); // 50/200

    // Setting the bar's value scrolls the view (the wired OnValueChanged callback).
    sv->GetVerticalScrollBar()->SetValue(0.5f);
    CHECK(sv->GetScrollOffset().y == doctest::Approx(100.0f)); // 0.5 * 200
}

TEST_CASE("scrollview: thumb proportion reflects the visible fraction")
{
    auto sv = Make<ScrollView>();
    sv->SetSize(foundation::Float2{100.0f, 100.0f});
    sv->SetContentSize(foundation::Float2{100.0f, 400.0f}); // viewport/content = 100/400 = 0.25
    CHECK(sv->GetVerticalScrollBar()->GetThumbProportion() == doctest::Approx(0.25f));
}

TEST_CASE("scrollview: auto-measures content from its children")
{
    auto sv = Make<ScrollView>();
    sv->SetSize(foundation::Float2{100.0f, 100.0f});
    sv->SetAutoMeasureContent(true);

    auto item = Cell(80.0f, 250.0f);
    sv->GetContent()->AddChild(item.Get()); // OnChildrenChanged -> MeasureContent
    CHECK(sv->GetContentSize().y == doctest::Approx(250.0f));
    CHECK(sv->ScrollRange().y == doctest::Approx(150.0f)); // 250 - 100
}

TEST_CASE("scrollview: keyboard scrolls when focused")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{200.0f, 200.0f});
    auto sv = Make<ScrollView>();
    sv->SetSize(foundation::Float2{100.0f, 100.0f});
    sv->SetContentSize(foundation::Float2{100.0f, 300.0f}); // range 200, viewport 100
    sv->SetLineStep(20.0f);
    root->AddChild(sv.Get());
    EventDispatcher* d = root->GetEventDispatcher();
    sv->RequestFocus();

    d->InjectKeyDown(static_cast<foundation::u32>(KeyCode::Down));
    CHECK(sv->GetScrollOffset().y == doctest::Approx(20.0f));
    d->InjectKeyDown(static_cast<foundation::u32>(KeyCode::PageDown));
    CHECK(sv->GetScrollOffset().y == doctest::Approx(120.0f)); // +viewport(100)
    d->InjectKeyDown(static_cast<foundation::u32>(KeyCode::End));
    CHECK(sv->GetScrollOffset().y == doctest::Approx(200.0f)); // clamped to range
    d->InjectKeyDown(static_cast<foundation::u32>(KeyCode::Home));
    CHECK(sv->GetScrollOffset().y == doctest::Approx(0.0f));
}

TEST_CASE("scrollbar: clicking the track pages toward the click")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{200.0f, 400.0f});
    auto sb = Make<ScrollBar>();
    sb->SetOrientation(Orientation::Vertical);
    sb->SetSize(foundation::Float2{16.0f, 200.0f});
    sb->SetThumbProportion(0.25f); // thumb 50 -> at value 0 occupies [0,50]
    root->AddChild(sb.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    // Click below the thumb (y=150) -> page down by one proportion (0.25).
    d->InjectMouseDown(foundation::Float2{8.0f, 150.0f}, MouseButton::Left);
    CHECK(sb->GetValue() == doctest::Approx(0.25f));
    d->InjectMouseUp(foundation::Float2{8.0f, 150.0f}, MouseButton::Left);
}

TEST_CASE("scrollbar: grabbing the thumb drags by cursor delta (no jump-to-center)")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{200.0f, 400.0f});
    auto sb = Make<ScrollBar>();
    sb->SetOrientation(Orientation::Vertical);
    sb->SetSize(foundation::Float2{16.0f, 200.0f});
    sb->SetThumbProportion(0.5f); // thumb 100 -> travel 100, at value 0 occupies [0,100]
    root->AddChild(sb.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    // Grab the thumb at y=40 (inside [0,100]); pressing there must NOT move the value.
    d->InjectMouseDown(foundation::Float2{8.0f, 40.0f}, MouseButton::Left);
    CHECK(sb->GetValue() == doctest::Approx(0.0f));
    // Move the cursor down 20px -> thumb moves 20px -> value 20/100 = 0.2.
    d->InjectMouseMove(foundation::Float2{8.0f, 60.0f});
    CHECK(sb->GetValue() == doctest::Approx(0.2f));
}
