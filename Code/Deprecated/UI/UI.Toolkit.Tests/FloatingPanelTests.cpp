// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// FloatingPanel control tests (foundation.ui.toolkit:floating_panel): the draggable / resizable /
// collapsible / closable float panel. The bare-panel cases run headlessly (no UIContext, no layout
// pass): construction, the collapse toggle, content swap, preferred-size clamping, and OnClose.
// The layout-driven clamp (ClampToParent from OnLayout) needs only an AbsoluteLayout parent inside
// a measured root - no captured mouse - and is covered below. POINTER drag / resize geometry still
// needs a captured mouse (the in-app path) and stays out of scope.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.ui;
import foundation.ui.toolkit;

using namespace foundation::ui;
using namespace foundation::ui::toolkit;
using namespace foundation::core;
namespace core = foundation::core;

TEST_CASE("FloatingPanel: constructs expanded")
{
    auto panel = core::MakeRef<FloatingPanel>(core::DefaultAllocator(), StringView(u8"Brush"));
    CHECK(panel.Get() != nullptr);
    CHECK_FALSE(panel->IsCollapsed());
}

TEST_CASE("FloatingPanel: collapse toggles and is idempotent")
{
    auto panel = core::MakeRef<FloatingPanel>(core::DefaultAllocator(), StringView(u8"Tools"));
    panel->SetCollapsed(true);
    CHECK(panel->IsCollapsed());
    panel->SetCollapsed(true); // no-op
    CHECK(panel->IsCollapsed());
    panel->SetCollapsed(false);
    CHECK_FALSE(panel->IsCollapsed());
}

TEST_CASE("FloatingPanel: collapse hides the content, expand shows it")
{
    auto panel = core::MakeRef<FloatingPanel>(core::DefaultAllocator(), StringView(u8"P"));
    auto body = core::MakeRef<Label>(core::DefaultAllocator(), StringView(u8"body"));
    panel->SetContent(RefPtr<View>(body.Get()));
    CHECK(body->Visibility == Visibility::Visible);
    panel->SetCollapsed(true);
    CHECK(body->Visibility == Visibility::Gone);
    panel->SetCollapsed(false);
    CHECK(body->Visibility == Visibility::Visible);
}

TEST_CASE("FloatingPanel: content swap and title/size setters are safe")
{
    auto panel = core::MakeRef<FloatingPanel>(core::DefaultAllocator(), StringView(u8"P"));
    auto body = core::MakeRef<Label>(core::DefaultAllocator(), StringView(u8"a"));
    panel->SetContent(RefPtr<View>(body.Get()));
    panel->SetContent({}); // clear
    panel->SetTitle(u8"New Title");
    panel->SetPreferredContentSize(320.0f, 240.0f);
    CHECK_FALSE(panel->IsCollapsed());
}

TEST_CASE("FloatingPanel: OnClose notifies subscribers")
{
    auto panel = core::MakeRef<FloatingPanel>(core::DefaultAllocator(), StringView(u8"P"));
    int closed = 0;
    panel->OnClose.Add([&closed]() { ++closed; });
    panel->OnClose.Invoke();
    CHECK(closed == 1);
}

// === Layout-driven clamping (ClampToParent runs from OnLayout; no mouse capture needed) ===

namespace
{
    // Runs the measure/layout pass TWICE: the first pass lays the panel out at the requested
    // LayoutStyle Left/Top and runs the clamp (which corrects them and invalidates); the
    // second pass applies the corrected position to Bounds - exactly the in-app flow, where the
    // clamp's Invalidate() schedules the next frame's layout.
    void RunLayout(UIContext& ctx, RootView* root, core::f32 w, core::f32 h)
    {
        root->ViewportSize = Float2{w, h};
        ctx.BeginFrame(0.016f);
        root->Measure(BoxConstraints::Tight(w, h));
        root->Layout(0, 0, w, h);
        root->Measure(BoxConstraints::Tight(w, h));
        root->Layout(0, 0, w, h);
    }
}

TEST_CASE("FloatingPanel: layout clamps X/Y so the whole panel stays inside the parent")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    root->ViewportSize = Float2{800, 600};
    ctx.AddRootView(root.Get());
    auto host = core::MakeRef<AbsoluteLayout>(core::DefaultAllocator());
    root->AddView(host.Get());

    auto panel = core::MakeRef<FloatingPanel>(core::DefaultAllocator(), StringView(u8"Tools"));
    LayoutStyle pos;
    pos.Left = 1000.0f; // far past the right edge
    pos.Top = 900.0f;  // far past the bottom edge
    host->AddView(panel.Get(), pos);

    RunLayout(ctx, root.Get(), 800, 600);

    // Clamped so the INTENDED box sits flush against the parent's right/bottom edge.
    CHECK(panel->Width() > 0.0f);
    CHECK(panel->Height() > 0.0f);
    CHECK(panel->Bounds.x == doctest::Approx(800.0f - panel->Width()));
    CHECK(panel->Bounds.y == doctest::Approx(600.0f - panel->Height()));
    CHECK(panel->Bounds.x + panel->Width() <= 800.0f + 0.01f);
    CHECK(panel->Bounds.y + panel->Height() <= 600.0f + 0.01f);
}

