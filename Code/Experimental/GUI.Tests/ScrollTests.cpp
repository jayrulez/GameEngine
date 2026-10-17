// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - scrolling: the ScrollBar control, and any node that scrolls (overflow: scroll or auto, E2
// of Specs/gui-lab-evolution.md): its range over the laid-out content, the content moving without a
// new layout, the wheel, the keys while focused, its bars (drag, page, over the content), a focused
// descendant brought into view, and the extent following the content.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.vg;
import experimental.gui;

using namespace experimental::gui;
namespace core = foundation::core;
namespace vg = foundation::vg;
using core::f32;

namespace
{
    template <typename T>
    core::RefPtr<T> Make()
    {
        return core::MakeRef<T>(core::DefaultAllocator());
    }

    // A 100 x 100 scroller at the root's corner holding `rows` 50-high rows.
    struct Scroller
    {
        core::RefPtr<SceneRoot> root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
        core::RefPtr<ScrollView> view = Make<ScrollView>();
        core::Array<core::RefPtr<Panel>> rows;
        explicit Scroller(int count = 6)
        {
            root->SetSize(core::Float2{300.0f, 300.0f});
            view->SetPosition(core::Float2{0.0f, 0.0f});
            view->SetSize(core::Float2{100.0f, 100.0f});
            root->AddChild(view.Get());
            for (int i = 0; i < count; ++i)
            {
                rows.PushBack(Make<Panel>());
                rows[rows.Size() - 1]->EditLayoutStyle([](LayoutStyle& s) { s.Height = LayoutLength::Px(50.0f); });
                rows[rows.Size() - 1]->SetTabFocusable(true);
                view->AddChild(rows[rows.Size() - 1].Get());
            }
            root->UpdateLayout();
        }
        EventDispatcher& D() { return *root->GetEventDispatcher(); }
    };
}

TEST_CASE("scrollbar: its value is an offset, kept within the range less the page; the thumb is the page's share")
{
    auto sb = Make<ScrollBar>();
    sb->SetMax(400.0);
    sb->SetPage(100.0); // a quarter shows
    CHECK(sb->GetThumbProportion() == doctest::Approx(0.25f));
    sb->SetValue(120.0);
    CHECK(sb->GetValue() == doctest::Approx(120.0));
    sb->SetValue(500.0);
    CHECK(sb->GetValue() == doctest::Approx(300.0)); // the maximum less the page
    sb->SetPage(1000.0);
    CHECK(sb->GetThumbProportion() == doctest::Approx(1.0f));
    CHECK(sb->GetValue() == doctest::Approx(0.0)); // nothing to scroll
}

