// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// ToastHost tests: timed expiry vs sticky toasts, deferred removal (never mid-dispatch),
// action/close buttons, and the bottom-right stacking layout.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.ui;
import foundation.ui.toolkit;

using namespace foundation::ui;
using namespace foundation::ui::toolkit;
using namespace foundation::core;
namespace core = foundation::core;

TEST_CASE("toolkit-toasthost: TimedExpiryAndStickiness")
{
    auto host = core::MakeRef<ToastHost>(core::DefaultAllocator());
    CHECK(host->ToastCount() == 0u);

    ToastRequest timed;
    timed.message = String(u8"cook finished");
    timed.severity = ToastSeverity::Success;
    timed.durationSeconds = 1.0f;
    const u64 a = host->Show(Move(timed));

    ToastRequest sticky;
    sticky.message = String(u8"cook FAILED");
    sticky.severity = ToastSeverity::Error;
    sticky.durationSeconds = 0.0f; // sticky
    const u64 b = host->Show(Move(sticky));

    CHECK(host->ToastCount() == 2u);
    CHECK(host->ChildCount() == 2u);
    CHECK(host->Contains(a));
    CHECK(host->Contains(b));

    host->Update(0.5f);
    CHECK(host->ToastCount() == 2u); // not yet
    host->Update(0.6f);              // a crosses 1s
    CHECK(!host->Contains(a));
    CHECK(host->Contains(b));

    for (int i = 0; i < 100; ++i)
    {
        host->Update(1.0f);
    } // sticky never expires
    CHECK(host->Contains(b));

    // Dismiss defers removal to the next Update (mutation-queue rule without the queue).
    host->Dismiss(b);
    CHECK(host->Contains(b));
    host->Update(0.0f);
    CHECK(host->ToastCount() == 0u);
    CHECK(host->ChildCount() == 0u);
}

TEST_CASE("toolkit-toasthost: ActionAndCloseButtons")
{
    auto host = core::MakeRef<ToastHost>(core::DefaultAllocator());

    ToastRequest request;
    request.message = String(u8"3 assets imported");
    request.durationSeconds = 0.0f;
    request.actionLabel = String(u8"Show");
    bool fired = false;
    request.onAction = Function<void()>{[&fired]() { fired = true; }};
    (void)host->Show(Move(request));

    auto* card = Cast<ViewGroup>(host->GetChildAt(0));
    REQUIRE(card != nullptr);
    REQUIRE(card->ChildCount() == 3u); // message + action + close
    auto* action = Cast<Button>(card->GetChildAt(1));
    REQUIRE(action != nullptr);
    action->OnClick.Invoke(action);
    CHECK(fired);
    CHECK(host->ToastCount() == 1u); // still live until Update
    host->Update(0.0f);
    CHECK(host->ToastCount() == 0u); // action also closes the toast

    // Without an action label there is no action button; close works the same way.
    ToastRequest plain;
    plain.message = String(u8"saved");
    plain.durationSeconds = 0.0f;
    (void)host->Show(Move(plain));
    card = Cast<ViewGroup>(host->GetChildAt(0));
    REQUIRE(card != nullptr);
    REQUIRE(card->ChildCount() == 2u); // message + close
    auto* close = Cast<Button>(card->GetChildAt(1));
    REQUIRE(close != nullptr);
    close->OnClick.Invoke(close);
    host->Update(0.0f);
    CHECK(host->ToastCount() == 0u);
}

TEST_CASE("toolkit-toasthost: BottomRightStacking")
{
    auto host = core::MakeRef<ToastHost>(core::DefaultAllocator());
    CHECK(!host->IsHitTestVisible); // pass-through outside the cards

    ToastRequest first;
    first.message = String(u8"older");
    first.durationSeconds = 0.0f;
    (void)host->Show(Move(first));
    ToastRequest second;
    second.message = String(u8"newest");
    second.durationSeconds = 0.0f;
    (void)host->Show(Move(second));

    host->Measure(BoxConstraints::Tight(800.0f, 600.0f));
    host->Layout(0, 0, 800.0f, 600.0f);
    CHECK(host->Width() == 800.0f); // fills the viewport

    View* older = host->GetChildAt(0);
    View* newest = host->GetChildAt(1);
    // Right edge: x = width - margin - toast width.
    CHECK(older->Bounds.x == doctest::Approx(800.0f - host->CornerMargin - host->ToastWidth));
    CHECK(newest->Bounds.x == older->Bounds.x);
    // Newest sits nearest the bottom edge; the older card stacks above it.
    CHECK(newest->Bounds.y > older->Bounds.y);
    CHECK(newest->Bounds.y + newest->Height() == doctest::Approx(600.0f - host->CornerMargin));
}
