// Draconic GUI - Window tests: title-bar drag moves the window, the grip resizes it (clamped
// to the minimum), title clicks fall through to the draggable bar, and a press raises the
// window to the front.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
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

TEST_CASE("window: dragging the title bar moves the window")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{600.0f, 400.0f});
    auto win = Make<Window>();
    win->SetSize(foundation::Float2{200.0f, 150.0f});
    win->SetPosition(foundation::Float2{50.0f, 50.0f});
    win->SetTitleBarHeight(28.0f);
    root->AddChild(win.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    // Press on the title bar (world y ~ 60, within [50,78)), then drag by (+40,+30).
    d->InjectMouseDown(foundation::Float2{120.0f, 60.0f}, MouseButton::Left);
    d->InjectMouseMove(foundation::Float2{160.0f, 90.0f});
    CHECK(win->GetPosition().x == doctest::Approx(90.0f));
    CHECK(win->GetPosition().y == doctest::Approx(80.0f));

    // Continue dragging; deltas accumulate.
    d->InjectMouseMove(foundation::Float2{170.0f, 90.0f});
    CHECK(win->GetPosition().x == doctest::Approx(100.0f));
    d->InjectMouseUp(foundation::Float2{170.0f, 90.0f}, MouseButton::Left);
    d->InjectMouseMove(foundation::Float2{300.0f, 300.0f}); // no longer dragging
    CHECK(win->GetPosition().x == doctest::Approx(100.0f));
}

TEST_CASE("window: title clicks fall through the caption to the draggable bar")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{600.0f, 400.0f});
    auto win = Make<Window>();
    win->SetSize(foundation::Float2{200.0f, 150.0f});
    win->SetPosition(foundation::Float2{0.0f, 0.0f});
    win->SetTitle(foundation::StringView(u8"Hello"));
    root->AddChild(win.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    // Press over where the caption sits (top-left of the title bar) and drag; the hit-test-
    // transparent Label must let the bar receive it, so the window still moves.
    d->InjectMouseDown(foundation::Float2{20.0f, 12.0f}, MouseButton::Left);
    d->InjectMouseMove(foundation::Float2{45.0f, 12.0f});
    CHECK(win->GetPosition().x == doctest::Approx(25.0f));
}

TEST_CASE("window: dragging the grip resizes, clamped to the minimum")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{600.0f, 400.0f});
    auto win = Make<Window>();
    win->SetSize(foundation::Float2{200.0f, 150.0f});
    win->SetPosition(foundation::Float2{0.0f, 0.0f});
    win->SetMinSize(foundation::Float2{120.0f, 80.0f});
    root->AddChild(win.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    // Grip is a 14x14 square at the bottom-right: world ~ [186,200) x [136,150).
    d->InjectMouseDown(foundation::Float2{193.0f, 143.0f}, MouseButton::Left);
    d->InjectMouseMove(foundation::Float2{243.0f, 193.0f}); // +50, +50
    CHECK(win->GetSize().x == doctest::Approx(250.0f));
    CHECK(win->GetSize().y == doctest::Approx(200.0f));

    // Shrink far past the minimum -> clamps.
    d->InjectMouseMove(foundation::Float2{0.0f, 0.0f});
    CHECK(win->GetSize().x == doctest::Approx(120.0f));
    CHECK(win->GetSize().y == doctest::Approx(80.0f));
}

TEST_CASE("window: content host is placed below the title bar and add-able")
{
    auto win = Make<Window>();
    win->SetSize(foundation::Float2{200.0f, 150.0f});
    win->SetTitleBarHeight(30.0f);

    Node* host = win->GetContent();
    REQUIRE(host != nullptr);
    CHECK(host->GetSize().x == doctest::Approx(200.0f));
    CHECK(host->GetSize().y == doctest::Approx(120.0f)); // 150 - 30

    auto child = Make<UIWidget>();
    child->SetSize(foundation::Float2{40.0f, 40.0f});
    host->AddChild(child.Get());
    CHECK(host->ChildCount() == 1);
}

TEST_CASE("window: pressing the title raises the window to the front")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{600.0f, 400.0f});
    auto a = Make<Window>();
    auto b = Make<Window>();
    a->SetSize(foundation::Float2{200.0f, 150.0f});
    b->SetSize(foundation::Float2{200.0f, 150.0f});
    a->SetPosition(foundation::Float2{0.0f, 0.0f});
    b->SetPosition(foundation::Float2{300.0f, 0.0f}); // separate, so the click hits only a
    root->AddChild(a.Get());
    root->AddChild(b.Get()); // b is on top (last child)
    CHECK(root->GetChildAt(1) == b.Get());

    EventDispatcher* d = root->GetEventDispatcher();
    d->InjectMouseDown(foundation::Float2{20.0f, 12.0f}, MouseButton::Left); // press a's title
    CHECK(root->GetChildAt(1) == a.Get());                             // a raised to front
    d->InjectMouseUp(foundation::Float2{20.0f, 12.0f}, MouseButton::Left);
}

TEST_CASE("window: the right button does not drag the window")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{600.0f, 400.0f});
    auto win = Make<Window>();
    win->SetSize(foundation::Float2{200.0f, 150.0f});
    win->SetPosition(foundation::Float2{50.0f, 50.0f});
    root->AddChild(win.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    d->InjectMouseDown(foundation::Float2{120.0f, 60.0f}, MouseButton::Right); // right press on title
    d->InjectMouseMove(foundation::Float2{200.0f, 120.0f});
    CHECK(win->GetPosition().x == doctest::Approx(50.0f)); // did not move
    CHECK(win->GetPosition().y == doctest::Approx(50.0f));
}
