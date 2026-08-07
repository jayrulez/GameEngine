// Draconic GUI - Node tree tests: ownership, reparenting, z-order, geometry, world-space
// conversion, hit testing, visibility, invalidation, and event listeners. Derived from
// eepp Scene::Node behavior, adapted to the RefPtr-owned child tree.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.gui;

using namespace draconic::gui;
namespace foundation = draconic::foundation;

namespace
{
    foundation::RefPtr<Node> MakeNode() { return foundation::MakeRef<Node>(foundation::DefaultAllocator()); }
}

TEST_CASE("node: add child sets parent and count")
{
    auto root = MakeNode();
    auto child = MakeNode();
    root->AddChild(child.Get());

    CHECK(root->ChildCount() == 1);
    CHECK(root->GetChildAt(0) == child.Get());
    CHECK(child->GetParent() == root.Get());
    CHECK(root->HasChild(child.Get()));
}

TEST_CASE("node: reparenting removes from old parent")
{
    auto a = MakeNode();
    auto b = MakeNode();
    auto child = MakeNode();
    a->AddChild(child.Get());
    b->AddChild(child.Get());

    CHECK(a->ChildCount() == 0);
    CHECK(b->ChildCount() == 1);
    CHECK(child->GetParent() == b.Get());
}

TEST_CASE("node: remove child clears parent")
{
    auto root = MakeNode();
    auto child = MakeNode();
    root->AddChild(child.Get());
    root->RemoveChild(child.Get());

    CHECK(root->ChildCount() == 0);
    CHECK(child->GetParent() == nullptr);
}

TEST_CASE("node: child kept alive by tree after external ref drops")
{
    auto root = MakeNode();
    Node* raw = nullptr;
    {
        auto child = MakeNode();
        raw = child.Get();
        root->AddChild(child.Get());
    } // external RefPtr released; tree still owns it
    CHECK(root->ChildCount() == 1);
    CHECK(root->GetChildAt(0) == raw);
    CHECK(raw->GetParent() == root.Get());
}

TEST_CASE("node: z-order and siblings")
{
    auto root = MakeNode();
    auto a = MakeNode();
    auto b = MakeNode();
    auto c = MakeNode();
    root->AddChild(a.Get());
    root->AddChild(b.Get());
    root->AddChild(c.Get());

    CHECK(root->GetFirstChild() == a.Get());
    CHECK(root->GetLastChild() == c.Get());
    CHECK(a->GetNextSibling() == b.Get());
    CHECK(b->GetPrevSibling() == a.Get());

    a->ToFront(); // move a to end (topmost)
    CHECK(root->GetLastChild() == a.Get());
    a->ToBack();
    CHECK(root->GetFirstChild() == a.Get());
}

TEST_CASE("node: size and local bounds")
{
    auto node = MakeNode();
    node->SetSize(foundation::Float2{120.0f, 40.0f});
    CHECK(node->GetSize() == foundation::Float2{120.0f, 40.0f});
    Rect b = node->GetLocalBounds();
    CHECK(b == Rect{0.0f, 0.0f, 120.0f, 40.0f});
}

TEST_CASE("node: world position accumulates parent chain")
{
    auto root = MakeNode();
    auto child = MakeNode();
    root->AddChild(child.Get());
    root->SetPosition(foundation::Float2{100.0f, 0.0f});
    child->SetPosition(foundation::Float2{10.0f, 5.0f});

    const foundation::Float2 screen = child->GetScreenPosition();
    CHECK(screen.x == doctest::Approx(110.0f));
    CHECK(screen.y == doctest::Approx(5.0f));
}

TEST_CASE("node: hit test returns topmost child")
{
    auto root = MakeNode();
    root->SetSize(foundation::Float2{200.0f, 200.0f});
    auto a = MakeNode();
    auto b = MakeNode();
    a->SetSize(foundation::Float2{100.0f, 100.0f});
    b->SetSize(foundation::Float2{100.0f, 100.0f});
    root->AddChild(a.Get());
    root->AddChild(b.Get()); // b is topmost in the overlap

    CHECK(root->OverFind(foundation::Float2{50.0f, 50.0f}) == b.Get());
    CHECK(root->OverFind(foundation::Float2{150.0f, 150.0f}) == root.Get()); // only root there
    CHECK(root->OverFind(foundation::Float2{300.0f, 300.0f}) == nullptr);    // outside all
}

TEST_CASE("node: hidden node is not hit and hides subtree")
{
    auto root = MakeNode();
    root->SetSize(foundation::Float2{100.0f, 100.0f});
    root->SetVisible(false);
    CHECK_FALSE(root->IsVisible());
    CHECK(root->OverFind(foundation::Float2{50.0f, 50.0f}) == nullptr);
}

TEST_CASE("node: tree visibility follows parents")
{
    auto root = MakeNode();
    auto child = MakeNode();
    root->AddChild(child.Get());
    CHECK(child->IsTreeVisible());
    root->SetVisible(false);
    CHECK_FALSE(child->IsTreeVisible());
    CHECK(child->IsVisible()); // its own flag is still true
}

TEST_CASE("node: invalidation bubbles to ancestors")
{
    auto root = MakeNode();
    auto child = MakeNode();
    root->AddChild(child.Get());
    root->ClearNeedsRedraw();
    child->ClearNeedsRedraw();

    child->SetSize(foundation::Float2{10.0f, 10.0f});
    CHECK(child->NeedsRedraw());
    CHECK(root->NeedsRedraw()); // bubbled up
}

TEST_CASE("node: event listener fires on size change")
{
    auto node = MakeNode();
    int fired = 0;
    EventType seen = EventType::Close;
    const auto id = node->AddEventListener(EventType::SizeChanged,
                                           [&](const Event& e)
                                           {
                                               ++fired;
                                               seen = e.Type;
                                           });

    node->SetSize(foundation::Float2{10.0f, 10.0f});
    CHECK(fired == 1);
    CHECK(seen == EventType::SizeChanged);

    node->RemoveEventListener(id);
    node->SetSize(foundation::Float2{20.0f, 20.0f});
    CHECK(fired == 1); // no longer listening
}

TEST_CASE("node: setting same size does not notify")
{
    auto node = MakeNode();
    node->SetSize(foundation::Float2{10.0f, 10.0f});
    int fired = 0;
    node->AddEventListener(EventType::SizeChanged, [&](const Event&) { ++fired; });
    node->SetSize(foundation::Float2{10.0f, 10.0f}); // unchanged
    CHECK(fired == 0);
}
