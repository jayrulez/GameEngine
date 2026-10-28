// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Ported from Sedulous.UI.Tests/src/MutationQueueTests.bf (faithful, full file, 7 cases). Tests the
// deferred-mutation queue: enqueue actions, HasPending, Drain (in-order + re-entrant), BeginFrame drain,
// and QueueDelete's double-delete guard. Beef `scope MutationQueue()` -> a stack MutationQueue value;
// `queue.HasPending` (property) -> queue.HasPending() (method); `new [&] () => {}` delegate -> a bare
// C++ lambda (QueueAction takes a Function<void()>); `ctx.MutationQueue.X` -> ctx.MutationQueueRef().X.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
import foundation.core;
import foundation.ui;
#include "TestHelpers.h"

using namespace foundation::ui;
using namespace foundation::ui::tests;
using namespace foundation::core;
namespace core = foundation::core;

TEST_CASE("mutation-queue: Empty_HasNoPending")
{
    MutationQueue queue;
    CHECK_FALSE(queue.HasPending());
}

TEST_CASE("mutation-queue: QueueAction_HasPending")
{
    MutationQueue queue;
    queue.QueueAction([]() {});
    CHECK(queue.HasPending());
}

TEST_CASE("mutation-queue: Drain_ExecutesActions")
{
    MutationQueue queue;
    int counter = 0;
    queue.QueueAction([&counter]() { counter++; });
    queue.QueueAction([&counter]() { counter++; });

    queue.Drain();
    CHECK(counter == 2);
    CHECK_FALSE(queue.HasPending());
}

TEST_CASE("mutation-queue: Drain_ExecutesInOrder")
{
    MutationQueue queue;
    int order = 0;
    int first = -1, second = -1;
    queue.QueueAction([&]() { first = order++; });
    queue.QueueAction([&]() { second = order++; });

    queue.Drain();
    CHECK(first == 0);
    CHECK(second == 1);
}

TEST_CASE("mutation-queue: Drain_HandlesReentrantEnqueue")
{
    MutationQueue queue;
    int counter = 0;
    queue.QueueAction(
        [&]()
        {
            counter++;
            // Enqueue another action during drain
            queue.QueueAction([&counter]() { counter++; });
        });

    queue.Drain();
    CHECK(counter == 2); // Both original and re-entrant executed
    CHECK_FALSE(queue.HasPending());
}

TEST_CASE("mutation-queue: Drain_IntegratedWithBeginFrame")
{
    UIContext ctx{DefaultAllocator()};
    int counter = 0;
    ctx.MutationQueueRef().QueueAction([&counter]() { counter++; });
    ctx.BeginFrame(0.016f);
    CHECK(counter == 1);
}

TEST_CASE("mutation-queue: QueueDelete_PreventsDoubleDelete")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());

    auto view = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(view.Get());

    ctx.MutationQueueRef().QueueDelete(view.Get());
    CHECK(view->IsPendingDeletion);

    // Second queue should be no-op
    ctx.MutationQueueRef().QueueDelete(view.Get());
    // Drain should only delete once
    ctx.MutationQueueRef().Drain();
}
