// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - Action tests: Move/Fade/Scale/Delay/Runnable/Sequence driven through a
// SceneRoot's ActionManager. Derived from eepp scene/actions behavior.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import experimental.gui;

using namespace experimental::gui;
namespace core = foundation::core;

namespace
{
    core::RefPtr<SceneRoot> MakeScene()
    {
        return core::MakeRef<SceneRoot>(core::DefaultAllocator());
    }
    core::RefPtr<Node> MakeNode() { return core::MakeRef<Node>(core::DefaultAllocator()); }
    core::Duration Sec(double s) { return core::Duration::FromSeconds(s); }
}

TEST_CASE("action: move interpolates position and completes")
{
    auto root = MakeScene();
    auto node = MakeNode();
    root->AddChild(node.Get());

    node->RunAction(core::MakeRef<MoveAction>(core::DefaultAllocator(), core::Float2{0.0f, 0.0f},
                                              core::Float2{100.0f, 0.0f}, Sec(1.0)));
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

    node->RunAction(core::MakeRef<FadeAction>(core::DefaultAllocator(), 1.0f, 0.0f, Sec(1.0)));
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

    node->RunAction(core::MakeRef<ScaleAction>(core::DefaultAllocator(), core::Float2{1.0f, 1.0f},
                                               core::Float2{3.0f, 3.0f}, Sec(1.0)));
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
        core::MakeRef<RunnableAction>(core::DefaultAllocator(), [&calls]() { ++calls; }, Sec(0.5)));

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

    auto seq = core::MakeRef<SequenceAction>(core::DefaultAllocator());
    seq->Add(core::MakeRef<MoveAction>(core::DefaultAllocator(), core::Float2{0.0f, 0.0f},
                                       core::Float2{100.0f, 0.0f}, Sec(1.0)));
    seq->Add(core::MakeRef<MoveAction>(core::DefaultAllocator(), core::Float2{100.0f, 0.0f},
                                       core::Float2{100.0f, 50.0f}, Sec(1.0)));
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
    auto node = MakeNode(); // no SceneRoot root
    node->RunAction(core::MakeRef<MoveAction>(core::DefaultAllocator(), core::Float2{0.0f, 0.0f},
                                              core::Float2{100.0f, 0.0f}, Sec(1.0)));
    CHECK(node->GetActionManager() == nullptr);
    CHECK(node->GetPosition().x == doctest::Approx(0.0f)); // unchanged
}

TEST_CASE("action: one whose target is destroyed is dropped, not run on freed memory")
{
    auto root = core::MakeRef<SceneRoot>(core::DefaultAllocator());
    int ticks = 0;
    {
        auto node = core::MakeRef<Node>(core::DefaultAllocator());
        root->AddChild(node.Get());
        auto delay = core::MakeRef<DelayAction>(core::DefaultAllocator(), core::Duration::FromSeconds(10.0));
        delay->SetOnDone([&](Action&) { ++ticks; });
        node->RunAction(delay);
        CHECK(root->GetActionManager()->Count() == 1u);
        root->RemoveChild(node.Get()); // the last reference goes with the scope
    }
    root->Update(core::Duration::FromSeconds(0.1));
    CHECK(root->GetActionManager()->Count() == 0u);
    CHECK(ticks == 0);
}
