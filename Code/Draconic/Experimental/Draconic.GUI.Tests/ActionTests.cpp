// Draconic GUI - Action tests: Move/Fade/Scale/Delay/Runnable/Sequence driven through a
// SceneNode's ActionManager. Derived from eepp scene/actions behavior.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.gui;

using namespace draconic::gui;
namespace foundation = draconic::foundation;

namespace
{
    foundation::RefPtr<SceneNode> MakeScene()
    {
        return foundation::MakeRef<SceneNode>(foundation::DefaultAllocator());
    }
    foundation::RefPtr<Node> MakeNode() { return foundation::MakeRef<Node>(foundation::DefaultAllocator()); }
    foundation::Duration Sec(double s) { return foundation::Duration::FromSeconds(s); }
}

TEST_CASE("action: move interpolates position and completes")
{
    auto root = MakeScene();
    auto node = MakeNode();
    root->AddChild(node.Get());

    node->RunAction(foundation::MakeRef<MoveAction>(foundation::DefaultAllocator(), foundation::Float2{0.0f, 0.0f},
                                              foundation::Float2{100.0f, 0.0f}, Sec(1.0)));
    CHECK(root->GetActionManager()->Count() == 1);

    root->Update(Sec(0.5));
    CHECK(node->GetPosition().x == doctest::Approx(50.0f));

    root->Update(Sec(0.5));
    CHECK(node->GetPosition().x == doctest::Approx(100.0f));
    CHECK(root->GetActionManager()->IsEmpty()); // finished action dropped
}

TEST_CASE("action: fade interpolates alpha")
{
    auto root = MakeScene();
    auto node = MakeNode();
    root->AddChild(node.Get());

    node->RunAction(foundation::MakeRef<FadeAction>(foundation::DefaultAllocator(), 1.0f, 0.0f, Sec(1.0)));
    root->Update(Sec(0.25));
    CHECK(node->GetAlpha() == doctest::Approx(0.75f));
    root->Update(Sec(0.75));
    CHECK(node->GetAlpha() == doctest::Approx(0.0f));
}

TEST_CASE("action: scale interpolates")
{
    auto root = MakeScene();
    auto node = MakeNode();
    root->AddChild(node.Get());

    node->RunAction(foundation::MakeRef<ScaleAction>(foundation::DefaultAllocator(), foundation::Float2{1.0f, 1.0f},
                                               foundation::Float2{3.0f, 3.0f}, Sec(1.0)));
    root->Update(Sec(0.5));
    CHECK(node->GetScale().x == doctest::Approx(2.0f));
}

TEST_CASE("action: runnable fires once after delay")
{
    auto root = MakeScene();
    auto node = MakeNode();
    root->AddChild(node.Get());

    int calls = 0;
    node->RunAction(
        foundation::MakeRef<RunnableAction>(foundation::DefaultAllocator(), [&calls]() { ++calls; }, Sec(0.5)));

    root->Update(Sec(0.25));
    CHECK(calls == 0); // not yet
    root->Update(Sec(0.25));
    CHECK(calls == 1); // fired at completion
    root->Update(Sec(0.5));
    CHECK(calls == 1); // and only once (action was dropped)
}

TEST_CASE("action: sequence runs children in order")
{
    auto root = MakeScene();
    auto node = MakeNode();
    root->AddChild(node.Get());

    auto seq = foundation::MakeRef<SequenceAction>(foundation::DefaultAllocator());
    seq->Add(foundation::MakeRef<MoveAction>(foundation::DefaultAllocator(), foundation::Float2{0.0f, 0.0f},
                                       foundation::Float2{100.0f, 0.0f}, Sec(1.0)));
    seq->Add(foundation::MakeRef<MoveAction>(foundation::DefaultAllocator(), foundation::Float2{100.0f, 0.0f},
                                       foundation::Float2{100.0f, 50.0f}, Sec(1.0)));
    node->RunAction(seq);

    root->Update(Sec(1.0)); // first move completes
    CHECK(node->GetPosition().x == doctest::Approx(100.0f));
    CHECK(node->GetPosition().y == doctest::Approx(0.0f));

    root->Update(Sec(1.0)); // second move completes
    CHECK(node->GetPosition().y == doctest::Approx(50.0f));
    CHECK(root->GetActionManager()->IsEmpty());
}

TEST_CASE("action: run on unattached node is a no-op")
{
    auto node = MakeNode(); // no SceneNode root
    node->RunAction(foundation::MakeRef<MoveAction>(foundation::DefaultAllocator(), foundation::Float2{0.0f, 0.0f},
                                              foundation::Float2{100.0f, 0.0f}, Sec(1.0)));
    CHECK(node->GetActionManager() == nullptr);
    CHECK(node->GetPosition().x == doctest::Approx(0.0f)); // unchanged
}
