// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// PopupLayer tests. root->GetPopupLayer() creates the layer on first access (not in the RootView
// constructor, which would make a module cycle), so the "is last child" tests fetch it into a local
// first. Popups are created with MakeRef and passed by .Get().
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.ui;
#include "TestHelpers.h"

using namespace foundation::ui;
using namespace foundation::ui::tests;
using namespace foundation::core;
namespace core = foundation::core;

namespace
{
    core::RefPtr<RootView> MakeRoot() { return core::MakeRef<RootView>(core::DefaultAllocator()); }
    core::RefPtr<TestView> MakeView(f32 w = 50.0f, f32 h = 30.0f)
    {
        return core::MakeRef<TestView>(core::DefaultAllocator(), w, h);
    }

    class TestPopupOwner final : public IPopupOwner
    {
    public:
        bool* Notified;
        View** ClosedPopup;
        TestPopupOwner(bool* notified, View** closedPopup)
            : Notified(notified), ClosedPopup(closedPopup)
        {
        }
        void OnPopupClosed(View* popup) override
        {
            *Notified = true;
            *ClosedPopup = popup;
        }
        // Not a View: cascade-close treats null as "outside any popup" (unused by these tests).
        [[nodiscard]] View* OwnerView() override { return nullptr; }
    };
}

TEST_CASE("popup-layer: RootView_HasPopupLayer")
{
    auto root = MakeRoot();
    CHECK(root->GetPopupLayer() != nullptr);
}

TEST_CASE("popup-layer: RootView_PopupLayerIsLastChild")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    PopupLayer* pl = root->GetPopupLayer();
    root->AddView(MakeView().Get());
    CHECK(root->GetChildAt(root->ChildCount() - 1) == pl);
}

TEST_CASE("popup-layer: RootView_PopupLayerStaysLast")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    PopupLayer* pl = root->GetPopupLayer();
    root->AddView(MakeView().Get());
    root->AddView(MakeView().Get());
    root->AddView(MakeView().Get());
    CHECK(root->GetChildAt(root->ChildCount() - 1) == pl);
}

TEST_CASE("popup-layer: ShowPopup_IncreasesCount")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    root->ViewportSize = Float2{800, 600};
    LayoutPass(ctx, root.Get());
    auto popup = MakeView(100, 50);
    root->GetPopupLayer()->ShowPopup(popup.Get(), nullptr, 10, 10, true, false, true);
    CHECK(root->GetPopupLayer()->PopupCount() == 1u);
}

TEST_CASE("popup-layer: ClosePopup_DecreasesCount")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    root->ViewportSize = Float2{800, 600};
    LayoutPass(ctx, root.Get());
    auto popup = MakeView(100, 50);
    root->GetPopupLayer()->ShowPopup(popup.Get(), nullptr, 10, 10, true, false, true);
    root->GetPopupLayer()->ClosePopup(popup.Get());
    CHECK(root->GetPopupLayer()->PopupCount() == 0u);
}

TEST_CASE("popup-layer: ShowPopup_OwnedView_UnregisteredOnClose")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    root->ViewportSize = Float2{800, 600};
    LayoutPass(ctx, root.Get());
    auto popup = MakeView(100, 50);
    const ViewId popupId = popup->Id;
    root->GetPopupLayer()->ShowPopup(popup.Get(), nullptr, 10, 10, true, false, true);
    root->GetPopupLayer()->ClosePopup(popup.Get());
    // Once detached it is out of the registry (the test's RefPtr keeps the popup itself alive).
    CHECK(ctx.GetViewById(popupId) == nullptr);
}

TEST_CASE("popup-layer: ShowPopup_NotOwned_NotDeletedOnClose")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    root->ViewportSize = Float2{800, 600};
    LayoutPass(ctx, root.Get());
    auto popup = MakeView(100, 50);
    root->GetPopupLayer()->ShowPopup(popup.Get(), nullptr, 10, 10, true, false, false);
    root->GetPopupLayer()->ClosePopup(popup.Get());
    CHECK(popup->Id.IsValid()); // still alive (the test's RefPtr owns it)
}

TEST_CASE("popup-layer: ShowPopup_NotifiesOwner")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    root->ViewportSize = Float2{800, 600};
    LayoutPass(ctx, root.Get());

    bool notified = false;
    View* closedPopup = nullptr;
    TestPopupOwner owner(&notified, &closedPopup);
    auto popup = MakeView(100, 50);
    root->GetPopupLayer()->ShowPopup(popup.Get(), &owner, 10, 10, true, false, true);
    root->GetPopupLayer()->ClosePopup(popup.Get());
    CHECK(notified);
}

TEST_CASE("popup-layer: Modal_HasModalPopup")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    root->ViewportSize = Float2{800, 600};
    LayoutPass(ctx, root.Get());
    auto popup = MakeView(100, 50);
    root->GetPopupLayer()->ShowPopup(popup.Get(), nullptr, 10, 10, true, true, true);
    CHECK(root->GetPopupLayer()->HasModalPopup());
    CHECK(root->GetPopupLayer()->TopmostModalPopup() == popup.Get());
}

TEST_CASE("popup-layer: Modal_HitTestBlocksBackground")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 800, 600);
    LayoutPass(ctx, root.Get());
    auto bg = MakeView(800, 600);
    root->AddView(bg.Get());
    LayoutPass(ctx, root.Get());
    auto popup = MakeView(100, 50);
    root->GetPopupLayer()->ShowPopup(popup.Get(), nullptr, 10, 10, true, true, true);
    LayoutPass(ctx, root.Get());

    View* hit = root->HitTest(Float2{700, 500}); // outside the popup, over bg
    CHECK(hit != bg.Get());
}

