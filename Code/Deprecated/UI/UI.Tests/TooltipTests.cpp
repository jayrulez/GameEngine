// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The tooltip subsystem, driven through the input manager and the frame tick: the show delay
// and the auto-hide, hover leaving, the owner being the nearest ANCESTOR with content (so a
// container's one tooltip serves all its children without restarting the clock), a provider
// asked before the plain text, no focus disturbance, hit-testability only when interactive,
// and a target deleted while the clock runs. Mirrored from the Beef port's suite - Raptor had
// none.

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

namespace
{
    /// A view that builds its own tooltip content (and also carries plain text, which must lose).
    class ProviderView final : public TestView, public ITooltipProvider
    {
        RTTI_OBJECT(ProviderView, TestView)
    public:
        i32 Created = 0;
        ProviderView() : TestView(50.0f, 30.0f) {}
        [[nodiscard]] ITooltipProvider* AsTooltipProvider() override { return this; }
        [[nodiscard]] core::RefPtr<View> CreateTooltipContent() override
        {
            ++Created;
            return core::MakeRef<TestView>(core::DefaultAllocator(), 100.0f, 40.0f);
        }
    };
    RTTI_DEFINE_OBJECT(ProviderView, "rtti::ui::tests")

    /// A (0,0) 50x30 with plain text, B (100,0) without, P (200,0) a provider, and a container
    /// at (0,100) 200x50 carrying ONE tooltip over two children at (0,100) and (100,100).
    struct Fixture
    {
        UIContext ctx{DefaultAllocator()};
        core::RefPtr<RootView> root = core::MakeRef<RootView>(core::DefaultAllocator());
        core::RefPtr<AbsoluteLayout> layout = core::MakeRef<AbsoluteLayout>(core::DefaultAllocator());
        core::RefPtr<TestView> a = core::MakeRef<TestView>(core::DefaultAllocator(), 50.0f, 30.0f);
        core::RefPtr<TestView> b = core::MakeRef<TestView>(core::DefaultAllocator(), 50.0f, 30.0f);
        core::RefPtr<ProviderView> p = core::MakeRef<ProviderView>(core::DefaultAllocator());
        core::RefPtr<AbsoluteLayout> group = core::MakeRef<AbsoluteLayout>(core::DefaultAllocator());
        core::RefPtr<TestView> child1 = core::MakeRef<TestView>(core::DefaultAllocator(), 50.0f, 30.0f);
        core::RefPtr<TestView> child2 = core::MakeRef<TestView>(core::DefaultAllocator(), 50.0f, 30.0f);

        Fixture()
        {
            Init(ctx, root.Get(), 400, 300);
            a->TooltipText = String(u8"Alpha");
            p->TooltipText = String(u8"ignored: the provider wins");
            group->TooltipText = String(u8"Group");
            LayoutStyle at;
            layout->AddView(a.Get(), at);
            at.Left = 100;
            layout->AddView(b.Get(), at);
            at.Left = 200;
            layout->AddView(p.Get(), at);
            at.Left = 0;
            at.Top = 100;
            at.Width = SizeSpec::Fixed(Unit::Dp(200));
            at.Height = SizeSpec::Fixed(Unit::Dp(50));
            layout->AddView(group.Get(), at);
            LayoutStyle inGroup;
            group->AddView(child1.Get(), inGroup);
            inGroup.Left = 100;
            group->AddView(child2.Get(), inGroup);
            root->AddView(layout.Get());
            LayoutPass(ctx, root.Get());
        }
        InputManager* Input() { return ctx.GetInputManager(); }
        TooltipManager* Tips() { return ctx.Tooltips(); }
        [[nodiscard]] usize Popups() { return root->GetPopupLayer()->PopupCount(); }
        void Tick(f32 seconds) { ctx.BeginFrame(seconds); }
    };
}

TEST_CASE("tooltip: shows after the hover delay and hides after the auto-hide delay")
{
    Fixture f;
    f.Input()->ProcessMouseMove(10, 10); // over A
    f.Tick(0.3f);
    CHECK_FALSE(f.Tips()->IsShowing());
    CHECK(f.Popups() == 0);
    f.Tick(0.3f); // 0.6 s >= the 0.5 s show delay
    CHECK(f.Tips()->IsShowing());
    CHECK(f.Popups() == 1);
    f.Tick(4.9f);
    CHECK(f.Tips()->IsShowing());
    f.Tick(0.2f); // 5.1 s shown >= the 5 s auto-hide
    CHECK_FALSE(f.Tips()->IsShowing());
    CHECK(f.Popups() == 0);
}

