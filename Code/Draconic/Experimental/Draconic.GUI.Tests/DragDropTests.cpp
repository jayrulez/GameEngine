// Draconic GUI - drag-and-drop tests: a source begins a drag, the dispatcher delivers
// enter/over/leave to the nearest accepting target under the cursor, and Drop on release;
// non-accepting targets are skipped; cancel/removal clean up.
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

    foundation::RefPtr<UIWidget> Zone(SceneNode* root, foundation::Float2 pos, foundation::Float2 size,
                                const char8_t* accept)
    {
        auto z = foundation::MakeRef<UIWidget>(foundation::DefaultAllocator());
        z->SetSize(size);
        z->SetPosition(pos);
        const foundation::String want(accept);
        z->SetDropAcceptor([want](const DragPayload& p)
                           { return p.Type.AsView() == want.AsView(); });
        root->AddChild(z.Get());
        return z;
    }

    DragPayload Payload(const char8_t* type, const char8_t* value)
    {
        DragPayload p;
        p.Type = foundation::String(type);
        p.Value = foundation::String(value);
        return p;
    }
}

TEST_CASE("dnd: drag delivers enter/over then drop on an accepting target")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{300.0f, 300.0f});
    auto target =
        Zone(root.Get(), foundation::Float2{100.0f, 0.0f}, foundation::Float2{100.0f, 100.0f}, u8"item");
    EventDispatcher* d = root->GetEventDispatcher();

    int enters = 0, overs = 0, leaves = 0, drops = 0;
    foundation::String dropped;
    target->AddEventListener(EventType::DragEnter, [&](const Event&) { ++enters; });
    target->AddEventListener(EventType::DragOver, [&](const Event&) { ++overs; });
    target->AddEventListener(EventType::DragLeave, [&](const Event&) { ++leaves; });
    target->AddEventListener(EventType::Drop,
                             [&](const Event& e)
                             {
                                 ++drops;
                                 dropped =
                                     foundation::String(static_cast<const DragEvent&>(e).Payload.Value);
                             });

    // Cursor starts outside the target; begin the drag.
    d->InjectMouseMove(foundation::Float2{10.0f, 10.0f});
    d->BeginDrag(nullptr, Payload(u8"item", u8"apple"));
    CHECK(d->IsDragging());
    CHECK(enters == 0); // not over the target yet

    // Move onto the target -> enter + over.
    d->InjectMouseMove(foundation::Float2{150.0f, 50.0f});
    CHECK(enters == 1);
    CHECK(overs == 1);
    CHECK(d->GetDropTarget() == target.Get());

    // Release over the target -> drop, drag ends.
    d->InjectMouseUp(foundation::Float2{150.0f, 50.0f}, MouseButton::Left);
    CHECK(drops == 1);
    CHECK(dropped.AsView() == foundation::StringView(u8"apple"));
    CHECK_FALSE(d->IsDragging());
}

TEST_CASE("dnd: moving off the target fires leave; a non-matching target is not entered")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{300.0f, 300.0f});
    auto good = Zone(root.Get(), foundation::Float2{0.0f, 0.0f}, foundation::Float2{100.0f, 100.0f}, u8"item");
    auto bad =
        Zone(root.Get(), foundation::Float2{150.0f, 0.0f}, foundation::Float2{100.0f, 100.0f}, u8"other");
    EventDispatcher* d = root->GetEventDispatcher();

    int goodEnter = 0, goodLeave = 0, badEnter = 0;
    good->AddEventListener(EventType::DragEnter, [&](const Event&) { ++goodEnter; });
    good->AddEventListener(EventType::DragLeave, [&](const Event&) { ++goodLeave; });
    bad->AddEventListener(EventType::DragEnter, [&](const Event&) { ++badEnter; });

    d->InjectMouseMove(foundation::Float2{50.0f, 50.0f});
    d->BeginDrag(nullptr, Payload(u8"item", u8"x")); // starts over good -> enter
    CHECK(goodEnter == 1);

    d->InjectMouseMove(foundation::Float2{200.0f, 50.0f}); // over the non-matching zone
    CHECK(goodLeave == 1);                           // left good
    CHECK(badEnter == 0);                            // bad rejects "item" -> no enter
    CHECK(d->GetDropTarget() == nullptr);

    // Dropping over a non-accepting area does nothing and ends the drag.
    int goodDrop = 0;
    good->AddEventListener(EventType::Drop, [&](const Event&) { ++goodDrop; });
    d->InjectMouseUp(foundation::Float2{200.0f, 50.0f}, MouseButton::Left);
    CHECK(goodDrop == 0);
    CHECK_FALSE(d->IsDragging());
}

TEST_CASE("dnd: drop bubbles to the nearest accepting ancestor")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{300.0f, 300.0f});
    auto container =
        Zone(root.Get(), foundation::Float2{0.0f, 0.0f}, foundation::Float2{200.0f, 200.0f}, u8"item");
    // A non-accepting child inside the container (the cursor lands on it).
    auto child = Make<UIWidget>();
    child->SetSize(foundation::Float2{50.0f, 50.0f});
    child->SetPosition(foundation::Float2{20.0f, 20.0f});
    container->AddChild(child.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    int drops = 0;
    container->AddEventListener(EventType::Drop, [&](const Event&) { ++drops; });

    d->InjectMouseMove(foundation::Float2{5.0f, 5.0f});
    d->BeginDrag(nullptr, Payload(u8"item", u8"x"));
    d->InjectMouseMove(foundation::Float2{40.0f, 40.0f}); // over the child (non-accepting)
    CHECK(d->GetDropTarget() == container.Get());   // bubbled to the accepting container
    d->InjectMouseUp(foundation::Float2{40.0f, 40.0f}, MouseButton::Left);
    CHECK(drops == 1);
}

TEST_CASE("dnd: CancelDrag ends the drag and leaves the current target")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{300.0f, 300.0f});
    auto target =
        Zone(root.Get(), foundation::Float2{0.0f, 0.0f}, foundation::Float2{100.0f, 100.0f}, u8"item");
    EventDispatcher* d = root->GetEventDispatcher();

    int leaves = 0;
    target->AddEventListener(EventType::DragLeave, [&](const Event&) { ++leaves; });

    d->InjectMouseMove(foundation::Float2{50.0f, 50.0f});
    d->BeginDrag(nullptr, Payload(u8"item", u8"x"));
    CHECK(d->GetDropTarget() == target.Get());
    d->CancelDrag();
    CHECK(leaves == 1);
    CHECK_FALSE(d->IsDragging());
    CHECK(d->GetDropTarget() == nullptr);
}