TEST_CASE("popup-layer: HandleClickOutside_ClosesCloseOnClick")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    root->ViewportSize = Float2{800, 600};
    LayoutPass(ctx, root.Get());
    auto popup = MakeView(100, 50);
    root->GetPopupLayer()->ShowPopup(popup.Get(), nullptr, 10, 10, true, false, true);
    CHECK(root->GetPopupLayer()->PopupCount() == 1u);
    root->GetPopupLayer()->HandleClickOutside(nullptr, 0); // outside any popup, LMB
    CHECK(root->GetPopupLayer()->PopupCount() == 0u);
}

TEST_CASE("popup-layer: ShowPopup_PushesFocus")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    root->ViewportSize = Float2{800, 600};
    auto view = MakeView();
    view->IsFocusable = true;
    root->AddView(view.Get());
    ctx.GetFocusManager()->SetFocus(view.Get());

    auto popup = MakeView(100, 50);
    root->GetPopupLayer()->ShowPopup(popup.Get(), nullptr, 10, 10, true, false, true);

    CHECK(ctx.GetFocusManager()->FocusedView() == nullptr);
    CHECK(ctx.GetFocusManager()->FocusStackDepth() == 1u);
}

TEST_CASE("popup-layer: ClosePopup_PopsFocus")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    root->ViewportSize = Float2{800, 600};
    auto view = MakeView();
    view->IsFocusable = true;
    root->AddView(view.Get());
    ctx.GetFocusManager()->SetFocus(view.Get());

    auto popup = MakeView(100, 50);
    root->GetPopupLayer()->ShowPopup(popup.Get(), nullptr, 10, 10, true, false, true);
    root->GetPopupLayer()->ClosePopup(popup.Get());

    CHECK(ctx.GetFocusManager()->FocusedView() == view.Get());
}

TEST_CASE("popup-layer: focus scope - Tab is TRAPPED inside an open focus-taking popup")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    root->ViewportSize = Float2{800, 600};
    auto background = MakeView();
    background->IsFocusable = true;
    background->IsTabStop = true;
    root->AddView(background.Get());

    // A popup whose content holds two focusable buttons.
    auto popup = core::MakeRef<ViewGroup>(core::DefaultAllocator());
    auto inner1 = MakeView();
    inner1->IsFocusable = true;
    inner1->IsTabStop = true;
    auto inner2 = MakeView();
    inner2->IsFocusable = true;
    inner2->IsTabStop = true;
    popup->AddView(inner1.Get());
    popup->AddView(inner2.Get());
    root->GetPopupLayer()->ShowPopup(popup.Get(), nullptr, 10, 10, true, true, false);
    LayoutPass(ctx, root.Get());

    auto* fm = ctx.GetFocusManager();
    fm->FocusNext(); // Tab with a focus-taking popup open lands inside the popup...
    View* first = fm->FocusedView();
    CHECK((first == inner1.Get() || first == inner2.Get()));
    fm->FocusNext();
    fm->FocusNext(); // ...and cycles within it, never onto the background view.
    View* cycled = fm->FocusedView();
    CHECK((cycled == inner1.Get() || cycled == inner2.Get()));
    CHECK(fm->FocusedView() != background.Get());

    root->GetPopupLayer()->ClosePopup(popup.Get());
    fm->FocusNext(); // the scope is gone, so the background is reachable again
    CHECK(fm->FocusedView() == background.Get());
}

TEST_CASE("popup-layer: out-of-LIFO-order close never cross-restores another popup's focus")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    root->ViewportSize = Float2{800, 600};
    auto baseView = MakeView();
    baseView->IsFocusable = true;
    root->AddView(baseView.Get());
    auto* fm = ctx.GetFocusManager();
    fm->SetFocus(baseView.Get()); // the focus popup A displaces

    auto popupA = MakeView(100, 50);
    root->GetPopupLayer()->ShowPopup(popupA.Get(), nullptr, 10, 10, true, false, true);
    auto popupB = MakeView(100, 50);
    root->GetPopupLayer()->ShowPopup(popupB.Get(), nullptr, 30, 30, true, false, true);

    // Close A first (out of order): A restores what it saved (baseView), and B's entry is untouched.
    root->GetPopupLayer()->ClosePopup(popupA.Get());
    CHECK(fm->FocusedView() == baseView.Get());
    // Closing B restores what B saved (nothing was focused when B opened), not A's entry again.
    root->GetPopupLayer()->ClosePopup(popupB.Get());
    CHECK(fm->FocusedView() == baseView.Get());
    CHECK(fm->FocusStackDepth() == 0u);
}

TEST_CASE("popup-layer: a popup anchored low is clamped to the space below it")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    root->ViewportSize = Float2{800, 600};
    LayoutPass(ctx, root.Get());
    auto popup = MakeView(100, 500); // measures 500 tall...
    root->GetPopupLayer()->ShowPopup(popup.Get(), nullptr, 10, 400, true, false, true);
    LayoutPass(ctx, root.Get());
    // ...but only 200 fit below y=400: the layout clamps instead of running off the layer
    // (scroll-aware popups see the smaller height and scroll their content).
    CHECK(popup->Height() == 200.0f);
}