TEST_CASE("tooltip: leaving the target hides it at once, and a view without content shows none")
{
    Fixture f;
    f.Input()->ProcessMouseMove(10, 10);
    f.Tick(0.6f);
    REQUIRE(f.Tips()->IsShowing());
    f.Input()->ProcessMouseMove(110, 10); // over B, which has no tooltip
    CHECK_FALSE(f.Tips()->IsShowing());
    CHECK(f.Popups() == 0);
    f.Tick(1.0f);
    CHECK_FALSE(f.Tips()->IsShowing());
}

TEST_CASE("tooltip: the owner is the nearest ancestor with content, so moving between its children keeps the clock")
{
    Fixture f;
    f.Input()->ProcessMouseMove(10, 110); // child 1 of the group
    f.Tick(0.3f);
    f.Input()->ProcessMouseMove(110, 110); // child 2: same owner (the group)
    f.Tick(0.3f);
    CHECK(f.Tips()->IsShowing()); // 0.6 s on one owner, not two restarted 0.3 s clocks
}

TEST_CASE("tooltip: a provider is asked before the plain text")
{
    Fixture f;
    f.Input()->ProcessMouseMove(210, 10); // over P
    f.Tick(0.6f);
    CHECK(f.Tips()->IsShowing());
    CHECK(f.p->Created == 1);
}

TEST_CASE("tooltip: showing takes no focus")
{
    Fixture f;
    f.b->IsFocusable = true;
    f.ctx.GetFocusManager()->SetFocus(f.b.Get());
    REQUIRE(f.ctx.GetFocusManager()->FocusedView() == f.b.Get());
    f.Input()->ProcessMouseMove(10, 10);
    f.Tick(0.6f);
    REQUIRE(f.Tips()->IsShowing());
    CHECK(f.ctx.GetFocusManager()->FocusedView() == f.b.Get());
}

TEST_CASE("tooltip: only an interactive tooltip is hit-testable, and a press inside it keeps it up")
{
    Fixture f;
    // Non-interactive: the tooltip sits below P (Bottom placement) but the pointer passes through it.
    f.Input()->ProcessMouseMove(210, 10);
    f.Tick(0.6f);
    REQUIRE(f.Tips()->IsShowing());
    LayoutPass(f.ctx, f.root.Get()); // the popup layer arranges the tooltip on the next pass
    View* hit = f.root->HitTest(Float2{210, 35});
    CHECK_FALSE(f.Tips()->IsTooltipOrDescendant(hit));
    f.Input()->ProcessMouseDown(MouseButton::Left, 210, 35, 0); // a press anywhere hides it
    CHECK_FALSE(f.Tips()->IsShowing());
    f.Input()->ProcessMouseUp(MouseButton::Left, 210, 35);

    // Interactive: the same spot now hits the tooltip, and pressing there keeps it shown.
    f.p->IsTooltipInteractive = true;
    f.Input()->ProcessMouseMove(300, 200); // away
    f.Input()->ProcessMouseMove(210, 10);  // and back, restarting the clock
    f.Tick(0.6f);
    REQUIRE(f.Tips()->IsShowing());
    LayoutPass(f.ctx, f.root.Get());
    hit = f.root->HitTest(Float2{210, 35});
    CHECK(f.Tips()->IsTooltipOrDescendant(hit));
    f.Input()->ProcessMouseMove(210, 35);
    f.Input()->ProcessMouseDown(MouseButton::Left, 210, 35, 0);
    CHECK(f.Tips()->IsShowing());
    f.Input()->ProcessMouseUp(MouseButton::Left, 210, 35);
    f.Input()->ProcessMouseDown(MouseButton::Left, 300, 200, 0); // outside: hides
    CHECK_FALSE(f.Tips()->IsShowing());
}

TEST_CASE("tooltip: a target deleted while the clock runs shows nothing and crashes nothing")
{
    Fixture f;
    f.Input()->ProcessMouseMove(10, 10);
    f.Tick(0.3f);
    f.layout->RemoveView(f.a.Get());
    f.Tick(0.3f);
    CHECK_FALSE(f.Tips()->IsShowing());
    CHECK(f.Popups() == 0);
}
