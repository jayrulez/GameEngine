// Draconic GUI - EventDispatcher tests: hover enter/leave, click, focus (click + program-
// matic), key/text routing to the focus node, and interaction-ref cleanup. Input is
// injected as abstract events (the shell bridge's job), hit-tested via the tree's OverFind.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.gui;

using namespace draconic::gui;
namespace foundation = draconic::foundation;

namespace
{
    foundation::RefPtr<SceneNode> MakeScene(foundation::Float2 size)
    {
        auto s = foundation::MakeRef<SceneNode>(foundation::DefaultAllocator());
        s->SetSize(size);
        return s;
    }
    foundation::RefPtr<Node> MakePanel(foundation::Float2 pos, foundation::Float2 size)
    {
        auto n = foundation::MakeRef<Node>(foundation::DefaultAllocator());
        n->SetSize(size);
        n->SetPosition(pos);
        return n;
    }
}

TEST_CASE("dispatch: hover enter/leave tracks the node under the cursor")
{
    auto root = MakeScene(foundation::Float2{200.0f, 200.0f});
    auto child = MakePanel(foundation::Float2{10.0f, 10.0f}, foundation::Float2{50.0f, 50.0f});
    root->AddChild(child.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    int enter = 0, leave = 0;
    child->AddEventListener(EventType::MouseEnter, [&](const Event&) { ++enter; });
    child->AddEventListener(EventType::MouseLeave, [&](const Event&) { ++leave; });

    d->InjectMouseMove(foundation::Float2{20.0f, 20.0f}); // over child
    CHECK(d->GetOverNode() == child.Get());
    CHECK(enter == 1);
    CHECK(leave == 0);

    d->InjectMouseMove(foundation::Float2{120.0f, 120.0f}); // off child, over root
    CHECK(d->GetOverNode() == root.Get());
    CHECK(leave == 1);
}

TEST_CASE("dispatch: press+release on the same node is a click and focuses it")
{
    auto root = MakeScene(foundation::Float2{200.0f, 200.0f});
    auto child = MakePanel(foundation::Float2{10.0f, 10.0f}, foundation::Float2{50.0f, 50.0f});
    root->AddChild(child.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    int clicks = 0;
    child->AddEventListener(EventType::MouseClick, [&](const Event&) { ++clicks; });

    d->InjectMouseDown(foundation::Float2{20.0f, 20.0f}, MouseButton::Left);
    d->InjectMouseUp(foundation::Float2{20.0f, 20.0f}, MouseButton::Left);
    CHECK(clicks == 1);
    CHECK(d->GetFocusNode() == child.Get());
    CHECK(child->IsFocused());
}

TEST_CASE("dispatch: mouse event payload is accessible via static_cast")
{
    auto root = MakeScene(foundation::Float2{200.0f, 200.0f});
    EventDispatcher* d = root->GetEventDispatcher();

    foundation::Float2 seen{-1.0f, -1.0f};
    root->AddEventListener(EventType::MouseMove, [&](const Event& e)
                           { seen = static_cast<const MouseEvent&>(e).Position; });

    d->InjectMouseMove(foundation::Float2{33.0f, 44.0f});
    CHECK(seen.x == doctest::Approx(33.0f));
    CHECK(seen.y == doctest::Approx(44.0f));
}

TEST_CASE("dispatch: key and text route to the focus node")
{
    auto root = MakeScene(foundation::Float2{200.0f, 200.0f});
    auto child = MakePanel(foundation::Float2{0.0f, 0.0f}, foundation::Float2{50.0f, 50.0f});
    root->AddChild(child.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    int keys = 0, texts = 0;
    foundation::StringView lastText;
    child->AddEventListener(EventType::KeyDown, [&](const Event&) { ++keys; });
    child->AddEventListener(EventType::TextInput,
                            [&](const Event& e)
                            {
                                ++texts;
                                lastText = static_cast<const TextInputEvent&>(e).Text;
                            });

    d->InjectKeyDown(65); // no focus yet -> dropped
    CHECK(keys == 0);

    d->SetFocusNode(child.Get());
    d->InjectKeyDown(65);
    d->InjectText(foundation::StringView(u8"hi"));
    CHECK(keys == 1);
    CHECK(texts == 1);
    CHECK(lastText == foundation::StringView(u8"hi"));
}

TEST_CASE("dispatch: programmatic focus gains and releases")
{
    auto root = MakeScene(foundation::Float2{200.0f, 200.0f});
    auto a = MakePanel(foundation::Float2{0.0f, 0.0f}, foundation::Float2{50.0f, 50.0f});
    auto b = MakePanel(foundation::Float2{60.0f, 0.0f}, foundation::Float2{50.0f, 50.0f});
    root->AddChild(a.Get());
    root->AddChild(b.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    a->RequestFocus();
    CHECK(d->GetFocusNode() == a.Get());
    CHECK(a->IsFocused());

    b->RequestFocus(); // moves focus: a loses, b gains
    CHECK(d->GetFocusNode() == b.Get());
    CHECK_FALSE(a->IsFocused());
    CHECK(b->IsFocused());

    b->ReleaseFocus();
    CHECK(d->GetFocusNode() == nullptr);
    CHECK_FALSE(b->IsFocused());
}

TEST_CASE("dispatch: NotifyNodeRemoved clears interaction refs")
{
    auto root = MakeScene(foundation::Float2{200.0f, 200.0f});
    auto child = MakePanel(foundation::Float2{0.0f, 0.0f}, foundation::Float2{50.0f, 50.0f});
    root->AddChild(child.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    d->InjectMouseMove(foundation::Float2{10.0f, 10.0f});
    d->SetFocusNode(child.Get());
    CHECK(d->GetOverNode() == child.Get());
    CHECK(d->GetFocusNode() == child.Get());

    d->NotifyNodeRemoved(child.Get());
    CHECK(d->GetOverNode() == nullptr);
    CHECK(d->GetFocusNode() == nullptr);
}

TEST_CASE("dispatch: Tab navigation cycles through tab-focusable nodes in order")
{
    auto root = MakeScene(foundation::Float2{200.0f, 200.0f});
    auto a = MakePanel(foundation::Float2{0.0f, 0.0f}, foundation::Float2{50.0f, 20.0f});
    auto skip = MakePanel(foundation::Float2{0.0f, 20.0f}, foundation::Float2{50.0f, 20.0f}); // NOT focusable
    auto b = MakePanel(foundation::Float2{0.0f, 40.0f}, foundation::Float2{50.0f, 20.0f});
    auto c = MakePanel(foundation::Float2{0.0f, 60.0f}, foundation::Float2{50.0f, 20.0f});
    a->SetTabFocusable(true);
    b->SetTabFocusable(true);
    c->SetTabFocusable(true);
    root->AddChild(a.Get());
    root->AddChild(skip.Get());
    root->AddChild(b.Get());
    root->AddChild(c.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    // Nothing focused -> Tab lands on the first stop.
    CHECK(d->FocusNext());
    CHECK(d->GetFocusNode() == a.Get());
    // Tab skips the non-focusable node.
    CHECK(d->FocusNext());
    CHECK(d->GetFocusNode() == b.Get());
    CHECK(d->FocusNext());
    CHECK(d->GetFocusNode() == c.Get());
    // Wraps back to the first.
    CHECK(d->FocusNext());
    CHECK(d->GetFocusNode() == a.Get());
    // Shift+Tab goes backwards (wraps to the last).
    CHECK(d->FocusPrevious());
    CHECK(d->GetFocusNode() == c.Get());
}

TEST_CASE("dispatch: the Tab key drives focus traversal (Shift+Tab reverses)")
{
    auto root = MakeScene(foundation::Float2{200.0f, 200.0f});
    auto a = MakePanel(foundation::Float2{0.0f, 0.0f}, foundation::Float2{50.0f, 20.0f});
    auto b = MakePanel(foundation::Float2{0.0f, 40.0f}, foundation::Float2{50.0f, 20.0f});
    a->SetTabFocusable(true);
    b->SetTabFocusable(true);
    root->AddChild(a.Get());
    root->AddChild(b.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    d->InjectKeyDown(static_cast<foundation::u32>(KeyCode::Tab));
    CHECK(d->GetFocusNode() == a.Get());
    d->InjectKeyDown(static_cast<foundation::u32>(KeyCode::Tab));
    CHECK(d->GetFocusNode() == b.Get());
    d->InjectKeyDown(static_cast<foundation::u32>(KeyCode::Tab), KeyModShift);
    CHECK(d->GetFocusNode() == a.Get());
}

TEST_CASE("dispatch: Tab is consumed (not routed) but other keys still reach the focus node")
{
    auto root = MakeScene(foundation::Float2{200.0f, 200.0f});
    auto a = MakePanel(foundation::Float2{0.0f, 0.0f}, foundation::Float2{50.0f, 20.0f});
    a->SetTabFocusable(true);
    root->AddChild(a.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    int tabKeys = 0, otherKeys = 0;
    a->AddEventListener(EventType::KeyDown,
                        [&](const Event& e)
                        {
                            if (static_cast<const KeyEvent&>(e).KeyCode ==
                                static_cast<foundation::u32>(KeyCode::Tab))
                                ++tabKeys;
                            else
                                ++otherKeys;
                        });

    d->InjectKeyDown(static_cast<foundation::u32>(KeyCode::Tab)); // focuses a, consumed
    CHECK(d->GetFocusNode() == a.Get());
    CHECK(tabKeys == 0);                                     // the widget never saw the Tab
    d->InjectKeyDown(static_cast<foundation::u32>(KeyCode::Home)); // a real key routes through
    CHECK(otherKeys == 1);
}

TEST_CASE("dispatch: only the left button synthesizes a click (right/middle do not activate)")
{
    auto root = MakeScene(foundation::Float2{200.0f, 200.0f});
    auto child = MakePanel(foundation::Float2{0.0f, 0.0f}, foundation::Float2{100.0f, 100.0f});
    root->AddChild(child.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    int clicks = 0, downs = 0;
    child->AddEventListener(EventType::MouseClick, [&](const Event&) { ++clicks; });
    child->AddEventListener(EventType::MouseDown, [&](const Event&) { ++downs; });

    // Right-click: down+up are delivered, but no click (no activation).
    d->InjectMouseDown(foundation::Float2{10.0f, 10.0f}, MouseButton::Right);
    d->InjectMouseUp(foundation::Float2{10.0f, 10.0f}, MouseButton::Right);
    CHECK(downs == 1);
    CHECK(clicks == 0);

    // Left-click activates.
    d->InjectMouseDown(foundation::Float2{10.0f, 10.0f}, MouseButton::Left);
    d->InjectMouseUp(foundation::Float2{10.0f, 10.0f}, MouseButton::Left);
    CHECK(clicks == 1);
}
