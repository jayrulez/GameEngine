// Draconic GUI - SceneNode coordinator tests: root lookup, deferred Close via the
// MutationQueue, and the update loop draining tree edits.
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
}

TEST_CASE("scene: nodes find the coordinator by walking to the root")
{
    auto root = MakeScene();
    auto mid = MakeNode();
    auto leaf = MakeNode();
    root->AddChild(mid.Get());
    mid->AddChild(leaf.Get());

    CHECK(leaf->GetRootNode() == root.Get());
    CHECK(leaf->GetActionManager() == root->GetActionManager());
    CHECK(leaf->GetMutationQueue() == root->GetMutationQueue());
}

TEST_CASE("scene: Close defers removal until update drains the queue")
{
    auto root = MakeScene();
    auto child = MakeNode();
    root->AddChild(child.Get());

    child->Close();
    CHECK(root->ChildCount() == 1); // not yet - deferred
    CHECK_FALSE(root->GetMutationQueue()->IsEmpty());

    root->Update(foundation::Duration::FromSeconds(0.016));
    CHECK(root->ChildCount() == 0); // drained
    CHECK(root->GetMutationQueue()->IsEmpty());
}

TEST_CASE("scene: Close without a coordinator removes immediately")
{
    auto parent = MakeNode(); // plain Node, not a SceneNode
    auto child = MakeNode();
    parent->AddChild(child.Get());

    child->Close();
    CHECK(parent->ChildCount() == 0); // immediate fallback
}

TEST_CASE("scene: update with nothing queued is harmless")
{
    auto root = MakeScene();
    root->Update(foundation::Duration::FromSeconds(0.016));
    CHECK(root->GetActionManager()->IsEmpty());
    CHECK(root->GetMutationQueue()->IsEmpty());
}

TEST_CASE("scene: OnUpdate hook fires each update")
{
    struct CountingScene : SceneNode
    {
        int ticks = 0;
        void OnUpdate(foundation::Duration) override { ++ticks; }
    };
    auto scene = foundation::MakeRef<CountingScene>(foundation::DefaultAllocator());
    scene->Update(foundation::Duration::FromSeconds(0.016));
    scene->Update(foundation::Duration::FromSeconds(0.016));
    CHECK(scene->ticks == 2);
}