TEST_CASE("scrollbar: drag keeps tracking after the cursor leaves it (capture + hover leave)")
{
    auto root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
    root->SetSize(core::Float2{200.0f, 400.0f});
    auto sb = Make<ScrollBar>();
    sb->SetOrientation(Orientation::Vertical);
    sb->SetSize(core::Float2{16.0f, 200.0f}); // vertical track length 200
    sb->SetMax(200.0);
    sb->SetPage(100.0);                       // thumb 100 -> travel 100
    int inputs = 0, changes = 0;
    sb->SetOnValueChanging([&](core::f64) { ++inputs; });
    sb->SetOnValueChanged([&](core::f64) { ++changes; });
    root->AddChild(sb.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    // Hover first (so a leave will fire when we drag off), then press near the top.
    d->InjectMouseMove(core::Float2{8.0f, 10.0f});
    CHECK(sb->IsHovered());
    d->InjectMouseDown(core::Float2{8.0f, 10.0f}, MouseButton::Left);

    // Drag well past the bottom edge -> the value saturates at the end (still tracking after leave).
    d->InjectMouseMove(core::Float2{8.0f, 500.0f});
    CHECK_FALSE(sb->IsHovered());
    CHECK(sb->GetValue() == doctest::Approx(100.0));
    CHECK(inputs == 1);
    CHECK(changes == 0); // committed on release

    // Release ends the drag.
    d->InjectMouseUp(core::Float2{8.0f, 500.0f}, MouseButton::Left);
    CHECK(changes == 1);
    d->InjectMouseMove(core::Float2{8.0f, 10.0f});
    CHECK(sb->GetValue() == doctest::Approx(100.0));
}

TEST_CASE("scrollbar: draws track + thumb")
{
    auto sb = Make<ScrollBar>();
    sb->SetSize(core::Float2{16.0f, 120.0f});
    sb->SetMax(100.0);
    sb->SetPage(30.0);
    sb->SetValue(35.0);

    vg::VGContext ctx;
    DrawContext dc{ctx};
    sb->Draw(dc);
    CHECK(ctx.GetBatch().vertices.Size() > 0);
}

TEST_CASE("scrollbar: clicking the track pages toward the click")
{
    auto root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
    root->SetSize(core::Float2{200.0f, 400.0f});
    auto sb = Make<ScrollBar>();
    sb->SetOrientation(Orientation::Vertical);
    sb->SetSize(core::Float2{16.0f, 200.0f});
    sb->SetMax(200.0);
    sb->SetPage(50.0); // thumb 50 -> at value 0 occupies [0,50]
    root->AddChild(sb.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    // Click below the thumb (y=150) -> page down by one page.
    d->InjectMouseDown(core::Float2{8.0f, 150.0f}, MouseButton::Left);
    CHECK(sb->GetValue() == doctest::Approx(50.0));
    d->InjectMouseUp(core::Float2{8.0f, 150.0f}, MouseButton::Left);
}

TEST_CASE("scrollbar: grabbing the thumb drags by cursor delta (no jump-to-center)")
{
    auto root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
    root->SetSize(core::Float2{200.0f, 400.0f});
    auto sb = Make<ScrollBar>();
    sb->SetOrientation(Orientation::Vertical);
    sb->SetSize(core::Float2{16.0f, 200.0f});
    sb->SetMax(200.0);
    sb->SetPage(100.0); // thumb 100 -> travel 100, at value 0 occupies [0,100]
    root->AddChild(sb.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    // Grab the thumb at y=40 (inside [0,100]); pressing there must NOT move the value.
    d->InjectMouseDown(core::Float2{8.0f, 40.0f}, MouseButton::Left);
    CHECK(sb->GetValue() == doctest::Approx(0.0));
    // Move the cursor down 20px -> thumb moves 20px -> a fifth of the travel: 20 of the 100 it can go.
    d->InjectMouseMove(core::Float2{8.0f, 60.0f});
    CHECK(sb->GetValue() == doctest::Approx(20.0));
}

TEST_CASE("scroll: a node that overflows scrolls over its laid-out content, the content moving with no layout")
{
    Scroller s;
    CHECK(s.view->IsScrollContainer());
    CHECK(s.view->GetContentExtent().y == doctest::Approx(300.0f));
    CHECK(s.view->GetScrollRange().x == doctest::Approx(0.0f)); // nothing wider than it
    CHECK(s.view->GetScrollRange().y == doctest::Approx(200.0f));
    s.view->SetScrollOffset(core::Float2{0.0f, 500.0f});
    CHECK(s.view->GetScrollOffset().y == doctest::Approx(200.0f));
    s.view->SetScrollOffset(core::Float2{0.0f, 60.0f});
    s.root->UpdateLayout();
    CHECK(s.root->LastLayoutCount() == 0u);                         // scrolling lays nothing out
    CHECK(s.rows[2]->GetPosition().y == doctest::Approx(100.0f));     // where the layout put it
    CHECK(s.rows[2]->GetScreenBounds().y == doctest::Approx(40.0f));  // where it shows
    CHECK(s.root->GetEventDispatcher()->IsPointerOver(core::Float2{10.0f, 45.0f}));
}

TEST_CASE("scroll: the wheel scrolls it from the content; keys scroll it only while it is focused")
{
    Scroller s;
    CHECK(s.D().InjectMouseWheel(core::Float2{20.0f, 20.0f}, core::Float2{0.0f, -1.0f}));
    CHECK(s.view->GetScrollOffset().y == doctest::Approx(s.view->GetScrollStep()));
    s.D().SetFocusNode(s.rows[0].Get());
    const f32 before = s.view->GetScrollOffset().y;
    s.D().InjectKeyDown(static_cast<core::u32>(KeyCode::PageDown));
    CHECK(s.view->GetScrollOffset().y == doctest::Approx(before)); // the row's key, left to navigation
    s.D().SetFocusNode(s.view.Get());
    CHECK(s.D().InjectKeyDown(static_cast<core::u32>(KeyCode::End)));
    CHECK(s.view->GetScrollOffset().y == doctest::Approx(200.0f));
    CHECK(s.view->GetScrollOffset().y == doctest::Approx(200.0f)); // already at the end
}

TEST_CASE("scroll: its bar lies over the content; the thumb drags, the track pages")
{
    Scroller s;
    const Rect track = s.view->ScrollBarTrack(1);
    REQUIRE(track.height > 0.0f);
    CHECK(track.x == doctest::Approx(92.0f)); // over the right edge, on top of the rows
    const Rect thumb = s.view->ScrollBarThumb(1);
    CHECK(thumb.height == doctest::Approx(100.0f * 100.0f / 300.0f));
    int rowPresses = 0;
    (void)s.rows[0]->AddEventListener(EventType::MouseDown, [&](const Event&) { ++rowPresses; });
    const core::Float2 grab{96.0f, thumb.y + 5.0f};
    s.D().InjectMouseDown(grab, MouseButton::Left);
    CHECK(rowPresses == 0); // the bar's, not the row's under it
    CHECK(s.D().GetPointerCapture() == s.view.Get());
    s.D().InjectMouseMove(core::Float2{96.0f, grab.y + (100.0f - thumb.height) * 0.5f});
    CHECK(s.view->GetScrollOffset().y == doctest::Approx(100.0f)); // half the travel: half the range
    s.D().InjectMouseUp(core::Float2{96.0f, 90.0f}, MouseButton::Left);
    s.view->SetScrollOffset(core::Float2{0.0f, 0.0f});
    s.D().InjectMouseDown(core::Float2{96.0f, 90.0f}, MouseButton::Left); // the track below the thumb
    CHECK(s.view->GetScrollOffset().y == doctest::Approx(100.0f));        // a page down
    s.D().InjectMouseUp(core::Float2{96.0f, 90.0f}, MouseButton::Left);
}

TEST_CASE("scroll: a focused descendant comes into view, by the least that shows it")
{
    Scroller s;
    s.D().SetFocusNode(s.rows[4].Get()); // y 200..250
    CHECK(s.view->GetScrollOffset().y == doctest::Approx(150.0f));
    s.D().SetFocusNode(s.rows[1].Get()); // y 50..100: above the view now
    CHECK(s.view->GetScrollOffset().y == doctest::Approx(50.0f));
}

TEST_CASE("scroll: the extent follows the content as it grows and shrinks; the offset stays in range")
{
    Scroller s(2);
    CHECK(s.view->GetScrollRange().y == doctest::Approx(0.0f));
    s.rows[1]->EditLayoutStyle([](LayoutStyle& st) { st.Height = LayoutLength::Px(250.0f); }); // a child styled taller later
    s.root->UpdateLayout();
    CHECK(s.view->GetScrollRange().y == doctest::Approx(200.0f));
    s.view->SetScrollOffset(core::Float2{0.0f, 200.0f});
    s.rows[1]->SetDisplayed(false);
    s.root->UpdateLayout();
    CHECK(s.view->GetScrollOffset().y == doctest::Approx(0.0f));
}

TEST_CASE("scroll: overflow from a sheet makes any node scroll; hidden clips without scrolling")
{
    auto root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
    root->SetSize(core::Float2{300.0f, 300.0f});
    auto panel = Make<Panel>();
    panel->AddClass(core::StringView(u8"list"));
    root->AddChild(panel.Get());
    auto tall = Make<Panel>();
    tall->EditLayoutStyle([](LayoutStyle& st) { st.Height = LayoutLength::Px(500.0f); st.Shrink = 0.0f; });
    panel->AddChild(tall.Get());
    StyleManager styles(CSSParser::Parse(core::StringView(u8".list { height: 120px; overflow-y: auto; }")));
    styles.ApplyTree(*root.Get());
    root->UpdateLayout();
    CHECK(panel->IsScrollContainer());
    CHECK(panel->ClipsChildren());
    CHECK(panel->GetScrollRange().y == doctest::Approx(380.0f));
    StyleManager hidden(CSSParser::Parse(core::StringView(u8".list { overflow: hidden; }")));
    hidden.ApplyTree(*root.Get());
    root->UpdateLayout();
    CHECK_FALSE(panel->IsScrollContainer());
    CHECK(panel->ClipsChildren());
}
