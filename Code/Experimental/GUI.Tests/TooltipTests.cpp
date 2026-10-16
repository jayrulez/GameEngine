// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - Tooltip + TooltipManager tests: a tooltip appears after the hover delay on a
// widget with tooltip text, positions near the cursor, and hides when the hover changes.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import experimental.gui;

using namespace experimental::gui;
namespace core = foundation::core;

namespace
{
    template <typename T>
    core::RefPtr<T> Make()
    {
        return core::MakeRef<T>(core::DefaultAllocator());
    }
}

TEST_CASE("tooltip: appears after the delay while hovering a widget with tooltip text")
{
    auto root = Make<SceneRoot>();
    root->SetSize(core::Float2{300.0f, 300.0f});
    auto w = Make<UIWidget>();
    w->SetSize(core::Float2{80.0f, 30.0f});
    w->SetTooltip(core::StringView(u8"Click me"));
    root->AddChild(w.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    TooltipManager tips;
    tips.SetDelay(0.5);

    d->InjectMouseMove(core::Float2{40.0f, 15.0f}); // hover the widget
    CHECK(d->GetOverNode() == w.Get());

    tips.Update(*d, *root.Get(), 0.3); // below the delay
    CHECK_FALSE(tips.IsShown());
    tips.Update(*d, *root.Get(), 0.3); // cumulative 0.6 >= 0.5 -> shown
    CHECK(tips.IsShown());
    CHECK(tips.GetTooltip()->GetParent() == root.Get());
    // Positioned near the cursor (offset +12,+18).
    CHECK(tips.GetTooltip()->GetPosition().x == doctest::Approx(52.0f));
    CHECK(tips.GetTooltip()->GetPosition().y == doctest::Approx(33.0f));
}

TEST_CASE("tooltip: hidden until the delay, and no tooltip for widgets without text")
{
    auto root = Make<SceneRoot>();
    root->SetSize(core::Float2{300.0f, 300.0f});
    auto plain = Make<UIWidget>(); // no tooltip text
    plain->SetSize(core::Float2{80.0f, 30.0f});
    root->AddChild(plain.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    TooltipManager tips;
    tips.SetDelay(0.2);
    d->InjectMouseMove(core::Float2{40.0f, 15.0f});
    tips.Update(*d, *root.Get(), 1.0); // long past any delay
    CHECK_FALSE(tips.IsShown());       // no text -> never shows
}

TEST_CASE("tooltip: changing the hovered node hides and resets the timer")
{
    auto root = Make<SceneRoot>();
    root->SetSize(core::Float2{300.0f, 300.0f});
    auto a = Make<UIWidget>();
    a->SetSize(core::Float2{80.0f, 30.0f});
    a->SetTooltip(core::StringView(u8"A"));
    auto b = Make<UIWidget>();
    b->SetSize(core::Float2{80.0f, 30.0f});
    b->SetPosition(core::Float2{0.0f, 100.0f}); // no tooltip
    root->AddChild(a.Get());
    root->AddChild(b.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    TooltipManager tips;
    tips.SetDelay(0.3);

    d->InjectMouseMove(core::Float2{40.0f, 15.0f}); // hover a
    tips.Update(*d, *root.Get(), 0.4);
    CHECK(tips.IsShown());

    d->InjectMouseMove(core::Float2{40.0f, 115.0f}); // move to b (no tooltip)
    tips.Update(*d, *root.Get(), 0.01);
    CHECK_FALSE(tips.IsShown());
    CHECK(tips.GetTooltip()->GetParent() == nullptr);
}