TEST_CASE("FloatingPanel: the panel follows a shrinking parent back inside")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    root->ViewportSize = Float2{800, 600};
    ctx.AddRootView(root.Get());
    auto host = core::MakeRef<AbsoluteLayout>(core::DefaultAllocator());
    root->AddView(host.Get());

    auto panel = core::MakeRef<FloatingPanel>(core::DefaultAllocator(), StringView(u8"Brush"));
    LayoutStyle pos;
    pos.Left = 500.0f;
    pos.Top = 350.0f;
    host->AddView(panel.Get(), pos);

    RunLayout(ctx, root.Get(), 800, 600);
    // Fits at 800x600: laid out exactly where it was placed.
    CHECK(panel->Bounds.x == doctest::Approx(500.0f));
    CHECK(panel->Bounds.y == doctest::Approx(350.0f));

    // Shrink the viewport (the bottom dock expanding upward): the panel is pulled back inside
    // instead of sliding behind the new edges.
    RunLayout(ctx, root.Get(), 600, 420);
    CHECK(panel->Bounds.x == doctest::Approx(600.0f - panel->Width()));
    CHECK(panel->Bounds.y == doctest::Approx(420.0f - panel->Height()));
    CHECK(panel->Bounds.x + panel->Width() <= 600.0f + 0.01f);
    CHECK(panel->Bounds.y + panel->Height() <= 420.0f + 0.01f);
}

TEST_CASE("FloatingPanel: a collapsed panel clamps against its header height")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    root->ViewportSize = Float2{800, 600};
    ctx.AddRootView(root.Get());
    auto host = core::MakeRef<AbsoluteLayout>(core::DefaultAllocator());
    root->AddView(host.Get());

    auto panel = core::MakeRef<FloatingPanel>(core::DefaultAllocator(), StringView(u8"P"));
    LayoutStyle pos;
    pos.Top = 900.0f; // past the bottom either way
    host->AddView(panel.Get(), pos);

    RunLayout(ctx, root.Get(), 800, 600);
    const core::f32 expandedY = panel->Bounds.y; // clamped against the full panel height

    panel->SetCollapsed(true);
    pos.Top = 900.0f; // push past the bottom again
    panel->SetLayout(pos);
    RunLayout(ctx, root.Get(), 800, 600);

    // Collapsed the panel is only its header tall, so it clamps flush to the bottom edge at a
    // LOWER Y than the full-height panel could reach.
    CHECK(panel->Bounds.y == doctest::Approx(600.0f - panel->Height()));
    CHECK(panel->Bounds.y > expandedY);
}

TEST_CASE("FloatingPanel: negative X/Y clamps to the parent origin")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    root->ViewportSize = Float2{800, 600};
    ctx.AddRootView(root.Get());
    auto host = core::MakeRef<AbsoluteLayout>(core::DefaultAllocator());
    root->AddView(host.Get());

    auto panel = core::MakeRef<FloatingPanel>(core::DefaultAllocator(), StringView(u8"P"));
    LayoutStyle pos;
    pos.Left = -120.0f;
    pos.Top = -45.0f;
    host->AddView(panel.Get(), pos);

    RunLayout(ctx, root.Get(), 800, 600);

    CHECK(panel->Bounds.x == doctest::Approx(0.0f));
    CHECK(panel->Bounds.y == doctest::Approx(0.0f));
}

TEST_CASE("FloatingPanel: the resize band lives in the border inset, plus a corner grab square")
{
    // A 9px hit band must not claim the outer ~3px of hosted content (a
    // PropertyGrid's scrollbar edge). Edges claim only the 6px content inset; the
    // bottom-right corner keeps a 12px OS-style grip square. CursorAt is the observable seam.
    auto panel = core::MakeRef<FloatingPanel>(core::DefaultAllocator(), StringView(u8"Band"));
    // A content child filling the body is the discriminator: a point the band does NOT claim
    // falls through to it; a band point returns the panel itself.
    auto content = core::MakeRef<View>(core::DefaultAllocator());
    content->IsHitTestVisible = true;
    panel->SetContent(content);
    panel->Measure(BoxConstraints::Tight(300.0f, 200.0f));
    panel->Layout(0.0f, 0.0f, 300.0f, 200.0f);

    // The override sits in FloatingPanel's protected section; View::HitTest is the public
    // surface, so probe through the base pointer.
    View* asView = panel.Get();
    // 8px inside the right edge (over content territory): NOT a resize zone anymore.
    CHECK(asView->HitTest(Float2{292.0f, 120.0f}) == content.Get());
    // Inside the 6px inset: the panel claims it (horizontal resize).
    CHECK(asView->HitTest(Float2{297.0f, 120.0f}) == panel.Get());
    // Bottom inset: the panel claims it (vertical resize).
    CHECK(asView->HitTest(Float2{150.0f, 197.0f}) == panel.Get());
    // The corner square reaches FURTHER in than the edges (12px): diagonal grab.
    CHECK(asView->HitTest(Float2{290.0f, 190.0f}) == panel.Get());
    // Same distance-in on a plain edge is content.
    CHECK(asView->HitTest(Float2{290.0f, 120.0f}) == content.Get());
}
